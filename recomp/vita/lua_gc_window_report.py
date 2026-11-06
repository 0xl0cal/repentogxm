#!/usr/bin/env python3
"""Summarise ISAAC_VITA_LUA_GC_PROFILE "[isaac-lua] gc window" records.

One record per 120 LuaEngine::Update steps (recomp/runtime/host_vita_lua_gc.c):

  gc window: win=N gcstep(n,sum_us,p95_us,max_us)=... fin(n,max_step,cb)=...
             count=KB KB arena(live,peak,fb)=... kb(req,app)=...

A log may hold several runs (the window number restarts); each run is
reported on its own, in arrival order.  In-game windows are the ones whose
guest pacer asks for at least --ingame-req-kb per step (the pacer sits at its
50 KB floor in the menu and ramps to 1-10 MB per step in a run with EID), so
the split does not depend on the clamp.  Per run it prints what the lua-gc
device gate compares: GC time per step, finalizers per step, the largest
step per window (p50/p90/p99/max and how many windows exceed 4 and 8 ms),
the largest finalizer batch in one step, the Lua heap count and the arena
peak/fallbacks.

usage: python lua_gc_window_report.py LOG [LOG ...] [--ingame-req-kb 200]
       python lua_gc_window_report.py --self-test
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import statistics
import sys

RECORD = re.compile(
    r"gc window: win=(\d+) gcstep\(n,sum_us,p95_us,max_us\)=(\d+),(\d+),(\d+),"
    r"(\d+) fin\(n,max_step,cb\)=(\d+),(\d+),(\d+) count=(\d+) KB "
    r"arena\(live,peak,fb\)=(\d+),(\d+),(\d+) kb\(req,app\)=(\d+),(\d+)")
FIELDS = ("win", "n", "sum_us", "p95_us", "max_us", "fin", "fin_step", "cb",
          "count_kb", "live_kb", "peak_kb", "fb", "req_kb", "app_kb")


def runs(lines) -> list[list[dict[str, int]]]:
    out: list[list[dict[str, int]]] = []
    last = None
    for line in lines:
        match = RECORD.search(line)
        if not match:
            continue
        row = dict(zip(FIELDS, (int(v) for v in match.groups())))
        if last is None or row["win"] <= last:
            out.append([])
        out[-1].append(row)
        last = row["win"]
    return out


def quantile(values: list[int], q: float) -> int:
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(q * len(ordered)))]


def summary(rows: list[dict[str, int]], ingame_req_kb: int) -> dict[str, float]:
    game = [r for r in rows if r["n"] and r["req_kb"] >= ingame_req_kb * r["n"]]
    result: dict[str, float] = {"windows": len(rows), "ingame": len(game)}
    if not game:
        return result
    steps = sum(r["n"] for r in game)
    worst = [r["max_us"] for r in game]
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
        peak_kb_max=max(r["peak_kb"] for r in rows),
        fb_max=max(r["fb"] for r in rows),
    )
    return result


def render(name: str, index: int, s: dict[str, float]) -> str:
    head = f"{name} run {index}: windows={s['windows']} ingame={s['ingame']}"
    if not s["ingame"]:
        return head + " (no in-game windows)"
    return (f"{head} gc_us/step={s['gc_us_per_step']:.0f} "
            f"(median window {s['gc_us_per_step_median']:.0f}) "
            f"fin/step={s['fin_per_step']:.2f} "
            f"applied_kb/step={s['applied_kb_per_step']:.0f} "
            f"max_us p50/p90/p99/max={s['max_us_p50']}/{s['max_us_p90']}/"
            f"{s['max_us_p99']}/{s['max_us_max']} "
            f"windows>4ms={s['windows_over_4ms']} >8ms={s['windows_over_8ms']} "
            f"p95_us_median={s['p95_us_median']:.0f} "
            f"fin_step_max={s['fin_step_max']} count_kb_max={s['count_kb_max']} "
            f"arena_peak_kb={s['peak_kb_max']} fb_max={s['fb_max']}")


def self_test() -> int:
    def rec(win, n, s, p95, mx, fin, fstep, count, peak, fb, req, app):
        return (f"[1.0 thr 0x1] [isaac-lua] gc window: win={win} "
                f"gcstep(n,sum_us,p95_us,max_us)={n},{s},{p95},{mx} "
                f"fin(n,max_step,cb)={fin},{fstep},{fin} count={count} KB "
                f"arena(live,peak,fb)={count},{peak},{fb} kb(req,app)={req},{app} "
                "collect(clamp,full,us)=0,0,0 other=0 unwind=0")
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
    second = summary(found[1], 200)
    assert second["ingame"] == 1 and second["fb_max"] == 2, second
    assert second["applied_kb_per_step"] == 40 and second["windows_over_4ms"] == 0
    menu_only = summary(found[0][:1], 200)
    assert menu_only["ingame"] == 0, menu_only
    print("lua_gc_window_report self-test: PASS")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("logs", nargs="*", type=Path)
    parser.add_argument("--ingame-req-kb", type=int, default=200)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.logs:
        parser.error("give at least one log, or --self-test")
    status = 0
    for path in args.logs:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            print(f"{path}: {exc}", file=sys.stderr)
            status = 1
            continue
        found = runs(text.splitlines())
        if not found:
            print(f"{path}: no gc window records (is ISAAC_VITA_LUA_GC_PROFILE ON?)")
            status = 1
        for index, rows in enumerate(found, 1):
            print(render(str(path), index, summary(rows, args.ingame_req_kb)))
    return status


if __name__ == "__main__":
    sys.exit(main())
