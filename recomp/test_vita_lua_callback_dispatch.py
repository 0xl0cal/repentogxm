#!/usr/bin/env python3
"""Run frozen native _RunCallback through the production 32-bit Lua bridge."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from typing import NoReturn


HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
VITA = HERE / "vita"
SETUP_HARNESS = VITA / "eid_oracle" / "eid_startup_harness.lua"
ORACLE = RUNTIME / "vita_lua_callback_dispatch_oracle.c"
DEFAULT_SENTINEL = (
    HERE.parent / "tools" / "isaac_vita_lua_sentinel" / "main.lua"
)

PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
RUN_CALLBACK_RVA = 0x0040EEF0
RUN_CALLBACK_END = 0x0040EF8E
RUN_CALLBACK_INSNS = 69
RUN_CALLBACK_RAW_SHA256 = (
    "d59bfcf1fe30bd2d5fc37818cdbfd97b209f41679c812ed762d43445eb522a87"
)
RUN_CALLBACK_CFG_SHA256 = (
    "b86e1ce2efaf0a3538824ae8ffedb2a7126ad0257053e01a261ad34fe223d996"
)
RUN_CALLBACK_ORDER_SHA256 = (
    "a4479278f877b330d717e849610b4cf88b38ddbbec3edc3787ab76c2be51d851"
)
# Re-frozen 2026-09-04 on wf/flags-local 2cd6417 (GPR-local GR()/GPUSH()
# spelling, flags-local, leaf/IAT tokens): the five IAT call sites, ret 8 and
# int3 below are still pinned individually, 69 instructions, no stub.
RUN_CALLBACK_DEFAULT_SOURCE_SHA256 = (
    "5075947a83fdd696a76efc9498cd93cfd04b581c961bda14fafa0c1acdcea5b9"
)
RUN_CALLBACK_VITA_SOURCE_SHA256 = (
    "4d88da2f86abe5965f37d2b056fda3ecbfaf9a5aed6e34352fb6848013186339"
)
RUN_CALLBACK_CALLERS = (0x003FCD4B, 0x003FFE6B, 0x00401AAE, 0x00401B8E)

RUN_CALLBACK_NAME_RVA = 0x0074F724
RUN_CALLBACK_NAME = b"_RunCallback\0"
REGISTRY_CAPTURE_START = 0x003FCC31
REGISTRY_CAPTURE_END = 0x003FCC6E
REGISTRY_CAPTURE_SHA256 = (
    "29dbbec2bb955c8d1922c522da164ca616634fb7aed2d73596b83fcc51c5aa69"
)
REGISTRY_CAPTURE_GETGLOBAL = 0x003FCC48
REGISTRY_CAPTURE_REF = 0x003FCC5C
REGISTRY_CAPTURE_STORE = 0x003FCC69

VITA_IMAGE_BASE = 0x98000000
SENTINEL_SIZE = 881
SENTINEL_SHA256 = (
    "b50aa71ed6faf6b140231c9381af13589f8e27df619c7fea894f14f81c278068"
)
EXPECTED_STDOUT = (
    "generated _RunCallback -> project sentinel: PASS "
    "(success=2 luaL_error=25 direct-longjmp=25 helpers=50)\n"
)

CORE_FILES = {
    "enums.lua": (
        149_353,
        "dce1892f51e4e645d66ac8ffb31d217e8c36a32457c1005dd128978141103dc5",
    ),
    "main.lua": (
        45_891,
        "ae0efa082e0ea8844a6e7a5ab55e4a96a8dc87a8fa046483e590c0677c40cfdf",
    ),
}


def fail(message: str) -> NoReturn:
    raise SystemExit(f"FAIL: {message}")


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def validate_inputs(pe: Path, core: Path, sentinel: Path) -> None:
    require(pe.is_file(), f"frozen PE is missing: {pe}")
    require(pe.stat().st_size == PE_SIZE, "frozen PE size changed")
    require(digest(pe) == PE_SHA256, "frozen PE hash changed")
    for name, (size, expected) in CORE_FILES.items():
        path = core / name
        require(path.is_file(), f"core script is missing: {name}")
        require(path.stat().st_size == size, f"core script size changed: {name}")
        require(digest(path) == expected, f"core script hash changed: {name}")
    require(sentinel.is_file(), f"project Lua sentinel is missing: {sentinel}")
    require(sentinel.stat().st_size == SENTINEL_SIZE,
            "project Lua sentinel size changed")
    require(digest(sentinel) == SENTINEL_SHA256,
            "project Lua sentinel source changed")


def rel32_callers(image, target: int) -> tuple[int, ...]:
    callers: list[int] = []
    for rva in range(image.text_rva, image.text_end - 4):
        data = image.code_at(rva, 5)
        if data[0] != 0xE8:
            continue
        displacement = int.from_bytes(data[1:], "little", signed=True)
        if (rva + 5 + displacement) & 0xFFFFFFFF == target:
            callers.append(rva)
    return tuple(callers)


def verify_registry_capture(image) -> None:
    require(
        image.code_at(RUN_CALLBACK_NAME_RVA, len(RUN_CALLBACK_NAME)) ==
        RUN_CALLBACK_NAME,
        "frozen _RunCallback global name changed",
    )
    capture = image.code_at(
        REGISTRY_CAPTURE_START, REGISTRY_CAPTURE_END - REGISTRY_CAPTURE_START
    )
    require(hashlib.sha256(capture).hexdigest() == REGISTRY_CAPTURE_SHA256,
            "frozen _RunCallback registry capture changed")

    # Do not infer this boundary from the byte hash alone.  These exact sites
    # connect the named Lua global to an 8-byte {state,key} allocation and the
    # LuaEngine +0x10 field consumed by sub_0040eef0.
    require(image.code_at(REGISTRY_CAPTURE_GETGLOBAL, 12) == bytes.fromhex(
        "6824f7743056ff15bc616030"),
        "_RunCallback lua_getglobal capture site changed")
    require(image.code_at(REGISTRY_CAPTURE_REF, 13) == bytes.fromhex(
        "ff15546260308b742418894704"),
        "_RunCallback luaL_ref/key store changed")
    require(image.code_at(REGISTRY_CAPTURE_STORE, 5) == bytes.fromhex(
        "6a08897e10"),
        "LuaEngine RunCallbackRegistry +0x10 store changed")


def verify_and_emit(pe: Path, destination: Path) -> None:
    os.environ["REPENTOGXM_PE"] = str(pe)
    sys.path.insert(0, str(HERE))
    import gen_all as G  # noqa: E402
    import trans as T  # noqa: E402
    from image import DEFAULT_BASE, Image  # noqa: E402

    pin = Image(str(pe), DEFAULT_BASE)
    verify_registry_capture(pin)
    instructions, order, indirect = T.decode(pin, RUN_CALLBACK_RVA)
    require(len(order) == RUN_CALLBACK_INSNS and not indirect,
            "frozen _RunCallback decoded shape changed")
    require(order[-1] == RUN_CALLBACK_END - 1,
            "frozen _RunCallback decoded end changed")

    cfg = hashlib.sha256()
    ordered_rvas = hashlib.sha256()
    for rva in order:
        instruction = instructions[rva]
        cfg.update(struct.pack("<I", rva))
        cfg.update(bytes((instruction.size,)))
        cfg.update(pin.code_at(rva, instruction.size))
        ordered_rvas.update(struct.pack("<I", rva))
    raw = pin.code_at(RUN_CALLBACK_RVA,
                      RUN_CALLBACK_END - RUN_CALLBACK_RVA)
    require(hashlib.sha256(raw).hexdigest() == RUN_CALLBACK_RAW_SHA256,
            "frozen _RunCallback raw body changed")
    require(cfg.hexdigest() == RUN_CALLBACK_CFG_SHA256,
            "frozen _RunCallback instruction CFG changed")
    require(ordered_rvas.hexdigest() == RUN_CALLBACK_ORDER_SHA256,
            "frozen _RunCallback instruction order changed")
    require(rel32_callers(pin, RUN_CALLBACK_RVA) == RUN_CALLBACK_CALLERS,
            "frozen _RunCallback direct caller census changed")

    sources: dict[int, str] = {}
    expected_digests = {
        DEFAULT_BASE: RUN_CALLBACK_DEFAULT_SOURCE_SHA256,
        VITA_IMAGE_BASE: RUN_CALLBACK_VITA_SOURCE_SHA256,
    }
    for base, expected in expected_digests.items():
        result = G._translate_function(
            Image(str(pe), base), {"rva": RUN_CALLBACK_RVA},
            None, {}, pin_img=pin,
        )
        require(result["stub"] is None and result["insns"] == RUN_CALLBACK_INSNS,
                f"_RunCallback became incomplete at base {base:#x}")
        source = str(result["text"])
        require(hashlib.sha256(source.encode()).hexdigest() == expected,
                f"_RunCallback generated source changed at base {base:#x}")
        require(source.count("lua_rawgeti") == 0,
                "generated wrapper bypassed the frozen IAT unexpectedly")
        for site in (
            "/* 0040ef0c  call dword ptr [0x3060624c] */",
            "/* 0040ef3b  call dword ptr [0x3060623c] */",
            "/* 0040ef4b  call dword ptr [0x30606258] */",
            "/* 0040ef5c  call dword ptr [0x30606254] */",
            "/* 0040ef68  call dword ptr [0x30606170] */",
            "/* 0040ef79  ret 8 */",
            "/* 0040ef8d  int3  */",
        ):
            if base == DEFAULT_BASE:
                require(source.count(site) == 1,
                        f"generated _RunCallback site changed: {site}")
        sources[base] = source

    generated = "\n".join((
        "/* Emitted by test_vita_lua_callback_dispatch.py from the frozen PE. */",
        "#include \"guest.h\"",
        "void sub_003f88d0(CPU *__restrict c);",
        "void sub_0040ed40(CPU *__restrict c);",
        sources[DEFAULT_BASE],
        "",
    ))
    destination.write_text(generated, encoding="utf-8", newline="\n")


def verify_transcript(path: Path) -> bytes:
    data = path.read_bytes()
    rows = data.decode("utf-8").splitlines()
    require("result\tok" in rows, "project sentinel setup did not complete")
    require(rows[-1] == "callback_count\t3",
            "project sentinel callback count changed")
    callbacks = [row for row in rows if row.startswith("callback\t")]
    require(len(callbacks) == 3,
            "project sentinel callback capture is incomplete")
    require(any("ModCallbacks.MC_POST_UPDATE" in row for row in callbacks),
            "project sentinel update callback was not registered")
    require(any("ModCallbacks.MC_POST_RENDER" in row for row in callbacks),
            "project sentinel render callback was not registered")
    require(any("ModCallbacks.MC_POST_GAME_STARTED" in row
                for row in callbacks),
            "project sentinel game-start callback was not registered")
    require(sum(row.startswith("callback_registry\t") for row in rows) == 3,
            "project sentinel core registry census changed")
    require(not any(row.startswith("callback_dispatch\t") for row in rows),
            "Lua setup harness dispatched before the native wrapper")
    return data


def build_and_run(pe: Path, core: Path, sentinel: Path,
                  lua_source: Path | None, keep_build: Path | None) -> None:
    import test_vita_lua_runtime as lua_runtime  # noqa: E402

    manifest = lua_runtime.records()
    source = lua_runtime.discover_source(lua_source)
    lua_sources = lua_runtime.validate_source(source, manifest)
    vcvars = lua_runtime.find_vcvars()

    temporary = None
    if keep_build is None:
        temporary = tempfile.TemporaryDirectory(prefix="isaac-lua-dispatch-")
        build = Path(temporary.name)
    else:
        build = keep_build.resolve()
        build.mkdir(parents=True, exist_ok=True)

    generated_source = build / "run_callback_generated.c"
    verify_and_emit(pe, generated_source)
    quote = lua_runtime.quote
    lua_compile = " ".join(
        ["cl", "/nologo", "/std:c11", "/O2", "/W3", "/MT",
         "/D_CRT_SECURE_NO_WARNINGS", f"/I{quote(source / 'src')}", "/c"] +
        [quote(path) for path in lua_sources]
    )
    bridge_sources = [
        RUNTIME / "host_vita_lua.c",
        ORACLE,
        generated_source,
    ]
    # Two bridge builds: generic handlers only, and the
    # ISAAC_VITA_LUA_IMPORT_FASTDISPATCH build in which the frozen
    # _RunCallback's lua_rawgeti/lua_pushvalue/lua_settop IAT sites reach the
    # typed endpoints (its lua_pcallk/luaL_ref stay generic).  Same sentinel
    # transcript and stdout; the knob build appends its typed-run census.
    variants = (
        ("", "vita-lua-callback-dispatch-oracle.exe"),
        ("/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1 "
         "/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH_ORACLE=1",
         "vita-lua-callback-dispatch-oracle-fast.exe"),
    )
    commands = [lua_compile]
    executables = []
    for defines, name in variants:
        bridge_compile = " ".join(
            ["cl", "/nologo", "/std:c11", "/O2", "/W4", "/WX", "/MT",
             "/wd4310"] + ([defines] if defines else []) +
            [f"/I{quote(source / 'src')}", f"/I{quote(RUNTIME)}", "/c"] +
            [quote(path) for path in bridge_sources]
        )
        objects = [f"{path.stem}.obj" for path in lua_sources + bridge_sources]
        executable = build / name
        link = " ".join([
            "link", "/nologo", f"/out:{quote(executable)}", *objects,
            "kernel32.lib",
        ])
        commands += [bridge_compile, link]
        executables.append(executable)
    lua_runtime.run_cmd(vcvars, commands, build)

    transcripts: list[bytes] = []
    outputs: list[str] = []
    for variant, executable in enumerate(executables):
        for index in range(2):
            transcript = build / f"callback-dispatch-{variant}-{index}.tsv"
            completed = subprocess.run(
                [str(executable), str(SETUP_HARNESS), str(sentinel.parent),
                 str(core), str(transcript)],
                cwd=build, stdin=subprocess.DEVNULL, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
            )
            require(completed.returncode == 0,
                    f"32-bit callback dispatch failed "
                    f"({completed.returncode}):\n"
                    f"{completed.stdout}{completed.stderr}")
            require(completed.stderr == "",
                    "32-bit callback dispatch wrote stderr")
            stdout = completed.stdout.replace("\r\n", "\n")
            if variant == 1:
                match = re.fullmatch(
                    re.escape(EXPECTED_STDOUT) +
                    r"fastdispatch: typed_runs=([1-9][0-9]*)\n", stdout)
                require(match is not None,
                        f"fast dispatch oracle stdout changed: {stdout!r}")
                stdout = EXPECTED_STDOUT
            outputs.append(stdout)
            transcripts.append(verify_transcript(transcript))
    require(outputs == [EXPECTED_STDOUT] * 4,
            "callback dispatch oracle stdout changed")
    require(all(item == transcripts[0] for item in transcripts),
            "project sentinel setup transcript differs between runs/builds")
    if temporary is not None:
        temporary.cleanup()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", required=True, type=Path)
    parser.add_argument("--core-scripts", required=True, type=Path)
    parser.add_argument("--sentinel", type=Path, default=DEFAULT_SENTINEL)
    parser.add_argument("--lua-source", type=Path)
    parser.add_argument("--keep-build", type=Path)
    arguments = parser.parse_args()

    pe = arguments.pe.resolve()
    core = arguments.core_scripts.resolve()
    sentinel = arguments.sentinel.resolve()
    validate_inputs(pe, core, sentinel)
    build_and_run(pe, core, sentinel, arguments.lua_source,
                  arguments.keep_build)
    print(
        "PASS: frozen native _RunCallback -> exact project sentinel; "
        "registry/stack + 25 luaL_error + 25 direct-longjmp recoveries"
    )
    print(
        "NEXT: device-only first engine-owned MC_POST_UPDATE call and log marker"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
