#!/usr/bin/env python3
"""Host tests of pps_abab.py (the presents/s keep/refute rule): python pps_abab_test.py [--artifacts DIR]

Legs:
  literal   why the simpler rule is not used: "below the base by more than the base-vs-base
            spread" refutes two hand-made A/A sides whose base runs agree by chance, and the rule
            as built does not
  decide    hand-made per-run values: every verdict (KEEP on pps and on st, FLAT, REFUTE on pps,
            pps_q, ups and start drop windows, CANNOT DECIDE on < 2 runs and on a missing value),
            thr = K * sigma * sqrt(1/nb + 1/nt) computed by hand, sigma = the base sd when it exceeds
            REF_SIGMA, and the 'cleared without' label of a loss carried by one run
  calibrate seeded iid normal runs: calibrate() measures sigma within 25 % and its A/A REFUTE rate at
            K = 2.5 stays under 15 %
  pipeline  synthetic 8-phase bench runs (bench_run_test's dry-section record lines, synth8):
            the command line gives FLAT / exit 0 on two builds with equal logs, CANNOT DECIDE / exit 2
            on one build, on a non-interleaved order and on one run per side, and --json
  device    the 12.11 ADMIT_BEHIND set under --artifacts (or $BENCH_SMOKE_ROOT; skipped without
            either, the device logs are not in the repository) as run directories
            bench-<tag>-20261112/: G0g,AB2a,G0h,AB2b is KEEP (the verdict of that device session)
            and the reversed set AB2a,G0h,AB2b,G0i is REFUTE
"""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import math
import os
import random
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bench_run_test as T  # noqa: E402
import pps_abab as A  # noqa: E402

FAILS: list[str] = []


def check(name: str, cond: bool, detail="") -> None:
    if not cond:
        FAILS.append(f"{name}: {detail}")
        print(f"  FAIL {name}: {detail}")


def near(a, x, eps=1e-9) -> bool:
    return a is not None and abs(a - x) < eps


IDLE = {"P1": 59.5, "P2": 50.0, "P3": 45.0, "P4": 38.0, "P5": 59.5, "P6": 59.6, "P7": 57.0, "P8": 54.0}


def run(over: dict | None = None, sd: tuple = (1, 1, 0)) -> dict:
    """One run's per-phase values: pps = pps_q = IDLE, ups 29.7, st 40 in the --start phases, start
    drop windows sd = (P2, P3, P8); over = {(phase, metric): value}."""
    pp = {}
    for i, p in enumerate(A.PHASES):
        pp[p] = {"pps": IDLE[p], "pps_q": IDLE[p], "ups": 29.7, "quiet": 9, "windows": 9,
                 "st": 40.0 if p in A.START else None, "sd": None, "k": None}
    for p, n in zip(("P2", "P3", "P8"), sd):
        pp[p]["sd"] = n
        pp[p]["k"] = A.START[p]
    for (p, m), v in (over or {}).items():
        pp[p][m] = v
    return pp


def leg_literal() -> None:
    # two base runs 0.01 apart in P1 (near the cap), test runs 0.05 lower: no lever can be resolved
    # from that; the literal spread rule calls it a loss, the built rule does not
    base = [run({("P1", "pps"): 59.56, ("P1", "pps_q"): 59.56}), run({("P1", "pps"): 59.57, ("P1", "pps_q"): 59.57})]
    test = [run({("P1", "pps"): 59.52, ("P1", "pps_q"): 59.52}), run({("P1", "pps"): 59.51, ("P1", "pps_q"): 59.51})]
    b = [r["P1"]["pps"] for r in base]
    t = [r["P1"]["pps"] for r in test]
    literal = sum(t) / 2 - sum(b) / 2 < -(max(b) - min(b))
    res = A.decide(base, test, A.START)
    check("literal spread rule refutes a 0.05 near-cap difference", literal, (b, t))
    check("built rule: FLAT on the same values", res["verdict"] == A.FLAT, res["verdict"] + str(res["loss"]))
    thr = A.K * A.REF_SIGMA["P1"]["pps"] * math.sqrt(1 / 2 + 1 / 2)
    check("built rule: P1 thr = 2.5 * 0.052 * 1", near(res["phases"]["P1"]["pps"]["thr"], thr), res["phases"]["P1"]["pps"])
    print(f"literal: P1 {b} -> {t}: spread rule LOSS={literal}, pps_abab {res['verdict']} (thr {thr:.3f})")


def leg_decide() -> None:
    two = [run(), run()]
    res = A.decide(two, [run(), run()], A.START)
    check("equal sides: FLAT", res["verdict"] == A.FLAT, res)
    check("equal sides: d 0, sigma = REF", near(res["phases"]["P3"]["pps"]["d"], 0.0)
          and near(res["phases"]["P3"]["pps"]["sigma"], A.REF_SIGMA["P3"]["pps"]), res["phases"]["P3"]["pps"])
    # KEEP on P2 pps: thr = 2.5 * 3.230 * 1 = 8.075
    res = A.decide(two, [run({("P2", "pps"): 58.1}), run({("P2", "pps"): 58.1})], A.START)
    check("P2 pps +8.1 > 8.075: KEEP", res["verdict"] == A.KEEP and res["gain"][0].startswith("P2 pps +8.10"),
          res["gain"])
    res = A.decide(two, [run({("P2", "pps"): 58.0}), run({("P2", "pps"): 58.0})], A.START)
    check("P2 pps +8.0 < 8.075: FLAT", res["verdict"] == A.FLAT, res["gain"])
    # KEEP on the start windows alone: P3 st thr = 2.5 * 3.051 = 7.6275
    res = A.decide(two, [run({("P3", "st"): 47.7}), run({("P3", "st"): 47.7})], A.START)
    check("P3 st +7.7: KEEP", res["verdict"] == A.KEEP and res["gain"][0].startswith("P3 st"), res["gain"])
    # a gain in P7 is not a KEEP (gain phases are P2/P3)
    res = A.decide(two, [run({("P7", "pps"): 60.0}), run({("P7", "pps"): 60.0})], A.START)
    check("P7 gain alone: FLAT", res["verdict"] == A.FLAT, res)
    # REFUTE on a P7 pps loss even with a P2 gain (a positive cost measurement must not keep it)
    lossy = {("P2", "pps"): 60.0, ("P7", "pps"): 54.6}       # P7 thr = 2.5 * 0.933 = 2.3325
    res = A.decide(two, [run(lossy), run(lossy)], A.START)
    check("P7 pps -2.4 with a P2 gain: REFUTE", res["verdict"] == A.REFUTE
          and any(x.startswith("P7 pps -2.400 < -thr 2.333") for x in res["loss"]), res["loss"])
    # pps_q alone
    res = A.decide(two, [run({("P4", "pps_q"): 34.9}), run({("P4", "pps_q"): 34.9})], A.START)
    check("P4 pps_q -3.1 > thr 3.0575: REFUTE", res["verdict"] == A.REFUTE
          and res["loss"] == ["P4 pps_q -3.100 < -thr 3.058"], res["loss"])
    # ups: P3 thr = 2.5 * 0.151 = 0.3775
    res = A.decide(two, [run({("P3", "ups"): 29.3}), run({("P3", "ups"): 29.3})], A.START)
    check("P3 ups -0.40: REFUTE", res["verdict"] == A.REFUTE and res["loss"][0].startswith("P3 ups -0.400"),
          res["loss"])
    res = A.decide(two, [run({("P3", "ups"): 29.35}), run({("P3", "ups"): 29.35})], A.START)
    check("P3 ups -0.35: FLAT", res["verdict"] == A.FLAT, res["loss"])
    # start drop windows: thr = 2.5 * 0.961 * 1 = 2.4025 per run
    res = A.decide(two, [run(sd=(2, 2, 1)), run(sd=(2, 2, 1))], A.START)
    check("start drops 2 -> 5 per run: REFUTE", res["verdict"] == A.REFUTE
          and res["loss"][0].startswith("start drop windows (P2+P3+P8) per run +3.00 > thr 2.40"), res["loss"])
    res = A.decide(two, [run(sd=(2, 2, 0)), run(sd=(2, 2, 0))], A.START)
    check("start drops 2 -> 4 per run: FLAT", res["verdict"] == A.FLAT, res["loss"])
    # sigma = base sd when the session's base is noisier than REF; thr scales with sqrt(1/nb + 1/nt)
    base = [run({("P7", "pps"): 55.0}), run({("P7", "pps"): 59.0}), run({("P7", "pps"): 57.0})]
    res = A.decide(base, [run(), run()], A.START)
    x = res["phases"]["P7"]["pps"]
    check("base sd 2.0 > REF 0.933: sigma 2.0", near(x["sigma"], 2.0) and near(x["sd_base"], 2.0), x)
    check("3v2 thr = 2.5 * 2.0 * sqrt(1/3 + 1/2)", near(x["thr"], 2.5 * 2.0 * math.sqrt(1 / 3 + 1 / 2)), x)
    check("3v2 spread printed 4.0", near(x["spread"], 4.0), x)
    # one run carries the loss: label it, verdict unchanged
    test = [run({("P7", "ups"): 28.3}), run(), run()]
    res = A.decide([run(), run(), run()], test, A.START, tags=(("Ba", "Bb", "Bc"), ("Ta", "Tb", "Tc")))
    check("one-run loss: REFUTE, labelled 'cleared without Ta'", res["verdict"] == A.REFUTE
          and res["loss"][0].endswith("(cleared without Ta)"), res["loss"])
    # CANNOT DECIDE
    res = A.decide([run()], [run(), run()], A.START)
    check("one base run: CANNOT DECIDE", res["verdict"] == A.CANNOT, res)
    res = A.decide(two, [run({("P5", "ups"): None}), run()], A.START)
    check("missing P5 ups: CANNOT DECIDE", res["verdict"] == A.CANNOT and "P5" in res["missing"][0], res["missing"])
    res = A.decide(two, [run(sd=(None, 1, 0)), run()], A.START)
    check("missing start drops: CANNOT DECIDE", res["verdict"] == A.CANNOT, res["missing"])
    print(f"decide: {len([1 for f in FAILS])} failures so far")


def leg_calibrate() -> None:
    rng = random.Random(20261113)
    sig = {p: {"pps": 1.0 + i * 0.25, "ups": 0.05 + 0.01 * i} for i, p in enumerate(A.PHASES)}
    runs = []
    for k in range(10):
        over = {}
        for p in A.PHASES:
            v = IDLE[p] + rng.gauss(0, sig[p]["pps"])
            over[(p, "pps")] = v
            over[(p, "pps_q")] = v
            over[(p, "ups")] = 29.7 + rng.gauss(0, sig[p]["ups"])
            if p in A.START:
                over[(p, "st")] = 40.0 + rng.gauss(0, 2.0)
        runs.append((f"R{k}", run(over, sd=(rng.randint(0, 2), rng.randint(0, 2), rng.randint(0, 1)))))
    txt = A.calibrate(runs, A.START, ks=(2.5,))
    table = {}
    for line in txt.splitlines():
        if line.strip().startswith('"P'):
            p = line.strip()[1:3]
            table[p] = json.loads("{" + line.strip().split("{", 1)[1].rstrip(","))
    ok = all(abs(table[p]["pps"] / sig[p]["pps"] - 1) < 0.5 for p in A.PHASES) and len(table) == 8
    check("calibrate: sigma table parsed for 8 phases, pps sd near the drawn sigma", ok, table)
    rate = [ln for ln in txt.splitlines() if ln.startswith("2v2 A/A")]
    pct = float(rate[0].split("REFUTE ")[1].split("%")[0]) if rate else 100.0
    check("calibrate: 2v2 A/A REFUTE rate at K=2.5 under 15 %", pct < 15.0, rate)
    print("calibrate: " + (rate[0] if rate else "no 2v2 line"))


def synth8(root: Path, tag: str, created: str, build: str = "slot") -> Path:
    """An 8-phase bench run (bench_run_test's dry-section records: P1-P8, every window quiet) of
    build rel:...-<build>-...; equal logs on both sides apart from the build name."""
    nonce = f"{abs(hash(tag)) % (1 << 48):012x}"
    nat = T.dry_native()
    if build != "slot":
        nat = nat.replace("-slot-", f"-{build}-")
    d = root / tag
    d.mkdir(parents=True)
    (d / "native.log").write_text(nat, encoding="utf-8")
    (d / "game.log").write_text("\n".join(T.good_game_dry(nonce)) + "\n", encoding="utf-8")
    (d / "manifest.json").write_text(json.dumps({"tag": tag, "nonce": nonce, "created": created,
                                                 "phases": T.PHASES_NEW}), encoding="utf-8")
    return d


def leg_pipeline() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        # the test build's name keeps the 4 letters of 'slot': bids are compared as their first 32 bytes
        spec = [("base", "Ba", "slot"), ("test", "Ta", "knob"), ("base", "Bb", "slot"), ("test", "Tb", "knob")]
        dirs = []
        for i, (side, tag, build) in enumerate(spec):
            dirs.append((side, synth8(root, tag, f"2026-11-13 10:{i:02d}:00", build=build)))
        argv = sum([["--run", f"{s}={d}"] for s, d in dirs], [])
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = A.main(argv + ["--json", str(root / "r.json")])
        txt = out.getvalue()
        check("pipeline: equal logs, two builds -> FLAT exit 0", rc == 0 and txt.startswith("PPS ABAB: FLAT"), txt[:400])
        check("pipeline: every phase line prints d, spread, sigma, thr",
              all(f"  {p} pps " in txt for p in A.PHASES) and txt.count("thr ") >= 24, txt[:1200])
        j = json.loads((root / "r.json").read_text(encoding="utf-8"))
        check("pipeline: --json verdict", j["result"]["verdict"] == A.FLAT, j["result"]["verdict"])
        # one build on both sides
        same = [(s, synth8(root, t + "s", f"2026-11-13 11:{i:02d}:00")) for i, (s, t, _) in enumerate(spec)]
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = A.main(sum([["--run", f"{s}={d}"] for s, d in same], []))
        check("pipeline: same build -> CANNOT DECIDE exit 2", rc == 2 and "same build" in out.getvalue(),
              out.getvalue()[:400])
        # not interleaved
        bad = [dirs[0], dirs[2], dirs[1], dirs[3]]
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = A.main(sum([["--run", f"{s}={d}"] for s, d in bad], []))
        check("pipeline: BBTT -> CANNOT DECIDE exit 2", rc == 2 and "not interleaved" in out.getvalue(),
              out.getvalue()[:400])
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = A.main(sum([["--run", f"{s}={d}"] for s, d in dirs[:2]], []))
        check("pipeline: one run per side -> CANNOT DECIDE exit 2", rc == 2 and "needs >= 2 runs" in out.getvalue(),
              out.getvalue()[:400])
    print("pipeline: done")


def leg_device(root: Path) -> None:
    sets = {"admit": (["base=G0g", "test=AB2a", "base=G0h", "test=AB2b"], 0, "PPS ABAB: KEEP"),
            "admit reversed": (["base=AB2a", "test=G0h", "base=AB2b", "test=G0i"], 1, "PPS ABAB: REFUTE")}
    for name, (spec, want_rc, want) in sets.items():
        dirs = [(s.split("=")[0], root / f"bench-{s.split('=')[1]}-20261112") for s in spec]
        if not all((d / "native.log").exists() and (d / "game.log").exists() for _, d in dirs):
            print(f"device: {name} SKIPPED (no 12.11 logs under {root})")
            continue
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = A.main(sum([["--run", f"{s}={d}"] for s, d in dirs], []))
        first = out.getvalue().splitlines()[0]
        check(f"device {name}: exit {want_rc}, '{want}'", rc == want_rc and first.startswith(want), first)
        print(f"device: {name} -> exit {rc}: {first[:200]}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    env = os.environ.get("BENCH_SMOKE_ROOT")
    ap.add_argument("--artifacts", type=Path, default=Path(env) if env else None,
                    help="root of the downloaded device run directories (default: $BENCH_SMOKE_ROOT); "
                         "without it the device leg is skipped")
    a = ap.parse_args(argv)
    FAILS.clear()
    leg_literal()
    leg_decide()
    leg_calibrate()
    leg_pipeline()
    if a.artifacts is None:
        print("device: SKIPPED (no --artifacts / BENCH_SMOKE_ROOT: the device logs are not in the repository)")
    else:
        leg_device(a.artifacts)
    if FAILS:
        print(f"pps_abab_test: FAIL ({len(FAILS)})")
        return 1
    print("pps_abab_test: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
