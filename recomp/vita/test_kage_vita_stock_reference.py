#!/usr/bin/env python3
"""Compile and run the stock-vitaGL lifecycle oracle on the host."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from pathlib import Path


def compiler() -> str:
    for name in ("cc", "gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    candidates = (
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
        "LLVM" / "bin" / "clang.exe",
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
        "Microsoft Visual Studio" / "2022" / "Community" / "VC" /
        "Tools" / "Llvm" / "x64" / "bin" / "clang.exe",
    )
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise RuntimeError("no host C compiler found")


def main() -> int:
    vita = Path(__file__).resolve().parent
    runtime = vita.parent / "runtime"
    with tempfile.TemporaryDirectory(prefix="isaac-stock-vitagl-") as value:
        for mode in ("off", "on"):
            executable = Path(value) / (
                f"stock-vitagl-oracle-{mode}.exe" if os.name == "nt" else
                f"stock-vitagl-oracle-{mode}"
            )
            command = [
                compiler(), "-std=c11", "-O2", "-Wall", "-Wextra",
                "-Werror", "-DISAAC_KAGE_VITA_BACKEND_ORACLE=1",
                "-DISAAC_VITA_VITAGL_STOCK_REFERENCE=1",
                f"-I{runtime}",
                str(runtime / "kage_vita_backend.c"),
                str(runtime / "kage_vita_stock_reference_oracle.c"),
                "-o", str(executable),
            ]
            if mode == "on":
                command.insert(
                    7, "-DISAAC_VITA_DISPLAY_RASTER_720=1"
                )
            subprocess.run(command, check=True)
            completed = subprocess.run(
                [str(executable)], check=True, text=True,
                capture_output=True,
            )
            expected = (
                "Stock vitaGL host oracle: PASS "
                "(720x408 init; 720x405 viewport; 960x540 logical)"
                if mode == "on" else
                "Stock vitaGL host oracle: PASS "
                "(960x544 init; display raster OFF)"
            )
            if expected not in completed.stdout:
                raise AssertionError(
                    "stock vitaGL oracle returned unexpected output: "
                    f"{completed.stdout!r}"
                )
            print(expected)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
