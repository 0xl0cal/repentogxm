#!/usr/bin/env python3
"""Seeded A/B timing gate over repeated bench runs.

  python ab_gate.py --run base=RUN --run test=RUN --run base=RUN ... [--phase P2 --phase P3]
                    [--min-runs 3] [--expect P2=16:27] [--hot 156d500] [--json OUT]
                    [--eid-heap-pin [SIDE=]KB:BYTES]

A RUN is a bench run directory as bench_run.py leaves it (native.log, game.log,
manifest.json).  The --run arguments are given IN THE ORDER THE RUNS WERE MADE on the device.
`base` is the control build (for S2: A-nomemo = recipe A with the memo OFF), `test` the build with
the one lever ON (A).  Exit 0: valid and no REGRESSION; 1: a gated phase regressed; 2: the inputs
cannot decide (invalid or too few runs).

Why not one run per build.  Two seeded runs of the SAME build (int-final-a2-20261025:
bench-A2S-20261025 vs bench-A2R0-20261025, same floor seed) differ by 2.51 ms in P2 R per render
(18.07 vs 20.58), as much as the S2 band (1.5-2.6 ms).  The fight is not frame-identical, and R per
render follows the draws per render (P2 medians 90.7 vs 106.7).  Per draw the same two runs differ
by 0.3 us (197.5 vs 197.8 us of render-phase samples per draw).  So the gate metric here is rd, the
game-thread render-phase sampler time per draw, and the decision needs a measured within-build
spread, not a band.

Metric per ph120 window (whole windows inside the phase, bench_run.phase_windows):
  rd  = ph120.hs samples(rnd) * period_us / ph120.c gl(d)          us per draw   (gated)
  R   = ph120.hs samples(rnd) * period_us / ph120.s cad(r) / 1000   ms per render (report only;
        the same R as bench_run's table)
  dpr = ph120.c gl(d) / ph120.s cad(r)                              draws per render
  hot = the --hot key's ph120.hotr samples * period_us / gl(d)      us per draw   (G12)
  sw  = ph120.rt s(n,w,us) field w / f(n)                           blocked S waits per present (G9)
  fr50 = ph120.rt fr(50) / 1000                                     ms, render thread  (G10)
  hr  = ph120.mem hr / 2^20 (why=win records)                       MiB heap headroom   (G15)
  vglf = ph120.mem vgl(f,t) field f / 2^20                          MiB vitaGL RAM free (G15)
A run's value is the median over its phase windows.  For hr and vglf higher is better, so their
SAVING / REGRESSION words are about the test build having more / less free memory; d is still
median(base) - median(test).  A run line also prints badalloc=N when ph120.mem why=badalloc
records exist (G15 requires none).

Decision per phase, over runs (each side >= --min-runs, default 3):
  d     = median(base runs) - median(test runs)     positive: the test build is faster
  noise = max(range(base runs), range(test runs))   the within-build spread of this session
  SAVING if d > noise; REGRESSION if -d > noise; NO DETECTABLE CHANGE otherwise.
The inputs are valid only if every run is BENCH OK and seeded with the same floor seed and room
sequence, each side is one build and the two builds differ, and the given run order alternates
sides (interleaved: BTBT... or TBTB...).  A manifest 'created' time is the PREPARE time, not the
run time (bench-A2R0-20261025 was prepared at 23:02 and launched at 05:01), so it is only checked
for consistency with the given order and printed; a contradiction or a span over
--max-session-hours is a warning, not proof.

Bench validity.  Every run is re-diagnosed from its logs
(bench_run.diagnose_dir, never a stored verdict), and the set is INVALID when a run has no EID
banner, has Lua errors in game.log, the EID versions differ, the EID heap ('Lua mem usage' after
EID's main.lua) differs within a side, or the ISAAC_VITA_LUA_LOADFILE_RETRY load counts differ
among the runs that have the knob ON.  No flag turns these off, not even --allow-same-build.  An
EID heap difference between the sides is a warning (a lane may change Lua memory; it then needs
--eid-heap-pin test=KB:BYTES, or bench_run's EID_HEAP_MISMATCH fails its runs).  Each run line
prints eid=<version> heap=<KB>KB+<bytes>B.
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
from collections import Counter
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bench_run as B  # noqa: E402

PHASES = ("P2", "P3")
# rd is gated; the others are printed with the same rule (for sw and fr50 lower is better too, so
# REGRESSION there is G9/G10's 'rises above the control by more than the within-build range').
METRICS = ("rd", "R", "dpr", "hot", "sw", "fr50", "hr", "vglf")
# For these two (MiB free) higher is better: gate() swaps the sides, so SAVING means the test build
# has more free memory and REGRESSION less, beyond the within-build range (G15).
HIGHER_IS_BETTER = frozenset(("hr", "vglf"))
HOT_KEY = "156d500"   # native samples under the KAGE flush 0x56d500 (G12)
VALID, TOO_FEW, INVALID = "VALID", "TOO FEW RUNS", "INVALID"
SAVING, REGRESSION, NO_CHANGE = "SAVING", "REGRESSION", "NO DETECTABLE CHANGE"


def hot_samples(native_text: str, build: str | None, rec: str = "ph120.hotr") -> dict:
    """{win: Counter(key -> samples)} over EVERY line of `rec` in a window (bench_run.parse_native
    keeps one line per record kind and window; a hotr window spans several lines)."""
    bid32 = build[:32] if build else None
    out: dict = {}
    for line in native_text.splitlines():
        m = B.RE_REC.search(line)
        if not m or m.group(1) != rec:
            continue
        if bid32 is not None and m.group(2) != bid32:
            continue
        f = B.kv(m.group(3))
        if "win" not in f or f.get("phase", "rnd") != "rnd":
            continue
        c = out.setdefault(int(f["win"]), Counter())
        for item in f.get("top", "").split(","):
            if ":" in item:
                k, v = item.split(":", 1)
                c[k] += int(v)
    return out


def window_values(w: dict, x: int, hot: dict, hot_key: str) -> dict | None:
    hs, s, c = w[x].get("ph120.hs"), w[x].get("ph120.s"), w[x].get("ph120.c")
    if not hs or not s or not c:
        return None
    smp = dict(zip(("svc", "upd", "rnd", "swp", "lim", "oth", "pace"), B.ints(hs.get("samples"))))
    period = int(hs.get("period_us", "1000"))
    cad, gl = B.ints(s.get("cad")), B.ints(c.get("gl"))
    renders = cad[5] if len(cad) > 5 else 0
    draws = gl[0] if gl else 0
    if not renders or not draws or "rnd" not in smp:
        return None
    rnd_us = smp["rnd"] * period
    v = {"rd": rnd_us / draws, "R": rnd_us / renders / 1e3, "dpr": draws / renders}
    if x in hot:
        v["hot"] = hot[x][hot_key] * period / draws
    rt = w[x].get("ph120.rt")
    if rt:
        sn, fn, fr = B.ints(rt.get("s")), B.ints(rt.get("f")), B.ints(rt.get("fr"))
        if len(sn) > 1 and fn and fn[0]:
            v["sw"] = sn[1] / fn[0]     # blocked S waits (s(n,w,us) field w) per present
        if fr:
            v["fr50"] = fr[0] / 1e3
    mem = w[x].get("ph120.mem")
    if mem and mem.get("why") == "win":   # the in-cadence record, not a badalloc/exit report
        vgl = B.ints(mem.get("vgl"))
        if "hr" in mem:
            v["hr"] = int(mem["hr"]) / 1048576.0
        if vgl:
            v["vglf"] = vgl[0] / 1048576.0
    return v


def badalloc_records(native_text: str, build: str | None) -> int:
    """ph120.mem records with why=badalloc (any window) of this build."""
    bid32 = build[:32] if build else None
    n = 0
    for line in native_text.splitlines():
        m = B.RE_REC.search(line)
        if m and m.group(1) == "ph120.mem" and (bid32 is None or m.group(2) == bid32):
            n += B.kv(m.group(3)).get("why") == "badalloc"
    return n


def load_run(path: Path, hot_key: str = HOT_KEY, eid_heap_pin: tuple | None = None) -> dict:
    """One bench run directory -> diagnosis, identity and per-phase medians.  The diagnosis is
    always re-run from the logs (bench_run.diagnose_dir: manifest nonce, kill.json); a stored
    result.json verdict is never read."""
    man_path = path / "manifest.json"
    man = json.loads(man_path.read_text(encoding="utf-8")) if man_path.exists() else {}
    native = (path / "native.log").read_text(encoding="utf-8", errors="replace")
    game = (path / "game.log").read_text(encoding="utf-8", errors="replace")
    res = B.diagnose_dir(path, eid_heap_pin=eid_heap_pin)
    run = {"dir": str(path), "tag": man.get("tag", path.name), "created": man.get("created"),
           "ok": res["ok"], "verdict": res["verdict"], "build": res["build"],
           "seed": (res.get("seed_level") or {}).get("seed"), "rooms": res.get("seed_rooms"),
           # bench validity gate: EID identity, Lua errors, and the
           # ISAAC_VITA_LUA_LOADFILE_RETRY load count (None: knob OFF)
           "eid": res["eid_version"], "eid_heap": res["eid_heap"], "lua_errors": res["lua_errors"],
           "loadfile_loads": (res["loadfile"] or {}).get("loads"),
           "eid_heap_pin": eid_heap_pin, "res": res, "phases": {}}
    nat, g = B.parse_native(native), B.parse_game(game)
    try:
        phases, _ = B.phase_windows(nat, g)
    except ValueError as e:
        run["phase_error"] = str(e)
        return run
    hot = hot_samples(native, nat["build"])
    run["badalloc"] = badalloc_records(native, nat["build"])
    for name, sel, _ in phases:
        vals = [v for v in (window_values(nat["wins"], x, hot, hot_key) for x in sel) if v]
        row = {"name": name, "windows": len(vals), "values": vals}
        for k in METRICS:
            row[k] = B.med([v.get(k) for v in vals])
        run["phases"][name.split()[0]] = row
    return run


def decide(base: list, test: list, min_runs: int = 3) -> dict:
    """The per-phase decision over per-run values (see the module text)."""
    out = {"n_base": len(base), "n_test": len(test)}
    if min_runs < 2:
        raise ValueError("min_runs must be >= 2: a single run has no within-build range")
    if len(base) < min_runs or len(test) < min_runs:
        out["verdict"] = TOO_FEW
        return out
    mb, mt = statistics.median(base), statistics.median(test)
    noise = max(max(base) - min(base), max(test) - min(test))
    d = mb - mt
    out.update(med_base=mb, med_test=mt, d=d, noise=noise,
               verdict=SAVING if d > noise else REGRESSION if -d > noise else NO_CHANGE)
    return out


def _created(run: dict):
    try:
        return datetime.strptime(run["created"], "%Y-%m-%d %H:%M:%S")
    except (KeyError, TypeError, ValueError):
        return None


def sides(order: list) -> tuple[list, list]:
    return [r for s, r in order if s == "base"], [r for s, r in order if s == "test"]


def validate(order: list, max_session_hours: float = 8.0,
             allow_same_build: bool = False) -> tuple[list[str], list[str]]:
    """order = [(side, run)] in run order.  Returns (reasons the set cannot decide, warnings)."""
    why, warn = [], []
    base, test = sides(order)
    for side, r in order:
        if side not in ("base", "test"):
            why.append(f"unknown side {side!r} (base or test)")
        if not r["ok"]:
            why.append(f"{side} {r['tag']}: {r['verdict']}")
        if r.get("phase_error"):
            why.append(f"{side} {r['tag']}: phases: {r['phase_error']}")
        if r["seed"] is None:
            why.append(f"{side} {r['tag']}: no proven floor seed (no OK SEED)")
        # bench validity gate: ab_gate's own rules, not a stored
        # verdict, and no flag (not even --allow-same-build) turns them off
        if r.get("eid") is None:
            why.append(f"{side} {r['tag']}: EID not loaded (no 'External Item Descriptions v... loaded.' "
                       "banner in game.log)")
        if r.get("lua_errors"):
            kinds = sorted({c for c, _ in r["lua_errors"]})
            why.append(f"{side} {r['tag']}: Lua errors in game.log ({', '.join(kinds)}): {r['lua_errors'][0][1][:120]}")
    eids = sorted({r["eid"] for _, r in order if r.get("eid") is not None})
    if len(eids) > 1:
        why.append(f"EID versions differ: {eids}")
    heaps = {}
    for side, rs in (("base", base), ("test", test)):
        hs = {tuple(r["eid_heap"]) for r in rs if r.get("eid_heap") is not None}
        heaps[side] = hs
        if len(hs) > 1:
            why.append(f"{side} runs' EID heap differs within the side ("
                       + ", ".join(f"{r['tag']} {r['eid_heap']}" for r in rs) + "): a partial Lua load")
    if len(heaps["base"]) == 1 and len(heaps["test"]) == 1 and heaps["base"] != heaps["test"]:
        warn.append(f"EID heap differs between sides (base {sorted(heaps['base'])[0]}, test "
                    f"{sorted(heaps['test'])[0]}): fine for a lane that changes Lua memory, otherwise a "
                    "partial load on one side")
    # loads compared across the runs that have the knob ON; a set where only some runs
    # have it ON (an A/B of the knob itself) is a WARN, not a refusal
    loads = [r.get("loadfile_loads") for _, r in order]
    listing = ", ".join(f"{r['tag']} {'OFF' if r.get('loadfile_loads') is None else r['loadfile_loads']}"
                        for _, r in order)
    if len({x for x in loads if x is not None}) > 1:
        why.append(f"ISAAC_VITA_LUA_LOADFILE_RETRY loads differ across the set ({listing}): the runs did "
                   "not load the same Lua files")
    elif None in loads and any(x is not None for x in loads):
        warn.append(f"ISAAC_VITA_LUA_LOADFILE_RETRY is ON in only some runs ({listing}): their Lua load "
                    "counts cannot be compared")
    pins = {r.get("eid_heap_pin") for _, r in order if r.get("eid_heap_pin") is not None}
    for p in sorted(pins):
        warn.append(f"EID heap pin overridden to {p[0]}:{p[1]} for "
                    + ", ".join(r["tag"] for _, r in order if r.get("eid_heap_pin") == p))
    seeds = {r["seed"] for _, r in order}
    if len(seeds) > 1:
        why.append(f"floor seeds differ: {sorted(map(str, seeds))}")
    rooms = {json.dumps(r["rooms"]) for _, r in order}
    if len(rooms) > 1:
        why.append(f"room sequences differ across runs ({len(rooms)} distinct)")
    for side, rs in (("base", base), ("test", test)):
        builds = {r["build"] for r in rs}
        if len(builds) > 1:
            why.append(f"{side} runs come from {len(builds)} builds: {sorted(map(str, builds))}")
    if not allow_same_build and {r["build"] for r in base} & {r["build"] for r in test}:
        why.append("base and test are the same build (an A/A calibration needs --allow-same-build)")
    seq = "".join("B" if s == "base" else "T" for s, _ in order)
    if any(seq[i] == seq[i + 1] for i in range(len(seq) - 1)):
        why.append(f"runs are not interleaved (order {seq}; expected BTBT... or TBTB...)")
    times = [_created(r) for _, r in order]
    if any(t is None for t in times):
        warn.append("a run has no manifest 'created' time: the given order is unchecked")
    else:
        if times != sorted(times):
            warn.append("manifest 'created' (prepare) times are not in the given run order: "
                        "check the order against the session notes")
        span = (max(times) - min(times)).total_seconds() / 3600.0
        if span > max_session_hours:
            warn.append(f"prepare times span {span:.1f} h (> {max_session_hours} h): "
                        "check that the runs were one session")
    return why, warn


def gate(order: list, phases=PHASES, min_runs: int = 3, expect: dict | None = None,
         max_session_hours: float = 8.0, allow_same_build: bool = False) -> dict:
    why, warn = validate(order, max_session_hours, allow_same_build)
    base, test = sides(order)
    out = {"inputs": VALID if not why else INVALID, "why": why, "warn": warn,
           "order": "".join("B" if s == "base" else "T" for s, _ in order), "phases": {}}
    for p in phases:
        row = {}
        for metric in METRICS:
            bv = [r["phases"].get(p, {}).get(metric) for r in base]
            tv = [r["phases"].get(p, {}).get(metric) for r in test]
            if not bv or not tv or any(v is None for v in bv + tv):
                row[metric] = {"verdict": "NO DATA", "n_base": len(bv), "n_test": len(tv)}
                continue
            if metric in HIGHER_IS_BETTER:
                x = decide(tv, bv, min_runs)
                for a, b in (("n_base", "n_test"), ("med_base", "med_test")):
                    if a in x:
                        x[a], x[b] = x[b], x[a]
                if "d" in x:
                    x["d"] = -x["d"]   # still median(base) - median(test)
            else:
                x = decide(bv, tv, min_runs)
            row[metric] = dict(x, base=bv, test=tv)
        lo_hi = (expect or {}).get(p)
        rd = row["rd"]
        if lo_hi and "d" in rd:
            lo, hi = lo_hi
            rd["band"] = "inside" if lo <= rd["d"] <= hi else "below" if rd["d"] < lo else "above"
            rd["resolvable"] = rd["noise"] < lo
        out["phases"][p] = row
    gated = [row["rd"].get("verdict") for row in out["phases"].values()]
    if out["inputs"] != VALID or not gated or any(v in (TOO_FEW, "NO DATA") for v in gated):
        out["exit"] = 2
    elif REGRESSION in gated:
        out["exit"] = 1
    else:
        out["exit"] = 0
    return out


METRIC_NAMES = {"rd": "rd us/draw (gated)", "R": "R ms/render (report)",
                "dpr": "draws/render (report)", "hot": f"hotr {HOT_KEY} us/draw (G12)",
                "sw": "S waits/present (G9)", "fr50": "fr50 ms (G10)",
                "hr": "heap headroom MiB (G15)", "vglf": "vitaGL RAM free MiB (G15)"}


def render(g: dict, order: list) -> str:
    head = {0: "AB GATE: PASS (no regression)", 1: "AB GATE: FAIL (regression)",
            2: "AB GATE: CANNOT DECIDE"}[g["exit"]]
    verdicts = "; ".join(f"{p} {row['rd'].get('verdict')}" + (
        f" {row['rd']['d']:+.1f} us/draw (noise {row['rd']['noise']:.1f})" if "d" in row["rd"] else "")
        for p, row in g["phases"].items())
    L = [f"{head}: {verdicts}"]
    L += [f"  INVALID: {w}" for w in g["why"]]
    L += [f"  WARN: {w}" for w in g["warn"]]
    L.append("")
    L.append(f"runs in the given order {g['order']} (prepared, side, tag, build; per phase "
             "rd us/draw | R ms/render | draws/render):")
    for side, r in order:
        cells = []
        for p in g["phases"]:
            ph = r["phases"].get(p, {})
            if ph.get("rd") is not None:
                cells.append(f"{p} {ph['rd']:.1f} | {ph['R']:.2f} | {ph['dpr']:.1f} ({ph['windows']} win)")
        bad = f"  badalloc={r['badalloc']}" if r.get("badalloc") else ""
        heap = r.get("eid_heap")
        eid = f"  eid={r.get('eid') or '-'} heap={f'{heap[0]}KB+{heap[1]}B' if heap else '-'}"
        L.append(f"  {r.get('created')} {side:4} {r['tag']:10} {r['build']}  " + "; ".join(cells) + bad + eid)
    L.append("")
    for p, row in g["phases"].items():
        for metric, x in row.items():
            name = METRIC_NAMES[metric]
            if "d" not in x:
                L.append(f"  {p} {name:28} {x.get('verdict')} (n={x.get('n_base')}/{x.get('n_test')})")
                continue
            extra = ""
            if "band" in x:
                extra = f"; expected band {x['band']}" + (
                    "" if x["resolvable"] else " (noise >= band floor: not resolvable, add runs)")
            L.append(f"  {p} {name:28} {x['verdict']:20} d={x['d']:+.3f} noise={x['noise']:.3f} "
                     f"base med {x['med_base']:.3f} test med {x['med_test']:.3f} "
                     f"n={x['n_base']}/{x['n_test']}{extra}")
    return "\n".join(L)


def parse_expect(items) -> dict:
    out = {}
    for it in items or []:
        p, band = it.split("=", 1)
        lo, hi = band.split(":", 1)
        out[p] = (float(lo), float(hi))
    return out


def parse_pin(item: str) -> tuple:
    """'test=11230:12' -> ('test', (11230, 12)); '11215:166' -> (None, (11215, 166))."""
    side, sep, pin = item.rpartition("=")
    if sep and side not in ("base", "test"):
        raise argparse.ArgumentTypeError(f"--eid-heap-pin side must be base or test, got {side!r}")
    return (side or None), B.parse_heap_pin(pin)


def parse_run(item: str) -> tuple[str, Path]:
    side, path = item.split("=", 1)
    return side, Path(path)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", action="append", required=True, type=parse_run,
                    help="base=DIR or test=DIR, repeated in the order the runs were made")
    ap.add_argument("--phase", action="append", help="phase prefix to gate (default P2 and P3)")
    ap.add_argument("--min-runs", type=int, default=3)
    ap.add_argument("--expect", action="append", help="PHASE=LO:HI expected saving in us per draw")
    ap.add_argument("--hot", default=HOT_KEY, help="ph120.hotr key for the G12 column")
    ap.add_argument("--max-session-hours", type=float, default=8.0)
    ap.add_argument("--allow-same-build", action="store_true", help="A/A calibration")
    ap.add_argument("--json", type=Path)
    ap.add_argument("--eid-heap-pin", action="append", type=parse_pin, metavar="[SIDE=]KB:BYTES",
                    help="expected EID heap for the runs of SIDE (base or test; all runs without SIDE=), "
                         "for a lane that changes Lua memory (bench_run analyze --eid-heap-pin)")
    a = ap.parse_args(argv)
    pins = dict(a.eid_heap_pin or [])
    order = [(side, load_run(path, a.hot, pins.get(side, pins.get(None)))) for side, path in a.run]
    g = gate(order, tuple(a.phase or PHASES), a.min_runs, parse_expect(a.expect),
             a.max_session_hours, a.allow_same_build)
    print(render(g, order))
    if a.json:
        slim = {"gate": g, "runs": [dict({k: v for k, v in r.items() if k != "res"}, side=s)
                                    for s, r in order]}
        a.json.write_text(json.dumps(slim, indent=1, default=str) + "\n", encoding="utf-8")
    return g["exit"]


if __name__ == "__main__":
    sys.exit(main())
