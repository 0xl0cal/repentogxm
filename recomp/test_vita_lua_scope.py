#!/usr/bin/env python3
"""Build/run the Lua scope + callback trampoline fast-path oracle.

ISAAC_VITA_LUA_SCOPE_FASTPATH (host_vita_lua.c) is compiled in both states;
the oracle (runtime/vita_lua_scope_oracle.c) must pass identically with the
new code (1, default) and the pre-knob code (0).  32-bit MSVC like the other
Lua bridge oracles: the raw lua_State/stack pointer ABI is part of the test.
The native getClass/getExact seam's owner protocol is included in both builds.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_vita_lua_runtime as lua_runtime  # noqa: E402

RUNTIME = HERE / "runtime"
ORACLE = RUNTIME / "vita_lua_scope_oracle.c"


def build_and_run(fastpath: int, lua_source: Path | None,
                  keep_build: Path | None) -> str:
    manifest = lua_runtime.records()
    source = lua_runtime.discover_source(lua_source)
    lua_sources = lua_runtime.validate_source(source, manifest)
    vcvars = lua_runtime.find_vcvars()
    quote = lua_runtime.quote

    temporary = None
    if keep_build is None:
        temporary = tempfile.TemporaryDirectory(
            prefix=f"isaac-lua-scope-{fastpath}-")
        build = Path(temporary.name)
    else:
        build = (keep_build / f"fastpath-{fastpath}").resolve()
        build.mkdir(parents=True, exist_ok=True)

    lua_compile = " ".join(
        ["cl", "/nologo", "/std:c11", "/O2", "/W3", "/MT",
         "/D_CRT_SECURE_NO_WARNINGS", f"/I{quote(source / 'src')}", "/c"]
        + [quote(path) for path in lua_sources]
    )
    bridge_sources = [RUNTIME / "host_vita_lua.c", ORACLE]
    bridge_compile = " ".join(
        ["cl", "/nologo", "/std:c11", "/O2", "/W4", "/WX", "/MT", "/wd4310",
         f"/DISAAC_VITA_LUA_SCOPE_FASTPATH={fastpath}",
         "/DISAAC_VITA_LUA_NATIVE_GETCLASS=1",
         f"/I{quote(source / 'src')}", f"/I{quote(RUNTIME)}", "/c"]
        + [quote(path) for path in bridge_sources]
    )
    objects = [f"{path.stem}.obj" for path in lua_sources + bridge_sources]
    executable = build / "vita-lua-scope-oracle.exe"
    link = " ".join(["link", "/nologo", f"/out:{quote(executable)}",
                     *objects, "kernel32.lib"])
    lua_runtime.run_cmd(vcvars, [lua_compile, bridge_compile, link], build)

    completed = subprocess.run(
        [str(executable)], cwd=build, stdin=subprocess.DEVNULL, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if completed.returncode or completed.stderr:
        lua_runtime.fail(
            f"scope oracle (fastpath={fastpath}) exit {completed.returncode}:\n"
            f"{completed.stdout}{completed.stderr}")
    line = completed.stdout.strip()
    if f"fastpath={fastpath}:" not in line or not line.endswith(": PASS"):
        lua_runtime.fail(f"scope oracle (fastpath={fastpath}) output: {line}")
    if temporary is not None:
        temporary.cleanup()
    return line


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lua-source", type=Path)
    parser.add_argument("--keep-build", type=Path)
    parser.add_argument("--fastpath", type=int, choices=(0, 1),
                        help="run only this knob state (default: 1 then 0)")
    args = parser.parse_args()
    states = (args.fastpath,) if args.fastpath is not None else (1, 0)
    for fastpath in states:
        print(build_and_run(fastpath, args.lua_source, args.keep_build))
    print("PASS: Lua scope/trampoline fast path oracle "
          f"(knob states {', '.join(str(s) for s in states)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
