#!/usr/bin/env python3
"""Summarise the Lua GC device-gate records of a Vita log.

Three record streams, each split into runs on its own:

  ISAAC_VITA_LUA_GC_PROFILE, one record per 120 LuaEngine::Update steps
  (recomp/runtime/host_vita_lua_gc.c):
    [isaac-lua] gc window: win=N gcstep(n,sum_us,p95_us,max_us)=...
        fin(n,max_step,cb)=... count=KB KB arena(live,peak,fb)=...
        kb(req,app)=... collect(clamp,full,us)=...
    A run starts where win restarts.  Its fin counts only finalizers entered
    inside a lua_gc import (the paced step and GCCOLLECT); finalizers run by
    allocation-driven steps are invisible here (about half of
    them with ISAAC_VITA_LUA_GC_FINALIZER_PACE).
  ISAAC_VITA_LUA_COST_PROFILE, one record per 2 s
  (recomp/runtime/host_vita_lua_cost.c):
    [isaac-lua] cost win=N ms=... fin=... gcvm=n/us gccb=n/us gcext=n/us
    fin counts every finalizer, whichever GC step ran it; gcvm/gccb/gcext is
    every outermost luaC_step (inside a pcall, inside a callback, outside any
    pcall = the paced step).  A run starts where win restarts.
  tools/isaac_vita_lua_gc_stress (game log.txt, Isaac.DebugString):
    [gcstress] armed per_update=... run_for=... drain=... report_every=...
    [gcstress] [done ]frame=F ud=U sentinels=S finalized=N backlog=B
        lag_max=L lag_mean=M kb=K
    A run starts at each "armed" line.  A stress run fails the log (exit 1)
    when it has fewer than 2 samples in its stress phase, no heap data
    (kb=-1), a growing heap (second-half max more than KB_GROWTH_LIMIT KB
    above the first half's) or a growing backlog (BACKLOG_GROWTH_LIMIT
    sentinels), no "done" line, or a backlog left at done:
    "stress=FAIL (...)".

In-game GC windows are, by default, those whose guest pacer asks for at
least --ingame-req-kb per step (the pacer sits at its 50 KB floor in the menu
and ramps to 1-10 MB per step in a run with EID).  The request follows the
guest's GCCOUNT feedback, so a build that changes the GC (pacing, clamp) can
move borderline windows across the split; --ingame-from-win N instead takes
every window from N on, which does not depend on the GC (use it for a
scripted route whose menu phase has a known length, and compare the
in-game window counts of the two builds either way).

Per GC run it prints what the device gate compares: GC time per step,
finalizers per step, the largest step per window (p50/p90/p99/max, windows
over 4 and 8 ms), the largest finalizer batch in one lua_gc call, the Lua
heap (max, and first-half vs second-half max of the in-game windows: a heap
that is still growing shows up as growth), the arena peak and fallbacks.
With --pace-step Q --pace-max M it also checks the derived build's per-step
bound floor(5Q/4) + 2M (lgc.c lines 44/45/848/1140; 256 for the
defaults) in every window without a full collection (GCCOLLECT run as a full
collection drains tobefnz in one call, as pristine does) and without an arena
fallback: an emergency collection (lmem.c:87-92, luaC_fullgc(L, 1)) needs a
failed allocation, which with the Lua arena is a failed fallback, and the
backlog it leaves doubles uncapped outside GCSswpallgc..GCScallfin (lgc.c:848
by design), so from the first window with fb > 0 (fb is cumulative) the
windows are counted as emergency-suspect, not judged.  Without the arena
(arena peak 0) an emergency leaves no trace in the record and is judged like
any window.  A Lua state a script latched (collectgarbage with an
option other than "count"/"isrunning") runs pristine's steps, which are not
bounded either; the route and the stress mod call neither, so a window over
the bound there is a finding.

usage: python lua_gc_window_report.py LOG [LOG ...] [--ingame-req-kb 200]
           [--ingame-from-win N] [--pace-step 192 --pace-max 8]
       python lua_gc_window_report.py --self-test
"""

from __future__ import annotations

import argparse
import contextlib
import io
from pathlib import Path
import re
import statistics
import sys
import tempfile

RECORD = re.compile(
    r"gc window: win=(\d+) gcstep\(n,sum_us,p95_us,max_us\)=(\d+),(\d+),(\d+),"
    r"(\d+) fin\(n,max_step,cb\)=(\d+),(\d+),(\d+) count=(\d+) KB "
    r"arena\(live,peak,fb\)=(\d+),(\d+),(\d+) kb\(req,app\)=(\d+),(\d+)"
    r"(?: collect\(clamp,full,us\)=(\d+),(\d+),(\d+))?")
FIELDS = ("win", "n", "sum_us", "p95_us", "max_us", "fin", "fin_step", "cb",
          "count_kb", "live_kb", "peak_kb", "fb", "req_kb", "app_kb",
          "collect_clamp", "collect_full", "collect_us")
COST = re.compile(
    r"\[isaac-lua\] cost win=(\d+) ms=(\d+) pc=\d+/\d+ cb=\d+/\d+ nest=\d+ "
    r"fin=(\d+) out=\d+ lost=\d+ gcvm=(\d+)/(\d+) gccb=(\d+)/(\d+) "
    r"gcext=(\d+)/(\d+)")
COST_FIELDS = ("win", "ms", "fin", "vm_n", "vm_us", "cb_n", "cb_us", "ext_n",
               "ext_us")
ARMED = re.compile(r"\[gcstress\] armed per_update=(\d+) run_for=(\d+) "
                   r"drain=(\d+) report_every=(\d+)")
STRESS = re.compile(
    r"\[gcstress\] (done )?frame=(\d+) ud=(\d+) sentinels=(\d+) "
    r"finalized=(\d+) backlog=(-?\d+) lag_max=(\d+) lag_mean=([0-9.]+) "
    r"kb=(-?\d+)")
KB_GROWTH_LIMIT = 512        # the stress run's heap criterion
NO_ARENA = " (no arena: emergencies invisible)"
BACKLOG_GROWTH_LIMIT = 64    # sentinels: about 2 s of updates


def runs(lines) -> list[list[dict[str, int]]]:
    """GC window records, one list per run (win restarts)."""
    out: list[list[dict[str, int]]] = []
    last = None
    for line in lines:
        match = RECORD.search(line)
        if not match:
            continue
        row = dict(zip(FIELDS, (int(v) if v is not None else 0
                                for v in match.groups())))
        if last is None or row["win"] <= last:
            out.append([])
        out[-1].append(row)
        last = row["win"]
    return out


def cost_runs(lines) -> list[list[dict[str, int]]]:
    out: list[list[dict[str, int]]] = []
    last = None
    for line in lines:
        match = COST.search(line)
        if not match:
            continue
        row = dict(zip(COST_FIELDS, (int(v) for v in match.groups())))
        if last is None or row["win"] <= last:
            out.append([])
        out[-1].append(row)
        last = row["win"]
    return out


def stress_runs(lines) -> list[dict]:
    out: list[dict] = []
    for line in lines:
        armed = ARMED.search(line)
        if armed:
            per_update, run_for, drain, every = (int(v) for v in armed.groups())
            out.append({"per_update": per_update, "run_for": run_for,
                        "drain": drain, "every": every, "rows": [],
                        "done": None})
            continue
        match = STRESS.search(line)
        if not match or not out:
            continue
        done, *values = match.groups()
        row = dict(zip(("frame", "ud", "sentinels", "finalized", "backlog",
                        "lag_max", "lag_mean", "kb"),
                       (float(v) if "." in v else int(v) for v in values)))
        if done:
            out[-1]["done"] = row
        else:
            out[-1]["rows"].append(row)
    return out


def quantile(values: list[int], q: float) -> int:
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(q * len(ordered)))]


def halves(values: list[int]) -> tuple[int, int]:
    """Max of the first and of the second half (arrival order)."""
    mid = len(values) // 2
    first = values[:mid] or values
    return max(first), max(values[mid:])


def pace_bound(step: int, most: int) -> int:
    return 5 * step // 4 + 2 * most


def summary(rows: list[dict[str, int]], ingame_req_kb: int,
            ingame_from_win: int | None = None,
            bound: int | None = None) -> dict[str, float]:
    if ingame_from_win is None:
        game = [r for r in rows
                if r["n"] and r["req_kb"] >= ingame_req_kb * r["n"]]
    else:
        game = [r for r in rows if r["n"] and r["win"] >= ingame_from_win]
    result: dict[str, float] = {"windows": len(rows), "ingame": len(game)}
    if bound is not None:
        unfull = [r for r in rows if not r["collect_full"]]
        judged = [r for r in unfull if not r["fb"]]
        result.update(
            bound=bound, full_windows=len(rows) - len(unfull),
            emergency_suspect_windows=len(unfull) - len(judged),
            emergency_visible=any(r["peak_kb"] for r in rows),
            fin_step_max_judged=max((r["fin_step"] for r in judged), default=0),
            windows_over_bound=sum(r["fin_step"] > bound for r in judged))
    if not game:
        return result
    steps = sum(r["n"] for r in game)
    worst = [r["max_us"] for r in game]
    first_kb, second_kb = halves([r["count_kb"] for r in game])
    result.update(
        gc_us_per_step=sum(r["sum_us"] for r in game) / steps,
        gc_us_per_step_median=statistics.median(r["sum_us"] / r["n"] for r in game),
        fin_per_step=sum(r["fin"] for r in game) / steps,
        applied_kb_per_step=sum(r["app_kb"] for r in game) / steps,
        max_us_p50=quantile(worst, 0.50), max_us_p90=quantile(worst, 0.90),
        max_us_p99=quantile(worst, 0.99), max_us_max=max(worst),
        windows_over_4ms=sum(v > 4000 for v in worst),
        windows_over_8ms=sum(v > 8000 for v in worst),
        p95_us_median=statistics.median(r["p95_us"] for r in game),
        fin_step_max=max(r["fin_step"] for r in game),
        count_kb_max=max(r["count_kb"] for r in game),
        count_kb_first_max=first_kb, count_kb_second_max=second_kb,
        count_kb_growth=second_kb - first_kb,
        peak_kb_max=max(r["peak_kb"] for r in rows),
        fb_max=max(r["fb"] for r in rows),
    )
    return result


def cost_summary(rows: list[dict[str, int]]) -> dict[str, float]:
    seconds = sum(r["ms"] for r in rows) / 1000.0
    result: dict[str, float] = {"windows": len(rows), "seconds": seconds}
    if seconds <= 0:
        return result
    for kind in ("vm", "cb", "ext"):
        result[f"{kind}_steps_per_s"] = sum(r[f"{kind}_n"] for r in rows) / seconds
        result[f"{kind}_ms_per_s"] = (sum(r[f"{kind}_us"] for r in rows) /
                                      1000.0 / seconds)
    result["gc_ms_per_s"] = sum(result[f"{k}_ms_per_s"]
                                for k in ("vm", "cb", "ext"))
    result["fin_per_s"] = sum(r["fin"] for r in rows) / seconds
    result["fin_per_window_max"] = max(r["fin"] for r in rows)
    return result


def stress_summary(run: dict) -> dict[str, float]:
    rows = [r for r in run["rows"] if r["frame"] <= run["run_for"]]
    result: dict[str, float] = {"samples": len(run["rows"]),
                                "stress_samples": len(rows),
                                "done": run["done"] is not None}
    if len(rows) >= 2:
        span = rows[-1]["frame"] - rows[0]["frame"]
        result["ud_per_update"] = ((rows[-1]["ud"] - rows[0]["ud"]) / span
                                   if span else 0.0)
        kb = [r["kb"] for r in rows]
        backlog = [r["backlog"] for r in rows]
        result["kb_first_max"], result["kb_second_max"] = halves(kb)
        result["backlog_first_max"], result["backlog_second_max"] = halves(backlog)
        # kb=-1: the mod had no collectgarbage (main.lua heapKb), no data
        result["kb_nodata"] = min(kb) < 0
        result["kb_plateau"] = (not result["kb_nodata"] and
                                result["kb_second_max"] <=
                                result["kb_first_max"] + KB_GROWTH_LIMIT)
        result["backlog_plateau"] = (result["backlog_second_max"] <=
                                     result["backlog_first_max"] +
                                     BACKLOG_GROWTH_LIMIT)
    every = run["rows"] + ([run["done"]] if run["done"] else [])
    if every:
        result["lag_max"] = max(r["lag_max"] for r in every)
        result["kb_max"] = max(r["kb"] for r in every)
    if run["done"]:
        result["done_backlog"] = run["done"]["backlog"]
        result["drained"] = run["done"]["backlog"] == 0
    return result


def render(name: str, index: int, s: dict[str, float]) -> str:
    head = f"{name} run {index}: windows={s['windows']} ingame={s['ingame']}"
    tail = ""
    if "bound" in s:
        verdict = "ok" if not s["windows_over_bound"] else "FAIL"
        tail = (f" pace_bound={s['bound']} fin_step_max_judged="
                f"{s['fin_step_max_judged']} windows_over_bound="
                f"{s['windows_over_bound']} full_collect_windows="
                f"{s['full_windows']} emergency_suspect_windows="
                f"{s['emergency_suspect_windows']}"
                f"{'' if s['emergency_visible'] else NO_ARENA}"
                f" bound={verdict}")
    if not s["ingame"]:
        return head + " (no in-game windows)" + tail
    return (f"{head} gc_us/step={s['gc_us_per_step']:.0f} "
            f"(median window {s['gc_us_per_step_median']:.0f}) "
            f"fin/step={s['fin_per_step']:.2f} "
            f"applied_kb/step={s['applied_kb_per_step']:.0f} "
            f"max_us p50/p90/p99/max={s['max_us_p50']}/{s['max_us_p90']}/"
            f"{s['max_us_p99']}/{s['max_us_max']} "
            f"windows>4ms={s['windows_over_4ms']} >8ms={s['windows_over_8ms']} "
            f"p95_us_median={s['p95_us_median']:.0f} "
            f"fin_step_max={s['fin_step_max']} count_kb_max={s['count_kb_max']} "
            f"count_kb_halves={s['count_kb_first_max']}/"
            f"{s['count_kb_second_max']} (growth {s['count_kb_growth']}) "
            f"arena_peak_kb={s['peak_kb_max']} fb_max={s['fb_max']}" + tail)


def render_cost(name: str, index: int, s: dict[str, float]) -> str:
    head = f"{name} cost run {index}: windows={s['windows']} s={s['seconds']:.0f}"
    if "gc_ms_per_s" not in s:
        return head
    return (f"{head} fin/s={s['fin_per_s']:.0f} "
            f"fin_per_window_max={s['fin_per_window_max']} "
            f"gc_ms/s={s['gc_ms_per_s']:.2f} (vm {s['vm_ms_per_s']:.2f}, cb "
            f"{s['cb_ms_per_s']:.2f}, ext {s['ext_ms_per_s']:.2f}) "
            f"steps/s vm/cb/ext={s['vm_steps_per_s']:.1f}/"
            f"{s['cb_steps_per_s']:.1f}/{s['ext_steps_per_s']:.1f}")


def stress_failures(s: dict[str, float]) -> list[str]:
    """Why a stress run fails the log (without these checks a growing heap
    printed kb_plateau=NO and still exited 0, and a run with 0 or 1
    samples, no done line or an undrained backlog printed no verdict and
    exited 0)."""
    reasons: list[str] = []
    if "kb_plateau" not in s:
        reasons.append(f"{s['stress_samples']} stress sample(s), need 2")
    else:
        if s["kb_nodata"]:
            reasons.append("no heap data")
        elif not s["kb_plateau"]:
            reasons.append("heap grows")
        if not s["backlog_plateau"]:
            reasons.append("backlog grows")
    if not s["done"]:
        reasons.append("no done line")
    elif not s["drained"]:
        reasons.append("backlog not drained")
    return reasons


def render_stress(name: str, index: int, s: dict[str, float]) -> str:
    head = (f"{name} stress run {index}: samples={s['samples']} "
            f"done={'yes' if s['done'] else 'no'}")
    if "ud_per_update" in s:
        head += (f" ud/update={s['ud_per_update']:.0f} kb_halves="
                 f"{s['kb_first_max']}/{s['kb_second_max']} backlog_halves="
                 f"{s['backlog_first_max']}/{s['backlog_second_max']} "
                 f"kb_plateau={'yes' if s['kb_plateau'] else 'NO'}"
                 f"{' (kb=-1: no heap data)' if s['kb_nodata'] else ''} "
                 f"backlog_plateau={'yes' if s['backlog_plateau'] else 'NO'}")
    failures = stress_failures(s)
    head += (f" stress=FAIL ({', '.join(failures)})" if failures
             else " stress=ok")
    if "lag_max" in s:
        head += f" lag_max={s['lag_max']} kb_max={s['kb_max']}"
    if "drained" in s:
        head += (f" done_backlog={s['done_backlog']} "
                 f"drained={'yes' if s['drained'] else 'NO'}")
    return head


def self_test() -> int:
    def rec(win, n, s, p95, mx, fin, fstep, count, peak, fb, req, app, full=0):
        return (f"[1.0 thr 0x1] [isaac-lua] gc window: win={win} "
                f"gcstep(n,sum_us,p95_us,max_us)={n},{s},{p95},{mx} "
                f"fin(n,max_step,cb)={fin},{fstep},{fin} count={count} KB "
                f"arena(live,peak,fb)={count},{peak},{fb} kb(req,app)={req},{app} "
                f"collect(clamp,full,us)=0,{full},0 other=0 unwind=0")
    # Hand-checked: run 1 = one menu window (50 KB/step) and two in-game
    # windows; run 2 starts when win restarts at 1 and holds a fallback.
    lines = [
        rec(1, 120, 9000, 600, 700, 0, 0, 11215, 17900, 0, 6000, 6000),
        "unrelated line",
        rec(2, 120, 120000, 1800, 13500, 1464, 1435, 11400, 19100, 0, 358560, 15360),
        rec(3, 120, 60000, 1600, 2200, 0, 0, 11500, 19100, 0, 358560, 15360),
        rec(1, 120, 30000, 400, 900, 96, 96, 12000, 19000, 2, 240000, 4800),
    ]
    found = runs(lines)
    assert [len(r) for r in found] == [3, 1], found
    first = summary(found[0], 200)
    assert first["windows"] == 3 and first["ingame"] == 2, first
    assert first["gc_us_per_step"] == (120000 + 60000) / 240, first
    assert first["fin_per_step"] == 1464 / 240, first
    assert first["max_us_max"] == 13500 and first["windows_over_8ms"] == 1, first
    assert first["max_us_p50"] == 13500 and first["fin_step_max"] == 1435, first
    assert first["applied_kb_per_step"] == 128 and first["peak_kb_max"] == 19100
    assert (first["count_kb_first_max"], first["count_kb_second_max"],
            first["count_kb_growth"]) == (11400, 11500, 100), first
    second = summary(found[1], 200)
    assert second["ingame"] == 1 and second["fb_max"] == 2, second
    assert second["applied_kb_per_step"] == 40 and second["windows_over_4ms"] == 0
    menu_only = summary(found[0][:1], 200)
    assert menu_only["ingame"] == 0, menu_only
    # --ingame-from-win: the menu window (win 1) is out whatever it requests
    by_win = summary(found[0], 10**9, ingame_from_win=2)
    assert by_win["ingame"] == 2 and by_win["fin_per_step"] == 1464 / 240
    # pace bound 5*192//4 + 2*8 = 256: 1435 in win 2 is over it, unless that
    # window ran a full collection (excluded, counted)
    assert pace_bound(192, 8) == 256
    judged = summary(found[0], 200, bound=256)
    assert (judged["windows_over_bound"], judged["fin_step_max_judged"],
            judged["full_windows"]) == (1, 1435, 0), judged
    full = runs([rec(1, 120, 9000, 600, 700, 0, 0, 11215, 17900, 0, 6000, 6000),
                 rec(2, 120, 9000, 600, 700, 900, 900, 11215, 17900, 0, 6000,
                     6000, full=1),
                 rec(3, 120, 9000, 600, 700, 250, 250, 11215, 17900, 0, 6000,
                     6000)])
    excused = summary(full[0], 200, bound=256)
    assert (excused["windows_over_bound"], excused["fin_step_max_judged"],
            excused["full_windows"]) == (0, 250, 1), excused
    # from the first window with an arena fallback (fb is
    # cumulative) an emergency collection may have run and its backlog
    # doubles uncapped: those windows are counted, not judged
    spill = runs([rec(1, 120, 9000, 600, 700, 250, 250, 11215, 17900, 0, 6000,
                      6000),
                  rec(2, 120, 9000, 600, 700, 4096, 4096, 11215, 17900, 3, 6000,
                      6000),
                  rec(3, 120, 9000, 600, 700, 900, 900, 11215, 17900, 3, 6000,
                      6000)])
    sp = summary(spill[0], 200, bound=256)
    assert (sp["windows_over_bound"], sp["fin_step_max_judged"],
            sp["emergency_suspect_windows"], sp["emergency_visible"]) == (
                0, 250, 2, True), sp
    assert "emergency_suspect_windows=2 bound=ok" in render("x", 1, sp)
    # without the arena (peak 0) an emergency is invisible: nothing excused
    bare = summary(runs([rec(1, 120, 9000, 600, 700, 900, 900, 11215, 0, 0,
                             6000, 6000)])[0], 200, bound=256)
    assert (bare["windows_over_bound"], bare["emergency_visible"]) == (
        1, False), bare
    assert NO_ARENA + " bound=FAIL" in render("x", 1, bare)
    # old records without the collect group still parse (full = 0)
    legacy = runs([rec(1, 120, 9000, 600, 700, 0, 0, 1, 2, 0, 6000, 6000)
                   .split(" collect(")[0]])
    assert legacy[0][0]["collect_full"] == 0 and legacy[0][0]["count_kb"] == 1
    # cost stream: 2 windows of 2 s, then a restart
    def cost(win, fin, vm, ext):
        return (f"[isaac-lua] cost win={win} ms=2000 pc=10/500 cb=3/100 nest=0 "
                f"fin={fin} out=0 lost=0 gcvm={vm[0]}/{vm[1]} gccb=0/0 "
                f"gcext={ext[0]}/{ext[1]} al=9/1/2 clk=40 ovf=0 ps=4022d0:1/1")
    costs = cost_runs([cost(1, 400, (30, 2000), (120, 6000)),
                       "[isaac-lua] cost-top win=1 top=40eef0:3/100",
                       cost(2, 800, (50, 6000), (120, 10000)),
                       cost(1, 10, (1, 10), (120, 100))])
    assert [len(r) for r in costs] == [2, 1], costs
    c = cost_summary(costs[0])
    assert c["seconds"] == 4 and c["fin_per_s"] == 300, c
    assert c["vm_ms_per_s"] == 2.0 and c["ext_ms_per_s"] == 4.0, c
    assert c["gc_ms_per_s"] == 6.0 and c["fin_per_window_max"] == 800, c
    # stress stream: a line before "armed" is ignored; halves and drain
    def st(frame, ud, made, done_n, lag, kb, done=""):
        return (f"Lua Debug: [gcstress] {done}frame={frame} ud={ud} "
                f"sentinels={made} finalized={done_n} backlog={made - done_n} "
                f"lag_max={lag} lag_mean=12.5 kb={kb}")
    stress = stress_runs([
        st(0, 0, 0, 0, 0, 11000),
        "[gcstress] armed per_update=300 run_for=900 drain=300 report_every=300",
        st(0, 300, 1, 0, 0, 11300),
        st(300, 90300, 301, 280, 40, 13000),
        st(600, 180300, 601, 560, 45, 13200),
        st(900, 270000, 900, 880, 50, 13300),
        st(1200, 270000, 900, 900, 50, 11900, done="done "),
    ])
    assert len(stress) == 1 and len(stress[0]["rows"]) == 4, stress
    s = stress_summary(stress[0])
    assert s["ud_per_update"] == (270000 - 300) / 900, s
    assert (s["kb_first_max"], s["kb_second_max"]) == (13000, 13300), s
    assert (s["backlog_first_max"], s["backlog_second_max"]) == (21, 41), s
    assert s["kb_plateau"] and s["backlog_plateau"] and s["drained"], s
    assert s["lag_max"] == 50 and s["kb_max"] == 13300, s
    growing = stress_runs([
        "[gcstress] armed per_update=300 run_for=900 drain=300 report_every=300",
        st(0, 300, 1, 0, 0, 11300), st(300, 90300, 301, 250, 40, 12000),
        st(600, 180300, 601, 400, 45, 13000), st(900, 270000, 900, 500, 50, 14000)])
    g = stress_summary(growing[0])
    assert not g["kb_plateau"] and not g["backlog_plateau"], g
    assert not g["done"] and "drained" not in g, g
    assert stress_failures(g) == ["heap grows", "backlog grows",
                                  "no done line"], g
    assert ("stress=FAIL (heap grows, backlog grows, no done line)"
            in render_stress("x", 1, g))
    assert stress_failures(s) == [] and "stress=ok" in render_stress("x", 1, s)
    # the heap alone growing (backlog flat) fails the log too
    heap_only = [
        "[gcstress] armed per_update=300 run_for=900 drain=300 report_every=300",
        st(0, 300, 1, 0, 0, 12000), st(300, 90300, 301, 280, 40, 12100),
        st(600, 180300, 601, 560, 45, 20000), st(900, 270000, 900, 880, 50, 30000)]
    h = stress_summary(stress_runs(heap_only)[0])
    assert not h["kb_plateau"] and h["backlog_plateau"], h
    assert stress_failures(h) == ["heap grows", "no done line"], h
    # kb=-1 (no collectgarbage in the mod) is no data, not a
    # plateau, and fails the log
    nodata = [
        "[gcstress] armed per_update=300 run_for=900 drain=300 report_every=300",
        st(0, 300, 1, 0, 0, -1), st(300, 90300, 301, 280, 40, -1),
        st(600, 180300, 601, 560, 45, -1), st(900, 270000, 900, 880, 50, -1)]
    n = stress_summary(stress_runs(nodata)[0])
    assert n["kb_nodata"] and not n["kb_plateau"] and n["backlog_plateau"], n
    assert stress_failures(n) == ["no heap data", "no done line"], n
    assert "kb_plateau=NO (kb=-1: no heap data)" in render_stress("x", 1, n)
    assert not s["kb_nodata"] and not g["kb_nodata"]
    # too few samples, no done line, undrained backlog
    armed = ("[gcstress] armed per_update=300 run_for=900 drain=300 "
             "report_every=300")
    vacuous = {
        "zero": [armed, st(1200, 270000, 900, 900, 50, 11900, done="done ")],
        "one": [armed, st(0, 300, 1, 0, 0, 11300),
                st(1200, 270000, 900, 900, 50, 11900, done="done ")],
        "notdone": [armed, st(0, 300, 1, 0, 0, 11300),
                    st(300, 90300, 301, 280, 40, 11400)],
        "undrained": [armed, st(0, 300, 1, 0, 0, 11300),
                      st(300, 90300, 301, 280, 40, 11400),
                      st(600, 90300, 301, 290, 40, 11400, done="done ")],
    }
    expect = {"zero": ["0 stress sample(s), need 2"],
              "one": ["1 stress sample(s), need 2"],
              "notdone": ["no done line"],
              "undrained": ["backlog not drained"]}
    for name, lines_ in vacuous.items():
        v = stress_summary(stress_runs(lines_)[0])
        assert stress_failures(v) == expect[name], (name, v)
        assert "stress=FAIL" in render_stress("x", 1, v), name
    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / "nodata.log"
        log.write_text("\n".join(nodata) + "\n", encoding="utf-8")
        good = Path(tmp) / "good.log"
        good.write_text("\n".join(lines) + "\n", encoding="utf-8")
        grows = Path(tmp) / "grows.log"
        grows.write_text("\n".join(heap_only) + "\n", encoding="utf-8")
        flat = Path(tmp) / "flat.log"
        flat.write_text("\n".join([
            "[gcstress] armed per_update=300 run_for=900 drain=300 "
            "report_every=300", st(0, 300, 1, 0, 0, 11300),
            st(300, 90300, 301, 280, 40, 13000),
            st(600, 180300, 601, 560, 45, 13200),
            st(900, 270000, 900, 880, 50, 13300),
            st(1200, 270000, 900, 900, 50, 11900, done="done ")]) + "\n",
            encoding="utf-8")
        with contextlib.redirect_stdout(io.StringIO()):
            assert main([str(log)]) == 1 and main([str(good)]) == 0
            assert main([str(grows)]) == 1 and main([str(flat)]) == 0
            for name, lines_ in vacuous.items():
                bad = Path(tmp) / f"{name}.log"
                bad.write_text("\n".join(lines_) + "\n", encoding="utf-8")
                assert main([str(bad)]) == 1, name
    print("lua_gc_window_report self-test: PASS")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("logs", nargs="*", type=Path)
    parser.add_argument("--ingame-req-kb", type=int, default=200)
    parser.add_argument("--ingame-from-win", type=int)
    parser.add_argument("--pace-step", type=int,
                        help="ISAAC_VITA_LUA_GC_FINALIZER_STEP of the build")
    parser.add_argument("--pace-max", type=int, default=8,
                        help="ISAAC_VITA_LUA_GC_FINALIZER_MAX of the build")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.logs:
        parser.error("give at least one log, or --self-test")
    bound = (pace_bound(args.pace_step, args.pace_max)
             if args.pace_step is not None else None)
    status = 0
    for path in args.logs:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            print(f"{path}: {exc}", file=sys.stderr)
            status = 1
            continue
        lines = text.splitlines()
        found = runs(lines)
        costs = cost_runs(lines)
        stress = stress_runs(lines)
        if not found and not costs and not stress:
            print(f"{path}: no gc window, cost or gcstress records (is "
                  "ISAAC_VITA_LUA_GC_PROFILE / _COST_PROFILE ON, the stress "
                  "mod enabled?)")
            status = 1
        for index, rows in enumerate(found, 1):
            s = summary(rows, args.ingame_req_kb, args.ingame_from_win, bound)
            print(render(str(path), index, s))
            if s.get("windows_over_bound"):
                status = 1
        for index, rows in enumerate(costs, 1):
            print(render_cost(str(path), index, cost_summary(rows)))
        for index, run in enumerate(stress, 1):
            s = stress_summary(run)
            print(render_stress(str(path), index, s))
            if stress_failures(s):
                status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
