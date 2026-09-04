#!/usr/bin/env python3
"""Frozen-PE/codegen oracle for the reserve-free Lua startup split."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from gpr_locals import legacy_text  # noqa: E402


HERE = Path(__file__).resolve().parent
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
ROOT = 0x0050B240
VITA_BASE = 0x98000000
FROZEN_CORE_PACKAGE_PATH_RVA = 0x0074F5D0
FROZEN_CORE_PACKAGE_PATH = b".\\resources\\scripts\\?.lua\0"
FENCE = "#if defined(ISAAC_VITA_LUA) && ISAAC_VITA_LUA"
ROUTE_BLOCK = "\n".join((
    FENCE,
    "            goto L_0050b7a7;",
    "#else",
    "            goto L_0050b7c1;",
    "#endif",
))


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


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


def command(arguments: list[str]) -> str:
    completed = subprocess.run(
        arguments,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    require(
        completed.returncode == 0,
        f"command failed ({completed.returncode}): {arguments!r}\n"
        f"{completed.stdout}",
    )
    return completed.stdout


def verify_codegen(pe_path: Path) -> None:
    os.environ["REPENTOGXM_PE"] = str(pe_path)
    sys.path.insert(0, str(HERE))
    import gen_all as G  # noqa: E402
    from image import DEFAULT_BASE, Image  # noqa: E402

    pin = Image(str(pe_path), DEFAULT_BASE)
    require(
        pin.mem[
            FROZEN_CORE_PACKAGE_PATH_RVA:
            FROZEN_CORE_PACKAGE_PATH_RVA + len(FROZEN_CORE_PACKAGE_PATH)
        ] == FROZEN_CORE_PACKAGE_PATH,
        "frozen Lua core package.path template changed",
    )
    for base in (DEFAULT_BASE, VITA_BASE):
        image = Image(str(pe_path), base)
        result = G._translate_function(
            image, {"rva": ROOT}, None, {}, pin_img=pin
        )
        require(result["stub"] is None,
                f"Lua startup owner became a stub at base {base:#x}")
        require(result["lua_pc_startup_bypass"] is True,
                f"Lua startup seam disappeared at base {base:#x}")
        source = legacy_text(result["text"])
        require(source.count(FENCE) == 1,
                f"Lua startup feature fence count changed at base {base:#x}")
        require(source.count(ROUTE_BLOCK) == 1,
                f"Lua startup ON/OFF route changed at base {base:#x}")

        injection = source.index(
            "/* explicit backend: skip huge reserve; Lua build runs Init */"
        )
        route = source.index(ROUTE_BLOCK, injection)
        original = source.index("/* 0050b6f2  test edi, edi */", route)
        initializer = source.index("sub_003fcb00(c);", original)
        continuation = source.index("L_0050b7c1:", initializer)
        require(injection < route < original < initializer < continuation,
                f"Lua startup fallback ordering changed at base {base:#x}")


def verify_preprocessor(cc: str) -> None:
    # Use the exact emitted conditional, with only its two real labels supplied
    # locally, so both feature values are selected by a C preprocessor rather
    # than by a Python imitation of the condition.
    source = "\n".join((
        "int route(void) {",
        ROUTE_BLOCK,
        "L_0050b7a7: return 0x50b7a7;",
        "L_0050b7c1: return 0x50b7c1;",
        "}",
    ))
    with tempfile.TemporaryDirectory(prefix="isaac-lua-startup-") as value:
        path = Path(value) / "route.c"
        path.write_text(source, encoding="utf-8", newline="\n")
        for enabled, expected, rejected in (
            (0, "goto L_0050b7c1;", "goto L_0050b7a7;"),
            (1, "goto L_0050b7a7;", "goto L_0050b7c1;"),
        ):
            output = command([
                cc, "-E", "-P", "-x", "c",
                f"-DISAAC_VITA_LUA={enabled}", str(path),
            ])
            require(expected in output and rejected not in output,
                    f"ISAAC_VITA_LUA={enabled} selected the wrong route")
            command([
                cc, "-fsyntax-only", "-x", "c",
                f"-DISAAC_VITA_LUA={enabled}", str(path),
            ])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", required=True, type=Path)
    parser.add_argument("--cc")
    arguments = parser.parse_args()

    pe_path = arguments.pe.resolve()
    require(pe_path.stat().st_size == PE_SIZE, "frozen PE size changed")
    require(digest(pe_path) == PE_SHA256, "frozen PE hash changed")
    verify_codegen(pe_path)
    verify_preprocessor(compiler(arguments.cc))
    print("Vita Lua startup OFF->bypass / ON->Init route: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
