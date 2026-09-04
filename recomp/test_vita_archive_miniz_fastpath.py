#!/usr/bin/env python3
"""Frozen-PE proof and focused differential oracle for archive MiniZ."""

from __future__ import annotations

import argparse
import _ctypes
import ctypes
import hashlib
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib


HERE = Path(__file__).resolve().parent
ROOT_DIR = HERE.parent
RUNTIME = HERE / "runtime"
SCRATCH = Path(tempfile.gettempdir()) / "repentogxm-scratch"
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
ARCHIVE_SIZE = 385_003_320
ARCHIVE_SHA256 = "b58a2c74f49f106be54021961b5bb11a8428861e125d7430efa24ded652187ea"
ROOT = 0x005AEB00
SWITCH_SITE = 0x005AEBFE
STATE_BYTES = 0x2AF0
INPUT_MAX = 0x7FF
OUTPUT_BYTES = 0x400
NEEDS_MORE_INPUT = 1
DONE = 0


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def run(command: list[str]) -> str:
    result = subprocess.run(
        command, check=False, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True
    )
    if result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}): {command!r}\n"
            f"{result.stdout}"
        )
    return result.stdout


def compiler(explicit: str | None) -> str:
    if explicit:
        return explicit
    for candidate in (os.environ.get("CC"), "clang", "gcc", "cc"):
        if candidate and shutil.which(candidate):
            return candidate
    if os.name == "nt":
        installed = Path(os.environ.get(
            "ProgramFiles", r"C:\Program Files"
        )) / "LLVM" / "bin" / "clang.exe"
        if installed.is_file():
            return str(installed)
    raise AssertionError("no GCC-compatible host compiler found; pass --cc")


def verify_codegen(pe_path: Path) -> None:
    os.environ["REPENTOGXM_PE"] = str(pe_path)
    sys.path.insert(0, str(HERE))
    import gen_all  # noqa: E402
    from image import DEFAULT_BASE, Image  # noqa: E402

    cases = gen_all.ARCHIVE_REMAP_CASES
    switch_info = {
        ROOT: {
            "entries": tuple(sorted(cases)),
            "edges": {SWITCH_SITE: cases},
            "rejected": (),
            "tables": {SWITCH_SITE: cases},
        },
    }
    pin = Image(str(pe_path), DEFAULT_BASE)
    for output_base in (DEFAULT_BASE, 0x98000000):
        image = Image(str(pe_path), output_base)
        result = gen_all._translate_function(
            image, {"rva": ROOT}, None, switch_info, pin_img=pin
        )
        require(result["stub"] is None, "archive MiniZ target became a stub")
        require(result["vita_archive_miniz_fastpath"],
                "archive MiniZ fast-path proof was not selected")
        text = result["text"]
        require(text.count("isaac_vita_archive_miniz_guest_try(c)") == 1,
                "archive MiniZ entry seam count changed")
        require("!g_guest_coverage_functions" in text and
                "!g_guest_coverage_cases" in text,
                "archive MiniZ coverage-preserving fallback disappeared")
        require("/* 005aeb00  push ebp */" in text and
                "/* 005b0624  ret  */" in text,
                "translated archive MiniZ fallback body disappeared")


class NativeMiniZ:
    def __init__(self, library: Path):
        self.library = ctypes.CDLL(str(library))
        self.call = self.library.isaac_vita_archive_miniz_native
        self.call.argtypes = (
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_uint32,
        )
        self.call.restype = ctypes.c_int

    def close(self) -> None:
        handle = self.library._handle
        self.call = None
        self.library = None
        if os.name == "nt":
            _ctypes.FreeLibrary(handle)
        else:
            _ctypes.dlclose(handle)

    def stream(self, frames: list[tuple[bytes, bool]]) -> tuple[bytes, list[int]]:
        state = ctypes.create_string_buffer(STATE_BYTES)
        output = (ctypes.c_uint8 * OUTPUT_BYTES)()
        decoded = bytearray()
        statuses = []
        for payload, last in frames:
            require(len(payload) <= INPUT_MAX, "synthetic frame exceeds game ABI")
            input_bytes = (ctypes.c_uint8 * max(1, len(payload)))()
            if payload:
                ctypes.memmove(input_bytes, payload, len(payload))
            input_size = ctypes.c_uint32(len(payload))
            output_size = ctypes.c_uint32(OUTPUT_BYTES)
            status = self.call(
                state, input_bytes, ctypes.byref(input_size),
                output, output, ctypes.byref(output_size),
                0 if last else 2,
            )
            require(input_size.value == len(payload),
                    "native MiniZ did not consume the complete archive frame")
            require(output_size.value <= OUTPUT_BYTES,
                    "native MiniZ exceeded the wrapping output window")
            require(status == (DONE if last else NEEDS_MORE_INPUT),
                    f"native MiniZ status changed: {status}, last={last}")
            decoded.extend(bytes(output[:output_size.value]))
            statuses.append(status)
        return bytes(decoded), statuses


def compressed_frames(data: bytes, *, level: int = 6,
                      strategy: int = zlib.Z_DEFAULT_STRATEGY
                      ) -> list[tuple[bytes, bool]]:
    compressor = zlib.compressobj(
        level, zlib.DEFLATED, -15, zlib.DEF_MEM_LEVEL, strategy
    )
    blocks = [data[index:index + OUTPUT_BYTES]
              for index in range(0, len(data), OUTPUT_BYTES)] or [b""]
    result = []
    for index, block in enumerate(blocks):
        last = index + 1 == len(blocks)
        payload = compressor.compress(block)
        payload += compressor.flush(zlib.Z_FINISH if last else zlib.Z_SYNC_FLUSH)
        require(len(payload) <= INPUT_MAX, "synthetic compressed frame too large")
        result.append((payload, last))
    return result


def verify_synthetic(native: NativeMiniZ) -> int:
    rng = random.Random(0x5AEB00)
    cases = (
        (b"", 6, zlib.Z_DEFAULT_STRATEGY),
        (b"hello miniz", 6, zlib.Z_FIXED),
        (bytes(range(256)) * 3, 0, zlib.Z_DEFAULT_STRATEGY),
        ((b"archive-coroutine-" * 311)[:5000], 6,
         zlib.Z_DEFAULT_STRATEGY),
        (bytes(rng.randrange(16) for _ in range(900)), 9,
         zlib.Z_DEFAULT_STRATEGY),
    )
    calls = 0
    for data, level, strategy in cases:
        frames = compressed_frames(data, level=level, strategy=strategy)
        decoded, statuses = native.stream(frames)
        require(decoded == data, "synthetic raw-DEFLATE differential mismatch")
        require(statuses[-1] == DONE, "synthetic stream did not terminate")
        calls += len(frames)
    return calls


def read_archive_entries(stream) -> list[tuple[int, int]]:
    header = stream.read(14)
    require(len(header) == 14 and header[:7] == b"ARCH000",
            "archive signature changed")
    require(header[7] == 2, "archive is not MiniZ mode")
    index_offset, count = struct.unpack_from("<IH", header, 8)
    stream.seek(index_offset)
    result = []
    for _ in range(count):
        row = stream.read(20)
        require(len(row) == 20, "truncated archive index")
        _, _, offset, length, _ = struct.unpack("<5I", row)
        result.append((offset, length))
    return result


def verify_archive(native: NativeMiniZ, path: Path) -> tuple[int, int, int]:
    require(path.stat().st_size == ARCHIVE_SIZE, "archive size changed")
    require(digest(path) == ARCHIVE_SHA256, "archive hash changed")
    calls = 0
    needs_more = 0
    complete_entries = 0
    with path.open("rb") as stream:
        entries = read_archive_entries(stream)
        for offset, expected_length in entries:
            stream.seek(offset)
            frames: list[tuple[bytes, bool]] = []
            compressed = True
            while True:
                raw_header = stream.read(4)
                require(len(raw_header) == 4, "truncated archive frame header")
                header = struct.unpack("<I", raw_header)[0]
                length = header & 0x7FFFFFFF
                last = bool(header & 0x80000000)
                require(length <= 0x800, "archive frame exceeds frozen buffer")
                payload = stream.read(length)
                require(len(payload) == length, "truncated archive frame")
                if compressed and not (not last and length == OUTPUT_BYTES):
                    frames.append((payload, last))
                else:
                    compressed = False
                if last:
                    break

            if not frames:
                continue
            decoded, statuses = native.stream(frames)
            calls += len(frames)
            needs_more += statuses.count(NEEDS_MORE_INPUT)
            if compressed:
                joined = b"".join(payload for payload, _ in frames)
                require(decoded == zlib.decompress(joined, -15),
                        "real archive native/zlib differential mismatch")
                require(len(decoded) == expected_length,
                        "real archive decoded length changed")
                complete_entries += 1

    require((calls, needs_more, complete_entries) == (59_972, 58_441, 1_531),
            "real archive MiniZ corpus census changed")
    return calls, needs_more, complete_entries


def compile_oracles(cc: str, root: Path) -> tuple[NativeMiniZ, str]:
    library = root / ("archive_miniz.dll" if os.name == "nt"
                      else "libarchive_miniz.so")
    guest = root / ("archive_miniz_guest.exe" if os.name == "nt"
                    else "archive_miniz_guest")
    common = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]
    shared = ["-shared"]
    if os.name != "nt":
        shared.append("-fPIC")
    run([
        *common, *shared,
        "-DISAAC_VITA_ARCHIVE_MINIZ_TEST_EXPORT=1",
        str(RUNTIME / "host_vita_archive_miniz_native.c"),
        "-o", str(library),
    ])
    run([
        *common,
        "-DGUEST_STACK_REQUIRED=1",
        "-DISAAC_VITA_ARCHIVE_MINIZ_FASTPATH=1",
        "-DISAAC_VITA_HEAP_RANGE_LEASE=1",
        "-I", str(RUNTIME),
        str(RUNTIME / "host_vita_archive_miniz_native.c"),
        str(RUNTIME / "host_vita_archive_miniz_guest.c"),
        str(RUNTIME / "host_vita_archive_miniz_guest_oracle.c"),
        "-o", str(guest),
    ])
    return NativeMiniZ(library), run([str(guest)]).strip()


def verify_build_wiring() -> None:
    cmake = (HERE / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    builder = (ROOT_DIR / "tools" / "build_vita.py").read_text(
        encoding="utf-8"
    )
    gate = (HERE / "vita" / "vita_raw_allocator_gate.py").read_text(
        encoding="utf-8"
    )
    require(cmake.count("option(ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH") == 1,
            "archive MiniZ CMake option is missing or duplicated")
    require(cmake.count("ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH=1") == 1,
            "archive MiniZ generated/guest/heap compile fence changed")
    require(cmake.count("host_vita_archive_miniz_native.c") == 1 and
            cmake.count("host_vita_archive_miniz_guest.c") == 3,
            "archive MiniZ runtime source ownership changed")
    require('f"-DISAAC_VITA_ARCHIVE_MINIZ_FASTPATH="' in builder,
            "canonical archive MiniZ build selection disappeared")
    require("verify_archive_miniz_fastpath_compile_scope(records, cache)" in gate,
            "allocator gate lost archive MiniZ compile-scope closure")


def verify_allocator_gate_scope(root: Path) -> None:
    sys.path.insert(0, str(HERE / "vita"))
    import vita_raw_allocator_gate as gate  # noqa: E402

    owner = root / "guest_0175.c"
    owner.write_text(
        "void sub_005aeb00(CPU *__restrict c)\n"
        "isaac_vita_archive_miniz_guest_try(c)\n",
        encoding="ascii",
    )
    key = "ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH"
    records = {
        owner: {"source": owner, "definitions": {key: ["1"]}},
        RUNTIME / "host_vita_heap.c": {
            "source": RUNTIME / "host_vita_heap.c",
            "definitions": {key: ["1"]},
        },
        RUNTIME / "host_vita_archive_miniz_guest.c": {
            "source": RUNTIME / "host_vita_archive_miniz_guest.c",
            "definitions": {key: ["1"], "ISAAC_VITA_HEAP_RANGE_LEASE": ["1"]},
        },
        RUNTIME / "host_vita_archive_miniz_native.c": {
            "source": RUNTIME / "host_vita_archive_miniz_native.c",
            "definitions": {},
        },
    }
    gate.verify_archive_miniz_fastpath_compile_scope(records, {key: "ON"})

    records[RUNTIME / "host_vita_archive_miniz_native.c"]["definitions"] = {
        key: ["1"]
    }
    try:
        gate.verify_archive_miniz_fastpath_compile_scope(records, {key: "ON"})
    except gate.GateError:
        pass
    else:
        raise AssertionError("allocator gate accepted widened MiniZ scope")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=Path)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--cc")
    arguments = parser.parse_args()

    require(arguments.pe.stat().st_size == PE_SIZE, "frozen PE size changed")
    require(digest(arguments.pe) == PE_SHA256, "frozen PE hash changed")
    verify_codegen(arguments.pe)
    verify_build_wiring()

    SCRATCH.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(
            prefix="isaac-archive-miniz-", dir=SCRATCH) as value:
        temporary = Path(value)
        verify_allocator_gate_scope(temporary)
        native, guest = compile_oracles(compiler(arguments.cc), temporary)
        try:
            synthetic_calls = verify_synthetic(native)
            archive_result = None
            if arguments.archive is not None:
                archive_result = verify_archive(native, arguments.archive)
        finally:
            native.close()

    require(guest.startswith("archive MiniZ guest oracle: PASS;"),
            "archive MiniZ guest ABI oracle changed")
    print(guest)
    print(f"archive MiniZ synthetic differential: PASS; calls={synthetic_calls}")
    if archive_result is not None:
        calls, needs_more, complete = archive_result
        print(
            "archive MiniZ real corpus: PASS; "
            f"calls={calls} needs-more={needs_more} complete={complete}"
        )
    print("Vita archive MiniZ fast path: PASS; frozen RVA=0x005aeb00")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
