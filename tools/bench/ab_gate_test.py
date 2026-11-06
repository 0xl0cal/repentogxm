#!/usr/bin/env python3
"""Host tests of ab_gate.py: python ab_gate_test.py [--artifacts DIR]

Legs:
  legacy    the defect being fixed: the earlier single-run G11 rule of the S2 lever (P2 R per render
            falls by 1.5-2.6 ms, A-off run vs A run) PASSES a pair of runs of ONE build, i.e. a
            change of zero by construction (MEASURED pair bench-A2R0 / bench-A2S, 20.58 / 18.07 ms)
  decide    the median/range rule on hand-made values: every verdict, the n >= 3 floor, min_runs < 2
  calibrate the rule's false-SAVING rate and power at n = 3 (seeded Monte Carlo, iid normal)
  pipeline  synthetic seeded runs built from bench_run_test's real record lines (rnd samples and
            draws edited, hotr lines added): hand-computed rd/R/dpr/hot, SAVING/NO CHANGE/
            REGRESSION, and each INVALID rule (order, seed, rooms, same build, mixed builds,
            BENCH FAILED), the exit codes and the command line
  device    the 2026-10-25 seeded logs under --artifacts (or $BENCH_SMOKE_ROOT; skipped without
            them, they are not in the repository): per-run values against analyze.txt, and the
            A/A pair cannot decide (n = 1 per side); the PKT D7 set (2026-10-26) is INVALID
            with D4A4Pc (EID failed to load) and P2 SAVING without it
  validity  (in pipeline) the bench validity rules (ab_gate.py 'Bench validity'): EID not
            loaded, Lua errors, EID versions, EID heap within / between sides, --eid-heap-pin,
            ISAAC_VITA_LUA_LOADFILE_RETRY load counts
"""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import random
import re
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ab_gate as G  # noqa: E402
import bench_run_test as T  # noqa: E402

FAILS: list[str] = []


def check(name: str, cond: bool, detail="") -> None:
    if not cond:
        FAILS.append(f"{name}: {detail}")
        print(f"  FAIL {name}: {detail}")


def near(a, x, eps=1e-6) -> bool:
    return a is not None and abs(a - x) < eps


# ----------------------------------------------------------------------------------- legacy
def legacy_g11(r_aoff: float, r_a: float) -> str:
    """The S2 lever's earlier G11 rule as written: 'A-off vs A, seeded: P2 R falls by 1.5-2.6 ms.
    Floor 0.9; above 3.6 means look for a confound', from one run of each build."""
    d = r_aoff - r_a
    if d > 3.6:
        return "CONFOUND"
    if d >= 1.5 and d <= 2.6:
        return "PASS"
    return "FLOOR" if d >= 0.9 else "FAIL"


def leg_legacy() -> None:
    # bench-A2R0-20261025 and bench-A2S-20261025: ONE build (int-final-a2-20261025), one floor seed,
    # analyze.txt line 11 (P2 R).  The true lever delta is zero.
    verdict = legacy_g11(20.58, 18.07)
    check("legacy G11 passes an A/A pair (the defect)", verdict == "PASS", verdict)
    # The same pair the other way round fails a change that is zero too.
    check("legacy G11 fails the same A/A pair reversed", legacy_g11(18.07, 20.58) == "FAIL", "")
    # The fixed gate cannot decide from one run per side.
    d = G.decide([20.58], [18.07])
    check("fixed gate: one run per side is TOO FEW RUNS", d["verdict"] == G.TOO_FEW, d)
    print("legacy: the single-run G11 passes an A/A pair; the fixed gate refuses n=1")


# ----------------------------------------------------------------------------------- decide
def leg_decide() -> None:
    D = G.decide
    r = D([197.6, 198.4, 197.1], [176.2, 177.5, 175.9])
    check("saving", r["verdict"] == G.SAVING and near(r["d"], 197.6 - 176.2) and near(r["noise"], 1.6), r)
    r = D([197.6, 198.4, 197.1], [197.9, 196.8, 198.6])
    check("no change", r["verdict"] == G.NO_CHANGE, r)
    r = D([190.0, 191.0, 190.5], [196.0, 195.5, 196.4])
    check("regression", r["verdict"] == G.REGRESSION and r["d"] < 0, r)
    # the noise is the WIDER side: a wide test side hides a 5 us difference
    r = D([190.0, 190.2, 190.1], [181.0, 195.0, 185.0])
    check("noise = max range", near(r["noise"], 14.0) and r["verdict"] == G.NO_CHANGE, r)
    r = D([1.0, 2.0], [5.0, 6.0, 7.0])
    check("n=2 is too few", r["verdict"] == G.TOO_FEW, r)
    try:
        D([1.0, 2.0], [3.0, 4.0], min_runs=1)
        check("min_runs=1 refused", False, "no error")
    except ValueError:
        pass
    # d exactly at the noise is not a saving (strict)
    r = D([10.0, 11.0, 12.0], [9.0, 10.0, 11.0])
    check("d == noise is no change", r["verdict"] == G.NO_CHANGE and near(r["d"], 1.0), r)
    print("decide: 7 hand cases")


def leg_calibrate() -> None:
    rng = random.Random(20261026)

    def rate(n: int, shift: float, trials: int = 4000) -> float:
        k = 0
        for _ in range(trials):
            a = [rng.gauss(0.0, 1.0) for _ in range(n)]
            b = [rng.gauss(-shift, 1.0) for _ in range(n)]
            k += G.decide(a, b, n)["verdict"] == G.SAVING
        return k / trials
    fp, p3, p4 = rate(3, 0.0), rate(3, 3.0), rate(3, 4.0)
    check("false SAVING at n=3 below 5 %", fp < 0.05, fp)
    check("power at 4 sigma above 85 %", p4 > 0.85, p4)
    print(f"calibrate: n=3 false SAVING {fp:.3f}, power at 3 sigma {p3:.3f}, at 4 sigma {p4:.3f} "
          "(4000 trials each)")


# ----------------------------------------------------------------------------------- pipeline
DRAWS = 4876   # W41's ph120.c gl(d)
RENDERS = 57   # W41's ph120.s cad(r)
HOT = (300, 11)  # two ph120.hotr lines per window carry key 156d500
VGLF = 15 << 20  # ph120.mem vgl(f,t) field f
MEM = ("[kage-vita] ph120.mem bid={bid} win={win} loops=1 mi(a,u,f,n,top)=0,0,0,0,0 hr={hr} la=0 "
       "led(n,b)=0,0 ov(l,b,nf,pf,rem)=0,0,0,0,0 slab(p,l,b)=0,0,0 lua(l,pk,fb)=0,0,0 "
       "vgl(f,t)={vgl},73400320 slot=0 ok=0 why={why}")


def _last(lines: list, old: str, new: str) -> list:
    i = max(k for k, line in enumerate(lines) if old in line)
    return lines[:i] + [lines[i].replace(old, new)] + lines[i + 1:]


def synth_run(root: Path, tag: str, created: str, rnd: int, build: str = "slot",
              level_seed: int = T.WANT_LEVEL, last_room: int | None = None,
              broken: bool = False, hr: int = 13 << 20, badalloc: bool = False,
              eid_fail: bool = False, heap: tuple | None = None, eid_version: str | None = None) -> Path:
    nonce = f"{abs(hash(tag)) % (1 << 48):012x}"
    nat = []
    for line in T.good_native():
        line = re.sub(r"(samples\(svc,upd,rnd,swp,lim,oth,pace\)=\d+,\d+,)944,", rf"\g<1>{rnd},", line)
        # S calls 12, of which 5 blocked: distinguishes s(w) from s(n) in the sw metric
        line = line.replace(" s(n,w,us)=0,0,0 ", " s(n,w,us)=12,5,900 ")
        if build != "slot":
            line = line.replace("-slot-", f"-{build}-")
        nat.append(line)
        m = re.match(r"\[kage-vita\] ph120\.hs bid=(\S+) win=(\d+) ", line)
        if m:
            bid, k = m.groups()
            nat.append(f"[kage-vita] ph120.hotr bid={bid} win={k} loops=1 phase=rnd samples={rnd} "
                       f"top=156d500:{HOT[0]},0:64,140eef0:61")
            nat.append(f"[kage-vita] ph120.hotr bid={bid} win={k} loops=1 phase=rnd samples={rnd} "
                       f"top=14a90:16,156d500:{HOT[1]}")
            if badalloc and k == "41":   # an out-of-cadence report first, then the window record
                nat.append(MEM.format(bid=bid, win=k, hr=0, vgl=0, why="badalloc"))
            nat.append(MEM.format(bid=bid, win=k, hr=hr, vgl=VGLF, why="win"))
    game = T.good_game(nonce, level_seed)
    if last_room is not None:  # the last room load differs in both logs (still aligned)
        nat = _last(nat, "pair=1,71 ", f"pair=1,{last_room} ")
        game = _last(game, "Room 1.71(", f"Room 1.{last_room}(")
    if broken:
        game = [g for g in game if "MARK BENCH DONE" not in g]
    # bench validity gate: the EID block of the failing run
    # D4A4Pc, another heap, another banner version
    if eid_fail:
        i = game.index(T.EID_LOADED[0])
        assert game[i:i + len(T.EID_LOADED)] == T.EID_LOADED
        game = game[:i] + T.EID_FAIL + game[i + len(T.EID_LOADED):]
    if heap is not None:
        game = [g.replace("Lua mem usage: 11215 KB and 166 bytes", f"Lua mem usage: {heap[0]} KB and {heap[1]} bytes")
                for g in game]
    if eid_version is not None:
        game = [g.replace("External Item Descriptions v5.23_bc0551a loaded.",
                          f"External Item Descriptions v{eid_version} loaded.") for g in game]
    d = root / tag
    d.mkdir(parents=True)
    (d / "native.log").write_text("\n".join(nat) + "\n", encoding="utf-8")
    (d / "game.log").write_text("\n".join(game) + "\n", encoding="utf-8")
    (d / "manifest.json").write_text(json.dumps({"tag": tag, "nonce": nonce, "created": created}),
                                     encoding="utf-8")
    return d


def order_of(root: Path, spec: list) -> list:
    """kw 'pin' is the run's --eid-heap-pin (load_run), the rest goes to synth_run."""
    return [(side, G.load_run(synth_run(root, tag, f"2026-10-26 10:{i:02d}:00", rnd,
                                        **{k: v for k, v in kw.items() if k != "pin"}),
                              eid_heap_pin=kw.get("pin")))
            for i, (side, tag, rnd, kw) in enumerate(spec)]


def leg_pipeline() -> None:
    with tempfile.TemporaryDirectory(prefix="ab-gate-") as tmp:
        root = Path(tmp)
        # hand values (W41 window, rnd edited): rd = rnd * 1000 / 4876 us, R = rnd / 57 ms,
        # dpr = 4876 / 57, hot = (300 + 11) * 1000 / 4876 us
        run = G.load_run(synth_run(root, "hand", "2026-10-26 09:00:00", 944))
        p2 = run["phases"].get("P2", {})
        check("load ok", run["ok"] and run["seed"] == T.WANT_LEVEL, run["verdict"])
        check("P2 windows", p2.get("windows") == 9, p2.get("windows"))
        check("rd", near(p2.get("rd"), 944 * 1000 / DRAWS), p2.get("rd"))
        check("R == bench_run R", near(p2.get("R"), 944 / RENDERS) and near(
            p2.get("R"), next(r["R"] for r in run["res"]["phases"] if r["phase"].startswith("P2"))), p2.get("R"))
        check("dpr", near(p2.get("dpr"), DRAWS / RENDERS), p2.get("dpr"))
        check("hot sums every hotr line", near(p2.get("hot"), sum(HOT) * 1000 / DRAWS), p2.get("hot"))
        # the fixture's ph120.rt, s edited: s(n,w,us)=12,5,900 f(n,o)=120,0 fr(50,95,max)=768,838,908
        check("sw = s(w) / f(n)", near(p2.get("sw"), 5 / 120), p2.get("sw"))
        check("fr50 in ms", near(p2.get("fr50"), 0.768), p2.get("fr50"))
        check("hr in MiB", near(p2.get("hr"), 13.0), p2.get("hr"))
        check("vglf in MiB", near(p2.get("vglf"), 15.0), p2.get("vglf"))
        check("no badalloc", run["badalloc"] == 0, run["badalloc"])
        bad = G.load_run(synth_run(root, "bad", "2026-10-26 09:01:00", 944, badalloc=True))
        check("badalloc counted, window record still used", bad["badalloc"] == 1 and near(
            bad["phases"]["P2"]["hr"], 13.0), (bad["badalloc"], bad["phases"]["P2"].get("hr")))

        # base = A-nomemo (rd ~ 194 us), test = A (rd ~ 174 us), interleaved BTBTBT
        good = [("base", "b1", 944, {}), ("test", "t1", 846, {"build": "slox"}),
                ("base", "b2", 950, {}), ("test", "t2", 850, {"build": "slox"}),
                ("base", "b3", 940, {}), ("test", "t3", 848, {"build": "slox"})]
        g = G.gate(order_of(root / "good", good), expect={"P2": (16.0, 28.0)})
        rd = g["phases"]["P2"]["rd"]
        check("good: valid", g["inputs"] == G.VALID and not g["why"], g["why"])
        check("good: SAVING", rd["verdict"] == G.SAVING and g["exit"] == 0, rd)
        check("good: d", near(rd["d"], (944 - 848) * 1000 / DRAWS), rd.get("d"))
        check("good: noise", near(rd["noise"], (950 - 940) * 1000 / DRAWS), rd.get("noise"))
        check("good: band", rd.get("band") == "inside" and rd.get("resolvable"), rd)
        check("good: P3 gated too", g["phases"]["P3"]["rd"]["verdict"] == G.SAVING, g["phases"]["P3"]["rd"])
        same = g["phases"]["P2"]["vglf"]
        check("good: equal memory is NO CHANGE", same["verdict"] == G.NO_CHANGE and near(same["d"], 0.0), same)

        # G15 direction (higher is better): the test build has 1 MiB less heap headroom
        lean = [(s, t, r, dict(kw, hr=(12 << 20) + 4096 * i) if s == "test" else dict(kw, hr=(13 << 20) + 4096 * i))
                for i, (s, t, r, kw) in enumerate(good)]
        g = G.gate(order_of(root / "lean", lean))
        hr = g["phases"]["P2"]["hr"]
        check("less headroom: REGRESSION, d = base - test > 0, rd still decides the exit",
              hr["verdict"] == G.REGRESSION and hr["d"] > 0.9 and hr["med_base"] > hr["med_test"]
              and hr["n_base"] == 3 and g["exit"] == 0, hr)
        more = [(s, t, r, dict(kw, hr=kw.get("hr", 13 << 20) + (2 << 20) if s == "test" else 13 << 20))
                for s, t, r, kw in lean]
        hr = G.gate(order_of(root / "more", more))["phases"]["P2"]["hr"]
        check("more headroom: SAVING", hr["verdict"] == G.SAVING and hr["d"] < 0, hr)
        txt = G.render(g, order_of(root / "good-r", good))
        check("render answer first", txt.startswith("AB GATE: PASS (no regression): P2 SAVING"), txt[:80])

        null = [("base", "b1", 944, {}), ("test", "t1", 946, {"build": "slox"}),
                ("base", "b2", 950, {}), ("test", "t2", 941, {"build": "slox"}),
                ("base", "b3", 940, {}), ("test", "t3", 948, {"build": "slox"})]
        g = G.gate(order_of(root / "null", null))
        check("null: NO CHANGE, exit 0", g["phases"]["P2"]["rd"]["verdict"] == G.NO_CHANGE and g["exit"] == 0, g)

        slow = [(s, t, r + (40 if s == "test" else 0), kw) for s, t, r, kw in null]
        g = G.gate(order_of(root / "slow", slow))
        check("slow: REGRESSION, exit 1", g["phases"]["P2"]["rd"]["verdict"] == G.REGRESSION and g["exit"] == 1, g)

        def invalid(name: str, spec: list, needle: str, **kw) -> None:
            g = G.gate(order_of(root / name, spec), **kw)
            hit = [w for w in g["why"] if needle in w]
            check(f"invalid {name}", g["inputs"] == G.INVALID and g["exit"] == 2 and hit, g["why"])

        invalid("order", [good[0], good[2], good[1], good[3], good[4], good[5]], "not interleaved")
        invalid("seed", good[:5] + [("test", "t3", 848, {"build": "slox", "level_seed": 1258606820})],
                "floor seeds differ")
        invalid("rooms", good[:5] + [("test", "t3", 848, {"build": "slox", "last_room": 99})],
                "room sequences differ")
        invalid("same-build", [(s, t, r, {}) for s, t, r, _ in good], "same build")
        invalid("mixed", good[:5] + [("test", "t3", 848, {"build": "slzz"})], "builds")
        invalid("failed", good[:5] + [("test", "t3", 848, {"build": "slox", "broken": True})], "BENCH FAILED")
        g = G.gate(order_of(root / "aa", [(s, t, r, {}) for s, t, r, _ in null]), allow_same_build=True)
        check("A/A calibration allowed", g["inputs"] == G.VALID and g["exit"] == 0, g["why"])
        g = G.gate(order_of(root / "few", good[:4]))
        check("n=2: cannot decide", g["exit"] == 2 and g["phases"]["P2"]["rd"]["verdict"] == G.TOO_FEW, g)

        # the command line, run order as given
        dirs = [synth_run(root / "cli", t, f"2026-10-26 11:{i:02d}:00", r, **kw)
                for i, (s, t, r, kw) in enumerate(good)]
        argv = sum([["--run", f"{s}={d}"] for (s, *_), d in zip(good, dirs)], [])
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = G.main(argv + ["--json", str(root / "cli.json")])
        check("cli exit 0", rc == 0, out.getvalue()[:300])
        check("cli json", json.loads((root / "cli.json").read_text(encoding="utf-8"))["gate"]["exit"] == 0, "")
        check("render: eid and heap on each run line",
              sum("eid=5.23_bc0551a heap=11215KB+166B" in x for x in out.getvalue().splitlines()) == 6,
              out.getvalue()[:600])
        validity(root, good, invalid)
    print("pipeline: hand values, 3 verdicts, G15 direction, 6 invalid rules, A/A, n=2, command line, "
          "bench validity rules")


def validity(root: Path, good: list, invalid) -> None:
    """The bench validity gate: ab_gate refuses a set with a broken Lua load."""
    slox = {"build": "slox"}
    # one eid_fail run in BTBTBT: INVALID, exit 2, the message names the tag and MOD_LUA_ERROR
    spec = good[:3] + [("test", "t2", 850, dict(slox, eid_fail=True))] + good[4:]
    g = G.gate(order_of(root / "v-eidfail", spec))
    hit = [w for w in g["why"] if "t2" in w and "MOD_LUA_ERROR" in w]
    check("validity: eid_fail run INVALID, exit 2, names tag + MOD_LUA_ERROR",
          g["inputs"] == G.INVALID and g["exit"] == 2 and hit, g["why"])
    check("validity: eid_fail run also 'EID not loaded' and 'Lua errors'",
          any("t2: EID not loaded" in w for w in g["why"])
          and any("t2: Lua errors in game.log (EID_ERROR, LUA_FILE_ERROR, MOD_LUA_ERROR, MOD_MAIN_CHUNK_ERROR)"
                  in w for w in g["why"]), g["why"])
    # --allow-same-build does not lift it
    g = G.gate(order_of(root / "v-eidfail-aa", [(s, t, r, {k: v for k, v in kw.items() if k != "build"})
                                                 for s, t, r, kw in spec]), allow_same_build=True)
    check("validity: --allow-same-build does not lift the EID rule", g["exit"] == 2 and any(
        "t2: EID not loaded" in w for w in g["why"]), g["why"])
    # two banner versions (an unpinned version is only a bench_run WARN, so only ab_gate refuses)
    invalid("v-versions", good[:5] + [("test", "t3", 848, dict(slox, eid_version="5.24_0000000"))],
            "EID versions differ")
    # within-side heap mismatch (bench_run also FAILs that run EID_HEAP_MISMATCH)
    invalid("v-heap-side", good[:5] + [("test", "t3", 848, dict(slox, heap=(11214, 166)))],
            "test runs' EID heap differs within the side")
    # cross-side heap difference with the test side's pin: WARN, exit 0
    pin = (11230, 12)
    cross = [(s, t, r, dict(kw, heap=pin, pin=pin) if s == "test" else kw) for s, t, r, kw in good]
    g = G.gate(order_of(root / "v-cross", cross))
    check("validity: cross-side heap difference is a WARN, exit 0",
          g["inputs"] == G.VALID and g["exit"] == 0
          and any("EID heap differs between sides" in w for w in g["warn"])
          and any("EID heap pin overridden to 11230:12 for t1, t2, t3" in w for w in g["warn"]),
          (g["why"], g["warn"]))
    # ... and without the pin bench_run FAILs the test runs EID_HEAP_MISMATCH
    g = G.gate(order_of(root / "v-cross-nopin", [(s, t, r, {k: v for k, v in kw.items() if k != "pin"})
                                                   for s, t, r, kw in cross]))
    check("validity: unpinned heap change is EID_HEAP_MISMATCH",
          g["exit"] == 2 and any("EID_HEAP_MISMATCH" in w for w in g["why"]), g["why"])
    # the command line: --eid-heap-pin test=KB:BYTES, and a bad side is refused by argparse
    dirs = [synth_run(root / "v-cli", t, f"2026-10-26 12:{i:02d}:00", r,
                      **{k: v for k, v in kw.items() if k != "pin"}) for i, (s, t, r, kw) in enumerate(cross)]
    argv = sum([["--run", f"{s}={d}"] for (s, *_), d in zip(cross, dirs)], [])
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = G.main(argv + ["--eid-heap-pin", "test=11230:12"])
    check("validity: cli --eid-heap-pin test=11230:12 exit 0, run lines show both heaps",
          rc == 0 and "heap=11230KB+12B" in out.getvalue() and "heap=11215KB+166B" in out.getvalue(),
          out.getvalue()[:400])
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = G.main(argv)
    check("validity: cli without the pin exit 2", rc == 2, out.getvalue()[:300])
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        try:
            G.main(argv + ["--eid-heap-pin", "tset=11230:12"])
            bad = False
        except SystemExit as e:
            bad = e.code == 2
    check("validity: --eid-heap-pin bad side refused", bad and "side must be base or test" in err.getvalue(),
          err.getvalue()[-200:])
    # loadfile loads: different counts among knob-ON runs INVALID; ON in only some runs a WARN
    on = [(side, dict(r, loadfile_loads=117)) for side, r in order_of(root / "v-lf", good)]
    check("validity: equal loads valid", not G.validate(on)[0], G.validate(on)[0])
    odd = on[:5] + [(on[5][0], dict(on[5][1], loadfile_loads=116))]
    why, _ = G.validate(odd)
    check("validity: loads differ INVALID", any("LOADFILE_RETRY loads differ" in w for w in why), why)
    mixed = on[:5] + [(on[5][0], dict(on[5][1], loadfile_loads=None))]
    why, warn = G.validate(mixed)
    check("validity: knob ON in only some runs is a WARN",
          not why and any("is ON in only some runs" in w for w in warn), (why, warn))


# ----------------------------------------------------------------------------------- device
def leg_device(root: Path) -> None:
    a2s, a2r0 = root / "bench-A2S-20261025", root / "bench-A2R0-20261025"
    if not (a2s / "native.log").exists() or not (a2r0 / "native.log").exists():
        print(f"device: SKIPPED (no 2026-10-25 seeded logs under {root})")
        return
    s, r = G.load_run(a2s), G.load_run(a2r0)
    for run, want_r, want_rd in ((s, 18.07, 197.5), (r, 20.58, 197.8)):
        p2 = run["phases"]["P2"]
        check(f"{run['tag']} P2 R == analyze.txt", abs(p2["R"] - want_r) < 0.005, p2["R"])
        check(f"{run['tag']} P2 rd", abs(p2["rd"] - want_rd) < 0.05, p2["rd"])
    check("A/A pair: same build", s["build"] == r["build"], (s["build"], r["build"]))
    check("A/A pair: same floor seed and rooms", s["seed"] == r["seed"] and s["rooms"] == r["rooms"], "")
    check("legacy G11 passes the measured A/A pair",
          legacy_g11(r["phases"]["P2"]["R"], s["phases"]["P2"]["R"]) == "PASS", "")
    g = G.gate([("test", s), ("base", r)], allow_same_build=True)
    check("fixed gate: the A/A pair cannot decide", g["exit"] == 2, g["exit"])
    leg_pkt(root)
    dr = r["phases"]["P2"]["R"] - s["phases"]["P2"]["R"]
    drd = r["phases"]["P2"]["rd"] - s["phases"]["P2"]["rd"]
    print(f"device: A/A P2 R per render differs by {dr:+.2f} ms, rd by {drd:+.2f} us/draw "
          f"(draws/render {s['phases']['P2']['dpr']:.1f} vs {r['phases']['P2']['dpr']:.1f})")


def leg_pkt(root: Path) -> None:
    """The bench validity gate on the PKT D7 set (gated with --expect P2=5:10 P3=5:10 --hot 156d500):
    with D4A4Pc (EID failed to load) in it the set is refused; without it the 2+2 set reproduces P2
    SAVING."""
    tags = ["D4A4a", "D4A4Pa", "D4A4b", "D4A4Pb", "D4A4c", "D4A4Pc"]
    dirs = [root / f"bench-{t}-20261026" for t in tags]
    if not all((d / "native.log").exists() and (d / "game.log").exists() for d in dirs):
        print(f"device: PKT D7 SKIPPED (no 2026-10-26 PKT logs under {root})")
        return
    argv = sum([["--run", f"{'test' if t.startswith('D4A4P') else 'base'}={d}"] for t, d in zip(tags, dirs)], [])
    tail = ["--expect", "P2=5:10", "--expect", "P3=5:10", "--hot", "156d500"]
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = G.main(argv + tail)
    txt = out.getvalue()
    check("PKT D7 with Pc: exit 2, names D4A4Pc", rc == 2 and txt.startswith("AB GATE: CANNOT DECIDE")
          and "INVALID: test D4A4Pc: BENCH FAILED (MOD_LUA_ERROR" in txt, txt[:600])
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc2 = G.main(argv[:-2] + tail + ["--min-runs", "2"])
    txt2 = out.getvalue()
    check("PKT D7 without Pc, --min-runs 2: exit 0, P2 SAVING",
          rc2 == 0 and "P2 SAVING" in txt2.splitlines()[0], txt2[:600])
    print(f"device: PKT D7 with D4A4Pc -> exit {rc}: {txt.splitlines()[0][:160]}")
    print("        " + next(x for x in txt.splitlines() if "INVALID: test D4A4Pc" in x)[:160])
    print(f"device: PKT D7 without D4A4Pc (--min-runs 2) -> exit {rc2}: {txt2.splitlines()[0][:160]}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    env = os.environ.get("BENCH_SMOKE_ROOT")
    ap.add_argument("--artifacts", type=Path, default=Path(env) if env else None,
                    help="root of the downloaded device logs (default: $BENCH_SMOKE_ROOT); without it "
                         "the device leg is skipped")
    a = ap.parse_args(argv)
    FAILS.clear()
    leg_legacy()
    leg_decide()
    leg_calibrate()
    leg_pipeline()
    if a.artifacts is None:
        print("device: SKIPPED (no --artifacts / BENCH_SMOKE_ROOT: the device logs are not in the repository)")
    else:
        leg_device(a.artifacts)
    if FAILS:
        print(f"ab_gate_test: FAIL ({len(FAILS)})")
        return 1
    print("ab_gate_test: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
