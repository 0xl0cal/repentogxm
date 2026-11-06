#!/usr/bin/env python3
"""ISAAC_VITA_LUA_CORE_ARM host checks (codegen-only knob).

* OFF identity: host_vita_lua.c preprocesses to the base commit's tokens in
  three define sets; ON adds exactly the one core-isa log statement.
* CMake: the Lua archive's compile options select -mthumb unless the knob is
  ON, -marm only then, and the bridge define is scoped to host_vita_lua.c.
* Raw allocator gate: both ISA-dependent pins follow the knob (the option
  sequence and the dead l_alloc relocation encoding, the latter checked on
  clang-assembled objects of the verbatim lauxlib.c body), and the
  tools/build_vita.py receipt verifier mirrors the gate's tables.
The ARM encoding itself is proved on an ARM build (objdump) and on the
console by the "[isaac-lua] core isa=" line.

usage: python recomp/test_vita_lua_core_arm.py --lua-source LUA-5.3.3 [--work DIR]
(--work defaults to a new temporary directory)
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
BASE_COMMIT = "2d8404cd53b8f723158eb6e86390d38e1a53da6b"
sys.path.insert(0, str(HERE))
import test_vita_lua_runtime as lua_runtime  # noqa: E402


def require(ok: bool, message: str) -> None:
    if not ok:
        raise SystemExit(f"FAIL: {message}")


def preprocess(vcvars: Path, work: Path, path: Path, defines: str, lua_src: Path,
               name: str) -> list[bytes]:
    q = lua_runtime.quote
    out = work / f"{name}.i"
    line = (f'call {q(vcvars)} x86 >nul && cl /nologo /EP {defines} '
            f'/I{q(RUNTIME)} /I{q(lua_src / "src")} {q(path)} > {q(out)}')
    done = subprocess.run(line, cwd=work, shell=True, stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE, text=True)
    require(done.returncode == 0, f"preprocess {name} failed: {done.stderr[-400:]}")
    return [l for l in out.read_bytes().splitlines() if l.strip()]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lua-source", type=Path)
    parser.add_argument("--work", type=Path,
                        help="scratch directory (default: a new temporary directory)")
    args = parser.parse_args()
    work = (args.work or Path(tempfile.mkdtemp(prefix="isaac-lua-core-arm-"))).resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    vcvars = lua_runtime.find_vcvars()
    lua_src = lua_runtime.discover_source(args.lua_source)

    have_base = subprocess.run(["git", "cat-file", "-e", BASE_COMMIT + "^{commit}"],
                               cwd=HERE, capture_output=True).returncode == 0
    if not have_base:
        print(f"OFF identity: SKIP (base commit {BASE_COMMIT[:7]} is not in this "
              "repository's history)")
    else:
        base = work / "base_host_vita_lua.c"
        base.write_bytes(subprocess.run(
            ["git", "show", f"{BASE_COMMIT}:recomp/runtime/host_vita_lua.c"],
            cwd=HERE, capture_output=True, check=True).stdout)
        new = RUNTIME / "host_vita_lua.c"
        sets = ("", "/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1",
                "/DISAAC_VITA_LUA_NATIVE_INDEX=1 /DISAAC_VITA_LUA_NATIVE_GETCLASS=1 "
                "/DISAAC_VITA_LUA_CALL_ERROR_CATCH=1 /DISAAC_VITA_LUA_GCSTEP_CLAMP_KB=128 "
                "/DISAAC_VITA_LUA_COST_PROFILE=1")
        for i, defines in enumerate(sets):
            a = preprocess(vcvars, work, base, defines, lua_src, f"base{i}")
            b = preprocess(vcvars, work, new, defines, lua_src, f"off{i}")
            require(a == b, f"OFF preprocessing differs from {BASE_COMMIT[:7]} (set {i})")
            on = preprocess(vcvars, work, new, defines + " /DISAAC_VITA_LUA_CORE_ARM=1",
                            lua_src, f"on{i}")
            added = [l for l in on if l not in set(a)]
            text = b"\n".join(added)
            require(b"core isa=%s lua_pushnil=0x%08x" in text and len(on) - len(a) <= 8,
                    f"ON adds more than the core-isa log (set {i}): {added}")
        print(f"OFF identity: PASS ({len(sets)} define sets equal {BASE_COMMIT[:7]}; ON adds only the core-isa log)")

    cmake = (HERE / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    block = re.search(r"if\(ISAAC_VITA_LUA_CORE_ARM\)\n\s*set\(ISAAC_LUA53_ISA -marm\)\n"
                      r"\s*else\(\)\n\s*set\(ISAAC_LUA53_ISA -mthumb\)\n\s*endif\(\)\n"
                      r"\s*target_compile_options\(isaac_lua53 PRIVATE\n\s*-std=gnu11 -O2\n"
                      r"\s*-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp \$\{ISAAC_LUA53_ISA\}\n",
                      cmake)
    require(block is not None, "isaac_lua53 ISA selection block changed")
    # The other -marm is the pre-existing generated-corpus ISAAC_VITA_GUEST_ISA knob.
    require(cmake.count("-marm") == 2, "unexpected -marm use in CMakeLists.txt")
    require(re.search(r'option\(ISAAC_VITA_LUA_CORE_ARM\n\s+"[^"]*" OFF\)', cmake) is not None,
            "ISAAC_VITA_LUA_CORE_ARM must default OFF")
    print("CMake: PASS (OFF -> -mthumb as before, ON -> -marm, default OFF)")

    sys.path.insert(0, str(HERE / "vita"))
    import vita_raw_allocator_gate as gate  # noqa: E402
    thumb = ["gcc", *gate.LUA_REQUIRED_COMPILE_OPTIONS]
    arm = ["-marm" if t == "-mthumb" else t for t in thumb]
    cases = ((thumb, False, True), (thumb, True, False), (arm, True, True),
             (arm, False, False), (arm + ["-O3"], True, False),
             ([t for t in arm if t != "-marm"], True, False))
    for tokens, core_arm, accepted in cases:
        try:
            gate.verify_lua_compile_options(tokens, core_arm)
            ok = True
        except gate.GateError:
            ok = False
        require(ok == accepted, f"gate core_arm={core_arm} accepted={ok} for {tokens}")
    require(gate.lua_required_compile_options(False) == gate.LUA_REQUIRED_COMPILE_OPTIONS,
            "gate OFF option sequence changed")
    print(f"allocator gate Lua ISA policy: PASS ({len(cases)} cases)")

    # The gate's second ISA-dependent pin: the dead l_alloc edges in
    # lauxlib.c.  Assemble the verbatim body in both states with the options
    # the gate pins and run readelf's real output through the gate's parser.
    clang, readelf = (Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
                      / "LLVM" / "bin" / name for name in ("clang.exe", "llvm-readelf.exe"))
    require(clang.is_file() and readelf.is_file(), f"installed LLVM missing: {clang}")
    lauxlib = (lua_src / "src" / "lauxlib.c").read_text(encoding="ascii").splitlines()
    body = lauxlib[999:1008]  # lauxlib.c:1000-1008, static void *l_alloc (...)
    require(body[0].startswith("static void *l_alloc (") and body[-1] == "}",
            f"lauxlib.c l_alloc moved: {body[0]!r} .. {body[-1]!r}")
    probe = work / "l_alloc_probe.c"
    probe.write_text("typedef unsigned int size_t;\n#define NULL ((void *)0)\n"
                     "void free(void *);\nvoid *realloc(void *, size_t);\n"
                     + "\n".join(body) + "\n"
                     "void *(*probe_owner(void))(void *, void *, size_t, size_t)"
                     " { return l_alloc; }\n", encoding="ascii")
    for core_arm in (False, True):
        isa = "-marm" if core_arm else "-mthumb"
        flags = [t for t in gate.lua_required_compile_options(core_arm)
                 if t.startswith(("-m", "-f", "-O"))]
        obj = work / f"l_alloc{isa}.o"
        done = subprocess.run([str(clang), "--target=armv7a-none-eabi", *flags, "-c",
                               str(probe), "-o", str(obj)], capture_output=True, text=True)
        require(done.returncode == 0, f"clang {isa}: {done.stderr[-400:]}")
        text = subprocess.run([str(readelf), "-Wr", str(obj)], capture_output=True,
                              text=True, check=True).stdout
        actual = gate.parse_raw_relocations(text)
        require(actual == gate.lua_expected_raw_relocations(core_arm),
                f"{isa} l_alloc edges {sorted(actual.items())}")
        gate.verify_lua_allocator_relocations("lauxlib.c", actual, core_arm)
        try:
            gate.verify_lua_allocator_relocations("lauxlib.c", actual, not core_arm)
            crossed = True
        except gate.GateError:
            crossed = False
        require(not crossed, f"{isa} l_alloc edges accepted under the other ISA")
        print(f"l_alloc {isa}: {sorted(actual.items())}")
    print("allocator gate Lua relocation policy: PASS (clang-assembled Thumb and ARM l_alloc)")

    spec = importlib.util.spec_from_file_location(
        "build_vita_lua_core_arm_test", HERE.parent / "tools" / "build_vita.py")
    require(spec is not None and spec.loader is not None, "cannot load build_vita.py")
    build_vita = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build_vita)
    for core_arm, rows in ((False, build_vita.LUA53_DEAD_RAW_RELOCATIONS),
                           (True, build_vita.LUA53_DEAD_RAW_RELOCATIONS_ARM)):
        require(list(rows) == gate._relocation_rows(gate.lua_expected_raw_relocations(core_arm)),
                f"build_vita receipt table disagrees with the gate (core_arm={core_arm})")
    print("build_vita receipt mirror: PASS (Thumb and ARM dead-edge tables equal the gate's)")
    print("PASS: test_vita_lua_core_arm")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
