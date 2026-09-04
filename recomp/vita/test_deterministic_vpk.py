#!/usr/bin/env python3
"""Hostile staging, ZIP metadata, and one-edge Ninja regression."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import zipfile


HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import deterministic_vpk as subject


EPOCH_TEXT = "1700000001"
EPOCH = 1_700_000_000


def run(command: list[str], *, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command, cwd=None if cwd is None else str(cwd), check=False,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
    )


def require_pass(completed: subprocess.CompletedProcess[str], label: str) -> None:
    if completed.returncode != 0:
        raise AssertionError(
            f"{label} failed rc={completed.returncode}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _native_rows(command: list[str]) -> tuple[list[tuple[Path, str]], Path, Path, Path]:
    resources: list[tuple[Path, str]] = []
    index = 1
    while command[index] == "-a":
        source, destination = command[index + 1].split("=", 1)
        resources.append((Path(source), destination))
        index += 2
    if command[index] != "-s" or command[index + 2] != "-b":
        raise AssertionError(f"staged native order changed: {command!r}")
    return resources, Path(command[index + 1]), Path(command[index + 3]), Path(
        command[index + 4]
    )


def fake_packer(command: list[str], environment: dict[str, str], cwd: Path) -> int:
    resources, param, eboot, output = _native_rows(command)
    if environment.get("TZ") != "UTC" or environment.get("SOURCE_DATE_EPOCH") != str(EPOCH):
        raise AssertionError("child deterministic environment changed")
    if cwd.parent != output.parent.parent:
        raise AssertionError("staging directory is not adjacent to requested output")
    rows = [(param, "sce_sys/param.sfo"), (eboot, "eboot.bin"), *resources]
    for source, _ in rows:
        metadata = source.stat()
        if (stat.S_IMODE(metadata.st_mode) != subject.DETERMINISTIC_MODE or
                int(metadata.st_mtime) != EPOCH or source.parent != cwd):
            raise AssertionError(f"staged input metadata changed: {source}")
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED,
                         compresslevel=9) as archive:
        for source, destination in rows:
            utc = time.gmtime(source.stat().st_mtime)
            info = zipfile.ZipInfo(destination, (
                utc.tm_year, utc.tm_mon, utc.tm_mday,
                utc.tm_hour, utc.tm_min, utc.tm_sec,
            ))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (stat.S_IFREG | 0o666) << 16
            archive.writestr(info, source.read_bytes())
    return 0


def corrupt_packer(command: list[str], environment: dict[str, str], cwd: Path) -> int:
    result = fake_packer(command, environment, cwd)
    output = Path(command[-1])
    payload = bytearray(output.read_bytes())
    with zipfile.ZipFile(output, "r") as archive:
        central_crc_offset = archive.start_dir + 16
    payload[central_crc_offset] ^= 0x01
    output.write_bytes(payload)
    return result


def failing_packer(command: list[str], environment: dict[str, str], cwd: Path) -> int:
    Path(command[-1]).write_bytes(b"partial corrupt output")
    return 23


def make_inputs(root: Path, mtime: int, *, output_name: str = "candidate.vpk") -> tuple[
        Path, list[str], Path]:
    root.mkdir(parents=True)
    param = root / "param.sfo"
    eboot = root / "eboot.bin"
    pe = root / "input.exe"
    config = root / "alsoft.conf"
    payloads = {
        param: b"synthetic param.sfo\x00",
        eboot: b"SCE\x00deterministic eboot\n",
        pe: b"MZ deterministic PE\n",
        config: b"drivers = vita\n",
    }
    for index, (path, payload) in enumerate(payloads.items()):
        path.write_bytes(payload)
        path.chmod(0o600 if index % 2 else 0o644)
        os.utime(path, (mtime + index * 18, mtime + index * 18))
    packer = root / "fake-vita-pack-vpk"
    packer.write_bytes(b"fake executable identity\n")
    packer.chmod(0o755)
    output = root / output_name
    native = [
        "-a", f"{pe}=isaac-ng.exe.unpacked.exe",
        "-a", f"{config}=alsoft.conf",
        "-s", str(param), "-b", str(eboot), str(output),
    ]
    return packer, native, output


def expect_red(callable_object, token: str) -> None:
    try:
        callable_object()
    except subject.VpkDeterminismError as exc:
        if token not in str(exc):
            raise AssertionError(f"RED lacks {token!r}: {exc}") from exc
    else:
        raise AssertionError(f"expected RED containing {token!r}")


def staging_regression(base: Path, real_packer: Path | None) -> dict[str, str]:
    left_packer, left_native, left_output = make_inputs(
        base / "A", 1_600_000_000
    )
    right_packer, right_native, right_output = make_inputs(
        base / "B-relocated-longer", 1_800_000_000
    )
    runner = subject._run_child if real_packer is not None else fake_packer
    packer_a = real_packer or left_packer
    packer_b = real_packer or right_packer
    subject.stage_and_pack(packer_a, EPOCH_TEXT, left_native, runner=runner)
    subject.stage_and_pack(packer_b, EPOCH_TEXT, right_native, runner=runner)
    if left_output.read_bytes() != right_output.read_bytes():
        raise AssertionError("two-root/different-mtime VPK bytes differ")
    first_hash = sha256(left_output)
    if stat.S_IMODE(left_output.stat().st_mode) != subject.DETERMINISTIC_MODE:
        raise AssertionError("atomic VPK output mode is not deterministic")

    # Idempotence must not depend on the original inputs' current metadata.
    for path in (base / "A").iterdir():
        if path.is_file() and path != left_output:
            os.utime(path, (1_900_000_000, 1_900_000_000))
    subject.stage_and_pack(packer_a, EPOCH_TEXT, left_native, runner=runner)
    if sha256(left_output) != first_hash:
        raise AssertionError("idempotent VPK rebuild changed bytes")

    old = b"preserve old VPK on child failure\n"
    left_output.write_bytes(old)
    expect_red(lambda: subject.stage_and_pack(
        left_packer, EPOCH_TEXT, left_native, runner=failing_packer
    ), "returned nonzero")
    if left_output.read_bytes() != old:
        raise AssertionError("child failure replaced the old VPK")
    if list(left_output.parent.glob(f".{left_output.name}.deterministic-*")):
        raise AssertionError("child failure left an adjacent staging directory")

    expect_red(lambda: subject.stage_and_pack(
        left_packer, EPOCH_TEXT, left_native, runner=corrupt_packer
    ), "CRC validation")
    if left_output.read_bytes() != old:
        raise AssertionError("corrupt child output replaced the old VPK")

    real_rmtree = subject.shutil.rmtree
    cleanup_calls = 0

    def fail_first_cleanup(path, *arguments, **keywords):
        nonlocal cleanup_calls
        cleanup_calls += 1
        if cleanup_calls == 1:
            raise OSError("injected pre-commit cleanup failure")
        return real_rmtree(path, *arguments, **keywords)

    subject.shutil.rmtree = fail_first_cleanup
    try:
        expect_red(lambda: subject.stage_and_pack(
            left_packer, EPOCH_TEXT, left_native, runner=fake_packer
        ), "before atomic replacement")
    finally:
        subject.shutil.rmtree = real_rmtree
    if left_output.read_bytes() != old:
        raise AssertionError("cleanup failure replaced the old VPK")
    if cleanup_calls != 2:
        raise AssertionError("cleanup failure was not retried exactly once")
    if (list(left_output.parent.glob(f".{left_output.name}.deterministic-*")) or
            list(left_output.parent.glob(
                f".{left_output.name}.deterministic-candidate-*"
            ))):
        raise AssertionError("cleanup failure left temporary VPK state")
    return {"sha256": first_hash, "size": str(right_output.stat().st_size)}


def hostile_argv_regression(base: Path) -> int:
    packer, native, output = make_inputs(base / "argv", 1_600_000_000)
    count = 0

    def red(arguments: list[str], token: str) -> None:
        nonlocal count
        expect_red(lambda: subject.stage_and_pack(
            packer, EPOCH_TEXT, arguments, runner=fake_packer
        ), token)
        count += 1

    red([], "native vita-pack-vpk argv")
    red(["--unknown", *native], "native vita-pack-vpk argv")
    red(["-a", "missing-equals", *native[-5:]], "source=destination")
    resource = str(base / "argv" / "input.exe")
    red(["-a", f"{resource}=../escape", *native[-5:]], "unsafe VPK destination")
    red(["-a", f"{resource}=eboot.bin", *native[-5:]], "destinations must be unique")
    red(native[:-1] + ["relative.vpk"], "output must be absolute")
    red(native + ["extra"], "native vita-pack-vpk argv")
    expect_red(lambda: subject.stage_and_pack(
        packer, "01700000000", native, runner=fake_packer
    ), "canonical decimal")
    count += 1
    expect_red(lambda: subject.main([
        "--real-packer", str(packer), "--source-date-epoch", EPOCH_TEXT,
        *native,
    ]), "exactly one '--'")
    count += 1
    if hasattr(os, "symlink"):
        link = base / "argv" / "linked-input.exe"
        try:
            link.symlink_to(base / "argv" / "input.exe")
        except OSError:
            pass
        else:
            red(["-a", f"{link}=linked.bin", *native[-5:]], "non-symlink")
    if output.exists():
        output.unlink()
    return count


def _newer_content(path: Path, reference: Path) -> None:
    original = path.read_bytes()
    deadline = time.monotonic() + 2.0
    while True:
        path.write_bytes(original + b"\n")
        if path.stat().st_mtime_ns > reference.stat().st_mtime_ns:
            return
        if time.monotonic() >= deadline:
            raise AssertionError(f"content mutation did not become newer: {path}")
        time.sleep(0.02)


def cmake_ninja_regression(base: Path) -> str:
    cmake = shutil.which("cmake")
    ninja = shutil.which("ninja")
    if cmake is None or ninja is None:
        return "not-available"
    source = base / "cmake-source"
    build = base / "cmake-build"
    source.mkdir()
    wrapper = source / "deterministic_vpk.py"
    helper = source / "deterministic_vpk.cmake"
    real = source / "vita-pack-vpk"
    shutil.copyfile(HERE / wrapper.name, wrapper)
    shutil.copyfile(HERE / helper.name, helper)
    real.write_text(f'''#!{Path(sys.executable).as_posix()}
import pathlib, stat, sys, time, zipfile
args = sys.argv[1:]
resources = []
while args and args[0] == "-a":
    source, destination = args[1].split("=", 1)
    resources.append((pathlib.Path(source), destination))
    args = args[2:]
if len(args) != 5 or args[0] != "-s" or args[2] != "-b":
    raise SystemExit(9)
rows = [(pathlib.Path(args[1]), "sce_sys/param.sfo"),
        (pathlib.Path(args[3]), "eboot.bin"), *resources]
with zipfile.ZipFile(args[4], "w", compression=zipfile.ZIP_DEFLATED,
                     compresslevel=9) as archive:
    for source_path, destination in rows:
        utc = time.gmtime(source_path.stat().st_mtime)
        info = zipfile.ZipInfo(destination,
            (utc.tm_year, utc.tm_mon, utc.tm_mday,
             utc.tm_hour, utc.tm_min, utc.tm_sec))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = (stat.S_IFREG | 0o666) << 16
        archive.writestr(info, source_path.read_bytes())
''', encoding="utf-8")
    real.chmod(0o755)
    (source / "param.sfo").write_bytes(b"sfo")
    (source / "eboot.bin").write_bytes(b"eboot")
    project = f'''cmake_minimum_required(VERSION 3.19)
project(deterministic_vpk_edge NONE)
include("{helper.as_posix()}")
    set(SOURCE_DATE_EPOCH "{EPOCH_TEXT}" CACHE STRING "")
set(VITA_PACK_VPK "{real.as_posix()}" CACHE FILEPATH "")
isaac_enable_deterministic_vpk(
  "${{VITA_PACK_VPK}}" "{Path(sys.executable).as_posix()}"
  "{wrapper.as_posix()}" "${{SOURCE_DATE_EPOCH}}")
set(_out "${{CMAKE_CURRENT_BINARY_DIR}}/package.vpk.out")
add_custom_command(
  OUTPUT "${{_out}}"
  COMMAND ${{VITA_PACK_VPK}}
    -s "{(source / 'param.sfo').as_posix()}"
    -b "{(source / 'eboot.bin').as_posix()}" "${{_out}}"
  DEPENDS "{(source / 'param.sfo').as_posix()}"
  VERBATIM)
isaac_append_deterministic_vpk_dependencies(
  "${{_out}}" "{wrapper.as_posix()}" "${{ISAAC_VITA_REAL_PACK_VPK}}")
add_custom_target(package ALL DEPENDS "${{_out}}")
'''
    (source / "CMakeLists.txt").write_text(project, encoding="utf-8")
    require_pass(run([cmake, "-G", "Ninja", "-S", str(source), "-B", str(build)]),
                 "deterministic VPK configure")
    ninja_payload = (build / "build.ninja").read_text(encoding="utf-8")
    if ninja_payload.count("build package.vpk.out | ") != 1:
        raise AssertionError("deterministic VPK added or lost a .vpk.out edge")
    for required in (str(wrapper).replace("\\", "/"),
                     str(real).replace("\\", "/"),
                     "--source-date-epoch", EPOCH_TEXT):
        if required not in ninja_payload.replace("\\", "/"):
            raise AssertionError(f"Ninja VPK edge lacks {required!r}")
    output = build / "package.vpk.out"
    require_pass(run([ninja, "-v", "package"], cwd=build),
                 "initial one-edge VPK build")
    if not output.is_file():
        raise AssertionError("initial VPK edge produced no output")
    baseline = run([ninja, "-n", "-v", "package"], cwd=build)
    require_pass(baseline, "baseline VPK dry-run")
    if "no work to do" not in (baseline.stdout + baseline.stderr).lower():
        raise AssertionError(
            "baseline VPK edge is unexpectedly dirty\n"
            f"stdout:\n{baseline.stdout}\nstderr:\n{baseline.stderr}"
        )

    _newer_content(wrapper, output)
    wrapper_dirty = run([ninja, "-n", "-v", "package"], cwd=build)
    require_pass(wrapper_dirty, "wrapper invalidation dry-run")
    if (wrapper_dirty.stdout + wrapper_dirty.stderr).count("[1/1]") != 1:
        raise AssertionError("wrapper invalidation changed the one-edge graph")
    output.write_bytes(b"restored after wrapper mutation\n")
    _newer_content(real, output)
    packer_dirty = run([ninja, "-n", "-v", "package"], cwd=build)
    require_pass(packer_dirty, "packer invalidation dry-run")
    if (packer_dirty.stdout + packer_dirty.stderr).count("[1/1]") != 1:
        raise AssertionError("real-packer invalidation changed the one-edge graph")
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    if (f"SOURCE_DATE_EPOCH:STRING={EPOCH_TEXT}" not in cache or
            f"ISAAC_VITA_REAL_PACK_VPK:FILEPATH={real}" not in cache):
        raise AssertionError("CMake cache lost the deterministic VPK contract")
    return hashlib.sha256(ninja_payload.encode("utf-8")).hexdigest()


def production_contract() -> None:
    cmake = (HERE / "CMakeLists.txt").read_text(encoding="utf-8")
    enable = cmake.index("isaac_enable_deterministic_vpk(")
    first_vpk = cmake.index("vita_create_vpk(")
    append = cmake.index("isaac_append_deterministic_vpk_dependencies(")
    if not enable < first_vpk < append:
        raise AssertionError("production VPK shadow/append order changed")
    if cmake.count("isaac_append_deterministic_vpk_dependencies(") != 1:
        raise AssertionError("production has duplicate deterministic VPK append calls")
    build_vita = (REPO / "tools" / "build_vita.py").read_text(encoding="utf-8")
    for token in (
        '"recomp/vita/deterministic_vpk.py"',
        '"recomp/vita/deterministic_vpk.cmake"',
        '"ISAAC_VITA_REAL_PACK_VPK"',
        'f"-DSOURCE_DATE_EPOCH={source_date_epoch}"',
    ):
        if token not in build_vita:
            raise AssertionError(f"build wrapper lost deterministic VPK token {token}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--real-packer", type=Path)
    args = parser.parse_args()
    production_contract()
    with tempfile.TemporaryDirectory(prefix="isaac-vpk-determinism-") as temporary:
        base = Path(temporary).resolve()
        fake = staging_regression(base / "fake", None)
        hostile = hostile_argv_regression(base / "hostile")
        ninja_hash = cmake_ninja_regression(base)
        actual = None
        if args.real_packer is not None:
            actual = staging_regression(base / "actual", args.real_packer.resolve())
    print(
        "deterministic VPK wrapper: PASS; "
        f"fake={fake['sha256']}/{fake['size']} hostile_red={hostile} "
        f"ninja={ninja_hash} actual=" +
        ("not-requested" if actual is None else f"{actual['sha256']}/{actual['size']}")
    )


if __name__ == "__main__":
    main()
