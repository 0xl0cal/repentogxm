#!/usr/bin/env python3
"""ISAAC_VITA_LUA_GC_FINALIZER_ALLOC_LATCH_BYTES: the finalizer-allocation latch.

Review round 2 of the finalizer pacer (an internal review of the r5
behaviour) left three blockers, all of one shape: a `__gc` that allocates.
B3 is the one this test is about -- finalizers that allocate more than
~200-300 KB per loop make the paced collector mark 1.35-3.0x pristine's work
and stretch loops past 16 ms.  The guard (lua53_gc_finalizer_pace.py
--alloc-latch A, derived lgc.c lines 61, 821 and 958) measures what each
cycle's finalizers allocate and latches the state to pristine Lua 5.3.3 once
a cycle passes A bytes, through the same latch a script's collectgarbage()
takes.

What is proven here, on the host, with the pace oracle
(runtime/vita_lua_gc_finalizer_pace_oracle.c) at the engine's schedule
(LIVE_KB heap, one lua_gc(LUA_GCSTEP, K) per loop):

1. derivation: A = 0 is byte-identical to the derivation without the guard
   (the pinned sha of test_vita_lua_gc_finalizer_pace.py); A > 0 changes
   lines 61, 821 and 958 only and keeps the line count; out-of-range A is
   refused; the CLI prints alloc_latch=A only when A > 0.  CMake (cmake -P
   probes of the knob's own blocks): budget 0 derives the pinned bytes, a
   budget the guard's; a budget with PACE OFF and an out-of-range budget
   are refused.
2. latch legs (LATCH_LEGS, finalizers that allocate): the guard build latches
   in every run, exactly once, within LATCH_FRAMES loops, and its JUDGED
   metrics, GC ms/s, cycles, loop p99 and worst loop are pristine's within
   BAND (median of RUNS against pristine's median, or inside pristine's own
   spread).
3. teeth: every TEETH leg names ONE metric and its direction, the thing
   the guard is there to prevent on that shape (loop p99 up, peak heap up,
   or the run passing the oracle's abort line).  The build without the
   guard (A = 0, the bytes of a323dc9) must be WORSE than pristine on that
   metric -- median above every pristine run and more than TOOTH_UP over
   pristine's median, or a majority of runs aborted -- and so must each
   mutant of the guard: mutant-guard-no-latch (counts, never latches) and
   mutant-guard-floor (a budget no leg reaches).  A difference in any other
   metric, or in the better direction, kills nothing; item 2's full list is
   printed after the verdict as information.  On b3-fs1048576-r1 the
   unguarded pacer is BETTER than pristine on loop p99 (2.7 vs 10.7 ms) and
   GC ms/s (-15 %); what it loses there is peak heap (+5-12 %), and that is
   the tooth.
4. inert legs (INERT_LEGS, finalizers that allocate nothing): the guard
   build never latches and counts no latch, and on the EXACT builds its
   digest equals the no-guard build's on every seed -- the collector's
   whole trajectory is bit-identical.  mutant-guard-no-reset (the per-cycle
   sum is never cleared) must latch on SMALL_LEG, where the guard build must
   not.
5. clamp legs (CLAMP_LEGS, K = 64 KB, the R60 recipe's
   ISAAC_VITA_LUA_GCSTEP_CLAMP_KB): the guard build AND the build without
   the guard (budget 0), each against pristine at the same K -- finishes,
   finalizer semantics hold, never latches, peak heap at most CLAMP_PEAK x
   pristine's and the worst loop's GC not above pristine's.

Not proven here: anything on the console; any budget but 65536 (no sweep);
review round 2's B2 chain shape; a __gc that allocates nothing net (it
resurrects its object or raises) -- the guard cannot see those.  B3 is
BOUNDED by falling back to pristine, not resolved: a state whose finalizers
allocate past the budget loses the pacer.

Time: the oracle uses no host time at all (vita_lua_gc_finalizer_pace_
oracle.c:172, "Only counts are computed here"); every ms figure is the
counted-work projection.  Runs of one build still differ, because pristine
Lua seeds its string hash from the time and from addresses (lstate.c
luai_makeseed, oracle.c:24), which moves table layouts and with them the
collector's trajectory -- that, not host load, is why RUNS is 5 and why a
pristine run can pass the abort line on one seed and not on another.

Usage: python recomp/test_vita_lua_gc_finalizer_alloc_latch.py
           --lua-source <lua-5.3.3> [--cc clang] [--work DIR] [--jobs N]
           [--runs N] [--explore]
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / "vita"))

import test_vita_lua_gc_finalizer_pace as T  # noqa: E402
import test_vita_lua_runtime as R            # noqa: E402

pace = T.pace
need = T.need

ALLOC_LATCH = 65536     # the budget under test (bytes of finalizer allocation per cycle)
K = 128                 # ISAAC_VITA_LUA_GCSTEP_CLAMP_KB the pace test's matrix runs
K_CLAMP = 64            # the R60 recipe's clamp
FRAMES = T.LEG_FRAMES
RUNS = 5                # seed-dependent trajectories (see "Time" above): keep >= 5

# name, finalizers per loop, options.  b3-*: review round 2 B3 (more than
# ~200-300 KB of finalizer allocation per loop); b1-*: B1 (more than ~7,000
# objects per loop); the last three are the pace test's ALLOC_LEGS (164-200
# KB per loop, which the review found fine without the guard).
LATCH_LEGS = (
    ("b3-fs32768-r10", 10, ("fin_str=32768",)),
    ("b3-fs262144-r2", 2, ("fin_str=262144",)),
    ("b3-fs1048576-r1", 1, ("fin_str=1048576",)),
    ("b1-fa800-r10", 10, ("fin_alloc=800",)),
    ("fs16384-r10", 10, ("fin_str=16384",)),
    ("fs4096-r50", 50, ("fin_str=4096",)),
    ("fa50-r50", 50, ("fin_alloc=50",)),
)
# leg: the one metric that must get WORSE (higher) without a working guard.
# "abort" = the run passes the oracle's abort line (T.ABORT, 256 MB).  The
# three ALLOC_LEGS are no teeth: there the unguarded pacer has the better
# p99 and only runs about twice the cycles.  Measured (5 runs, two sessions):
#   b3-fs32768-r10   p99 98-108 ms against pristine's 13.7
#   b3-fs262144-r2   p99 266-276 ms against 10.7
#   b3-fs1048576-r1  peak 137-148 MB against 130-132 (p99 and GC ms/s better)
#   b1-fa800-r10     5 of 5 runs abort
TEETH = {
    "b3-fs32768-r10": "frame_ms_p99",
    "b3-fs262144-r2": "frame_ms_p99",
    "b3-fs1048576-r1": "peak_kb",
    "b1-fa800-r10": "abort",
}
TOOTH_UP = 0.03         # the pace test's PRISTINE_TOL: over pristine's median by more than this
# one finalizer per loop that leaves a 64-byte string: ~80 bytes x one
# cycle's loops (~190) stays far under the budget, the whole run's sum
# (36,000 loops) does not -- the per-cycle reset's tooth
SMALL_LEG = "small-fs64-r1"
CLAMP_GC = 1.10         # measured x1.05-1.07 at K = 64 (the pacer's known cost)
INERT_LEGS = (
    (SMALL_LEG, 1, ("fin_str=64",)),
    ("r100", 100, ()),
    ("r200", 200, ()),
    ("r600", 600, ()),
    ("r1000", 1000, ()),
)
CLAMP_LEGS = (
    ("k64-r100", 100, ()),
    ("k64-r120", 120, ()),
    ("k64-r200", 200, ()),
)
EXACT_SEEDS = (1, 2, 3)


def derive_variants(pristine: bytes) -> dict[str, bytes]:
    paced = pace.derive(pristine)
    need(hashlib.sha256(paced).hexdigest() == T.DERIVED_SHA256,
         "alloc latch 0 is not the pinned derivation: "
         f"{hashlib.sha256(paced).hexdigest()}")
    need(pace.derive(pristine, T.MAX, T.STEP, T.SWEEP_STEP, 0) == paced,
         "an explicit alloc latch of 0 changed the derivation")
    guard = pace.derive(pristine, T.MAX, T.STEP, T.SWEEP_STEP, ALLOC_LATCH)
    need(T.changed_lines(paced, guard) == [61, pace.ALLOC_LATCH_LINE, 958],
         f"the guard changed lines {T.changed_lines(paced, guard)}")
    need(T.changed_lines(pristine, guard) ==
         sorted([*T.EXPECTED_LINES, pace.ALLOC_LATCH_LINE]),
         "the guard build's changed lines are not the pacer's plus 821")
    need(guard.count(b"\n") == pristine.count(b"\n"), "line count changed")
    nosweep = pace.derive(pristine, T.MAX, T.STEP, 0, ALLOC_LATCH)
    need(T.changed_lines(pace.derive(pristine, T.MAX, T.STEP, 0), nosweep) ==
         [61, pace.ALLOC_LATCH_LINE, 958], "no-sweep guard changed other lines")
    for bad in (1, pace.ALLOC_LATCH_MIN - 1, pace.ALLOC_LATCH_MAX + 1, -5):
        try:
            pace.derive(pristine, T.MAX, T.STEP, T.SWEEP_STEP, bad)
        except pace.DeriveError:
            continue
        T.fail(f"alloc latch {bad} was not refused")
    for good in (pace.ALLOC_LATCH_MIN, pace.ALLOC_LATCH_MAX):
        pace.derive(pristine, T.MAX, T.STEP, T.SWEEP_STEP, good)

    def swap(text: bytes, old: bytes, new: bytes) -> bytes:
        need(text.count(old) == 1, f"mutant anchor not found once: {old!r}")
        return text.replace(old, new)

    limit = b"(lu_mem)%d) { " % ALLOC_LATCH
    return {
        "pristine": pristine,
        "paced": paced,
        "guard": guard,
        # the guard counts but never latches
        "mutant-guard-no-latch": swap(
            guard, b"isaac_pace_alloc_latches++; "
                   b"isaac_lua_gc_script_control(L, LUA_GCCOLLECT); } }",
            b"isaac_pace_alloc_latches++; } }"),
        # a budget nothing reaches (the knob at its ceiling)
        "mutant-guard-floor": swap(guard, limit,
                                   b"(lu_mem)%d) { " % pace.ALLOC_LATCH_MAX),
        # the per-cycle sum is never cleared
        "mutant-guard-no-reset": swap(guard, pace._ALLOC_RESET[1],
                                      pace._ALLOC_RESET[0]),
    }


def cli_tests(lua_src: Path, work: Path) -> None:
    script = T.PACE_SCRIPT
    out = work / "cli"
    out.mkdir(parents=True, exist_ok=True)
    base = [sys.executable, str(script), "--input", str(lua_src / "lgc.c"),
            "--baselib-input", str(lua_src / "lbaselib.c"),
            "--baselib-output", str(out / "lbaselib.c")]
    off = subprocess.run([*base, "--output", str(out / "lgc0.c")],
                         capture_output=True, text=True)
    need(off.returncode == 0 and "alloc_latch" not in off.stdout and
         f"sha256={T.DERIVED_SHA256}" in off.stdout,
         f"CLI without --alloc-latch: {off.stdout}{off.stderr}")
    on = subprocess.run([*base, "--output", str(out / "lgc1.c"),
                         "--alloc-latch", str(ALLOC_LATCH)],
                        capture_output=True, text=True)
    sha = hashlib.sha256((out / "lgc1.c").read_bytes()).hexdigest()
    need(on.returncode == 0 and
         f"alloc_latch={ALLOC_LATCH} sha256={sha}" in on.stdout,
         f"CLI with --alloc-latch: {on.stdout}{on.stderr}")
    bad = subprocess.run([*base, "--output", str(out / "lgc2.c"),
                          "--alloc-latch", "100"],
                         capture_output=True, text=True)
    need(bad.returncode == 1 and "alloc latch must be" in bad.stderr and
         not (out / "lgc2.c").exists(), "CLI accepted --alloc-latch 100")
    print(f"derivation: alloc latch 0 = pinned {T.DERIVED_SHA256[:16]}..., "
          f"{ALLOC_LATCH} changes lines 61/821/958 only (sha {sha[:16]}...), "
          "range refusals OK")


KNOB = "ISAAC_VITA_LUA_GC_FINALIZER_ALLOC_LATCH_BYTES"


PACER = "ISAAC_VITA_LUA_GC_FINALIZER_PACE"


def receipt_regions(runtime: str) -> list[tuple[int, int]]:
    """[start, end) of host_vita_lua.c's two `#if defined(<PACER>)` regions
    (the receipt and its call), each up to its own #endif."""
    pacer = PACER
    opener = f"#if defined({pacer})\n"
    need(runtime.count(opener) == 2,
         f"host_vita_lua.c: expected two '#if defined({pacer})' regions")
    regions = []
    for start in (i for i in range(len(runtime))
                  if runtime.startswith(opener, i)):
        depth, pos = 0, start
        while True:
            nxt = min((p for p in (runtime.find("\n#if", pos),
                                   runtime.find("\n#endif", pos)) if p >= 0),
                      default=-1)
            need(nxt >= 0, "host_vita_lua.c: unbalanced #if in the receipt")
            if runtime.startswith("\n#endif", nxt):
                if depth == 0:
                    regions.append((start, nxt))
                    break
                depth -= 1
            else:
                depth += 1
            pos = nxt + 1
    return regions


def receipt_static(cmake_text: str, runtime: str) -> None:
    """host_vita_lua.c's pacer receipt: every line of it is inside
    `#if defined(ISAAC_VITA_LUA_GC_FINALIZER_PACE)` (CMake defines that with
    the knob only, so a PACE OFF object cannot contain it), the latch part
    inside `#if <KNOB> > 0`, and the call passes the importing state."""
    pacer = PACER
    need(cmake_text.count(f"          {pacer}=1)") == 1,
         "CMakeLists.txt does not give host_vita_lua.c the pacer's definition "
         "exactly once")
    regions = receipt_regions(runtime)
    inside = "".join(runtime[a:b] for a, b in regions)
    outside = runtime
    for a, b in reversed(regions):
        outside = outside[:a] + outside[b:]
    for token in ("vita_lua_gc_pace_receipt", "isaac_lua_gc_alloc_latches",
                  "isaac_lua_gc_state_latched", "finalizer pace",
                  "finalizer alloc latch", f"{KNOB}"):
        need(token in inside and token not in outside,
             f"host_vita_lua.c: {token!r} is not confined to the "
             f"'#if defined({pacer})' regions")
    need(inside.count("vita_lua_gc_pace_receipt(state);") == 1,
         "host_vita_lua.c: the receipt is not called with the importing state")
    body = inside[inside.index("static void vita_lua_gc_pace_receipt"):]
    flat = "".join(part.lstrip() for part in body.split('"\n'))
    need('[isaac-lua] finalizer pace: on=1 "alloc_latch_bytes=%d '
         'gcstep_clamp_kb=%d' in flat,
         "host_vita_lua.c: no resolved-state line")
    at = body.index(f"#if {KNOB} > 0")
    latch = body[at:body.index("#else", at)]
    for token in ("isaac_lua_gc_alloc_latches()",
                  "isaac_lua_gc_state_latched(state)", "LATCHED",
                  "state_latched=%d", "finalizer pace state: state=%08x"):
        need(token in latch and token not in body.replace(latch, ""),
             f"host_vita_lua.c: {token!r} is not inside '#if {KNOB} > 0'")


RECEIPT_HARNESS = r'''
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
void isaac_vita_log(const char *format, ...);
void isaac_vita_log(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vprintf(format, ap);
    printf("\n");
    va_end(ap);
}
@RECEIPT@
#endif
/* what vita_lua_gc_import does around the receipt (host_vita_lua.c) */
static void gc_import(lua_State *L, int what, int data)
{
    lua_gc(L, what, data);
    vita_lua_gc_pace_receipt(L);
}
static lua_State *fresh(void)
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    /* every finalizer leaves a 16 KB string behind */
    if (luaL_dostring(L, "keep = {} function mk() setmetatable({}, {__gc = "
                         "function() keep[#keep + 1] = string.rep('x', 16384)"
                         " .. #keep end}) end"))
        printf("setup failed: %s\n", lua_tostring(L, -1));
    return L;
}
int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    lua_State *L = fresh();
    int i;

    gc_import(L, LUA_GCSTEP, 0);
    if (!strcmp(mode, "alloc")) {
        /* the engine's schedule: garbage with allocating finalizers, one
         * lua_gc(LUA_GCSTEP) import per loop */
        for (i = 0; i < 150; i++) {
            if (luaL_dostring(L, "for i = 1, 10 do mk() end"))
                printf("loop failed: %s\n", lua_tostring(L, -1));
            gc_import(L, LUA_GCSTEP, 128);
        }
    } else if (!strcmp(mode, "close")) {
        /* the budget is passed by the finalizers lua_close runs on the OLD
         * state; the next import is the NEW state's, which is not latched */
        lua_State *next = fresh();
        lua_gc(L, LUA_GCSTOP, 0);
        if (luaL_dostring(L, "for i = 1, 40 do mk() end"))
            printf("loop failed: %s\n", lua_tostring(L, -1));
        lua_close(L);
        L = next;
        gc_import(L, LUA_GCSTEP, 0);
        gc_import(L, LUA_GCSTEP, 0);
    } else if (!strcmp(mode, "script")) {
        /* a script's collectgarbage latches without the allocation count */
        if (luaL_dostring(L, "collectgarbage('step')"))
            printf("script failed: %s\n", lua_tostring(L, -1));
        gc_import(L, LUA_GCSTEP, 0);
        gc_import(L, LUA_GCSTEP, 0);
    } else {
        printf("unknown mode\n");
        return 2;
    }
    printf("END state=%08x\n", (unsigned)(uintptr_t)L);
    lua_close(L);
    return 0;
}
'''


def receipt_runtime(cc: Path, lua_src: Path, work: Path,
                    variants: dict[str, bytes]) -> None:
    """EXECUTES host_vita_lua.c's receipt: the text of its `#if defined(
    ISAAC_VITA_LUA_GC_FINALIZER_PACE)` region, copied from the file at test
    time, compiled with the knob's definitions and linked with the derived
    lgc.c / lbaselib.c of this run (needs build()'s common objects).  Three
    scenarios, each line of output judged; three mutants, each must fail."""
    import re
    runtime = (T.ROOT / "runtime" / "host_vita_lua.c").read_text(
        encoding="utf-8")
    start, end = receipt_regions(runtime)[0]
    region = runtime[start:end]
    common = sorted(str(p) for p in (work / "common").glob("*.o"))
    need(len(common) == 31, f"receipt: {len(common)} common objects, not 31")
    baselib = work / "lbaselib" / "lbaselib.o"
    directory = work / "receipt"
    directory.mkdir(parents=True, exist_ok=True)
    budget = f"-D{KNOB}={ALLOC_LATCH}"
    clamp = f"-DISAAC_VITA_LUA_GCSTEP_CLAMP_KB={K_CLAMP}"

    def make(tag: str, lgc: bytes, text: str, defines: list[str]) -> Path:
        out = directory / tag
        out.mkdir(parents=True, exist_ok=True)
        (out / "lgc.c").write_bytes(lgc)
        (out / "harness.c").write_text(
            RECEIPT_HARNESS.replace("@RECEIPT@", text), encoding="utf-8",
            newline="\n")
        log = T.run([str(cc), "-m32", "-O2", "-Wall", "-Wextra",
                     f"-I{lua_src}", f"-D{PACER}=1", *defines,
                     str(out / "harness.c"), "-c", "-o",
                     str(out / "harness.o")], work)
        need(tag.startswith("mutant-") or T.warning_count(log) == 0,
             f"receipt {tag}: the receipt warns under -Wall -Wextra:\n{log}")
        T.run([str(cc), "-m32", "-O2", "-w", f"-I{lua_src}", "-c",
               str(out / "lgc.c"), "-o", str(out / "lgc.o")], work)
        T.run([str(cc), "-m32", str(out / "harness.o"), str(out / "lgc.o"),
               str(baselib), *common, "-o", str(out / "receipt.exe")], work)
        return out / "receipt.exe"

    def lines(exe: Path, mode: str) -> list[str]:
        done = subprocess.run([str(exe), mode], capture_output=True, text=True)
        need(done.returncode == 0, f"receipt {exe.parent.name} {mode}: rc "
             f"{done.returncode}\n{done.stdout}{done.stderr}")
        return done.stdout.splitlines()

    banner = (f"[isaac-lua] finalizer pace: on=1 alloc_latch_bytes="
              f"{ALLOC_LATCH} gcstep_clamp_kb={K_CLAMP}")
    state = (r"\[isaac-lua\] finalizer pace state: state=([0-9a-f]{8}) "
             r"latched=%d alloc_latches=%d line=%d/32")
    latched = (rf"\[isaac-lua\] finalizer alloc latch: LATCHED latches=1 "
               rf"budget={ALLOC_LATCH} state=([0-9a-f]{{8}}) state_latched=%d "
               r"\(1: this state runs the pristine 5\.3\.3 collector from "
               r"here on; 0: the budget was passed in another state\)")

    def judge(exe: Path) -> list[str]:
        """[] = the three scenarios print exactly what they must."""
        bad = []

        def check(mode: str, want: list[str], same: tuple, differ: tuple = ()):
            got = lines(exe, mode)
            if len(got) != len(want) + 2 or got[0] != banner:
                bad.append(f"{mode}: {got}")
                return
            found = [re.fullmatch(pattern, line)
                     for pattern, line in zip(want, got[1:-1])]
            end_state = got[-1].removeprefix("END state=")
            if not all(found):
                bad.append(f"{mode}: {got}")
                return
            ids = [m.group(1) for m in found]
            if any(ids[i] != end_state for i in same) or any(
                    ids[i] == end_state for i in differ):
                bad.append(f"{mode}: state ids {ids} end {end_state}")

        # the live state passes the budget: LATCHED says state_latched=1
        check("alloc", [state % (0, 0, 1), latched % 1, state % (1, 1, 2)],
              same=(0, 1, 2))
        # the closed state passed it: the new state's lines say latched=0
        check("close", [state % (0, 0, 1), latched % 0, state % (0, 1, 2)],
              same=(1, 2), differ=(0,))
        # a script latch: a state line, the count stays 0, no LATCHED line
        check("script", [state % (0, 0, 1), state % (1, 0, 2)], same=(0, 1))
        return bad

    guard = variants["guard"]
    real = make("real", guard, region, [budget, clamp])
    bad = judge(real)
    need(not bad, "receipt: the real build prints the wrong lines: "
         + " | ".join(bad))

    def swap(text, old, new):
        need(text.count(old) == 1, f"receipt mutant anchor not once: {old!r}")
        return text.replace(old, new)

    mutants = {
        # the receipt reads the process-wide count as "this state"
        "mutant-receipt-process-wide": (guard, swap(
            region, "isaac_lua_gc_state_latched(state) != 0", "now != 0")),
        # the accessor answers for the process, not for L's state
        "mutant-accessor-process-wide": (swap(
            guard, b"return isaac_lua_gc_scripted(G(L)); }",
            b"return isaac_pace_alloc_latches != 0; }"), region),
        # no line when a state's latch changes (the script latch is silent)
        "mutant-receipt-no-state-line": (guard, swap(
            region, "latched != s_latched) &&", "latched < s_latched) &&")),
    }
    for name, (lgc, text) in mutants.items():
        why = judge(make(name, lgc, text, [budget, clamp]))
        need(bool(why), f"receipt {name} SURVIVED")
        print(f"teeth receipt {name}: killed ({why[0][:150]})")
    # budget 0: links against the derivation without the accessors and
    # prints the resolved line only, whatever happens
    zero = make("budget0", variants["paced"], region, [clamp])
    for mode in ("alloc", "script"):
        got = lines(zero, mode)
        need(len(got) == 2 and got[0] == banner.replace(
            f"alloc_latch_bytes={ALLOC_LATCH}", "alloc_latch_bytes=0"),
            f"receipt budget 0 {mode}: {got}")
    print("receipt (executed): resolved line; LATCHED state_latched=1 on the "
          "live state, =0 when lua_close's finalizers passed the budget; a "
          "script latch prints a state line and no LATCHED; budget 0 prints "
          f"the resolved line only; {len(mutants)}/{len(mutants)} mutants "
          "killed")


def cmake_tests(lua_root: Path, pristine: bytes, work: Path) -> None:
    """The knob's CMake text, run as cmake -P probes (the pace test's way):
    its two validation blocks and the swap block that calls the generator."""
    import shutil
    cmake = shutil.which("cmake") or "C:/Program Files/CMake/bin/cmake.exe"
    need(Path(cmake).exists(), "cmake is required for the CMake leg")
    validation = T.block(f'if(NOT {KNOB} MATCHES "^[0-9]+$" OR',
                         "if(", "endif(", 2)
    need("must be 0 or an integer" in validation and
         "takes effect only with" in validation,
         "the extracted validation lost a check")
    swap = T.block("  if(ISAAC_VITA_LUA_GC_FINALIZER_PACE)", "if(", "endif(", 1)
    need(f'--alloc-latch "${{{KNOB}}}"' in swap,
         "the swap block does not pass --alloc-latch")
    text = T.VITA_CMAKE.read_text(encoding="utf-8")
    need(f'set({KNOB} 0 CACHE STRING' in text, "the knob's default is not 0")
    need(f"{KNOB}=${{{KNOB}}})" in text,
         "host_vita_lua.c does not get the knob's definition")
    runtime = (T.ROOT / "runtime" / "host_vita_lua.c").read_text(
        encoding="utf-8")
    receipt_static(text, runtime)

    def configure(tag: str, pace_on: str, budget: str,
                  sweep: int = T.SWEEP_STEP,
                  profile: bool = False) -> tuple[int, str, Path]:
        build = work / "cmake" / tag
        build.mkdir(parents=True, exist_ok=True)
        script = build / "probe.cmake"
        script.write_text("\n".join([
            "cmake_minimum_required(VERSION 3.16)",
            "function(set_property)",
            "endfunction()",
            "macro(add_custom_command)",
            "endmacro()",
            "set(ISAAC_VITA_LUA ON)",
            f"set(ISAAC_VITA_LUA_GC_FINALIZER_PACE {pace_on})",
            f'set(ISAAC_VITA_LUA_GC_FINALIZER_MAX "{T.MAX}")',
            f'set(ISAAC_VITA_LUA_GC_FINALIZER_STEP "{T.STEP}")',
            f'set(ISAAC_VITA_LUA_GC_SWEEP_STEP "{sweep}")',
            f'set({KNOB} "{budget}")',
            *(("set(ISAAC_VITA_LUA_GC_STATE_PROFILE ON)",) if profile else ()),
            f'set(ISAAC_LUA53_SOURCE_DIR "{T.posix(lua_root)}")',
            f'set(CMAKE_CURRENT_SOURCE_DIR "{T.posix(T.VITA_CMAKE.parent)}")',
            f'set(CMAKE_BINARY_DIR "{T.posix(build)}")',
            f'set(Python3_EXECUTABLE "{T.posix(Path(sys.executable))}")',
            validation,
            f'include("{T.posix(T.VITA_CMAKE.parent / "lua53_vanilla.cmake")}")',
            swap,
            "",
        ]), encoding="utf-8", newline="\n")
        done = subprocess.run([cmake, "-P", str(script)], capture_output=True,
                              text=True)
        return done.returncode, done.stdout + done.stderr, build

    rc, out, build = configure("on-0", "ON", "0")
    lgc = build / "lua53-gc-finalizer-pace" / "lgc.c"
    need(rc == 0 and lgc.read_bytes() == pace.derive(pristine) and
         "alloc_latch" not in out, f"PACE ON, budget 0: {out}")
    rc, out, build = configure("on-a", "ON", str(ALLOC_LATCH))
    lgc = build / "lua53-gc-finalizer-pace" / "lgc.c"
    want = pace.derive(pristine, T.MAX, T.STEP, T.SWEEP_STEP, ALLOC_LATCH)
    need(rc == 0 and lgc.read_bytes() == want and
         f"alloc_latch={ALLOC_LATCH} sha256={hashlib.sha256(want).hexdigest()}"
         in " ".join(out.split()), f"PACE ON, budget {ALLOC_LATCH}: {out}")
    # F-GCSWEEP: the 0.2.0 arms GW (sweep quota T.SWEEP_STEP_020 with the
    # recipe's budget) and GWS (the same plus the state profile), end to
    # end: CMake derives, and the allocator gate, reading the three knobs
    # from a CMakeCache.txt, accepts exactly that build tree's lgc.c and
    # refuses it under a cache that names another quota, budget or profile
    import vita_raw_allocator_gate as gate
    sweep_key = gate.LUA_GC_SWEEP_STEP_CACHE_KEY
    profile_key = gate.LUA_GC_STATE_PROFILE_CACHE_KEY
    gated = 0
    for tag, profile in (("on-a-sweep020", False), ("on-a-sweep020-p", True)):
        rc, out, build = configure(tag, "ON", str(ALLOC_LATCH),
                                   sweep=T.SWEEP_STEP_020, profile=profile)
        lgc = build / "lua53-gc-finalizer-pace" / "lgc.c"
        want020 = pace.derive(pristine, T.MAX, T.STEP, T.SWEEP_STEP_020,
                              ALLOC_LATCH, state_profile=profile)
        need(rc == 0 and lgc.read_bytes() == want020 and want020 != want and
             f"sweep_step={T.SWEEP_STEP_020} alloc_latch={ALLOC_LATCH} "
             + ("state_profile=1 " if profile else "") +
             f"sha256={hashlib.sha256(want020).hexdigest()}"
             in " ".join(out.split()),
             f"PACE ON, sweep {T.SWEEP_STEP_020}, budget {ALLOC_LATCH}, "
             f"profile {profile}: {out}")
        cache_file = build / "CMakeCache.txt"

        def gate_run(**changed: str) -> dict:
            entries = {
                gate.LUA_GC_FINALIZER_PACE_CACHE_KEY: ("BOOL", "ON"),
                gate.LUA_GC_FINALIZER_MAX_CACHE_KEY: ("STRING", str(T.MAX)),
                gate.LUA_GC_FINALIZER_STEP_CACHE_KEY: ("STRING", str(T.STEP)),
                sweep_key: ("STRING", str(T.SWEEP_STEP_020)),
                gate.LUA_GC_FINALIZER_ALLOC_LATCH_CACHE_KEY:
                    ("STRING", str(ALLOC_LATCH)),
                profile_key: ("BOOL", "ON" if profile else "OFF"),
            }
            for key, value in changed.items():
                entries[key] = (entries[key][0], value)
            cache_file.write_text(
                "# test cache\n" + "".join(
                    f"{key}:{kind}={value}\n"
                    for key, (kind, value) in entries.items()),
                encoding="utf-8", newline="\n")
            return gate.lua_derived_sources(
                gate.read_cache(cache_file), build_dir=build,
                lua_source=lua_root)

        replaced = gate_run()
        need(replaced.get((lua_root / "src" / "lgc.c").resolve()) ==
             lgc.resolve() and len(replaced) == 2,
             f"the gate did not map lgc.c to {lgc}: {replaced}")
        for label, changed in (
                ("the default quota", {sweep_key: str(T.SWEEP_STEP)}),
                ("no budget", {gate.LUA_GC_FINALIZER_ALLOC_LATCH_CACHE_KEY:
                               "0"}),
                ("the other profile setting",
                 {profile_key: "OFF" if profile else "ON"})):
            try:
                gate_run(**changed)
            except gate.GateError as exc:
                need("derived lgc.c differs from the derivation" in str(exc),
                     f"{tag}, cache with {label}: wrong refusal: {exc}")
            else:
                T.fail(f"{tag}: the gate accepted sweep "
                       f"{T.SWEEP_STEP_020} bytes under a cache with {label}")
            gated += 1
    refusals = (
        ("off-a", "OFF", str(ALLOC_LATCH), "takes effect only with"),
        ("on-low", "ON", "15999", "must be 0 or an integer"),
        ("on-high", "ON", "1073741825", "must be 0 or an integer"),
        ("on-text", "ON", "64k", "must be 0 or an integer"),
    )
    for tag, pace_on, budget, message in refusals:
        rc, out, build = configure(tag, pace_on, budget)
        need(rc != 0 and message in " ".join(out.split()) and
             not (build / "lua53-gc-finalizer-pace").exists(),
             f"{tag}: budget {budget} with PACE {pace_on} was not refused "
             f"with {message!r}: {out}")
    rc, out, build = configure("off-0", "OFF", "0")
    need(rc == 0 and not (build / "lua53-gc-finalizer-pace").exists(),
         f"PACE OFF, budget 0: {out}")
    print(f"cmake: budget 0 derives the pinned bytes, {ALLOC_LATCH} the "
          f"guard's and prints alloc_latch={ALLOC_LATCH}; sweep "
          f"{T.SWEEP_STEP_020} with the budget (and with the state profile) "
          f"derives, prints its receipt and passes the allocator gate from "
          f"a CMakeCache.txt, {gated} wrong-cache refusals; "
          f"{len(refusals)} refusals OK; the receipt in host_vita_lua.c is "
          "behind the pacer's and the knob's definitions")


def build(cc: Path, lua_src: Path, sources: list[Path], baselib: bytes,
          variants: dict[str, bytes], work: Path,
          parts: dict) -> dict[str, Path]:
    """As test_vita_lua_gc_finalizer_pace.build_variants, for these variants
    (same flags, same run-path discipline); every build is -Wall -Wextra on
    the oracle and must not warn on the derived lgc.c either."""
    common = work / "common"
    common.mkdir(parents=True, exist_ok=True)
    flags = ["-m32", "-O2", "-w", f"-I{lua_src}"]
    objects = []
    for source in sources:
        if source.name in ("lgc.c", "lbaselib.c"):
            continue
        obj = common / (source.stem + ".o")
        T.run([str(cc), *flags, "-c", str(source), "-o", str(obj)], work)
        objects.append(str(obj))
    bl_dir = work / "lbaselib"
    bl_dir.mkdir(parents=True, exist_ok=True)
    (bl_dir / "lbaselib.c").write_bytes(baselib)
    T.run([str(cc), *flags, "-c", str(bl_dir / "lbaselib.c"), "-o",
           str(bl_dir / "lbaselib.o")], work)
    T.run([str(cc), *flags, "-c", str(lua_src / "lbaselib.c"), "-o",
           str(bl_dir / "lbaselib-pristine.o")], work)
    quota = ["-DORACLE_QUOTA=1", f"-DORACLE_FIN_MAX={T.MAX}",
             f"-DORACLE_FIN_STEP={T.STEP}",
             f"-DORACLE_SWEEP_STEP={T.SWEEP_STEP}", "-DORACLE_LATCH=1"]
    exes = {}
    for index, (name, text) in enumerate(variants.items()):
        directory = work / name
        directory.mkdir(parents=True, exist_ok=True)
        lgc = directory / "lgc.c"
        lgc.write_bytes(text)
        defines = [] if name == "pristine" else list(quota)
        lgc_defines = [d for d in defines
                       if d.startswith(("-DORACLE_QUOTA", "-DORACLE_LATCH"))]
        if name not in ("pristine", "paced"):
            lgc_defines.append("-DORACLE_ALLOC_LATCH=1")
        bl_obj = bl_dir / ("lbaselib-pristine.o" if name == "pristine"
                           else "lbaselib.o")
        lgc_obj = directory / "lgc.o"
        out = T.run([str(cc), "-m32", "-O2", "-Wall", "-Wextra",
                     f"-I{lua_src}", "-DluaC_step=oracle_real_luaC_step",
                     "-DluaE_setdebt=oracle_lgc_setdebt_hook", *lgc_defines,
                     "-include", str(lgc), "-c", str(T.ORACLE_LGC), "-o",
                     str(lgc_obj)], work)
        if name in ("paced", "guard"):
            need(T.warning_count(out) == 0,
                 f"{name}: lgc.c warns under -Wall -Wextra:\n{out}")
        exe = directory / "oracle.exe"
        T.run([str(cc), "-m32", "-O2", "-Wall", "-Wextra", f"-I{lua_src}",
               *defines, str(T.ORACLE), *objects, str(lgc_obj), str(bl_obj),
               "-o", str(exe)], work)
        exes[name] = T.runner_copy(work, exe, f"{index:03d}", 0)
        parts[name] = (lgc_obj, bl_obj, defines)
    return exes


def latches(res: dict) -> int:
    for line in res["out"].splitlines():
        if line.startswith("  guard alloc_latches="):
            return int(line.split("=", 1)[1])
    T.fail(f"no guard line in the oracle output\n{res['out']}")
    raise AssertionError


# pristine's band on a latch leg: the pace test's JUDGED metrics by its own
# rule (judge_pristine: PRISTINE_TOL or pristine's spread), plus the figures
# the claims review said the pace test did not judge
BAND = 0.10
EXTRA = ("gc_ms_per_s", "cycles", "frame_ms_p99", "frame_ms_max", "peak_kb")
LATCH_FRAMES = 400      # loops; measured 178-182 (the first cycle that runs finalizers)
CLAMP_PEAK = 1.15       # the pace test's matrix bound on the peak heap


def latch_reasons(new: list[dict]) -> list[str]:
    """Why `new` did not latch as the guard must ([] = it did)."""
    reasons = list(T.not_finished(new))
    if reasons:
        return reasons
    for res in new:
        if res["scripted"] != 1:
            reasons.append("a run never latched")
        elif res["scripted_frame"] > LATCH_FRAMES:
            reasons.append(f"latched at loop {res['scripted_frame']} "
                           f"(> {LATCH_FRAMES})")
        if latches(res) != 1:
            reasons.append(f"alloc_latches={latches(res)} (want 1)")
    return sorted(set(reasons))


def metric_reasons(new: list[dict], base: list[dict]) -> list[str]:
    """Why `new` is not in pristine's band ([] = it is)."""
    reasons = list(T.not_finished(new))
    if reasons:
        return reasons
    for res in new:
        if not res["semantics_ok"]:
            reasons.append("finalizer semantics violated")
    more, unused = T.judge_pristine(new, base)
    reasons += more
    for key in EXTRA:
        ref = [res[key] for res in base]
        a = statistics.median(ref)
        b = statistics.median(res[key] for res in new)
        if a and abs(b / a - 1.0) > BAND and not min(ref) <= b <= max(ref):
            reasons.append(f"{key} {b:.4g} vs pristine {a:.4g} "
                           f"({b / a - 1.0:+.1%})")
    return sorted(set(reasons))


def tooth_reason(new: list[dict], base: list[dict], metric: str) -> str:
    """How `new` is WORSE than pristine on the leg's named metric ('' = it is
    not).  One-sided: only the named metric, only upward.  A majority of
    aborted runs is worse on any leg (the heap passed the abort line)."""
    lost = [res for res in new if not T.finished(res)]
    if 2 * len(lost) > len(new):
        return (f"abort: {len(lost)}/{len(new)} runs "
                f"({'; '.join(T.not_finished(new))})")
    if metric == "abort":
        return ""
    ref = [res[metric] for res in base]
    a = statistics.median(ref)
    b = statistics.median(res[metric] for res in new if T.finished(res))
    if b > max(ref) and a > 0 and b / a - 1.0 > TOOTH_UP:
        return (f"{metric} up: {b:.4g} vs pristine {a:.4g} ({b / a - 1.0:+.1%}, "
                f"above pristine's worst run {max(ref):.4g})")
    return ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--lua-source", type=Path)
    parser.add_argument("--cc", type=Path)
    parser.add_argument("--work", type=Path)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--runs", type=int, default=RUNS)
    parser.add_argument("--static-only", action="store_true",
                        help="stop after the derivation, CLI, CMake and "
                             "receipt legs (no collector runs)")
    parser.add_argument("--explore", action="store_true",
                        help="print every run's row and judge nothing")
    args = parser.parse_args()
    lua_root = R.discover_source(args.lua_source)
    sources = R.validate_source(lua_root, R.records())
    need(len(sources) == 33, f"expected the 33-C closure, got {len(sources)}")
    lua_src = lua_root / "src"
    pristine = (lua_src / "lgc.c").read_bytes()
    pristine_baselib = (lua_src / "lbaselib.c").read_bytes()
    cc = T.find_cc(args.cc)
    holder = None
    if args.work is None:
        holder = tempfile.TemporaryDirectory(prefix="isaac-lua-gc-alat-")
        work = Path(holder.name)
    else:
        work = args.work.resolve()
        work.mkdir(parents=True, exist_ok=True)
    failures: list[str] = []
    try:
        variants = derive_variants(pristine)
        cli_tests(lua_src, work)
        cmake_tests(lua_root, pristine, work)
        parts: dict = {}
        exes = build(cc, lua_src, sources,
                     pace.derive_baselib(pristine_baselib), variants, work,
                     parts)
        receipt_runtime(cc, lua_src, work, variants)
        if args.static_only:
            print("static-only: derivation, CLI, CMake and the executed "
                  "receipt legs passed; no collector leg was run")
            return 0
        exact = T.build_exact(cc, lua_src, work, parts, ("paced", "guard"))
        print(f"built {len(exes)} oracle variants + {len(exact)} exact builds")
        pool = ThreadPoolExecutor(max_workers=max(1, args.jobs))
        jobs: dict = {}

        def submit(build_name: str, leg: tuple, k: int, run: int,
                   exe: Path | None = None, extra: tuple = ()) -> None:
            name, fin, options = leg
            runner = T.runner_copy(work, exe or exes[build_name],
                                   f"{len(jobs) % 1000:03d}", 10 + run)
            jobs[(build_name, name, run)] = pool.submit(
                T.run_leg, runner, k, fin, FRAMES, T.GARB,
                (*options, *extra, T.ABORT))

        mutant_names = [n for n in variants if n.startswith("mutant-")]
        for leg in LATCH_LEGS:
            for build_name in ("pristine", "paced", "guard", *mutant_names):
                for run in range(args.runs):
                    submit(build_name, leg, K, run)
        for leg in INERT_LEGS:
            for build_name in ("pristine", "paced", "guard",
                               "mutant-guard-no-reset"):
                for run in range(args.runs):
                    submit(build_name, leg, K, run)
            for seed in EXACT_SEEDS:
                for build_name in ("paced", "guard"):
                    submit(f"x-{build_name}", leg, K, seed,
                           exact[build_name], (f"seed={seed}",))
        for leg in CLAMP_LEGS:
            for build_name in ("pristine", "paced", "guard"):
                for run in range(args.runs):
                    submit(build_name, leg, K_CLAMP, run)
        results = {key: future.result() for key, future in jobs.items()}
        pool.shutdown()

        def runs_of(build_name: str, leg: tuple) -> list[dict]:
            return [results[(build_name, leg[0], run)]
                    for run in range(args.runs)]

        def show(leg: tuple, names: tuple) -> None:
            for build_name in names:
                for res in runs_of(build_name, leg):
                    tail = ""
                    if T.finished(res):
                        tail = (f" alloc_latches={latches(res)} "
                                f"cyc_frames={res.get('per_cycle_frames', 0):.0f}")
                    print("  " + T.row(leg[0], build_name, res) + tail)

        for leg in LATCH_LEGS:
            show(leg, ("pristine", "paced", "guard", *mutant_names))
        for leg in INERT_LEGS:
            show(leg, ("pristine", "paced", "guard", "mutant-guard-no-reset"))
        for leg in CLAMP_LEGS:
            show(leg, ("pristine", "paced", "guard"))
        if args.explore:
            for leg in INERT_LEGS:
                for seed in EXACT_SEEDS:
                    print(f"  exact {leg[0]} seed={seed} "
                          f"paced={results[('x-paced', leg[0], seed)].get('digest')} "
                          f"guard={results[('x-guard', leg[0], seed)].get('digest')}")
            print("explore: nothing judged")
            return 0

        def verdict(what: str, reasons: list[str]) -> None:
            print(f"{what}: {'PASS' if not reasons else 'FAIL ' + '; '.join(reasons)}")
            if reasons:
                failures.append(what)

        # Runs differ by Lua's time- and address-derived string-hash seed
        # (the docstring's "Time"), and at the review's shapes pristine
        # 5.3.3 itself can run past the oracle's 256 MB abort line on some
        # seeds (measured: 1 of 5 runs of b3-fs262144-r2).  Such a run is
        # printed and counted, the band is pristine's finished runs, and a
        # leg where pristine does not finish a majority is not judged at all
        # (that stops the test).
        pristine_lost: list[str] = []

        def pristine_of(leg: tuple) -> list[dict]:
            runs = runs_of("pristine", leg)
            done = [res for res in runs if T.finished(res)]
            need(2 * len(done) > len(runs),
                 f"pristine did not finish {leg[0]}: "
                 f"{'; '.join(T.not_finished(runs))}")
            note = f"{leg[0]} {len(runs) - len(done)}/{len(runs)}"
            if len(done) != len(runs) and note not in pristine_lost:
                pristine_lost.append(note)
                print(f"NOTE pristine itself: {leg[0]} "
                      f"{'; '.join(T.not_finished(runs))} -- the band is its "
                      f"{len(done)} finished runs")
            return done

        # 2. latch legs: the guard latches and is in pristine's band
        for leg in LATCH_LEGS:
            base = pristine_of(leg)
            guard_runs = runs_of("guard", leg)
            at = sorted(res.get("scripted_frame", -1) for res in guard_runs)
            verdict(f"latch {leg[0]} guard latches (loops {at}) and is in "
                    "pristine's band",
                    latch_reasons(guard_runs) + metric_reasons(guard_runs, base))
        # 3. teeth: without the guard, or with a broken one, the leg's named
        # metric is worse than pristine's (one-sided, tooth_reason); the
        # guard build itself must NOT trip its own tooth
        for leg in LATCH_LEGS:
            if leg[0] not in TEETH:
                continue
            metric = TEETH[leg[0]]
            base = pristine_of(leg)
            own = tooth_reason(runs_of("guard", leg), base, metric)
            verdict(f"teeth {leg[0]} guard is not worse on {metric}",
                    [own] if own else [])
            for build_name in ("paced", "mutant-guard-no-latch",
                               "mutant-guard-floor"):
                got = runs_of(build_name, leg)
                why = tooth_reason(got, base, metric)
                never = all(not T.finished(res) or res["scripted"] == 0
                            for res in got)
                ok = bool(why) and never
                info = "; ".join(metric_reasons(got, base))
                print(f"teeth {leg[0]} {build_name} on {metric}: "
                      f"{'killed' if ok else 'SURVIVED'} ({why or 'not worse'}"
                      f"{'' if never else '; it latched'})  [all differences, "
                      f"either direction: {info}]")
                if not ok:
                    failures.append(f"teeth {leg[0]} {build_name}")
        counted = [latches(res) for leg in LATCH_LEGS if leg[0] in TEETH
                   for res in runs_of("mutant-guard-no-latch", leg)
                   if T.finished(res)]
        verdict("mutant-guard-no-latch counted past the budget in every "
                f"finished run (min {min(counted) if counted else 0})",
                [] if counted and min(counted) > 0 else ["no count"])
        # 4. inert legs: never latches, bit-identical trajectory
        for leg in INERT_LEGS:
            reasons = list(T.not_finished(runs_of("guard", leg)))
            for res in runs_of("guard", leg):
                if T.finished(res) and (res["scripted"] or latches(res)):
                    reasons.append(f"latched at loop {res['scripted_frame']}")
            digests = []
            for seed in EXACT_SEEDS:
                a = results[("x-paced", leg[0], seed)].get("digest")
                b = results[("x-guard", leg[0], seed)].get("digest")
                digests.append(b)
                if a is None or a != b:
                    reasons.append(f"seed {seed}: digest {b} != no-guard {a}")
                if results[("x-guard", leg[0], seed)].get("scripted"):
                    reasons.append(f"seed {seed}: the exact build latched")
            verdict(f"inert {leg[0]} guard never latches, exact digests = "
                    f"no-guard build's ({' '.join(str(d) for d in digests)})",
                    reasons)
            if leg[0] == SMALL_LEG:
                got = runs_of("mutant-guard-no-reset", leg)
                ok = (not T.not_finished(got) and
                      all(res["scripted"] == 1 for res in got))
                print(f"teeth {leg[0]} mutant-guard-no-reset: "
                      f"{'killed (latched in every run)' if ok else 'SURVIVED'}")
                if not ok:
                    failures.append(f"teeth {leg[0]} mutant-guard-no-reset")
        # 5. clamp legs at K = 64
        for leg, clamp_build in [(leg, name) for leg in CLAMP_LEGS
                                 for name in ("guard", "paced")]:
            base = runs_of("pristine", leg)
            got = runs_of(clamp_build, leg)
            reasons = list(T.not_finished(got)) + list(T.not_finished(base))
            figures = ""
            if not reasons:
                for res in got:
                    if not res["semantics_ok"]:
                        reasons.append("finalizer semantics violated")
                    if res["bound_bad"]:
                        reasons.append(f"bound violations {res['bound_bad']}")
                    if res["scripted"] or latches(res):
                        reasons.append("latched")

                def med(runs: list[dict], key: str) -> float:
                    return statistics.median(res[key] for res in runs)

                peak = med(got, "peak_kb") / med(base, "peak_kb")
                worst = med(got, "frame_ms_max") / med(base, "frame_ms_max")
                gc = med(got, "gc_ms_per_s") / med(base, "gc_ms_per_s")
                figures = (f" (peak x{peak:.3f}, worst loop x{worst:.3f} = "
                           f"{med(got, 'frame_ms_max'):.2f} ms vs "
                           f"{med(base, 'frame_ms_max'):.2f}, GC ms/s x{gc:.3f})")
                if peak > CLAMP_PEAK:
                    reasons.append(f"peak x{peak:.3f} > {CLAMP_PEAK}")
                if worst > 1.0:
                    reasons.append(f"worst loop x{worst:.3f}")
                if gc > CLAMP_GC:
                    reasons.append(f"GC ms/s x{gc:.3f} > {CLAMP_GC}")
            verdict(f"clamp {leg[0]} K={K_CLAMP} {clamp_build} vs "
                    f"pristine{figures}",
                    sorted(set(reasons)))
        if failures:
            print(f"finalizer alloc latch: FAIL ({len(failures)}): "
                  + " | ".join(failures))
        else:
            print(f"finalizer alloc latch (A={ALLOC_LATCH}): PASS -- "
                  f"{len(LATCH_LEGS)} latch legs, {len(TEETH)} x 3 one-sided "
                  f"teeth + the no-reset tooth, {len(INERT_LEGS)} inert legs x "
                  f"{len(EXACT_SEEDS)} exact seeds, {len(CLAMP_LEGS)} clamp "
                  f"legs x 2 builds, {args.runs} runs per build; pristine runs that "
                  "did not finish: "
                  f"{', '.join(pristine_lost) if pristine_lost else 'none'}")
    finally:
        if holder is not None:
            holder.cleanup()
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
