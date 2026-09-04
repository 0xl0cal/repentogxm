#!/usr/bin/env python3
"""Stage vita-pack-vpk inputs at one canonical DOS timestamp."""

from __future__ import annotations

import binascii
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import time
from typing import Callable
import zipfile


DOS_EPOCH_MIN = 315_532_800       # 1980-01-01 00:00:00 UTC
DOS_EPOCH_MAX = 4_354_819_198     # 2107-12-31 23:59:58 UTC
DESTINATION_SEGMENT_RE = re.compile(r"^[A-Za-z0-9._-]+$")
LOCAL_HEADER = struct.Struct("<I5H3I2H")
CENTRAL_HEADER = struct.Struct("<I6H3I5H2I")
END_RECORD = struct.Struct("<I4H2IH")
LOCAL_SIGNATURE = 0x04034B50
CENTRAL_SIGNATURE = 0x02014B50
END_SIGNATURE = 0x06054B50
DETERMINISTIC_MODE = 0o666 if os.name == "nt" else 0o644


class VpkDeterminismError(RuntimeError):
    """The native packaging argv, inputs, or output violated the contract."""


Runner = Callable[[list[str], dict[str, str], Path], int]


def _regular_no_symlink(path: Path, label: str) -> Path:
    if not path.is_absolute():
        raise VpkDeterminismError(f"{label} must be absolute: {path}")
    try:
        mode = path.lstat().st_mode
    except OSError as exc:
        raise VpkDeterminismError(f"cannot inspect {label} {path}: {exc}") from exc
    if stat.S_ISLNK(mode) or not stat.S_ISREG(mode):
        raise VpkDeterminismError(
            f"{label} must be a regular non-symlink file: {path}"
        )
    return path


def _safe_destination(value: str) -> str:
    if not value or "\\" in value or value.startswith("/"):
        raise VpkDeterminismError(f"unsafe VPK destination: {value!r}")
    pure = PurePosixPath(value)
    if pure.as_posix() != value or any(
            part in ("", ".", "..") or
            DESTINATION_SEGMENT_RE.fullmatch(part) is None
            for part in pure.parts):
        raise VpkDeterminismError(f"unsafe VPK destination: {value!r}")
    return value


def _parse_epoch(value: str) -> int:
    if re.fullmatch(r"(?:0|[1-9][0-9]*)", value) is None:
        raise VpkDeterminismError(
            f"source-date epoch must be canonical decimal: {value!r}"
        )
    epoch = int(value, 10) & ~1
    if not DOS_EPOCH_MIN <= epoch <= DOS_EPOCH_MAX:
        raise VpkDeterminismError(
            f"source-date epoch is outside the even DOS range: {value}"
        )
    return epoch


def _parse_native_args(arguments: list[str]) -> tuple[
        list[tuple[Path, str]], Path, Path, Path]:
    resources: list[tuple[Path, str]] = []
    index = 0
    while index < len(arguments) and arguments[index] == "-a":
        if index + 1 >= len(arguments):
            raise VpkDeterminismError("native -a is missing its source=destination")
        mapping = arguments[index + 1]
        if mapping.count("=") != 1:
            raise VpkDeterminismError(
                f"native -a must contain one source=destination: {mapping!r}"
            )
        source_text, destination = mapping.split("=", 1)
        source = _regular_no_symlink(Path(source_text), "VPK resource")
        resources.append((source, _safe_destination(destination)))
        index += 2
    tail = arguments[index:]
    if len(tail) != 5 or tail[0] != "-s" or tail[2] != "-b":
        raise VpkDeterminismError(
            "native vita-pack-vpk argv must be repeated '-a source=dest', "
            "then exactly '-s param.sfo -b eboot.bin output'"
        )
    param = _regular_no_symlink(Path(tail[1]), "param.sfo input")
    eboot = _regular_no_symlink(Path(tail[3]), "eboot input")
    output = Path(tail[4])
    if not output.is_absolute() or not output.parent.is_dir():
        raise VpkDeterminismError(
            f"VPK output must be absolute with an existing parent: {output}"
        )
    if output.parent.is_symlink():
        raise VpkDeterminismError(f"VPK output parent must not be a symlink: {output}")
    if output.exists() or output.is_symlink():
        _regular_no_symlink(output, "existing VPK output")
    destinations = ["sce_sys/param.sfo", "eboot.bin", *(
        destination for _, destination in resources
    )]
    if len(destinations) != len(set(destinations)):
        raise VpkDeterminismError(
            f"VPK destinations must be unique: {destinations!r}"
        )
    return resources, param, eboot, output


def _copy_staged(source: Path, destination: Path, epoch: int) -> bytes:
    payload = source.read_bytes()
    destination.write_bytes(payload)
    destination.chmod(DETERMINISTIC_MODE)
    # The file was just created by this process inside a private directory;
    # Windows does not implement follow_symlinks=False for os.utime().
    os.utime(destination, (epoch, epoch))
    actual = destination.stat()
    if (stat.S_IMODE(actual.st_mode) != DETERMINISTIC_MODE or
            int(actual.st_mtime) != epoch):
        raise VpkDeterminismError(
            f"cannot apply deterministic staging metadata: {destination}"
        )
    return payload


def _dos_fields(epoch: int) -> tuple[int, int, tuple[int, int, int, int, int, int]]:
    utc = time.gmtime(epoch)
    date = ((utc.tm_year - 1980) << 9) | (utc.tm_mon << 5) | utc.tm_mday
    clock = (utc.tm_hour << 11) | (utc.tm_min << 5) | (utc.tm_sec // 2)
    return date, clock, (
        utc.tm_year, utc.tm_mon, utc.tm_mday,
        utc.tm_hour, utc.tm_min, utc.tm_sec,
    )


def _validate_archive(
    path: Path, expected: list[tuple[str, bytes]], epoch: int,
) -> bytes:
    _regular_no_symlink(path, "staged VPK output")
    payload = path.read_bytes()
    expected_date, expected_time, expected_tuple = _dos_fields(epoch)
    try:
        with zipfile.ZipFile(path, "r") as archive:
            infos = archive.infolist()
            expected_names = [name for name, _ in expected]
            actual_names = [info.filename for info in infos]
            if actual_names != expected_names:
                raise VpkDeterminismError(
                    f"VPK member order/set changed: {actual_names!r} != "
                    f"{expected_names!r}"
                )
            if archive.comment != b"" or archive.testzip() is not None:
                raise VpkDeterminismError("VPK comment or CRC validation changed")

            local_cursor = 0
            for info, (name, expected_payload) in zip(infos, expected):
                if info.header_offset != local_cursor:
                    raise VpkDeterminismError(
                        f"VPK has a local-header gap before {name!r}"
                    )
                if info.date_time != expected_tuple:
                    raise VpkDeterminismError(
                        f"VPK central timestamp changed for {name!r}: "
                        f"{info.date_time!r} != {expected_tuple!r}"
                    )
                if info.extra or info.comment or info.flag_bits & 0x08:
                    raise VpkDeterminismError(
                        f"VPK member has extra/comment/data-descriptor metadata: {name!r}"
                    )
                actual_payload = archive.read(info)
                expected_crc = binascii.crc32(expected_payload) & 0xFFFFFFFF
                if actual_payload != expected_payload or info.CRC != expected_crc:
                    raise VpkDeterminismError(
                        f"VPK payload/central CRC changed for {name!r}"
                    )
                if local_cursor + LOCAL_HEADER.size > len(payload):
                    raise VpkDeterminismError("truncated VPK local header")
                local = LOCAL_HEADER.unpack_from(payload, local_cursor)
                if local[0] != LOCAL_SIGNATURE:
                    raise VpkDeterminismError("invalid VPK local-header signature")
                filename_length, extra_length = local[9], local[10]
                name_start = local_cursor + LOCAL_HEADER.size
                name_end = name_start + filename_length
                if payload[name_start:name_end] != name.encode("utf-8"):
                    raise VpkDeterminismError(
                        f"VPK local filename changed for {name!r}"
                    )
                if (local[2] != info.flag_bits or local[3] != info.compress_type or
                        local[4] != expected_time or local[5] != expected_date or
                        local[6] != expected_crc or local[7] != info.compress_size or
                        local[8] != len(expected_payload) or extra_length != 0):
                    raise VpkDeterminismError(
                        f"VPK local metadata changed for {name!r}"
                    )
                local_cursor = name_end + extra_length + info.compress_size
            if local_cursor != archive.start_dir:
                raise VpkDeterminismError("VPK has data outside contiguous local members")

            central_cursor = archive.start_dir
            for info, (name, expected_payload) in zip(infos, expected):
                if central_cursor + CENTRAL_HEADER.size > len(payload):
                    raise VpkDeterminismError("truncated VPK central header")
                central = CENTRAL_HEADER.unpack_from(payload, central_cursor)
                if central[0] != CENTRAL_SIGNATURE:
                    raise VpkDeterminismError("invalid VPK central-header signature")
                filename_length = central[10]
                extra_length, comment_length = central[11], central[12]
                name_start = central_cursor + CENTRAL_HEADER.size
                name_end = name_start + filename_length
                expected_crc = binascii.crc32(expected_payload) & 0xFFFFFFFF
                if payload[name_start:name_end] != name.encode("utf-8"):
                    raise VpkDeterminismError(
                        f"VPK central filename changed for {name!r}"
                    )
                if (central[3] != info.flag_bits or
                        central[4] != info.compress_type or
                        central[5] != expected_time or central[6] != expected_date or
                        central[7] != expected_crc or
                        central[8] != info.compress_size or
                        central[9] != len(expected_payload) or
                        extra_length != 0 or comment_length != 0 or
                        central[13] != 0 or central[16] != info.header_offset):
                    raise VpkDeterminismError(
                        f"VPK central metadata changed for {name!r}"
                    )
                central_cursor = name_end + extra_length + comment_length

            if central_cursor + END_RECORD.size != len(payload):
                raise VpkDeterminismError("VPK has an extra/truncated end record")
            end = END_RECORD.unpack_from(payload, central_cursor)
            if (end[0] != END_SIGNATURE or end[1:3] != (0, 0) or
                    end[3] != len(expected) or end[4] != len(expected) or
                    end[5] != central_cursor - archive.start_dir or
                    end[6] != archive.start_dir or end[7] != 0):
                raise VpkDeterminismError("VPK end record changed")
    except (OSError, zipfile.BadZipFile, RuntimeError) as exc:
        if isinstance(exc, VpkDeterminismError):
            raise
        raise VpkDeterminismError(f"cannot validate staged VPK {path}: {exc}") from exc
    return payload


def _run_child(command: list[str], environment: dict[str, str], cwd: Path) -> int:
    return subprocess.run(command, cwd=str(cwd), env=environment,
                          check=False).returncode


def stage_and_pack(
    real_packer: Path, source_date_epoch: str, native_args: list[str],
    *, runner: Runner = _run_child,
) -> Path:
    real_packer = _regular_no_symlink(real_packer, "real vita-pack-vpk")
    if os.name != "nt" and not os.access(real_packer, os.X_OK):
        raise VpkDeterminismError(f"real vita-pack-vpk is not executable: {real_packer}")
    epoch = _parse_epoch(source_date_epoch)
    resources, param, eboot, output = _parse_native_args(native_args)
    temporary: Path | None = Path(tempfile.mkdtemp(
        prefix=f".{output.name}.deterministic-", dir=output.parent
    ))
    candidate: Path | None = None
    try:
        assert temporary is not None
        temporary.chmod(0o700)
        staged_param = temporary / "000-param.sfo"
        staged_eboot = temporary / "001-eboot.bin"
        expected: list[tuple[str, bytes]] = [
            ("sce_sys/param.sfo", _copy_staged(param, staged_param, epoch)),
            ("eboot.bin", _copy_staged(eboot, staged_eboot, epoch)),
        ]
        staged_resources: list[tuple[Path, str]] = []
        for index, (source, destination) in enumerate(resources, 2):
            staged = temporary / f"{index:03d}-resource.bin"
            expected.append((destination, _copy_staged(source, staged, epoch)))
            staged_resources.append((staged, destination))
        staged_output = temporary / "output.vpk"
        command = [str(real_packer)]
        for source, destination in staged_resources:
            command.extend(("-a", f"{source}={destination}"))
        command.extend(("-s", str(staged_param), "-b", str(staged_eboot),
                        str(staged_output)))
        environment = {
            **os.environ,
            "TZ": "UTC",
            "SOURCE_DATE_EPOCH": str(epoch),
        }
        if runner(command, environment, temporary) != 0:
            raise VpkDeterminismError("real vita-pack-vpk returned nonzero")
        archive_payload = _validate_archive(staged_output, expected, epoch)

        candidate_fd, candidate_text = tempfile.mkstemp(
            prefix=f".{output.name}.deterministic-candidate-",
            dir=output.parent,
        )
        candidate = Path(candidate_text)
        with os.fdopen(candidate_fd, "wb") as stream:
            stream.write(archive_payload)
        candidate.chmod(DETERMINISTIC_MODE)
        candidate_metadata = candidate.stat()
        if (stat.S_IMODE(candidate_metadata.st_mode) != DETERMINISTIC_MODE or
                candidate.read_bytes() != archive_payload):
            raise VpkDeterminismError(
                f"cannot apply deterministic candidate metadata: {candidate}"
            )

        # No cleanup operation may remain after the only commit point.  If this
        # removal fails, the finally block retries it while the old output is
        # still untouched.
        try:
            shutil.rmtree(temporary, ignore_errors=False)
        except OSError as exc:
            raise VpkDeterminismError(
                "cannot clean deterministic VPK staging before atomic replacement"
            ) from exc
        temporary = None
        os.replace(candidate, output)
        candidate = None
        return output
    finally:
        try:
            if candidate is not None:
                candidate.unlink(missing_ok=True)
        finally:
            if temporary is not None:
                shutil.rmtree(temporary, ignore_errors=False)


def main(arguments: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if arguments is None else arguments)
    if argv.count("--") != 1:
        raise VpkDeterminismError("wrapper argv must contain exactly one '--'")
    separator = argv.index("--")
    wrapper, native = argv[:separator], argv[separator + 1:]
    if (len(wrapper) != 4 or wrapper[0] != "--real-packer" or
            wrapper[2] != "--source-date-epoch"):
        raise VpkDeterminismError(
            "wrapper argv must be exactly '--real-packer PATH "
            "--source-date-epoch EPOCH -- ...'"
        )
    stage_and_pack(Path(wrapper[1]), wrapper[3], native)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, VpkDeterminismError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
