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
        # off/on: display raster 720 OFF/ON.  fbo-rt: the
        # ISAAC_VITA_STOCK_FBO_RT_SCENES=8 knob (0007 hook called once per
        # initialize with 8, banner prints the returned value); fbo-rt-720 the
        # same on the 720 raster.  fbo-vr-observe/fbo-vr-on: the
        # ISAAC_VITA_STOCK_FBO_VALID_REGION=1/2 knob on top of rt-scenes=8
        # (0009 mode hook called once with 0/1, banner prints
        # fbo-valid-region=observe/on).
        for mode in ("off", "on", "fbo-rt", "fbo-rt-720",
                     "fbo-vr-observe", "fbo-vr-on"):
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
            if mode in ("on", "fbo-rt-720"):
                command.insert(
                    7, "-DISAAC_VITA_DISPLAY_RASTER_720=1"
                )
            if mode in ("fbo-rt", "fbo-rt-720", "fbo-vr-observe", "fbo-vr-on"):
                command.insert(7, "-DISAAC_VITA_STOCK_FBO_RT_SCENES=8")
            if mode == "fbo-vr-observe":
                command.insert(7, "-DISAAC_VITA_STOCK_FBO_VALID_REGION=1")
            elif mode == "fbo-vr-on":
                command.insert(7, "-DISAAC_VITA_STOCK_FBO_VALID_REGION=2")
            subprocess.run(command, check=True)
            completed = subprocess.run(
                [str(executable)], check=True, text=True,
                capture_output=True,
            )
            expected = (
                "Stock vitaGL host oracle: PASS "
                "(720x408 init; 720x405 viewport; 960x540 logical"
                if mode in ("on", "fbo-rt-720") else
                "Stock vitaGL host oracle: PASS "
                "(960x544 init; display raster OFF"
            )
            if mode in ("fbo-rt", "fbo-rt-720", "fbo-vr-observe", "fbo-vr-on"):
                expected += "; fbo rt-scenes=8"
            if mode == "fbo-vr-observe":
                expected += "; fbo valid-region=observe"
            elif mode == "fbo-vr-on":
                expected += "; fbo valid-region=on"
            expected += ")"
            if expected not in completed.stdout:
                raise AssertionError(
                    "stock vitaGL oracle returned unexpected output: "
                    f"{completed.stdout!r}"
                )
            print(expected)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
