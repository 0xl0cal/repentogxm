#!/usr/bin/env python3
"""Prove three frozen parser roots and their exact Save Reader edges."""

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
ROOT_SITES = {0x005237A0: 48, 0x00524320: 82, 0x005282D0: 196}
ROOT_GUEST_CALLS = {0x005237A0: 108, 0x00524320: 174, 0x005282D0: 411}
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
ORACLE_RESULT = (
    "Vita Save Reader direct-edge oracle: PASS; checks=33; "
    "rva=1; va=1; fault=1; rejected=1"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(command: list[str]) -> str:
    result = subprocess.run(
        command, check=False, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True,
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
    for root, count in ROOT_SITES.items():
        for base in (DEFAULT_BASE, 0x98000000):
            result = gen_all._translate_function(
                Image(str(pe_path), base), {"rva": root}, None, {},
                pin_img=pin,
            )
            require(result["stub"] is None,
                    f"direct-edge owner became a stub at {root:#x}/{base:#x}")
            require(len(result["vita_save_reader_direct_edges"]) == count,
                    f"direct-edge census changed at {root:#x}/{base:#x}")
            text = result["text"]
            require(text.count(
                "isaac_vita_save_reader_direct_try(c, _target)") == count,
                f"direct helper seam count changed at {root:#x}/{base:#x}")
            require(text.count("if (c->fault != NULL) return;") == count,
                    f"returning-fault guard count changed at {root:#x}")
            require(text.count("guest_call(c, _target);") ==
                    ROOT_GUEST_CALLS[root],
                    f"guest_call fallback count changed at {root:#x}")


def run_oracle(cc: str) -> str:
    with tempfile.TemporaryDirectory(prefix="isaac-reader-direct-") as value:
        output = Path(value) / ("oracle.exe" if os.name == "nt" else "oracle")
        run([
            cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-DISAAC_VITA_SAVE_READER_DIRECT_EDGES=1",
            "-DISAAC_VITA_PHASE_PROFILE=1",
            "-DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
            "-I", str(RUNTIME),
            str(RUNTIME / "host_vita_save_reader_direct.c"),
            str(RUNTIME / "host_vita_save_reader_direct_oracle.c"),
            "-o", str(output),
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
    require(result == ORACLE_RESULT, "direct-edge oracle result changed")
    print(result)
    print("Vita Save Reader direct edges: PASS; sites=326")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
