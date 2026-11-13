#!/usr/bin/env python3
"""Proofs for ISAAC_VITA_LUA_VM_COMPUTED_GOTO (recomp/vita/lua53_vm_computed_goto.py).

1. derivation: the pinned pristine lvm.c and lopcodes.h shas equal the
   lua53_vanilla.cmake records; derive() changes exactly lvm.c lines
   765-768, 790 and 1317, keeps every other pristine line at its number and
   appends the three mode constants; the jump table names every opcode of the
   parsed enum at its own index and fills slots NUM_OPCODES..63; non-pristine
   inputs (a changed byte, CRLF, a tampered lopcodes.h) are refused, and so
   are the shapes the checks exist for (a missing case, a braceless
   `if (c) vmbreak;`, a plain `break;` in luaV_execute, an extra enumerator);
   the script's main() converges (a second run does not rewrite its output);
2. CMake wiring: the option defaults OFF and is refused without
   ISAAC_VITA_LUA; the archive swap keeps lvm.c's slot and precedes
   add_library(isaac_lua53); the derived lvm.c (and nothing else) gets
   -fno-crossjumping appended; the bridge definition is scoped to
   host_vita_lua.c and the log reads the derived file's constants;
3. host differential (gcc, WSL on Windows or native on Linux): pristine
   Lua 5.3.3 (lua.c interpreter, official src/Makefile flags minus readline)
   is built with the pristine and with the derived lvm.c, and twice more
   with per-opcode counters patched identically into both (fetch: after
   vmfetch reads the instruction; body: at the case label).  The test-only
   harness fixes lstate.c's string-hash seed (time and addresses otherwise)
   in all four builds, so table order cannot differ between them.  Corpus:
   (a) an own script generated here that dispatches every opcode (including
   OP_EXTRAARG and the case-less opcodes 47 and 63 through crafted binary
   chunks, TFORLOOP after a yield inside the iterator, LOADKX and SETLIST's
   EXTRAARG through a 262201-item constructor, yields inside every
   metamethod luaV_finishOp resumes), compared byte for byte (stdout,
   stderr, exit status), with counts per opcode identical between the
   builds, every opcode 0..46 fetched, fetch == body for 0..46, and slots 47
   and 63 fetched with no body; (b) the official Lua 5.3.3 test suite
   (lua.org lua-5.3.3-tests.tar.gz, sha256 checked before extraction, run
   in full mode with its C libraries built), which must end "final OK !!!"
   in both builds with identical output once clock, memory and address
   fields are masked; the per-opcode counts of both counted runs are
   printed and compared;
4. optional --arm-cc leg: pristine and derived lvm.c with the eboot's Lua
   archive flags (-mthumb, and -marm for ISAAC_VITA_LUA_CORE_ARM), the
   derived one also with the -fno-crossjumping CMake appends to it, no
   warning the pristine file does not have, and the indirect branches of
   luaV_execute counted from objdump (the dispatch the knob exists for):
   at least ARM_MIN_DISPATCH_SITES in the derived object as built, and the
   count without -fno-crossjumping printed beside it (gcc 10.3 merges the
   dispatch tails back into one there, which is why CMake adds the flag).

The gcc differential compiles the derived lvm.c with the same appended
option, so the interpreters it compares are the ones the knob builds.
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
import tarfile
import tempfile
import time

ROOT = Path(__file__).resolve().parent
RUNTIME = ROOT / "runtime"
VITA = ROOT / "vita"
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(VITA))

import lua53_vm_computed_goto as cg                  # noqa: E402
import vita_raw_allocator_gate as gate               # noqa: E402  (appended lvm.c option)
import test_vita_lua_runtime as lua_runtime          # noqa: E402  (Lua source pins)

# The official suite as published on https://www.lua.org/tests/ (the page
# lists this sha256 next to lua-5.3.3-tests.tar.gz).
SUITE_NAME = "lua-5.3.3-tests.tar.gz"
SUITE_SHA256 = "13154abc20976196119db531b4169ce1ce511755879d40b4192e4173291287e5"

# copied: recomp/test_vita_lua_integer2str.py:58-62 (the eboot's Lua archive
# flags, CMakeLists.txt isaac_lua53 target)
ARM_LUA_FLAGS = [
    "-DNDEBUG", "-std=gnu11", "-O2", "-mcpu=cortex-a9", "-mfpu=neon",
    "-mfloat-abi=softfp", "-mthumb", "-fno-strict-aliasing",
    "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra",
]
# What CMakeLists.txt appends to the derived lvm.c's compile, as the raw
# allocator gate pins it (vita_raw_allocator_gate.py
# LUA_VM_COMPUTED_GOTO_COMPILE_OPTIONS).
GOTO_EXTRA_FLAGS = list(gate.LUA_VM_COMPUTED_GOTO_COMPILE_OPTIONS)
# The derived luaV_execute has one dispatch per vmbreak/vmdispatch site; 47
# indirect branches were measured with arm-none-eabi-gcc 10.3.1 -mthumb and
# -marm.  The floor below is a judgement: it leaves room for a compiler that
# still shares a few tails, and is far above the 1 of the pristine switch.
ARM_MIN_DISPATCH_SITES = 40
# lua-5.3.3/src/Makefile:10 CFLAGS (-O2 -Wall -Wextra -DLUA_COMPAT_5_2) and
# :109-110 linux (LUA_USE_LINUX = LUA_USE_POSIX + LUA_USE_DLOPEN, -Wl,-E -ldl),
# without readline; plus the archive's -std=gnu11 -fno-strict-aliasing -DNDEBUG
HOST_LUA_FLAGS = ["-std=gnu11", "-O2", "-fno-strict-aliasing", "-DNDEBUG",
                  "-DLUA_COMPAT_5_2", "-DLUA_USE_POSIX", "-DLUA_USE_DLOPEN",
                  "-Wall", "-Wextra"]
# lua-5.3.3-tests/libs/makefile CFLAGS
SUITE_LIB_FLAGS = ["-Wall", "-std=gnu99", "-O2", "-fPIC", "-shared"]

IS_WINDOWS = os.name == "nt"


def require(condition: object, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def hpath(path: Path) -> str:
    """The path as the build shell sees it (WSL mount on Windows)."""
    resolved = path.resolve()
    if not IS_WINDOWS:
        return str(resolved)
    drive = resolved.drive.rstrip(":").lower()
    return f"/mnt/{drive}" + resolved.as_posix()[2:]


def shell(script: str) -> subprocess.CompletedProcess:
    argv = ["wsl", "-e", "sh", "-c", script] if IS_WINDOWS else ["sh", "-c", script]
    return subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def q(*parts: str) -> str:
    return " ".join(shlex.quote(p) for p in parts)


def must(script: str, what: str) -> str:
    completed = shell(script)
    text = completed.stdout.decode("utf-8", "replace")
    require(completed.returncode == 0, f"{what} failed ({completed.returncode}):\n{text}")
    return text


# --- 1. derivation -----------------------------------------------------------

def must_refuse(label: str, call) -> None:
    try:
        call()
    except cg.DeriveError:
        return
    raise AssertionError(f"the derivation accepted {label}")


def derivation_leg(lua_source: Path, work: Path) -> tuple[bytes, list[str]]:
    records = dict(lua_runtime.records())
    require(records["lvm.c"] == cg.PRISTINE_LVM_SHA256,
            "lua53_vm_computed_goto.py pins another lvm.c than lua53_vanilla.cmake")
    require(records["lopcodes.h"] == cg.PRISTINE_LOPCODES_SHA256,
            "lua53_vm_computed_goto.py pins another lopcodes.h than lua53_vanilla.cmake")
    lvm_path = lua_source / "src" / "lvm.c"
    opcodes_path = lua_source / "src" / "lopcodes.h"
    pristine = lvm_path.read_bytes()
    lopcodes = opcodes_path.read_bytes()
    require(hashlib.sha256(pristine).hexdigest() == cg.PRISTINE_LVM_SHA256,
            "the Lua source tree's lvm.c is not pristine")
    require(re.search(rb"^#define SIZE_OP\t\t6$", lopcodes, re.M) and cg.SIZE_OP == 6,
            "lopcodes.h:44 SIZE_OP is not the script's SIZE_OP")
    require(re.search(rb"^#define POS_OP\t\t0$", lopcodes, re.M),
            "lopcodes.h:46 POS_OP is not 0 (the corpus masks the low bits)")
    names = cg.opcode_names(lopcodes)
    require(len(names) == cg.EXPECTED_OPCODES == 47, "opcode count")
    derived = cg.derive(pristine, lopcodes)
    require(cg.derive(pristine, lopcodes) == derived, "derive() is not deterministic")
    old = pristine.split(b"\n")
    new = derived.split(b"\n")
    require(len(old) - 1 == cg.PRISTINE_LINES, "pristine lvm.c line count moved")
    changed = [n + 1 for n in range(len(old) - 1) if old[n] != new[n]]
    require(changed == [765, 766, 767, 768, 790, 1317], f"derived lines changed: {changed}")
    require(old[764] == b"#define vmdispatch(o)\tswitch(o)" and
            new[764].startswith(b"#define vmdispatch(o)\tgoto *isaac_lvm_disptab[o];"),
            "line 765 is not vmdispatch")
    require(old[765] == b"#define vmcase(l)\tcase l:" and
            new[765].startswith(b"#define vmcase(l)\tL_##l:"), "line 766 is not vmcase")
    require(old[766] == b"#define vmbreak\t\tbreak" and
            new[766].startswith(b"#define vmbreak\t\tvmfetch(); vmdispatch(GET_OPCODE(i))"),
            "line 767 is not vmbreak")
    require(old[767] == b"" and new[767].startswith(b"#define isaac_lvm_jumptable "),
            "line 768 is not the jump table macro")
    table = new[767].decode("ascii")
    entries = re.findall(r"\[(OP_[A-Z]+)\] = &&L_(OP_[A-Z]+)", table)
    require(entries == [(n, n) for n in names], "jump table entries are not the enum in order")
    slots = 1 << cg.SIZE_OP
    require(f"[NUM_OPCODES ... {slots - 1}] = &&{cg.FILLER}" in table, "filler range missing")
    require(table.count("_Static_assert(") == 3, "jump table static asserts missing")
    require(f"_Static_assert(NUM_OPCODES == {len(names)}," in table, "count assert missing")
    require(old[789] == b"  StkId base;" and new[789] == b"  StkId base;  isaac_lvm_jumptable",
            "line 790 does not expand the table inside luaV_execute")
    require(old[1316] == b"    }" and new[1316].endswith(b"    }") and
            new[1316].lstrip().startswith(b"L_isaac_op_none: vmbreak;"),
            "line 1317 does not carry the filler label before the switch brace")
    appended = b"\n".join(new[len(old) - 1:])
    for want in (b'const char isaac_lua_vm_dispatch[] = "computed-goto";',
                 b"const unsigned int isaac_lua_vm_dispatch_slots = 1u << SIZE_OP;",
                 b"const unsigned int isaac_lua_vm_dispatch_opcodes = NUM_OPCODES;"):
        require(appended.count(want) == 1, f"appended block lacks {want!r}")
    require(derived.count(b"vmcase(") == pristine.count(b"vmcase("), "case count moved")

    must_refuse("one changed byte", lambda: cg.derive(pristine[:-2] + b"x\n", lopcodes))
    must_refuse("CRLF", lambda: cg.derive(pristine.replace(b"\n", b"\r\n"), lopcodes))
    must_refuse("a tampered lopcodes.h",
                lambda: cg.derive(pristine, lopcodes.replace(b"OP_MOVE,", b"OP_MOVE ,", 1)))
    text = pristine
    must_refuse("a missing case", lambda: cg.check_cases(
        text.replace(b"vmcase(OP_MOVE)", b"vmcase(OP_XMOVE)", 1), names))
    must_refuse("a braceless if (c) vmbreak;", lambda: cg.check_cases(
        text.replace(b"        vmbreak;\n", b"        if (c)\n        vmbreak;\n", 1), names))
    must_refuse("a plain break in luaV_execute", lambda: cg.check_cases(
        text.replace(b"        vmbreak;\n", b"        break;\n        vmbreak;\n", 1), names))
    must_refuse("an extra enumerator", lambda: cg.opcode_names(
        lopcodes.replace(b"OP_EXTRAARG", b"OP_EXTRAARG,\nOP_EXTRA", 1)))
    cg.check_cases(pristine, names)

    output = work / "lua53-vm-computed-goto" / "lvm.c"
    script = VITA / "lua53_vm_computed_goto.py"
    for run in (1, 2):
        completed = subprocess.run(
            [sys.executable, str(script), "--input", str(lvm_path), "--opcodes",
             str(opcodes_path), "--output", str(output)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        require(completed.returncode == 0, f"lua53_vm_computed_goto.py failed:\n{completed.stdout}")
        if run == 1:
            first = output.stat().st_mtime_ns
            time.sleep(0.05)
    require(output.stat().st_mtime_ns == first, "the derivation script rewrote an up-to-date output")
    require(output.read_bytes() == derived, "the script's output differs from derive()")
    sha = hashlib.sha256(derived).hexdigest()
    require(f"sha256={sha}" in completed.stdout, "the script prints another sha256")
    print(f"derivation: lines 765-768,790,1317 changed, {len(new) - len(old)} lines appended, "
          f"table = {len(names)} opcodes in enum order + filler {len(names)}..{slots - 1}; "
          "non-pristine and unsafe shapes refused; script converges; "
          f"derived sha256={sha}")
    return derived, names


# --- 2. CMake wiring ------------------------------------------------------------

def cmake_leg() -> None:
    text = (VITA / "CMakeLists.txt").read_text(encoding="utf-8")
    require(re.search(r'option\(ISAAC_VITA_LUA_VM_COMPUTED_GOTO\n\s+"[^"]*(?:\\"[^"]*)*" OFF\)', text),
            "ISAAC_VITA_LUA_VM_COMPUTED_GOTO is not an OFF-by-default option")
    require('if(ISAAC_VITA_LUA_VM_COMPUTED_GOTO AND NOT ISAAC_VITA_LUA)\n'
            '  message(FATAL_ERROR "ISAAC_VITA_LUA_VM_COMPUTED_GOTO requires ISAAC_VITA_LUA=ON")'
            in text, "missing FATAL dependency on ISAAC_VITA_LUA")
    start = text.index("  if(ISAAC_VITA_LUA_VM_COMPUTED_GOTO)\n")
    block = text[start:text.index("\n  endif()\n", start)]
    for needle in ('"${ISAAC_LUA53_SOURCE_DIR}/src/lvm.c"',
                   '"${ISAAC_LUA53_SOURCE_DIR}/src/lopcodes.h"',
                   '"${CMAKE_BINARY_DIR}/lua53-vm-computed-goto/lvm.c"',
                   "list(REMOVE_AT ISAAC_LUA53_C_SOURCES ${ISAAC_VITA_LUA_VM_COMPUTED_GOTO_INDEX})",
                   "list(INSERT ISAAC_LUA53_C_SOURCES ${ISAAC_VITA_LUA_VM_COMPUTED_GOTO_INDEX}",
                   "add_custom_command(", "CMAKE_CONFIGURE_DEPENDS",
                   '"Isaac Lua: VM dispatch by computed goto "'):
        require(needle in block, f"archive swap lacks {needle}")
    appended = ('set_property(SOURCE "${ISAAC_VITA_LUA_VM_COMPUTED_GOTO_LVM}"\n'
                f'      APPEND PROPERTY COMPILE_OPTIONS {" ".join(GOTO_EXTRA_FLAGS)})')
    require(block.count(appended) == 1 and text.count("-fno-crossjumping)") == 1,
            "the derived lvm.c must get the gate's appended option, and only it")
    require(start < text.index("  add_library(isaac_lua53 STATIC ${ISAAC_LUA53_C_SOURCES})"),
            "the lvm.c swap must precede add_library(isaac_lua53)")
    define = ('set_property(SOURCE "${ISAAC_RUNTIME}/host_vita_lua.c"\n'
              '        APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_LUA_VM_COMPUTED_GOTO=1)')
    require(text.count(define) == 1 and text.count("ISAAC_VITA_LUA_VM_COMPUTED_GOTO=1") == 1,
            "the bridge definition is not scoped to host_vita_lua.c once")
    bridge = (RUNTIME / "host_vita_lua.c").read_text(encoding="utf-8")
    require(bridge.count("#if defined(ISAAC_VITA_LUA_VM_COMPUTED_GOTO)") == 1 and
            bridge.count('"[isaac-lua] vm dispatch=%s slots=%u opcodes=%u"') == 1 and
            bridge.count("extern const char isaac_lua_vm_dispatch[];") == 1,
            "the bridge must log the derived file's constants once, under the definition only")
    print("CMake wiring: option OFF, FATAL without ISAAC_VITA_LUA, lvm.c slot swap (with "
          f"{' '.join(GOTO_EXTRA_FLAGS)} on that source only) before "
          "add_library, bridge definition scoped to host_vita_lua.c, log reads the linked mode")


# --- 3. host differential --------------------------------------------------------

LSTATE_SEED = (b"  unsigned int h = luai_makeseed();\n",
               b"  unsigned int h = 0x2545F491u;  /* test harness: fixed seed */\n")
LSTATE_HASH = (b"  return luaS_hash(buff, p, h);\n",
               b"  (void)buff; (void)p;  /* test harness: no address in the seed */\n"
               b"  return h;\n")
FETCH = (b"  i = *(ci->u.l.savedpc++); \\\n",
         b"  i = *(ci->u.l.savedpc++); ISAAC_TEST_FETCH(i); \\\n")
CASE_PRISTINE = (b"#define vmcase(l)\tcase l:\n",
                 b"#define vmcase(l)\tcase l: ISAAC_TEST_BODY(l);\n")
CASE_DERIVED = (b"#define vmcase(l)\tL_##l:",
                b"#define vmcase(l)\tL_##l: ISAAC_TEST_BODY(l);")

COUNTERS_H = b"""/* test harness (recomp/test_vita_lua_vm_computed_goto.py) */
extern unsigned long long isaac_test_fetch[64];
extern unsigned long long isaac_test_body[64];
#define ISAAC_TEST_FETCH(i) (isaac_test_fetch[GET_OPCODE(i)]++)
#define ISAAC_TEST_BODY(l) (isaac_test_body[(l)]++)
"""
COUNTERS_C = b"""/* test harness (recomp/test_vita_lua_vm_computed_goto.py): one line per
   process, appended to $ISAAC_TEST_COUNTS at exit */
#include <stdio.h>
#include <stdlib.h>
unsigned long long isaac_test_fetch[64];
unsigned long long isaac_test_body[64];
__attribute__((destructor)) static void isaac_test_dump(void) {
  const char *path = getenv("ISAAC_TEST_COUNTS");
  FILE *f;
  int k;
  if (path == NULL || (f = fopen(path, "a")) == NULL)
    return;
  fputs("F", f);
  for (k = 0; k < 64; k++)
    fprintf(f, " %llu", isaac_test_fetch[k]);
  fputs(" B", f);
  for (k = 0; k < 64; k++)
    fprintf(f, " %llu", isaac_test_body[k]);
  fputc('\\n', f);
  fclose(f);
}
"""


def patch_once(data: bytes, pair: tuple[bytes, bytes], what: str) -> bytes:
    require(data.count(pair[0]) == 1, f"harness anchor not found once in {what}: {pair[0]!r}")
    return data.replace(pair[0], pair[1])


def build_luas(lua_source: Path, derived: bytes, work: Path) -> dict[str, Path]:
    src = lua_source / "src"
    harness = work / "harness"
    harness.mkdir()
    lstate = patch_once(patch_once((src / "lstate.c").read_bytes(), LSTATE_SEED, "lstate.c"),
                        LSTATE_HASH, "lstate.c")
    (harness / "lstate.c").write_bytes(lstate)
    (harness / "counters.h").write_bytes(COUNTERS_H)
    (harness / "counters.c").write_bytes(COUNTERS_C)
    pristine = (src / "lvm.c").read_bytes()
    variants = {
        "pristine": pristine,
        "goto": derived,
        "pristine-count": patch_once(patch_once(pristine, FETCH, "lvm.c"), CASE_PRISTINE, "lvm.c"),
        "goto-count": patch_once(patch_once(derived, FETCH, "derived lvm.c"), CASE_DERIVED,
                                 "derived lvm.c"),
    }
    objs = work / "obj"
    objs.mkdir()
    common = sorted(p for p in src.glob("*.c") if p.name not in ("lvm.c", "lstate.c", "luac.c"))
    require(len(common) == 32, f"expected 32 common Lua sources, found {len(common)}")
    cc = ["gcc", *HOST_LUA_FLAGS, f"-I{hpath(src)}"]
    steps = [q(*cc, "-c", hpath(p), "-o", hpath(objs / (p.stem + ".o"))) for p in common]
    steps.append(q(*cc, "-c", hpath(harness / "lstate.c"), "-o", hpath(objs / "lstate.o")))
    steps.append(q(*cc, "-c", hpath(harness / "counters.c"), "-o", hpath(objs / "counters.o")))
    common_objs = [hpath(objs / (p.stem + ".o")) for p in common] + [hpath(objs / "lstate.o")]
    binaries = {}
    for name, data in variants.items():
        lvm = harness / f"lvm-{name}.c"
        lvm.write_bytes(data)
        extra = ["-include", hpath(harness / "counters.h")] if name.endswith("-count") else []
        if name.startswith("goto"):
            extra += GOTO_EXTRA_FLAGS
        obj = objs / f"lvm-{name}.o"
        log = objs / f"lvm-{name}.log"
        steps.append(q(*cc, *extra, "-c", hpath(lvm), "-o", hpath(obj)) + " 2> " + q(hpath(log)))
        binary = work / "bin" / f"lua-{name}"
        link = [*common_objs, hpath(obj)]
        if name.endswith("-count"):
            link.append(hpath(objs / "counters.o"))
        steps.append(q("gcc", "-o", hpath(binary), *link, "-lm", "-ldl", "-Wl,-E"))
        binaries[name] = binary
    (work / "bin").mkdir()
    must(" && ".join(steps), "gcc build of the four Lua interpreters")
    # the derivation adds no warning: same diagnostics as pristine, path-normalised
    def diagnostics(name: str) -> list[str]:
        text = (objs / f"lvm-{name}.log").read_text(encoding="utf-8", errors="replace")
        return [re.sub(r"^\S*lvm-[a-z-]+\.c:\d+:\d+: ", "", line) for line in text.splitlines()
                if ": warning: " in line or ": error: " in line]
    require(diagnostics("goto") == diagnostics("pristine"),
            f"derived lvm.c diagnostics differ from pristine:\n{diagnostics('goto')}")
    print(f"gcc build: 4 interpreters (pristine, goto, pristine-count, goto-count; derived "
          f"lvm.c with {' '.join(GOTO_EXTRA_FLAGS)}); lvm.c warnings pristine={len(diagnostics('pristine'))} derived={len(diagnostics('goto'))}")
    return binaries


CORPUS = r'''-- Own VM corpus for the ISAAC_VITA_LUA_VM_COMPUTED_GOTO differential,
-- generated by recomp/test_vita_lua_vm_computed_goto.py (not game code).
local OP = @OPCODES@
local NUM_OPCODES = @NUM_OPCODES@
local SLOTS = @SLOTS@
local format, concat = string.format, table.concat

local function show(v)
  local t = type(v)
  if t == "number" then
    if math.type(v) == "integer" then return "i" .. format("%d", v) end
    return "f" .. format("%.17g", v)
  elseif t == "string" then return format("%q", v)
  elseif t == "nil" or t == "boolean" then return tostring(v)
  end
  return t   -- never an address: heap layout may differ between the builds
end
local function shows(...)
  local t = table.pack(...)
  for k = 1, t.n do t[k] = show(t[k]) end
  return concat(t, " ")
end

-- A. arithmetic, bitwise, concat and comparisons over register operands
local vals = {0, 1, -1, 7, -7, 3, 2^53, math.maxinteger, math.mininteger, 0.5, -2.25,
              1e300, -0.0, 1/0, -1/0, 0/0, "10", "0x10", "3.5", " 7 ", "x", true}
local binops = {
  function(a, b) return a + b end, function(a, b) return a - b end,
  function(a, b) return a * b end, function(a, b) return a % b end,
  function(a, b) return a ^ b end, function(a, b) return a / b end,
  function(a, b) return a // b end, function(a, b) return a & b end,
  function(a, b) return a | b end, function(a, b) return a ~ b end,
  function(a, b) return a << b end, function(a, b) return a >> b end,
  function(a, b) return a .. b end, function(a, b) return a == b end,
  function(a, b) return a < b end, function(a, b) return a <= b end,
  function(a, b) return a ~= b end, function(a, b) return a > b end,
  function(a, b) return a >= b end,
}
for i = 1, #vals do
  local row = {}
  for j = 1, #vals do
    local cell = {}
    for k = 1, #binops do
      local ok, r = pcall(binops[k], vals[i], vals[j])
      cell[k] = ok and show(r) or ("E:" .. tostring(r))
    end
    row[#row + 1] = concat(cell, ",")
  end
  print("arith", i, concat(row, ";"))
end
-- constant operands (RK) and unary operators
local unops = {
  function(a) return -a end, function(a) return ~a end, function(a) return not a end,
  function(a) return #a end, function(a) return a + 1 end, function(a) return 2 * a end,
  function(a) return a // 2 end, function(a) return 1.5 % a end,
  function(a) return a == 7 end, function(a) return 3 < a end,
  function(a) return a <= "m" end, function(a) return a .. 1 end,
  function(a) return a >> 1 end, function(a) return 0xF0 & a end,
}
for i = 1, #vals do
  local cell = {}
  for k = 1, #unops do
    local ok, r = pcall(unops[k], vals[i])
    cell[k] = ok and show(r) or ("E:" .. tostring(r))
  end
  print("unary", i, concat(cell, ","))
end

-- B. metamethods
local mt = {}
local function B(v) return setmetatable({v = v}, mt) end
local function unbox(x) if type(x) == "table" then return x.v end return x end
mt.__add = function(a, b) return B(unbox(a) + unbox(b)) end
mt.__sub = function(a, b) return B(unbox(a) - unbox(b)) end
mt.__mul = function(a, b) return B(unbox(a) * unbox(b)) end
mt.__mod = function(a, b) return B(unbox(a) % unbox(b)) end
mt.__pow = function(a, b) return B(unbox(a) ^ unbox(b)) end
mt.__div = function(a, b) return B(unbox(a) / unbox(b)) end
mt.__idiv = function(a, b) return B(unbox(a) // unbox(b)) end
mt.__band = function(a, b) return B(unbox(a) & unbox(b)) end
mt.__bor = function(a, b) return B(unbox(a) | unbox(b)) end
mt.__bxor = function(a, b) return B(unbox(a) ~ unbox(b)) end
mt.__shl = function(a, b) return B(unbox(a) << unbox(b)) end
mt.__shr = function(a, b) return B(unbox(a) >> unbox(b)) end
mt.__unm = function(a) return B(-a.v) end
mt.__bnot = function(a) return B(~a.v) end
mt.__len = function(a) return a.v * 10 end
mt.__eq = function(a, b) return unbox(a) == unbox(b) end
mt.__lt = function(a, b) return unbox(a) < unbox(b) end
mt.__le = function(a, b) return unbox(a) <= unbox(b) end
mt.__concat = function(a, b) return tostring(unbox(a)) .. "&" .. tostring(unbox(b)) end
mt.__call = function(self, x, y) return self.v + x, y end
mt.__index = function(t, k) return "idx:" .. tostring(k) end
mt.__newindex = function(t, k, v) rawset(t, k, v * 2) end
do
  local a, b = B(6), B(4)
  print("meta", shows(unbox(a + b), unbox(a - 1), unbox(2 * a), unbox(a % b), unbox(a ^ 2),
        unbox(a / b), unbox(a // b), unbox(a & 3), unbox(a | b), unbox(a ~ b), unbox(a << 2),
        unbox(a >> 1), unbox(-a), unbox(~a), #a, a == B(6), a == b, a ~= b, a < b, a <= b,
        a > b, a >= b, "x" .. a .. "y" .. 1 .. b, a(5, "z"), a.missing, a[1]))
  a.newfield = 21
  print("meta2", rawget(a, "newfield"))
  local chain = setmetatable({}, {__index = setmetatable({}, {__index = {deep = 42}})})
  local sink = {}
  local nt = setmetatable({}, {__newindex = sink})
  nt.k = "v"
  print("meta3", chain.deep, rawget(nt, "k"), sink.k, rawequal(a, B(6)))
end

-- C. tables, constructors, SETLIST, LOADKX
local function three() return 1, 2, 3 end
local t1 = {three()}
local t2 = {three(), three()}
local t3 = {n = 1, [2] = "x", "a", "b", three()}
local function va(...) return {...}, select("#", ...), ... end
local tv, nv, v1, v2 = va(nil, 2, nil)
print("tables", #t1, #t2, t3.n, t3[1], t3[2], t3[3], t3[5], nv, tv[2], v1, v2)
local mixed = {}
mixed[1] = "i"; mixed[1.0] = "f"; mixed[2^53] = "big"; mixed[-0.0] = "z"; mixed["1"] = "s"
print("keys", mixed[1], mixed[0], mixed[2^53], mixed["1"], #mixed)
do
  -- more than MAXARG_Bx constants (LOADKX) and more than MAXARG_C flushes of
  -- LFIELDS_PER_FLUSH items (SETLIST with an EXTRAARG count) in one function
  local N = 262200
  local parts = {"return {"}
  for i = 0, N do parts[#parts + 1] = i .. "," end
  parts[#parts + 1] = "}"
  local big = assert(load(concat(parts), "=big"))()
  local s = 0
  for i = 1, #big do s = s + big[i] end
  print("big", #big, big[1], big[#big], s)
end

-- D. control flow
local acc = {}
for i = 1, 5 do acc[#acc + 1] = i end
for i = 10, 1, -3 do acc[#acc + 1] = i end
for x = 0.5, 2, 0.25 do acc[#acc + 1] = x end
for x = 1, 0 do acc[#acc + 1] = "never" end
for i = 1, 3.7 do acc[#acc + 1] = i end
for i = -3, -1.5 do acc[#acc + 1] = i end
for i = math.maxinteger - 1, math.maxinteger - 1 do acc[#acc + 1] = i end
for i = "2", "4" do acc[#acc + 1] = i end
print("for", shows(table.unpack(acc)))
local n, w = 0, 0
while true do
  n = n + 1
  if n > 10 then break end
  if n % 2 == 0 then goto cont end
  w = w + n
  ::cont::
end
repeat n = n - 3 until n < 0
local fns = {}
for i = 1, 3 do local j = i * 2; fns[#fns + 1] = function() j = j + 1; return i + j end end
do local k = 0; while k < 3 do k = k + 1; local c = k; fns[#fns + 1] = function() return c end end end
do
  local i = 1
  ::top::
  local x = i
  fns[#fns + 1] = function() return x * 100 end
  i = i + 1
  if i <= 3 then goto top end
end
local fr = {}
for k = 1, #fns do fr[k] = fns[k]() end
for k = 1, 3 do fr[#fr + 1] = fns[k]() end
print("loops", n, w, concat(fr, " "))
local function tsel(a, b) local x = a or b; local y = a and b; local z = not a; if a then z = "t" end; return x, y, z end
local function cmp(a, b) local r = a < b; return r, a == b, not (a <= b) end
print("tests", shows(tsel(nil, 2)), shows(tsel(false, nil)), shows(tsel(1, 2)), shows(cmp(1, 2)), shows(cmp(2.5, 2)))
local la, lb, lc
local ld, le = 1
print("nil", shows(la, lb, lc, ld, le))

-- E. calls, varargs, tail calls, methods, C functions
local function fib(k) if k < 2 then return k end return fib(k - 1) + fib(k - 2) end
local function tail(k, a) if k == 0 then return a end return tail(k - 1, a + 1) end
local function cnt(...) return select("#", ...) end
local function pass(...) return ... end
local function va2(a, ...) local x, y = ...; return a, x, y, cnt(...), cnt(pass(...)) end
print("calls", fib(20), tail(100000, 0), cnt(), cnt(nil), cnt(nil, nil), shows(va2(1)), shows(va2(1, 2, 3, 4)), (pass(7, 8)))
local obj = {n = 0}
function obj:inc(k) self.n = self.n + (k or 1) return self end
obj:inc():inc(5):inc()
local str = "Hello, VM"
print("self", obj.n, str:upper(), str:len(), ("x"):rep(3, "-"), str:sub(-2), #str, select(-1, 1, 2, 3))
print("unpack", shows(table.unpack({1, nil, 3}, 1, 3)), shows(string.byte("abc", 1, -1)))

-- F. errors raised by VM instructions and by C functions
local errs = {}
local function E(f, ...) local ok, m = pcall(f, ...); errs[#errs + 1] = ok and ("ok " .. show(m)) or tostring(m) end
E(function() local x; x() end)
E(function() local t = {}; t.a.b = 1 end)
E(function() return undefinedglobal.x end)
E(function() return {} < {} end)
E(function() return 1 < "2" end)
E(function() return #5 end)
E(function() return -{} end)
E(function() return 1 .. {} end)
E(function() local a, b = 1, 0; return a // b end)
E(function() local a, b = 1, 0; return a % b end)
E(function() local a = 1.5; return a & 1 end)
E(function() local a = "1.5"; return a | 0 end)
E(function() local a = math.huge; return a | 0 end)
E(function() error("msg", 0) end)
E(function() error("lvl1") end)
E(function() local function inner() error("lvl2", 2) end inner() end)
E(function() error() end)
E(function() error(setmetatable({}, {__tostring = function() return "tostr" end})) end)
E(function() local t = setmetatable({}, {__index = function(_, k) error("idx " .. k) end}); return t.zz end)
E(function() for i = "a", 2 do end end)
E(function() for i = 1, {} do end end)
E(function() return ("x"):bad() end)
E(function() local a = {}; a = a + 1 end)
E(function() return 1 < nil end)
E(function() local s = "abc"; return s.x.y end)
E(function() string.rep() end)
E(function() return tostring(setmetatable({}, {__tostring = function() return 1 end})) end)
E(function() return math.tointeger(2^63) end)
E(function() local a = 2^63; return a // 1 | 0 end)
E(function() local t = setmetatable({}, {__newindex = "notatable"}); t.x = 1 end)
E(function() return 2^53 | 0, 3.0 | 0, "0x10" + 0, "1e2" * 1 end)
for k = 1, #errs do print("err", k, errs[k]) end
local function deep(k) if k == 0 then error("bottom") end return (deep(k - 1)) end
print("tb", select(2, xpcall(deep, debug.traceback, 3)))
print("tb2", select(2, xpcall(function() local x = nil; return x.y end, function(m) return "handled: " .. m end)))

-- G. coroutines: plain yields, yield inside a generic-for iterator (resumed
-- at the TFORLOOP, dispatched rather than reached by goto), yields inside
-- every metamethod luaV_finishOp resumes, yield across pcall
local gen = coroutine.wrap(function(a, b)
  local c = coroutine.yield(a + b)
  local d, e = coroutine.yield(c * 2)
  return d + e
end)
print("co", gen(1, 2), gen(10), gen(3, 4))
local co2 = coroutine.wrap(function()
  local function it(_, c) if c < 4 then coroutine.yield("it" .. c); return c + 1 end end
  local s = 0
  for v in it, nil, 0 do s = s + v end
  return "done" .. s
end)
local seq = {}
for _ = 1, 6 do local r = co2(); seq[#seq + 1] = r; if r:sub(1, 4) == "done" then break end end
print("cofor", concat(seq, " "))
local ymt = {
  __add = function(a, b) coroutine.yield("add") return a.v + b end,
  __lt = function(a, b) coroutine.yield("lt") return a.v < b.v end,
  __le = function(a, b) coroutine.yield("le") return a.v <= b.v end,
  __eq = function(a, b) coroutine.yield("eq") return a.v == b.v end,
  __index = function(t, k) coroutine.yield("index") return k .. "!" end,
  __newindex = function(t, k, v) coroutine.yield("newindex") rawset(t, k, v) end,
  __concat = function(a, b) coroutine.yield("concat") return "cc" end,
  __call = function(self, x) coroutine.yield("call") return x * 3 end,
  __len = function(a) coroutine.yield("len") return 99 end,
  __unm = function(a) coroutine.yield("unm") return -a.v end,
  __shl = function(a, b) coroutine.yield("shl") return 1 end,
}
local co3 = coroutine.create(function()
  local a = setmetatable({v = 2}, ymt)
  local b = setmetatable({v = 3}, ymt)
  local r = {}
  r[#r + 1] = a + 5
  r[#r + 1] = a < b
  r[#r + 1] = a <= b
  r[#r + 1] = a == b
  r[#r + 1] = not (b < a)
  r[#r + 1] = a.foo
  a.bar = 7
  r[#r + 1] = rawget(a, "bar")
  r[#r + 1] = "x" .. a .. "y"
  r[#r + 1] = a(4)
  r[#r + 1] = #a
  r[#r + 1] = -a
  r[#r + 1] = a << 1
  if a < b then r[#r + 1] = "jmp" end
  if not (a == b) then r[#r + 1] = "ne" end
  return r
end)
local events = {}
while true do
  local ok, v = coroutine.resume(co3)
  assert(ok, v)
  if coroutine.status(co3) == "dead" then
    for k = 1, #v do v[k] = show(v[k]) end
    events[#events + 1] = concat(v, " ")
    break
  end
  events[#events + 1] = v
end
print("coyield", concat(events, ","))
local co4 = coroutine.wrap(function()
  local ok, v = pcall(function() local x = coroutine.yield(1); error("e" .. x) end)
  return ok, v
end)
print("copcall", co4(), co4(5))
local co5 = coroutine.create(function() local x = nil; coroutine.yield(); return x.boom end)
print("coerr", coroutine.resume(co5))
print("coerr2", coroutine.resume(co5))
print("coerr3", coroutine.status(co5), coroutine.resume(co5))
print("costat", coroutine.isyieldable(), select(2, coroutine.running()))

-- H. upvalues and _ENV
local function counter() local c = 0; return function() c = c + 1; return c end, function() return c end end
local inc, get = counter()
inc(); inc()
print("upv", get(), inc(), get())
g_global = 5
g_global = g_global + 1
do local _ENV = {print = print, x = 3}; y = x * 2; print("env", x, y) end
print("env2", g_global, rawget(_G, "y"))

-- I. hooks
local lines, calls, rets, counts = {}, 0, 0, 0
local function hooked(k) local s = 0 for i = 1, k do s = s + i end return s end
debug.sethook(function(ev, line)
  if ev == "line" then lines[#lines + 1] = line
  elseif ev == "call" or ev == "tail call" then calls = calls + 1
  elseif ev == "return" then rets = rets + 1 end
end, "crl")
local hr = hooked(5)
debug.sethook()
debug.sethook(function() counts = counts + 1 end, "", 7)
local hr2 = hooked(100)
debug.sethook()
print("hooks", hr, hr2, concat(lines, " "), calls, rets, counts)

-- J. garbage collection, finalizers, weak tables
local order = {}
for i = 1, 5 do setmetatable({}, {__gc = function() order[#order + 1] = i end}) end
collectgarbage(); collectgarbage()
local weak = setmetatable({}, {__mode = "k"})
local keep = {}
for i = 1, 10 do local k = {}; weak[k] = i; if i % 3 == 0 then keep[#keep + 1] = k end end
collectgarbage()
local wc = 0
for _ in pairs(weak) do wc = wc + 1 end
print("gc", concat(order, " "), wc, #keep)

-- K. string keys (order follows the harness's fixed hash seed), iterators
local st = {}
for _, word in ipairs{"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"} do st[word] = #word end
local ks = {}
for k, v in pairs(st) do ks[#ks + 1] = k .. "=" .. v end
for word in ("a,b,,c"):gmatch("([^,]*)") do ks[#ks + 1] = "[" .. word .. "]" end
for i, v in ipairs({10, 20, nil, 40}) do ks[#ks + 1] = i .. ":" .. v end
local arr = {1, 2, 3, x = 1}
for k, v in next, arr do ks[#ks + 1] = tostring(k) .. "/" .. v end
print("pairs", concat(ks, " "))

-- L. crafted binary chunks: dispatch OP_EXTRAARG standalone and two opcode
-- values without a case (NUM_OPCODES and SLOTS - 1): each is a no-op in the
-- pristine switch (no default), so `local a = 10; a = 20; return a` with the
-- second LOADK replaced returns 10
local SIZE_INT = string.packsize("i")
local function patched(op)
  local s = string.dump(function() local a = 10; a = 20; return a end, true)
  -- ldump.c luaU_dump:210-211 (DumpHeader, sizeupvalues byte), DumpHeader:184-196,
  -- DumpFunction:166-176 (source = one 0 byte when stripped, linedefined and
  -- lastlinedefined ints, numparams/is_vararg/maxstacksize bytes, DumpCode)
  local hdr = 4 + 1 + 1 + 6 + 5 + string.packsize("j") + string.packsize("n")
  assert(s:byte(4 + 1 + 1 + 6 + 3) == 4, "sizeof(Instruction)")
  local at = hdr + 1 + 1 + 2 * SIZE_INT + 3 + 1
  local count, code = string.unpack("i", s, at)
  assert(count == 4, "sizecode " .. count)
  local ins = {}
  for k = 1, count do ins[k] = string.unpack("I4", s, code + 4 * (k - 1)) end
  local mask = SLOTS - 1
  assert(ins[1] & mask == OP.OP_LOADK and ins[2] & mask == OP.OP_LOADK and
         ins[3] & mask == OP.OP_RETURN and ins[4] & mask == OP.OP_RETURN, "code shape")
  local chunk = s:sub(1, code + 3) .. string.pack("I4", (ins[2] & ~mask) | op) .. s:sub(code + 8)
  return assert(load(chunk, "=crafted", "b"))
end
print("crafted", patched(OP.OP_LOADK)(), patched(OP.OP_EXTRAARG)(), patched(NUM_OPCODES)(), patched(SLOTS - 1)())
local dumped = assert(load(string.dump(fib)))
print("dump", pcall(dumped, 10))

print("corpus OK")
'''


def corpus_text(names: list[str]) -> bytes:
    table = "{" + ", ".join(f"{n} = {k}" for k, n in enumerate(names)) + "}"
    text = (CORPUS.replace("@OPCODES@", table)
            .replace("@NUM_OPCODES@", str(len(names)))
            .replace("@SLOTS@", str(1 << cg.SIZE_OP)))
    require("@" not in text, "corpus placeholders left")
    return text.encode("ascii")


def run_lua(binary: Path, cwd: Path, script: str, out: Path, counts: Path | None) -> int:
    env = f"ISAAC_TEST_COUNTS={q(hpath(counts))} " if counts is not None else ""
    completed = shell(f"cd {q(hpath(cwd))} && {env}{q(hpath(binary))} {q(script)} "
                      f"> {q(hpath(out))}.out 2> {q(hpath(out))}.err; echo rc=$?")
    text = completed.stdout.decode("ascii", "replace").strip()
    require(completed.returncode == 0 and text.startswith("rc="), f"run of {binary.name}: {text}")
    return int(text[3:])


def read_counts(path: Path) -> tuple[list[int], list[int], int]:
    fetch, body = [0] * 64, [0] * 64
    lines = path.read_text(encoding="ascii").splitlines()
    for line in lines:
        parts = line.split()
        require(len(parts) == 130 and parts[0] == "F" and parts[65] == "B", f"bad count line {line[:40]}")
        for k in range(64):
            fetch[k] += int(parts[1 + k])
            body[k] += int(parts[66 + k])
    return fetch, body, len(lines)


def corpus_leg(binaries: dict[str, Path], names: list[str], work: Path) -> None:
    run = work / "corpus"
    run.mkdir()
    (run / "corpus.lua").write_bytes(corpus_text(names))
    results = {}
    for name, binary in binaries.items():
        counts = run / f"{name}.counts" if name.endswith("-count") else None
        rc = run_lua(binary, run, "corpus.lua", run / name, counts)
        results[name] = (rc, (run / f"{name}.out").read_bytes(), (run / f"{name}.err").read_bytes())
    rc, out, err = results["pristine"]
    require(rc == 0 and out.endswith(b"corpus OK\n") and err == b"",
            f"pristine corpus run failed rc={rc}:\n{err.decode('utf-8', 'replace')[:2000]}")
    for name in ("goto", "pristine-count", "goto-count"):
        require(results[name] == results["pristine"],
                f"corpus output of lua-{name} differs from pristine (rc/stdout/stderr)")
    fetch_p, body_p, procs_p = read_counts(run / "pristine-count.counts")
    fetch_g, body_g, procs_g = read_counts(run / "goto-count.counts")
    require(procs_p == procs_g == 1, f"count processes {procs_p}/{procs_g}")
    require(fetch_p == fetch_g and body_p == body_g, "per-opcode counts differ between the builds:\n"
            + "\n".join(f"  {k}: fetch {fetch_p[k]}/{fetch_g[k]} body {body_p[k]}/{body_g[k]}"
                        for k in range(64) if (fetch_p[k], body_p[k]) != (fetch_g[k], body_g[k])))
    nop = len(names)
    slots = 1 << cg.SIZE_OP
    missing = [names[k] for k in range(nop) if fetch_p[k] == 0]
    require(not missing, f"opcodes never dispatched by the corpus: {missing}")
    unequal = [names[k] for k in range(nop) if fetch_p[k] != body_p[k]]
    require(not unequal, f"fetch != body for {unequal}")
    require(fetch_p[nop] > 0 and fetch_p[slots - 1] > 0 and
            all(body_p[k] == 0 for k in range(nop, slots)) and
            all(fetch_p[k] == 0 for k in range(nop + 1, slots - 1)),
            "case-less slots: expected fetches at NUM_OPCODES and 63 only, no body")
    lines = out.count(b"\n")
    total = sum(fetch_p)
    low = sorted(range(nop), key=lambda k: fetch_p[k])[:5]
    print(f"own corpus: pristine == goto == counted builds byte for byte "
          f"({len(out)} B stdout, {lines} lines, stderr empty, rc 0); "
          f"{total} dispatches, all {nop} opcodes fetched, fetch == body for each, "
          f"case-less slots {nop} x{fetch_p[nop]} and {slots - 1} x{fetch_p[slots - 1]} fetched "
          "with no body; rarest: " + ", ".join(f"{names[k]} {fetch_p[k]}" for k in low))


def remove_tree(path: Path) -> None:
    # the suite's tar members are read-only; Windows refuses to unlink those
    def retry(function, name, _info):
        os.chmod(name, 0o700)
        function(name)
    shutil.rmtree(path, onerror=retry)


def extract_suite(tarball: Path, dest: Path) -> Path:
    data = tarball.read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    require(sha == SUITE_SHA256, f"{tarball.name}: sha256 {sha} is not the published {SUITE_SHA256}")
    dest.mkdir()
    with tarfile.open(tarball, "r:gz") as archive:
        members = archive.getmembers()
        for member in members:
            require(member.isfile() or member.isdir(), f"unexpected tar member type: {member.name}")
            require(not member.name.startswith(("/", "..")) and ".." not in Path(member.name).parts,
                    f"unsafe tar member: {member.name}")
        # members were checked above (regular files and directories, no absolute
        # or parent paths); the data filter is used where this Python has it
        extra = {"filter": "data"} if hasattr(tarfile, "data_filter") else {}
        archive.extractall(dest, **extra)
    root = dest / "lua-5.3.3-tests"
    require((root / "all.lua").is_file(), "suite has no all.lua")
    return root


MASKS = [
    (re.compile(rb"0x[0-9a-fA-F]+"), b"0xADDR"),
    (re.compile(rb"^(time: )\S+ \(\+\S+\)$", re.M), rb"\1T (+T)"),
    (re.compile(rb"(---- total memory: )\S+(, max memory: )\S+( ----)"), rb"\1M\2M\3"),
    (re.compile(rb"^(total time: )\S+( \(wall time: )\S+(\))$", re.M), rb"\1T\2T\3"),
    (re.compile(rb"^(WARNING: time difference from previous test: )\S+$", re.M), rb"\1T"),
    # files.lua:787 prints the date and time of the run
    (re.compile(rb"^(test done on )\S+(, at )\S+$", re.M), rb"\1D\2T"),
    # all.lua:138-144 writes one '.' to stderr per collection cycle; the cycle
    # count moves between two runs of the same binary (measured: 1036 and 1038
    # bytes of stderr), so it is masked and the counts are printed
    (re.compile(rb"\.{2,}"), b"DOTS"),
    # main.lua prints its os.tmpname() file (/tmp/lua_XXXXXX under LUA_USE_POSIX)
    (re.compile(rb"/tmp/lua_[A-Za-z0-9]{6}"), b"/tmp/lua_TMP"),
    # sort.lua:255/279 print timings; main.lua:345 prints a background pid
    (re.compile(rb" in [0-9.]+ msec\."), b" in T msec."),
    (re.compile(rb"(running in background, pid )\d+"), rb"\1PID"),
    # constructs.lua:236 `_ENV.GLOB1 = math.floor(os.time()) % 2` picks one of
    # two cases by the clock, and :247 prints which
    (re.compile(rb"^(testing short-circuit optimizations \()[01](\))$", re.M), rb"\1G\2"),
]


def masked(data: bytes, binary: str) -> tuple[bytes, int]:
    # main.lua:27 prints the interpreter's own path (each build has its own)
    hits = data.count(binary.encode())
    data = data.replace(binary.encode(), b"LUA")
    for pattern, replacement in MASKS:
        data, n = pattern.subn(replacement, data)
        hits += n
    return data, hits


def suite_leg(binaries: dict[str, Path], lua_source: Path, tarball: Path, work: Path) -> None:
    pristine_suite = extract_suite(tarball, work / "suite-src")
    libs = work / "suite-libs"
    libs.mkdir()
    # lua-5.3.3-tests/libs/makefile: lib1 lib11 lib2 lib21, then lib2.so is
    # moved to lib2-v2.so; built here from the same sources and flags
    steps = []
    for stem, out in (("lib1", "lib1.so"), ("lib11", "lib11.so"), ("lib21", "lib21.so"),
                      ("lib2", "lib2-v2.so")):
        steps.append(q("gcc", *SUITE_LIB_FLAGS, f"-I{hpath(lua_source / 'src')}", "-o",
                       hpath(libs / out), hpath(pristine_suite / "libs" / f"{stem}.c")))
    must(" && ".join(steps), "suite C libraries")
    results = {}
    # pristine-count runs twice: the second run is the noise floor for the
    # per-opcode counts (the suite has clock- and address-dependent paths)
    runs = [*binaries.items(), ("pristine-count-2", binaries["pristine-count"])]
    for name, binary in runs:
        cwd = work / f"suite-{name}"
        shutil.copytree(pristine_suite, cwd)
        for lib in libs.iterdir():
            shutil.copy2(lib, cwd / "libs" / lib.name)
        counts = work / f"suite-{name}.counts" if "-count" in name else None
        started = time.monotonic()
        rc = run_lua(binary, cwd, "all.lua", work / f"suite-{name}", counts)
        seconds = time.monotonic() - started
        out = (work / f"suite-{name}.out").read_bytes()
        err = (work / f"suite-{name}.err").read_bytes()
        results[name] = (rc, out, err, seconds, hpath(binary))
    for name, (rc, out, err, seconds, _) in results.items():
        require(rc == 0 and b"final OK !!!" in out,
                f"official suite under lua-{name}: rc={rc}, no 'final OK !!!':\n"
                f"{out[-1500:].decode('utf-8', 'replace')}\n{err[-1500:].decode('utf-8', 'replace')}")
    base_out, hits = masked(results["pristine"][1], results["pristine"][4])
    base_err, err_hits = masked(results["pristine"][2], results["pristine"][4])
    for name in ("goto", "pristine-count", "goto-count", "pristine-count-2"):
        out, _ = masked(results[name][1], results[name][4])
        err, _ = masked(results[name][2], results[name][4])
        if out != base_out or err != base_err:
            a, b = base_out.split(b"\n"), out.split(b"\n")
            diff = [f"  {k + 1}: {x!r} | {y!r}" for k, (x, y) in enumerate(zip(a, b)) if x != y][:8]
            raise AssertionError(f"suite output of lua-{name} differs from pristine after masking "
                                 f"(lines {len(a)}/{len(b)}):\n" + "\n".join(diff))
    counted = {name: read_counts(work / f"suite-{name}.counts")
               for name in ("pristine-count", "goto-count", "pristine-count-2")}
    fetch_p, body_p, procs_p = counted["pristine-count"]
    covered = sum(1 for k in range(64) if fetch_p[k])
    missing = [k for k in range(47) if fetch_p[k] == 0]
    print(f"official suite ({SUITE_NAME}, full mode, C libs built): 'final OK !!!' in all five "
          f"runs; stdout {len(base_out)} B / stderr {len(base_err)} B identical after masking "
          f"{hits + err_hits} path/clock/pid/memory/address/progress fields (stderr '.' per GC cycle: "
          + "/".join(str(results[n][2].count(b".")) for n in results) + "); wall s pristine "
          f"{results['pristine'][3]:.1f} goto {results['goto'][3]:.1f}")

    def delta(other: str) -> str:
        fetch_o, body_o, procs_o = counted[other]
        ops = [k for k in range(64) if (fetch_p[k], body_p[k]) != (fetch_o[k], body_o[k])]
        moved = sum(abs(fetch_p[k] - fetch_o[k]) for k in range(64))
        return (f"{other}: {procs_o} processes, {sum(fetch_o)} dispatches, {len(ops)} opcodes "
                f"differ, sum |d fetch| {moved} ({moved / max(1, sum(fetch_p)):.2e})")
    # Information, not a gate: these counts move between two runs of the SAME binary (the suite has
    # os.clock loops, constructs.lua's GLOB1 time-parity branch, GC cycle counts; a re-run
    # of 267f3c5 saw 1.28e-04 / 9.37e-04 where an earlier run saw 4.55e-05 / 7.73e-05).  The exact
    # dispatch gate is the own corpus above (fetch == body, byte-identical output).
    print(f"  counts (information, run-dependent): pristine-count {procs_p} processes, {sum(fetch_p)} dispatches, {covered} "
          f"slots fetched, never fetched among 0..46: {missing or 'none'}; vs "
          f"{delta('pristine-count-2')}; vs {delta('goto-count')}")


# --- 4. ARM cross-compile ----------------------------------------------------------

def arm_leg(arm_cc: str, lua_source: Path, derived: bytes, work: Path) -> None:
    arm = work / "arm"
    arm.mkdir()
    (arm / "lvm-derived.c").write_bytes(derived)
    cc = shlex.split(arm_cc)
    require(cc[-1].endswith("gcc"), f"--arm-cc must name a gcc driver: {arm_cc}")
    objdump = cc[-1][:-3] + "objdump"
    src = lua_source / "src"
    plan = []
    for isa in ("-mthumb", "-marm"):
        flags = [isa if f == "-mthumb" else f for f in ARM_LUA_FLAGS]
        for label, source, more in (
                ("pristine", src / "lvm.c", []),
                ("derived", arm / "lvm-derived.c", GOTO_EXTRA_FLAGS),
                ("derived-crossjumping", arm / "lvm-derived.c", [])):
            obj = arm / f"lvm-{label}{isa}.o"
            log = arm / f"lvm-{label}{isa}.log"
            plan.append((isa, label, obj, log))
            must(q(*cc, *flags, *more, f"-I{hpath(src)}", "-c", hpath(source), "-o", hpath(obj))
                 + " 2> " + q(hpath(log)), f"ARM compile {label} {isa}")

    def warnings(log: Path) -> list[str]:
        return [re.sub(r"^\S*lvm(-derived)?\.c:\d+:\d+: ", "", line)
                for line in log.read_text(encoding="utf-8", errors="replace").splitlines()
                if ": warning: " in line]

    report = []
    counts = {}
    for isa, label, obj, log in plan:
        text = must(q(objdump, "-d", "--section=.text.luaV_execute", hpath(obj)),
                    f"objdump {obj.name}")
        insns = [line.split("\t") for line in text.splitlines() if re.match(r"^\s+[0-9a-f]+:\t", line)]
        mnem = [(parts[2].strip() if len(parts) > 2 else "", parts[3].strip() if len(parts) > 3 else "")
                for parts in insns]
        # ARM-mode dispatch is conditional: pristine -marm switches with
        # `ldrls pc, [pc, r3, lsl #2]` (measured), so strip a condition code
        def base(m: str) -> str:
            m = m.split(".")[0]
            hit = re.fullmatch(r"(ldr|mov|add|bx)(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)", m)
            return hit.group(1) if hit else m
        table = sum(1 for m, _ in mnem if base(m) in ("tbb", "tbh"))
        ldrpc = sum(1 for m, ops in mnem if base(m) == "ldr" and ops.startswith("pc,"))
        movpc = sum(1 for m, ops in mnem if base(m) in ("mov", "add") and ops.startswith("pc,"))
        bxreg = sum(1 for m, ops in mnem if base(m) == "bx" and ops != "lr")
        blxreg = sum(1 for m, ops in mnem if m == "blx" and re.fullmatch(r"r\d+|ip|lr", ops))
        dispatch = table + ldrpc + movpc + bxreg
        counts[(isa, label)] = dispatch
        report.append(f"{label}{isa}: {len(insns)} insns, indirect jumps {dispatch} "
                      f"(tbb/tbh {table}, ldr pc {ldrpc}, mov/add pc {movpc}, bx reg {bxreg}), "
                      f"blx reg {blxreg}")
    for isa in ("-mthumb", "-marm"):
        logs = {label: log for i, label, _, log in plan if i == isa}
        require(warnings(logs["derived"]) == warnings(logs["pristine"]),
                f"ARM {isa}: derived lvm.c warns where pristine does not:\n{warnings(logs['derived'])}")
        require(counts[(isa, "derived")] >= ARM_MIN_DISPATCH_SITES,
                f"ARM {isa}: luaV_execute has {counts[(isa, 'derived')]} indirect jumps "
                f"(pristine {counts[(isa, 'pristine')]}, floor {ARM_MIN_DISPATCH_SITES}): "
                "the computed gotos were merged back")
    print(f"ARM leg ({arm_cc}, Lua archive flags; derived = as CMake builds it, with "
          f"{' '.join(GOTO_EXTRA_FLAGS)}; derived-crossjumping = without it, informational): "
          "no new warnings; luaV_execute " + "; ".join(report))


# --- main ----------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lua-source", type=Path, help="pristine lua-5.3.3 (or ISAAC_LUA53_SOURCE_DIR)")
    parser.add_argument("--tests-tarball", type=Path,
                        help=f"{SUITE_NAME} from https://www.lua.org/tests/ (sha256 checked)")
    parser.add_argument("--arm-cc", help="ARM gcc driver for the objdump leg "
                        "(arm-vita-eabi-gcc on a Linux build host, arm-none-eabi-gcc in WSL)")
    parser.add_argument("--skip-gcc", action="store_true")
    parser.add_argument("--keep", type=Path, help="work directory to keep (default: temp)")
    args = parser.parse_args()

    lua_source = lua_runtime.discover_source(args.lua_source)
    lua_runtime.validate_source(lua_source, lua_runtime.records())
    tmp = None
    if args.keep:
        work = args.keep.resolve()
        if work.exists():
            remove_tree(work)
        work.mkdir(parents=True)
    else:
        tmp = tempfile.TemporaryDirectory(prefix="isaac-lua-vmgoto-")
        work = Path(tmp.name)
    try:
        derived, names = derivation_leg(lua_source, work)
        cmake_leg()
        if args.skip_gcc:
            print("gcc differential: skipped (--skip-gcc)")
        else:
            require(shutil.which("wsl" if IS_WINDOWS else "gcc"), "no gcc (or wsl) for the differential")
            binaries = build_luas(lua_source, derived, work)
            corpus_leg(binaries, names, work)
            if args.tests_tarball:
                suite_leg(binaries, lua_source, args.tests_tarball.resolve(), work)
            else:
                print("official suite: skipped (no --tests-tarball)")
        if args.arm_cc:
            arm_leg(args.arm_cc, lua_source, derived, work)
        else:
            print("ARM leg: skipped (no --arm-cc)")
    finally:
        if tmp is not None:
            tmp.cleanup()
    print("Vita Lua VM computed goto (derived lvm.c == pristine switch): PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
