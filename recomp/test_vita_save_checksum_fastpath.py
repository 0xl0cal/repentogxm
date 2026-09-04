#!/usr/bin/env python3
"""Focused frozen-PE/codegen and native save-checksum differential oracle."""

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
ROOT = 0x0025B340


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
        installed_llvm = Path(os.environ.get(
            "ProgramFiles", r"C:\Program Files"
        )) / "LLVM" / "bin" / "clang.exe"
        if installed_llvm.is_file():
            return str(installed_llvm)
    raise AssertionError("no GCC-compatible host compiler found; pass --cc")


def verify_codegen(pe_path: Path) -> None:
    os.environ["REPENTOGXM_PE"] = str(pe_path)
    sys.path.insert(0, str(HERE))
    import gen_all  # noqa: E402
    from image import DEFAULT_BASE, Image  # noqa: E402

    pin = Image(str(pe_path), DEFAULT_BASE)
    result = gen_all._translate_function(
        pin, {"rva": ROOT}, None, {}, pin_img=pin
    )
    require(result["stub"] is None, "checksum target became a stub")
    require(result["vita_save_checksum_fastpath"],
            "checksum fast-path proof was not selected")
    text = result["text"]
    require(text.count("isaac_vita_save_checksum_guest_try(c)") == 1,
            "checksum entry seam count changed")
    require("defined(ISAAC_VITA_SAVE_CHECKSUM_FASTPATH)" in text,
            "checksum feature fence disappeared")
    require("/* 0025b340  push ebp */" in text and
            "/* 0025b4a8  ret 8 */" in text,
            "translated fallback body disappeared")

    vita = Image(str(pe_path), 0x98000000)
    vita_result = gen_all._translate_function(
        vita, {"rva": ROOT}, None, {}, pin_img=pin
    )
    require(vita_result["stub"] is None,
            "Vita-base checksum target became a stub")
    require(vita_result["vita_save_checksum_fastpath"],
            "Vita-base checksum fast-path proof was not selected")
    require(vita_result["text"].count(
        "isaac_vita_save_checksum_guest_try(c)") == 1,
        "Vita-base checksum entry seam count changed")


def run_native_oracle(cc: str, saves: list[Path]) -> str:
    for save in saves:
        require(save.is_file(), f"save does not exist: {save}")
    with tempfile.TemporaryDirectory(prefix="isaac-save-checksum-") as value:
        output = Path(value) / ("oracle.exe" if os.name == "nt" else "oracle")
        run([
            cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            str(RUNTIME / "host_vita_save_checksum_native.c"),
            str(RUNTIME / "host_vita_save_checksum_native_oracle.c"),
            "-o", str(output),
        ])
        return run([str(output), *(str(path) for path in saves)]).strip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=Path)
    parser.add_argument("--cc")
    parser.add_argument("--save", action="append", default=[], type=Path)
    arguments = parser.parse_args()

    require(arguments.pe.stat().st_size == PE_SIZE, "frozen PE size changed")
    require(digest(arguments.pe) == PE_SHA256, "frozen PE hash changed")
    verify_codegen(arguments.pe)
    result = run_native_oracle(compiler(arguments.cc), arguments.save)
    require(result.startswith("Vita save checksum oracle: PASS;"),
            "native differential oracle result changed")
    print(result)
    print("Vita save checksum fast path: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
