#!/usr/bin/env python3
"""Device gates for ISAAC_VITA_OGG_READAHEAD, computed from native logs.

    ogg_readahead_gates.py ab A.log B.log   knob OFF (A) against knob ON (B)
    ogg_readahead_gates.py suspend S.log    knob ON, one PS-button suspend
    ogg_readahead_gates.py windows LOG      the per-window table and classes

A and B are the first-arm-fault (native) logs of one launch each, same
recipe and route, built with the guest sampler (ph120.hotc) and the phase
profile (ph120.t/.s/.rl).  Exit status: 0 every gate passed, 1 a gate failed,
3 no gate failed but one could not decide (too little data, partial result).

Why per window class.  The fread time under 0x596560 differs by class: in
the P2b session (2026-10-17) gameplay windows spent 0.269 ms/loop there and
menu windows 0.417 with the classes below (P2B_REFERENCE; an independent
split by its own rule measured 0.266 and 0.416), and two menu windows
(73, 74; single-loop svc of 292 and 431 ms) held 1671 of 6557 samples.  A whole-route average therefore moves
with the route mix more than with the knob, and the burst windows put a
floor under it that a working read-ahead cannot remove.  Every gate below
compares A and B class by class, and windows with an svc max of 100 ms or
more are reported on their own, never averaged in.

Window classes (one 120-loop ph120 window each, first rule that matches):
  transition  a room-log marker landed (ph120.rl n > 0) or upd max > 500 ms
  burst       svc max >= 100 ms
  game        ph120.s cad g >= 55 game updates (the device analyzers' rule)
  menu        everything else

Gates (the corrected gate text; test_ogg_readahead_gates.py --rules r0 keeps
the first draft):
  G1  KAGE VITA OGG READAHEAD start line once, worker=on rc=0x00000000, and at
      least one STATS line (B and suspend runs); none of these lines in A
  G2  no-suspend runs: real_fail sync_fail pass foreign reg_fail full and
      fill_err short open_fail all 0 in the last STATS/WORKER pair; a run
      whose DESCRIPTOR RECOVER line shows enodev > 0 had a suspend and is
      judged by `suspend` instead
  G3  hit ratio hits / (hits + miss) over the STATS differences whose interval
      (log arrival order) lies inside game windows only: >= 0.90 pass,
      < 0.70 fail (try _CHUNKS=8 first), between: partial
  G4  fread samples under 596560 in ph120.hotc phase=svc divided by loops,
      for game and for menu windows separately; B at least 70 % below A in
      both classes passes, either class less than 50 % below A fails (the
      saving is not real), anything else is partial; a class with fewer than
      10 windows in A or B cannot decide.  A uses the listed count (lower
      bound), B the largest count the truncated top-12 list allows (upper
      bound).  Transition and burst windows are printed, not gated.
  G5  game windows with an svc max of 16 ms or more, per 100 game windows:
      B at most half of A (A needs 3 such windows to decide); the median svc
      p95 over game windows is printed beside it
  G6  game-window medians of all p50/p95, upd p95, rnd p95 and the median upd
      max of transition windows: WARN when B is more than 5 % (10 % for
      transitions) above A; not a pass/fail gate, run-to-run noise is unknown
  G7  wait_to at most one per started 10 minutes of the run
  G8  suspend run: a DESCRIPTOR RECOVER line with enodev >= 1 and
      recovered >= 1, reopen_fail = redo_fail = unknown_pos = 0; the
      read-ahead's failure counters sync_fail, real_fail, pass and fill_err
      may be nonzero (sync_fail is how a dead descriptor surfaces after a
      hit, real_fail after a miss; fill_err when the worker's descriptor
      predates the suspend), foreign reg_fail full short open_fail stay 0;
      WARN (not fail) when neither sync_fail nor real_fail moved (the worker
      opened its descriptor after the resume: hits continue, oracle scenario
      suspend-late-open) or when no hit followed the recovery
Also printed: CRT FREAD SHORT receipts and FREAD COMPLETE short= in A and B,
the WORKER short= counter, and, when NATIVE_VORBIS_RECEIPT is on, the
NATIVE VORBIS CLOSE pairs with equal f= and frames= in A and B, whose run=
CRCs must be equal (a mismatch fails: the PCM differs).
"""
from __future__ import annotations

import argparse
import math
import re
import statistics
import sys
from dataclasses import dataclass, field

FREAD_RVA = "596560"
TOP_N = 12                      # kage_vita_guest_sampler.c KVGS_TOP
BURST_SVC_US = 100000
TRANSITION_UPD_US = 500000
GAME_UPDATES = 55
MIN_CLASS_WINDOWS = 10
PASS_DOWN = 0.70
REAL_DOWN = 0.50
HIT_PASS = 0.90
HIT_FAIL = 0.70
SPIKE_SVC_US = 16000

# P2b (the 2026-10-17 device log p2b-native.log, reduced to
# host_tests/ogg_readahead_p2b_windows.csv), this
# script's classes, A side: measured 2026-10-25, quoted for orientation only;
# every gate uses the A log it is given.
P2B_REFERENCE = {"game": (1193, 4440), "menu": (2552, 6120),
                 "transition": (1141, 2520), "burst": (1671, 240)}

HEALTH_STATS = ("real_fail", "sync_fail", "pass", "foreign", "reg_fail",
                "full")
HEALTH_WORKER = ("fill_err", "short", "open_fail")
SUSPEND_MAY_MOVE = ("sync_fail", "real_fail", "pass", "fill_err")
SUSPEND_ZERO = ("foreign", "reg_fail", "full", "short", "open_fail")

RECORD = re.compile(r"ph120\.(t|hotc|s|rl) bid=(\S+) win=(\d+) loops=(\d+) (.*)")
TRIPLE = r"\b%s=(\d+)/(\d+)/(\d+)"
BANNER = re.compile(r"\bbuild (\S+); guest base=")
KV = re.compile(r"(\w+)=(\S+)")


@dataclass
class Window:
    win: int
    loops: int = 0
    total_loops: int = 0
    svc: tuple = (0, 0, 0)
    upd: tuple = (0, 0, 0)
    rnd: tuple = (0, 0, 0)
    all: tuple = (0, 0, 0)
    fread_lo: int = 0
    fread_hi: int = 0
    svc_samples: int = 0
    games: int | None = None
    room_markers: int | None = None
    at_us: int | None = None
    kind: str = ""


@dataclass
class Run:
    path: str
    bid: str = ""
    runs_in_file: int = 1
    windows: dict = field(default_factory=dict)
    order: list = field(default_factory=list)   # ("t", win) / ("stats", i)
    start_lines: list = field(default_factory=list)
    stats: list = field(default_factory=list)   # (dict, open window)
    worker: list = field(default_factory=list)
    recover: list = field(default_factory=list)
    fread_short: int = 0
    fread_complete_short: int | None = None
    closes: list = field(default_factory=list)
    bids: set = field(default_factory=set)


def kv(text: str) -> dict:
    out = {}
    for key, value in KV.findall(text):
        out[key] = int(value) if re.fullmatch(r"\d+", value) else value
    return out


def parse_top(top: str, samples: int) -> tuple[int, int]:
    """(lower, upper) bound of the samples under FREAD_RVA in one top list.

    The list is the 12 largest caller-view counts (kage_vita_guest_sampler.c
    kvgs_top_insert/kvgs_log_top), descending.  A listed entry is exact.  An
    absent one is 0 when the list is not full, and at most the smallest
    listed count when it is (it may have been cut)."""
    items = []
    for part in top.split(","):
        if ":" in part:
            name, count = part.rsplit(":", 1)
            items.append((name, int(count)))
    for name, count in items:
        if name == FREAD_RVA:
            return count, count
    if len(items) >= TOP_N:
        return 0, min(items[-1][1], samples)
    return 0, 0


def split_runs(lines: list[str]) -> list[list[str]]:
    """One list per launch.  The log async/rotate lines precede the build
    banner of their own launch, so only a second banner starts a new one."""
    runs: list[list[str]] = [[]]
    banner_seen = False
    for line in lines:
        if BANNER.search(line):
            if banner_seen:
                runs.append([])
            banner_seen = True
        runs[-1].append(line)
    return runs


def parse(path: str, text: str | None = None) -> Run:
    if text is None:
        with open(path, encoding="utf-8", errors="replace") as handle:
            text = handle.read()
    runs = split_runs(text.splitlines())
    run = Run(path=path, runs_in_file=len(runs))
    last_t = 0
    prev_total = 0
    for line in runs[-1]:
        banner = BANNER.search(line)
        if banner:
            run.bid = banner.group(1)
            continue
        m = RECORD.search(line)
        if m:
            tag, bid, win, total, rest = (m.group(1), m.group(2),
                                          int(m.group(3)), int(m.group(4)),
                                          m.group(5))
            run.bids.add(bid)
            w = run.windows.setdefault(win, Window(win))
            if tag == "t":
                for name in ("svc", "upd", "rnd", "all"):
                    value = re.search(TRIPLE % name, rest)
                    if value:
                        setattr(w, name, tuple(int(x) for x in value.groups()))
                w.total_loops = total
                w.loops = total - prev_total
                prev_total = total
                last_t = win
                run.order.append(("t", win))
            elif tag == "hotc" and "phase=svc " in rest:
                samples = int(re.search(r"samples=(\d+)", rest).group(1))
                top = re.search(r"top=(\S*)", rest).group(1)
                w.svc_samples = samples
                w.fread_lo, w.fread_hi = parse_top(top, samples)
            elif tag == "s":
                cad = re.search(r"cad\(w,s,d,m,g,r,b,p\)=([\d,]+)", rest)
                if cad:
                    w.games = int(cad.group(1).split(",")[4])
                at = re.search(r"\bat=([0-9a-fA-F]+)", rest)
                if at:
                    w.at_us = int(at.group(1), 16)
            elif tag == "rl":
                n = re.search(r"\bn=(\d+)", rest)
                if n:
                    w.room_markers = int(n.group(1))
            continue
        if "KAGE VITA OGG READAHEAD STATS:" in line:
            run.stats.append((kv(line.split("STATS:", 1)[1]), last_t + 1))
            run.order.append(("stats", len(run.stats) - 1))
        elif "KAGE VITA OGG READAHEAD WORKER:" in line:
            run.worker.append(kv(line.split("WORKER:", 1)[1]))
        elif "KAGE VITA OGG READAHEAD:" in line:
            run.start_lines.append(kv(line.split("READAHEAD:", 1)[1]))
        elif "KAGE VITA CRT DESCRIPTOR RECOVER:" in line:
            run.recover.append((kv(line.split("RECOVER:", 1)[1]),
                                len(run.stats)))
        elif "KAGE VITA CRT FREAD SHORT:" in line:
            run.fread_short += 1
        elif "KAGE VITA CRT FREAD COMPLETE:" in line:
            run.fread_complete_short = kv(line).get("short")
        elif "KAGE VITA NATIVE VORBIS CLOSE:" in line:
            run.closes.append(kv(line.split("CLOSE:", 1)[1]))
    for w in run.windows.values():
        w.kind = classify(w)
    return run


def classify(w: Window) -> str:
    if (w.room_markers or 0) > 0 or w.upd[2] > TRANSITION_UPD_US:
        return "transition"
    if w.svc[2] >= BURST_SVC_US:
        return "burst"
    if w.games is not None:
        return "game" if w.games >= GAME_UPDATES else "menu"
    # ph120.s missing: the phase-profile fallback, update p50 under 1 ms = menu
    return "menu" if w.upd[0] < 1000 else "game"


def complete_windows(run: Run) -> list[Window]:
    return [run.windows[k] for k in sorted(run.windows)
            if run.windows[k].loops > 0]


def class_rate(run: Run, kind: str, bound: str) -> tuple[int, int, int]:
    windows = [w for w in complete_windows(run) if w.kind == kind]
    samples = sum(w.fread_hi if bound == "hi" else w.fread_lo
                  for w in windows)
    return samples, sum(w.loops for w in windows), len(windows)


class Report:
    def __init__(self) -> None:
        self.verdicts: list[tuple[str, str, str]] = []

    def add(self, gate: str, verdict: str, detail: str) -> None:
        self.verdicts.append((gate, verdict, detail))
        print(f"{gate:3s} {verdict:12s} {detail}")

    def status(self) -> int:
        kinds = {v for _, v, _ in self.verdicts}
        if "FAIL" in kinds:
            return 1
        if "INCONCLUSIVE" in kinds:
            return 3
        return 0


def check_banner(run: Run) -> None:
    if run.runs_in_file > 1:
        print(f"note: {run.path} holds {run.runs_in_file} launches; "
              "only the last one is read")
    if len(run.bids) > 1:
        raise SystemExit(f"{run.path}: ph120 records of more than one build "
                         f"in one launch: {sorted(run.bids)}")


def gate_g1(report: Report, run: Run, want_on: bool) -> None:
    if not want_on:
        if run.start_lines or run.stats:
            report.add("G1", "FAIL", f"A ({run.path}) carries read-ahead lines:"
                       " the OFF build is not OFF")
        return
    if len(run.start_lines) != 1:
        report.add("G1", "FAIL", f"{len(run.start_lines)} start lines (want 1);"
                   " none means nothing registered and the knob is inert")
        return
    start = run.start_lines[0]
    ok = start.get("worker") == "on" and start.get("rc") == "0x00000000"
    geometry = " ".join(f"{k}={start.get(k)}" for k in
                        ("prio", "cpu", "entries", "chunks", "chunk",
                         "window", "wait_us"))
    if not ok or not run.stats:
        report.add("G1", "FAIL", f"worker={start.get('worker')} "
                   f"rc={start.get('rc')} stats_lines={len(run.stats)}")
    else:
        report.add("G1", "PASS", f"worker=on rc=0x00000000 {geometry}, "
                   f"{len(run.stats)} STATS lines")


def last_stats(run: Run) -> dict:
    return run.stats[-1][0] if run.stats else {}


def last_worker(run: Run) -> dict:
    return run.worker[-1] if run.worker else {}


def suspend_seen(run: Run) -> bool:
    return any(r.get("enodev", 0) > 0 or r.get("ebadf", 0) > 0
               for r, _ in run.recover)


def gate_g2(report: Report, run: Run) -> None:
    if not run.stats:
        report.add("G2", "INCONCLUSIVE", "no STATS line")
        return
    if suspend_seen(run):
        report.add("G2", "INCONCLUSIVE", "DESCRIPTOR RECOVER shows enodev/ebadf"
                   " > 0: this run had a suspend; judge it with `suspend`")
        return
    stats, worker = last_stats(run), last_worker(run)
    bad = [f"{k}={stats.get(k)}" for k in HEALTH_STATS if stats.get(k, 0)]
    bad += [f"{k}={worker.get(k)}" for k in HEALTH_WORKER if worker.get(k, 0)]
    if bad:
        report.add("G2", "FAIL", "no suspend, yet " + " ".join(bad))
    else:
        report.add("G2", "PASS", "failure counters all 0 in the last "
                   "STATS/WORKER pair")


@dataclass
class GameIntervals:
    hits: int = 0
    misses: int = 0
    miss_ms: int = 0
    loops: int = 0          # windows spanned between the lines (estimate)
    intervals: int = 0
    lines: int = 0


def game_intervals(run: Run) -> GameIntervals:
    """Sum of STATS deltas over the intervals that lie inside game windows.

    A STATS line printed after ph120.t win=N and before win=N+1 was printed
    while window N+1 was open; the reads it adds since the previous STATS
    line happened in the windows open between the two lines.  loops counts
    the windows between the two lines (q - p for lines printed in windows p
    and q), an estimate of the interval's length."""
    out = GameIntervals(lines=len(run.stats))
    prev = None
    prev_open = 0
    for stats, open_win in run.stats:
        if prev is not None:
            span = range(prev_open, open_win + 1)
            kinds = {run.windows[w].kind if w in run.windows and
                     run.windows[w].loops > 0 else "?" for w in span}
            if kinds == {"game"}:
                out.hits += stats.get("hits", 0) - prev.get("hits", 0)
                out.misses += stats.get("miss", 0) - prev.get("miss", 0)
                out.miss_ms += stats.get("miss_ms", 0) - prev.get("miss_ms", 0)
                out.loops += sum(run.windows[w].loops
                                 for w in range(prev_open + 1, open_win + 1))
                out.intervals += 1
        prev, prev_open = stats, open_win
    return out


def gate_g3(report: Report, run: Run) -> None:
    if not run.stats:
        report.add("G3", "INCONCLUSIVE", "no STATS line")
        return
    total = last_stats(run)
    whole = total.get("hits", 0) / max(1, total.get("hits", 0) +
                                       total.get("miss", 0))
    g = game_intervals(run)
    hits, misses, intervals, lines = g.hits, g.misses, g.intervals, g.lines
    if hits + misses < 64:
        report.add("G3", "INCONCLUSIVE", f"only {hits + misses} reads in "
                   f"{intervals} game-only STATS intervals (whole run "
                   f"{whole:.3f}, cumulative, not gated)")
        return
    ratio = hits / (hits + misses)
    detail = (f"game {ratio:.3f} ({hits}/{hits + misses} over {intervals} of "
              f"{lines - 1} intervals); whole run {whole:.3f} (cumulative, "
              "includes stream starts, not gated)")
    if ratio >= HIT_PASS:
        report.add("G3", "PASS", detail)
    elif ratio < HIT_FAIL:
        report.add("G3", "FAIL", detail + "; try _CHUNKS=8 before anything "
                   "else")
    else:
        report.add("G3", "INCONCLUSIVE", detail + f"; partial ({HIT_FAIL} .. "
                   f"{HIT_PASS})")


def gate_g4(report: Report, a: Run, b: Run) -> None:
    print("    class        A samples/loops  ms/loop   B samples/loops  ms/loop"
          "   down")
    verdicts = []
    for kind in ("game", "menu", "transition", "burst"):
        sa, la, na = class_rate(a, kind, "lo")
        sb, lb, nb = class_rate(b, kind, "hi")
        ra = sa / la if la else float("nan")
        rb = sb / lb if lb else float("nan")
        down = 1.0 - rb / ra if la and lb and ra > 0 else float("nan")
        ref = P2B_REFERENCE.get(kind)
        print(f"    {kind:11s} {sa:6d}/{la:<6d} {ra:8.3f}   {sb:6d}/{lb:<6d} "
              f"{rb:8.3f}  {down * 100:5.1f} %  (windows {na}/{nb}; P2b A "
              f"{ref[0] / ref[1]:.3f})")
        if kind not in ("game", "menu"):
            continue
        if na < MIN_CLASS_WINDOWS or nb < MIN_CLASS_WINDOWS or not ra > 0:
            verdicts.append((kind, "few", down))
        elif down >= PASS_DOWN:
            verdicts.append((kind, "pass", down))
        elif down < REAL_DOWN:
            verdicts.append((kind, "not-real", down))
        else:
            verdicts.append((kind, "partial", down))
    g = game_intervals(b)
    if g.loops:
        # What is left of B's game fread time, split: the real reads of the
        # misses (STATS miss_ms, measured by the module) against everything
        # else under fread (CPU in vita_crt_fread/newlib, hit copies), which
        # no read-ahead removes.  Interval lengths are estimated.
        sb, lb, _ = class_rate(b, "game", "hi")
        io = g.miss_ms / g.loops
        print(f"    B game intervals: miss I/O {io:.3f} ms/loop (STATS miss_ms "
              f"{g.miss_ms} over ~{g.loops} loops) of {sb / lb if lb else 0:.3f}"
              " ms/loop under fread; the rest is not card I/O")
    text = ", ".join(f"{k} {v} ({d * 100:.0f} % down)" for k, v, d in verdicts)
    states = {v for _, v, _ in verdicts}
    if "not-real" in states:
        report.add("G4", "FAIL", text + "; the saving is not real in that "
                   "class: do not ship it")
    elif states == {"pass"}:
        report.add("G4", "PASS", text)
    else:
        report.add("G4", "INCONCLUSIVE", text)


def game_windows(run: Run) -> list[Window]:
    return [w for w in complete_windows(run) if w.kind == "game"]


def gate_g5(report: Report, a: Run, b: Run) -> None:
    ga, gb = game_windows(a), game_windows(b)
    if not ga or not gb:
        report.add("G5", "INCONCLUSIVE", "no game window in A or B")
        return
    sa = sum(1 for w in ga if w.svc[2] >= SPIKE_SVC_US)
    sb = sum(1 for w in gb if w.svc[2] >= SPIKE_SVC_US)
    pa, pb = 100.0 * sa / len(ga), 100.0 * sb / len(gb)
    p95a = statistics.median(w.svc[1] for w in ga) / 1000
    p95b = statistics.median(w.svc[1] for w in gb) / 1000
    detail = (f"svc max >= 16 ms in {sa}/{len(ga)} A and {sb}/{len(gb)} B game "
              f"windows ({pa:.1f} vs {pb:.1f} per 100); median svc p95 "
              f"{p95a:.2f} vs {p95b:.2f} ms")
    if sa < 3:
        report.add("G5", "INCONCLUSIVE", detail + "; A has too few spikes")
    elif pb <= pa / 2:
        report.add("G5", "PASS", detail)
    else:
        report.add("G5", "FAIL", detail)


def gate_g6(report: Report, a: Run, b: Run) -> None:
    rows = []
    warn = []
    ga, gb = game_windows(a), game_windows(b)
    for label, pick in (("all p50", lambda w: w.all[0]),
                        ("all p95", lambda w: w.all[1]),
                        ("upd p95", lambda w: w.upd[1]),
                        ("rnd p95", lambda w: w.rnd[1])):
        if ga and gb:
            va = statistics.median(pick(w) for w in ga) / 1000
            vb = statistics.median(pick(w) for w in gb) / 1000
            rows.append(f"game {label} {va:.2f}->{vb:.2f}")
            if vb > va * 1.05:
                warn.append(f"game {label}")
    ta = [w for w in complete_windows(a) if w.kind == "transition"]
    tb = [w for w in complete_windows(b) if w.kind == "transition"]
    if ta and tb:
        va = statistics.median(w.upd[2] for w in ta) / 1000
        vb = statistics.median(w.upd[2] for w in tb) / 1000
        rows.append(f"transition upd max {va:.0f}->{vb:.0f}")
        if vb > va * 1.10:
            warn.append("transition upd max")
    report.add("G6", "WARN" if warn else "PASS", "; ".join(rows) + " ms" +
               (" (above A: " + ", ".join(warn) + ")" if warn else ""))


def run_minutes(run: Run) -> float:
    stamps = [w.at_us for w in complete_windows(run) if w.at_us is not None]
    if len(stamps) >= 2:
        return (max(stamps) - min(stamps)) / 60e6
    return len(complete_windows(run)) * 2.0 / 60.0


def gate_g7(report: Report, run: Run) -> None:
    if not run.stats:
        report.add("G7", "INCONCLUSIVE", "no STATS line")
        return
    stats = last_stats(run)
    minutes = run_minutes(run)
    allowed = max(1, math.ceil(minutes / 10.0))
    timeouts = stats.get("wait_to", 0)
    detail = (f"wait_to={timeouts} wait_ms={stats.get('wait_ms', 0)} over "
              f"{minutes:.1f} min (allowed {allowed})")
    report.add("G7", "PASS" if timeouts <= allowed else "FAIL", detail)


def extras(a: Run, b: Run) -> int:
    """Short-read and PCM receipts; returns the number of CLOSE pairs whose
    run= CRC differs."""
    print(f"    FREAD SHORT receipts A={a.fread_short} B={b.fread_short}; "
          f"FREAD COMPLETE short= A={a.fread_complete_short} "
          f"B={b.fread_complete_short}; WORKER short="
          f"{last_worker(b).get('short')}")
    pairs: dict = {}
    for c in a.closes:
        pairs.setdefault((c.get("f"), c.get("frames")), []).append(c.get("run"))
    matched = mismatched = 0
    for c in b.closes:
        runs = pairs.get((c.get("f"), c.get("frames")))
        if runs:
            if c.get("run") in runs:
                matched += 1
            else:
                mismatched += 1
    if not a.closes and not b.closes:
        print("    NATIVE VORBIS CLOSE pairs: not checked (RECEIPT off)")
    else:
        print(f"    NATIVE VORBIS CLOSE pairs with equal f= and frames=: "
              f"{matched} equal run=, {mismatched} different")
    return mismatched


def command_ab(a_path: str, b_path: str) -> int:
    a, b = parse(a_path), parse(b_path)
    check_banner(a)
    check_banner(b)
    print(f"A {a_path}: build {a.bid or '?'}, {len(complete_windows(a))} "
          f"windows, {run_minutes(a):.1f} min")
    print(f"B {b_path}: build {b.bid or '?'}, {len(complete_windows(b))} "
          f"windows, {run_minutes(b):.1f} min")
    report = Report()
    gate_g1(report, a, False)
    gate_g1(report, b, True)
    gate_g2(report, b)
    gate_g3(report, b)
    gate_g4(report, a, b)
    gate_g5(report, a, b)
    gate_g6(report, a, b)
    gate_g7(report, b)
    if extras(a, b):
        report.add("X1", "FAIL", "NATIVE VORBIS CLOSE run= differs for an "
                   "equal f=/frames= pair: the PCM is not identical")
    return report.status()


def gate_g8(report: Report, run: Run) -> None:
    hit = [r for r, _ in run.recover if r.get("enodev", 0) > 0]
    if not hit:
        report.add("G8", "INCONCLUSIVE", "no DESCRIPTOR RECOVER line with "
                   "enodev >= 1: the suspend did not reach a live descriptor "
                   "(or the line was not printed yet: let a periodic report "
                   "pass)")
        return
    recover = run.recover[-1][0]
    first_at = next(i for r, i in run.recover if r.get("enodev", 0) > 0)
    counters = dict(last_worker(run))
    counters.update(last_stats(run))
    zero = [f"{k}={counters.get(k)}" for k in SUSPEND_ZERO
            if counters.get(k, 0)]
    zero += [f"{k}={recover.get(k)}" for k in
             ("reopen_fail", "redo_fail", "unknown_pos") if recover.get(k, 0)]
    moved = {k: counters.get(k, 0) for k in SUSPEND_MAY_MOVE}
    detail = (f"enodev={recover.get('enodev')} recovered="
              f"{recover.get('recovered')}; " +
              " ".join(f"{k}={v}" for k, v in moved.items()))
    if zero:
        report.add("G8", "FAIL", detail + "; must be 0: " + " ".join(zero))
    elif not recover.get("recovered", 0):
        report.add("G8", "FAIL", detail + "; recovered=0: the CRT did not "
                   "recover the dead descriptor")
    else:
        report.add("G8", "PASS", detail)
        if not (moved["sync_fail"] or moved["real_fail"]):
            report.add("G8w", "WARN", "neither sync_fail nor real_fail moved: "
                       "no registered stream met its dead descriptor (worker "
                       "opened after the resume, or no music was streaming)")
        before = run.stats[first_at - 1][0] if first_at >= 1 else {}
        after = run.stats[-1][0] if run.stats else {}
        if len(run.stats) <= first_at or \
                after.get("hits", 0) <= before.get("hits", 0):
            report.add("G8w", "WARN", "no hit after the recovery line: check "
                       "that music still plays and a later STATS line exists")


def command_suspend(path: str) -> int:
    run = parse(path)
    check_banner(run)
    report = Report()
    gate_g1(report, run, True)
    gate_g8(report, run)
    print("    by ear: music continues after the resume without a gap")
    return report.status()


def command_windows(path: str) -> int:
    run = parse(path)
    check_banner(run)
    for w in complete_windows(run):
        print(f"win={w.win:4d} {w.kind:10s} loops={w.loops} g={w.games} "
              f"rl={w.room_markers} fread={w.fread_lo}"
              f"{'' if w.fread_hi == w.fread_lo else '..' + str(w.fread_hi)} "
              f"svc={w.svc[0]}/{w.svc[1]}/{w.svc[2]} upd={w.upd[0]}/"
              f"{w.upd[1]}/{w.upd[2]}")
    for kind in ("game", "menu", "transition", "burst"):
        s, loops, n = class_rate(run, kind, "lo")
        print(f"{kind:10s} windows={n:3d} loops={loops:6d} fread={s:5d} "
              f"ms/loop={s / loops if loops else float('nan'):.3f}")
    s = sum(w.fread_lo for w in complete_windows(run))
    loops = sum(w.loops for w in complete_windows(run))
    print(f"whole route windows={len(complete_windows(run))} loops={loops} "
          f"fread={s} ms/loop={s / loops if loops else float('nan'):.3f}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    ab = sub.add_parser("ab")
    ab.add_argument("a")
    ab.add_argument("b")
    sp = sub.add_parser("suspend")
    sp.add_argument("log")
    wn = sub.add_parser("windows")
    wn.add_argument("log")
    args = parser.parse_args(argv)
    if args.command == "ab":
        return command_ab(args.a, args.b)
    if args.command == "suspend":
        return command_suspend(args.log)
    return command_windows(args.log)


if __name__ == "__main__":
    sys.exit(main())
