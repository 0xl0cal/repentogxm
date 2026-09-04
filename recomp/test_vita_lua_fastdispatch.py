#!/usr/bin/env python3
"""ILP32 host oracle for ISAAC_VITA_LUA_IMPORT_FASTDISPATCH.

Builds pristine Lua 5.3.3 plus the production host_vita_lua.c with the knob
(and its oracle-only typed-body counter) as a 32-bit MSVC executable and runs
runtime/vita_lua_fastdispatch_oracle.c: every typed Lua endpoint against the
generic handler on the same CPU/x86-stack/Lua-stack state (EAX, EDX, ESP,
x87 ring, low-water, fault, census, Lua stack dump), in four stack shapes;
then runtime/vita_lua_fastdispatch_hostile_oracle.c: the same differential
through nested typed imports (generic pcallk root -> guest binding, typed
pushstring root -> GC -> guest __gc), a foreign CPU entering a held bridge,
an owner word cleared under a typed root, a Lua error unwinding an abandoned
nested typed frame, and floor/low-water/misaligned frame shapes.

Also pins the default-OFF scope: host_vita_lua.c and host_vita_import_id.c
preprocessed without the knob carry no typed endpoint, no binding table and no
prepare hook, and the CMake option defaults OFF.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_vita_lua_runtime as lua_runtime  # noqa: E402

RUNTIME = HERE / "runtime"
ORACLE = RUNTIME / "vita_lua_fastdispatch_oracle.c"
HOSTILE = RUNTIME / "vita_lua_fastdispatch_hostile_oracle.c"
PASS_MARKER = "Vita Lua import fast dispatch oracle: PASS"
HOSTILE_MARKER = "Vita Lua fast dispatch HOSTILE oracle: PASS"
KNOB = "ISAAC_VITA_LUA_IMPORT_FASTDISPATCH"
TYPED = (
    "lua_rawgetp", "lua_getmetatable", "lua_type", "lua_touserdata",
    "lua_pushvalue", "lua_rawget", "lua_rawgeti", "lua_settop", "lua_gettop",
    "lua_pushnil", "lua_pushinteger", "lua_pushnumber", "lua_pushstring",
)


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


def verify_build_gate() -> None:
    cmake = (HERE / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    option = re.search(
        r'option\(' + KNOB + r'\s+"[^"]*"\s+(ON|OFF)\)', cmake)
    if option is None or option.group(1) != "OFF":
        fail(f"CMake option {KNOB} must exist and default OFF")
    if len(re.findall(r"COMPILE_DEFINITIONS\s+" + KNOB + r"=1\)", cmake)) != 1:
        fail(f"{KNOB}=1 must be applied exactly once (both owners in one "
             "set_property)")
    owners = re.search(
        r"set_property\(SOURCE\s+\"\$\{ISAAC_RUNTIME\}/host_vita_lua\.c\"\s+"
        r"\"\$\{ISAAC_RUNTIME\}/host_vita_import_id\.c\"\s+APPEND PROPERTY "
        r"COMPILE_DEFINITIONS\s+" + KNOB + r"=1\)", cmake)
    if owners is None:
        fail("the knob must name exactly host_vita_lua.c and "
             "host_vita_import_id.c as its owners")
    if f"if({KNOB} AND NOT ISAAC_VITA_LUA)" not in cmake:
        fail(f"{KNOB} must refuse ISAAC_VITA_LUA=OFF")


def verify_source_inventory() -> None:
    source = (RUNTIME / "host_vita_lua.c").read_text(encoding="utf-8")
    block = source[source.index(f"#if defined({KNOB})"):]
    for name in TYPED:
        if f'{{ "Lua5.3.3r.dll!{name}",' not in block:
            fail(f"typed inventory lost {name}")
    endpoints = re.findall(
        r"static int vita_lua_fast_(\w+)\(CPU \*__restrict c, uint32_t index,",
        block)
    if sorted(endpoints) != sorted(name[len("lua_"):] for name in TYPED):
        fail(f"typed endpoint set differs from the inventory: {endpoints}")
    for absent in ("lua_pushlightuserdata", "lua_tonumberx", "lua_tointegerx"):
        if absent in source:
            fail(f"{absent} is not a frozen import; it must not appear")


def verify_off_scope(env, compiler, root: Path) -> None:
    """Preprocess both owners without the knob: no typed text survives."""
    for unit, defs in (
            ("host_vita_lua.c", ["/DISAAC_VITA_LUA=1"]),
            ("host_vita_import_id.c", ["/DISAAC_VITA_LUA=1",
                                       "/DISAAC_VITA_AUDIO=1",
                                       "/DISAAC_VITA_XINPUT=1"])):
        out = root / (unit + ".off.i")
        command = [compiler, "/nologo", "/P", "/std:c11", "/wd4310", "/wd4996",
                   "/DGUEST_IMAGE_BASE=0x98000000u", "/I", str(RUNTIME),
                   "/I", str(HERE / "vita")]
        if unit == "host_vita_lua.c":
            command += ["/I", str(lua_runtime.discover_source(None) / "src")]
        command += defs + [str(RUNTIME / unit), "/Fi:" + str(out)]
        completed = subprocess.run(command, env=env, capture_output=True)
        if completed.returncode != 0:
            sys.stderr.write(completed.stdout.decode("cp866", errors="replace"))
            sys.stderr.write(completed.stderr.decode("cp866", errors="replace"))
            fail(f"preprocessing {unit} failed")
        text = out.read_text(encoding="utf-8", errors="replace")
        leaked = [token for token in (
            "vita_lua_fast_", "g_isaac_vita_lua_import_fast_by_index",
            "isaac_vita_lua_import_fast_prepare",
            "isaac_vita_lua_import_fast_binding", "fast_ =")
            if token in text]
        if leaked:
            fail(f"default-OFF {unit} retained typed dispatch text: {leaked}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lua-source", type=Path)
    parser.add_argument("--keep-build", type=Path)
    args = parser.parse_args()

    verify_build_gate()
    verify_source_inventory()
    manifest = lua_runtime.records()
    source = lua_runtime.discover_source(args.lua_source)
    lua_sources = lua_runtime.validate_source(source, manifest)
    vcvars = lua_runtime.find_vcvars()
    import build_all as BA  # noqa: E402
    env, compiler = BA.msvc_env()

    temporary = None
    if args.keep_build:
        build = args.keep_build.resolve()
        build.mkdir(parents=True, exist_ok=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="isaac-lua-fast-")
        build = Path(temporary.name)

    verify_off_scope(env, compiler, build)

    quote = lua_runtime.quote
    lua_compile = " ".join(
        ["cl", "/nologo", "/std:c11", "/O2", "/W3",
         "/D_CRT_SECURE_NO_WARNINGS", f"/I{quote(source / 'src')}", "/c"]
        + [quote(path) for path in lua_sources]
    )
    bridge_sources = [RUNTIME / "host_vita_lua.c", ORACLE, HOSTILE]
    bridge_compile = " ".join(
        ["cl", "/nologo", "/std:c11", "/O2", "/W4", "/WX", "/wd4310",
         "/wd4702", f"/D{KNOB}=1", f"/D{KNOB}_ORACLE=1",
         "/DGUEST_IMAGE_BASE=0x98000000u",
         f"/I{quote(source / 'src')}", f"/I{quote(RUNTIME)}", "/c"]
        + [quote(path) for path in bridge_sources]
    )
    common_objects = [f"{path.stem}.obj"
                      for path in lua_sources + [RUNTIME / "host_vita_lua.c"]]
    executable = build / "vita-lua-fastdispatch-oracle.exe"
    hostile = build / "vita-lua-fastdispatch-hostile-oracle.exe"
    link = " ".join(["link", "/nologo", f"/out:{quote(executable)}",
                     *common_objects, f"{ORACLE.stem}.obj"])
    link_hostile = " ".join(["link", "/nologo", f"/out:{quote(hostile)}",
                             *common_objects, f"{HOSTILE.stem}.obj"])
    lua_runtime.run_cmd(vcvars, [lua_compile, bridge_compile, link,
                                 link_hostile], build)

    for exe, marker, what in ((executable, PASS_MARKER, "fast dispatch"),
                              (hostile, HOSTILE_MARKER,
                               "hostile fast dispatch")):
        completed = subprocess.run(
            [str(exe)], cwd=build, stdin=subprocess.DEVNULL, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        sys.stdout.write(completed.stdout if exe is executable else
                         "".join(line + "\n" for line in
                                 completed.stdout.splitlines()
                                 if not line.startswith("  ")))
        sys.stderr.write(completed.stderr)
        if completed.returncode != 0 or marker not in completed.stdout:
            fail(f"{what} oracle failed ({completed.returncode})")
    print("PASS: typed Lua endpoints == generic handlers on identical "
          "CPU/stack/Lua state (ILP32 MSVC), incl. nested/GC/foreign-CPU/"
          "owner-cleared/ERRGCMM/floor/low-water/misaligned hostile shapes; "
          "default-OFF scope pinned")
    if temporary is not None:
        temporary.cleanup()
    return 0


if __name__ == "__main__":
    sys.exit(main())
