#!/usr/bin/env python3
"""Host tests of heap_stage_costs.py: python heap_stage_costs_test.py [--artifacts DIR]

Legs:
  format    record lines rendered from the printf formats in kage_vita_phase_profile.c itself
            (ph120.s/hc/hf/hm/hg/hv; the format text is read from the source on every run, so a
            changed record layout fails here), parsed back with hand-computed per-call costs,
            spread, per-update counts and the ph120.hg split
  tripwire  the prefilter tripwire on hand-made windows: PASS (exact), WARN (one count off),
            FAIL (a block-handled free the prefilter skipped; exit code 1), n/a (prefilter OFF)
  compare   B - A verdicts: a 0.3 us drop of free p is DROP, an identical run is NOISE, a 0.1 us
            shift (under FLOOR_US) is NOISE
  device    the 2026-10-25 seeded logs under --artifacts (or $BENCH_SMOKE_ROOT; skipped without
            them, they are not in the repository): windows 23-31 reproduce the earlier per-route
            analysis's figures to the printed digit, and the A/A pair (same eboot) gives no
            DROP/RISE on any gated stage
"""
from __future__ import annotations

import argparse
import contextlib
import io
import math
import os
import re
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import heap_stage_costs as hs  # noqa: E402

SOURCE = HERE.parents[1] / "recomp" / "runtime" / "kage_vita_phase_profile.c"
FAILS: list[str] = []
BID = "rel:v0.1.2-alpha-heap-test"


def check(name: str, cond: bool, detail="") -> None:
    print(("PASS " if cond else "FAIL ") + name + (f" ({detail})" if detail and not cond else ""))
    if not cond:
        FAILS.append(name)


def formats() -> dict:
    """record -> printf format, from every KVPP_PRINTF("[kage-vita] ph120.X ..." in the source."""
    text = SOURCE.read_text(encoding="utf-8")
    macros = {"KVPP_HEAP_SPAN": "%u/%u/%u"}
    out = {}
    for m in re.finditer(r"KVPP_PRINTF\(\s*((?:\"(?:[^\"\\]|\\.)*\"\s*|KVPP_HEAP_SPAN\s*)+),", text):
        parts = re.findall(r"\"((?:[^\"\\]|\\.)*)\"|(KVPP_HEAP_SPAN)", m.group(1))
        fmt = "".join(lit if lit or not mac else macros[mac] for lit, mac in parts)
        fmt = fmt.replace("\\n", "\n")
        rec = re.match(r"\[kage-vita\] (ph120\.\w+) ", fmt)
        if rec and rec.group(1) in ("ph120.s", "ph120.hc", "ph120.hf", "ph120.hm", "ph120.hg", "ph120.hv"):
            out[rec.group(1)] = fmt
    return out


def render(fmt: str, win: int, values: list) -> str:
    convs = re.findall(r"%[0-9.]*[a-z]", fmt)
    if len(convs) != len(values) + 3:
        raise AssertionError(f"{fmt.split()[1]}: {len(convs) - 3} conversions, {len(values)} values")
    return "[1.000000 thr 0x40010003] " + (fmt % tuple([BID, win, 120 * win] + values)).rstrip("\n")


def spans(*triples):
    return [x for t in triples for x in t]


def window(F, win, *, p_us=130, g_upd=60, pf=(6394, 6), hc_blocks=(3, 2, 1), cost=True):
    """One window's six records.  Hand values: hm z = 100/70/0 (z 0.7)."""
    s_vals = [0, win * 2000000] + [0, 0, 0, 0, g_upd, 60, 0, 0] + [0] * 3 + [0] * 4 + [0] * 5 + [0] + [0] * 2 + [0] * 2 + [0, 0]
    o, a, t = hc_blocks
    hc_vals = [6400, 10, o, a, t, 20, 0, 0] + [6500, 0, 0, 0, 0, 0] + [5, 7]
    hf_vals = spans((102, 400, 2), (100, p_us, 0), (100, 90, 0), (100, 150, 0), (100, 80, 0),
                    (100, 160, 0), (100, 180, 0), (100, 85, 0))
    hm_vals = spans((100, 350, 0), (100, 120, 0), (100, 80, 0), (100, 85, 0), (100, 170, 0),
                    (100, 120, 0), (100, 80, 0), (100, 70, 0))
    lines = [render(F["ph120.s"], win, s_vals), render(F["ph120.hc"], win, hc_vals),
             render(F["ph120.hf"], win, hf_vals), render(F["ph120.hm"], win, hm_vals)]
    if cost:
        hg_vals = spans((100, 150, 0), (100, 75, 0), (101, 95, 1)) + [6400, 6464, 3] + [6400, 6500, 12, 4] + [0, 3000, 524288]
        hv_vals = list(pf) + [6390, 6390, 1, 0, 6390, 0, 0]
        lines += [render(F["ph120.hg"], win, hg_vals), render(F["ph120.hv"], win, hv_vals)]
    return lines


def run_main(argv) -> tuple[int, str]:
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = hs.main(argv)
    return rc, buf.getvalue()


def write(tmp: Path, name: str, lines: list) -> Path:
    p = tmp / name
    p.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return p


def leg_format(F: dict, tmp: Path) -> None:
    check("format: six record formats found in kage_vita_phase_profile.c", len(F) == 6, sorted(F))
    native = write(tmp, "fmt.log", window(F, 1) + window(F, 2, p_us=120))
    rc, out = run_main(["--native", str(native), "--windows", "1-2"])
    check("format: exit 0", rc == 0, out)
    s = hs.summarize(hs.window_rows(hs.br.parse_native(native.read_text()), [1, 2]))
    check("format: two windows parsed", s["windows"] == 2, s.get("windows"))
    check("format: z = 140/200 = 0.7", abs(s["z"] - 0.7) < 1e-12, s["z"])
    # free p: windows 1.3 - 0.7 = 0.6 and 1.2 - 0.7 = 0.5; pooled 250/200 - 0.7 = 0.55;
    # se = stdev(0.6, 0.5) / sqrt(2) = 0.05.
    p, se, n = s[("free", "p")]
    check("format: free p pooled 0.55", abs(p - 0.55) < 1e-12, p)
    check("format: free p se 0.05", abs(se - 0.05) < 1e-12, se)
    # free a: 102 spans with 2 over: 800 / 200 - 0.7 = 3.3 (over excluded from n)
    check("format: free a = us / (n - over) - z", abs(s[("free", "a")][0] - 3.3) < 1e-12, s[("free", "a")])
    check("format: malloc g = 1.2 - 0.7", abs(s[("malloc", "g")][0] - 0.5) < 1e-12, s[("malloc", "g")])
    check("format: frees/update = 12800 / 120", abs(s["frees_per_update"] - 12800 / 120) < 1e-12)
    check("format: mallocs/update = 13000 / 120", abs(s["mallocs_per_update"] - 13000 / 120) < 1e-12)
    # hg: k 1.5 - 0.7, e 0.75 - 0.7, r 190/200 - 0.7 (one over each window)
    check("format: hg k/e/r per call", all(abs(a - b) < 1e-12 for a, b in
                                         zip((s["cost"][k] for k in "ker"), (0.8, 0.05, 0.25))), s.get("cost"))
    check("format: probes/lookup 12928/12800, scan/remove 13000/12800, moves 24/12800",
          abs(s["probes_per_lookup"] - 12928 / 12800) < 1e-12 and abs(s["scan_per_remove"] - 13000 / 12800) < 1e-12
          and abs(s["moves_per_remove"] - 24 / 12800) < 1e-12)
    check("format: report prints the hg and hv lines", "hg ledger g split" in out and "hv prefilter skip=12788" in out, out)


def leg_tripwire(F: dict, tmp: Path) -> None:
    cases = (("PASS", (6394, 6), (3, 2, 1), 0), ("WARN", (6394, 5), (3, 2, 1), 0),
             ("FAIL", (6400, 0), (3, 2, 1), 1), ("OFF", (0, 0), (3, 2, 1), 0))
    for state, pf, blocks, want_rc in cases:
        second = window(F, 2, pf=(0, 0)) if state == "OFF" else window(F, 2)
        native = write(tmp, f"tw-{state}.log", window(F, 1, pf=pf, hc_blocks=blocks) + second)
        rc, out = run_main(["--native", str(native), "--windows", "1-2"])
        s = hs.summarize(hs.window_rows(hs.br.parse_native(native.read_text()), [1, 2]))
        check(f"tripwire: {state} state", s.get("pf_state") == state, s.get("pf_state"))
        check(f"tripwire: {state} exit {want_rc}", rc == want_rc, out)
    native = write(tmp, "tw-nocost.log", window(F, 1, cost=False) + window(F, 2, cost=False))
    s = hs.summarize(hs.window_rows(hs.br.parse_native(native.read_text()), [1, 2]))
    check("tripwire: stage-profile-only run has no hg/hv section", "cost" not in s and "pf" not in s)


def leg_compare(F: dict, tmp: Path) -> None:
    a = write(tmp, "cmp-a.log", window(F, 1) + window(F, 2, p_us=120) + window(F, 3, p_us=125))
    b = write(tmp, "cmp-b.log", window(F, 1, p_us=100) + window(F, 2, p_us=90) + window(F, 3, p_us=95))
    rc, out = run_main(["--native", str(a), "--native-b", str(b), "--windows", "1-3"])
    line = next((x for x in out.splitlines() if x.strip().startswith("free p:")), "")
    check("compare: 0.3 us drop of free p is DROP", rc == 0 and line.endswith("DROP")
          and "delta -0.300" in line, line or out)
    rc, out = run_main(["--native", str(a), "--native-b", str(a), "--windows", "1-3"])
    verdicts = [x.split()[-1] for x in out.splitlines() if re.match(r"    (free|malloc) \w: A", x)]
    check("compare: identical runs are NOISE on every gated stage",
          len(verdicts) == len(hs.GATED) and set(verdicts) == {"NOISE"}, verdicts)
    # A drop smaller than the floor is NOISE even with a tiny se: 0.1 us on every window.
    c = write(tmp, "cmp-c.log", window(F, 1, p_us=120) + window(F, 2, p_us=110) + window(F, 3, p_us=115))
    rc, out = run_main(["--native", str(a), "--native-b", str(c), "--windows", "1-3"])
    line = next((x for x in out.splitlines() if x.strip().startswith("free p:")), "")
    check("compare: a 0.1 us shift (below FLOOR_US) is NOISE", line.endswith("NOISE")
          and "delta -0.100" in line, line or out)


def leg_device(artifacts: Path | None) -> None:
    if artifacts is None:
        print("SKIP device (no --artifacts / BENCH_SMOKE_ROOT: the 2026-10-25 seeded logs are not in "
              "the repository)")
        return
    a, r0 = artifacts / "bench-A2S-20261025", artifacts / "bench-A2R0-20261025"
    if not (a / "native.log").is_file() or not (r0 / "native.log").is_file():
        print("SKIP device (2026-10-25 seeded logs not present)")
        return
    # the earlier per-route analysis, windows 23-31 (printed 2026-10-26):
    #   A2S  z=0.722 a=5.00 p=0.59 s=0.17 o=0.67 l=0.11 g=0.89 n=1.14 t=0.14 frees/upd=164
    #   A2R0 z=0.725 a=4.14 p=0.64 s=0.13 o=0.58 l=0.13 g=0.95 n=1.10 t=0.09 frees/upd=168
    want = {a: ("0.722", "5.00 0.59 0.17 0.67 0.11 0.89 1.14 0.14", 164),
            r0: ("0.725", "4.14 0.64 0.13 0.58 0.13 0.95 1.10 0.09", 168)}
    for run, (z, free, fpu) in want.items():
        nat = hs.br.parse_native((run / "native.log").read_text(encoding="utf-8", errors="replace"))
        s = hs.summarize(hs.window_rows(nat, list(range(23, 32))))
        got = " ".join(f"{s[('free', k)][0]:.2f}" for k in hs.FREE)
        check(f"device: {run.name} windows 23-31 reproduce the per-route analysis",
              f"{s['z']:.3f}" == z and got == free and round(s["frees_per_update"]) == fpu,
              f"z={s['z']:.3f} free={got} fpu={s['frees_per_update']:.1f}")
    rc, out = run_main(["--native", str(a / "native.log"), "--game", str(a / "game.log"),
                        "--native-b", str(r0 / "native.log"), "--game-b", str(r0 / "game.log")])
    verdicts = [x.split()[-1] for x in out.splitlines() if re.match(r"    (free|malloc) \w: A", x)]
    check("device: A/A pair (same eboot, P1-P5) gives NOISE on every gated stage",
          rc == 0 and len(verdicts) == 5 * len(hs.GATED) and set(verdicts) == {"NOISE"}, out)


def main() -> int:
    ap = argparse.ArgumentParser()
    env = os.environ.get("BENCH_SMOKE_ROOT")
    ap.add_argument("--artifacts", type=Path, default=Path(env) if env else None,
                    help="root of the downloaded device logs (default: $BENCH_SMOKE_ROOT); without it "
                         "the device leg is skipped")
    a = ap.parse_args()
    F = formats()
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        leg_format(F, tmp)
        leg_tripwire(F, tmp)
        leg_compare(F, tmp)
    leg_device(a.artifacts)
    print(f"heap_stage_costs tests: {'PASS' if not FAILS else 'FAIL ' + ', '.join(FAILS)}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
