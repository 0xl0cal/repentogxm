#!/usr/bin/env python3
"""Keep/refute rule on presents/s for one lever arm against its base, over interleaved bench runs.

  python pps_abab.py --run base=RUN --run test=RUN --run base=RUN --run test=RUN ...
                     [--start P2=2 --start P3=3 --start P8=1] [--json OUT]
  python pps_abab.py --calibrate RUN RUN RUN RUN ...     (same-build runs: sigma table + A/A rates)

Written for the Lua virtual-machine lever's device A/B, where the device rule must be able to refute a
lever on a pps loss in any phase, not only on an updates/s loss.  Nothing in it is specific to that lever.

A RUN is a bench run directory (native.log, game.log, manifest.json), given IN THE ORDER THE RUNS WERE
MADE.  Validity is ab_gate.validate (every run BENCH OK with EID loaded and no Lua errors, one floor
seed and room sequence, one build per side, the two builds differ, sides interleaved), plus at least
two runs per side.  The per-phase numbers are bench_run.diagnose_dir's own rows (the analyze table):

  pps   all-window presents/s (pps_mean: sum cad p / sum at-delta over the phase's whole windows)
  pps_q presents/s over the quiet windows (no drop(t), no reset), with the quiet-window count
  ups   all-window Game updates/s (ups_mean)
and, for each phase named by --start (default P2=2 P3=3 P8=1: the fight/spawn start windows),
the first K whole windows of the phase:
  st    their presents/s (sum cad p / sum at-delta), and
  sd    how many of them are drop windows (ph120.s drop(t) > 0 or reset > 0: not quiet), summed over
        the --start phases per run.

Per metric and phase: d = mean(test runs) - mean(base runs) and
  thr = K * sigma * sqrt(1/n_base + 1/n_test),  sigma = max(REF_SIGMA, sd of the base runs)
REF_SIGMA is the run-to-run sample sd of one build measured over 13 runs (below); the base runs' own sd
replaces it when this session's base is noisier.  Every line prints d next to the base spread
(max - min), the sigma used and thr.

  LOSS    in a phase P1-P8: d(pps) < -thr, or d(pps_q) < -thr, or d(ups) < -thr
  DROPS   start drop windows per run: d > thr
  GAIN    in P2 or P3: d(pps) > thr or d(st) > thr

  REFUTE  any LOSS or DROPS                                                 exit 1
  KEEP    no LOSS, no DROPS, and a GAIN                                     exit 0
  FLAT    neither: every phase within thr of the base                       exit 0
  CANNOT DECIDE (invalid set, fewer than 2 runs per side, a phase missing)  exit 2

FLAT is what a lever below the bench's resolution produces; the lever is then judged on its own cost
measurement, and a REFUTE is never overridden by one.  The smallest loss the rule can see is
thr itself: print it (--json has it per phase) before reading a FLAT as "no regression".

Why not "below the base by more than the base-vs-base spread" (the obvious simpler rule): over every
2-vs-2 split of 13 same-build runs (no lever at all), that rule refutes 88 % of splits on pps and 86 %
on pps_q (69 % with a 0.5 presents/s floor), and finds a P2/P3 gain in 47 %, because the range of two
runs is a poor estimate of the noise of a difference of two means.  K = 2.5 on REF_SIGMA refutes 4.6 %
of the same 2-vs-2 splits (4290) and 4.7 % of the 3-vs-3 splits (34320), and finds a gain in 1.0 % /
0.8 %; K = 2.0 refutes 28.7 % / 27.0 %, K = 3.0 0.0 % / 0.1 % (--calibrate prints these rates for any
same-build set; these numbers are its output on the 13 G0 runs named at REF_SIGMA below).

A LOSS line ends '(cleared without <run>)' when dropping that one run alone (thr unchanged) clears it:
a rare hitch in one run reads exactly like a loss.  The label never changes the verdict.  On the 12.11
device sets: ADMIT_BEHIND G0g,AB2a,G0h,AB2b is KEEP (P2 st +15.5 > thr 7.4; the lever was kept) and the
same runs with the sides swapped are REFUTE (P4 pps -3.06 < -1.37); IDLE_STEP G0a,GIa,G0b,GIb is REFUTE
(P1 pps -0.131 < -0.130, cleared without G0b/GIb; the lever was refuted: no gain, P1 gapmax 24.8 -> 36.2
ms in both GI runs); IA4 vs G0, 3+3, is
REFUTE on P3 ups -0.310 < -0.308 (cleared without G0n/I4c) and P7 ups -0.447 < -0.127 (cleared without
I4a), two items that were traced to a P3 spawn-start residue and a pre-existing P7 fight-start hitch
before IA4 was kept.
"""
from __future__ import annotations

import argparse
import itertools
import json
import math
import statistics
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ab_gate as G  # noqa: E402
import bench_run as B  # noqa: E402

PHASES = ("P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8")
GAIN_PHASES = ("P2", "P3")
START = {"P2": 2, "P3": 3, "P8": 1}
METRICS = ("pps", "pps_q", "ups")
K = 2.5
# Run-to-run sample sd (n-1) of rel:v0.1.2-alpha-own-g00-cda-20261112 (eboot ba3b3840) over 13 bench
# runs on 12.11 08:45-23:02 (G0a..G0j, G0m, G0n, G0p), as printed
# by `pps_abab.py --calibrate` over those runs.  st = the default
# --start windows; DROPS = start drop windows per run summed over P2+P3+P8.  Re-measure (--calibrate)
# when the base build's noise is known to differ: a quieter base (IA4 P2 runs 56.12/56.22/56.65) makes
# this table conservative, i.e. it hides losses smaller than thr rather than inventing them.
REF_SIGMA = {
    "P1": {"pps": 0.052, "pps_q": 0.052, "ups": 0.016},
    "P2": {"pps": 3.230, "pps_q": 4.026, "ups": 0.098, "st": 2.943},
    "P3": {"pps": 3.685, "pps_q": 4.721, "ups": 0.151, "st": 3.051},
    "P4": {"pps": 0.548, "pps_q": 1.223, "ups": 0.142},
    "P5": {"pps": 0.086, "pps_q": 0.086, "ups": 0.016},
    "P6": {"pps": 0.025, "pps_q": 0.025, "ups": 0.013},
    "P7": {"pps": 0.933, "pps_q": 1.092, "ups": 0.062},
    "P8": {"pps": 1.989, "pps_q": 2.023, "ups": 0.072, "st": 4.169},
}
REF_SIGMA_DROPS = 0.961
KEEP, FLAT, REFUTE, CANNOT = "KEEP", "FLAT", "REFUTE", "CANNOT DECIDE"


def start_values(native: str, game: str, start: dict) -> dict:
    """{phase: (presents/s over its first K whole windows, drop windows among them, K used)}."""
    nat, g = B.parse_native(native), B.parse_game(game)
    phases, at = B.phase_windows(nat, g)
    wins = sorted(at)
    prev = {wins[i]: wins[i - 1] for i in range(1, len(wins))}
    out = {}
    for name, sel, _ in phases:
        p = name.split()[0]
        if p not in start:
            continue
        ws = [x for x in sel if x in prev][:start[p]]
        if not ws:
            continue
        S = {x: nat["wins"][x]["ph120.s"] for x in ws}
        cad = {x: B.ints(S[x].get("cad")) for x in ws}
        mus = sum(at[x] - at[prev[x]] for x in ws)
        pps = sum(cad[x][7] for x in ws) / mus * 1e6 if mus > 0 and all(len(cad[x]) > 7 for x in ws) else None
        drops = sum(1 for x in ws if (B.ints(S[x].get("drop")) or [0])[0] or (B.ints(S[x].get("reset")) or [0])[0])
        out[p] = (pps, drops, len(ws))
    return out


def load_run(path: Path, start: dict) -> dict:
    """ab_gate's run record (identity and validity fields) plus this rule's per-phase values."""
    run = G.load_run(path)
    rows = {r["phase"].split()[0]: r for r in run["res"].get("phases", [])}
    native = (path / "native.log").read_text(encoding="utf-8", errors="replace")
    game = (path / "game.log").read_text(encoding="utf-8", errors="replace")
    try:
        st = start_values(native, game, start)
    except ValueError as e:
        run["phase_error"] = str(e)
        st = {}
    run["pp"] = {}
    for p in PHASES:
        r = rows.get(p) or {}
        run["pp"][p] = {"pps": r.get("pps_mean"), "pps_q": r.get("pps_q"), "ups": r.get("ups_mean"),
                        "quiet": r.get("quiet"), "windows": r.get("windows"),
                        "st": st.get(p, (None,))[0], "sd": st.get(p, (None, None))[1],
                        "k": st.get(p, (None, None, None))[2]}
    return run


def cmp(base: list, test: list, ref: float | None, k: float = K) -> dict | None:
    """d = mean(test) - mean(base); thr = k * max(ref, sd(base)) * sqrt(1/nb + 1/nt); None when a value
    is missing or a side has fewer than 2 runs."""
    if len(base) < 2 or len(test) < 2 or any(v is None for v in base + test):
        return None
    sdb = statistics.stdev(base)
    sigma = sdb if ref is None else max(ref, sdb)
    return {"base": base, "test": test, "d": statistics.mean(test) - statistics.mean(base),
            "spread": max(base) - min(base), "sd_base": sdb, "ref": ref, "sigma": sigma,
            "thr": k * sigma * math.sqrt(1 / len(base) + 1 / len(test))}


def drops_per_run(pp: dict, start: dict):
    sp = [p for p in PHASES if p in start]
    return sum(pp[p]["sd"] for p in sp) if all(pp[p]["sd"] is not None for p in sp) else None


def one_run(x: dict, tags: tuple | None, sign: int = -1) -> str:
    """Information for a LOSS/DROPS line: the runs whose removal alone (thr unchanged) clears it, i.e.
    the shortfall is carried by one run (a rare hitch in one run reads exactly like a loss)."""
    if not tags:
        return ""
    bt, tt = tags
    hit = []
    for side, vals, names in (("base", x["base"], bt), ("test", x["test"], tt)):
        for i, name in enumerate(names):
            rest = vals[:i] + vals[i + 1:]
            b, t = (rest, x["test"]) if side == "base" else (x["base"], rest)
            d = statistics.mean(t) - statistics.mean(b)
            if (d >= -x["thr"]) if sign < 0 else (d <= x["thr"]):
                hit.append(name)
    return f" (cleared without {'/'.join(hit)})" if hit else ""


def decide(base_pp: list, test_pp: list, start: dict, k: float = K, ref: dict | None = None,
           ref_drops: float | None = REF_SIGMA_DROPS, tags: tuple | None = None) -> dict:
    """base_pp/test_pp: per run {phase: {pps, pps_q, ups, st, sd, ...}}.  The rule of the module text.
    tags = (base run names, test run names) only labels the LOSS lines (one_run); it never changes the
    verdict."""
    ref = REF_SIGMA if ref is None else ref
    out = {"phases": {}, "loss": [], "gain": [], "drops": None, "missing": [], "k": k}
    if len(base_pp) < 2 or len(test_pp) < 2:
        out["verdict"] = CANNOT
        out["missing"].append(f"needs >= 2 runs per side (base {len(base_pp)}, test {len(test_pp)})")
        return out
    for p in PHASES:
        row = {}
        for m in METRICS + ("st",):
            row[m] = cmp([r[p][m] for r in base_pp], [r[p][m] for r in test_pp], ref.get(p, {}).get(m), k)
        out["phases"][p] = row
        if any(row[m] is None for m in METRICS):
            out["missing"].append(f"{p}: no all-window pps, pps_q or ups in every run")
            continue
        for m in METRICS:
            x = row[m]
            if x["d"] < -x["thr"]:
                out["loss"].append(f"{p} {m} {x['d']:+.3f} < -thr {x['thr']:.3f}{one_run(x, tags)}")
        if p in GAIN_PHASES:
            for m in ("pps", "st"):
                x = row[m]
                if x is not None and x["d"] > x["thr"]:
                    out["gain"].append(f"{p} {m} {x['d']:+.2f} > thr {x['thr']:.2f}")
    sp = [p for p in PHASES if p in start]
    out["drops"] = cmp([drops_per_run(r, start) for r in base_pp], [drops_per_run(r, start) for r in test_pp],
                       ref_drops, k)
    if out["drops"] is None:
        out["missing"].append(f"start drop windows not readable in every run ({', '.join(sp)})")
    elif out["drops"]["d"] > out["drops"]["thr"]:
        out["loss"].append(f"start drop windows ({'+'.join(sp)}) per run {out['drops']['d']:+.2f} > "
                           f"thr {out['drops']['thr']:.2f}{one_run(out['drops'], tags, +1)}")
    if out["missing"]:
        out["verdict"] = CANNOT
    elif out["loss"]:
        out["verdict"] = REFUTE
    elif out["gain"]:
        out["verdict"] = KEEP
    else:
        out["verdict"] = FLAT
    return out


def fmt(x: dict | None, f: str = "{:.2f}") -> str:
    if x is None:
        return "-"
    return ("/".join(f.format(v) for v in x["base"]) + " -> " + "/".join(f.format(v) for v in x["test"])
            + f"  d {x['d']:+.3f} (spread {x['spread']:.3f}, sigma {x['sigma']:.3f}"
            + ("" if x["ref"] is None or x["sigma"] == x["ref"] else " = base sd") + f", thr {x['thr']:.3f})")


def render(why: list, warn: list, order: list, res: dict, start: dict) -> str:
    head = {KEEP: "PPS ABAB: KEEP", FLAT: "PPS ABAB: FLAT (within thr of the base everywhere)",
            REFUTE: "PPS ABAB: REFUTE", CANNOT: "PPS ABAB: CANNOT DECIDE"}[res["verdict"]]
    reasons = res["loss"] if res["verdict"] == REFUTE else res["gain"] if res["verdict"] == KEEP else \
        res["missing"] if res["verdict"] == CANNOT else []
    L = [head + (": " + "; ".join(reasons) if reasons else "")]
    L += [f"  INVALID: {w}" for w in why]
    L += [f"  WARN: {w}" for w in warn]
    L.append("runs in the given order " + "".join("B" if s == "base" else "T" for s, _ in order) + ":")
    for side, r in order:
        L.append(f"  {side:4} {r['tag']:12} {r['build']}  {r['verdict'][:60]}")
    L.append(f"per phase, base runs -> test runs, d = mean(test) - mean(base), thr = {res['k']} * sigma * "
             "sqrt(1/nb + 1/nt), sigma = max(REF_SIGMA, base sd):")
    for p, row in res["phases"].items():
        q = "/".join(str(r["pp"][p]["quiet"]) for s, r in order if s == "base") + " -> " + \
            "/".join(str(r["pp"][p]["quiet"]) for s, r in order if s == "test")
        L.append(f"  {p} pps   {fmt(row['pps'])}")
        L.append(f"     pps_q {fmt(row['pps_q'])}  (quiet windows {q})")
        L.append(f"     ups   {fmt(row['ups'], '{:.3f}')}")
        if p in start:
            sd = "/".join(str(r["pp"][p]["sd"]) for s, r in order if s == "base") + " -> " + \
                 "/".join(str(r["pp"][p]["sd"]) for s, r in order if s == "test")
            L.append(f"     start {start[p]} windows pps {fmt(row['st'])}  (drop windows {sd})")
    if res.get("drops") is not None:
        L.append(f"  start drop windows per run ({'+'.join(p for p in PHASES if p in start)}): "
                 f"{fmt(res['drops'], '{:.0f}')}")
    return "\n".join(L)


def calibrate(runs: list, start: dict, ks=(2.0, 2.5, 3.0)) -> str:
    """runs: [(tag, pp)] of ONE build.  Sample sd per phase and metric, then for each k the share of
    2-vs-2 and 3-vs-3 splits of these runs (no lever) that decide() calls REFUTE or KEEP, using the
    sd just measured as REF_SIGMA."""
    ref = {}
    for p in PHASES:
        for m in METRICS + ("st",):
            xs = [pp[p][m] for _, pp in runs]
            if all(x is not None for x in xs):
                ref.setdefault(p, {})[m] = statistics.stdev(xs)
    dr = [drops_per_run(pp, start) for _, pp in runs]
    ref_drops = statistics.stdev(dr) if None not in dr else None
    L = [f"{len(runs)} runs of one build: " + " ".join(t for t, _ in runs), "sample sd (n-1):",
         "REF_SIGMA = {"]
    for p in PHASES:
        L.append(f'    "{p}": {{' + ", ".join(f'"{m}": {v:.3f}' for m, v in ref.get(p, {}).items()) + "},")
    L += ["}", f"REF_SIGMA_DROPS = {ref_drops:.3f}" if ref_drops is not None else "REF_SIGMA_DROPS = None"]
    idx = range(len(runs))
    for n in (2, 3):
        if len(runs) < 2 * n:
            continue
        splits = [(b, tuple(i for i in pick if i not in b)) for pick in itertools.combinations(idx, 2 * n)
                  for b in itertools.combinations(pick, n)]
        for k in ks:
            c = {REFUTE: 0, KEEP: 0, FLAT: 0, CANNOT: 0}
            for b, t in splits:
                c[decide([runs[i][1] for i in b], [runs[i][1] for i in t], start, k, ref, ref_drops)["verdict"]] += 1
            L.append(f"{n}v{n} A/A, {len(splits)} splits, K={k}: REFUTE {c[REFUTE] / len(splits):.1%}, "
                     f"KEEP {c[KEEP] / len(splits):.1%}, FLAT {c[FLAT] / len(splits):.1%}, "
                     f"CANNOT {c[CANNOT] / len(splits):.1%}")
    return "\n".join(L)


def parse_start(items) -> dict:
    if not items:
        return dict(START)
    out = {}
    for it in items:
        p, k = it.split("=", 1)
        out[p] = int(k)
        if p not in PHASES or out[p] < 1:
            raise argparse.ArgumentTypeError(f"--start wants Pn=K with K >= 1, got {it!r}")
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", action="append", type=G.parse_run,
                    help="base=DIR or test=DIR, repeated in the order the runs were made")
    ap.add_argument("--calibrate", nargs="+", type=Path, metavar="DIR",
                    help="same-build runs: print their sigma table and the A/A REFUTE/KEEP rates")
    ap.add_argument("--start", action="append", help="Pn=K: the phase's first K whole windows (default "
                    + " ".join(f"{p}={k}" for p, k in START.items()) + ")")
    ap.add_argument("--max-session-hours", type=float, default=8.0)
    ap.add_argument("--json", type=Path)
    a = ap.parse_args(argv)
    start = parse_start(a.start)
    if a.calibrate:
        loaded = [load_run(p, start) for p in a.calibrate]
        bad = [f"{r['tag']}: {r['verdict']}" for r in loaded if not r["ok"] or r.get("phase_error")
               or r.get("eid") is None or r.get("lua_errors")]
        builds = {r["build"] for r in loaded}
        if bad or len(builds) != 1 or len({r["seed"] for r in loaded}) != 1:
            print("CALIBRATION REFUSED: " + "; ".join(bad + ([f"builds {sorted(map(str, builds))}"]
                                                              if len(builds) != 1 else [])
                                                      + (["floor seeds differ"]
                                                         if len({r['seed'] for r in loaded}) != 1 else [])))
            return 2
        print(f"build {builds.pop()}")
        print(calibrate([(r["tag"], r["pp"]) for r in loaded], start))
        return 0
    if not a.run:
        ap.error("--run (or --calibrate) is required")
    order = [(side, load_run(path, start)) for side, path in a.run]
    why, warn = G.validate(order, a.max_session_hours)
    base = [r["pp"] for s, r in order if s == "base"]
    test = [r["pp"] for s, r in order if s == "test"]
    res = decide(base, test, start, tags=(tuple(r["tag"] for s, r in order if s == "base"),
                                          tuple(r["tag"] for s, r in order if s == "test")))
    if why:
        res["verdict"] = CANNOT
        res["missing"] = why + res["missing"]
    print(render(why, warn, order, res, start))
    if a.json:
        a.json.write_text(json.dumps({"result": res, "why": why, "warn": warn,
                                      "runs": [{"side": s, "tag": r["tag"], "build": r["build"], "pp": r["pp"]}
                                               for s, r in order]}, indent=1, default=str) + "\n",
                          encoding="utf-8")
    return {KEEP: 0, FLAT: 0, REFUTE: 1, CANNOT: 2}[res["verdict"]]


if __name__ == "__main__":
    sys.exit(main())
