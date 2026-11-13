#!/usr/bin/env python3
"""Derive the finalizer-paced lgc.c and lbaselib.c for ISAAC_VITA_LUA_GC_FINALIZER_PACE.

Pristine Lua 5.3.3 runs the finalizers of a collection cycle in a burst.
lgc.c:841-850 `runafewfinalizers` doubles `g->gcfinnum` on every call
(1, 2, 4, ...) and lgc.c:45 accounts a finalizer as GCFINALIZECOST =
GCSWEEPCOST = (sizeof(TString) + 4) / 4 = 5 work units on a 32-bit target.
One paced LUA_GCSTEP of K KB (K * 1024 work units, lapi.c:1076-1078)
therefore runs up to ~K * 205 finalizers, so every pending finalizer of a
cycle lands in one luaC_step.  With EID that is ~1,400-1,700 LuaBridge `__gc`
callbacks into translated code per cycle, measured 13-18 ms in one step on
the console (GC profile, grid-align-deep-owner-20260907, fin max_step
1435-1447).

Review r5 (LUA-GC-R5.md).  The pacer serves the engine's own schedule only.
The game drives the collector through two calls (host_vita_lua.c:2415-2471,
host_vita_lua_gc.c): lua_gc(LUA_GCSTEP, n) once per loop with n >= 50 KB
(clamped to ISAAC_VITA_LUA_GCSTEP_CLAMP_KB) and lua_gc(LUA_GCCOLLECT) at mod
load and floor transitions (or the GCCOLLECT clamp's LUA_GCSTEP), plus
GCCOUNT/GCCOUNTB queries; the shipped mods call collectgarbage("count")
only.  Four review rounds found a new way for a mod's collectgarbage() to
drive the paced schedule worse than pristine's (review r2 B3: "stop" and
setstepmul 40; review r3 B-R1: "restart"; review r4 B-R2: a "restart" or
"step" while the cycle sweeps cancels the next pause; B-R4: setstepmul
toggled every loop starts a cycle every ~2 loops), each fixed by another
schedule rule.  r5 stops chasing them: the first time a Lua script calls
collectgarbage with an option other than "count" or "isrunning", that Lua
state is latched to pristine 5.3.3 for the rest of its life.  From then on
every step, pause, finalizer and sweep decision of the derived lgc.c is
pristine's (each derived line below says what it does when the state is
latched), and the cycle in flight continues from wherever it is: its
backlog is run with pristine's doubling by the next step, its debt and
pause are pristine's arithmetic on the state the pacer left.  A state latched
before its first paced step (at lua_newstate, or by a setup call after the
setup's full collection) is pristine bit for bit (the exact legs of the
test).  A state latched later is not pristine-equivalent in general: it
runs pristine's code from the heap, the backlog and the cycle position the
pacer left, and how far it then stays from a pristine run of the same
script is a host measurement per leg and seed (LUA-GC-R5-round2.md, the
test's LATCH_BOUNDS), not a property of the code.

Where a script is told from the engine.  The derived lbaselib.c
(luaB_collectgarbage, lbaselib.c:182) calls isaac_lua_gc_script_control(L,
option) before lua_gc; luaB_collectgarbage is the base library's
collectgarbage and is reachable only from Lua, while the engine reaches
lua_gc through the lua_gc import (host_vita_lua.c vita_lua_gc_import), never
through the base library, so an engine call cannot latch and a script call
cannot avoid it.  isaac_lua_gc_script_control (lgc.c:61) ignores LUA_GCCOUNT,
LUA_GCCOUNTB and LUA_GCISRUNNING and latches on every other option,
including ones that change nothing (setpause 200).  An unknown option
raises in luaL_checkoption (lbaselib.c:181) before line 182, so it never
latches.

The latch is per global_State: a table of four states in the derived
lgc.c (a coroutine's collectgarbage latches its global state).  A fifth
latched state sets a process-wide flag instead, so every state is pristine
from then on, for the life of the process (nothing clears the flag);
luaC_freeallobjects (lua_close) clears the state's slot, so a new state at
a reused address starts paced.  The table, like the step budget, is a file
static: the Lua states of the process are assumed to run on one thread (a
reading of host_vita_lua.c, which creates one state and drives it from the
game thread, not a measurement; a reader on another thread could miss a
latch for one step, never see a false one).  The budget is not saved or
restored around a nested step: it is written at lgc.c:1133 and read until
the step's tail ends, so a step of a second state run from inside a
finalizer of the first (nothing in the game does that) would leave the
second state's budget for the rest of the first step.

While no script has taken control every step is paced (the review-r3 rule
that ran a step behind the schedule as pristine's served collectgarbage
"stop", low setstepmul and "step", which now latch; at the engine's own
schedule it never fired in the review matrix, and lgc.c:845's per-call cap
existed only for the gcfinnum such a step doubled, so r5 drops both).  The
schedule survives in one rule, the pause after a paced cycle:

  V = GCdebt + p   in GCScallfin, p = totalbytes - threshold + E on entry,

where threshold is setpause's (lgc.c:937-946) on the estimate at entry and
E is the work the paced sweep charged beyond pristine's 5 per object,
converted to bytes as lgc.c:1140 converts (work / stepmul * STEPMULADJ).
V > 0 means a pristine collector, which spends E less of the debt on this
cycle, would already be past this cycle's pause; the engine's
lua_gc(LUA_GCSTEP, K) adds K KB of debt per loop whatever the loop
allocates, which V sees through the debt.  A cycle that ends in a paced
step starts the next one no later than V: its pause debt is
max(setpause's, min(V, totalbytes)).  Without that rule the pause does not
absorb the longer paced cycle (the mutant-no-late test leg).

What E books: the extra charge of every paced sweep singlestep, n * (c - 5),
and the whole sweep of the tobefnz list, n * c -- pristine's tail call
(lgc.c:1142) doubles through the sweep and has usually finalized that list
before GCSswptobefnz, and a finalized object leaves tobefnz for allgc ahead
of the sweep pointer, so it is not swept again in that cycle
(udata2finalize, lgc.c:787-797).  Booking all of it overstates pristine's
saving when pristine had not drained the list yet; the error makes the
paced schedule run early, never late.

What V does not book: the allocations of finalizers.  An allocating __gc
reaches the not-running path (lgc.c:1130, pristine: GCTM clears gcrunning
around the call), which sets pristine's credit of 10 * GCSTEPSIZE bytes
with a plain luaE_setdebt; inside a step the step's own setdebt (1141) then
keeps V from that point, and after the tail the restore (1142) puts the
debt, and with it V, back where they were before the tail.  So a paced
cycle does not count what its finalizers allocate (or resurrect) towards
its pause, as pristine's step does not count it towards its debt.  Booking
it (1130 and 1142 through isaac_pace_setdebt, measured as "cnd1" in
LUA-GC-R5-round2.md) kept the heap at pristine's on those legs, but a
cycle whose finalizers kept raising then ended with V far past the pause,
and with unprotected host allocations (adv9g, ADV_HOSTPUSH 10..40) it
panicked in 13 of 48 runs where pristine and this derivation panicked in
none.

luaC_checkfinalizer (lgc.c:899-916) is changed too while no script has
taken control.  When the sweep position sits right behind an object that is
given a __gc metatable (the atomic step ran inside that object's own
allocation, so entersweep swept it as live), pristine 5.3.3 calls
sweeptolive, which frees every dead object up to the next live one outside
any luaC_step: one call can free megabytes, and GCestimate never sees those
bytes, so the next threshold is too high by twice that.  The paced cycle
lands there more often than pristine's (the mutant-no-cf test leg), so the
derived file moves the sweep position to the object's old slot instead --
O(1), nothing freed -- and the regular sweep frees that run and books it.

Per-cycle bookkeeping lives in g->GCmemtrav, which pristine Lua writes before
it reads it in every state that uses it (singlestep GCSpause / GCSpropagate
/ atomic, lgc.c:1044-1050, 979), and which is otherwise dead from entersweep
to the next GCSpause: from entersweep it holds E's work units (unsigned,
saturating), from GCScallfin the offset p (signed, kept by the step's own
setdebt, lgc.c:1141, so that V only moves with new debt).  GCScallfin
is entered only through lgc.c:1079, so the offset is always set there.  A
latched state keeps writing it; nothing reads it there.  The budget of the
running step is a file static set at lgc.c:1133 (-1 = a pristine step) and
set to -1 by the latch, so a script that takes control from inside a
finalizer of a paced step (a __gc calling collectgarbage) makes the rest of
that step pristine too.

Nothing here bounds the heap by construction.  The late rule bounds how far
a paced cycle's pause lags pristine's (plus the error in E); the heap
against pristine is a host measurement (LUA-GC-R5.md: the branch oracle and
the reviewers' harnesses, 10..1000 finalizers per step at the engine's own
schedule), not a property of the code.

What it costs at the engine's schedule (LUA-GC-R5.md; times are the
counted-work device projection, not host time): more GC work at high
finalizer rates -- the paced cycles sweep more objects than pristine's --
and many medium steps instead of rare long ones; finalizers run later, so
memory that finalized userdata own outside the Lua heap peaks higher than
pristine's.  Where finalizers allocate or resurrect (the paragraph on what V
does not book), the paced cycles are longer than pristine's: peak heap up
to +12 % and down to half the cycles, so a __gc that resurrects its object
runs down to half as often; where every 100th __gc raises, +16-21 % peak,
1.7-1.8x the GC time per second and 2.4x the loop p99 (LUA-GC-R5-round2.md).

The derivation rewrites these lines of the pristine files and nothing else
(line numbers are preserved).  Q is the finalizer quota
(ISAAC_VITA_LUA_GC_FINALIZER_STEP), MAX the per-call cap
(ISAAC_VITA_LUA_GC_FINALIZER_MAX), S the sweep quota
(ISAAC_VITA_LUA_GC_SWEEP_STEP, 0 = pristine line 39):

lgc.c
* 39 (S > 0) GCSWEEPCOST becomes isaac_pace_cost(S, 400): a paced
  sweepstep (lgc.c:1026-1038) sweeps GCSWEEPMAX = (GCSTEPSIZE / c) / 4
  objects for GCSWEEPMAX * c units, so a paced step sweeps ~S objects; the
  cap 400 keeps GCSWEEPMAX >= 1.  Budget -1 (a pristine step, every step of
  a latched state): 5.
* 44 (a comment) becomes `isaac_pace_budget` and
  `isaac_pace_cost(quota, cap)` = clamp((budget + GCSTEPSIZE) / quota, 5,
  cap); a budget of -1 charges 5.
* 45 GCFINALIZECOST becomes isaac_pace_cost(Q, MAX_LMEM / (2 * MAX)): a
  paced step in GCScallfin runs about Q finalizers whatever its budget (the
  loop of lgc.c:1133-1136 stops once the charge reaches budget +
  GCSTEPSIZE).
* 60 (a blank line) holds the schedule helpers (isaac_pace_*); 61 (a blank
  line) the latch: isaac_lua_gc_script_control and isaac_lua_gc_scripted.
* 848 in a paced step past atomic (GCSswpallgc..GCScallfin) the doubling
  of gcfinnum saturates at MAX.  atomic sets gcfinnum to 1 (lgc.c:1007) and
  every step of an unlatched state is paced, so in those states gcfinnum
  never exceeds MAX and one runafewfinalizers call -- a GCScallfin
  singlestep or the tail call of luaC_step -- runs at most MAX.  A paced
  step therefore runs at most floor((budget + GCSTEPSIZE - 1) / c) + 2 *
  MAX <= floor(5 * Q / 4) + 2 * MAX finalizers.  Outside those states
  tobefnz is non-empty only after an emergency collection (lmem.c:90
  luaC_fullgc(L, 1) runs no finalizer); there the pristine doubling stays (a
  cap there panics the oracle's 13,312 KB allocator-cap leg, em13312-r100,
  where pristine survives).  Budget -1: pristine.
* 909 / 913 luaC_checkfinalizer: no sweeptolive while paced, the sweep
  position follows the object's unlink; a latched state calls sweeptolive
  (after which the sweep position is past the object, so 913's move is
  inert).
* 958 entersweep starts E at 0.
* 975 luaC_freeallobjects drops the state's latch.
* 1033 sweepstep books its charge past pristine's into E.
* 1079 entering GCScallfin turns E into the offset p.
* 1133 picks the step's budget: -1 (pristine step) when the state is
  latched, else the step's debt.
* 1138 the pause after the cycle: setpause, then no later than V (not in a
  latched state).
* 1140 a paced step that ends in GCScallfin keeps at most GCSTEPSIZE
  bytes of credit (after the conversion to bytes, so setstepmul does not
  scale it), so ~1.6 KB of allocation start the next step.  Without it the
  quota's overshoot becomes credit and the paced finalizer drain falls
  behind (review r1 B1; the mutant-no-clamp test leg).
* 1141 luaE_setdebt goes through isaac_pace_setdebt, which keeps V.
* 1142 a paced step's tail runafewfinalizers (after the step's own setdebt)
  restores the step's debt when it returns.  An allocating __gc in the
  tail reaches the not-running path (1130, pristine), and pristine's 16,000
  bytes of credit would hold off the next paced step until that much more
  is allocated: with finalizers that allocate ~300 bytes each the drain
  falls to about one step per loop and behind the creation rate (4 tables
  per finalizer at 200 finalizers per loop: past 256 MB where pristine
  peaks at 37.7 MB; the mutant-no-restore test leg).  Review r5 kept the
  debt in the not-running path instead (1130's credit only in a latched
  state), which chained paced steps into one loop when the tail allocated
  past the step's credit (review r5 B1: 4 KB per __gc at 10 per loop,
  worst loop 59 ms where pristine's is 16), and left a raising __gc's
  unwound step without the credit, so the next unprotected allocation
  stepped and ran the next raising __gc outside any protected call (review
  r5 B2; the mutant-r5-stopped test leg).  A tail that raises unwinds past
  the restore: the debt is then pristine's credit from the raising __gc's
  own allocation, as in pristine.  An emergency collection inside the tail
  (an allocation failure in a __gc) ends in GCSpause; the restore then
  replaces setpause's debt with the step's, so the next cycle starts after
  at most GCSTEPSIZE more bytes instead of after the pause.  Budget -1
  (latched, also by a __gc in this tail): pristine.

* 821 (A > 0 only; A is ISAAC_VITA_LUA_GC_FINALIZER_ALLOC_LATCH_BYTES, the
  --alloc-latch argument, 0 = this line, 958 and 61 are as without it) GCTM
  measures gettotalbytes around the finalizer's luaD_pcall and adds the
  growth to a per-cycle sum (isaac_pace_fin_bytes, cleared by entersweep,
  line 958).  GCTM clears gcrunning around the call (lgc.c:817), so nothing
  is collected inside it and the growth is what the finalizer allocated and
  left behind.  Once the sum of one cycle passes A bytes the state is
  latched exactly as by a script's collectgarbage (line 61:
  isaac_lua_gc_script_control), so the rest of that step and every later
  step of the state are pristine 5.3.3's.  This bounds review round 2's
  blockers that need a __gc that allocates (B3: finalizers allocating more
  than ~200-300 KB per loop; the same shape drives B1 and B2): such a
  workload runs paced for at most A bytes of finalizer allocation and
  pristine from there on.  The sum is one file static, not one per state:
  with several unlatched states it is the sum over all of them, which
  latches earlier, never later.  isaac_lua_gc_alloc_latches() counts the
  latches of the process and isaac_lua_gc_state_latched(L) says whether L's
  own state is latched, by this guard or by a script (the runtime's receipt,
  host_vita_lua.c: the count alone cannot tell a latch of a state that is
  being closed from a latch of the live one).  What it
  does not see: a __gc that allocates nothing net (it resurrects its
  object, or raises an interned message) -- those costs stay as stated
  above.

lbaselib.c
* 172 (a blank line) declares isaac_lua_gc_script_control.
* 182 luaB_collectgarbage calls it with the option before lua_gc.

Which objects are finalized, the order (tobefnz head first,
udata2finalize/GCTM lgc.c:787-836), the error a raising __gc raises (GCTM's
propagateerrors path: the same LUA_ERRGCMM and message), emergency
collections, luaC_fullgc / lua_close (callallpendingfinalizers, lgc.c:856)
are unchanged; *when* a pending finalizer runs, and when a dead object's
memory is returned, move.  So does *where* a raising __gc surfaces: from
the luaC_step that ran it, and a paced build runs finalizers in many more
steps than pristine, so more of them surface from allocation-driven steps
(inside whatever API call allocated) than from the explicit lua_gc step
(LUA-GC-R3.md, m2).  An allocation outside any lua_pcall that steps into a
raising __gc panics in either build; on the measured legs (adv9g, a raising
__gc every 7th or 100th finalizer, 0..60 unprotected host allocations per
loop) this derivation panicked in the same legs as pristine
(LUA-GC-R5-round2.md), which is a measurement on those legs, not a bound.
The game's own step is such a call: vita_lua_gc_import calls lua_gc with no
lua_pcall around it (host_vita_lua.c).  With the explicit step unprotected
(adv9g ADV_GAME=1, a raising __gc every 7th to 1000th finalizer, 100 or 200
per loop) every run of every build panicked, pristine included, from that
step: pristine at loop 167-180, this derivation at loop 168-190 on four legs
and 371-1699 on 200 per loop / every 1000th.  The derivation moves the
loop, not the outcome.

A control call gated on a GC observable (collectgarbage("count") above a
threshold) fires at a different loop, because the state is paced until
then; from that call on, the measured legs match pristine (adv9 countcollect
200/13000 and fullstep 1000/300, 8 runs: post-call p99, worst loop and GC
ms/s within 1.1 %), while the whole run's p99 carries the paced prefix
(x2.7 and x1.33 of pristine's).
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys

# lua53_vanilla.cmake: "lgc.c|6f1e1801..." and "lbaselib.c|e4a49f05...".
# test_vita_lua_gc_finalizer_pace.py checks these copies still match the
# manifest records.
PRISTINE_LGC_SHA256 = (
    "6f1e180167b0276ae063d2ba32d94b46c26455218f46ae12f8f222b9edd1e932")
PRISTINE_LBASELIB_SHA256 = (
    "e4a49f0507ba1ea12b82c4bb50fb367f5984997a47b8159a3024bcc195486cef")

MAX_MIN, MAX_MAX = 1, 4096
MAX_DEFAULT = 8
# FIN_STEP_MAX: GCSTEPSIZE / 320 = 5, so the pristine floor never binds and
# floor((budget + GCSTEPSIZE) / Q) >= 4 keeps the 5Q/4 bound.  FIN_STEP_MIN:
# up to (GCSTEPSIZE + 24) / 24 = 67 finalizable objects (24 bytes, a 0-byte
# userdata on 32-bit ARM) are made per GCSTEPSIZE of allocation, and 72 keeps
# the quota above that.  It is a margin, not a guarantee: in the measured
# legs the schedule rule, not the quota, keeps the heap near pristine's, and
# only Q = 192 is measured (LUA-GC-R3.md, LUA-GC-R5.md).
FIN_STEP_MIN, FIN_STEP_MAX = 72, 320
FIN_STEP_DEFAULT = 192
# SWEEP_STEP 0 leaves lgc.c:39 pristine.  400..65536: (400 + GCSTEPSIZE) / 400
# = 5, so a step with a budget of up to 400 units sweeps as pristine does.
SWEEP_STEP_MIN, SWEEP_STEP_MAX = 400, 65536
SWEEP_STEP_DEFAULT = 6144
# ALLOC_LATCH 0 leaves lines 61, 821 and 958 as without the guard (the bytes
# of every earlier pin).  The floor is 10 * GCSTEPSIZE (lgc.c:1130's credit;
# GCSTEPSIZE = 100 * sizeof(TString) = 1600 on the 32-bit targets): a cycle
# whose finalizers regrow a shrunk stack or CallInfo list (lgc.c
# traversethread -> luaD_shrinkstack) must not latch.
ALLOC_LATCH_MIN, ALLOC_LATCH_MAX = 16000, 1 << 30
ALLOC_LATCH_DEFAULT = 0
ALLOC_LATCH_LINE = 821

_TAG = b"ISAAC_VITA_LUA_GC_FINALIZER_PACE"
_FIVE = b"(lu_mem)((sizeof(TString) + 4) / 4)"
_SLOTS = 4


def _helpers() -> bytes:
    """lgc.c:60: the schedule helpers, one line (the oracle copies it)."""
    parts = [
        # lgc.c:61, the latch: a Lua script called collectgarbage with an
        # option that controls the collector (derived lbaselib.c:182)
        b"LUAI_FUNC int isaac_lua_gc_scripted (const global_State *g);",
        b"LUAI_FUNC void isaac_lua_gc_script_control (lua_State *L, int what);",
        # GCmemtrav as a signed offset (callfin) and back, saturating
        b"static l_mem isaac_pace_rd (const global_State *g) { "
        b"lu_mem u = g->GCmemtrav; "
        b"return u > (lu_mem)MAX_LMEM ? -(l_mem)(~u) - 1 : (l_mem)u; }",
        b"static void isaac_pace_wr (global_State *g, long long v) { "
        b"g->GCmemtrav = (lu_mem)(l_mem)(v < -(long long)MAX_LMEM ? -MAX_LMEM "
        b": v > (long long)MAX_LMEM ? MAX_LMEM : v); }",
        # setpause's threshold (lgc.c:937-944) on the current estimate
        b"static l_mem isaac_pace_thr (const global_State *g) { "
        b"l_mem e = g->GCestimate / PAUSEADJ; if (e <= 0) e = 1; "
        b"return (g->gcpause < MAX_LMEM / e) ? e * g->gcpause : MAX_LMEM; }",
        # E: extra work units of this cycle, as bytes
        b"static long long isaac_pace_extra (const global_State *g) { "
        b"return (long long)(g->GCmemtrav / g->gcstepmul) * STEPMULADJ; }",
        # luaE_setdebt that keeps V = GCdebt + p in GCScallfin
        # (gettotalbytes does not move)
        b"static void isaac_pace_setdebt (global_State *g, l_mem debt) { "
        b"l_mem old = g->GCdebt; luaE_setdebt(g, debt); "
        b"if (g->gcstate == GCScallfin) "
        b"isaac_pace_wr(g, (long long)isaac_pace_rd(g) + old - g->GCdebt); }",
        # entering GCScallfin: E -> offset p, V = GCdebt + p
        b"static void isaac_pace_open (global_State *g) { "
        b"isaac_pace_wr(g, (long long)g->totalbytes - isaac_pace_thr(g) + "
        b"isaac_pace_extra(g)); }",
        # end of cycle: V before setpause (none in a latched state), then the
        # pause no later than V
        b"static long long isaac_pace_vend (const global_State *g) { "
        b"return isaac_lua_gc_scripted(g) ? -(long long)MAX_LMEM - 1 "
        b": (long long)g->GCdebt + isaac_pace_rd(g); }",
        b"static void isaac_pace_late (global_State *g, long long v) { "
        b"l_mem tb = (l_mem)gettotalbytes(g); "
        b"if (v > g->GCdebt) luaE_setdebt(g, v < tb ? (l_mem)v : tb); }",
        # sweepstep's charge, booking what pristine would not have spent
        b"static lu_mem isaac_pace_swept (global_State *g, int n, lu_mem c) { "
        b"lu_mem x = (lu_mem)n * (g->gcstate == GCSswptobefnz ? c : c - " + _FIVE +
        b"); g->GCmemtrav = g->GCmemtrav < (lu_mem)MAX_LMEM - x ? "
        b"g->GCmemtrav + x : (lu_mem)MAX_LMEM; return (lu_mem)n * c; }",
    ]
    return (b"  ".join(parts) + b"  /* " + _TAG +
            b" schedule (pristine: blank line) */\n")


def _latch(alloc_latch: int = 0) -> bytes:
    """lgc.c:61: the per-state latch (lgc.o only; the oracle calls it)."""
    slots = str(_SLOTS).encode("ascii")
    parts = [
        b"static const global_State *isaac_pace_scripts[" + slots + b"];",
        # a fifth latched state: every state is pristine from then on
        b"static int isaac_pace_scripts_all;",
        b"int isaac_lua_gc_scripted (const global_State *g) { int i; "
        b"if (isaac_pace_scripts_all) return 1; "
        b"for (i = 0; i < " + slots + b"; i++) "
        b"if (isaac_pace_scripts[i] == g) return 1; return 0; }",
        # collectgarbage(what) from a script (derived lbaselib.c:182): any
        # option but the counts and isrunning latches; the step that is
        # running (a __gc's collectgarbage) is pristine from here on
        b"void isaac_lua_gc_script_control (lua_State *L, int what) { "
        b"const global_State *g = G(L); int i; "
        b"if (what == LUA_GCCOUNT || what == LUA_GCCOUNTB || "
        b"what == LUA_GCISRUNNING) return; isaac_pace_budget = -1; "
        b"if (isaac_lua_gc_scripted(g)) return; "
        b"for (i = 0; i < " + slots + b"; i++) "
        b"if (isaac_pace_scripts[i] == NULL) { isaac_pace_scripts[i] = g; "
        b"return; } isaac_pace_scripts_all = 1; }",
        # lua_close (luaC_freeallobjects): a state's latch ends with it
        b"static void isaac_pace_forget (const global_State *g) { int i; "
        b"for (i = 0; i < " + slots + b"; i++) "
        b"if (isaac_pace_scripts[i] == g) isaac_pace_scripts[i] = NULL; }",
    ]
    if alloc_latch:
        limit = str(alloc_latch).encode("ascii")
        parts += [
            # ISAAC_VITA_LUA_GC_FINALIZER_ALLOC_LATCH_BYTES: what this
            # cycle's finalizers allocated (cleared at lgc.c:958), and the
            # process's count of latches by it (the runtime's receipt)
            b"static lu_mem isaac_pace_fin_bytes;",
            b"static unsigned int isaac_pace_alloc_latches;",
            b"unsigned int isaac_lua_gc_alloc_latches (void);",
            b"unsigned int isaac_lua_gc_alloc_latches (void) { "
            b"return isaac_pace_alloc_latches; }",
            # the receipt's per-state view: the count above is the process's,
            # the latch itself is per Lua state (review of the latch lane)
            b"int isaac_lua_gc_state_latched (lua_State *L);",
            b"int isaac_lua_gc_state_latched (lua_State *L) { "
            b"return isaac_lua_gc_scripted(G(L)); }",
            # GCTM (lgc.c:821): the heap grew across a finalizer's call
            b"static void isaac_pace_fin_grew (lua_State *L, lu_mem before) { "
            b"global_State *g = G(L); lu_mem now = gettotalbytes(g); "
            b"if (now <= before || isaac_lua_gc_scripted(g)) return; "
            b"now -= before; isaac_pace_fin_bytes = "
            b"now < (lu_mem)MAX_LMEM - isaac_pace_fin_bytes ? "
            b"isaac_pace_fin_bytes + now : (lu_mem)MAX_LMEM; "
            b"if (isaac_pace_fin_bytes > (lu_mem)" + limit + b") { "
            b"isaac_pace_alloc_latches++; "
            b"isaac_lua_gc_script_control(L, LUA_GCCOLLECT); } }",
        ]
    return (b"  ".join(parts) + b"  /* " + _TAG +
            b" script latch (pristine: blank line) */\n")


_HELPER_NEW = (
    b"static l_mem isaac_pace_budget;  "
    b"static lu_mem isaac_pace_cost (l_mem quota, l_mem cap) { "
    b"l_mem b = isaac_pace_budget < MAX_LMEM - GCSTEPSIZE ? "
    b"isaac_pace_budget + GCSTEPSIZE : MAX_LMEM; "
    b"l_mem c = b / quota, base = (l_mem)((sizeof(TString) + 4) / 4); "
    b"return (lu_mem)(isaac_pace_budget < 0 || c < base ? base : "
    b"c > cap ? cap : c); }"
    b"  /* " + _TAG + b" quota charge, budget -1 = pristine step "
    b"(pristine: comment line) */\n")

# line: (pristine bytes, derived bytes or a %-template)
_EDITS: dict[int, tuple[bytes, bytes]] = {
    39: (b"#define GCSWEEPCOST\t((sizeof(TString) + 4) / 4)\n",
         b"#define GCSWEEPCOST\tisaac_pace_cost(%d, 400)  "
         b"/* ISAAC_VITA_LUA_GC_SWEEP_STEP (pristine: "
         b"(sizeof(TString) + 4) / 4 = 5) */\n"),
    44: (b"/* cost of calling one finalizer */\n", _HELPER_NEW),
    45: (b"#define GCFINALIZECOST\tGCSWEEPCOST\n",
         b"#define GCFINALIZECOST\tisaac_pace_cost(%d, MAX_LMEM / %d)  "
         b"/* ISAAC_VITA_LUA_GC_FINALIZER_STEP (pristine: GCSWEEPCOST) */\n"),
    60: (b"\n", b""),  # _helpers()
    61: (b"\n", b""),  # _latch()
    848: (b"                    : g->gcfinnum * 2;  "
          b"/* else call a few more next time */\n",
          b"                    : (g->gcfinnum * 2 > %du && "
          b"isaac_pace_budget >= 0 && g->gcstate >= GCSswpallgc && "
          b"g->gcstate <= GCScallfin ? %du : g->gcfinnum * 2);  "
          b"/* ISAAC_VITA_LUA_GC_FINALIZER_MAX cap in a paced step after "
          b"atomic, which sets gcfinnum to 1 (pristine: doubles) */\n"),
    909: (b"        g->sweepgc = sweeptolive(L, g->sweepgc);  "
          b"/* change 'sweepgc' */\n",
          b"        if (isaac_lua_gc_scripted(g)) g->sweepgc = "
          b"sweeptolive(L, g->sweepgc);  /* change 'sweepgc'; " + _TAG +
          b": only in a latched state, else it moves to o's slot at the "
          b"unlink below */\n"),
    913: (b"    *p = o->next;  /* remove 'o' from 'allgc' list */\n",
          b"    *p = o->next;  if (g->sweepgc == &o->next) { g->sweepgc = p; }  "
          b"/* remove 'o' from 'allgc' list; " + _TAG + b": the sweep "
          b"position follows it, nothing is freed here */\n"),
    958: (b"  g->gcstate = GCSswpallgc;\n",
          b"  g->gcstate = GCSswpallgc;  g->GCmemtrav = 0;  "
          b"/* " + _TAG + b": extra sweep charge */\n"),
    975: (b"  lua_assert(g->strt.nuse == 0);\n",
          b"  lua_assert(g->strt.nuse == 0);  isaac_pace_forget(g);  "
          b"/* " + _TAG + b": the state's latch ends with it */\n"),
    1033: (b"      return (GCSWEEPMAX * GCSWEEPCOST);\n",
           b"      return isaac_pace_swept(g, GCSWEEPMAX, GCSWEEPCOST);  "
           b"/* " + _TAG + b" (pristine: GCSWEEPMAX * GCSWEEPCOST) */\n"),
    1079: (b"      g->gcstate = GCScallfin;\n",
           b"      g->gcstate = GCScallfin;  isaac_pace_open(g);  "
           b"/* " + _TAG + b": schedule offset */\n"),
    1133: (b"  do {  /* repeat until pause or enough \"credit\" "
           b"(negative debt) */\n",
           b"  isaac_pace_budget = isaac_lua_gc_scripted(g) ? -1 : debt;  "
           b"do {  /* repeat until pause or enough \"credit\" "
           b"(negative debt); " + _TAG + b": -1 = pristine step, a latched "
           b"state */\n"),
    1138: (b"    setpause(g);  /* pause until next cycle */\n",
           b"    { long long isaac_pace_v0 = isaac_pace_vend(g); setpause(g); "
           b"isaac_pace_late(g, isaac_pace_v0); }  /* pause until next cycle, "
           b"" + _TAG + b": no later than the schedule */\n"),
    1140: (b"    debt = (debt / g->gcstepmul) * STEPMULADJ;  "
           b"/* convert 'work units' to Kb */\n",
           b"    debt = (debt / g->gcstepmul) * STEPMULADJ;  "
           b"if (debt < -GCSTEPSIZE && isaac_pace_budget >= 0 && "
           b"g->gcstate == GCScallfin) debt = -GCSTEPSIZE;  "
           b"/* convert 'work units' to Kb; " + _TAG + b": a paced finalizer "
           b"step keeps at most GCSTEPSIZE bytes of credit */\n"),
    1141: (b"    luaE_setdebt(g, debt);\n",
           b"    isaac_pace_setdebt(g, debt);\n"),
    1142: (b"    runafewfinalizers(L);\n",
           b"    { l_mem isaac_pace_d = g->GCdebt; runafewfinalizers(L); "
           b"if (isaac_pace_budget >= 0) "
           b"luaE_setdebt(g, isaac_pace_d); }  /* " + _TAG +
           b": a paced step's debt, and V, are as before its tail "
           b"finalizers (pristine: runafewfinalizers(L)) */\n"),
}

# lbaselib.c line: (pristine bytes, derived bytes)
_BASELIB_EDITS: dict[int, tuple[bytes, bytes]] = {
    172: (b"\n",
          b"LUAI_FUNC void isaac_lua_gc_script_control (lua_State *L, "
          b"int what);  /* " + _TAG + b": the derived lgc.c:61 "
          b"(pristine: blank line) */\n"),
    182: (b"  int res = lua_gc(L, o, ex);\n",
          b"  int res = (isaac_lua_gc_script_control(L, o), "
          b"lua_gc(L, o, ex));  /* " + _TAG + b": a script that controls the "
          b"collector gets pristine 5.3.3's (pristine: lua_gc(L, o, ex)) */\n"),
}

# with --alloc-latch A > 0 only: line: (pristine bytes, derived bytes); 958
# is the derived line of _EDITS with the per-cycle reset appended
_ALLOC_EDIT = (
    b"    status = luaD_pcall(L, dothecall, NULL, "
    b"savestack(L, L->top - 2), 0);\n",
    b"    { lu_mem isaac_pace_tb = gettotalbytes(g); "
    b"status = luaD_pcall(L, dothecall, NULL, savestack(L, L->top - 2), 0); "
    b"isaac_pace_fin_grew(L, isaac_pace_tb); }  /* " + _TAG +
    b": ISAAC_VITA_LUA_GC_FINALIZER_ALLOC_LATCH_BYTES, a cycle whose "
    b"finalizers allocate past the budget latches the state to pristine "
    b"(pristine: the luaD_pcall alone) */\n")
_ALLOC_RESET = (b"g->GCmemtrav = 0;  ",
                b"g->GCmemtrav = 0;  isaac_pace_fin_bytes = 0;  ")

# with --state-profile only (ISAAC_VITA_LUA_GC_STATE_PROFILE): marks for the
# host's per-gcstate time split (recomp/runtime/host_vita_lua_gc.c).  Nothing
# here reads or writes collector state: the three hooks are extern calls with
# values copied out of the global_State, and isaac_gcsp_cls only remembers
# the last class marked so that a run of singlesteps in one gcstate costs one
# compare each.  Applied AFTER the pace edits, to the derived line: (line,
# text that must occur exactly once in it, replacement).  Class ids: a
# singlestep is marked with its entry gcstate (lgc.h:39-46, GCSpropagate 0 ..
# GCSpause 7), the four cuts inside atomic() are 8..11 and the tail
# runafewfinalizers of luaC_step is 12.
_STATE_TAG = b"ISAAC_VITA_LUA_GC_STATE_PROFILE"
_STATE_DECL = (
    b"  extern void isaac_lua_gc_state_begin (l_mem debt, lu_mem estimate, "
    b"lu_mem total, int gcstate); "
    b"extern void isaac_lua_gc_state_mark (int cls, lu_mem info); "
    b"extern void isaac_lua_gc_state_end (l_mem debt, int gcstate); "
    b"static int isaac_gcsp_cls;  /* " + _STATE_TAG + b" hooks */\n")
_STATE_EDITS: tuple[tuple[int, bytes, bytes], ...] = (
    (994, b"  propagateall(g);  /* propagate changes */\n",
     b"  propagateall(g);  isaac_gcsp_cls = 8; "
     b"isaac_lua_gc_state_mark(8, g->GCmemtrav);  /* propagate changes; "
     b"" + _STATE_TAG + b": 'grayagain' next, info = bytes traversed from "
     b"the roots */\n"),
    (997, b"  propagateall(g);  /* traverse 'grayagain' list */\n",
     b"  propagateall(g);  isaac_gcsp_cls = 9; "
     b"isaac_lua_gc_state_mark(9, g->GCmemtrav - work);  /* traverse "
     b"'grayagain' list; " + _STATE_TAG + b": weak tables next, info = "
     b"'grayagain' bytes */\n"),
    (1005, b"  work += g->GCmemtrav;  /* stop counting (objects being "
     b"finalized) */\n",
     b"  work += g->GCmemtrav;  isaac_gcsp_cls = 10; "
     b"isaac_lua_gc_state_mark(10, g->GCmemtrav);  /* stop counting "
     b"(objects being finalized); " + _STATE_TAG + b": finalizer "
     b"separation next, info = ephemeron bytes */\n"),
    (1009, b"  propagateall(g);  /* remark, to propagate 'resurrection' */\n",
     b"  propagateall(g);  isaac_gcsp_cls = 11; "
     b"isaac_lua_gc_state_mark(11, g->GCmemtrav);  /* remark, to propagate "
     b"'resurrection'; " + _STATE_TAG + b": the atomic tail next, info = "
     b"bytes since the weak cut */\n"),
    (1133, b"  isaac_pace_budget = ",
     b"  isaac_gcsp_cls = -1; isaac_lua_gc_state_begin(g->GCdebt, "
     b"g->GCestimate, gettotalbytes(g), g->gcstate);  "
     b"isaac_pace_budget = "),
    (1134, b"    lu_mem work = singlestep(L);  /* perform one single step "
     b"*/\n",
     b"    lu_mem work; if (g->gcstate != isaac_gcsp_cls) { isaac_gcsp_cls = "
     b"g->gcstate; isaac_lua_gc_state_mark(isaac_gcsp_cls, 0); } "
     b"work = singlestep(L);  /* perform one single step; " + _STATE_TAG +
     b": mark a gcstate change */\n"),
    (1142, b"    { l_mem isaac_pace_d = g->GCdebt; ",
     b"    isaac_lua_gc_state_mark(12, 0); "
     b"{ l_mem isaac_pace_d = g->GCdebt; "),
    (1143, b"  }\n",
     b"  }  isaac_lua_gc_state_end(g->GCdebt, g->gcstate);  /* " +
     _STATE_TAG + b" */\n"),
)
STATE_PROFILE_LINES = (60,) + tuple(line for line, unused, unused2
                                    in _STATE_EDITS)

# 1-based lines the derivation rewrites (39 only with a sweep quota, 821 only
# with an allocation budget)
SWEEP_LINE = 39
CHANGED_LINES = tuple(sorted(line for line in _EDITS if line != SWEEP_LINE))
PRISTINE_LINES = {line: old for line, (old, unused) in _EDITS.items()}
BASELIB_CHANGED_LINES = tuple(sorted(_BASELIB_EDITS))
BASELIB_PRISTINE_LINES = {line: old
                          for line, (old, unused) in _BASELIB_EDITS.items()}


class DeriveError(Exception):
    pass


def derive(pristine: bytes, max_per_call: int = MAX_DEFAULT,
           fin_step: int = FIN_STEP_DEFAULT,
           sweep_step: int = SWEEP_STEP_DEFAULT,
           alloc_latch: int = ALLOC_LATCH_DEFAULT,
           state_profile: bool = False) -> bytes:
    """Return the paced lgc.c for the given constants (bytes in, bytes out)."""
    actual = hashlib.sha256(pristine).hexdigest()
    if actual != PRISTINE_LGC_SHA256:
        raise DeriveError(f"input is not pristine Lua 5.3.3 lgc.c: {actual}")
    if not MAX_MIN <= max_per_call <= MAX_MAX:
        raise DeriveError(f"max must be {MAX_MIN}..{MAX_MAX}: {max_per_call}")
    if not FIN_STEP_MIN <= fin_step <= FIN_STEP_MAX:
        raise DeriveError(
            f"finalizer step must be {FIN_STEP_MIN}..{FIN_STEP_MAX}: "
            f"{fin_step}")
    if sweep_step and not SWEEP_STEP_MIN <= sweep_step <= SWEEP_STEP_MAX:
        raise DeriveError(
            f"sweep step must be 0 or {SWEEP_STEP_MIN}..{SWEEP_STEP_MAX}: "
            f"{sweep_step}")
    if alloc_latch and not ALLOC_LATCH_MIN <= alloc_latch <= ALLOC_LATCH_MAX:
        raise DeriveError(
            f"alloc latch must be 0 or {ALLOC_LATCH_MIN}..{ALLOC_LATCH_MAX}: "
            f"{alloc_latch}")
    fill = {39: (sweep_step,), 45: (fin_step, 2 * max_per_call),
            848: (max_per_call, max_per_call)}
    lines = pristine.splitlines(keepends=True)
    for line, (old, new) in _EDITS.items():
        if line == SWEEP_LINE and not sweep_step:
            continue
        if lines[line - 1] != old:
            raise DeriveError(f"line {line} is not {old!r}")
        if line == 60:
            new = _helpers()
        elif line == 61:
            new = _latch(alloc_latch)
        elif line in fill:
            new = new % fill[line]
        if line == 958 and alloc_latch:
            if new.count(_ALLOC_RESET[0]) != 1:
                raise DeriveError("line 958 lost its reset anchor")
            new = new.replace(*_ALLOC_RESET)
        lines[line - 1] = new
    if alloc_latch:
        if lines[ALLOC_LATCH_LINE - 1] != _ALLOC_EDIT[0]:
            raise DeriveError(
                f"line {ALLOC_LATCH_LINE} is not {_ALLOC_EDIT[0]!r}")
        lines[ALLOC_LATCH_LINE - 1] = _ALLOC_EDIT[1]
    if state_profile:
        if not lines[59].endswith(b"\n") or b"isaac_gcsp_cls" in lines[59]:
            raise DeriveError("line 60 cannot take the state-profile hooks")
        lines[59] = lines[59][:-1] + _STATE_DECL
        for line, old, new in _STATE_EDITS:
            if lines[line - 1].count(old) != 1:
                raise DeriveError(
                    f"state profile: line {line} does not hold {old!r} once")
            lines[line - 1] = lines[line - 1].replace(old, new)
    text = b"".join(lines)
    if text.count(b"\n") != pristine.count(b"\n"):
        raise DeriveError("derivation changed the line count")
    return text


def derive_baselib(pristine: bytes) -> bytes:
    """Return the derived lbaselib.c (no constants: the latch call only)."""
    actual = hashlib.sha256(pristine).hexdigest()
    if actual != PRISTINE_LBASELIB_SHA256:
        raise DeriveError(
            f"input is not pristine Lua 5.3.3 lbaselib.c: {actual}")
    lines = pristine.splitlines(keepends=True)
    for line, (old, new) in _BASELIB_EDITS.items():
        if lines[line - 1] != old:
            raise DeriveError(f"lbaselib.c line {line} is not {old!r}")
        lines[line - 1] = new
    text = b"".join(lines)
    if text.count(b"\n") != pristine.count(b"\n"):
        raise DeriveError("derivation changed the lbaselib.c line count")
    return text


def _write(path: Path, data: bytes, newest_input: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # Equal bytes are left alone (a reconfigure does not recompile), unless
    # the output is older than an input: the CMake build rule
    # (add_custom_command) then reruns until the output is newer.
    if (not path.is_file() or path.read_bytes() != data or
            path.stat().st_mtime_ns < newest_input):
        path.write_bytes(data)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--input", required=True, type=Path,
                        help="pristine lgc.c")
    parser.add_argument("--output", required=True, type=Path,
                        help="derived lgc.c")
    # The derived lgc.c is only half of the knob: without the derived
    # lbaselib.c no script ever latches, so both are required.
    parser.add_argument("--baselib-input", required=True, type=Path,
                        help="pristine lbaselib.c")
    parser.add_argument("--baselib-output", required=True, type=Path,
                        help="derived lbaselib.c")
    parser.add_argument("--max", type=int, default=MAX_DEFAULT)
    parser.add_argument("--fin-step", type=int, default=FIN_STEP_DEFAULT)
    parser.add_argument("--sweep-step", type=int, default=SWEEP_STEP_DEFAULT)
    parser.add_argument("--alloc-latch", type=int, default=ALLOC_LATCH_DEFAULT,
                        help="ISAAC_VITA_LUA_GC_FINALIZER_ALLOC_LATCH_BYTES "
                             "(0 = no guard)")
    parser.add_argument("--state-profile", action="store_true",
                        help="ISAAC_VITA_LUA_GC_STATE_PROFILE: add the "
                             "per-gcstate time marks (the output without "
                             "this flag is unchanged)")
    args = parser.parse_args(argv)
    try:
        derived = derive(args.input.read_bytes(), args.max, args.fin_step,
                         args.sweep_step, args.alloc_latch,
                         args.state_profile)
        baselib = derive_baselib(args.baselib_input.read_bytes())
    except (OSError, DeriveError) as exc:
        print(f"lua53_gc_finalizer_pace: {exc}", file=sys.stderr)
        return 1
    script = Path(__file__).stat().st_mtime_ns
    _write(args.output, derived, max(args.input.stat().st_mtime_ns, script))
    _write(args.baselib_output, baselib,
           max(args.baselib_input.stat().st_mtime_ns, script))
    print(f"lua53_gc_finalizer_pace: max={args.max} fin_step={args.fin_step} "
          f"sweep_step={args.sweep_step} "
          + (f"alloc_latch={args.alloc_latch} " if args.alloc_latch else "")
          + ("state_profile=1 " if args.state_profile else "") +
          f"sha256={hashlib.sha256(derived).hexdigest()} "
          f"lbaselib_sha256={hashlib.sha256(baselib).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
