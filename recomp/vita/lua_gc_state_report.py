#!/usr/bin/env python3
"""Summarise ISAAC_VITA_LUA_GC_STATE_PROFILE / _IDLE_STEP records of a Vita log.

Records (recomp/runtime/host_vita_lua_gc.c), one group per gc window, i.e.
per 120 LuaEngine::Update GC steps:

  [isaac-lua] gc state: win=N step(n,us,max) upd=.. alloc=.. other=..
      atomic(upd,alloc,other,max_us,roots_kb,again_kb)=.. fin_alloc=..
      dropped=..
  [isaac-lua] gc cls: win=N (n,us,max) pause=.. prop=.. atomic=.. sweep=..
      swpend=.. callfin=.. tailfin=..
  [isaac-lua] gc asub: win=N us(roots,again,weak,fnz,tail)=..
  [isaac-lua] gc worst: win=N us=.. origin=.. gcstate=A>B cls_us=..
      asub_us=.. fin=.. debt_kb=A>B est_kb=.. tot_kb=.. roots_kb=..
      again_kb=..
  [isaac-lua] gc idle: win=N calls=.. kind(unk,free,busy,late)=..
      deferred=.. ran(idle,over,late,busy,unk)=.. atomic(...)=.. dropped=..
  [isaac-lua] gc idle us: win=N us(idle,over,late,busy,unk)=.. over_us=..
      slack_us=.. owed_kb_max=.. kb(req,ran)=.. owed_kb(in,out,drop)=..
      max_us=..
  [isaac-lua] gc idle why: win=N
      busy(noop,pause,prop,propb,atomic,sweep,callfin)=..
      carry(propb,sweep,callfin)=.. walk(n,objs,max)=..
      settle(count,step)=.. catomic=..

A run starts where win restarts.  The device's cumulative log holds every
earlier build as well, and window numbers restart per launch:

  --bid BID     keep only the launches whose first `bid=` token is BID.  A
                launch ends where the line prefix time ([seconds thr ..])
                falls by more than 5 s.
  --bench DIR   a bench run directory (native.log + game.log): the windows
                are also summarised per bench phase.  A record belongs to
                the phase whose selected ph120.s windows cover its prefix
                time (tools/bench/bench_run.py phase_windows; the rule of
                the 12.11 gc_phase.py).

What the summary answers:
  * which collector state the long steps are (the worst steps, by class);
  * how much collector time is outside the Update site (origin alloc),
    which the gc window line cannot see;
  * where the atomic runs, and what it spends its time on;
  * with IDLE_STEP: which steps the BUSY tails issued, how many of them the
    rule carried and why not the rest, where the carried ones ran, whether
    any ran atomic() (catomic), and the gate identities.

Usage: python lua_gc_state_report.py [LOG] [--bid BID] [--bench DIR]
           [--top N] [--long-us US]
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

CLASSES = ("pause", "prop", "atomic", "sweep", "swpend", "callfin", "tailfin")
SUBS = ("roots", "again", "weak", "fnz", "tail")
ORIGINS = ("upd", "alloc", "other")
PLACES = ("idle", "over", "late", "busy", "unk")
BUSY = ("noop", "pause", "prop", "propb", "atomic", "sweep", "callfin")
FIELD = re.compile(r"([a-z_]+)(?:\([a-z_,]+\))?=(-?\d+(?:[,>]-?\d+)*)")
RECORD = re.compile(r"\[isaac-lua\] gc (state|cls|asub|worst|idle us|"
                    r"idle why|idle|window): (win=\d+ .*)$")
PREFIX = re.compile(r"^\[(\d+\.\d+) thr ")
BID = re.compile(r"\bbid=(\S+)")
LAUNCH_GAP_S = 5.0


def fields(line: str) -> dict[str, list[int]]:
    return {key: [int(v) for v in re.split(r"[,>]", value)]
            for key, value in FIELD.findall(line)}


def launches(lines: list[str]) -> list[tuple[str | None, list[str]]]:
    """[(bid, lines)] per launch of a cumulative log."""
    out: list[tuple[str | None, list[str]]] = []
    current: list[str] = []
    bid = None
    last = None
    for line in lines:
        stamp = PREFIX.match(line)
        if stamp:
            now = float(stamp.group(1))
            if last is not None and now < last - LAUNCH_GAP_S:
                out.append((bid, current))
                current, bid = [], None
            last = now
        if bid is None:
            found = BID.search(line)
            if found:
                bid = found.group(1)
        current.append(line)
    if current:
        out.append((bid, current))
    return out


def parse(lines: list[str]) -> list[dict]:
    """Runs of windows; a window is {"state":..,"cls":..,"asub":..,
    "worst":..,"idle":..,"idle us":..,"idle why":..} of field dicts, plus
    "t": the prefix time (s) of its first record, when the line has one."""
    runs: list[dict] = []
    current: dict[int, dict] = {}
    last = {}
    for line in lines:
        match = RECORD.search(line)
        if not match or match.group(1) == "window":
            continue
        kind, body = match.groups()
        row = fields(body)
        window = row["win"][0]
        if window <= last.get(kind, 0):
            runs.append(current)
            current, last = {}, {}
        last[kind] = window
        if kind == "worst":
            origin = re.search(r"origin=([a-z]+)", body)
            row["origin"] = origin.group(1) if origin else "?"
        entry = current.setdefault(window, {})
        entry[kind] = row
        stamp = PREFIX.match(line)
        if stamp and "t" not in entry:
            entry["t"] = float(stamp.group(1))
    if current:
        runs.append(current)
    return runs


def bench_phases(directory: Path) -> list[tuple[str, list[tuple[int, int]]]]:
    """[(phase, [(from_us, to_us)])] of a bench run directory."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"
                           / "bench"))
    import bench_run
    native = (directory / "native.log").read_text(encoding="utf-8",
                                                  errors="replace")
    game = (directory / "game.log").read_text(encoding="utf-8",
                                              errors="replace")
    phases, at = bench_run.phase_windows(bench_run.parse_native(native),
                                         bench_run.parse_game(game))
    windows = sorted(at)
    before = {w: (at[windows[i - 1]] if i else 0)
              for i, w in enumerate(windows)}
    return [(name, [(before[w], at[w]) for w in selected])
            for name, selected, unused in phases]


def in_phase(seconds: float, spans: list[tuple[int, int]]) -> bool:
    return any(a < seconds * 1e6 <= b for a, b in spans)


def percent(part: int, whole: int) -> str:
    return f"{100.0 * part / whole:5.1f}%" if whole else "    -"


def report_run(index: int, run: dict[int, dict], top: int,
               long_us: int) -> list[str]:
    out = [f"run {index}: {len(run)} windows"]
    state = [w["state"] for w in run.values() if "state" in w]
    if state:
        total = sum(s[o][1] for s in state for o in ORIGINS)
        out.append(f"  luaC_step time {total / 1000.0:.1f} ms over "
                   f"{len(state)} windows "
                   f"({total / 1000.0 / len(state):.2f} ms per window)")
        for origin in ORIGINS:
            n = sum(s[origin][0] for s in state)
            us = sum(s[origin][1] for s in state)
            worst = max(s[origin][2] for s in state)
            out.append(f"    origin {origin:5s} steps={n:7d} us={us:9d} "
                       f"{percent(us, total)} max_us={worst}")
        atomic = [sum(s["atomic"][i] for s in state) for i in range(3)]
        longest = max(state, key=lambda s: s["atomic"][3])
        out.append(f"    atomic steps upd={atomic[0]} alloc={atomic[1]} "
                   f"other={atomic[2]}; longest {longest['atomic'][3]} us "
                   f"(roots {longest['atomic'][4]} KB, grayagain "
                   f"{longest['atomic'][5]} KB)")
        out.append(f"    finalizers run by allocation-driven steps: "
                   f"{sum(s['fin_alloc'][0] for s in state)}; steps dropped "
                   f"by a longjmp: {sum(s['dropped'][0] for s in state)}")
    cls = [w["cls"] for w in run.values() if "cls" in w]
    if cls:
        total = sum(c[k][1] for c in cls for k in CLASSES)
        for name in CLASSES:
            n = sum(c[name][0] for c in cls)
            us = sum(c[name][1] for c in cls)
            worst = max(c[name][2] for c in cls)
            out.append(f"    class {name:7s} steps={n:7d} us={us:9d} "
                       f"{percent(us, total)} max_us={worst}")
    asub = [w["asub"] for w in run.values() if "asub" in w]
    if asub:
        sums = [sum(a["us"][i] for a in asub) for i in range(5)]
        out.append("    atomic sub-phases us: " + " ".join(
            f"{name}={value}" for name, value in zip(SUBS, sums)))
    worst = sorted(((w["worst"], number) for number, w in run.items()
                    if "worst" in w), key=lambda item: -item[0]["us"][0])
    long_steps = [item for item in worst if item[0]["us"][0] >= long_us]
    if worst:
        by_class = dict.fromkeys(CLASSES, 0)
        by_origin = dict.fromkeys(ORIGINS, 0)
        for row, unused in long_steps:
            dominant = max(range(7), key=lambda i: row["cls_us"][i])
            by_class[CLASSES[dominant]] += 1
            by_origin[row["origin"]] = by_origin.get(row["origin"], 0) + 1
        out.append(f"  windows whose worst step is >= {long_us} us: "
                   f"{len(long_steps)} of {len(worst)}; dominant class: "
                   + " ".join(f"{k}={v}" for k, v in by_class.items() if v)
                   + "; origin: "
                   + " ".join(f"{k}={v}" for k, v in by_origin.items() if v))
        for row, number in worst[:top]:
            split = " ".join(f"{name}={value}" for name, value
                             in zip(CLASSES, row["cls_us"]) if value)
            sub = " ".join(f"{name}={value}" for name, value
                           in zip(SUBS, row["asub_us"]) if value)
            out.append(f"    win {number}: {row['us'][0]} us origin="
                       f"{row['origin']} gcstate {row['gcstate'][0]}>"
                       f"{row['gcstate'][1]} [{split}]"
                       + (f" atomic[{sub}]" if sub else "")
                       + f" fin={row['fin'][0]} debt_kb "
                       f"{row['debt_kb'][0]}>{row['debt_kb'][1]} est "
                       f"{row['est_kb'][0]} KB total {row['tot_kb'][0]} KB")
    idle = [(w["idle"], w.get("idle us"), w.get("idle why"))
            for w in run.values() if "idle" in w]
    if idle:
        calls = sum(i["calls"][0] for i, unused, why in idle)
        kinds = [sum(i["kind"][k] for i, unused, why in idle)
                 for k in range(4)]
        ran = [sum(i["ran"][k] for i, unused, why in idle) for k in range(5)]
        atomic = [sum(i["atomic"][k] for i, unused, why in idle)
                  for k in range(5)]
        out.append(f"  idle step: calls={calls} kind unk={kinds[0]} "
                   f"free={kinds[1]} busy={kinds[2]} late={kinds[3]}; "
                   f"carried={sum(i['deferred'][0] for i, u, y in idle)} "
                   f"dropped={sum(i['dropped'][0] for i, u, y in idle)}")
        out.append("    ran: " + " ".join(
            f"{name}={value}" for name, value in zip(PLACES, ran))
            + "; atomic: " + " ".join(
            f"{name}={value}" for name, value in zip(PLACES, atomic)))
        times = [t for unused, t, why in idle if t]
        if times:
            us = [sum(t["us"][k] for t in times) for k in range(5)]
            out.append("    us: " + " ".join(
                f"{name}={value}" for name, value in zip(PLACES, us))
                + f"; past the slack {sum(t['over_us'][0] for t in times)} "
                f"us; slack offered {sum(t['slack_us'][0] for t in times)} "
                f"us; longest step {max(t['max_us'][0] for t in times)} us; "
                f"kb req/ran {sum(t['kb'][0] for t in times)}/"
                f"{sum(t['kb'][1] for t in times)}")
            owed = [t for t in times if "owed_kb" in t]
            if owed:
                # the gate identity, per window: a step requested by a
                # window's last call may still be owed when it closes
                bad = [t["win"][0] for t in owed
                       if t["kb"][0] + t["owed_kb"][0] !=
                       t["kb"][1] + t["owed_kb"][1] + t["owed_kb"][2]]
                out.append(f"    kb balance (req + owed.in == ran + owed.out "
                           f"+ drop): {len(owed) - len(bad)} of {len(owed)} "
                           f"windows"
                           + (f"; BROKEN in win {bad[:8]}" if bad else "")
                           + f"; windows closing with a step owed: "
                           f"{sum(1 for t in owed if t['owed_kb'][1])}; kb "
                           f"dropped {sum(t['owed_kb'][2] for t in owed)}")
        whys = [why for unused, t, why in idle if why]
        if whys:
            busy = [sum(y["busy"][k] for y in whys) for k in range(7)]
            carry = [sum(y["carry"][k] for y in whys) for k in range(3)]
            walks = sum(y["walk"][0] for y in whys)
            objects = sum(y["walk"][1] for y in whys)
            settle = [sum(y["settle"][k] for y in whys) for k in range(2)]
            out.append("    BUSY tails by the step issued there: " + " ".join(
                f"{name}={value}" for name, value in zip(BUSY, busy))
                + f" ({percent(sum(carry), sum(busy)).strip()} carried)")
            out.append(f"    carried: propb={carry[0]} sweep={carry[1]} "
                       f"callfin={carry[2]}; run in the GCCOUNT import "
                       f"{settle[0]}, at the GCSTEP import {settle[1]}; "
                       f"carried steps that ran atomic(): "
                       f"{sum(y['catomic'][0] for y in whys)}")
            out.append(f"    gray-list walks: {walks}, "
                       f"{objects / walks if walks else 0:.0f} objects each, "
                       f"longest {max(y['walk'][2] for y in whys)}")
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("log", type=Path, nargs="?")
    parser.add_argument("--bid")
    parser.add_argument("--bench", type=Path)
    parser.add_argument("--top", type=int, default=8)
    parser.add_argument("--long-us", type=int, default=5000)
    args = parser.parse_args(argv)
    log = args.log or (args.bench / "native.log" if args.bench else None)
    if log is None:
        parser.error("LOG or --bench DIR")
    lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    if args.bid:
        found = launches(lines)
        kept = [body for bid, body in found if bid == args.bid]
        print(f"--bid {args.bid}: {len(kept)} of {len(found)} launches "
              f"(bids seen: {sorted({str(b) for b, unused in found})})")
        lines = [line for body in kept for line in body]
    runs = parse(lines)
    if not runs:
        print("no [isaac-lua] gc state / gc idle records "
              "(ISAAC_VITA_LUA_GC_STATE_PROFILE / _IDLE_STEP builds only)")
        return 1
    for index, run in enumerate(runs, 1):
        print("\n".join(report_run(index, run, args.top, args.long_us)))
    if args.bench:
        if len(runs) != 1:
            print(f"--bench: {len(runs)} runs in the log, per-phase split "
                  f"needs exactly one launch")
            return 1
        for name, spans in bench_phases(args.bench):
            part = {number: window for number, window in runs[0].items()
                    if "t" in window and in_phase(window["t"], spans)}
            seconds = sum(b - a for a, b in spans) / 1e6
            print(f"== phase {name}: {seconds:.1f} s selected, "
                  f"{len(part)} gc windows")
            if part:
                print("\n".join(report_run(1, part, args.top,
                                           args.long_us)[1:]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
