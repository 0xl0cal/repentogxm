#!/usr/bin/env python3
"""Focused frozen-PE and differential oracle for native inflate_flush."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
ROOT = 0x005D6720
NATIVE_RESULT = (
    "zlib 1.1.4 inflate_flush native oracle: PASS; "
    "flush=8192 adler=1024"
)
GUEST_RESULT = (
    "zlib inflate_flush guest oracle: PASS; leases=5 hostile=16"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def run(command: list[str]) -> str:
    result = subprocess.run(
        command, check=False, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True
    )
    if result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}): {command!r}\n{result.stdout}"
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

    pin = Image(str(pe_path), DEFAULT_BASE)
    for output_base in (DEFAULT_BASE, 0x98000000):
        image = pin if output_base == DEFAULT_BASE else Image(
            str(pe_path), output_base
        )
        result = gen_all._translate_function(
            image, {"rva": ROOT}, None, {}, pin_img=pin
        )
        require(result["stub"] is None,
                "inflate_flush target became a stub")
        require(result["vita_png_inflate_flush_fastpath"],
                "inflate_flush fast-path proof was not selected")
        text = result["text"]
        require(text.count(
            "isaac_vita_zlib_inflate_flush_guest_try(c)") == 1,
            "inflate_flush entry seam count changed")
        require("ISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH" in text,
                "inflate_flush feature fence disappeared")
        require("/* 005d6720  push ebp */" in text and
                "/* 005d6829  ret  */" in text,
                "translated inflate_flush fallback body disappeared")


def compile_and_run_oracles(cc: str) -> None:
    with tempfile.TemporaryDirectory(
            prefix="isaac-zlib-inflate-flush-") as value:
        root = Path(value)
        suffix = ".exe" if os.name == "nt" else ""
        native = root / f"native{suffix}"
        guest = root / f"guest{suffix}"
        common = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]

        run([
            *common,
            str(RUNTIME / "host_vita_zlib_inflate_flush_native.c"),
            str(RUNTIME / "host_vita_zlib_inflate_flush_native_oracle.c"),
            "-o", str(native),
        ])
        require(run([str(native)]).strip() == NATIVE_RESULT,
                "native inflate_flush differential result changed")

        run([
            *common,
            "-DGUEST_STACK_REQUIRED=1",
            "-DISAAC_VITA_HEAP_RANGE_LEASE=1",
            "-DISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH=1",
            "-I", str(RUNTIME),
            str(RUNTIME / "host_vita_zlib_inflate_flush_native.c"),
            str(RUNTIME / "host_vita_zlib_inflate_flush_guest.c"),
            str(RUNTIME / "host_vita_zlib_inflate_flush_guest_oracle.c"),
            "-o", str(guest),
        ])
        require(run([str(guest)]).strip() == GUEST_RESULT,
                "guest inflate_flush hostile result changed")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=Path)
    parser.add_argument("--cc")
    arguments = parser.parse_args()

    require(arguments.pe.stat().st_size == PE_SIZE, "frozen PE size changed")
    require(digest(arguments.pe) == PE_SHA256, "frozen PE hash changed")
    verify_codegen(arguments.pe)
    compile_and_run_oracles(compiler(arguments.cc))
    print(NATIVE_RESULT)
    print(GUEST_RESULT)
    print("Vita zlib inflate_flush fast path: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
