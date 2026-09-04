#!/usr/bin/env python3
"""Execute the Vita windowless USER32 policy on a 32-bit host build."""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parent
RUNTIME = ROOT / "runtime"


def run_checked(command: list[str], *, env: dict[str, str] | None = None) -> None:
    process = subprocess.run(
        command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )
    output = process.stdout.decode("mbcs" if os.name == "nt" else "utf-8",
                                   errors="replace")
    if output:
        print(output, end="" if output.endswith("\n") else "\n")
    if process.returncode:
        raise SystemExit(process.returncode)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=pathlib.Path)
    args = parser.parse_args()

    contract = [sys.executable, str(ROOT / "test_vita_import_id_map.py")]
    if args.pe is not None:
        contract.extend(("--pe", str(args.pe.resolve())))
    run_checked(contract)

    if os.name != "nt":
        print("SKIP: executable USER32 oracle needs Windows x86 MSVC; "
              "run vita/test_user32_imports.sh for the Vita ABI half")
        return

    from build_all import msvc_env

    environment, compiler = msvc_env()
    work = pathlib.Path(tempfile.mkdtemp(prefix="isaac-user32-x86-"))
    try:
        executable = work / "vita-user32-oracle.exe"
        run_checked([
            compiler,
            "/nologo", "/TC", "/std:c11", "/O2", "/W4", "/WX",
            # guest.h intentionally diagnoses a 32-bit constant truncation in
            # an unrelated guard helper; the production Vita compiler does not.
            "/wd4310",
            "/DGUEST_IMAGE_BASE=0x98000000u",
            "/I", str(RUNTIME),
            str(RUNTIME / "host_vita_user32.c"),
            str(RUNTIME / "vita_user32_import_test.c"),
            "/Fe:" + str(executable),
        ], env=environment)
        run_checked([str(executable)], env=environment)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("Vita USER32 x86 behavior oracle: PASS")


if __name__ == "__main__":
    main()
