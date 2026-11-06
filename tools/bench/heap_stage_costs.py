#!/usr/bin/env python3
"""Guest heap stage costs per call, per bench phase (the heap levers' device gate).

  python heap_stage_costs.py --native A/native.log --game A/game.log
         [--native-b B/native.log --game-b B/game.log] [--phase P3 ...] [--windows 23-31]

Reads ph120.hc/hf/hm (ISAAC_VITA_HEAP_STAGE_PROFILE) and, when present, ph120.hg/hv
(ISAAC_VITA_HEAP_FREE_COST_PROFILE).  Legends: kage_vita_phase_profile.c, the comments above
kage_vita_profile_log_heap_stage and kage_vita_profile_log_heap_free_cost.

Per call = us / (n - over) - z, z = ph120.hm z.us / z.n (the empty clock bracket), pooled over
the phase's whole windows (bench_run.phase_windows) -- the method of the earlier per-route
analysis whose figures heap_stage_costs_test.py's device leg reproduces.  "se" is the standard
error of the per-window per-call values (each with the pooled z).  Per update = per ph120.s cad g.

With a second run (B), each gated stage prints A, B, B - A and the combined se, and a verdict:
DROP when B - A <= -max(3 se, FLOOR_US), RISE when >= +max(3 se, FLOOR_US), else NOISE.  Heap-lever gates:
free p falls with ISAAC_VITA_HEAP_FREE_PREFILTER, free o with
ISAAC_VITA_NATIVE_VORBIS_FREE_WATCH_MASK; every other stage and frees/update stay put.

Exactness tripwire, every window of a run whose ph120.hv pf is not all zero:
  prefilter  pf.s + pf.m == hc f.n (every free import passes the prefilter), and
             pf.m >= hc f.o + f.a + f.t (a free one of the three block routes handled was
             handed to them) -- the second fails only if the prefilter skipped a block free.
             PASS exact, WARN within PF_RACE counts per window, FAIL (exit 1) beyond.
Read-only over the logs; never estimates.
"""
from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench_run as br  # noqa: E402

FREE = ("a", "p", "s", "o", "l", "g", "n", "t")    # ph120.hf us(a,p,s,o,l,g,n,t)
MALLOC = ("a", "p", "l", "r", "n", "g", "t", "z")  # ph120.hm us(a,p,l,r,n,g,t,z)
COST = ("k", "e", "r")                             # ph120.hg us(k,e,r)
HC_F = ("n", "0", "o", "a", "t", "s", "k", "p")    # ph120.hc f(n,0,o,a,t,s,k,p)
HC_M = ("n", "a", "t", "s", "p", "0")              # ph120.hc m(n,a,t,s,p,0)
# Stages compared A/B.  The whole-call spans a are left out: preemption inside a span shorter
# than the 100 us outlier cut inflates them by whole windows (A2S P1 malloc a 10.67 vs A2R0 3.23
# us, same eboot).
GATED = (("free", "p"), ("free", "o"), ("free", "s"), ("free", "l"), ("free", "g"), ("free", "n"),
         ("free", "t"), ("malloc", "p"), ("malloc", "g"), ("malloc", "n"))
# A verdict needs |B - A| >= max(3 se, FLOOR_US).  MEASURED on the A/A pair bench-A2S /
# bench-A2R0-20261025 (same eboot, seeded), P1-P5: |B - A| of free p at most 0.076 us and of
# free o at most 0.104 us; the largest gated delta, malloc n +0.285 us, has se 0.116 (so 3 se
# holds it); every gated stage is NOISE (heap_stage_costs_test.py device leg).  The heap levers
# are expected to move free p and o by about 0.3 us or more (ESTIMATED).
FLOOR_US = 0.15
# The stage counters are plain increments (the stage profile's unsynchronized-counter
# shortcut, host_vita_heap.c): a guest free on another thread can lose one.  A per-window
# deviation up to this many counts is WARN, beyond it FAIL.
PF_RACE = 2


def spans(rec: dict, names) -> dict:
    vals = rec["us"].split(",")
    if len(vals) != len(names):
        raise ValueError(f"expected {len(names)} spans, got {len(vals)}: {rec['us']}")
    out = {}
    for k, v in zip(names, vals):
        n, us, over = (int(x) for x in v.split("/"))
        out[k] = (n, us, over)
    return out


def named(value: str, names) -> dict:
    vals = br.ints(value)
    if len(vals) != len(names):
        raise ValueError(f"expected {len(names)} values, got {len(vals)}: {value}")
    return dict(zip(names, vals))


def per_call(t, z: float) -> float | None:
    n, us, over = t
    return us / (n - over) - z if n > over else None


def window_rows(nat: dict, sel: list) -> list:
    rows = []
    for w in sel:
        rec = nat["wins"].get(w, {})
        if not all(k in rec for k in ("ph120.hf", "ph120.hm", "ph120.hc", "ph120.s")):
            continue
        cad = br.ints(rec["ph120.s"].get("cad"))
        row = {"win": w, "f": spans(rec["ph120.hf"], FREE), "m": spans(rec["ph120.hm"], MALLOC),
               "hcf": named(rec["ph120.hc"]["f"], HC_F), "hcm": named(rec["ph120.hc"]["m"], HC_M),
               "g": cad[4] if len(cad) > 5 else 0}
        if "ph120.hg" in rec:
            hg = rec["ph120.hg"]
            row["cost"] = spans(hg, COST)
            row["lk"] = named(hg["lk"], ("n", "p", "m"))
            row["rm"] = named(hg["rm"], ("n", "s", "v", "m"))
            row["ls"] = int(hg["ls"])
        if "ph120.hv" in rec:
            hv = rec["ph120.hv"]
            row["pf"] = named(hv["pf"], ("s", "m"))
            row["nv"] = named(hv["nv"], ("n", "k", "m", "0", "1", "2", "3"))
        rows.append(row)
    return rows


def pooled(rows: list, key: str, names) -> dict:
    return {k: tuple(sum(r[key][k][i] for r in rows) for i in range(3)) for k in names}


def summarize(rows: list) -> dict:
    """Pooled per-call costs, per-window spread, per-update counts and the tripwires."""
    if not rows:
        return {"windows": 0}
    m = pooled(rows, "m", MALLOC)
    z = m["z"][1] / m["z"][0] if m["z"][0] else 0.0
    out = {"windows": len(rows), "wins": [r["win"] for r in rows], "z": z,
           "updates": sum(r["g"] for r in rows)}
    for kind, key, names in (("free", "f", FREE), ("malloc", "m", MALLOC)):
        tot = pooled(rows, key, names)
        for k in names:
            if kind == "malloc" and k == "z":
                continue
            vals = [per_call(r[key][k], z) for r in rows]
            vals = [v for v in vals if v is not None]
            se = (math.sqrt(sum((v - sum(vals) / len(vals)) ** 2 for v in vals) / (len(vals) - 1))
                  / math.sqrt(len(vals))) if len(vals) > 1 else None
            out[(kind, k)] = (per_call(tot[k], z), se, tot[k][0])
    upd = max(out["updates"], 1)
    out["frees_per_update"] = sum(r["hcf"]["n"] for r in rows) / upd
    out["mallocs_per_update"] = sum(r["hcm"]["n"] for r in rows) / upd
    handled = sum(r["hcf"]["o"] + r["hcf"]["a"] + r["hcf"]["t"] for r in rows)
    out["block_handled"] = handled
    if all("cost" in r for r in rows):
        c = pooled(rows, "cost", COST)
        out["cost"] = {k: per_call(c[k], z) for k in COST}
        out["cost_n"] = {k: c[k][0] for k in COST}
        lk = {k: sum(r["lk"][k] for r in rows) for k in ("n", "p")}
        rm = {k: sum(r["rm"][k] for r in rows) for k in ("n", "s", "v")}
        out["probes_per_lookup"] = lk["p"] / max(lk["n"], 1)
        out["scan_per_remove"] = rm["s"] / max(rm["n"], 1)
        out["moves_per_remove"] = rm["v"] / max(rm["n"], 1)
        out["probe_max"] = max(r["lk"]["m"] for r in rows)
        out["scan_max"] = max(r["rm"]["m"] for r in rows)
        out["lease_scans_per_update"] = sum(r["ls"] for r in rows) / upd
    if all("pf" in r for r in rows):
        s = sum(r["pf"]["s"] for r in rows)
        mm = sum(r["pf"]["m"] for r in rows)
        out["pf"] = (s, mm)
        # Per window: decisions - free imports, and maybe - block-handled frees (both >= 0
        # exactly, and == 0 for the first, when every counter increment lands).
        out["pf_dev"] = [(r["win"], r["pf"]["s"] + r["pf"]["m"] - r["hcf"]["n"],
                          r["pf"]["m"] - (r["hcf"]["o"] + r["hcf"]["a"] + r["hcf"]["t"]))
                         for r in rows]
        if s + mm == 0:
            out["pf_state"] = "OFF"   # cost profile without the prefilter: nothing to check
        elif all(d == 0 and h >= 0 for _, d, h in out["pf_dev"]):
            out["pf_state"] = "PASS"
        elif all(abs(d) <= PF_RACE and h >= -PF_RACE for _, d, h in out["pf_dev"]):
            out["pf_state"] = "WARN"  # within the unsynchronized-counter allowance
        else:
            out["pf_state"] = "FAIL"
        nv = {k: sum(r["nv"][k] for r in rows) for k in ("n", "k", "0", "1", "2", "3")}
        nv["m"] = max(r["nv"]["m"] for r in rows)
        out["nv"] = nv
    return out


def fmt(v, nd=2):
    return "-" if v is None else f"{v:.{nd}f}"


def report(name: str, s: dict) -> list:
    if not s["windows"]:
        return [f"  {name}: no window with ph120.hf/hm/hc/s (needs ISAAC_VITA_HEAP_STAGE_PROFILE)"]
    out = [f"  {name}: windows {s['wins'][0]}..{s['wins'][-1]} ({s['windows']}) updates={s['updates']} "
           f"z={s['z']:.3f} us frees/upd={s['frees_per_update']:.1f} "
           f"mallocs/upd={s['mallocs_per_update']:.1f}"]
    out.append("    free us/call: " + " ".join(
        f"{k}={fmt(s[('free', k)][0])}+-{fmt(s[('free', k)][1])}" for k in FREE))
    out.append("    malloc us/call: " + " ".join(
        f"{k}={fmt(s[('malloc', k)][0])}+-{fmt(s[('malloc', k)][1])}" for k in MALLOC if k != "z"))
    if "cost" in s:
        out.append("    hg ledger g split us/call: " + " ".join(
            f"{k}={fmt(s['cost'][k])} (n={s['cost_n'][k]})" for k in COST)
            + f"; probes/lookup={s['probes_per_lookup']:.3f} (max {s['probe_max']}) "
              f"scan/remove={s['scan_per_remove']:.3f} (max {s['scan_max']}) "
              f"moves/remove={s['moves_per_remove']:.3f} lease scans/upd={s['lease_scans_per_update']:.2f}")
    if "pf" in s:
        sk, mb = s["pf"]
        tot = max(sk + mb, 1)
        nv = s["nv"]
        out.append(f"    hv prefilter skip={sk} maybe={mb} ({100.0 * mb / tot:.3f} % handed to the block "
                   f"routes; block-handled frees {s['block_handled']}); Vorbis observer calls={nv['n']} "
                   f"live bits/call={nv['k'] / max(nv['n'], 1):.3f} max={nv['m']} "
                   f"by live bits 0/1/2/3+={nv['0']}/{nv['1']}/{nv['2']}/{nv['3']}")
        state = s["pf_state"]
        if state == "OFF":
            out.append("    TRIPWIRE prefilter: n/a (pf all zero: the prefilter is not in this build)")
        else:
            worst = [(w, d, h) for w, d, h in s["pf_dev"] if d or h < 0]
            out.append(f"    TRIPWIRE prefilter: {state} (every window: s+m == f.n and m >= f.o+f.a+f.t"
                       + (f"; deviating windows (win, s+m-f.n, m-handled) {worst[:8]}" if worst else "")
                       + ")")
    return out


def compare(name: str, a: dict, b: dict) -> list:
    if not a["windows"] or not b["windows"]:
        return [f"  {name}: compare skipped (a run has no heap stage windows)"]
    out = [f"  {name}: B - A (us/call; se = sqrt(seA^2 + seB^2)); frees/upd A {a['frees_per_update']:.1f} "
           f"B {b['frees_per_update']:.1f}"]
    for kind, k in GATED:
        va, sa, _ = a[(kind, k)]
        vb, sb, _ = b[(kind, k)]
        if va is None or vb is None:
            out.append(f"    {kind} {k}: -")
            continue
        se = math.sqrt((sa or 0.0) ** 2 + (sb or 0.0) ** 2)
        d = vb - va
        bar = max(3 * se, FLOOR_US)
        verdict = "DROP" if d <= -bar else "RISE" if d >= bar else "NOISE"
        out.append(f"    {kind} {k}: A {va:.3f} B {vb:.3f} delta {d:+.3f} se {se:.3f} {verdict}")
    return out


def load(native: Path, game: Path):
    nat = br.parse_native(native.read_text(encoding="utf-8", errors="replace"))
    g = br.parse_game(game.read_text(encoding="utf-8", errors="replace")) if game else None
    return nat, g


def selections(nat: dict, game, phases, windows) -> list:
    if windows:
        sel = []
        for part in windows.split(","):
            lo, _, hi = part.partition("-")
            sel.extend(range(int(lo), int(hi or lo) + 1))
        return [(f"windows {windows}", sel)]
    pw, _ = br.phase_windows(nat, game)
    return [(name, sel) for name, sel, _ in pw if not phases or name.split()[0] in phases]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--native", type=Path, required=True)
    ap.add_argument("--game", type=Path)
    ap.add_argument("--native-b", type=Path)
    ap.add_argument("--game-b", type=Path)
    ap.add_argument("--phase", action="append", default=[])
    ap.add_argument("--windows", help="explicit windows instead of phases, e.g. 23-31 or 3-11,13")
    args = ap.parse_args(argv)
    if not args.windows and not args.game:
        ap.error("--game is needed for the phase windows (or pass --windows)")
    runs = [("A", args.native, args.game)]
    if args.native_b:
        if not args.windows and not args.game_b:
            ap.error("--game-b is needed with --native-b (or pass --windows)")
        runs.append(("B", args.native_b, args.game_b))
    summ = {}
    fail = False
    for tag, native, game in runs:
        nat, g = load(native, game)
        print(f"{tag}: {native} build={nat['build']}")
        for name, sel in selections(nat, g, args.phase, args.windows):
            s = summarize(window_rows(nat, sel))
            summ[(tag, name)] = s
            for line in report(name, s):
                print(line)
            fail |= s.get("pf_state") == "FAIL"
    if len(runs) == 2:
        print("B vs A")
        for (tag, name), a in summ.items():
            if tag == "A" and ("B", name) in summ:
                for line in compare(name, a, summ[("B", name)]):
                    print(line)
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
