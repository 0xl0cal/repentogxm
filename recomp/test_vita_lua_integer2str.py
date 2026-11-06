#!/usr/bin/env python3
"""Proofs for ISAAC_VITA_LUA_INTEGER2STR (recomp/vita/lua53_integer2str.py).

1. derivation: the pinned pristine lobject.c sha equals the
   lua53_vanilla.cmake record; derive() changes exactly lines 366 and 376,
   keeps every other pristine line at its number and appends one definition;
   a non-pristine input is refused; the script's main() converges (a second
   run does not rewrite its output);
2. CMake wiring: the option defaults OFF and is refused without
   ISAAC_VITA_LUA; the archive swap keeps lobject.c's slot; the self-test
   unit and its definition are scoped to host_vita_lua.c and the unit, and
   only when ISAAC_VITA_LUA_COST_PROFILE is ON too;
3. host oracle (recomp/runtime/vita_lua_integer2str_oracle.c), generated
   here from the pristine tree on every run: pristine Lua 5.3.3 is linked
   once with the pristine and once with the derived lobject.c.  The derived
   build compares isaac_lua_integer2str with snprintf(LUA_INTEGER_FMT) on
   ~20 million values with coverage per sign x digit count and per 64-bit
   loop round count, and runs the boot self-test unit with a fake clock; both
   builds print a transcript digest of what Lua itself formats (lua_tolstring,
   lua_pushfstring %d/%I, a chunk with tostring/../table.concat/error
   positions/floats), which must be identical.  Legs: MSVC x86 (32-bit
   size_t and 64-bit division helpers like the console) and WSL gcc x86-64
   (glibc's snprintf);
4. optional --arm-cc leg (ARM build host): the derived lobject.c (-mthumb and -marm,
   the latter for ISAAC_VITA_LUA_CORE_ARM) and the self-test unit with the
   eboot's Lua flags and -Werror; readelf -s finds isaac_lua_integer2str;
   LUA_INTEGER_FMT "%lld" (one use in lobject.c, line 376) is gone from the
   derived object and present in the pristine one built the same way, and
   luaO_tostring has no more snprintf relocations than pristine (the float
   path keeps lua_number2str = snprintf, luaconf.h:424/591).
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent
RUNTIME = ROOT / "runtime"
VITA = ROOT / "vita"
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(VITA))

import lua53_integer2str as i2s                      # noqa: E402
import test_vita_lua_runtime as lua_runtime          # noqa: E402  (vcvars, Lua source pins)

ORACLE = RUNTIME / "vita_lua_integer2str_oracle.c"
SELFTEST = RUNTIME / "host_vita_lua_integer2str_selftest.c"
ARM_LUA_FLAGS = [
    "-DNDEBUG", "-std=gnu11", "-O2", "-mcpu=cortex-a9", "-mfpu=neon",
    "-mfloat-abi=softfp", "-mthumb", "-fno-strict-aliasing",
    "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra",
]


def require(condition: object, message: str) -> None:
    if not condition:
        raise AssertionError(message)


# --- 1. derivation -----------------------------------------------------------

def derivation_leg(lua_source: Path, work: Path) -> bytes:
    records = dict(lua_runtime.records())
    require(records["lobject.c"] == i2s.PRISTINE_LOBJECT_SHA256,
            "lua53_integer2str.py pins another lobject.c than lua53_vanilla.cmake")
    pristine_path = lua_source / "src" / "lobject.c"
    pristine = pristine_path.read_bytes()
    require(hashlib.sha256(pristine).hexdigest() == i2s.PRISTINE_LOBJECT_SHA256,
            "the Lua source tree's lobject.c is not pristine")
    derived = i2s.derive(pristine)
    require(i2s.derive(pristine) == derived, "derive() is not deterministic")
    old = pristine.split(b"\n")
    new = derived.split(b"\n")
    require(len(old) - 1 == i2s.PRISTINE_LINES, "pristine lobject.c line count moved")
    changed = [index + 1 for index in range(len(old) - 1) if old[index] != new[index]]
    require(changed == [366, 376], f"derived lines changed: {changed}")
    require(old[365] == b"" and b"isaac_lua_integer2str (char *buff" in new[365],
            "line 366 is not the prototype")
    require(old[375].strip() == b"len = lua_integer2str(buff, sizeof(buff), ivalue(obj));"
            and new[375].strip().startswith(
                b"len = isaac_lua_integer2str(buff, sizeof(buff), ivalue(obj));"),
            "line 376 is not the call")
    appended = b"\n".join(new[len(old) - 1:])
    require(appended.count(b"int isaac_lua_integer2str (char *buff, size_t sz, lua_Integer n) {") == 1,
            "the appended block does not define isaac_lua_integer2str once")
    require(derived.count(b"lua_integer2str(") == derived.count(b"isaac_lua_integer2str(") == 1,
            "a bare lua_integer2str( call survived, or the derived call is missing")
    for label, data in (("one changed byte", pristine[:-2] + b"x\n"),
                        ("CRLF", pristine.replace(b"\n", b"\r\n"))):
        try:
            i2s.derive(data)
        except i2s.DeriveError:
            pass
        else:
            raise AssertionError(f"derive() accepted a non-pristine lobject.c ({label})")
    # main() converges: a second run must not rewrite the output.
    output = work / "lua53-integer2str" / "lobject.c"
    script = VITA / "lua53_integer2str.py"
    for run in (1, 2):
        completed = subprocess.run(
            [sys.executable, str(script), "--input", str(pristine_path),
             "--output", str(output)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        require(completed.returncode == 0, f"lua53_integer2str.py failed:\n{completed.stdout}")
        if run == 1:
            first = output.stat().st_mtime_ns
            time.sleep(0.05)
    require(output.stat().st_mtime_ns == first, "the derivation script rewrote an up-to-date output")
    require(output.read_bytes() == derived, "the script's output differs from derive()")
    sha = hashlib.sha256(derived).hexdigest()
    require(f"sha256={sha}" in completed.stdout, "the script prints another sha256")
    print(f"derivation: lines 366,376 changed, {len(new) - len(old)} lines appended, "
          f"non-pristine refused, script converges; derived sha256={sha}")
    return derived


# --- 2. CMake wiring ------------------------------------------------------------

def cmake_leg() -> None:
    text = (VITA / "CMakeLists.txt").read_text(encoding="utf-8")
    require(re.search(r'option\(ISAAC_VITA_LUA_INTEGER2STR\n\s+"[^"]*(?:\\"[^"]*)*" OFF\)', text),
            "ISAAC_VITA_LUA_INTEGER2STR is not an OFF-by-default option")
    require('if(ISAAC_VITA_LUA_INTEGER2STR AND NOT ISAAC_VITA_LUA)\n'
            '  message(FATAL_ERROR "ISAAC_VITA_LUA_INTEGER2STR requires ISAAC_VITA_LUA=ON")'
            in text, "missing FATAL dependency on ISAAC_VITA_LUA")
    block = text[text.index("  if(ISAAC_VITA_LUA_INTEGER2STR)\n"):]
    block = block[:block.index("\n  endif()\n")]
    for needle in ('"${ISAAC_LUA53_SOURCE_DIR}/src/lobject.c"',
                   '"${CMAKE_BINARY_DIR}/lua53-integer2str/lobject.c"',
                   "list(REMOVE_AT ISAAC_LUA53_C_SOURCES ${ISAAC_VITA_LUA_INTEGER2STR_INDEX})",
                   "list(INSERT ISAAC_LUA53_C_SOURCES ${ISAAC_VITA_LUA_INTEGER2STR_INDEX}",
                   "add_custom_command(", "CMAKE_CONFIGURE_DEPENDS"):
        require(needle in block, f"archive swap lacks {needle}")
    require(text.index("  if(ISAAC_VITA_LUA_INTEGER2STR)\n") <
            text.index("  add_library(isaac_lua53 STATIC ${ISAAC_LUA53_C_SOURCES})"),
            "the lobject.c swap must precede add_library(isaac_lua53)")
    selftest = text[text.index("    if(ISAAC_VITA_LUA_INTEGER2STR AND ISAAC_VITA_LUA_COST_PROFILE)\n"):]
    selftest = selftest[:selftest.index("\n    endif()\n")]
    require(selftest.count("ISAAC_VITA_LUA_INTEGER2STR_SELFTEST=1") == 1 and
            '"${ISAAC_RUNTIME}/host_vita_lua.c"\n        "${ISAAC_RUNTIME}/host_vita_lua_integer2str_selftest.c"\n'
            '        APPEND PROPERTY COMPILE_DEFINITIONS\n          ISAAC_VITA_LUA_INTEGER2STR_SELFTEST=1'
            in selftest, "self-test definition is not scoped to the bridge and the unit")
    require(text.count("ISAAC_VITA_LUA_INTEGER2STR_SELFTEST") == 1,
            "ISAAC_VITA_LUA_INTEGER2STR_SELFTEST applied outside its block")
    bridge = (RUNTIME / "host_vita_lua.c").read_text(encoding="utf-8")
    require(bridge.count("#if defined(ISAAC_VITA_LUA_INTEGER2STR_SELFTEST)") == 1 and
            bridge.count("isaac_vita_lua_integer2str_selftest();") == 1,
            "the bridge must call the self-test once, under its definition only")
    print("CMake wiring: option OFF, FATAL without ISAAC_VITA_LUA, lobject.c slot swap "
          "before add_library, self-test scoped to bridge + unit under COST_PROFILE")


# --- 3. host oracle --------------------------------------------------------------

def parse_oracle(output: str, derived: bool, tag: str) -> str:
    print(f"host oracle [{tag}]:")
    print("  " + output.strip().replace("\n", "\n  "))
    want = f"integer2str oracle ({'derived' if derived else 'pristine'} lobject.c): PASS"
    require(want in output, f"{tag}: oracle did not pass")
    transcript = re.search(r"^transcript: .*$", output, re.M)
    require(transcript, f"{tag}: no transcript line")
    if derived:
        direct = re.search(r"^direct: checked=(\d+) mismatches=(\d+) ", output, re.M)
        require(direct and int(direct.group(2)) == 0 and int(direct.group(1)) > 20_000_000,
                f"{tag}: direct check failed or too small")
        for sign in ("pos", "neg"):
            line = re.search(rf"^coverage digits {sign}:(.*)$", output, re.M)
            require(line, f"{tag}: no {sign} digit coverage")
            counts = dict(re.findall(r" (\d+)=(\d+)", line.group(1)))
            require(sorted(map(int, counts)) == list(range(1, 20)) and
                    all(int(value) > 0 for value in counts.values()),
                    f"{tag}: {sign} digit-count coverage has a hole: {counts}")
        rounds = re.search(r"^coverage loop64 rounds: 0=(\d+) 1=(\d+) 2=(\d+) llong_min=(\d+)$",
                           output, re.M)
        require(rounds and all(int(value) > 0 for value in rounds.groups()),
                f"{tag}: 64-bit loop round coverage has a hole")
        selftest = re.search(r"^selftest lines=1: \[isaac-lua\] i2s selftest build=oracle "
                             r"values=2048 mismatches=0 first_bad=0x0000000000000000 "
                             r"small n=1024 snprintf_ns=\d+ itoa_ns=\d+ wide n=1024 "
                             r"snprintf_ns=\d+ itoa_ns=\d+ passes=3$", output, re.M)
        require(selftest, f"{tag}: self-test line missing or malformed")
    return transcript.group(0)


def msvc_leg(lua_source: Path, lua_sources: list[Path], derived_path: Path, work: Path) -> None:
    vcvars = lua_runtime.find_vcvars()
    quote = lua_runtime.quote
    build = work / "msvc"
    build.mkdir()
    common = ["cl", "/nologo", "/std:c11", "/O2", "/D_CRT_SECURE_NO_WARNINGS",
              f"/I{quote(lua_source / 'src')}"]
    core = [p for p in lua_sources if p.name != "lobject.c"]
    steps = [
        " ".join([*common, "/W3", "/c", *[quote(p) for p in core]]),
        " ".join([*common, "/W3", "/c", quote(lua_source / "src" / "lobject.c"),
                  f"/Fo{quote(build / 'pristine_lobject.obj')}"]),
        # the derived file at the pristine sources' level (its own code is
        # held to -Werror by the gcc leg)
        " ".join([*common, "/W3", "/c", quote(derived_path),
                  f"/Fo{quote(build / 'derived_lobject.obj')}"]),
        " ".join([*common, "/W4", "/WX", "/DISAAC_VITA_LUA_INTEGER2STR_SELFTEST=1",
                  "/DISAAC_VITA_LUA_INTEGER2STR_ORACLE=1",
                  '/DISAAC_VITA_LUA_INTEGER2STR_BUILD_ID=\\"oracle\\"', "/c", quote(SELFTEST),
                  f"/Fo{quote(build / 'selftest.obj')}"]),
    ]
    exes = {}
    for derived in (0, 1):
        obj = build / f"oracle_{derived}.obj"
        steps.append(" ".join([*common, "/W4", "/WX", f"/DI2S_DERIVED={derived}", "/c",
                               quote(ORACLE), f"/Fo{quote(obj)}"]))
        exe = build / f"i2s-oracle-{'derived' if derived else 'pristine'}.exe"
        objects = [f"{p.stem}.obj" for p in core] + [obj.name]
        objects += ["derived_lobject.obj", "selftest.obj"] if derived else ["pristine_lobject.obj"]
        steps.append(" ".join(["link", "/nologo", f"/out:{quote(exe)}", *objects]))
        exes[derived] = exe
    vc = f"call {quote(vcvars)} x86 >nul && "
    for step in steps:
        completed = subprocess.run(vc + step, cwd=build, text=True, shell=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        require(completed.returncode == 0, f"MSVC step failed:\n{step}\n{completed.stdout}")
        require("warning" not in completed.stdout.lower() or step.startswith("link")
                or " /W3 " in step, f"MSVC warned:\n{step}\n{completed.stdout}")
    transcripts = {}
    for derived, exe in exes.items():
        completed = subprocess.run([str(exe)], cwd=build, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        require(completed.returncode == 0, f"MSVC oracle exited {completed.returncode}:\n{completed.stdout}")
        transcripts[derived] = parse_oracle(completed.stdout, bool(derived), f"msvc-x86 {'derived' if derived else 'pristine'}")
    require(transcripts[0] == transcripts[1],
            f"MSVC transcripts differ:\n  pristine {transcripts[0]}\n  derived  {transcripts[1]}")
    print(f"MSVC x86 leg: pristine == derived transcript ({transcripts[1]})")


def wsl_path(path: Path) -> str:
    resolved = path.resolve()
    drive = resolved.drive.rstrip(":").lower()
    return f"/mnt/{drive}" + resolved.as_posix()[2:]


def gcc_leg(lua_source: Path, lua_sources: list[Path], derived_path: Path, work: Path) -> None:
    if shutil.which("wsl") is None:
        print("gcc leg: skipped (no wsl)")
        return
    build = work / "gcc"
    build.mkdir()
    out = wsl_path(build)
    src = wsl_path(lua_source / "src")
    core = [wsl_path(p) for p in lua_sources if p.name != "lobject.c"]
    base = ["gcc", "-std=gnu11", "-O2", f"-I{src}", "-DLUA_USE_POSIX"]
    # -Werror on the three files this change owns; the pristine Lua sources
    # keep their upstream warning level
    strict = [*base, "-Wall", "-Wextra", "-Werror", "-c"]
    selftest_defs = ["-DISAAC_VITA_LUA_INTEGER2STR_SELFTEST=1",
                     "-DISAAC_VITA_LUA_INTEGER2STR_ORACLE=1",
                     '-DISAAC_VITA_LUA_INTEGER2STR_BUILD_ID="oracle"']
    plan = [
        [*strict, wsl_path(derived_path), "-o", f"{out}/derived_lobject.o"],
        [*strict, *selftest_defs, wsl_path(SELFTEST), "-o", f"{out}/selftest.o"],
        [*strict, "-DI2S_DERIVED=1", wsl_path(ORACLE), "-o", f"{out}/oracle_1.o"],
        [*strict, "-DI2S_DERIVED=0", wsl_path(ORACLE), "-o", f"{out}/oracle_0.o"],
        [*base, "-o", f"{out}/i2s-oracle-1", *core, f"{out}/derived_lobject.o",
         f"{out}/selftest.o", f"{out}/oracle_1.o", "-lm"],
        [*base, "-o", f"{out}/i2s-oracle-0", *core, f"{src}/lobject.c",
         f"{out}/oracle_0.o", "-lm"],
    ]
    script = " && ".join(" ".join(shlex.quote(part) for part in step) for step in plan)
    completed = subprocess.run(["wsl", "-e", "sh", "-c", script + " 2>&1"], text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    require(completed.returncode == 0, f"gcc build failed:\n{completed.stdout}")
    transcripts = {}
    for derived in (0, 1):
        completed = subprocess.run(["wsl", "-e", f"{out}/i2s-oracle-{derived}"], text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        require(completed.returncode == 0,
                f"gcc oracle exited {completed.returncode}:\n{completed.stdout}")
        transcripts[derived] = parse_oracle(
            completed.stdout, bool(derived), f"gcc-x86_64 {'derived' if derived else 'pristine'}")
    require(transcripts[0] == transcripts[1],
            f"gcc transcripts differ:\n  pristine {transcripts[0]}\n  derived  {transcripts[1]}")
    print(f"gcc x86-64 leg: pristine == derived transcript ({transcripts[1]}); "
          "derived lobject.c, oracle and self-test -Wall -Wextra -Werror clean")


# --- 4. ARM cross-compile (optional, ARM build host) ------------------------------

def arm_leg(arm_cc: str, lua_source: Path, derived_path: Path, work: Path) -> None:
    cc = shlex.split(arm_cc)
    require(shutil.which(cc[0]) is not None, f"ARM compiler not found: {cc[0]}")
    tool = Path(shutil.which(cc[0]))
    prefix = tool.name[:tool.name.rindex("gcc")]
    readelf = str(tool.with_name(prefix + "readelf"))
    objdump = str(tool.with_name(prefix + "objdump"))
    pristine_path = lua_source / "src" / "lobject.c"
    selftest = work / "arm_selftest.c.obj"
    # -mthumb is the default archive ISA, -marm is ISAAC_VITA_LUA_CORE_ARM=ON (recipes A and B)
    plan = [(derived_path, work / "arm_lobject.c.obj", "-mthumb", []),
            (derived_path, work / "arm_lobject_marm.c.obj", "-marm", []),
            (pristine_path, work / "arm_lobject_pristine.c.obj", "-mthumb", []),
            (pristine_path, work / "arm_lobject_pristine_marm.c.obj", "-marm", []),
            (SELFTEST, selftest, "-mthumb", ["-DISAAC_VITA_LUA_INTEGER2STR_SELFTEST=1",
                                             '-DISAAC_VITA_LUA_INTEGER2STR_BUILD_ID="test"'])]
    for source, obj, isa, extra in plan:
        flags = [isa if f == "-mthumb" else f for f in ARM_LUA_FLAGS]
        completed = subprocess.run([*cc, *flags, "-Werror", f"-I{lua_source / 'src'}",
                                    *extra, "-c", str(source), "-o", str(obj)],
                                   text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        # pristine Lua is not held to -Werror (the archive builds it with -Wall -Wextra only)
        require(completed.returncode == 0 and (source == pristine_path or "warning" not in completed.stdout),
                f"ARM compile of {source.name} ({isa}) failed or warned:\n{completed.stdout}")

    def snprintf_relocs(obj: Path) -> int:
        # luaO_tostring formats floats with lua_number2str = snprintf("%.14g")
        # (luaconf.h:424/591) in both files; only the integer call goes away
        relocs = subprocess.run([objdump, "-dr", "-j", ".text.luaO_tostring", str(obj)], text=True,
                                stdout=subprocess.PIPE, check=True).stdout
        return sum(1 for line in relocs.splitlines() if re.search(r"\sR_\w+\s+\S*snprintf", line))

    counts = []
    for derived_obj, pristine_obj in ((plan[0][1], plan[2][1]), (plan[1][1], plan[3][1])):
        symbols = subprocess.run([readelf, "-sW", str(derived_obj)], text=True,
                                 stdout=subprocess.PIPE, check=True).stdout
        require(re.search(r"FUNC\s+GLOBAL\s+HIDDEN\s+\S+\s+isaac_lua_integer2str$", symbols, re.M),
                f"readelf: isaac_lua_integer2str is not a GLOBAL HIDDEN FUNC in {derived_obj.name}")
        require(re.search(r"FUNC\s+GLOBAL\s+HIDDEN\s+\S+\s+luaO_tostring$", symbols, re.M),
                f"readelf: luaO_tostring missing in {derived_obj.name}")
        # LUA_INTEGER_FMT "%lld" has one use in lobject.c, line 376 (the call
        # the derivation replaces), so its bytes leave the object; the relocation
        # count is only a cross-check (-fcrossjumping may merge pristine's two
        # same-shaped snprintf calls under softfp)
        require(b"%lld\0" in pristine_obj.read_bytes(), f"{pristine_obj.name}: no \"%lld\" (probe broken)")
        require(b"%lld\0" not in derived_obj.read_bytes(),
                f"{derived_obj.name}: still carries LUA_INTEGER_FMT \"%lld\"")
        derived_n, pristine_n = snprintf_relocs(derived_obj), snprintf_relocs(pristine_obj)
        require(1 <= pristine_n and derived_n <= pristine_n,
                f"{derived_obj.name}: luaO_tostring snprintf relocations derived={derived_n} pristine={pristine_n}")
        counts.append(f"{derived_n}/{pristine_n}")
    selftest_symbols = subprocess.run([readelf, "-sW", str(selftest)], text=True,
                                      stdout=subprocess.PIPE, check=True).stdout
    for want in (r"GLOBAL\s+DEFAULT\s+\S+\s+isaac_vita_lua_integer2str_selftest$",
                 r"GLOBAL\s+DEFAULT\s+UND\s+isaac_lua_integer2str$",
                 r"GLOBAL\s+DEFAULT\s+UND\s+sceKernelGetProcessTimeWide$"):
        require(re.search(want, selftest_symbols, re.M), f"readelf self-test contract: {want}")
    print("ARM leg (Lua archive flags, -Werror, -mthumb and -marm): isaac_lua_integer2str GLOBAL HIDDEN; "
          "\"%lld\" gone from the derived object (present in pristine); luaO_tostring snprintf "
          f"relocations derived/pristine thumb {counts[0]}, arm {counts[1]} (the float call stays); "
          "self-test unit links the archive function and the Vita clock")


# --- main ----------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lua-source", type=Path, help="pristine lua-5.3.3 (or ISAAC_LUA53_SOURCE_DIR)")
    parser.add_argument("--arm-cc", help="arm-vita-eabi-gcc (ARM cross-compile leg)")
    parser.add_argument("--skip-msvc", action="store_true")
    parser.add_argument("--skip-gcc", action="store_true")
    parser.add_argument("--keep", type=Path, help="work directory to keep (default: temp)")
    args = parser.parse_args()

    lua_source = lua_runtime.discover_source(args.lua_source)
    lua_sources = lua_runtime.validate_source(lua_source, lua_runtime.records())
    lua_sources = [p for p in lua_sources if p.name not in ("lua.c", "luac.c")]
    tmp = None
    if args.keep:
        work = args.keep.resolve()
        if work.exists():
            shutil.rmtree(work)
        work.mkdir(parents=True)
    else:
        tmp = tempfile.TemporaryDirectory(prefix="isaac-lua-i2s-")
        work = Path(tmp.name)
    try:
        derived = derivation_leg(lua_source, work)
        derived_path = work / "lua53-integer2str" / "lobject.c"
        require(derived_path.read_bytes() == derived, "derived file drifted")
        cmake_leg()
        if args.skip_msvc:
            print("MSVC x86 leg: skipped (--skip-msvc)")
        else:
            msvc_leg(lua_source, lua_sources, derived_path, work)
        if args.skip_gcc:
            print("gcc leg: skipped (--skip-gcc)")
        else:
            gcc_leg(lua_source, lua_sources, derived_path, work)
        if args.arm_cc:
            arm_leg(args.arm_cc, lua_source, derived_path, work)
        else:
            print("ARM leg: skipped (no --arm-cc)")
    finally:
        if tmp is not None:
            tmp.cleanup()
    print("Vita Lua integer2str (lobject.c digit loop == snprintf %lld): PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
