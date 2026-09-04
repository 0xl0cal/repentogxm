#!/usr/bin/env python3
"""Prove the frozen read32 wrapper seam and run its narrow oracle."""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
ROOT = 0x0052EA90
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
ORACLE_RESULT = (
    "Vita Save read32 fused oracle: PASS; checks=57; "
    "handled=3; returning-fault=2; rejected=4"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(command: list[str]) -> str:
    result = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}): {command!r}\n{result.stdout}"
        )
    return result.stdout


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def compiler(explicit: str | None) -> str:
    if explicit:
        return explicit
    if os.environ.get("CC"):
        return os.environ["CC"]
    for candidate in ("clang", "cc", "gcc"):
        found = shutil.which(candidate)
        if found:
            return found
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
    for base in (DEFAULT_BASE, 0x98000000):
        image = Image(str(pe_path), base)
        result = gen_all._translate_function(
            image, {"rva": ROOT}, None, {}, pin_img=pin
        )
        require(result["stub"] is None,
                f"read32 wrapper became a stub at base {base:#x}")
        require(result["vita_save_read32_fused"],
                f"read32 fusion proof was not selected at base {base:#x}")
        text = result["text"]
        require(text.count("isaac_vita_save_read32_guest_try(c)") == 1,
                "read32 fusion entry seam count changed")
        require("defined(ISAAC_VITA_SAVE_READ32_FUSED)" in text,
                "read32 fusion feature fence disappeared")
        require("/* 0052ea90  push ebp */" in text and
                "/* 0052eabc  ret 4 */" in text and
                "sub_0025b340(c);" in text,
                "translated read32 fallback disappeared")


def run_oracle(cc: str) -> str:
    with tempfile.TemporaryDirectory(prefix="isaac-save-read32-") as value:
        output = Path(value) / ("oracle.exe" if os.name == "nt" else "oracle")
        run([
            cc,
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-DISAAC_VITA_SAVE_READ32_FUSED=1",
            "-DISAAC_VITA_PHASE_PROFILE=1",
            "-DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
            "-I",
            str(RUNTIME),
            str(RUNTIME / "host_vita_save_read32_guest.c"),
            str(RUNTIME / "host_vita_save_read32_guest_oracle.c"),
            "-o",
            str(output),
        ])
        return run([str(output)]).strip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=Path)
    parser.add_argument("--cc")
    arguments = parser.parse_args()

    require(arguments.pe.stat().st_size == PE_SIZE, "frozen PE size changed")
    require(digest(arguments.pe) == PE_SHA256, "frozen PE hash changed")
    verify_codegen(arguments.pe)
    result = run_oracle(compiler(arguments.cc))
    require(result == ORACLE_RESULT, "Save read32 oracle result changed")
    print(result)
    print("Vita Save read32 fused path: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
