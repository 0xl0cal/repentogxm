#!/usr/bin/env python3
"""Derive the finalizer-paced lgc.c for ISAAC_VITA_LUA_GC_FINALIZER_PACE.

Pristine Lua 5.3.3 runs the finalizers of a collection cycle in a burst.
lgc.c:841-850 `runafewfinalizers` doubles `g->gcfinnum` on every call
(1, 2, 4, ...) and lgc.c:45 accounts a finalizer as GCFINALIZECOST =
GCSWEEPCOST = (sizeof(TString) + 4) / 4 = 5 work units on a 32-bit target.
One paced LUA_GCSTEP of K KB (K * 1024 work units, lapi.c:1076) therefore
runs up to ~K * 205 finalizers, so every pending finalizer of a cycle lands in
one luaC_step.  With EID that is ~1,700 LuaBridge `__gc` callbacks into
translated code per cycle, measured 13-18 ms in one step on the console
(device GC profile of 2026-09-07, fin max_step 1435-1447).

The derivation changes two lines of the pristine file (three with a sweep
cost other than the pristine 5) and nothing else (line numbers are
preserved):

* lgc.c:45 GCFINALIZECOST becomes COST work units, so a K KB step
  (K * 1024 units) pays for at most ~K * 1024 / COST finalizers.  CMake
  default 512 (ISAAC_VITA_LUA_GC_FINALIZER_COST): on the console one LuaBridge
  finalizer costs 5-20 us and one work unit ~8.9 ns, i.e. 560-2240 units, so
  512 still under-charges a finalizer and keeps the cycle short when the
  finalizer rate rises (measured: 1024 doubled the stress-case heap peak);
* lgc.c:847-848 the doubling of `gcfinnum` saturates at MAX (CMake default
  8, ISAAC_VITA_LUA_GC_FINALIZER_MAX), so one runafewfinalizers call -- one
  singlestep in GCScallfin (lgc.c:1082-1085) or the unaccounted tail call of
  luaC_step (lgc.c:1142) -- runs at most MAX.  Together: one luaC_step runs
  at most (budget + GCSTEPSIZE) / COST + 2 * MAX finalizers.
* lgc.c:39 GCSWEEPCOST becomes SWEEP work units per swept object
  (ISAAC_VITA_LUA_GC_SWEEP_COST; the default 5 is the pristine
  (sizeof(TString) + 4) / 4 on a 32-bit target and leaves the line
  untouched).  sweepstep (lgc.c:1026-1038) sweeps GCSWEEPMAX =
  (GCSTEPSIZE / SWEEP) / 4 objects (lgc.c:42) and charges
  GCSWEEPMAX * SWEEP, so a K KB step sweeps ~K * 1024 / SWEEP objects.
  The console sweeps one object in ~0.28-0.57 us (measured) against ~8.9 ns
  per marked byte, i.e. 31-64 units, so pristine 5 lets one 128 KB step
  sweep ~26,000 objects: the 11-15 ms GC steps without finalizers in the
  device GC profile.  SWEEP must stay <= 400: above it GCSWEEPMAX is 0, a
  sweep singlestep does no work and charges none, and luaC_step never
  returns.  The finalizer cost of line 45 no longer follows GCSWEEPCOST (it
  is COST).

Which objects are finalized, the order (tobefnz head first, udata2finalize/GCTM lgc.c:787-836),
error propagation (GCTM's propagateerrors path), emergency collections,
luaC_fullgc / lua_close (callallpendingfinalizers, lgc.c:856) are
unchanged; only *when* a pending finalizer runs moves later.  This is the
policy Lua 5.4 adopted (lgc.c 5.4.6: GCFINMAX 10, GCFINALIZECOST 50, no
doubling), with constants scaled to this port's paced step.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys

# lua53_vanilla.cmake: "lgc.c|6f1e1801...".  test_vita_lua_gc_finalizer_pace.py
# checks that this copy still matches the manifest record.
PRISTINE_LGC_SHA256 = (
    "6f1e180167b0276ae063d2ba32d94b46c26455218f46ae12f8f222b9edd1e932")

MAX_MIN, MAX_MAX = 1, 4096
COST_MIN, COST_MAX = 1, 65536
# GCSWEEPMAX = (GCSTEPSIZE / SWEEP) / 4 with GCSTEPSIZE = 1600 (lgc.h:32 on a
# 32-bit target) must stay >= 1, or sweepstep returns 0 work forever.
SWEEP_MIN, SWEEP_MAX = 1, 400
SWEEP_PRISTINE = 5   # (sizeof(TString) + 4) / 4, sizeof(TString) = 16

_COST_OLD = b"#define GCFINALIZECOST\tGCSWEEPCOST\n"
_COST_NEW = (b"#define GCFINALIZECOST\t%d  /* ISAAC_VITA_LUA_GC_FINALIZER_PACE "
             b"(pristine: GCSWEEPCOST) */\n")
_DOUBLE_OLD = (b"                    : g->gcfinnum * 2;  "
               b"/* else call a few more next time */\n")
_DOUBLE_NEW = (b"                    : (g->gcfinnum * 2 > %du ? %du : "
               b"g->gcfinnum * 2);  /* ISAAC_VITA_LUA_GC_FINALIZER_PACE cap "
               b"(pristine: doubles) */\n")
_SWEEP_OLD = b"#define GCSWEEPCOST\t((sizeof(TString) + 4) / 4)\n"
_SWEEP_NEW = (b"#define GCSWEEPCOST\t%d  /* ISAAC_VITA_LUA_GC_SWEEP_COST "
              b"(pristine: (sizeof(TString) + 4) / 4 = 5) */\n")


class DeriveError(Exception):
    pass


def derive(pristine: bytes, max_per_call: int, cost: int,
           sweep_cost: int = SWEEP_PRISTINE) -> bytes:
    """Return the paced lgc.c for the given constants (bytes in, bytes out)."""
    actual = hashlib.sha256(pristine).hexdigest()
    if actual != PRISTINE_LGC_SHA256:
        raise DeriveError(f"input is not pristine Lua 5.3.3 lgc.c: {actual}")
    if not MAX_MIN <= max_per_call <= MAX_MAX:
        raise DeriveError(f"max must be {MAX_MIN}..{MAX_MAX}: {max_per_call}")
    if not COST_MIN <= cost <= COST_MAX:
        raise DeriveError(f"cost must be {COST_MIN}..{COST_MAX}: {cost}")
    if not SWEEP_MIN <= sweep_cost <= SWEEP_MAX:
        raise DeriveError(
            f"sweep cost must be {SWEEP_MIN}..{SWEEP_MAX}: {sweep_cost}")
    text = pristine
    edits = [(_COST_OLD, _COST_NEW % cost),
             (_DOUBLE_OLD, _DOUBLE_NEW % (max_per_call, max_per_call))]
    if sweep_cost != SWEEP_PRISTINE:
        edits.append((_SWEEP_OLD, _SWEEP_NEW % sweep_cost))
    for old, new in edits:
        if text.count(old) != 1:
            raise DeriveError(f"anchor not found exactly once: {old!r}")
        text = text.replace(old, new)
    if text.count(b"\n") != pristine.count(b"\n"):
        raise DeriveError("derivation changed the line count")
    return text


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--max", required=True, type=int)
    parser.add_argument("--cost", required=True, type=int)
    parser.add_argument("--sweep-cost", type=int, default=SWEEP_PRISTINE)
    args = parser.parse_args(argv)
    try:
        derived = derive(args.input.read_bytes(), args.max, args.cost,
                         args.sweep_cost)
    except (OSError, DeriveError) as exc:
        print(f"lua53_gc_finalizer_pace: {exc}", file=sys.stderr)
        return 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.is_file() or args.output.read_bytes() != derived:
        args.output.write_bytes(derived)
    print(f"lua53_gc_finalizer_pace: max={args.max} cost={args.cost} "
          f"sweep_cost={args.sweep_cost} "
          f"sha256={hashlib.sha256(derived).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
