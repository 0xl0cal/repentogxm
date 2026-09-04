#!/usr/bin/env python3
"""Build/run the Lua bridge smoke against pristine upstream Lua 5.3.3.

This is intentionally a 32-bit host test: raw lua_State/string/userdata
pointers are part of both the frozen x86 DLL ABI and the Vita ABI.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from typing import NoReturn


ROOT = Path(__file__).resolve().parent
RUNTIME = ROOT / "runtime"
VITA = ROOT / "vita"
SOURCE_MANIFEST = VITA / "lua53_vanilla.cmake"
SMOKE_SCRIPT = VITA / "lua_oracle" / "main.lua"
SMOKE_MODULE = VITA / "lua_oracle" / "oracle_module.lua"
CORE_MODULE = VITA / "lua_oracle" / "json.lua"


def fail(message: str) -> NoReturn:
    raise SystemExit(f"FAIL: {message}")


def records() -> list[tuple[str, str]]:
    pattern = re.compile(r'^\s*"([^"|]+)\|([0-9a-f]{64})"\)?\s*$')
    found = []
    for line in SOURCE_MANIFEST.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if match:
            found.append((match.group(1), match.group(2)))
    if len(found) != 58 or len({name for name, _ in found}) != len(found):
        fail("Lua 5.3.3 source manifest is malformed")
    return found


def discover_source(explicit: Path | None) -> Path:
    candidates: list[Path] = []
    if explicit is not None:
        candidates.append(explicit)
    environment = os.environ.get("ISAAC_LUA53_SOURCE_DIR")
    if environment:
        candidates.append(Path(environment))
    for candidate in candidates:
        if (candidate / "src" / "lua.h").is_file():
            return candidate.resolve()
    fail("pass --lua-source or set ISAAC_LUA53_SOURCE_DIR to pristine lua-5.3.3")


def validate_source(source: Path, manifest: list[tuple[str, str]]) -> list[Path]:
    c_sources = []
    for name, expected in manifest:
        path = source / "src" / name
        if not path.is_file():
            fail(f"Lua source is missing: {name}")
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            fail(f"Lua source hash mismatch: {name}: {actual}")
        if path.suffix == ".c":
            c_sources.append(path)
    return c_sources


def find_vcvars() -> Path:
    roots = [os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles")]
    for root in filter(None, roots):
        vswhere = Path(root) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        if not vswhere.is_file():
            continue
        result = subprocess.run(
            [str(vswhere), "-latest", "-products", "*", "-requires",
             "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationPath"],
            check=True, text=True, stdout=subprocess.PIPE,
        )
        installation = result.stdout.strip()
        if installation:
            candidate = Path(installation) / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"
            if candidate.is_file():
                return candidate
    fail("Visual Studio C++ x86 tools were not found through vswhere")


def quote(value: Path | str) -> str:
    return subprocess.list2cmdline([str(value)])


def run_cmd(vcvars: Path, commands: list[str], cwd: Path,
            capture: bool = False) -> str:
    command = f'call {quote(vcvars)} x86 >nul && ' + " && ".join(commands)
    completed = subprocess.run(
        command, cwd=cwd, text=True, shell=True,
        stdout=subprocess.PIPE if capture else None)
    if capture:
        sys.stdout.write(completed.stdout)
    if completed.returncode:
        fail(f"32-bit bridge build/run failed with exit {completed.returncode}")
    return completed.stdout or ""


# The bridge is built five times: as shipped (every knob off); with
# ISAAC_VITA_LUA_IMPORT_FASTDISPATCH, whose typed endpoints serve
# settop/gettop/pushstring/pushinteger/... on the bound main CPU while the
# unbound probe CPUs take their generic fallback (that build additionally
# reports typed_runs > 0); with the lua_gc policy knobs ON (host_vita_lua_gc.c
# joins the TU set and the oracle's GC cases expect the clamp line for the
# pinned site and the gc window line); with the native LuaBridge
# indexMetaMethod replay plus its VERIFY variant; and with the replay alone
# (the perf configuration).  main.lua prints a digest of every __index
# scenario; every build must print the same digest, and the ON builds' own
# counters must agree with what the fixture exercised.  The "trip" run
# (oracle argv[2]) adds a getter whose result is a fresh table per call:
# VERIFY must report exactly one mismatch, disable the replay, and the
# fallback must keep the digest identical to the OFF build.
BRIDGE_VARIANTS: dict[str, list[str]] = {
    "off": [],
    "fastdispatch": [
        "/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1",
        "/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH_ORACLE=1",
    ],
    "gc": [
        "/DISAAC_VITA_LUA_GCCOLLECT_CLAMP=1",
        "/DISAAC_VITA_LUA_GCCOLLECT_STEP_KB=256",
        "/DISAAC_VITA_LUA_GC_PROFILE=1",
    ],
    "native-index": [
        "/DISAAC_VITA_LUA_NATIVE_INDEX=1",
        "/DISAAC_VITA_LUA_NATIVE_INDEX_VERIFY=1",
    ],
    "native-index-noverify": ["/DISAAC_VITA_LUA_NATIVE_INDEX=1"],
}
# TUs that join the bridge only under their knob, exactly as
# recomp/vita/CMakeLists.txt adds them (host_vita_lua_gc.c #errors when it is
# compiled without one of its features).
BRIDGE_EXTRA_SOURCES: dict[str, list[Path]] = {
    "gc": [RUNTIME / "host_vita_lua_gc.c"],
}
# Variants that also run with main.lua's VERIFY-trip scenarios armed.
TRIP_VARIANTS = ("off", "native-index")
SCENARIOS = 32
TRIP_SCENARIOS = 36
NATIVE_INDEX_STATS = re.compile(
    r"\[log\] \[isaac-lua\] native-index win=(\d+) hits=(\d+) "
    r"kind\(method,property,nil\)=(\d+),(\d+),(\d+) hops=(\d+) "
    r"fallbacks=(\d+) reason\(nomt,notcf,propget,propcf,parent,off\)="
    r"(\d+),(\d+),(\d+),(\d+),(\d+),(\d+) "
    r"verify\(runs,mismatch\)=(\d+),(\d+) disabled=(\d+)$")
EXPECT_LINE = re.compile(
    r"^\[expect\] hits=(\d+) fallbacks=(\d+) nested=(\d+)$", re.MULTILINE)


def digest_lines(output: str) -> list[str]:
    return [line[len("[digest] "):] for line in output.splitlines()
            if line.startswith("[digest] ")]


def check_digests(name: str, off_output: str, on_output: str,
                  count: int) -> None:
    off_digest = digest_lines(off_output)
    on_digest = digest_lines(on_output)
    if len(off_digest) != count:
        fail(f"{name}: __index fixture digest has {len(off_digest)} lines, "
             f"expected {count}")
    if off_digest != on_digest:
        for left, right in zip(off_digest, on_digest):
            if left != right:
                fail(f"{name}: __index digest differs between builds:\n"
                     f"  off: {left}\n  on:  {right}")
        fail(f"{name}: __index digest differs between builds (length)")
    if any("native-index" in line for line in off_output.splitlines()):
        fail("the OFF bridge logged native-index lines")


def check_native_index(name: str, on_output: str, verify: bool,
                       trip: bool) -> str:
    if "native-index fallback:" not in on_output:
        fail(f"{name}: native-index fallbacks were not logged individually")
    expect = EXPECT_LINE.search(on_output)
    stats = [NATIVE_INDEX_STATS.match(line) for line in on_output.splitlines()]
    stats = [match for match in stats if match]
    if not expect or not stats:
        fail(f"{name}: native-index stats/expectation lines missing")
    fields = [int(value) for value in stats[-1].groups()]
    (_, hits, method, prop, nil, hops, fallbacks,
     nomt, notcf, propget, propcf, parent, off,
     verify_runs, verify_mismatch, disabled) = fields
    expected_hits, expected_fallbacks, nested = (
        int(v) for v in expect.groups())
    observed = (hits, method, prop, nil, hops, fallbacks,
                nomt, notcf, propget, propcf, parent, off,
                verify_runs, verify_mismatch, disabled)
    # 22 hits: 8 method (method, method-call, parent-method,
    # parent-method-call, method-error, namespace-method, arity-3,
    # self-index-key), 8 property (property, parent-property, boxed-property,
    # base-property, namespace-property, getter-self, getter-nested outer +
    # inner), 6 nil (nil, nil-integer-key, namespace-nil, arity-1,
    # nil-bool-key, nil-nan-key); 9 __parent hops; 9 fallbacks: 1
    # no-metatable, 3 not-cfunction (not-cfunction, const-table,
    # hop-then-lua-function), 3 propget-not-table (missing-propget,
    # propget-not-table, arity-1-fallback), 1 propget-not-cfunction, 1
    # parent-not-table.
    want_hits, want_method, want_prop, want_nil, want_hops = 22, 8, 8, 6, 9
    want_reasons = [1, 3, 3, 1, 1, 0]
    want_verify_runs = want_mismatch = want_disabled = 0
    if verify:
        # Re-running the outer getter-nested body repeats its inner hit.
        want_hits += nested
        want_prop += nested
        want_verify_runs = want_hits
    if trip:
        # trip-fresh-table is a property hit whose VERIFY trips; the three
        # post-trip accesses fall back with reason "off" (not logged
        # individually: only the first 8 fallbacks are).
        want_hits += 1
        want_prop += 1
        want_verify_runs += 1
        want_reasons[5] = 3
        want_mismatch = want_disabled = 1
    wanted = (want_hits, want_method, want_prop, want_nil, want_hops,
              expected_fallbacks, *want_reasons,
              want_verify_runs, want_mismatch, want_disabled)
    if (expected_hits != (23 if trip else 22) or nested != 1 or
            expected_fallbacks != (12 if trip else 9) or observed != wanted):
        fail(f"{name}: native-index counters {observed} != expected {wanted} "
             f"(fixture hits={expected_hits} fallbacks={expected_fallbacks} "
             f"nested={nested})")
    mismatches = [line for line in on_output.splitlines()
                  if "native-index verify mismatch" in line]
    if trip:
        if (len(mismatches) != 1 or
                "verify mismatch: slot value kind=property" not in mismatches[0]
                or "key=string:pack" not in mismatches[0]
                or "replay disabled" not in mismatches[0]):
            fail(f"{name}: VERIFY trip did not log exactly one slot-value "
                 f"mismatch: {mismatches}")
    elif mismatches:
        fail(f"{name}: native-index VERIFY reported a mismatch")
    return stats[-1].group(0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lua-source", type=Path)
    parser.add_argument("--keep-build", type=Path)
    args = parser.parse_args()

    manifest = records()
    source = discover_source(args.lua_source)
    lua_sources = validate_source(source, manifest)
    vcvars = find_vcvars()

    temporary = None
    if args.keep_build:
        build = args.keep_build.resolve()
        build.mkdir(parents=True, exist_ok=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="isaac-lua53-")
        build = Path(temporary.name)

    lua_compile = " ".join(
        ["cl", "/nologo", "/std:c11", "/O2", "/W3",
         "/D_CRT_SECURE_NO_WARNINGS", f"/I{quote(source / 'src')}", "/c"]
        + [quote(path) for path in lua_sources]
    )
    bridge_sources = [
        RUNTIME / "host_vita_lua.c",
        RUNTIME / "vita_lua_runtime_oracle.c",
    ]
    lua_objects = [f"{path.stem}.obj" for path in lua_sources]
    variants: dict[str, tuple[str, str, Path]] = {}
    for variant, defines in BRIDGE_VARIANTS.items():
        variant_dir = build / variant
        variant_dir.mkdir(parents=True, exist_ok=True)
        variant_sources = (bridge_sources[:1]
                           + BRIDGE_EXTRA_SOURCES.get(variant, [])
                           + bridge_sources[1:])
        bridge_compile = " ".join(
            ["cl", "/nologo", "/std:c11", "/O2", "/W4", "/WX",
             "/wd4310", *defines, f"/I{quote(source / 'src')}",
             f"/I{quote(RUNTIME)}", f"/Fo{quote(str(variant_dir) + os.sep)}",
             "/c"]
            + [quote(path) for path in variant_sources]
        )
        executable = variant_dir / "vita-lua-runtime-oracle.exe"
        link = " ".join(
            ["link", "/nologo", f"/out:{quote(executable)}", *lua_objects,
             *[quote(variant_dir / f"{path.stem}.obj")
               for path in variant_sources]]
        )
        variants[variant] = (bridge_compile, link, executable)
    data_root = build / "oracle-data"
    mod_root = data_root / "mods" / "836319872"
    core_root = data_root / "resources" / "scripts"
    mod_root.mkdir(parents=True, exist_ok=True)
    core_root.mkdir(parents=True, exist_ok=True)
    main_script = mod_root / "main.lua"
    shutil.copyfile(SMOKE_SCRIPT, main_script)
    shutil.copyfile(SMOKE_MODULE, mod_root / "oracle_module.lua")
    shutil.copyfile(CORE_MODULE, core_root / "json.lua")

    # The oracle exercises luaL_loadfilex's specified NULL-filename/stdin
    # fallback.  Give it deterministic EOF instead of inheriting a terminal.
    run_cmd(vcvars, [lua_compile], build)
    outputs: dict[str, str] = {}
    for variant, (bridge_compile, link, executable) in variants.items():
        run_cmd(vcvars, [bridge_compile, link], build)
        modes = ("normal", "trip") if variant in TRIP_VARIANTS else (
            "normal",)
        for mode in modes:
            execute = (f"{quote(executable)} {quote(main_script)}"
                       + (" trip" if mode == "trip" else "") + " < NUL")
            print(f"--- bridge variant: {variant} ({mode})")
            outputs[f"{variant}:{mode}"] = run_cmd(
                vcvars, [execute], build, capture=True)
    check_digests("normal", outputs["off:normal"],
                  outputs["native-index:normal"], SCENARIOS)
    check_digests("normal/noverify", outputs["off:normal"],
                  outputs["native-index-noverify:normal"], SCENARIOS)
    check_digests("trip", outputs["off:trip"],
                  outputs["native-index:trip"], TRIP_SCENARIOS)
    # The typed-endpoint and lua_gc-knob bridges leave __index alone: their
    # digest must be the OFF build's.
    check_digests("normal/fastdispatch", outputs["off:normal"],
                  outputs["fastdispatch:normal"], SCENARIOS)
    check_digests("normal/gc", outputs["off:normal"],
                  outputs["gc:normal"], SCENARIOS)
    stats = check_native_index("on+verify", outputs["native-index:normal"],
                               verify=True, trip=False)
    stats_plain = check_native_index(
        "on", outputs["native-index-noverify:normal"], verify=False,
        trip=False)
    stats_trip = check_native_index("on+verify trip",
                                    outputs["native-index:trip"],
                                    verify=True, trip=True)
    print("PASS: exact Lua 5.3.3 source closure + 32-bit bridge oracle "
          "(generic, ISAAC_VITA_LUA_IMPORT_FASTDISPATCH, lua_gc-knob, "
          "ISAAC_VITA_LUA_NATIVE_INDEX+VERIFY and NATIVE_INDEX bridge builds)")
    print(f"PASS: native-index digest identical OFF vs ON+VERIFY vs ON "
          f"({SCENARIOS} scenarios) and OFF vs ON+VERIFY with the VERIFY trip "
          f"({TRIP_SCENARIOS}); " + stats[len("[log] "):])
    print("PASS: native-index ON without VERIFY: " + stats_plain[len("[log] "):])
    print("PASS: native-index VERIFY trip disables the replay: "
          + stats_trip[len("[log] "):])
    if temporary is not None:
        try:
            temporary.cleanup()
        except (OSError, RecursionError):
            # A lingering toolchain helper (vctip) can hold the directory
            # for a moment; leave temp garbage rather than fail the proof.
            shutil.rmtree(build, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
