#!/usr/bin/env python3
"""Host oracle for ISAAC_VITA_LUA_GC_FINALIZER_PACE (no Vita needed).

1. The derivation (recomp/vita/lua53_gc_finalizer_pace.py) pins the
   manifest's pristine lgc.c and lbaselib.c and changes exactly
   EXPECTED_LINES of lgc.c (39 only with a sweep quota) and BASELIB_LINES of
   lbaselib.c, refuses a non-pristine input and out-of-range constants; its
   CLI writes the same bytes for both files, leaves an up-to-date output
   alone and rewrites a stale or missing one.  The derived files add no
   -Wall -Wextra warning (clang -m32; WSL gcc when available).  The review-r0
   derivation (0f5324a: constant charge 512, sweep 32), the review-r1
   derivation (eaec5e2: quotas and a credit clamp, no schedule rule) and the
   review-r4 derivation (c447d9e: the schedule with the restart detector and
   the cut, no latch) are rebuilt from their pinned lines and checked
   against their receipt shas; all three are negative controls below.
2. recomp/runtime/vita_lua_gc_finalizer_pace_oracle.c is built 32-bit (the
   Vita ABI: sizeof(TString) = 16, GCSTEPSIZE = 1600) against the
   manifest-verified Lua 5.3.3 sources, each lgc.c variant compiled into
   vita_lua_gc_finalizer_pace_oracle_lgc.c (the oracle reads the budget the
   variant's own lgc.c:1133 chose and charges with the variant's own
   macros; it predicts nothing), for: pristine, review-r0, review-r1,
   review-r4, the derived default (STEP 192, SWEEP_STEP 6144, MAX 8, with
   the derived lbaselib.c), the derived default without the sweep quota,
   mutants that each undo or move one derived rule (mutants()) and line-60/61
   boundary mutants (helper_mutants()).
     - line-60/61 boundaries: every latch build passes the oracle's
       "helpers" checks (the swept booking and its conversion, open's
       offset, vend, keep-V in GCScallfin only, the late rule and its clip,
       the quota charge; count/countb/isrunning never latch, every control
       option latches and hands its own collection to pristine, a latched
       state has no late rule, per state, four slots, forget, the
       overflow); every boundary mutant fails them;
     - semantics, every build: no finalizer runs early, no finalized object
       is still a weak value, batches keep tobefnz order, every object is
       finalized exactly once (also after lua_close), weak keys are cleared,
       and a raising __gc surfaces the same LUA_ERRGCMM in every build but
       review-r5 and mutant-r5-stopped, which must panic in that case (the
       next raising __gc runs from lua_pushcfunction's own GC check, ahead
       of the lua_pcall: review r5 B2);
     - pacing bound, derived builds, every paced luaC_step (a latched
       state's steps, which are pristine, are counted apart): at most
       (budget + GCSTEPSIZE) / c + 2 * MAX + 1
       finalizers and at most floor(5 * STEP / 4) + 2 * MAX (bound0, the
       device criterion); pristine and the charge/cap/budget mutants must
       exceed it; the variant's recorded budget equals getdebt on entry in
       every paced step (budget_mismatch 0);
     - coverage: doubling, cap, drain, GCScallfin, tail, the quota charge
       and allocation-driven steps that ran finalizers;
     - sweep line live: the quota build needs >= 3x the sweep steps per
       cycle of the no-sweep build.
3. The review-r1 matrix, 30 simulated minutes per leg (108,000 loops at 60
   loops/s), engine only (no control script): pristine at K=128 (today),
   review-r0 at K=40 and the derived default at K=128, on 10, 100, 120 and
   200 finalizers per loop, the burst profile, one loop that kills 8,328 at
   once, the 100/200 legs under a 33 MiB and a 25 MiB allocator cap, and two
   emergency legs.  The derived build must finish, keep the semantics and
   the bound, never latch, run no pristine step, peak <= 1.15x
   pristine with no growth in the second half, lag max <= 2x, no refusal
   under the caps; on the normal legs cut the per-loop GC time p99 and max
   to <= 0.5x pristine's, with a share of loops over 2 ms of GC no more than
   3 points above review-r1's (the trade r1 was accepted with); review-r0
   must fail every leg but r10.  Frame times are a device projection, not
   host time.
4. Engine legs (ENGINE_LEGS, 36,000 loops, RUNS runs per build): the
   engine-only defaults at 600 and 1000 finalizers per loop, a script that
   calls only collectgarbage("count") or ("isrunning") every loop, and the
   engine's own lua_gc(LUA_GCCOLLECT) every 600 loops.  The derived build
   must never latch, keep the benefit (median worst loop GC <= 0.5x
   pristine's, p99 no worse than pristine's -- at 1000 per loop pristine's
   p99 is already low, its cost sits in a few huge loops -- and neither
   worse than review-r4's, c447d9e, beyond max(3 %, its spread)) and a
   median peak within ENGINE_PEAK_RATIO of pristine's (or at most
   pristine's highest run).  The same runs, against pristine and review-r5
   (0474604):
     - ALLOC_LEGS (review r5 B1: a C __gc that allocates a 16 KB string at
       10 per loop, a 4 KB one at 50, 50 tables at 50): the derived build
       never latches, keeps pristine's worst loop and p99 (median within
       max(3 %, pristine's spread)) and its peak (ENGINE_PEAK_RATIO);
       review-r5 must lose on the worst loop or the p99 on each of
       ALLOC_TEETH (the two string legs; on fa50-r50 pristine's own worst
       loop is trimodal, 150/281/429 ms over five runs, and r5's 204 ms
       sits inside it, so that leg judges the derived build only);
     - ERROR_LEGS (review r5 B2: every 100th __gc raises, the loop's work
       and the engine's step each inside lua_pcall, with and without 20
       unprotected host allocations per loop): pristine and the derived
       build finish every run, never latch, and every raise surfaces from
       one of the protected calls; review-r5 must panic.
5. Script-control legs latched from their first loop (36,000 loops,
   LATCH_RUNS runs of pristine and of paced, RUNS of each review build),
   judged two-sided against pristine running the same script: the median
   of each JUDGED metric (peak heap, marking units per loop, GC ms/s, loop
   GC p99 and max, collection cycles) within max(3 %, pristine's
   run-to-run spread) of pristine's median.  The derived build must latch
   in every run, forget the state at lua_close and pass every leg:
     - SCHEDULE_LEGS (review r2 B3: stop, setstepmul 40, step) and
       RESTART_LEGS (review r3 B-R1): review-r1 must fail every one by the
       peak judge of r4 (its quotas cap the finalizer throughput); the
       test prints review-r4's regressions;
     - BIRTH_LEGS: a state latched at lua_newstate is pristine all along.
   Every judged run starts from a file of its own, all on one path length
   (work/run/NNN/RR/oracle.exe): the module path sits on the CRT heap ahead
   of Lua's first allocation, so its length moves every Lua object's
   address and with it the collisions and resizes of the pointer-keyed
   tables, and the package library keys the registry with the light
   userdata &CLIBS, a static address whose slot follows the image base
   Windows draws per file and boot.  On a chaotic leg one pristine binary
   run from two places gave worst loops 2.3x apart (LUA-GC-R5.md): with one
   file per build that is a bias between builds that no number of runs
   averages out, with one file per run it is part of the spread.
6. Exact legs (EXACT_LEGS, EXACT_SEEDS): pristine, the derived default and
   the EXACT_TEETH mutants are linked once more with lstate.c's string-hash
   seed taken from the oracle's seed=N alone and with /DYNAMICBASE:NO, and
   with seed=N the oracle leaves the package library closed (its registry
   key &CLIBS is a static address, different in every link), so a run is
   a function of collector code, seed and options (pristine twice on one seed
   gives one digest, and the seeds do not all give one digest).  The
   oracle folds the collector's state after every loop (gcstate, total
   bytes, GCdebt, GCestimate, gcfinnum, tobefnz length, the loop's
   finalizers and steps) into a digest: a state latched before its first
   paced step -- at lua_newstate, or by a setup control call after the
   setup's full collection -- must reproduce pristine's digest bit for bit
   on every seed, and each EXACT_TEETH mutant must miss its reference
   digest on some seed.
     The script-control legs that latch mid-run run here as x-<leg>, one
   run of pristine, paced and (latch legs) review-r4 per seed of
   EXACT_LATCH_SEEDS, paced judged against pristine on the same seed (on
   the exact builds a verdict cannot move with the work path, review r5
   claims B3):
     - LATCH_LEGS (review r4 B-R2 count-then-step and per-state restart /
       step, B-R4 setstepmul / setpause toggles, behaviour-round-2 m1 stop
       at high rates, m2 setpause toggles) and HANDOVER_LEGS (a no-op
       collectgarbage("setpause", 200) at the first loop in a given GC
       state with a given tobefnz backlog);
     - the latch follows the script's control call, at its loop, and
       lua_close forgets the state; on a count-gated leg (countstep /
       countcollect) and a hand-over leg the seeds judged are those whose
       paced run made the call (at least one; the paced heap need not reach
       a count threshold, and a script that only counts keeps the pacer);
     - EXACT_LATCH_BITWISE legs (latched before the paced run diverged)
       must give pristine's digest on every seed; on every leg each JUDGED
       metric of paced must be within the leg's LATCH_BOUNDS of pristine's
       on each judged seed, PRISTINE_TOL where it names none, two-sided.  A
       bound above PRISTINE_TOL is a measured cost of a mid-run latch, not
       pristine-equivalence (LUA-GC-R5-round2.md);
     - a hand-over leg must drain the backlog within HANDOVER_DRAIN loops
       of the mark, or no later than pristine does from the same mark on
       the same seed;
     - review-r4 must leave the bounds on every latch leg but cs11700-r10
       (R4_NEUTRAL: there c447d9e's script mostly never takes control); the
       test prints on how many it leaves them upwards.
7. Teeth of the rules (TEETH): each mutant must lose on a named leg -- by
   the two-sided judge, latching where it must not, not latching where it
   must, keeping a backlog after the hand-over, keeping a closed state
   latched, the pacing bound, peak, the worst loop, the share of loops over
   2 ms or a panic where paced and pristine finish.
8. The real CMake blocks of recomp/vita/CMakeLists.txt (retired knobs, knob
   validation, the isaac_lua53 source swap and its build rule), extracted
   verbatim and run under cmake -P: OFF leaves the 33-source list
   untouched; ON swaps exactly lgc.c and lbaselib.c, each in place, for
   <build>/lua53-gc-finalizer-pace/{lgc,lbaselib}.c holding the derived
   bytes; the build rule's command recreates either deleted file;
   out-of-range constants, PACE without ISAAC_VITA_LUA, non-default
   sub-knobs with PACE OFF and non-default retired knobs are refused;
   retired knobs at their old defaults are dropped; PACE with
   ISAAC_VITA_LUA_GCSTEP_CLAMP_KB other than 128 (the only clamp the matrix
   covers) configures with a warning.

usage: python recomp/test_vita_lua_gc_finalizer_pace.py
           [--lua-source DIR] [--cc CLANG] [--work DIR] [--jobs N]
           [--build-only]
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import tempfile

import test_vita_lua_runtime as R

ROOT = Path(__file__).resolve().parent
ORACLE = ROOT / "runtime" / "vita_lua_gc_finalizer_pace_oracle.c"
ORACLE_LGC = ROOT / "runtime" / "vita_lua_gc_finalizer_pace_oracle_lgc.c"
VITA_CMAKE = ROOT / "vita" / "CMakeLists.txt"
PACE_SCRIPT = ROOT / "vita" / "lua53_gc_finalizer_pace.py"
sys.path.insert(0, str(ROOT / "vita"))
import lua53_gc_finalizer_pace as pace  # noqa: E402

MAX, STEP, SWEEP_STEP = 8, 192, 6144     # the CMake defaults under test
# F-GCSWEEP (0.2.0): the sweep quota of box arm GW.  Not a default: the knob
# stays at 6144 until the device gate; this value only has to derive,
# configure and pass the allocator gate.  It is validated with
# ISAAC_VITA_LUA_GCSTEP_CLAMP_KB=64 only (the host sweep found a catch-up
# cliff at 2048 with 128; see gcsweep.report.md of the lane), and
# CMakeLists.txt refuses a quota below 3072 with CLAMP_KB 0 or above 64.
SWEEP_STEP_020 = 2048
DERIVED_SHA256 = (
    "34dd3a2d16975d0f1e59b1dcc20bedefa905d742b8907a2c675c7f7b6cd16516")
DERIVED_NOSWEEP_SHA256 = (
    "322e018b5d217292aea1bad90a7fb9767cd9384d8a2ab13504b3723203bde671")
BASELIB_SHA256 = (
    "70c64d2fc3d6e7f165e0688d565955d0cb6d2409e6533c2c4bdc21aaab8e9187")
EXPECTED_LINES = [39, 44, 45, 60, 61, 848, 909, 913, 958, 975, 1033, 1079,
                  1133, 1138, 1140, 1141, 1142]
BASELIB_LINES = [172, 182]
# review r0 (0f5324a): configure receipt "max=8 cost=512 sweep_cost=32
# sha256=2c6c2c0783eca292..."; its three derived lines, verbatim.
R0_SHA256 = "2c6c2c0783eca2925d55bed5fca3e29ec4de75ddafd5a67eea1379bc6ecd2508"
R0_LINES = {
    39: b"#define GCSWEEPCOST\t32  /* ISAAC_VITA_LUA_GC_SWEEP_COST (pristine: "
        b"(sizeof(TString) + 4) / 4 = 5) */\n",
    45: b"#define GCFINALIZECOST\t512  /* ISAAC_VITA_LUA_GC_FINALIZER_PACE "
        b"(pristine: GCSWEEPCOST) */\n",
    848: b"                    : (g->gcfinnum * 2 > 8u ? 8u : g->gcfinnum * 2);  "
         b"/* ISAAC_VITA_LUA_GC_FINALIZER_PACE cap (pristine: doubles) */\n",
}
# review r1 (eaec5e2): derive() defaults, configure receipt "max=8
# fin_step=192 sweep_step=6144 sha256=f48e5ce369c11287..."; its six derived
# lines, copied byte for byte from that derivation's output.
R1_SHA256 = "f48e5ce369c11287f2590166f4b62418aacc91ce245d1201bc746a10e8c8413e"
R1_LINES = {
    39: b'#define GCSWEEPCOST\tisaac_pace_cost(6144, 400)  /* ISAAC_VITA_LUA_GC_SWEEP_STEP (pristine: (sizeof(TString) + 4) / 4 = 5) */\n',
    44: b'static l_mem isaac_pace_budget;  static lu_mem isaac_pace_cost (l_mem quota, l_mem cap) { l_mem b = isaac_pace_budget < MAX_LMEM - GCSTEPSIZE ? isaac_pace_budget + GCSTEPSIZE : MAX_LMEM; l_mem c = b / quota, base = (l_mem)((sizeof(TString) + 4) / 4); return (lu_mem)(c < base ? base : c > cap ? cap : c); }  /* ISAAC_VITA_LUA_GC_FINALIZER_PACE quota charge (pristine: comment line) */\n',
    45: b'#define GCFINALIZECOST\tisaac_pace_cost(192, MAX_LMEM / 16)  /* ISAAC_VITA_LUA_GC_FINALIZER_STEP (pristine: GCSWEEPCOST) */\n',
    848: b'                    : (g->gcfinnum * 2 > 8u && g->gcstate >= GCSswpallgc && g->gcstate <= GCScallfin ? 8u : g->gcfinnum * 2);  /* ISAAC_VITA_LUA_GC_FINALIZER_MAX cap after atomic (pristine: doubles) */\n',
    1133: b'  isaac_pace_budget = debt;  do {  /* repeat until pause or enough "credit" (negative debt) */\n',
    1140: b"    debt = ((g->gcstate == GCScallfin && debt < -GCSTEPSIZE ? -GCSTEPSIZE : debt) / g->gcstepmul) * STEPMULADJ;  /* convert 'work units' to Kb; ISAAC_VITA_LUA_GC_FINALIZER_PACE: a finalizer step keeps at most GCSTEPSIZE of credit */\n",
}
# review r4 (c447d9e): configure receipt "max=8 fin_step=192 sweep_step=6144
# sha256=83d9b815ccaacc7d..."; its 21 derived lines, copied byte for byte
# from that derivation's output.  No latch: its lbaselib.c is pristine.
R4_SHA256 = "83d9b815ccaacc7d4b8d064fbdfe657c031ebf8425c6a8e6ede861372dace6f3"
R4_LINES = {
    39: b'#define GCSWEEPCOST\tisaac_pace_cost(6144, 400)  /* ISAAC_VITA_LUA_GC_SWEEP_STEP (pristine: (sizeof(TString) + 4) / 4 = 5) */\n',
    44: b'static l_mem isaac_pace_budget;  static lu_mem isaac_pace_cost (l_mem quota, l_mem cap) { l_mem b = isaac_pace_budget < MAX_LMEM - GCSTEPSIZE ? isaac_pace_budget + GCSTEPSIZE : MAX_LMEM; l_mem c = b / quota, base = (l_mem)((sizeof(TString) + 4) / 4); return (lu_mem)(isaac_pace_budget < 0 || c < base ? base : c > cap ? cap : c); }  /* ISAAC_VITA_LUA_GC_FINALIZER_PACE quota charge, budget -1 = pristine step (pristine: comment line) */\n',
    45: b'#define GCFINALIZECOST\tisaac_pace_cost(192, MAX_LMEM / 16)  /* ISAAC_VITA_LUA_GC_FINALIZER_STEP (pristine: GCSWEEPCOST) */\n',
    60: b'static const lu_mem isaac_pace_none = (lu_mem)MAX_LMEM + 1u;  static struct { const global_State *g; long long t; } isaac_pace_marks[4];  static unsigned isaac_pace_marks_next;  static int isaac_pace_slot (const global_State *g) { int i; for (i = 0; i < 4; i++) if (isaac_pace_marks[i].g == g) return i; return -1; }  static void isaac_pace_mark (global_State *g) { int i = isaac_pace_slot(g); if (i < 0) { i = (int)(isaac_pace_marks_next++ % 4u); isaac_pace_marks[i].g = g; } isaac_pace_marks[i].t = g->totalbytes; }  static int isaac_pace_restarted (const global_State *g) { int i = isaac_pace_slot(g); return i >= 0 && (isaac_pace_marks[i].t - (long long)g->totalbytes) % 1024 != 0; }  static l_mem isaac_pace_rd (const global_State *g) { lu_mem u = g->GCmemtrav; return u > (lu_mem)MAX_LMEM ? -(l_mem)(~u) - 1 : (l_mem)u; }  static void isaac_pace_wr (global_State *g, long long v) { g->GCmemtrav = (lu_mem)(l_mem)(v < -(long long)MAX_LMEM ? -MAX_LMEM : v > (long long)MAX_LMEM ? MAX_LMEM : v); }  static l_mem isaac_pace_thr (const global_State *g) { l_mem e = g->GCestimate / PAUSEADJ; if (e <= 0) e = 1; return (g->gcpause < MAX_LMEM / e) ? e * g->gcpause : MAX_LMEM; }  static long long isaac_pace_extra (const global_State *g) { return (long long)(g->GCmemtrav / g->gcstepmul) * STEPMULADJ; }  static long long isaac_pace_v (const global_State *g) { return g->gcstate == GCScallfin ? (long long)g->GCdebt + isaac_pace_rd(g) : (long long)gettotalbytes(g) - isaac_pace_thr(g) + isaac_pace_extra(g); }  static int isaac_pace_escape (const global_State *g) { return g->gcstate >= GCSswpallgc && g->gcstate <= GCScallfin && ((g->gcstate == GCScallfin && g->GCmemtrav == isaac_pace_none) || isaac_pace_v(g) > 0); }  static void isaac_pace_setdebt (global_State *g, l_mem debt) { l_mem old = g->GCdebt; luaE_setdebt(g, debt); isaac_pace_mark(g); if (g->gcstate == GCScallfin && g->GCmemtrav != isaac_pace_none) isaac_pace_wr(g, (long long)isaac_pace_rd(g) + old - g->GCdebt); }  static void isaac_pace_open (global_State *g) { isaac_pace_wr(g, (long long)g->totalbytes - isaac_pace_thr(g) + isaac_pace_extra(g)); }  static long long isaac_pace_vend (const global_State *g) { return g->GCmemtrav == isaac_pace_none ? -(long long)MAX_LMEM - 1 : (long long)g->GCdebt + isaac_pace_rd(g); }  static void isaac_pace_late (global_State *g, long long v) { l_mem tb = (l_mem)gettotalbytes(g); if (v > g->GCdebt) { luaE_setdebt(g, v < tb ? (l_mem)v : tb); isaac_pace_mark(g); } }  static lu_mem isaac_pace_swept (global_State *g, int n, lu_mem c) { lu_mem x = (lu_mem)n * (g->gcstate == GCSswptobefnz ? c : c - (lu_mem)((sizeof(TString) + 4) / 4)); g->GCmemtrav = g->GCmemtrav < (lu_mem)MAX_LMEM - x ? g->GCmemtrav + x : (lu_mem)MAX_LMEM; return (lu_mem)n * c; }  static void isaac_pace_cut (global_State *g, int r) { if ((!r && g->GCdebt > 0) || g->gcstate < GCSswpallgc || g->gcstate > GCScallfin) return; if (g->gcstate != GCScallfin) { long long d = (long long)isaac_pace_thr(g) - (long long)gettotalbytes(g); long long u = d < 0 ? 0 : (d / STEPMULADJ + 1) * g->gcstepmul; if (u > (long long)MAX_LMEM) u = MAX_LMEM; if ((lu_mem)u > g->GCmemtrav) g->GCmemtrav = (lu_mem)u; } else if (g->GCmemtrav != isaac_pace_none && isaac_pace_v(g) <= 0) isaac_pace_wr(g, 1 - (long long)g->GCdebt); }  /* ISAAC_VITA_LUA_GC_FINALIZER_PACE schedule (pristine: blank line) */\n',
    845: b'  for (i = 0; g->tobefnz && i < (g->gcfinnum > 8u && isaac_pace_budget >= 0 && g->gcstate >= GCSswpallgc && g->gcstate <= GCScallfin ? 8u : g->gcfinnum); i++)  /* ISAAC_VITA_LUA_GC_FINALIZER_MAX per call in a paced step after atomic, also for a gcfinnum an earlier pristine step doubled (pristine: i < g->gcfinnum) */\n',
    848: b'                    : (g->gcfinnum * 2 > 8u && isaac_pace_budget >= 0 && g->gcstate >= GCSswpallgc && g->gcstate <= GCScallfin ? 8u : g->gcfinnum * 2);  /* ISAAC_VITA_LUA_GC_FINALIZER_MAX cap in a paced step after atomic (pristine: doubles) */\n',
    909: b"        (void)sweeptolive;  /* ISAAC_VITA_LUA_GC_FINALIZER_PACE: 'sweepgc' moves to o's slot at the unlink below (pristine: sweeptolive) */\n",
    913: b"    *p = o->next;  if (g->sweepgc == &o->next) { g->sweepgc = p; }  /* remove 'o' from 'allgc' list; ISAAC_VITA_LUA_GC_FINALIZER_PACE: the sweep position follows it, nothing is freed here */\n",
    945: b'  luaE_setdebt(g, debt);  isaac_pace_mark(g);  g->GCmemtrav = isaac_pace_none;  /* ISAAC_VITA_LUA_GC_FINALIZER_PACE: no cycle offset */\n',
    958: b'  g->gcstate = GCSswpallgc;  g->GCmemtrav = 0;  /* ISAAC_VITA_LUA_GC_FINALIZER_PACE: extra sweep charge */\n',
    1033: b'      return isaac_pace_swept(g, GCSWEEPMAX, GCSWEEPCOST);  /* ISAAC_VITA_LUA_GC_FINALIZER_PACE (pristine: GCSWEEPMAX * GCSWEEPCOST) */\n',
    1079: b'      g->gcstate = GCScallfin;  isaac_pace_open(g);  /* ISAAC_VITA_LUA_GC_FINALIZER_PACE: schedule offset */\n',
    1128: b'  l_mem debt = getdebt(g), isaac_pace_saved = isaac_pace_budget, isaac_pace_mine;  /* GC deficit (be paid now) */\n',
    1129: b"  isaac_pace_cut(g, isaac_pace_restarted(g));  if (!g->gcrunning) {  /* not running? ISAAC_VITA_LUA_GC_FINALIZER_PACE: lua_gc(LUA_GCSTEP, 0) or a restart ends pristine's pause */\n",
    1130: b'    isaac_pace_setdebt(g, -GCSTEPSIZE * 10);  /* avoid being called too often; ISAAC_VITA_LUA_GC_FINALIZER_PACE: keeps V */\n',
    1133: b'  isaac_pace_budget = isaac_pace_mine = isaac_pace_escape(g) ? -1 : debt;  do {  /* repeat until pause or enough "credit" (negative debt); ISAAC_VITA_LUA_GC_FINALIZER_PACE: -1 = pristine step */\n',
    1135: b'    debt -= work;  isaac_pace_budget = isaac_pace_mine;\n',
    1138: b'    { long long isaac_pace_v0 = isaac_pace_vend(g); setpause(g); isaac_pace_late(g, isaac_pace_v0); }  /* pause until next cycle, ISAAC_VITA_LUA_GC_FINALIZER_PACE: no later than the schedule */\n',
    1140: b"    debt = (debt / g->gcstepmul) * STEPMULADJ;  if (debt < -GCSTEPSIZE && isaac_pace_budget >= 0 && g->gcstate == GCScallfin) debt = -GCSTEPSIZE;  /* convert 'work units' to Kb; ISAAC_VITA_LUA_GC_FINALIZER_PACE: a paced finalizer step keeps at most GCSTEPSIZE bytes of credit */\n",
    1141: b'    isaac_pace_setdebt(g, debt);\n',
    1143: b'  }  isaac_pace_budget = isaac_pace_saved;\n',
}
# review r5 (0474604): configure receipt "max=8 fin_step=192 sweep_step=6144
# sha256=fa2b23ce42d57998..."; it differs from the derivation under test in
# two lines only, copied byte for byte from that derivation's output: 1130
# kept an unlatched state's debt in the stopped path (review r5 B1/B2: an
# allocating __gc's tail chained paced steps into one loop, and a raising
# __gc's unwound step left a debt that the next unprotected allocation
# stepped on), and 1142 was pristine.  No other line moved, so it is the
# derivation under test with those two lines, checked against its receipt.
R5_SHA256 = "fa2b23ce42d57998067dbec03b253e26239f0e0ffacbbeb37816b7be8f72e25e"
R5_LINES = {
    1130: b"    if (isaac_lua_gc_scripted(g)) luaE_setdebt(g, -GCSTEPSIZE * 10);  /* avoid being called too often; ISAAC_VITA_LUA_GC_FINALIZER_PACE: only in a latched state -- an unlatched state stops only around a __gc, where this credit would hold off the next paced step */\n",
    1142: b"    runafewfinalizers(L);\n",
}
LIVE_KB = 11300   # EID heap (P2b ph120.mem lua(l) 11223..11674 KiB)
GARB = 35
LPS = 60          # LuaEngine::Update steps per second on the console
MATRIX_FRAMES = 30 * 60 * LPS
LEG_FRAMES = 10 * 60 * LPS
RUNS = 5
# pristine and paced runs of every script-control leg: several are bimodal in
# pristine itself (smtoggle, countstep, stop with 3 steps per loop at 1000
# per loop: p99 35.7 ms in 8 of 15 runs, 289.7 in 7), and 5 runs can miss a
# mode that 9 catch (host screens in LUA-GC-R5.md)
LATCH_RUNS = 9
# count-gated controls (`if collectgarbage("count") > A then latchmark() ...`):
# the script takes control only in runs whose heap passes A
GATED_CTL = ("ctl=countstep", "ctl=countcollect")
# latch legs review-r4 need not fail: at 10 per loop c447d9e's paced heap,
# like r5's before a latch, mostly stays below 11,700 KB, so the script never
# steps and both run the pacer for a count-only script, as the policy says
R4_NEUTRAL = ("cs11700-r10",)
ABORT = "abort_kb=262144"   # a diverging build stops at 256 MB
K_TODAY, K_R0 = 128, 40
RUN_TIMEOUT = 900           # s; a hanging mutant is a result, not a stall
HANG_TIMEOUT = 120
# every oracle runs from a path of this many characters, with this
# environment, whatever --work and the caller's environment are (module
# docstring: both sit on the CRT heap ahead of Lua's first allocation)
RUN_PATH_LEN = 160
RUN_ENV = {"SystemRoot": os.environ.get("SystemRoot", "C:\\Windows")}

# semantics/bound legs, every build: name, K, finalizers/loop, loops, options
SEMANTIC_LEGS = (
    ("k128-r10", 128, 10, 4000, ()),
    ("k40-r30", 40, 30, 4000, ()),
    ("k128-r100", 128, 100, 3000, ()),
    ("k128-burst", 128, 10, 3000, ("burst=600,300,70",)),
)
# review-r1 matrix legs: name, finalizers/loop, options, kind
MATRIX_LEGS = (
    ("r10", 10, (), "normal"),
    ("burst30", 10, ("burst=1800,840,70",), "normal"),
    ("burst60", 10, ("burst=3600,840,70",), "normal"),
    ("spike8328", 10, ("spike=600,8328",), "spike"),
    ("r100", 100, (), "rate"),
    ("r120", 120, (), "rate"),
    ("r200", 200, (), "rate"),
    ("cap33-r100", 100, ("cap_kb=33792",), "cap"),
    ("cap33-r200", 200, ("cap_kb=33792",), "cap"),
    ("cap25-r100", 100, ("cap_kb=25600",), "cap"),
    ("cap25-r200", 200, ("cap_kb=25600",), "cap"),
    ("em11648-r10", 10, ("cap_kb=11648",), "emergency"),
    ("em13312-r100", 100, ("cap_kb=13312",), "emergency"),
)
# em11648 only sometimes reaches its cap (Lua's string-hash seed is
# time/ASLR-derived, so heaps differ by a few hundred KB run to run);
# em13312-r100 refused in every one of 12 host runs per build
EMERGENCY_REFUSALS_LEG = "em13312-r100"
PEAK_RATIO, LAG_RATIO, FRAME_RATIO = 1.15, 2.0, 0.5
SHARE_SLACK = 3.0   # points of loops over 2 ms of GC above review-r1's

# Leg tuples below: (name, finalizers/loop, garbage tables/loop, options).
# ctl=NAME[:A[,B]] is a Lua control script of the oracle (s_ctls): every
# collectgarbage() in it goes through the base library, as a mod's does.
#
# engine legs: no control call the latch may see
ENGINE_LEGS = (
    ("r600", 600, GARB, ()),
    ("r1000", 1000, GARB, ()),
    ("count-r200", 200, GARB, ("ctl=count",)),
    ("count-r600", 600, GARB, ("ctl=count",)),
    ("isrunning-r200", 200, GARB, ("ctl=isrunning",)),
    ("engcollect-r600", 600, GARB, ("collect=600",)),
)
ENGINE_PEAK_RATIO = 1.05
# engine legs whose C __gc allocates (review r5 behaviour B1: in r5 the
# tail's allocations chained paced steps into one loop; the reviewers'
# finbig 10/4096 is milder in this oracle, where r5 keeps pristine's worst
# loop, so the legs are the ones r5 loses here: a 16 KB string per __gc at
# 10 per loop, a 4 KB one at 50, 50 tables per __gc at 50): the derived
# build must keep pristine's worst loop and p99 (median within
# max(PRISTINE_TOL, pristine's spread)) and its peak (ENGINE_PEAK_RATIO),
# never latch; review-r5 must lose on the worst loop or the p99 on each
# ALLOC_TEETH leg
ALLOC_LEGS = (
    ("fs16384-r10", 10, GARB, ("fin_str=16384",)),
    ("fs4096-r50", 50, GARB, ("fin_str=4096",)),
    ("fa50-r50", 50, GARB, ("fin_alloc=50",)),
)
# the legs where review-r5 must lose.  Not fa50-r50: pristine's worst loop
# there is trimodal across runs (150 / 281 / 429 ms, peak 39 / 55 / 72 MB
# in one five-run set), its median sits above r5's 204 ms, so r5 cannot be
# shown to lose by a median; the leg still judges the derived build
ALLOC_TEETH = ("fs16384-r10", "fs4096-r50")
# a raising __gc with the loop's work and the engine's step each inside
# lua_pcall (err=P; review r5 behaviour B2): every run of pristine and the
# derived build finishes (no panic) and never latches; review-r5 and
# mutant-r5-stopped panic -- lua_pushcfunction's own luaC_checkGC, outside
# the pcall, steps on the debt an unwound step left (hostpush=N adds N
# unprotected allocations per loop).  Not here: err=7 and err=20, where
# pristine itself panics or passes 256 MB (each engine step dies on its
# first raising __gc and the backlog only grows)
ERROR_LEGS = (
    ("gcerr100-r100", 100, GARB, ("err=100",)),
    ("gcerr100-h20-r100", 100, GARB, ("err=100", "hostpush=20")),
)
# review-r2 B3: collectgarbage("stop"), ("setstepmul", 40), ("step") x3/loop
SCHEDULE_LEGS = (
    ("stop-r200", 200, GARB, ("ctl=stop",)),
    ("stop-r250", 250, GARB, ("ctl=stop",)),
    ("stop-r400", 400, GARB, ("ctl=stop",)),
    ("sm40-r150", 150, GARB, ("ctl=stepmul:40",)),
    ("sm40-r300", 300, GARB, ("ctl=stepmul:40",)),
    ("sm40-r1000", 1000, GARB, ("ctl=stepmul:40",)),
    ("s0x3-r1000", 1000, GARB, ("ctl=luasteps:3",)),
    ("stop-s0x3-r1000", 1000, GARB, ("ctl=stop", "ctl=luasteps:3")),
    ("sm40-s0x3-r1000", 1000, GARB, ("ctl=stepmul:40", "ctl=luasteps:3")),
)
# review-r3 B-R1: collectgarbage("restart") every loop or every 10th, stop
# around each loop's work, restart then stop at once
RESTART_LEGS = (
    ("restart-r600", 600, GARB, ("ctl=restart:1",)),
    ("restart-r1000", 1000, GARB, ("ctl=restart:1",)),
    ("restart10-r600", 600, GARB, ("ctl=restart:10",)),
    ("sr-r200", 200, GARB, ("ctl=stoprestart",)),
    ("sr-r600", 600, GARB, ("ctl=stoprestart",)),
    ("sr-sm40-r200", 200, GARB, ("ctl=stoprestart", "ctl=stepmul:40")),
    ("rs-r600", 600, GARB, ("ctl=restartstop",)),
)
# review r4 B-R2 / B-R4 and behaviour round 2: each one review-r4 (c447d9e)
# loses to pristine on the host-oracle screen (19 candidates x 5 runs,
# LUA-GC-R5.md); left out: hyst13000-r200, sr-r184-g0 and sm10000-r1000-g0
# (chaotic or bimodal: r5 passes all three at 9 runs a build on the host
# screen, but sm10000's worst loop spans 1.3-3.2 s in pristine alone, so a
# leg here would need far more runs than the others) and sr-r1200-g0
# (review-r4 passes it)
LATCH_LEGS = (
    # B-R2: collectgarbage("restart") / ("step") once per cycle in one state
    ("rs2-r10", 10, GARB, ("ctl=restartstate:2",)),
    ("rs6-r200", 200, GARB, ("ctl=restartstate:6",)),
    ("s0s2-r10", 10, GARB, ("ctl=step0state:2",)),
    ("s0s6-r200", 200, GARB, ("ctl=step0state:6",)),
    # B-R2: collectgarbage("step") once the heap passes a count
    ("cs11700-r10", 10, GARB, ("ctl=countstep:11700",)),
    ("cs13000-r200", 200, GARB, ("ctl=countstep:13000",)),
    ("cs14000-r400", 400, GARB, ("ctl=countstep:14000",)),
    # B-R4: setstepmul toggled every loop / every 600 loops
    ("smt1000-r10", 10, GARB, ("ctl=smtoggle:1000",)),
    ("smt1000-r200", 200, GARB, ("ctl=smtoggle:1000",)),
    ("smper-r200", 200, GARB, ("ctl=smper:1000,600",)),
    # behaviour round 2 m2: setpause toggled every loop / every 300 loops
    ("pt100-r200", 200, GARB, ("ctl=pausetoggle:100",)),
    ("pslow-r200", 200, GARB, ("ctl=pauseslow:1000",)),
    # behaviour round 2 m1: collectgarbage("stop") at high rates, with and
    # without per-loop garbage
    ("stop-r600", 600, GARB, ("ctl=stop",)),
    ("stop-r2000", 2000, GARB, ("ctl=stop",)),
    ("stop-r1000-g0", 1000, 0, ("ctl=stop",)),
)
# ctl=latch:S,B: a no-op collectgarbage("setpause", 200) at the first loop
# start in GC state S with at least B objects in tobefnz.  Every state a
# script can see at a loop start, backlogs from 0 to 134,000; left out (host
# screens in LUA-GC-R5.md): GCSatomic, which a loop start shows about once
# in 10^4 cycles (polled after each allocation it latched in 3 of 7 runs),
# GCSswpend, which no Lua code ever sees (entering it returns no work, so the
# step loop runs it at once)
HANDOVER_LEGS = (
    ("ho-s6b1-r5", 5, GARB, ("ctl=latch:6,1",)),
    ("ho-s0b0-r200", 200, GARB, ("ctl=latch:0,0",)),
    ("ho-s2b1000-r200", 200, GARB, ("ctl=latch:2,1000",)),
    ("ho-s3b1000-r200", 200, GARB, ("ctl=latch:3,1000",)),
    ("ho-s4b1000-r200", 200, GARB, ("ctl=latch:4,1000",)),
    ("ho-s6b1000-r200", 200, GARB, ("ctl=latch:6,1000",)),
    ("ho-s6b100-r200", 200, GARB, ("ctl=latch:6,100",)),
    ("ho-s7b0-r200", 200, GARB, ("ctl=latch:7,0",)),
    ("ho-s2b1-r1", 1, GARB, ("ctl=latch:2,1",)),
    ("ho-s2b500-r10", 10, GARB, ("ctl=latch:2,500",)),
    ("ho-s6b500-r10", 10, GARB, ("ctl=latch:6,500",)),
    ("ho-s3b1000-r1000", 1000, GARB, ("ctl=latch:3,1000",)),
    ("ho-s6b1000-r1000", 1000, GARB, ("ctl=latch:6,1000",)),
)
BIRTH_LEGS = (
    ("birth-r200", 200, GARB, ("birth=1",)),
    ("birth-r1000", 1000, GARB, ("birth=1",)),
)
# exact legs, on the EXACT builds (seed=N is the whole string-hash seed, no
# address randomization): latched before the first paced step, so every
# loop's collector state must equal pristine's on every seed.  ctl=stepmul,
# ctl=stop and ctl=setpause latch in the setup, after its full collection
# (the engine's lua_gc(LUA_GCCOLLECT)), and the paced setup must have left
# pristine's state there (any difference shows as a different digest); the
# fin_alloc legs run allocating __gc (lines 1130 and 1142)
EXACT_SEEDS = (1, 2, 3)
EXACT_LEGS = (
    ("x-birth-r200", 200, GARB, ("birth=1",)),
    ("x-birth-r1000", 1000, GARB, ("birth=1",)),
    ("x-sm10000-r1000-g0", 1000, 0, ("ctl=stepmul:10000",)),
    ("x-sp200-r1000", 1000, GARB, ("ctl=setpause:200",)),
    ("x-birth-fa4-r200", 200, GARB, ("birth=1", "fin_alloc=4")),
    ("x-stop-fa4-r200", 200, GARB, ("ctl=stop", "fin_alloc=4")),
)
# LATCH_LEGS and HANDOVER_LEGS run on the exact builds too, one run per seed
# of EXACT_LATCH_SEEDS, as "x-<leg>", pristine against paced on the same
# seed (review r5 claims B2/B3: in normal mode the verdict on a mid-run
# latch moved with the work path).  Those that latch before the paced
# prefix has diverged must be bit-exact (EXACT_LATCH_BITWISE); every other
# one is judged per seed on each JUDGED metric, |paced / pristine - 1| at
# most the leg's bound in LATCH_BOUNDS, PRISTINE_TOL where it names none.
# A bound above PRISTINE_TOL is a measured cost of a mid-run latch (the
# collector runs pristine's code from the heap the pacer left), written
# down so that it cannot grow silently.  The count-gated and hand-over legs are
# judged on the seeds whose paced run made the control call.
EXACT_LATCH_SEEDS = (1, 2, 3, 4)
# Bit-exact on seeds 1-4 (host screen, LUA-GC-R5-round2.md): the script's
# first control call comes before the paced run left pristine's.  Not here:
# pslow-r200 (it latches in loop 0 after that loop's work: pristine's digest
# on 3 of 4 seeds, every JUDGED metric equal on all 4), and every latch at a
# later loop.
EXACT_LATCH_BITWISE = ("smt1000-r10", "smt1000-r200", "smper-r200",
                       "stop-r600", "stop-r2000",
                       "stop-r1000-g0", "cs13000-r200", "cs14000-r400",
                       "ho-s0b0-r200", "ho-s7b0-r200")
# The measured cost of a mid-run latch where it exceeds PRISTINE_TOL [M,
# seeds 1-4, paced against pristine on the same seed, the largest |deviation|
# rounded up]: the worst loop after a hand-over that leaves the pristine code
# a long finalizer backlog in the sweep or callfin states (+3.9 % .. +11.4 %;
# [E] pristine's doubling then runs a backlog pristine itself never holds),
# and at 1000 finalizers per loop with 130,817 pending in GCScallfin a mode
# switch: pristine itself is bimodal across seeds there (peak 38,087 KB /
# worst 813 ms on seeds 1, 2, 4; 43,700 KB / 1,245 ms on seed 3) and the
# latched run lands in the high mode on every seed (44,612 KB / 1,213 ms),
# +17.1 % peak and +49.1 % worst against the same seed.  Every other leg and
# metric stays within PRISTINE_TOL.
LATCH_BOUNDS: dict[str, dict[str, float]] = {
    "ho-s6b1-r5": {"frame_ms_max": 0.08},           # +7.41 %
    "ho-s3b1000-r200": {"frame_ms_max": 0.12},      # +11.36 %
    "ho-s4b1000-r200": {"frame_ms_max": 0.05},      # +4.23 %
    "ho-s6b500-r10": {"frame_ms_max": 0.045},       # +3.85 %
    "ho-s3b1000-r1000": {"frame_ms_max": 0.08},     # +7.32 %
    "ho-s6b1000-r1000": {"peak_kb": 0.18,           # +17.13 %
                         "frame_ms_max": 0.50},     # +49.09 %
}
# each must miss its reference digest (pristine's, or paced's where paced
# itself is not bit-exact) on some seed of its leg: the late rule acting in a
# latched state, a latched state kept paced while it has a backlog, the
# script's setstepmul never latching (birth=1 latches through the oracle,
# not lbaselib.c, so latch-never needs the script leg), each latched-state
# guard dropped, and the latch after the option's own lua_gc
EXACT_TEETH = (
    ("mutant-vend-latched", "x-birth-r1000", "pristine"),
    ("mutant-keep-backlog", "x-birth-r200", "pristine"),
    ("mutant-latch-never", "x-sm10000-r1000-g0", "pristine"),
    ("mutant-guard-44", "x-sm10000-r1000-g0", "pristine"),
    ("mutant-guard-848", "x-birth-r200", "pristine"),
    ("mutant-guard-909", "x-birth-r200", "pristine"),
    ("mutant-guard-1140", "x-stop-r600", "pristine"),
    ("mutant-guard-1142", "x-birth-fa4-r200", "pristine"),
    ("mutant-latch-after", "x-s0s6-r200", "paced"),
)
EXACT_RE = re.compile(r"^  exact seed=(\d+) digest=([0-9a-f]{16})$", re.M)
# two-sided judge: each metric's median within max(PRISTINE_TOL, pristine's
# (max - min) / median over its runs) of pristine's median; values below the
# floor in both builds are equal
PRISTINE_TOL = 0.03
JUDGED = (("peak_kb", 64.0), ("mark_units_per_frame", 64.0),
          ("gc_ms_per_s", 0.05), ("frame_ms_p99", 0.05),
          ("frame_ms_max", 0.05), ("cycles", 1.0))

KV = re.compile(r"([A-Za-z_0-9()]+)=(-?[0-9.]+)")
VIOLATION = re.compile(
    r"violations early=(\d+) weak=(\d+) order=(\d+) twice=(\d+) never=(\d+) "
    r"keys=(\d+) bound=(\d+) order_slack=(\d+)")
COVERAGE = re.compile(
    r"coverage double=(\d+) cap=(\d+) drain=(\d+) callfin=(\d+) tail=(\d+) "
    r"quota=(\d+) alloc_fin=(\d+) over_bound0=(\d+)")
SWEEP_RE = re.compile(r"sweep step=(\d+) steps=(\d+) per_cycle=([0-9.]+)")
PRISTINE_RE = re.compile(r"pristine steps=(\d+) max_step_fin=(\d+) "
                       r"budget_mismatch=(\d+)")
LATCH_RE = re.compile(
    r"latch build=(\d) scripted=(\d) frame=(-?\d+) steps=(\d+) "
    r"forgotten=(-?\d+) birth=(\d) mark=(-?\d+) state=(\S+) "
    r"backlog=(-?\d+) drain=(-?\d+) post_peak_kb=(\d+) "
    r"post_frame_ms_max=([0-9.]+) pre_frame_ms_max=([0-9.]+)")
HELPERS_RE = re.compile(r"^helpers checks=(\d+) bad=(\d+)$", re.M)
ERROR_LINE = re.compile(r"^error-case .*$", re.M)
# builds that must panic in the oracle's __gc error case (review r5 B2)
ERROR_CASE_PANICS = ("review-r5", "mutant-r5-stopped")


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


def need(ok: bool, message: str) -> None:
    if not ok:
        fail(message)


def lines_of(data: bytes) -> list[bytes]:
    return data.splitlines(keepends=True)


def changed_lines(old: bytes, new: bytes) -> list[int]:
    a, b = lines_of(old), lines_of(new)
    need(len(a) == len(b), "derivation changed the line count")
    return [i + 1 for i, (x, y) in enumerate(zip(a, b)) if x != y]


def pinned_derive(pristine: bytes, lines: dict[int, bytes], sha: str,
                  name: str) -> bytes:
    text = lines_of(pristine)
    for line, content in lines.items():
        text[line - 1] = content
    data = b"".join(text)
    need(hashlib.sha256(data).hexdigest() == sha,
         f"{name} lgc.c does not reproduce its receipt sha")
    return data


def r0_derive(pristine: bytes) -> bytes:
    return pinned_derive(pristine, R0_LINES, R0_SHA256, "review-r0")


def r1_derive(pristine: bytes) -> bytes:
    return pinned_derive(pristine, R1_LINES, R1_SHA256, "review-r1")


def r4_derive(pristine: bytes) -> bytes:
    return pinned_derive(pristine, R4_LINES, R4_SHA256, "review-r4")


def r5_derive(pristine: bytes) -> bytes:
    return pinned_derive(pace.derive(pristine), R5_LINES, R5_SHA256,
                         "review-r5")


def edit(text: bytes, line: int, old: bytes, new: bytes) -> bytes:
    """Replace the one occurrence of `old` on 1-based `line` of `text`."""
    rows = lines_of(text)
    need(rows[line - 1].count(old) == 1,
         f"mutant anchor not unique on line {line}: {old!r}")
    rows[line - 1] = rows[line - 1].replace(old, new)
    return b"".join(rows)


def restore(text: bytes, pristine: bytes, *lines: int) -> bytes:
    rows, base = lines_of(text), lines_of(pristine)
    for line in lines:
        rows[line - 1] = base[line - 1]
    return b"".join(rows)


_PRED = (b" && isaac_pace_budget >= 0 && g->gcstate >= GCSswpallgc && "
         b"g->gcstate <= GCScallfin")
_CLAMP = (b"if (debt < -GCSTEPSIZE && isaac_pace_budget >= 0 && "
          b"g->gcstate == GCScallfin) debt = -GCSTEPSIZE;")
_BUDGET = b"isaac_pace_budget = isaac_lua_gc_scripted(g) ? -1 : debt;"
_QUERIES = (b"if (what == LUA_GCCOUNT || what == LUA_GCCOUNTB || "
            b"what == LUA_GCISRUNNING) return; ")
_FULLGC = 1157    # "  global_State *g = G(L);" of luaC_fullgc


def mutants(derived: bytes, pristine: bytes, baselib: bytes,
            pristine_baselib: bytes
            ) -> dict[str, tuple[bytes, bytes, list[str]]]:
    """name: (lgc.c bytes, lbaselib.c bytes, extra oracle defines); each
    undoes or moves one rule.  Not here, because no reachable state tells
    them from the derivation: the r4 placement of the credit clamp before
    the conversion to bytes (equal at stepmul 200, and only a script --
    which latches, and a latched state has no clamp -- changes stepmul),
    and line 845's per-call cap (atomic sets gcfinnum to 1 and line 848
    keeps it <= MAX in every paced step past atomic)."""
    lgc = {
        # line 45 charging the pristine 5 per finalizer: no finalizer quota
        "mutant-fin-charge5": edit(
            derived, 45, b"isaac_pace_cost(192, MAX_LMEM / 16)",
            b"((sizeof(TString) + 4) / 4)"),
        # line 45 back to `GCSWEEPCOST`: finalizers pay the sweep quota's
        # charge ((budget + 1600) / 6144), so a step runs ~6,144 of them
        "mutant-line45-pristine": restore(derived, pristine, 45),
        # line 848 pristine: gcfinnum doubles without a cap
        "mutant-no-cap": restore(derived, pristine, 848),
        # the cap in every state of a paced step, not only after atomic
        "mutant-cap-everywhere": edit(derived, 848, _PRED,
                                      b" && isaac_pace_budget >= 0"),
        # line 1133 charging every step as if its budget were 0
        "mutant-no-budget": edit(derived, 1133, _BUDGET,
                                 b"isaac_pace_budget = isaac_lua_gc_scripted(g)"
                                 b" ? -1 : 0;"),
        # line 1142 pristine: the tail finalizers' allocations reach the
        # stopped path (line 1130), whose 10 x GCSTEPSIZE bytes of credit
        # then hold off the next paced step
        "mutant-no-restore": restore(derived, pristine, 1142),
        # line 1130 as review r5 had it: an unlatched state keeps its debt
        # in the stopped path, so after a raising __gc unwinds a step the
        # next unprotected allocation steps (and runs the next raising
        # __gc outside any protected call)
        "mutant-r5-stopped": edit(
            derived, 1130, b"    luaE_setdebt(g, -GCSTEPSIZE * 10);",
            b"    if (isaac_lua_gc_scripted(g)) luaE_setdebt(g, "
            b"-GCSTEPSIZE * 10);"),
        # the latched-state guards, one each: the paced rule acting in a
        # latched state (exact legs: the digest leaves pristine's)
        "mutant-guard-44": edit(derived, 44, b"isaac_pace_budget < 0 || ", b""),
        "mutant-guard-848": edit(derived, 848, b"isaac_pace_budget >= 0 && ",
                                 b""),
        "mutant-guard-909": edit(derived, 909,
                                 b"if (isaac_lua_gc_scripted(g)) ",
                                 b"if (0) "),
        "mutant-guard-1140": edit(derived, 1140, b"isaac_pace_budget >= 0 && ",
                                  b""),
        "mutant-guard-1142": edit(derived, 1142,
                                  b"if (isaac_pace_budget >= 0) luaE_setdebt",
                                  b"luaE_setdebt"),
        # line 1141 back to a bare luaE_setdebt: the step's own setdebt no
        # longer keeps V in GCScallfin
        "mutant-no-keepv-step": restore(derived, pristine, 1141),
        # line 1138 pristine: the pause after a late cycle is not shortened
        "mutant-no-late": restore(derived, pristine, 1138),
        # line 1140 pristine: no credit clamp in a paced GCScallfin step
        "mutant-no-clamp": restore(derived, pristine, 1140),
        # lines 909 / 913 pristine: luaC_checkfinalizer calls sweeptolive
        "mutant-no-cf": restore(derived, pristine, 909, 913),
        # line 60: sweep steps book no extra units into V
        "mutant-no-book": edit(
            derived, 60,
            b"g->GCmemtrav = g->GCmemtrav < (lu_mem)MAX_LMEM - x ? "
            b"g->GCmemtrav + x : (lu_mem)MAX_LMEM; return",
            b"(void)x; return"),
        # the latch also set by the engine's own lua_gc(LUA_GCCOLLECT)
        # (luaC_fullgc)
        "mutant-latch-engine": edit(
            derived, _FULLGC, b"  global_State *g = G(L);\n",
            b"  global_State *g = G(L);  "
            b"isaac_lua_gc_script_control(L, LUA_GCCOLLECT);\n"),
        # collectgarbage("count") and ("isrunning") latch too
        "mutant-latch-on-count": edit(derived, 61, _QUERIES, b""),
        # collectgarbage("step") does not latch (one option ignored)
        "mutant-latch-ignores-step": edit(
            derived, 61, b"what == LUA_GCISRUNNING) return;",
            b"what == LUA_GCISRUNNING || what == LUA_GCSTEP) return;"),
        # collectgarbage("setstepmul") does not latch (one option ignored)
        "mutant-latch-ignores-setstepmul": edit(
            derived, 61, b"what == LUA_GCISRUNNING) return;",
            b"what == LUA_GCISRUNNING || what == LUA_GCSETSTEPMUL) return;"),
        # the hand-over keeps the paced backlog: a latched state stays paced
        # until tobefnz is empty
        "mutant-keep-backlog": edit(
            derived, 1133, _BUDGET,
            b"isaac_pace_budget = isaac_lua_gc_scripted(g) && !g->tobefnz ? "
            b"-1 : debt;"),
        # line 975 pristine: lua_close keeps the state latched, so a new
        # state at the same address starts latched
        "mutant-no-forget": restore(derived, pristine, 975),
    }
    found = {name: (text, baselib, []) for name, text in lgc.items()}
    # the derived lgc.c with the pristine lbaselib.c: the latch is never set
    found["mutant-latch-never"] = (derived, pristine_baselib, [])
    # lbaselib.c:182 latching after lua_gc: the call's own step or
    # collection is a paced one
    found["mutant-latch-after"] = (derived, edit(
        baselib, 182,
        b"int res = (isaac_lua_gc_script_control(L, o), lua_gc(L, o, ex));",
        b"int res = lua_gc(L, o, ex); isaac_lua_gc_script_control(L, o);"),
        [])
    return found


def helper_mutants(derived: bytes) -> dict[str, bytes]:
    """line-60/61 boundary mutants, judged by the oracle's helpers check
    alone (their workloads are not run): each moves one boundary."""
    def line(n: int, old: bytes, new: bytes) -> bytes:
        return edit(derived, n, old, new)
    return {
        "mutant-late-noclip": line(
            60, b"luaE_setdebt(g, v < tb ? (l_mem)v : tb);",
            b"luaE_setdebt(g, (l_mem)v);"),
        "mutant-no-keepv-helper": line(
            60, b"if (g->gcstate == GCScallfin) isaac_pace_wr(g, (long long)"
                b"isaac_pace_rd(g) + old - g->GCdebt);",
            b"(void)old;"),
        "mutant-keepv-everywhere": line(
            60, b"if (g->gcstate == GCScallfin) isaac_pace_wr(",
            b"isaac_pace_wr("),
        "mutant-swept-full": line(
            60, b"c : c - (lu_mem)((sizeof(TString) + 4) / 4));", b"c : c);"),
        "mutant-open-no-extra": line(
            60, b"- isaac_pace_thr(g) + isaac_pace_extra(g)); }",
            b"- isaac_pace_thr(g)); }"),
        "mutant-vend-latched": line(
            60, b"return isaac_lua_gc_scripted(g) ? -(long long)MAX_LMEM - 1 "
                b": (long long)g->GCdebt",
            b"return (long long)g->GCdebt"),
        "mutant-latch-countb": line(
            61, b"what == LUA_GCCOUNT || what == LUA_GCCOUNTB || ",
            b"what == LUA_GCCOUNT || "),
        "mutant-latch-keeps-budget": line(61, b"isaac_pace_budget = -1; ",
                                          b""),
        "mutant-latch-shared": line(
            61, b"if (isaac_pace_scripts[i] == g) return 1;",
            b"if (isaac_pace_scripts[i] != NULL) return 1;"),
        "mutant-latch-3slots": line(
            61, b"for (i = 0; i < 4; i++) if (isaac_pace_scripts[i] == NULL)",
            b"for (i = 0; i < 3; i++) if (isaac_pace_scripts[i] == NULL)"),
        "mutant-latch-no-overflow": line(61, b"isaac_pace_scripts_all = 1; }",
                                         b"}"),
    }


def derivation_tests(pristine: bytes, pristine_baselib: bytes,
                     work: Path) -> None:
    need(hashlib.sha256(pristine).hexdigest() == pace.PRISTINE_LGC_SHA256,
         "PRISTINE_LGC_SHA256 differs from the manifest-verified lgc.c")
    need(hashlib.sha256(pristine_baselib).hexdigest() ==
         pace.PRISTINE_LBASELIB_SHA256,
         "PRISTINE_LBASELIB_SHA256 differs from the manifest-verified "
         "lbaselib.c")
    old = lines_of(pristine)
    for line, content in pace.PRISTINE_LINES.items():
        need(old[line - 1] == content, f"pristine anchor moved: line {line}")
    old_bl = lines_of(pristine_baselib)
    for line, content in pace.BASELIB_PRISTINE_LINES.items():
        need(old_bl[line - 1] == content,
             f"pristine lbaselib.c anchor moved: line {line}")
    need((pace.MAX_DEFAULT, pace.FIN_STEP_DEFAULT, pace.SWEEP_STEP_DEFAULT) ==
         (MAX, STEP, SWEEP_STEP), "derivation defaults changed")
    derived = pace.derive(pristine)
    need(derived == pace.derive(pristine, MAX, STEP, SWEEP_STEP),
         "derive() defaults are not MAX/STEP/SWEEP_STEP")
    need(hashlib.sha256(derived).hexdigest() == DERIVED_SHA256,
         f"derived lgc.c sha changed: {hashlib.sha256(derived).hexdigest()}")
    changed = changed_lines(pristine, derived)
    need(changed == EXPECTED_LINES, f"derivation changed lines {changed}")
    need(sorted(pace.CHANGED_LINES + (pace.SWEEP_LINE,)) == EXPECTED_LINES,
         f"pace.CHANGED_LINES {pace.CHANGED_LINES} != the test's list")
    baselib = pace.derive_baselib(pristine_baselib)
    need(hashlib.sha256(baselib).hexdigest() == BASELIB_SHA256,
         f"derived lbaselib.c sha changed: "
         f"{hashlib.sha256(baselib).hexdigest()}")
    need(changed_lines(pristine_baselib, baselib) == BASELIB_LINES and
         list(pace.BASELIB_CHANGED_LINES) == BASELIB_LINES,
         "the lbaselib.c derivation changed other lines")
    new_bl = lines_of(baselib)
    need(new_bl[171].startswith(b"LUAI_FUNC void isaac_lua_gc_script_control "
                                b"(lua_State *L, int what);"),
         f"lbaselib.c line 172: {new_bl[171]!r}")
    need(new_bl[181].startswith(
        b"  int res = (isaac_lua_gc_script_control(L, o), lua_gc(L, o, ex));"),
         f"lbaselib.c line 182: {new_bl[181]!r}")
    # the latch call sits in luaB_collectgarbage, the one function of the
    # base library that reaches lua_gc, and nowhere else
    need(baselib.count(b"isaac_lua_gc_script_control") == 2 and
         pristine_baselib.count(b"lua_gc(") == 2,
         "lbaselib.c: the latch is not the only new lua_gc caller")
    new = lines_of(derived)
    checks = {
        39: b"#define GCSWEEPCOST\tisaac_pace_cost(6144, 400)",
        44: b"static l_mem isaac_pace_budget;  static lu_mem isaac_pace_cost "
            b"(l_mem quota, l_mem cap) {",
        45: b"#define GCFINALIZECOST\tisaac_pace_cost(192, MAX_LMEM / 16)",
        61: b"static const global_State *isaac_pace_scripts[4];  "
            b"static int isaac_pace_scripts_all;",
        848: b"                    : (g->gcfinnum * 2 > 8u" + _PRED +
             b" ? 8u : g->gcfinnum * 2);",
        975: b"  lua_assert(g->strt.nuse == 0);  isaac_pace_forget(g);",
        1133: b"  " + _BUDGET + b"  do {",
        1140: b"    debt = (debt / g->gcstepmul) * STEPMULADJ;  if (debt < "
              b"-GCSTEPSIZE && isaac_pace_budget >= 0 && g->gcstate == "
              b"GCScallfin) debt = -GCSTEPSIZE;",
    }
    for line, start in checks.items():
        need(new[line - 1].startswith(start), f"line {line}: {new[line - 1]!r}")
    for helper in (b"isaac_pace_rd ", b"isaac_pace_wr ", b"isaac_pace_thr ",
                   b"isaac_pace_extra ",
                   b"isaac_pace_setdebt ", b"isaac_pace_open ",
                   b"isaac_pace_vend ", b"isaac_pace_late ",
                   b"isaac_pace_swept "):
        need(helper in new[59], f"line 60 lacks {helper!r}")
    # retired: the r4 restart detector, cut and marks, and r5's own first
    # draft's schedule escape (isaac_pace_v / _escape) and setpause mark
    # (isaac_pace_none)
    for gone in (b"isaac_pace_cut", b"isaac_pace_mark", b"isaac_pace_restarted",
                 b"isaac_pace_mine", b"isaac_pace_saved", b"isaac_pace_v ",
                 b"isaac_pace_v(", b"isaac_pace_escape", b"isaac_pace_none"):
        need(gone not in derived, f"retired machinery {gone!r} is back")
    for latch in (b"int isaac_lua_gc_scripted ", b"void isaac_lua_gc_script_control ",
                  b"static void isaac_pace_forget "):
        need(latch in new[60], f"line 61 lacks {latch!r}")
    nosweep = pace.derive(pristine, MAX, STEP, 0)
    need(hashlib.sha256(nosweep).hexdigest() == DERIVED_NOSWEEP_SHA256,
         f"no-sweep derived sha changed: {hashlib.sha256(nosweep).hexdigest()}")
    need(changed_lines(pristine, nosweep) == EXPECTED_LINES[1:],
         "sweep step 0 must leave lgc.c:39 pristine")
    need(changed_lines(nosweep, derived) == [39],
         "the sweep step changed a line other than 39")
    # F-GCSWEEP: another quota is the same derivation with the one number on
    # line 39 replaced (the expected line is made from the default's, not
    # retyped)
    quota = b"isaac_pace_cost(%d, 400)"
    cand = pace.derive(pristine, MAX, STEP, SWEEP_STEP_020)
    need(SWEEP_STEP_020 != SWEEP_STEP and
         new[38].count(quota % SWEEP_STEP) == 1 and
         changed_lines(derived, cand) == [39] and
         lines_of(cand)[38] == new[38].replace(quota % SWEEP_STEP,
                                               quota % SWEEP_STEP_020),
         f"sweep step {SWEEP_STEP_020}: line 39 is {lines_of(cand)[38]!r}, "
         f"changed lines {changed_lines(derived, cand)}")
    need(changed_lines(pristine, cand) == EXPECTED_LINES,
         f"sweep step {SWEEP_STEP_020} changed other pristine lines")
    r0 = r0_derive(pristine)
    need(changed_lines(pristine, r0) == [39, 45, 848], "review-r0 lines")
    r1 = r1_derive(pristine)
    need(changed_lines(pristine, r1) == [39, 44, 45, 848, 1133, 1140],
         "review-r1 lines")
    r4 = r4_derive(pristine)
    need(changed_lines(pristine, r4) == sorted(R4_LINES), "review-r4 lines")
    # against r4: the latch (61, 975, and lines 60/909/1133 where it is
    # read), the restart detector, cut and marks gone (60, 945, 1128, 1129),
    # the caller's budget no longer saved and restored (1128, 1133, 1135,
    # 1143), the schedule escape gone (60, 1133) and with it line 845's cap
    # (848's comment says why), the stopped path pristine (1130) and the
    # paced step's debt restored after its tail (1142)
    need(changed_lines(r4, derived) ==
         [60, 61, 845, 848, 909, 945, 975, 1128, 1129, 1130, 1133, 1135,
          1142, 1143],
         f"derived against review-r4: {changed_lines(r4, derived)}")
    # against review r5 (0474604): those two lines only
    need(changed_lines(r5_derive(pristine), derived) == [1130, 1142],
         f"derived against review-r5: "
         f"{changed_lines(r5_derive(pristine), derived)}")
    for name, (text, bl, unused) in mutants(derived, pristine, baselib,
                                            pristine_baselib).items():
        need(len(lines_of(text)) == len(old) and
             (text != derived or bl != baselib),
             f"{name} is not a one-rule mutant")
    for name, text in helper_mutants(derived).items():
        need(changed_lines(derived, text) in ([60], [61]),
             f"{name} is not a line-60/61 mutant")
    for bad in ((0, STEP, SWEEP_STEP), (4097, STEP, SWEEP_STEP),
                (MAX, 71, SWEEP_STEP), (MAX, 321, SWEEP_STEP),
                (MAX, STEP, 1), (MAX, STEP, 399), (MAX, STEP, 65537)):
        try:
            pace.derive(pristine, *bad)
        except pace.DeriveError:
            pass
        else:
            fail(f"derive accepted out-of-range constants {bad}")
    for good in ((1, 72, 400), (4096, 320, 65536)):
        pace.derive(pristine, *good)
    for refuse, data in ((pace.derive, pristine + b"\n"),
                         (pace.derive_baselib, pristine_baselib + b"\n")):
        try:
            refuse(data)
        except pace.DeriveError as exc:
            need("not pristine" in str(exc), f"wrong refusal: {exc}")
        else:
            fail(f"{refuse.__name__} accepted a non-pristine input")
    # CLI: both files, same bytes; up to date -> untouched; stale or missing
    # -> written; the lbaselib arguments are required.
    src = work / "cli" / "pristine-lgc.c"
    src_bl = work / "cli" / "pristine-lbaselib.c"
    out = work / "cli" / "out" / "lgc.c"
    out_bl = work / "cli" / "out" / "lbaselib.c"
    src.parent.mkdir(parents=True, exist_ok=True)
    src.write_bytes(pristine)
    src_bl.write_bytes(pristine_baselib)
    cmd = [sys.executable, str(PACE_SCRIPT), "--input", str(src),
           "--output", str(out), "--baselib-input", str(src_bl),
           "--baselib-output", str(out_bl)]
    first = subprocess.run(cmd, capture_output=True, text=True)
    need(first.returncode == 0 and out.read_bytes() == derived and
         out_bl.read_bytes() == baselib,
         f"CLI derivation failed: {first.stdout}{first.stderr}")
    need(f"max={MAX} fin_step={STEP} sweep_step={SWEEP_STEP} "
         f"sha256={DERIVED_SHA256} lbaselib_sha256={BASELIB_SHA256}"
         in first.stdout, f"CLI receipt: {first.stdout}")
    newest = max(src.stat().st_mtime_ns, src_bl.stat().st_mtime_ns,
                 PACE_SCRIPT.stat().st_mtime_ns)
    fresh = newest + 10 * 10**9
    for path in (out, out_bl):
        os.utime(path, ns=(fresh, fresh))
    again = subprocess.run(cmd, capture_output=True, text=True)
    need(again.returncode == 0 and out.stat().st_mtime_ns == fresh and
         out_bl.stat().st_mtime_ns == fresh,
         "CLI rewrote an up-to-date output")
    stale = newest - 10 * 10**9
    for path in (out, out_bl):
        os.utime(path, ns=(stale, stale))
    redo = subprocess.run(cmd, capture_output=True, text=True)
    need(redo.returncode == 0 and out.stat().st_mtime_ns > stale and
         out_bl.stat().st_mtime_ns > stale and out.read_bytes() == derived and
         out_bl.read_bytes() == baselib, "CLI left a stale output alone")
    out.unlink()
    out_bl.unlink()
    remade = subprocess.run(cmd, capture_output=True, text=True)
    need(remade.returncode == 0 and out.read_bytes() == derived and
         out_bl.read_bytes() == baselib,
         "CLI did not recreate missing outputs")
    alone = subprocess.run(cmd[:6], capture_output=True, text=True)
    need(alone.returncode != 0 and "--baselib-input" in alone.stderr,
         f"CLI derived lgc.c without lbaselib.c: {alone.stderr}")
    nosweep_out = work / "cli" / "out-nosweep" / "lgc.c"
    second = subprocess.run(
        [sys.executable, str(PACE_SCRIPT), "--input", str(src), "--output",
         str(nosweep_out), "--baselib-input", str(src_bl), "--baselib-output",
         str(nosweep_out.parent / "lbaselib.c"), "--sweep-step", "0"],
        capture_output=True, text=True)
    need(second.returncode == 0 and nosweep_out.read_bytes() == nosweep and
         f"sweep_step=0 sha256={DERIVED_NOSWEEP_SHA256}" in second.stdout,
         f"CLI no-sweep derivation failed: {second.stdout}{second.stderr}")
    cand_out = work / "cli" / "out-sweep020" / "lgc.c"
    third = subprocess.run(
        [sys.executable, str(PACE_SCRIPT), "--input", str(src), "--output",
         str(cand_out), "--baselib-input", str(src_bl), "--baselib-output",
         str(cand_out.parent / "lbaselib.c"), "--sweep-step",
         str(SWEEP_STEP_020)], capture_output=True, text=True)
    need(third.returncode == 0 and cand_out.read_bytes() == cand and
         f"max={MAX} fin_step={STEP} sweep_step={SWEEP_STEP_020} "
         f"sha256={hashlib.sha256(cand).hexdigest()} "
         f"lbaselib_sha256={BASELIB_SHA256}" in third.stdout,
         f"CLI --sweep-step {SWEEP_STEP_020} failed: "
         f"{third.stdout}{third.stderr}")
    src.write_bytes(pristine.replace(b"GCSWEEPCOST", b"GCSWEEPCOSX", 1))
    bad = subprocess.run(cmd, capture_output=True, text=True)
    need(bad.returncode == 1 and "not pristine" in bad.stderr,
         f"CLI accepted a non-pristine input: {bad.returncode} {bad.stderr}")
    src.write_bytes(pristine)
    src_bl.write_bytes(pristine_baselib.replace(b"collectgarbage",
                                                b"collectgarbagX", 1))
    bad = subprocess.run(cmd, capture_output=True, text=True)
    need(bad.returncode == 1 and "not pristine Lua 5.3.3 lbaselib.c"
         in bad.stderr,
         f"CLI accepted a non-pristine lbaselib.c: {bad.returncode} "
         f"{bad.stderr}")
    print(f"derivation: lgc.c lines {changed} changed, sha "
          f"{DERIVED_SHA256[:16]}...; lbaselib.c lines {BASELIB_LINES}, sha "
          f"{BASELIB_SHA256[:16]}...; no-sweep sha "
          f"{DERIVED_NOSWEEP_SHA256[:16]}...; sweep step {SWEEP_STEP_020} "
          f"moves line 39 only (sha "
          f"{hashlib.sha256(cand).hexdigest()[:16]}...); review-r0 (sha "
          f"{R0_SHA256[:16]}...), review-r1 (sha {R1_SHA256[:16]}...) and "
          f"review-r4 (sha {R4_SHA256[:16]}...) rebuilt; "
          f"{len(mutants(derived, pristine, baselib, pristine_baselib))} "
          f"mutants + {len(helper_mutants(derived))} line-60/61 boundary "
          f"mutants; range/pristine/CLI checks OK")


def block(first: str, opener: str, closer: str, blocks: int) -> str:
    """`blocks` consecutive opener/closer blocks of CMakeLists.txt, verbatim."""
    lines = VITA_CMAKE.read_text(encoding="utf-8").split("\n")
    need(lines.count(first) == 1, f"CMake anchor not unique: {first!r}")
    start = lines.index(first)
    depth = done = 0
    for index in range(start, len(lines)):
        stripped = lines[index].strip()
        if stripped.startswith(opener):
            depth += 1
        elif stripped.startswith(closer):
            depth -= 1
            if depth == 0:
                done += 1
                if done == blocks:
                    return "\n".join(lines[start:index + 1])
    fail(f"CMake block {first!r} has no matching {closer}")
    raise AssertionError


def posix(path: Path) -> str:
    return str(path).replace("\\", "/")


def cmake_tests(lua_root: Path, pristine: bytes, pristine_baselib: bytes,
                work: Path) -> None:
    cmake = (shutil.which("cmake") or "C:/Program Files/CMake/bin/cmake.exe")
    need(Path(cmake).exists() or shutil.which(cmake) is not None,
         "cmake is required for the CMake leg")
    retired = block("foreach(_isaac_retired IN ITEMS", "foreach(",
                    "endforeach(", 1)
    validation = block(
        "if(ISAAC_VITA_LUA_GC_FINALIZER_PACE AND NOT ISAAC_VITA_LUA)",
        "if(", "endif(", 7)
    need("ISAAC_VITA_LUA_GC_SWEEP_STEP must be" in validation and
         "is validated with" in validation and
         "ISAAC_VITA_LUA_GC_FINALIZER_STEP must be" in validation and
         "needs ISAAC_VITA_LUA_GCSTEP_CLAMP_KB in 1..64" in validation,
         "the extracted CMake validation lost a range check")
    swap = block("  if(ISAAC_VITA_LUA_GC_FINALIZER_PACE)", "if(", "endif(", 1)
    need("add_custom_command(" in swap, "the swap block lost its build rule")
    derived = pace.derive(pristine)
    baselib = pace.derive_baselib(pristine_baselib)

    def configure(tag: str, pace_on: str, lua_on: str = "ON",
                  max_text: str = str(MAX), step_text: str = str(STEP),
                  sweep_text: str = str(SWEEP_STEP),
                  clamp_text: str = "128",
                  extra: tuple[str, ...] = (),
                  sources: str = "") -> tuple[int, str, Path]:
        build = work / "cmake" / tag
        build.mkdir(parents=True, exist_ok=True)
        script = build / "probe.cmake"
        script.write_text("\n".join([
            "cmake_minimum_required(VERSION 3.16)",
            "# set_property(DIRECTORY ...) and add_custom_command are not",
            "# scriptable; the probe records the build rule's arguments.",
            "function(set_property)",
            "endfunction()",
            "macro(add_custom_command)",
            '  set(PROBE_CUSTOM "${ARGV}")',
            "endmacro()",
            f'set(ISAAC_VITA_LUA {lua_on})',
            f'set(ISAAC_VITA_LUA_GC_FINALIZER_PACE {pace_on})',
            f'set(ISAAC_VITA_LUA_GC_FINALIZER_MAX "{max_text}")',
            f'set(ISAAC_VITA_LUA_GC_FINALIZER_STEP "{step_text}")',
            f'set(ISAAC_VITA_LUA_GC_SWEEP_STEP "{sweep_text}")',
            f'set(ISAAC_VITA_LUA_GCSTEP_CLAMP_KB "{clamp_text}")',
            *extra,
            f'set(ISAAC_LUA53_SOURCE_DIR "{posix(lua_root)}")',
            f'set(CMAKE_CURRENT_SOURCE_DIR "{posix(VITA_CMAKE.parent)}")',
            f'set(CMAKE_BINARY_DIR "{posix(build)}")',
            f'set(Python3_EXECUTABLE "{posix(Path(sys.executable))}")',
            retired,
            validation,
            'if(DEFINED ISAAC_VITA_LUA_GC_FINALIZER_COST OR '
            'DEFINED ISAAC_VITA_LUA_GC_SWEEP_COST)',
            '  message(STATUS "PROBE_RETIRED=kept")',
            'else()',
            '  message(STATUS "PROBE_RETIRED=dropped")',
            'endif()',
            f'include("{posix(VITA_CMAKE.parent / "lua53_vanilla.cmake")}")',
            sources,
            'set(PROBE_BEFORE "${ISAAC_LUA53_C_SOURCES}")',
            swap,
            'message(STATUS "PROBE_BEFORE=${PROBE_BEFORE}")',
            'message(STATUS "PROBE_AFTER=${ISAAC_LUA53_C_SOURCES}")',
            'message(STATUS "PROBE_CUSTOM=${PROBE_CUSTOM}")',
            "",
        ]), encoding="utf-8", newline="\n")
        done = subprocess.run([cmake, "-P", str(script)], capture_output=True,
                              text=True)
        return done.returncode, done.stdout + done.stderr, build

    def probe(out: str, key: str) -> str:
        match = re.search(rf"{key}=(.*)", out)
        need(match is not None, f"probe output lacks {key}: {out}")
        assert match is not None
        return match.group(1).strip()

    rc, out, build = configure("off", "OFF")
    need(rc == 0, f"CMake OFF probe failed: {out}")
    before = probe(out, "PROBE_BEFORE").split(";")
    need(len(before) == 33 and probe(out, "PROBE_AFTER").split(";") == before,
         "OFF changed the Lua source list")
    need(not (build / "lua53-gc-finalizer-pace").exists(),
         "OFF wrote a derived file")
    rc, out, build = configure("on", "ON")
    need(rc == 0, f"CMake ON probe failed: {out}")
    before = probe(out, "PROBE_BEFORE").split(";")
    after = probe(out, "PROBE_AFTER").split(";")
    lgc = build / "lua53-gc-finalizer-pace" / "lgc.c"
    lbl = build / "lua53-gc-finalizer-pace" / "lbaselib.c"
    names = [Path(item).name for item in before]
    expect = list(before)
    expect[names.index("lgc.c")] = posix(lgc)
    expect[names.index("lbaselib.c")] = posix(lbl)
    need(after == expect,
         f"ON did not swap exactly lgc.c and lbaselib.c in place: {after}")
    need(lgc.read_bytes() == derived,
         "ON derived lgc.c differs from lua53_gc_finalizer_pace.derive")
    need(lbl.read_bytes() == baselib,
         "ON derived lbaselib.c differs from derive_baselib")
    status = (f"Isaac Lua: __gc finalizers paced (lua53_gc_finalizer_pace: "
              f"max={MAX} fin_step={STEP} sweep_step={SWEEP_STEP} "
              f"sha256={DERIVED_SHA256} lbaselib_sha256={BASELIB_SHA256}); "
              f"isaac_lua53 compiles {posix(lgc)} and {posix(lbl)}")
    need(status in " ".join(out.split()), f"ON status receipt missing: {out}")
    need(probe(out, "PROBE_RETIRED") == "dropped", "retired knobs leaked")
    need("is validated with" not in out, f"default build warns: {out}")
    # the build rule: OUTPUT is both swapped files, DEPENDS the script and
    # both pristine sources, and its COMMAND recreates either deleted file
    custom = probe(out, "PROBE_CUSTOM").split(";")
    need(custom[:3] == ["OUTPUT", posix(lgc), posix(lbl)] and
         "VERBATIM" in custom, f"build rule output: {custom}")
    command = custom[custom.index("COMMAND") + 1:custom.index("DEPENDS")]
    depends = custom[custom.index("DEPENDS") + 1:custom.index("COMMENT")]
    need(sorted(depends) == sorted([posix(PACE_SCRIPT),
                                    posix(lua_root / "src" / "lgc.c"),
                                    posix(lua_root / "src" / "lbaselib.c")]),
         f"build rule depends: {depends}")
    for gone in (lgc, lbl):
        gone.unlink()
        rule = subprocess.run(command, capture_output=True, text=True)
        need(rule.returncode == 0 and lgc.read_bytes() == derived and
             lbl.read_bytes() == baselib,
             f"build rule did not recreate {gone.name}: {rule.stderr}")
    rc, out, build = configure("on-nosweep", "ON", sweep_text="0")
    need(rc == 0, f"CMake ON no-sweep probe failed: {out}")
    need((build / "lua53-gc-finalizer-pace" / "lgc.c").read_bytes() ==
         pace.derive(pristine, MAX, STEP, 0), "ON no-sweep derived bytes")
    # F-GCSWEEP: the 0.2.0 candidate quota with the recipe's clamp (64):
    # the derived bytes, the receipt, no warning, and a build rule that
    # recreates the file with the same quota
    cand = pace.derive(pristine, MAX, STEP, SWEEP_STEP_020)
    rc, out, build = configure("on-sweep020", "ON",
                               sweep_text=str(SWEEP_STEP_020),
                               clamp_text="64")
    need(rc == 0, f"CMake ON sweep {SWEEP_STEP_020} probe failed: {out}")
    lgc = build / "lua53-gc-finalizer-pace" / "lgc.c"
    lbl = build / "lua53-gc-finalizer-pace" / "lbaselib.c"
    need(lgc.read_bytes() == cand and cand != derived and
         lbl.read_bytes() == baselib,
         f"ON sweep {SWEEP_STEP_020} derived bytes")
    need(f"max={MAX} fin_step={STEP} sweep_step={SWEEP_STEP_020} "
         f"sha256={hashlib.sha256(cand).hexdigest()} "
         f"lbaselib_sha256={BASELIB_SHA256}); isaac_lua53 compiles "
         f"{posix(lgc)} and {posix(lbl)}" in " ".join(out.split()),
         f"ON sweep {SWEEP_STEP_020} status receipt missing: {out}")
    need("is validated with" not in out,
         f"sweep {SWEEP_STEP_020} with CLAMP_KB=64 must not warn: {out}")
    custom = probe(out, "PROBE_CUSTOM").split(";")
    command = custom[custom.index("COMMAND") + 1:custom.index("DEPENDS")]
    need(command[command.index("--sweep-step") + 1] == str(SWEEP_STEP_020),
         f"build rule quota: {command}")
    lgc.unlink()
    rule = subprocess.run(command, capture_output=True, text=True)
    need(rule.returncode == 0 and lgc.read_bytes() == cand,
         f"build rule did not recreate lgc.c at sweep {SWEEP_STEP_020}: "
         f"{rule.stderr}")
    # F-GCSWEEP guard: a quota below 3072 configures only with CLAMP_KB in
    # 1..64 (the recipe's 64, and the G0 shape 6144 / 64 above); these
    # shapes must still configure, the refusals are in the table below
    for tag, sweep_text, clamp_text in (
            ("guard-3072-k128", "3072", "128"),
            ("guard-1536-k64", "1536", "64"),
            ("guard-2048-k40", str(SWEEP_STEP_020), "40"),
            ("guard-6144-k0", str(SWEEP_STEP), "0")):
        rc, out, unused = configure(tag, "ON", sweep_text=sweep_text,
                                    clamp_text=clamp_text)
        need(rc == 0 and "needs ISAAC_VITA_LUA_GCSTEP_CLAMP_KB" not in
             " ".join(out.split()),
             f"PACE with sweep {sweep_text} and CLAMP_KB={clamp_text} must "
             f"configure: {out}")
    rc, out, unused = configure("clamp40", "ON", clamp_text="40")
    need(rc == 0 and "is validated with" in " ".join(out.split()),
         f"PACE with CLAMP_KB=40 must configure with a warning: {out}")
    # 64 is validated by test_vita_lua_gc_finalizer_alloc_latch.py's clamp
    # legs (R60 lever 6): no warning, and the CMake comment says where
    rc, out, unused = configure("clamp64", "ON", clamp_text="64")
    need(rc == 0 and "is validated with" not in out,
         f"PACE with CLAMP_KB=64 must configure without the warning: {out}")
    need("test_vita_lua_gc_finalizer_alloc_latch.py adds" in
         VITA_CMAKE.read_text(encoding="utf-8"),
         "CMakeLists.txt no longer names the test that validates CLAMP_KB=64")
    rc, out, unused = configure("clamp40-off", "OFF", clamp_text="40")
    need(rc == 0 and "is validated with" not in out,
         f"CLAMP_KB=40 without PACE must not warn: {out}")
    rc, out, build = configure(
        "retired-defaults", "ON",
        extra=('set(ISAAC_VITA_LUA_GC_FINALIZER_COST "512")',
               'set(ISAAC_VITA_LUA_GC_SWEEP_COST "5")'))
    need(rc == 0 and probe(out, "PROBE_RETIRED") == "dropped",
         f"retired knobs at their old defaults must be dropped: {out}")
    refusals = (
        ("ON", "max0", {"max_text": "0"}, "ISAAC_VITA_LUA_GC_FINALIZER_MAX must"),
        ("ON", "max-big", {"max_text": "4097"},
         "ISAAC_VITA_LUA_GC_FINALIZER_MAX must"),
        ("ON", "step-text", {"step_text": "fast"},
         "ISAAC_VITA_LUA_GC_FINALIZER_STEP must"),
        ("ON", "step-small", {"step_text": "71"},
         "ISAAC_VITA_LUA_GC_FINALIZER_STEP must"),
        ("ON", "step-big", {"step_text": "321"},
         "ISAAC_VITA_LUA_GC_FINALIZER_STEP must"),
        ("ON", "sweep-small", {"sweep_text": "399"},
         "ISAAC_VITA_LUA_GC_SWEEP_STEP must"),
        ("ON", "sweep-big", {"sweep_text": "65537"},
         "ISAAC_VITA_LUA_GC_SWEEP_STEP must"),
        ("ON", "no-lua", {"lua_on": "OFF"}, "requires ISAAC_VITA_LUA=ON"),
        # F-GCSWEEP guard: quota below 3072 with the default clamp (128),
        # one KB over the validated 64, and the unclamped step (0)
        ("ON", "sweep020-k128", {"sweep_text": str(SWEEP_STEP_020)},
         f"ISAAC_VITA_LUA_GC_SWEEP_STEP={SWEEP_STEP_020} is below 3072 and "
         "needs ISAAC_VITA_LUA_GCSTEP_CLAMP_KB in 1..64 (got 128)"),
        ("ON", "sweep3071-k65", {"sweep_text": "3071", "clamp_text": "65"},
         "needs ISAAC_VITA_LUA_GCSTEP_CLAMP_KB in 1..64 (got 65)"),
        ("ON", "sweep400-k0", {"sweep_text": "400", "clamp_text": "0"},
         "needs ISAAC_VITA_LUA_GCSTEP_CLAMP_KB in 1..64 (got 0)"),
        ("OFF", "step-pace-off", {"step_text": "128"}, "take effect only with"),
        ("OFF", "sweep-pace-off", {"sweep_text": "0"}, "take effect only with"),
        ("ON", "retired-cost", {"extra": (
            'set(ISAAC_VITA_LUA_GC_FINALIZER_COST "1024")',)},
         "ISAAC_VITA_LUA_GC_FINALIZER_COST is retired"),
        ("OFF", "retired-sweep", {"extra": (
            'set(ISAAC_VITA_LUA_GC_SWEEP_COST "32")',)},
         "ISAAC_VITA_LUA_GC_SWEEP_COST is retired"),
        # either pristine file missing from the source list is refused, not
        # half-swapped
        ("ON", "no-lbaselib", {"sources": (
            'list(REMOVE_ITEM ISAAC_LUA53_C_SOURCES '
            '"${ISAAC_LUA53_SOURCE_DIR}/src/lbaselib.c")')},
         "lbaselib.c is not in the Lua source list"),
        ("ON", "no-lgc", {"sources": (
            'list(REMOVE_ITEM ISAAC_LUA53_C_SOURCES '
            '"${ISAAC_LUA53_SOURCE_DIR}/src/lgc.c")')},
         "lgc.c is not in the Lua source list"),
    )
    for pace_on, tag, kwargs, needle in refusals:
        rc, out, unused = configure(tag, pace_on, **kwargs)
        # cmake wraps long FATAL_ERROR text
        need(rc != 0 and needle in " ".join(out.split()),
             f"CMake accepted {tag}: {out}")
    print("cmake: OFF list untouched; ON swaps lgc.c and lbaselib.c in place "
          f"with the derived bytes (sweep quota, 0 and {SWEEP_STEP_020} with "
          "CLAMP_KB 64) and prints the receipt; "
          "the build rule recreates either file; retired defaults dropped; "
          "CLAMP_KB != 128 warns; quota < 3072 configures with CLAMP_KB "
          f"1..64 only (4 accepted shapes); {len(refusals)} refusals OK")


def find_cc(explicit: Path | None) -> Path:
    candidates = [explicit] if explicit else []
    env = os.environ.get("ISAAC_CLANG")
    if env:
        candidates.append(Path(env))
    candidates.append(Path("C:/Program Files/LLVM/bin/clang.exe"))
    which = shutil.which("clang")
    if which:
        candidates.append(Path(which))
    for candidate in candidates:
        if candidate and candidate.is_file():
            return candidate
    fail("clang not found: pass --cc or set ISAAC_CLANG")
    raise AssertionError


def run(cmd: list[str], cwd: Path) -> str:
    done = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if done.returncode:
        fail(f"command failed ({done.returncode}): {' '.join(cmd)}\n"
             f"{done.stdout}{done.stderr}")
    return done.stdout + done.stderr


def warning_count(text: str) -> int:
    return len(re.findall(r"warning:", text))


def wsl_path(path: Path) -> str:
    text = posix(path.resolve())
    return f"/mnt/{text[0].lower()}{text[2:]}" if text[1:2] == ":" else text


def warnings_tests(cc: Path, lua_src: Path, pristine: bytes,
                   pristine_baselib: bytes, work: Path) -> None:
    directory = work / "warnings"
    directory.mkdir(parents=True, exist_ok=True)
    files = {"pristine": ("lgc.c", pristine),
             "derived": ("lgc.c", pace.derive(pristine)),
             "nosweep": ("lgc.c", pace.derive(pristine, MAX, STEP, 0)),
             "pristine-lbaselib": ("lbaselib.c", pristine_baselib),
             "derived-lbaselib": ("lbaselib.c",
                                  pace.derive_baselib(pristine_baselib))}
    counts: dict[str, tuple[int, int | None]] = {}
    have_wsl = shutil.which("wsl") is not None
    for name, (base, data) in files.items():
        path = directory / name / base
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        done = subprocess.run(
            [str(cc), "-m32", "-std=gnu11", "-O2", "-Wall", "-Wextra",
             f"-I{lua_src}", "-c", str(path), "-o", str(path.with_suffix(".o"))],
            capture_output=True, text=True)
        need(done.returncode == 0, f"clang -Wall -Wextra {name}: {done.stderr}")
        gcc: int | None = None
        if have_wsl:
            try:
                gdone = subprocess.run(
                    ["wsl", "-e", "gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra",
                     "-fsyntax-only", f"-I{wsl_path(lua_src)}", wsl_path(path)],
                    capture_output=True, text=True, timeout=120)
                if gdone.returncode == 0:
                    gcc = warning_count(gdone.stderr)
            except (OSError, subprocess.TimeoutExpired):
                gcc = None
        counts[name] = (warning_count(done.stderr), gcc)
    for name, ref in (("derived", "pristine"), ("nosweep", "pristine"),
                      ("derived-lbaselib", "pristine-lbaselib")):
        need(counts[name][0] == counts[ref][0], f"clang warnings {counts}")
        if counts[name][1] is not None and counts[ref][1] is not None:
            need(counts[name][1] == counts[ref][1], f"gcc warnings {counts}")
    gcc_note = ("WSL gcc same" if counts["derived"][1] is not None
                else "WSL gcc unavailable, skipped")
    print(f"warnings: clang -m32 -Wall -Wextra lgc.c pristine/derived/no-sweep "
          f"{counts['pristine'][0]}/{counts['derived'][0]}/"
          f"{counts['nosweep'][0]}, lbaselib.c pristine/derived "
          f"{counts['pristine-lbaselib'][0]}/{counts['derived-lbaselib'][0]}; "
          f"{gcc_note}")


def run_root(work: Path) -> Path:
    """The directory every oracle runs from: work/ppp...p, padded so that
    each runner, <root>/NNN/RR/oracle.exe, has an absolute path of exactly
    RUN_PATH_LEN characters whatever --work is (module docstring)."""
    tail = len(str(Path("000") / "00" / "oracle.exe")) + 1
    pad = RUN_PATH_LEN - len(str(work)) - 1 - tail
    need(pad >= 1, f"--work {work} is too long: every oracle runs from a "
                   f"path of {RUN_PATH_LEN} characters")
    return work / ("p" * pad)


def runner_copy(work: Path, exe: Path, slot: str, index: int) -> Path:
    """A copy of `exe` at <run_root>/<slot>/<index>/oracle.exe (slot: 3
    characters), the one path length every run uses."""
    target = run_root(work) / slot / f"{index:02d}" / "oracle.exe"
    need(len(slot) == 3 and len(str(target)) == RUN_PATH_LEN,
         f"runner path {target} is not {RUN_PATH_LEN} characters")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(exe, target)
    return target


def build_variants(cc: Path, lua_src: Path, sources: list[Path],
                   pristine: bytes, pristine_baselib: bytes, work: Path,
                   only: tuple[str, ...] = (),
                   parts: dict | None = None) -> dict[str, Path]:
    """Every variant's oracle, as <run_root>/NNN/00/oracle.exe (one path
    length for all, see the module docstring); `parts` receives each
    variant's (lgc.o, lbaselib.o, oracle defines) for build_exact."""
    common = work / "common"
    common.mkdir(parents=True, exist_ok=True)
    flags = ["-m32", "-O2", "-w", f"-I{lua_src}"]
    objects = []
    for source in sources:
        if source.name in ("lgc.c", "lbaselib.c"):
            continue
        obj = common / (source.stem + ".o")
        run([str(cc), *flags, "-c", str(source), "-o", str(obj)], work)
        objects.append(str(obj))
    derived = pace.derive(pristine)
    baselib = pace.derive_baselib(pristine_baselib)
    quota = ["-DORACLE_QUOTA=1", f"-DORACLE_FIN_MAX={MAX}",
             f"-DORACLE_FIN_STEP={STEP}"]
    sweep = [f"-DORACLE_SWEEP_STEP={SWEEP_STEP}"]
    latch = ["-DORACLE_LATCH=1"]
    variants = {
        # name: (lgc bytes, lbaselib bytes, oracle defines)
        "pristine": (pristine, pristine_baselib, []),
        "review-r0": (r0_derive(pristine), pristine_baselib,
                      ["-DORACLE_FIN_MAX=8"]),
        "review-r1": (r1_derive(pristine), pristine_baselib,
                      [*quota, *sweep]),
        "review-r4": (r4_derive(pristine), pristine_baselib,
                      [*quota, *sweep]),
        "review-r5": (r5_derive(pristine), baselib, [*quota, *sweep, *latch]),
        "paced": (derived, baselib, [*quota, *sweep, *latch]),
        "paced-nosweep": (pace.derive(pristine, MAX, STEP, 0), baselib,
                          [*quota, *latch]),
    }
    for name, (text, bl, extra) in mutants(derived, pristine, baselib,
                                           pristine_baselib).items():
        variants[name] = (text, bl, [*quota, *sweep, *latch, *extra])
    for name, text in helper_mutants(derived).items():
        variants[name] = (text, baselib, [*quota, *sweep, *latch])
    baselib_objects: dict[str, Path] = {}
    exes = {}
    for index, (name, (text, bl, defines)) in enumerate(variants.items()):
        if only and name not in only:
            continue
        directory = work / name
        directory.mkdir(parents=True, exist_ok=True)
        lgc = directory / "lgc.c"
        lgc.write_bytes(text)
        key = hashlib.sha256(bl).hexdigest()
        if key not in baselib_objects:
            bl_dir = work / f"lbaselib-{key[:16]}"
            bl_dir.mkdir(parents=True, exist_ok=True)
            (bl_dir / "lbaselib.c").write_bytes(bl)
            run([str(cc), *flags, "-c", str(bl_dir / "lbaselib.c"), "-o",
                 str(bl_dir / "lbaselib.o")], work)
            baselib_objects[key] = bl_dir / "lbaselib.o"
        # the variant's lgc.c is the head of the accessor TU; every
        # luaC_step goes through the oracle's wrapper and lgc.c's own
        # luaE_setdebt calls through the recording hook
        lgc_obj = directory / "lgc.o"
        lgc_defines = [d for d in defines
                       if d.startswith(("-DORACLE_QUOTA", "-DORACLE_LATCH"))]
        run([str(cc), *flags, "-DluaC_step=oracle_real_luaC_step",
             "-DluaE_setdebt=oracle_lgc_setdebt_hook", *lgc_defines,
             "-include", str(lgc), "-c", str(ORACLE_LGC), "-o", str(lgc_obj)],
            work)
        exe = directory / "oracle.exe"
        run([str(cc), "-m32", "-O2", "-Wall", "-Wextra", f"-I{lua_src}",
             *defines, str(ORACLE), *objects, str(lgc_obj),
             str(baselib_objects[key]), "-o", str(exe)], work)
        exes[name] = runner_copy(work, exe, f"{index:03d}", 0)
        if parts is not None:
            parts[name] = (lgc_obj, baselib_objects[key], defines)
    return exes


def build_exact(cc: Path, lua_src: Path, work: Path, parts: dict,
                names: tuple[str, ...]) -> dict[str, Path]:
    """The EXACT builds of `names`: lstate.c's makeseed returns the oracle's
    seed=N alone (no stack, heap or image address in it) and the oracle links
    with /DYNAMICBASE:NO, so neither the string hashes nor the CRT heap's
    addresses move between runs; each runs from <run_root>/xNN/00 (one path
    length, with the fixed environment of oracle())."""
    need(os.name == "nt", "the exact legs need an address space that is not "
                          "randomized: clang for Windows, /DYNAMICBASE:NO")
    directory = work / "exact"
    directory.mkdir(parents=True, exist_ok=True)
    source = (lua_src / "lstate.c").read_bytes()
    old = b"  return luaS_hash(buff, p, h);\n}\n"
    need(source.count(old) == 1, "lstate.c: makeseed's return not found once")
    (directory / "lstate.c").write_bytes(source.replace(
        old, b"  (void)buff; (void)p; return h;  /* exact: the seed alone */"
             b"\n}\n"))
    (directory / "seed.h").write_text("unsigned int oracle_seed(void);\n")
    lstate = directory / "lstate.o"
    run([str(cc), "-m32", "-O2", "-w", f"-I{lua_src}", "-include",
         str(directory / "seed.h"), "-Dluai_makeseed()=oracle_seed()", "-c",
         str(directory / "lstate.c"), "-o", str(lstate)], work)
    common = sorted(str(obj) for obj in (work / "common").glob("*.o")
                    if obj.name != "lstate.o")
    need(len(common) == 30, f"exact: {len(common)} common objects, not 30")
    exes = {}
    for index, name in enumerate(names):
        lgc_obj, bl_obj, defines = parts[name]
        exe = directory / f"{index:02d}" / "oracle.exe"
        exe.parent.mkdir(parents=True, exist_ok=True)
        run([str(cc), "-m32", "-O2", "-Wall", "-Wextra", f"-I{lua_src}",
             *defines, str(ORACLE), *common, str(lstate), str(lgc_obj),
             str(bl_obj), "-Wl,/DYNAMICBASE:NO", "-o", str(exe)], work)
        exes[name] = runner_copy(work, exe, f"x{index:02d}", 0)
    return exes


def parse(out: str) -> dict[str, float]:
    values: dict[str, float] = {}
    for line in out.splitlines():
        if (line.startswith("build ") or line.startswith("  sweep ") or
                line.startswith("  latch ") or line.startswith("  exact ")):
            continue        # build defines; sweep/latch/exact: own regexes
        for key, value in KV.findall(line):
            values.setdefault(key, float(value))
    return values


def oracle(exe: Path, args: list[str],
           timeout: int = RUN_TIMEOUT) -> tuple[int | str, str]:
    # the fixed environment and working directory: the CRT copies the
    # environment block onto its heap ahead of Lua's first allocation, so
    # the caller's environment would move every Lua object's address
    try:
        done = subprocess.run([str(exe), *args], capture_output=True,
                              text=True, timeout=timeout, env=RUN_ENV,
                              cwd=exe.parent.parent.parent)
    except subprocess.TimeoutExpired:
        return "TIMEOUT", ""
    return done.returncode, done.stdout + done.stderr


def run_leg(exe: Path, k: int, fin: int, frames: int, garb: int,
            options: tuple[str, ...], timeout: int = RUN_TIMEOUT) -> dict:
    rc, out = oracle(exe, ["run", str(k), str(LIVE_KB), str(fin), str(garb),
                           str(frames), str(LPS), *options], timeout)
    result: dict = {"rc": rc, "out": out, "panic": "PANIC" in out,
                    "abort": "ABORT" in out}
    if not result["panic"] and not result["abort"] and rc in (0, 1):
        violations = VIOLATION.search(out)
        coverage = COVERAGE.search(out)
        sweep = SWEEP_RE.search(out)
        steps = PRISTINE_RE.search(out)
        latch = LATCH_RE.search(out)
        need(violations is not None and coverage is not None and
             sweep is not None and steps is not None and latch is not None,
             f"unparsable oracle output\n{out}")
        assert violations is not None and coverage is not None
        assert sweep is not None and steps is not None and latch is not None
        early, weak, order, twice, never, keys, bound, slack = (
            int(v) for v in violations.groups())
        result.update(parse(out))
        result["semantics_ok"] = (rc == 0 and not (early or weak or twice or
                                                   never or keys) and
                                  order <= slack)
        result["bound_bad"] = bound
        result["coverage"] = [int(v) for v in coverage.groups()]
        result["sweep_per_cycle"] = float(sweep.group(3))
        result["pristine_steps"] = int(steps.group(1))
        result["pristine_max_fin"] = int(steps.group(2))
        result["budget_mismatch"] = int(steps.group(3))
        groups = latch.groups()
        result["scripted"] = int(groups[1])
        result["scripted_frame"] = int(groups[2])
        result["latched_steps"] = int(groups[3])
        result["forgotten"] = int(groups[4])
        result["mark"] = int(groups[6])
        result["mark_state"] = groups[7]
        result["mark_backlog"] = int(groups[8])
        result["drain"] = int(groups[9])
        result["post_peak_kb"] = int(groups[10])
        result["post_frame_ms_max"] = float(groups[11])
        result["pre_frame_ms_max"] = float(groups[12])
        exact = EXACT_RE.search(out)
        if exact is not None:
            result["digest"] = exact.group(2)
    return result


def finished(res: dict) -> bool:
    return not res["panic"] and not res["abort"] and res["rc"] == 0


def share_over_2ms(res: dict) -> float:
    return 100.0 * res["frames_over_2ms"] / res["frames"]


def judge(leg: tuple, new: dict, base: dict) -> list[str]:
    """Reasons `new` fails the matrix leg against pristine `base` ([] = pass)."""
    name, unused, unused2, kind = leg
    reasons = []
    if not finished(new):
        return [f"did not finish (rc={new['rc']}, panic={new['panic']}, "
                f"abort={new['abort']})"]
    if not new["semantics_ok"]:
        reasons.append("finalizer semantics violated")
    if new["bound_bad"] or new.get("over_bound0", 0):
        reasons.append(f"pacing bound exceeded ({new['bound_bad']} steps, "
                       f"{new.get('over_bound0', 0)} over bound0)")
    if new["peak_kb"] > PEAK_RATIO * base["peak_kb"]:
        reasons.append(f"peak {new['peak_kb']:.0f} KB > {PEAK_RATIO} x "
                       f"{base['peak_kb']:.0f}")
    if new["peak_second_kb"] > 1.02 * new["peak_first_kb"] + 64:
        reasons.append(f"heap still growing: second half "
                       f"{new['peak_second_kb']:.0f} KB > first "
                       f"{new['peak_first_kb']:.0f}")
    if new["lag_max"] > LAG_RATIO * base["lag_max"]:
        reasons.append(f"lag max {new['lag_max']:.0f} > {LAG_RATIO} x "
                       f"{base['lag_max']:.0f} loops")
    if kind == "cap" and new["alloc_fail"]:
        reasons.append(f"{new['alloc_fail']:.0f} allocator refusals")
    if kind == "normal":
        for key in ("frame_ms_p99", "frame_ms_max"):
            if new[key] > FRAME_RATIO * base[key]:
                reasons.append(f"{key} {new[key]:.2f} > {FRAME_RATIO} x "
                               f"{base[key]:.2f}")
    return reasons


def median_peak(runs: list[dict]) -> float:
    return statistics.median(res["peak_kb"] for res in runs)


def not_finished(new: list[dict]) -> list[str]:
    bad = [res for res in new if not finished(res)]
    if not bad:
        return []
    tail = (bad[0]["out"].strip().splitlines() or [f"rc={bad[0]['rc']}"])[-1]
    return [f"{len(bad)}/{len(new)} runs did not finish: {tail[:100]}"]


def judge_schedule(new: list[dict], base: list[dict]) -> list[str]:
    """The review-r2..r4 peak judge (review-r1 must fail it): the median
    peak above 1.05x pristine's and above pristine's highest run."""
    reasons = not_finished(new)
    if reasons:
        return reasons
    peak, ref = median_peak(new), median_peak(base)
    top = max(res["peak_kb"] for res in base)
    if peak > 1.05 * ref and peak > top:
        reasons.append(f"median peak {peak:.0f} KB > 1.05 x {ref:.0f} and > "
                       f"pristine's highest run {top:.0f}")
    return reasons


def judge_pristine(new: list[dict], base: list[dict]
                   ) -> tuple[list[str], list[str]]:
    """(reasons, figures): `new` against pristine's runs of the same leg,
    two-sided, per JUDGED metric: the median within max(PRISTINE_TOL,
    pristine's (max - min) / median) of pristine's median."""
    reasons = not_finished(new)
    if reasons:
        return reasons, []
    if not all(res["semantics_ok"] for res in new):
        reasons.append("finalizer semantics violated")
    figures = []
    for key, floor in JUDGED:
        ref = [res[key] for res in base]
        mine = [res[key] for res in new]
        a, b = statistics.median(ref), statistics.median(mine)
        if max(abs(a), abs(b)) < floor:
            figures.append(f"{key} {b:.4g}/{a:.4g} (below {floor})")
            continue
        tol = max(PRISTINE_TOL, (max(ref) - min(ref)) / a if a else 0.0)
        dev = b / a - 1.0 if a else float("inf")
        figures.append(f"{key} {b:.4g}/{a:.4g} {dev:+.1%} (tol {tol:.1%})")
        # a median inside pristine's own range is within the spread by
        # construction (|b - a| <= max - min); testing it directly keeps a
        # bimodal leg, whose two medians sit in different modes, from
        # failing on a last-bit tie between b/a - 1 and (max - min)/a
        if abs(dev) > tol and not min(ref) <= b <= max(ref):
            reasons.append(f"{key} {b:.4g} vs pristine {a:.4g} ({dev:+.1%}, "
                           f"tol {tol:.1%})")
    return reasons, figures


def row(name: str, build: str, res: dict) -> str:
    if not finished(res):
        tail = res["out"].strip().splitlines()[-1] if res["out"].strip() else ""
        return f"{name:18s} {build:24s} rc={res['rc']} {tail[:100]}"
    return (f"{name:18s} {build:24s} peak={res['peak_kb']:7.0f} KB "
            f"mark={res['mark_units_per_frame']:6.0f} "
            f"loop GC ms p99/max={res['frame_ms_p99']:6.2f}/"
            f"{res['frame_ms_max']:7.2f} >2ms={share_over_2ms(res):4.1f}% "
            f"GC ms/s={res['gc_ms_per_s']:5.1f} cycles={res['cycles']:.0f} "
            f"lag={res['lag_max']:.0f} pristine_steps={res['pristine_steps']} "
            f"latched={res['scripted']}@{res['scripted_frame']}")




def worse(new: list[dict], base: list[dict]) -> list[str]:
    """The JUDGED metrics on which `new` is worse (higher) than pristine's
    median beyond max(PRISTINE_TOL, pristine's spread): the review-r4
    verdict on a latch leg (r4 must regress, not merely differ)."""
    if not_finished(new):
        return not_finished(new)
    found = []
    for key, floor in JUDGED:
        ref = [res[key] for res in base]
        a = statistics.median(ref)
        b = statistics.median(res[key] for res in new)
        if max(abs(a), abs(b)) < floor or not a:
            continue
        tol = max(PRISTINE_TOL, (max(ref) - min(ref)) / a)
        if b / a - 1.0 > tol:
            found.append(f"{key} {b:.4g} vs {a:.4g} ({b / a - 1.0:+.1%}, "
                         f"tol {tol:.1%})")
    return found


# Teeth of the rules: (mutant, (leg name, finalizers/loop, garbage tables
# per loop, options), the property the mutant must break).  The host-oracle
# screens that chose these legs are in LUA-GC-R5.md.
TEETH_RUNS = 3
# loops from the hand-over until tobefnz is empty: at most HANDOVER_DRAIN, or
# pristine's own longest drain from the same mark (state and backlog) where
# pristine stops there too (a 1000-object backlog in GCSswpallgc takes
# pristine 2 loops at 200 finalizers per loop and 4 at 10)
HANDOVER_DRAIN = 1
TEETH_PEAK_RATIO = 1.05
TEETH_SHARE = 5.0
TEETH_MAX_RATIO = 2.0
BOUND_TEETH = ("pristine", "mutant-fin-charge5", "mutant-line45-pristine",
               "mutant-no-cap", "mutant-no-budget")
# mutants whose tooth is one helpers check (oracle "helpers" mode): exactly
# that check must fail; line 44 without its latched-state guard charges a
# budget -1 step the quota cost instead of pristine's
HELPER_TEETH = {"mutant-guard-44": "budget -1 charges the pristine cost"}
TEETH = (
    # 1142: without the restore an allocating __gc in the tail reaches the
    # stopped path, whose 10 x GCSTEPSIZE credit holds off the paced drain
    ("mutant-no-restore", ("fa4-r200", 200, GARB, ("fin_alloc=4",)),
     "peak"),
    # 1130 as review r5 had it: a raising __gc's unwound step leaves a debt
    # that an unprotected luaC_checkGC steps on
    ("mutant-r5-stopped", ERROR_LEGS[0], "panic"),
    ("mutant-no-keepv-step", ("r600", 600, GARB, ()), "peak"),
    ("mutant-no-late", ("r600", 600, GARB, ()), "peak"),
    ("mutant-no-book", ("r1000", 1000, GARB, ()), "peak"),
    ("mutant-no-cf", ("r1000", 1000, GARB, ()), "peak"),
    ("mutant-no-clamp", ("k128-r200-g0", 200, 0, ()), "max"),
    ("mutant-cap-everywhere", ("em13312-r100", 100, GARB, ("cap_kb=13312",)),
     "panic"),
    # the latch: set by count, by the engine's lua_gc(LUA_GCCOLLECT), blind
    # to one option, never set, kept past lua_close, keeping the backlog
    ("mutant-latch-on-count", ("count-r200", 200, GARB, ("ctl=count",)),
     "unlatched"),
    ("mutant-latch-engine", ("engcollect-r600", 600, GARB, ("collect=600",)),
     "unlatched"),
    ("mutant-latch-ignores-step",
     ("s0x3-r1000", 1000, GARB, ("ctl=luasteps:3",)), "judge"),
    ("mutant-latch-ignores-setstepmul",
     ("smt1000-r10", 10, GARB, ("ctl=smtoggle:1000",)), "judge"),
    ("mutant-latch-never", ("stop-r600", 600, GARB, ("ctl=stop",)), "judge"),
    ("mutant-no-forget", ("stop-r600", 600, GARB, ("ctl=stop",)), "forget"),
    ("mutant-keep-backlog",
     ("ho-s2b1000-r200", 200, GARB, ("ctl=latch:2,1000",)), "drain"),
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--lua-source", type=Path)
    parser.add_argument("--cc", type=Path)
    parser.add_argument("--work", type=Path)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--build-only", action="store_true",
                        help="derivation, CMake and warnings legs, then build "
                             "every oracle variant into --work and stop")
    args = parser.parse_args()
    lua_root = R.discover_source(args.lua_source)
    sources = R.validate_source(lua_root, R.records())
    need(len(sources) == 33, f"expected the 33-C closure, got {len(sources)}")
    lua_src = lua_root / "src"
    records = dict(R.records())
    need(records["lgc.c"] == pace.PRISTINE_LGC_SHA256,
         "lua53_gc_finalizer_pace.PRISTINE_LGC_SHA256 != lua53_vanilla.cmake "
         "lgc.c")
    need(records["lbaselib.c"] == pace.PRISTINE_LBASELIB_SHA256,
         "lua53_gc_finalizer_pace.PRISTINE_LBASELIB_SHA256 != "
         "lua53_vanilla.cmake lbaselib.c")
    pristine = (lua_src / "lgc.c").read_bytes()
    pristine_baselib = (lua_src / "lbaselib.c").read_bytes()
    cc = find_cc(args.cc)
    holder = None
    pool = None
    if args.work is None:
        holder = tempfile.TemporaryDirectory(prefix="isaac-lua-gc-pace-")
        work = Path(holder.name)
    else:
        work = args.work.resolve()
        work.mkdir(parents=True, exist_ok=True)
    try:
        derivation_tests(pristine, pristine_baselib, work)
        cmake_tests(lua_root, pristine, pristine_baselib, work)
        warnings_tests(cc, lua_src, pristine, pristine_baselib, work)
        parts: dict = {}
        all_exes = build_variants(cc, lua_src, sources, pristine,
                                  pristine_baselib, work, parts=parts)
        exact_exes = build_exact(
            cc, lua_src, work, parts,
            ("pristine", "paced", "review-r4",
             *dict.fromkeys(name for name, unused, unused2 in EXACT_TEETH)))
        if args.build_only:
            print(f"build-only: {len(all_exes)} oracle variants and "
                  f"{len(exact_exes)} exact builds in {work}")
            return 0
        derived = pace.derive(pristine)
        boundary = set(helper_mutants(derived))
        # mutants() members that edit the helper lines 60/61 themselves
        # (mutant-no-book, the latch-option mutants): the helpers check may
        # or may not see them; their teeth are their TEETH legs
        lines = derived.split(b"\n")
        helper_edits = {
            name for name, (text, unused, unused2) in mutants(
                derived, pristine, pace.derive_baselib(pristine_baselib),
                pristine_baselib).items()
            if text.split(b"\n")[59:61] != lines[59:61]}
        exes = {name: exe for name, exe in all_exes.items()
                if name not in boundary}
        judged = ({name for name, unused, unused2 in TEETH} | set(BOUND_TEETH) |
                  set(HELPER_TEETH) |
                  {name for name, unused, unused2 in EXACT_TEETH})
        unjudged = sorted(name for name in exes if name.startswith("mutant-")
                          and name not in judged)
        need(not unjudged, f"mutants without a teeth leg: {unjudged}")
        latch_builds = [name for name in all_exes
                        if name not in ("pristine", "review-r0", "review-r1",
                                        "review-r4")]
        pool = ThreadPoolExecutor(max_workers=max(1, args.jobs))
        helpers = {name: pool.submit(oracle, all_exes[name], ["helpers"])
                   for name in all_exes}
        semantic = {
            (leg[0], name): pool.submit(run_leg, exe, leg[1], leg[2], leg[3],
                                        GARB, leg[4])
            for leg in SEMANTIC_LEGS for name, exe in exes.items()}
        matrix_builds = (("pristine", K_TODAY), ("review-r0", K_R0),
                         ("paced", K_TODAY))
        matrix = {
            (leg[0], name): pool.submit(run_leg, exes[name], k, leg[1],
                                        MATRIX_FRAMES, GARB, leg[2])
            for leg in MATRIX_LEGS for name, k in matrix_builds}
        normal_r1 = {
            leg[0]: pool.submit(run_leg, exes["review-r1"], K_TODAY, leg[1],
                                MATRIX_FRAMES, GARB, leg[2])
            for leg in MATRIX_LEGS if leg[3] == "normal"}

        runs: dict[tuple[str, str], list] = {}
        copies: dict[tuple[str, int], Path] = {}

        def copy_of(build: str, index: int) -> Path:
            """run `index` of a judged leg from a file of its own,
            <run_root>/NNN/RR/oracle.exe (one path length): the image base,
            and with it the registry slot of the light-userdata key &CLIBS,
            is drawn per file, so one file is one sample (module docstring)"""
            key = (build, index)
            if key not in copies:
                master = all_exes[build]
                copies[key] = runner_copy(work, master,
                                          master.parent.parent.name,
                                          index + 1)
            return copies[key]

        def submit(leg: tuple, build: str, count: int = RUNS) -> None:
            key = (leg[0], build)
            if key not in runs:
                runs[key] = [pool.submit(run_leg, copy_of(build, index),
                                         K_TODAY, leg[1], LEG_FRAMES, leg[2],
                                         (*leg[3], ABORT))
                             for index in range(count)]

        for leg in ENGINE_LEGS:
            for build in ("pristine", "review-r4", "paced"):
                submit(leg, build)
        for leg in ALLOC_LEGS:
            for build in ("pristine", "review-r5", "paced"):
                submit(leg, build)
        for leg in ERROR_LEGS:
            for build in ("pristine", "review-r5", "paced"):
                submit(leg, build)
        # pristine and paced: LATCH_RUNS on every script-control leg (the
        # judge's tolerance is pristine's own spread, which needs both modes
        # of a bimodal leg in pristine's sample); the review builds: RUNS
        for leg in SCHEDULE_LEGS + RESTART_LEGS:
            for build in ("pristine", "paced"):
                submit(leg, build, LATCH_RUNS)
            for build in ("review-r1", "review-r4"):
                submit(leg, build)
        for leg in BIRTH_LEGS:
            for build in ("pristine", "paced"):
                submit(leg, build, LATCH_RUNS)
        for name, leg, unused in TEETH:
            for build in (name, "paced", "pristine"):
                submit(leg, build, TEETH_RUNS)
        errors = {name: pool.submit(oracle, exe, ["error"])
                  for name, exe in exes.items()}
        # exact legs: name -> (leg, seeds); the script-control legs as x-<leg>
        exact_table: dict[str, tuple[tuple, tuple[int, ...]]] = {
            leg[0]: (leg, EXACT_SEEDS) for leg in EXACT_LEGS}
        for leg in LATCH_LEGS + HANDOVER_LEGS:
            exact_table["x-" + leg[0]] = (leg, EXACT_LATCH_SEEDS)
        exact_runs: dict = {}

        def submit_exact(leg_name: str, build: str) -> None:
            leg, seeds = exact_table[leg_name]
            for seed in seeds:
                if (leg_name, build, seed) not in exact_runs:
                    exact_runs[(leg_name, build, seed)] = pool.submit(
                        run_leg, exact_exes[build], K_TODAY, leg[1],
                        LEG_FRAMES, leg[2], (*leg[3], f"seed={seed}", ABORT))

        for leg_name in exact_table:
            submit_exact(leg_name, "pristine")
            submit_exact(leg_name, "paced")
        for leg in LATCH_LEGS:
            submit_exact("x-" + leg[0], "review-r4")
        for name, leg_name, unused in EXACT_TEETH:
            submit_exact(leg_name, name)
        # the method: pristine again on one seed (same digest)
        repeat = pool.submit(run_leg, exact_exes["pristine"], K_TODAY,
                             EXACT_LEGS[0][1], LEG_FRAMES, EXACT_LEGS[0][2],
                             (*EXACT_LEGS[0][3], f"seed={EXACT_SEEDS[0]}",
                              ABORT))

        def results(leg_name: str, build: str) -> list[dict]:
            return [future.result() for future in runs[(leg_name, build)]]

        # line-60/61 boundaries
        checks = 0
        for name, future in helpers.items():
            rc, out = future.result()
            if name not in latch_builds:
                need(rc == 2 and "helpers: not a latch build" in out,
                     f"{name}: a non-latch build answered helpers\n{out}")
                continue
            match = HELPERS_RE.search(out)
            need(match is not None, f"{name}: no helpers verdict\n{out}")
            assert match is not None
            count, bad = int(match.group(1)), int(match.group(2))
            if name in boundary:
                need(rc == 1 and bad > 0,
                     f"{name} passes the line-60/61 boundary checks: no teeth")
                failed = [line[len("helper FAIL "):] for line in
                          out.splitlines() if line.startswith("helper FAIL ")]
                print(f"helpers {name:32s} fails {bad}: {'; '.join(failed)}")
            elif name in helper_edits:
                print(f"helpers {name:32s} edits line 60/61: {bad} of "
                      f"{count} checks fail (judged by its TEETH leg)")
            elif name in HELPER_TEETH:
                failed = [line[len("helper FAIL "):] for line in
                          out.splitlines() if line.startswith("helper FAIL ")]
                need(rc == 1 and failed == [HELPER_TEETH[name]],
                     f"{name}: expected exactly the helpers check "
                     f"{HELPER_TEETH[name]!r} to fail, got {failed}\n{out}")
                print(f"teeth {name:32s} helpers: fails only "
                      f"{failed[0]!r}")
            else:
                need(rc == 0 and bad == 0,
                     f"{name} fails the line-60/61 boundary checks\n{out}")
                checks = count
        print(f"helpers: {checks} boundary checks pass in "
              f"{len(latch_builds) - len(boundary)} latch builds; "
              f"{len(boundary)} boundary mutants each fail")

        # semantics, bound, coverage, sweep line, the recorded budget
        bound_bad: dict[str, list[str]] = {name: [] for name in exes}
        coverage: dict[str, list[int]] = {name: [0] * 8 for name in exes}
        sweep_cycles: dict[str, float] = {}
        for (leg, name), future in semantic.items():
            res = future.result()
            need(finished(res) and res.get("semantics_ok", False),
                 f"{name} {leg}: finalizer semantics violated "
                 f"(rc={res['rc']})\n{res['out']}")
            if name == "mutant-no-budget":   # records 0: the check's teeth
                need(res["budget_mismatch"] > 0,
                     f"{name} {leg}: the recorded-budget check misses a "
                     "budget of 0")
            else:
                need(res["budget_mismatch"] == 0,
                     f"{name} {leg}: {res['budget_mismatch']} steps whose "
                     "recorded budget is not getdebt on entry")
            if res["bound_bad"] or res["over_bound0"]:
                bound_bad[name].append(leg)
            for i, value in enumerate(res["coverage"]):
                coverage[name][i] += value
            if leg == "k128-r10":
                sweep_cycles[name] = res["sweep_per_cycle"]
            if name in ("paced", "paced-nosweep"):
                need(res["pristine_steps"] == 0 and res["scripted"] == 0,
                     f"{name} {leg}: {res['pristine_steps']} pristine steps "
                     f"without a script (latched={res['scripted']})")
            print(f"{leg:11s} {name:32s} bound_violations={res['bound_bad']:5d} "
                  f"over_bound0={res['over_bound0']:5.0f} "
                  f"max_step_fin={res['max_step_fin']:6.0f} "
                  f"peak={res['peak_kb']:6.0f} KB")
        for name in ("paced", "paced-nosweep"):
            need(not bound_bad[name],
                 f"{name} exceeded the pacing bound in {bound_bad[name]}")
            values = coverage[name]
            need(all(values[:7]),
                 f"{name} coverage incomplete: double/cap/drain/callfin/tail/"
                 f"quota/alloc_fin = {values[:7]}")
        for name in BOUND_TEETH:
            need(bool(bound_bad[name]),
                 f"{name} never exceeded the pacing bound: no teeth")
            print(f"teeth {name:32s} pacing bound exceeded in "
                  f"{bound_bad[name]}")
        need(coverage["pristine"][1] == 0 and coverage["pristine"][0] > 0,
             f"pristine coverage implausible: {coverage['pristine']}")
        need(sweep_cycles["paced"] >= 3 * sweep_cycles["paced-nosweep"],
             f"sweep line not live: sweep steps/cycle "
             f"{sweep_cycles['paced-nosweep']} -> {sweep_cycles['paced']}")
        print(f"sweep line: k128 sweep steps/cycle "
              f"{sweep_cycles['paced-nosweep']:.1f} (no quota) -> "
              f"{sweep_cycles['paced']:.1f} (SWEEP_STEP {SWEEP_STEP})")
        for name, values in coverage.items():
            print(f"coverage {name:32s} double={values[0]} cap={values[1]} "
                  f"drain={values[2]} callfin={values[3]} tail={values[4]} "
                  f"quota={values[5]} alloc_fin={values[6]} "
                  f"bound_violations_in={bound_bad[name]}")

        # review-r1 matrix, engine only
        print(f"matrix: {MATRIX_FRAMES} loops at {LPS}/s per leg; pristine "
              f"K={K_TODAY}, review-r0 K={K_R0}, paced K={K_TODAY} "
              f"(STEP {STEP}, SWEEP_STEP {SWEEP_STEP}, MAX {MAX})")
        r0_passed = []
        for leg in MATRIX_LEGS:
            base = matrix[(leg[0], "pristine")].result()
            old = matrix[(leg[0], "review-r0")].result()
            new = matrix[(leg[0], "paced")].result()
            need(finished(base),
                 f"pristine did not finish {leg[0]}: {base['out'][-400:]}")
            for name, res in (("pristine", base), ("review-r0", old),
                              ("paced", new)):
                print(row(leg[0], name, res))
            reasons = judge(leg, new, base)
            if finished(new) and (new["pristine_steps"] or new["scripted"]):
                reasons.append(f"{new['pristine_steps']} pristine steps, "
                               f"latched={new['scripted']} without a script")
            if leg[3] == "normal":
                r1 = normal_r1[leg[0]].result()
                need(finished(r1), f"review-r1 did not finish {leg[0]}")
                print(row(leg[0], "review-r1", r1))
                if (finished(new) and
                        share_over_2ms(new) > share_over_2ms(r1) + SHARE_SLACK):
                    reasons.append(f"loops over 2 ms {share_over_2ms(new):.1f} "
                                   f"% > review-r1's {share_over_2ms(r1):.1f} % "
                                   f"+ {SHARE_SLACK}")
            need(not reasons, f"paced fails {leg[0]}: {'; '.join(reasons)}")
            old_reasons = judge(leg, old, base)
            print(f"{'':18s} review-r0 judged: "
                  f"{'; '.join(old_reasons) if old_reasons else 'passes'}")
            if leg[0] != "r10" and not old_reasons:
                r0_passed.append(leg[0])
            if leg[0] == EMERGENCY_REFUSALS_LEG:
                need(base["alloc_fail"] > 0 and new["alloc_fail"] > 0,
                     f"{leg[0]} produced no allocator refusal: not an "
                     "emergency leg any more")
        need(not r0_passed,
             f"review-r0 passes legs it must fail: {r0_passed}")

        def show(leg_name: str, builds: tuple[str, ...]) -> None:
            for build in builds:
                for res in results(leg_name, build):
                    print(row(leg_name, build, res))

        # engine legs: never latch, keep the benefit
        print(f"engine: {LEG_FRAMES} loops, {RUNS} runs per build; paced "
              f"median worst loop GC <= {FRAME_RATIO} x pristine's and p99 "
              f"<= pristine's; p99 and worst loop no worse than review-r4's "
              f"(c447d9e: the defaults keep its benefit) beyond max("
              f"{PRISTINE_TOL:.0%}, its spread); median peak <= "
              f"{ENGINE_PEAK_RATIO} x pristine's (or <= its highest run)")
        for leg in ENGINE_LEGS:
            show(leg[0], ("pristine", "review-r4", "paced"))
            base, new = results(leg[0], "pristine"), results(leg[0], "paced")
            old = results(leg[0], "review-r4")
            need(not not_finished(base), f"pristine did not finish {leg[0]}")
            need(not not_finished(old), f"review-r4 did not finish {leg[0]}")
            reasons = not_finished(new)
            if not reasons:
                latched = [res["scripted"] for res in new]
                if any(latched) or any(res["pristine_steps"] for res in new):
                    reasons.append(f"latched {latched} / pristine steps "
                                   f"{[res['pristine_steps'] for res in new]}")
                for key, ratio in (("frame_ms_p99", 1.0),
                                   ("frame_ms_max", FRAME_RATIO)):
                    a = statistics.median(res[key] for res in base)
                    b = statistics.median(res[key] for res in new)
                    if b > ratio * a:
                        reasons.append(f"{key} {b:.2f} > {ratio} x "
                                       f"pristine's {a:.2f}")
                    values = [res[key] for res in old]
                    c = statistics.median(values)
                    tol = max(PRISTINE_TOL,
                              (max(values) - min(values)) / c if c else 0.0)
                    if b > (1.0 + tol) * c:
                        reasons.append(f"{key} {b:.2f} > review-r4's "
                                       f"{c:.2f} + {tol:.0%}")
                peak, ref = median_peak(new), median_peak(base)
                top = max(res["peak_kb"] for res in base)
                if peak > ENGINE_PEAK_RATIO * ref and peak > top:
                    reasons.append(f"median peak {peak:.0f} KB > "
                                   f"{ENGINE_PEAK_RATIO} x {ref:.0f} and > "
                                   f"pristine's highest run {top:.0f}")
            need(not reasons, f"paced fails {leg[0]}: {'; '.join(reasons)}")
            def ratio(key: str, ref: list) -> float:
                return (statistics.median(r[key] for r in new) /
                        statistics.median(r[key] for r in ref))
            print(f"{'':18s} paced passes: vs pristine peak x"
                  f"{median_peak(new) / median_peak(base):.3f}, loop GC p99 "
                  f"x{ratio('frame_ms_p99', base):.2f}, max "
                  f"x{ratio('frame_ms_max', base):.2f}; vs review-r4 p99 "
                  f"x{ratio('frame_ms_p99', old):.2f}, max "
                  f"x{ratio('frame_ms_max', old):.2f}")

        def over_pristine(key: str, new: list[dict], base: list[dict]
                          ) -> str:
            """'' when the median of `key` is within max(PRISTINE_TOL,
            pristine's spread) above pristine's median, else the excess"""
            ref = [res[key] for res in base]
            a = statistics.median(ref)
            b = statistics.median(res[key] for res in new)
            tol = max(PRISTINE_TOL, (max(ref) - min(ref)) / a if a else 0.0)
            return (f"{key} {b:.2f} > pristine's {a:.2f} + {tol:.0%}"
                    if b > (1.0 + tol) * a else "")

        # engine legs with an allocating C __gc (review r5 behaviour B1)
        print(f"allocating __gc: {LEG_FRAMES} loops, {RUNS} runs per build; "
              f"paced median worst loop and p99 <= pristine's + max("
              f"{PRISTINE_TOL:.0%}, its spread), median peak <= "
              f"{ENGINE_PEAK_RATIO} x pristine's (or <= its highest run); "
              f"review-r5 must exceed the worst loop or the p99 on "
              f"{list(ALLOC_TEETH)}")
        for leg in ALLOC_LEGS:
            show(leg[0], ("pristine", "review-r5", "paced"))
            base, new = results(leg[0], "pristine"), results(leg[0], "paced")
            need(not not_finished(base), f"pristine did not finish {leg[0]}")
            reasons = not_finished(new)
            if not reasons:
                if any(res["scripted"] or res["pristine_steps"] for res in new):
                    reasons.append("latched or ran pristine steps without a "
                                   "script")
                reasons += [x for x in (over_pristine(key, new, base) for key
                                        in ("frame_ms_max", "frame_ms_p99"))
                            if x]
                peak, ref = median_peak(new), median_peak(base)
                top = max(res["peak_kb"] for res in base)
                if peak > ENGINE_PEAK_RATIO * ref and peak > top:
                    reasons.append(f"median peak {peak:.0f} KB > "
                                   f"{ENGINE_PEAK_RATIO} x {ref:.0f} and > "
                                   f"pristine's highest run {top:.0f}")
            need(not reasons, f"paced fails {leg[0]}: {'; '.join(reasons)}")
            old = results(leg[0], "review-r5")
            lost = not_finished(old) or [
                x for x in (over_pristine(key, old, base)
                            for key in ("frame_ms_max", "frame_ms_p99")) if x]
            if leg[0] in ALLOC_TEETH:
                need(bool(lost), f"review-r5 keeps pristine's worst loop and "
                                 f"p99 on {leg[0]}: the leg has no teeth for "
                                 "review r5 B1")
            print(f"{'':18s} paced passes (peak x"
                  f"{median_peak(new) / median_peak(base):.3f}); review-r5: "
                  f"{'; '.join(lost) if lost else 'keeps pristine (not a tooth leg)'}")

        # a raising __gc, protected work (review r5 behaviour B2)
        print(f"raising __gc: {LEG_FRAMES} loops, {RUNS} runs per build; "
              "pristine and paced finish every run and every raise surfaces "
              "from a protected call; review-r5 panics")
        for leg in ERROR_LEGS:
            show(leg[0], ("pristine", "review-r5", "paced"))
            for build in ("pristine", "paced"):
                mine = results(leg[0], build)
                need(not not_finished(mine),
                     f"{build} does not finish {leg[0]}: "
                     f"{'; '.join(not_finished(mine))}")
                need(all(res["raised"] > 0 and res["raised"] ==
                         res["frame_errors"] + res["step_errors"] and
                         not res["scripted"] for res in mine),
                     f"{build} {leg[0]}: raises not all surfaced from the "
                     f"protected calls: "
                     f"{[(r['raised'], r['frame_errors'], r['step_errors']) for r in mine]}")
            old = results(leg[0], "review-r5")
            need(any(res["panic"] for res in old),
                 f"review-r5 survives {leg[0]}: the leg has no teeth for "
                 "review r5 B2")
            print(f"{'':18s} pristine and paced finish; review-r5 panics in "
                  f"{sum(1 for res in old if res['panic'])}/{len(old)} runs")

        # script-control legs latched from the first loop
        print(f"script control from the first loop: {LEG_FRAMES} loops, "
              f"{LATCH_RUNS} runs of pristine and paced, {RUNS} of each "
              "review build; "
              f"two-sided, each of {[key for key, unused in JUDGED]} within "
              f"max({PRISTINE_TOL:.0%}, pristine's spread) of pristine's "
              "median")

        failed_legs = []    # judge_latched: every failing leg, one FAIL

        def judge_latched(leg: tuple) -> None:
            """a leg latched from its first loop (schedule, restart, birth):
            the two-sided judge on LATCH_RUNS runs, latched in every run,
            forgotten at lua_close"""
            base, new = results(leg[0], "pristine"), results(leg[0], "paced")
            need(not not_finished(base), f"pristine did not finish {leg[0]}")
            reasons, figures = judge_pristine(new, base)
            if not not_finished(new):
                unlatched = [i for i, res in enumerate(new)
                             if not res["scripted"]]
                if unlatched:
                    reasons.append(f"runs {unlatched} never latched")
                if any(res["forgotten"] != 1 for res in new):
                    reasons.append("lua_close left the state latched")
            if reasons:     # collected: every failing leg in one run
                failed_legs.append(f"paced fails {leg[0]}: "
                                   f"{'; '.join(reasons)}")
                print(f"{'':18s} FAILS: {'; '.join(reasons)}")
            print(f"{'':18s} paced: {'; '.join(figures)}")

        r1_passed = []
        for leg in SCHEDULE_LEGS + RESTART_LEGS:
            show(leg[0], ("pristine", "review-r1", "review-r4", "paced"))
            judge_latched(leg)
            base = results(leg[0], "pristine")
            old = judge_schedule(results(leg[0], "review-r1"), base)
            print(f"{'':18s} review-r1 peak judge: "
                  f"{'; '.join(old) if old else 'passes'}")
            if not old:
                r1_passed.append(leg[0])
            r4 = worse(results(leg[0], "review-r4"), base)
            print(f"{'':18s} review-r4 regressions: "
                  f"{'; '.join(r4) if r4 else 'none'}")
        need(not r1_passed,
             f"review-r1 passes schedule/restart legs it must fail: "
             f"{r1_passed}")
        for leg in BIRTH_LEGS:
            show(leg[0], ("pristine", "paced"))
            judge_latched(leg)
            need(all(res["scripted_frame"] == 0 and
                     res["latched_steps"] == res["steps"]
                     for res in results(leg[0], "paced")),
                 f"{leg[0]}: a state latched at birth ran a paced step")
        need(not failed_legs, f"{len(failed_legs)} script-control leg(s) "
             "fail the pristine judge:\n  " + "\n  ".join(failed_legs))

        # exact: the method first (deterministic, the seed reaches lstate.c)
        def exact_res(leg_name: str, build: str) -> list[dict]:
            unused, seeds = exact_table[leg_name]
            return [exact_runs[(leg_name, build, s)].result() for s in seeds]

        def digest(key: tuple) -> str:
            res = exact_runs[key].result()
            need(finished(res) and res.get("semantics_ok", False) and
                 "digest" in res,
                 f"exact {key}: not finished or no digest "
                 f"(rc={res['rc']})\n{res.get('out', '')[-2000:]}")
            return res["digest"]

        def digests(leg_name: str, build: str) -> list[str]:
            unused, seeds = exact_table[leg_name]
            return [digest((leg_name, build, s)) for s in seeds]

        def judge_exact(leg_name: str, build: str, judged_seeds: tuple
                        ) -> tuple[list[str], list[str], bool]:
            """(reasons, figures, above): `build` against pristine on the
            same seed, for each seed of `judged_seeds`: every JUDGED metric
            within the leg's bound (LATCH_BOUNDS, else PRISTINE_TOL), two-
            sided; values below the floor in both runs are equal.  `above`:
            some metric left its bound upwards."""
            leg, seeds = exact_table[leg_name]
            pairs = [(seed, a, b) for seed, a, b in
                     zip(seeds, exact_res(leg_name, "pristine"),
                         exact_res(leg_name, build))
                     if seed in judged_seeds]
            reasons = not_finished([b for unused, unused2, b in pairs])
            if reasons:
                return reasons, [], False
            bounds = LATCH_BOUNDS.get(leg[0], {})
            figures, above = [], False
            for key, floor in JUDGED:
                bound = bounds.get(key, PRISTINE_TOL)
                devs = []
                for seed, a_res, b_res in pairs:
                    a, b = a_res[key], b_res[key]
                    if max(abs(a), abs(b)) < floor:
                        devs.append((0.0, seed))
                    else:
                        devs.append((b / a - 1.0 if a else float("inf"),
                                     seed))
                if not devs:
                    continue
                figures.append(f"{key} {min(devs)[0]:+.1%}..{max(devs)[0]:+.1%}"
                               + (f" (bound {bound:.1%})"
                                  if key in bounds else ""))
                over = [(dev, seed) for dev, seed in devs if abs(dev) > bound]
                if over:
                    dev, seed = max(over, key=lambda x: abs(x[0]))
                    reasons.append(f"{key} {dev:+.1%} on seed {seed} "
                                   f"(bound {bound:.1%})")
                    above = above or any(dev > 0 for dev, unused in over)
            return reasons, figures, above

        first = (EXACT_LEGS[0][0], "pristine", EXACT_SEEDS[0])
        again = repeat.result()
        need(finished(again) and again.get("digest") == digest(first),
             f"exact: pristine twice on seed {EXACT_SEEDS[0]} gave "
             f"{digest(first)} and {again.get('digest')}: not deterministic")
        for leg in EXACT_LEGS:
            mine = digests(leg[0], "pristine")
            # two seeds can place every string alike (sm10000: seeds 1 and
            # 2 give one digest), so the method only needs some seed to
            # differ
            need(len(set(mine)) > 1,
                 f"exact {leg[0]}: seeds {EXACT_SEEDS} all give {mine[0]}: "
                 "the seed does not reach the string hash")
            paced = digests(leg[0], "paced")
            need(paced == mine,
                 f"exact {leg[0]}: the latched derived build leaves "
                 f"pristine's collector state: {paced} vs {mine}")
            need(all(res["scripted"] for res in exact_res(leg[0], "paced")),
                 f"exact {leg[0]}: a paced run did not latch")
            print(f"exact {leg[0]:22s} paced == pristine on seeds "
                  f"{EXACT_SEEDS}: {' '.join(mine)}")

        # the script-control legs, seed by seed on the exact builds
        print(f"exact script control: {LEG_FRAMES} loops, seeds "
              f"{EXACT_LATCH_SEEDS}, paced against pristine on the same seed; "
              f"bit-exact: {list(EXACT_LATCH_BITWISE)}; every leg: each "
              f"JUDGED metric within its bound ({PRISTINE_TOL:.0%} unless "
              "LATCH_BOUNDS names one) on every seed whose run latched")
        r4_passed, r4_regressed = [], []
        for leg in LATCH_LEGS + HANDOVER_LEGS:
            name = "x-" + leg[0]
            unused, seeds = exact_table[name]
            base, new = exact_res(name, "pristine"), exact_res(name, "paced")
            need(not not_finished(base), f"pristine did not finish {name}")
            for build in ("pristine", "paced"):
                for seed, res in zip(seeds, exact_res(name, build)):
                    print(row(f"{name}@{seed}", build, res))
            # a count-gated script takes control only once the heap passes
            # its threshold, which the paced heap need not reach (a script
            # that only counts keeps the pacer, as an engine leg); a
            # hand-over leg latches at its mark.  Judged: the seeds whose
            # paced run made the control call.
            marked = (any(str(o).startswith(GATED_CTL) for o in leg[3]) or
                      leg in HANDOVER_LEGS)
            reasons = not_finished(new)
            took = tuple(seed for seed, res in zip(seeds, new)
                         if not reasons and
                         (res["mark"] >= 0 if marked else True))
            if not reasons:
                wrong = [seed for seed, res in zip(seeds, new)
                         if (bool(res["scripted"]) != (res["mark"] >= 0) or
                             (res["scripted"] and
                              res["scripted_frame"] != res["mark"]))
                         ] if marked else [
                    seed for seed, res in zip(seeds, new)
                    if not res["scripted"]]
                if wrong:
                    reasons.append(f"seeds {wrong}: the latch does not follow "
                                   "the script's control call (mark, latch): "
                                   f"{[(r['mark'], r['scripted_frame']) for r in new]}")
                if any(res["forgotten"] != 1 for res in new):
                    reasons.append("lua_close left the state latched")
                if not took:
                    reasons.append("no seed made the control call: nothing "
                                   "to judge")
            more, figures, unused = judge_exact(name, "paced", took)
            reasons += more
            mine, theirs = digests(name, "paced"), digests(name, "pristine")
            same = sum(1 for a, b in zip(mine, theirs) if a == b)
            if leg[0] in EXACT_LATCH_BITWISE and same != len(seeds):
                reasons.append(f"digest equals pristine's on {same}/"
                               f"{len(seeds)} seeds, not all")
            if leg in HANDOVER_LEGS and not not_finished(new):
                marks = [(res["mark"], res["mark_state"], res["mark_backlog"],
                          res["drain"]) for res in new]
                if not all(m[0] >= 0 for m in marks):
                    reasons.append(f"the hand-over point was never reached: "
                                   f"{marks}")
                # at most HANDOVER_DRAIN loops, or pristine's own drain from
                # the same mark on the same seed
                allowed = [max(HANDOVER_DRAIN, res["drain"])
                           if res["mark"] >= 0 and res["drain"] >= 0
                           else HANDOVER_DRAIN for res in base]
                late = [(seed, m[3], cap) for seed, m, cap
                        in zip(seeds, marks, allowed) if not 0 <= m[3] <= cap]
                if late:
                    reasons.append(f"the backlog outlived the hand-over "
                                   f"(seed, loops, allowed): {late}")
                figures.insert(0, "hand-over (loop, state, backlog, drain) "
                                  f"{marks}")
            figures.insert(0, f"judged seeds {list(took)}, digest = "
                              f"pristine's on {same}/{len(seeds)}")
            if reasons:
                failed_legs.append(f"paced fails {name}: "
                                   f"{'; '.join(reasons)}")
                print(f"{'':18s} FAILS: {'; '.join(reasons)}")
            print(f"{'':18s} paced: {'; '.join(figures)}")
            if leg in LATCH_LEGS:
                for seed, res in zip(seeds, exact_res(name, "review-r4")):
                    print(row(f"{name}@{seed}", "review-r4", res))
                off, unused, above = judge_exact(name, "review-r4", seeds)
                print(f"{'':18s} review-r4: "
                      f"{'; '.join(off) if off else 'within the bounds'}")
                if not off and leg[0] not in R4_NEUTRAL:
                    r4_passed.append(leg[0])
                if above:
                    r4_regressed.append(leg[0])
        print(f"{'':18s} review-r4 leaves the bounds on "
              f"{len(LATCH_LEGS) - len(r4_passed) - len(R4_NEUTRAL)} of "
              f"{len(LATCH_LEGS) - len(R4_NEUTRAL)} latch legs (exempt: "
              f"{list(R4_NEUTRAL)}), upwards on {len(r4_regressed)}")
        if r4_passed:
            failed_legs.append(f"review-r4 (c447d9e) stays within the bounds "
                               f"on latch legs it must fail: {r4_passed}")
        need(not failed_legs, f"{len(failed_legs)} exact script-control "
             "leg(s) fail:\n  " + "\n  ".join(failed_legs))

        for name, leg_name, reference in EXACT_TEETH:
            unused, seeds = exact_table[leg_name]
            theirs = digests(leg_name, reference)
            mut = []
            for seed in seeds:
                res = exact_runs[(leg_name, name, seed)].result()
                mut.append(res.get("digest", "unfinished")
                           if finished(res) else "unfinished")
            missed = sum(1 for a, b in zip(mut, theirs) if a != b)
            need(missed > 0, f"{name} reproduces {reference}'s digest on "
                             f"{leg_name}: no teeth")
            print(f"teeth {name:32s} {leg_name:18s} misses {reference}'s "
                  f"digest on {missed}/{len(seeds)} seeds")

        # teeth
        for name, leg, kind in TEETH:
            mut = results(leg[0], name)
            good = results(leg[0], "paced")
            base = results(leg[0], "pristine")
            show(leg[0], (name, "paced", "pristine"))
            need(not not_finished(good) and not not_finished(base),
                 f"paced or pristine did not finish teeth leg {leg[0]}")
            if kind == "judge":
                reasons, unused = judge_pristine(mut, base)
                need(bool(reasons), f"{name} passes {leg[0]}: no teeth")
                verdict = "; ".join(reasons)
            elif kind == "unlatched":
                need(not any(res["scripted"] for res in good) and
                     all(res["scripted"] for res in mut),
                     f"{name} has no teeth on {leg[0]}: latched "
                     f"{[r['scripted'] for r in mut]}")
                verdict = "latched without a control call"
            elif kind == "drain":
                drains = [res.get("drain", -1) for res in mut]
                # HANDOVER_DRAIN, or pristine's longest drain from the same
                # mark (its runs mark without latching)
                allowed = max([HANDOVER_DRAIN,
                               *(res["drain"] for res in base
                                 if res["mark"] >= 0 and res["drain"] >= 0)])
                need(not_finished(mut) or
                     any(d < 0 or d > allowed for d in drains),
                     f"{name} drains within {allowed} loop(s) on "
                     f"{leg[0]}: {drains}")
                verdict = f"loops to drain after the hand-over: {drains}"
            elif kind == "forget":
                need(all(res["forgotten"] == 0 for res in mut) and
                     all(res["forgotten"] == 1 for res in good),
                     f"{name} has no teeth on {leg[0]}")
                verdict = "the closed state stays latched"
            elif kind == "peak":
                # a heap past abort_kb (the run stops with ABORT) is the
                # strongest form of the property
                aborted = sum(1 for res in mut if res["abort"])
                if aborted:
                    verdict = (f"heap past {ABORT} in {aborted}/{len(mut)} "
                               f"runs (paced {median_peak(good):.0f} KB)")
                else:
                    need(not not_finished(mut) and
                         median_peak(mut) > TEETH_PEAK_RATIO *
                         median_peak(good),
                         f"{name} has no teeth on {leg[0]}: peak "
                         f"{median_peak(mut) if not not_finished(mut) else '-'}"
                         f" vs paced {median_peak(good):.0f}")
                    verdict = (f"peak {median_peak(mut):.0f} KB > "
                               f"{TEETH_PEAK_RATIO} x paced "
                               f"{median_peak(good):.0f}")
            elif kind == "max":
                mine = statistics.median(r["frame_ms_max"] for r in mut)
                theirs = statistics.median(r["frame_ms_max"] for r in good)
                need(not not_finished(mut) and mine > TEETH_MAX_RATIO * theirs,
                     f"{name} has no teeth on {leg[0]}: worst loop "
                     f"{mine:.1f} ms vs paced {theirs:.1f} ms")
                verdict = (f"worst loop {mine:.1f} ms > {TEETH_MAX_RATIO} x "
                           f"paced {theirs:.1f} ms")
            elif kind == "share":
                mine = statistics.median(share_over_2ms(r) for r in mut)
                theirs = statistics.median(share_over_2ms(r) for r in good)
                need(not not_finished(mut) and mine > theirs + TEETH_SHARE,
                     f"{name} has no teeth on {leg[0]}: loops over 2 ms "
                     f"{mine:.1f} % vs paced {theirs:.1f} %")
                verdict = (f"loops over 2 ms {mine:.1f} % > paced "
                           f"{theirs:.1f} % + {TEETH_SHARE}")
            else:   # panic where paced and pristine finish
                need(any(res["panic"] for res in mut),
                     f"{name} survives {leg[0]}: no teeth")
                verdict = (f"panics in {sum(1 for r in mut if r['panic'])}/"
                           f"{len(mut)} runs where paced and pristine finish")
            print(f"teeth {name:32s} {leg[0]:18s} {verdict}")

        error_lines = {}
        for name, future in errors.items():
            rc, out = future.result()
            if name in ERROR_CASE_PANICS:
                # review r5 B2 in its smallest form: lua_pushcfunction's own
                # luaC_checkGC, ahead of the lua_pcall, steps on the debt
                # the unwound step left and runs the next raising __gc
                need(rc != 0 and "PANIC" in out,
                     f"{name} survives the __gc error case: no teeth for "
                     f"review r5 B2 (rc={rc})\n{out}")
                print(f"error case: {name} panics outside the protected "
                      "call")
                continue
            match = ERROR_LINE.search(out)
            need(rc == 0 and match is not None,
                 f"{name}: __gc error case failed (rc={rc})\n{out}")
            assert match is not None
            error_lines[name] = match.group(0)
        need(len(set(error_lines.values())) == 1,
             f"__gc error behaviour differs between builds: {error_lines}")
        print(f"error case (all builds identical): {error_lines['paced']}")
        pool.shutdown()
    finally:
        if pool is not None:    # a FAIL leaves queued runs: drop them
            pool.shutdown(wait=False, cancel_futures=True)
        if holder is not None:
            holder.cleanup()
    print("PASS: test_vita_lua_gc_finalizer_pace (derivation + CMake + "
          f"warnings + {len(SEMANTIC_LEGS)} semantic legs x {len(exes)} builds "
          f"+ {len(MATRIX_LEGS)} matrix legs x 3 builds + "
          f"{len(ENGINE_LEGS)} engine + {len(ALLOC_LEGS)} allocating-__gc + "
          f"{len(ERROR_LEGS)} raising-__gc legs, {RUNS} runs per build + "
          f"{len(SCHEDULE_LEGS) + len(RESTART_LEGS) + len(BIRTH_LEGS)} "
          f"script-control legs latched from the first loop, {LATCH_RUNS} "
          f"runs of pristine and paced, {RUNS} per review build + "
          f"{len(EXACT_LEGS)} exact legs x {len(EXACT_SEEDS)} seeds + "
          f"{len(LATCH_LEGS)} latch and {len(HANDOVER_LEGS)} hand-over legs "
          f"x {len(EXACT_LATCH_SEEDS)} seeds on the exact builds + "
          f"{len(TEETH) + len(BOUND_TEETH) + len(EXACT_TEETH) + len(HELPER_TEETH)} teeth + "
          f"line-60/61 boundary "
          f"checks in {len(latch_builds)} builds + __gc error case)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
