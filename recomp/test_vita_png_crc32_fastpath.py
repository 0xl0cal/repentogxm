#!/usr/bin/env python3
"""Frozen-PE proof and focused differential oracle for PNG CRC-32."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
ROOT = 0x005C3320
TABLE_RVA = 0x0072E9B0


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


def standard_table() -> bytes:
    words = []
    for value in range(256):
        crc = value
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
        words.append(crc)
    return struct.pack("<256I", *words)


def verify_codegen(pe_path: Path) -> None:
    os.environ["REPENTOGXM_PE"] = str(pe_path)
    sys.path.insert(0, str(HERE))
    import gen_all  # noqa: E402
    from image import DEFAULT_BASE, Image  # noqa: E402

    pin = Image(str(pe_path), DEFAULT_BASE)
    require(pin.code_at(TABLE_RVA, 1024) == standard_table(),
            "frozen CRC table is not the standard reflected polynomial")
    for output_base in (DEFAULT_BASE, 0x98000000):
        image = Image(str(pe_path), output_base)
        result = gen_all._translate_function(
            image, {"rva": ROOT}, None, {}, pin_img=pin
        )
        require(result["stub"] is None, "CRC32 target became a stub")
        require(result["vita_png_crc32_fastpath"],
                "CRC32 fast-path proof was not selected")
        text = result["text"]
        require(text.count("isaac_vita_png_crc32_guest_try(c)") == 1,
                "CRC32 entry seam count changed")
        require(text.count("Frozen zlib CRC32 native byte-loop fast path") == 1,
                "CRC32 marker changed")
        require("ISAAC_VITA_PNG_CRC32_FASTPATH" in text,
                "CRC32 feature fence disappeared")
        require("/* 005c3320  push ebp */" in text and
                "/* 005c3410  ret  */" in text,
                "translated CRC32 fallback body disappeared")


def compile_and_run_oracles(cc: str) -> tuple[str, str]:
    with tempfile.TemporaryDirectory(prefix="isaac-png-crc32-") as value:
        root = Path(value)
        suffix = ".exe" if os.name == "nt" else ""
        native = root / f"native{suffix}"
        guest = root / f"guest{suffix}"
        common = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]

        run([
            *common,
            str(RUNTIME / "host_vita_png_crc32_native.c"),
            str(RUNTIME / "host_vita_png_crc32_native_oracle.c"),
            "-o", str(native),
        ])
        native_result = run([str(native)]).strip()

        run([
            *common,
            "-DGUEST_STACK_REQUIRED=1",
            "-DISAAC_VITA_PNG_CRC32_FASTPATH=1",
            "-DISAAC_VITA_PNG_CRC32_TABLE_ADDRESS=0x22010000U",
            "-I", str(RUNTIME),
            str(RUNTIME / "host_vita_png_crc32_native.c"),
            str(RUNTIME / "host_vita_png_crc32_guest.c"),
            str(RUNTIME / "host_vita_png_crc32_guest_oracle.c"),
            "-o", str(guest),
        ])
        guest_result = run([str(guest)]).strip()
    return native_result, guest_result


def verify_build_wiring() -> None:
    cmake = (HERE / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    builder = (HERE.parent / "tools" / "build_vita.py").read_text(
        encoding="utf-8"
    )
    require(cmake.count("option(ISAAC_VITA_PNG_CRC32_FASTPATH") == 1,
            "PNG CRC32 CMake option is missing or duplicated")
    require(cmake.count("ISAAC_VITA_PNG_CRC32_FASTPATH=1") == 1,
            "PNG CRC32 generated/guest compile fence changed")
    require(cmake.count("host_vita_png_crc32_native.c") == 1 and
            cmake.count("host_vita_png_crc32_guest.c") == 2,
            "PNG CRC32 runtime source ownership changed")
    require(builder.count('"-DISAAC_VITA_PNG_CRC32_FASTPATH=ON"') == 2,
            "canonical PNG CRC32 build selection changed")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=Path)
    parser.add_argument("--cc")
    arguments = parser.parse_args()

    require(arguments.pe.stat().st_size == PE_SIZE, "frozen PE size changed")
    require(digest(arguments.pe) == PE_SHA256, "frozen PE hash changed")
    verify_codegen(arguments.pe)
    verify_build_wiring()
    native, guest = compile_and_run_oracles(compiler(arguments.cc))
    require(native.startswith("PNG CRC32 native oracle: PASS;"),
            "native CRC32 differential oracle changed")
    require(guest.startswith("PNG CRC32 guest oracle: PASS;"),
            "guest CRC32 ABI oracle changed")
    print(native)
    print(guest)
    print("Vita PNG CRC32 fast path: PASS; direct-call sites=2")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
