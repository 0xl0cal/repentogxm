#!/usr/bin/env python3
"""Tests for ogg_readahead_gates.py, the device gates of ISAAC_VITA_OGG_READAHEAD.

The A side is the measured P2b session (the 2026-10-17 device log
p2b-native.log, knob absent): its 111 ph120
windows are kept in host_tests/ogg_readahead_p2b_windows.csv and rendered
back into device-format log lines here (three verbatim P2b lines check that
rendering).  The B sides are models built from it, each named after what it
models.  The suspend runs carry the module's own STATS/WORKER lines from the
host oracle scenarios suspend and suspend-late-open
(host_tests/ogg_readahead_oracle_suspend.txt; test_ogg_readahead.sh feeds
the live oracle lines through the same evaluator).

Every case states what a correct gate must answer.  --rules r1 (default)
judges with ogg_readahead_gates.py; --rules r0 judges with the first draft
of the gate text (transcribed below), which was found wrong.  r0 must fail
the cases that show its two defects and r1 must pass all of them.

    test_ogg_readahead_gates.py [--rules r0|r1] [--p2b LOG] [--oracle-log LOG]
    test_ogg_readahead_gates.py --refresh-fixture LOG

--p2b also proves the fixture equals the parse of the real log.
"""
from __future__ import annotations

import argparse
import contextlib
import csv
import io
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ogg_readahead_gates as gates  # noqa: E402

FIXTURE = HERE / "host_tests" / "ogg_readahead_p2b_windows.csv"
ORACLE_LINES = HERE / "host_tests" / "ogg_readahead_oracle_suspend.txt"
FIELDS = ("win", "total_loops", "at_us", "games", "room_markers",
          "svc", "upd", "rnd", "all", "svc_samples", "fread_lo", "fread_hi")
BID = "rel:v0.1.2-alpha-int4-session-p2"
BANNER_A = ("[0.327 thr 0x40010003] build rel:v0.1.2-alpha-int4-session-"
            "p2b-20261017; guest base=0x98000000; softfp ARMv7")
# Verbatim P2b lines of window 1 (p2b-native.log), the rendering reference.
VERBATIM_W1 = (
    "[kage-vita] ph120.t bid=rel:v0.1.2-alpha-int4-session-p2 win=1 loops=120 "
    "us(50,95,max) svc=39/4316/89616 upd=293/396/3892769 rnd=12177/12545/30208"
    " swp=127/163/18053 lim=0/0/0 all=16643/26444/3899468 gap(n,min,50,95,max)"
    "=108/3935/16643/21840/4147877 bad=0 clamp=0",
    "[kage-vita] ph120.hotc bid=rel:v0.1.2-alpha-int4-session-p2 win=1 "
    "loops=120 phase=svc samples=262 top=596560:245,5b06f0:5,5be080:3,"
    "5a20e0:3,59c270:2,5bf100:2,598d60:1,56f040:1",
    "[kage-vita] ph120.s bid=rel:v0.1.2-alpha-int4-session-p2 win=1 loops=120 "
    "at=0000000001ff7a9e cad(w,s,d,m,g,r,b,p)=120,120,120,120,0,109,109,109 "
    "phase(f,n,i)=60,60,60 skip(a,l,n,f)=11,6,5,0 viol(d,p,g,s,o)=0,0,0,0,0 "
    "reset=0 drop(t,us)=290,4833333 wait(c,us)=92,504355 debt=285/78601",
    "[kage-vita] ph120.rl bid=rel:v0.1.2-alpha-int4-session-p2 win=1 loops=120"
    " epoch=1 n=0 seq=0 drop=0 reset_lost=0 sat=0",
)
START = ("[1.000 thr 0x40010003] KAGE VITA OGG READAHEAD: worker=on "
         "rc=0x00000000 prio=168 cpu=2 entries=4 chunks=4 chunk=16384 "
         "window=65536 wait_us=20000")
STATS_KEYS = ("reads", "hits", "eof", "miss", "hit_kb", "miss_kb", "miss_ms",
              "waits", "wait_ms", "wait_to", "busy", "vseek", "fseek", "sync",
              "sync_fail", "pass", "real_fail", "foreign", "reg", "full",
              "reg_fail", "retired", "fsync", "live")
WORKER_KEYS = ("fills", "fill_kb", "fill_ms", "fill_err", "short",
               "open_fail", "dropped")


# ---------------------------------------------------------------- fixture --
def window_row(w: gates.Window) -> list[str]:
    row = []
    for name in FIELDS:
        value = getattr(w, name)
        row.append("/".join(map(str, value)) if isinstance(value, tuple)
                   else str(value))
    return row


def refresh_fixture(log: str) -> None:
    run = gates.parse(log)
    with FIXTURE.open("w", newline="\n", encoding="utf-8") as handle:
        handle.write("# P2b A side, p2b-native.log 2026-10-17, parsed by "
                     "ogg_readahead_gates.py\n")
        out = csv.writer(handle, lineterminator="\n")
        out.writerow(FIELDS)
        for w in gates.complete_windows(run):
            out.writerow(window_row(w))
    print(f"wrote {FIXTURE} ({len(gates.complete_windows(run))} windows)")


def load_fixture() -> list[dict]:
    with FIXTURE.open(encoding="utf-8") as handle:
        rows = [line for line in handle if not line.startswith("#")]
    out = []
    for row in csv.DictReader(rows):
        item = {}
        for name in FIELDS:
            value = row[name]
            item[name] = tuple(int(x) for x in value.split("/")) \
                if "/" in value else int(value)
        out.append(item)
    return out


# -------------------------------------------------------------- rendering --
def render_window(w: dict, total_loops: int, fread: int | None = None,
                  svc_max: int | None = None) -> list[str]:
    """Device-format ph120.t/.hotc/.s/.rl lines for one window.  fread
    replaces the 596560 count (the other callers keep their share of the
    samples); svc_max replaces the svc max."""
    svc = w["svc"] if svc_max is None else (w["svc"][0], w["svc"][1], svc_max)
    lo = w["fread_lo"] if fread is None else fread
    samples = w["svc_samples"] - w["fread_lo"] + lo
    loops = total_loops
    head = f"[kage-vita] ph120.%s bid={BID} win={w['win']} loops={loops}"
    t = " ".join(f"{name}={'/'.join(map(str, w[name] if name != 'svc' else svc))}"
                 for name in ("svc", "upd", "rnd"))
    lines = [head % "t" + f" us(50,95,max) {t} swp=0/0/0 lim=0/0/0 all="
             f"{'/'.join(map(str, w['all']))} gap(n,min,50,95,max)=0/0/0/0/0 "
             "bad=0 clamp=0"]
    if samples:
        others = samples - lo
        top = ([f"596560:{lo}"] if lo else []) + \
            ([f"59c270:{others}"] if others else [])
        lines.append(head % "hotc" + f" phase=svc samples={samples} top="
                     + ",".join(top))
    lines.append(head % "s" + f" at={w['at_us']:016x} cad(w,s,d,m,g,r,b,p)="
                 f"120,120,120,120,{w['games']},120,120,120 phase(f,n,i)=0,0,0")
    lines.append(head % "rl" + f" epoch=1 n={w['room_markers']} seq=0 drop=0")
    return lines


def stats_line(c: dict) -> str:
    return ("[2.000 thr 0x40010003] KAGE VITA OGG READAHEAD STATS: " +
            " ".join(f"{k}={c.get(k, 0)}" for k in STATS_KEYS))


def worker_line(c: dict) -> str:
    return ("[2.000 thr 0x40010003] KAGE VITA OGG READAHEAD WORKER: " +
            " ".join(f"{k}={c.get(k, 0)}" for k in WORKER_KEYS))


def recover_line(enodev: int, recovered: int, reopen_fail: int = 0) -> str:
    return ("[3.000 thr 0x40010003] KAGE VITA CRT DESCRIPTOR RECOVER: "
            f"why=periodic enodev={enodev} recovered={recovered} "
            f"unknown_pos=0 reopen_fail={reopen_fail} redo_fail=0 retained=3 "
            "ebadf=0 fd_repaired=0 fd_leaked=0")


def kind_of(w: dict) -> str:
    return gates.classify(gates.Window(
        w["win"], svc=w["svc"], upd=w["upd"], games=w["games"],
        room_markers=w["room_markers"]))


def render_run(windows: list[dict], knob: bool, scale: float | None = None,
               keep: frozenset = frozenset(), hit_ratio: dict | None = None,
               reads_per_window: int = 12, drop: frozenset = frozenset(),
               period: int = 16, svc_cap: int | None = None) -> str:
    """A whole launch.  scale multiplies the 596560 samples of every window
    not in keep (the read-ahead's effect); svc_cap lowers the svc max of
    every game window not in keep to at most that many us; hit_ratio maps a
    window class to the hit share of its reads (default 0.9); drop leaves
    windows out of the route (the cumulative loop counter follows the windows
    kept).  With the knob a STATS and WORKER pair is printed every `period`
    reads (the module's ISAAC_OGG_RA_REPORT_PERIOD), while the window in
    which that read falls is open, as report_if_due does.  12 reads a window
    is an estimate:
    P2b's 0.27 ms/loop of gameplay fread time over 120 loops at 2.5 ms per
    16 KiB read."""
    lines = [BANNER_A]
    if knob:
        lines.append(START)
    counters = {k: 0 for k in STATS_KEYS}
    counters["reg"] = 1
    worker = {k: 0 for k in WORKER_KEYS}
    since = 0
    total = previous = 0
    for w in windows:
        loops, previous = w["total_loops"] - previous, w["total_loops"]
        if w["win"] in drop:
            continue
        total += loops
        if knob:
            ratio = (hit_ratio or {}).get(kind_of(w), 0.9)
            hits = round(reads_per_window * ratio)
            for i in range(reads_per_window):
                counters["reads"] += 1
                counters["hits" if i < hits else "miss"] += 1
                worker["fills"] += 1
                since += 1
                if since == period:
                    since = 0
                    lines.append(stats_line(counters))
                    lines.append(worker_line(worker))
        fread = None
        if scale is not None and w["win"] not in keep:
            fread = round(w["fread_lo"] * scale)
        svc_max = None
        if (svc_cap is not None and w["win"] not in keep and
                kind_of(w) == "game" and w["svc"][2] > svc_cap):
            svc_max = svc_cap
        lines.extend(render_window(w, total, fread=fread, svc_max=svc_max))
    return "\n".join(lines) + "\n"


# ------------------------------------------------- r0 (first draft) --
def r0_g4(a: gates.Run, b: gates.Run) -> str:
    """First-draft G4: samples under 596560 over all windows /
    loops; 'B must be <= 0.15 ms/loop (>= 70 % down).  Under 50 % down means
    the saving is not real'."""
    def rate(run, bound):
        ws = gates.complete_windows(run)
        s = sum(w.fread_hi if bound == "hi" else w.fread_lo for w in ws)
        return s / sum(w.loops for w in ws)
    ra, rb = rate(a, "lo"), rate(b, "hi")
    if rb <= 0.15:
        return "PASS"
    return "FAIL" if 1 - rb / ra < 0.50 else "INCONCLUSIVE"


def r0_g4_down(a: gates.Run, b: gates.Run) -> float:
    ws_a, ws_b = gates.complete_windows(a), gates.complete_windows(b)
    ra = sum(w.fread_lo for w in ws_a) / sum(w.loops for w in ws_a)
    rb = sum(w.fread_hi for w in ws_b) / sum(w.loops for w in ws_b)
    return 1 - rb / ra


def r0_g3(b: gates.Run) -> str:
    """First-draft G3: 'last STATS line: hits / (hits + miss) >= 0.90'."""
    s = gates.last_stats(b)
    ratio = s["hits"] / (s["hits"] + s["miss"])
    return "PASS" if ratio >= 0.90 else ("FAIL" if ratio < 0.7 else
                                         "INCONCLUSIVE")


def r0_g8(run: gates.Run) -> str:
    """First-draft G2 (real_fail sync_fail foreign reg_fail full and
    fill_err short open_fail all 0) with G8 (fill_err >= 1 and pass
    growing expected, real_fail >= 1 allowed)."""
    s, w = gates.last_stats(run), gates.last_worker(run)
    exempt = {"fill_err", "pass", "real_fail"}
    for key in ("real_fail", "sync_fail", "foreign", "reg_fail", "full"):
        if key not in exempt and s.get(key, 0):
            return "FAIL"
    for key in ("fill_err", "short", "open_fail"):
        if key not in exempt and w.get(key, 0):
            return "FAIL"
    return "PASS"


# ------------------------------------------------------------- r1 (tool) --
def r1(gate: str, *runs: gates.Run) -> str:
    report = gates.Report()
    with contextlib.redirect_stdout(io.StringIO()):
        if gate == "G1on":
            gates.gate_g1(report, runs[0], True)
        else:
            getattr(gates, "gate_" + gate.lower())(report, *runs)
    verdicts = [v for g, v, _ in report.verdicts if g == gate[:2]]
    return verdicts[0] if verdicts else "NONE"


def r1_detail(gate: str, *runs: gates.Run) -> str:
    report = gates.Report()
    with contextlib.redirect_stdout(io.StringIO()):
        getattr(gates, "gate_" + gate.lower())(report, *runs)
    return report.verdicts[0][2]


# ---------------------------------------------------------------- cases --
class Cases:
    def __init__(self, rules: str) -> None:
        self.rules = rules
        self.failed: list[str] = []
        self.count = 0

    def expect(self, name: str, got: str, want: str, why: str) -> None:
        self.count += 1
        ok = got == want
        print(f"{'ok  ' if ok else 'FAIL'} {name}: {got} (want {want}) -- {why}")
        if not ok:
            self.failed.append(name)

    def check(self, name: str, condition: bool, detail: str) -> None:
        self.count += 1
        print(f"{'ok  ' if condition else 'FAIL'} {name}: {detail}")
        if not condition:
            self.failed.append(name)


def suspend_blocks(oracle_log: str | None = None) -> dict[str, list[str]]:
    """The module's STATS/WORKER pair of each suspend scenario: from the
    fixture, or (oracle_log) from a live oracle log, where each scenario's
    `  log:` receipt lines come right before its `  [name] reads=` summary."""
    blocks: dict[str, list[str]] = {}
    if oracle_log:
        pending: list[str] = []
        text = Path(oracle_log).read_text(encoding="utf-8", errors="replace")
        for raw in text.splitlines():
            line = raw.strip()
            if line.startswith("log: KAGE VITA OGG READAHEAD STATS:"):
                pending = [line]
            elif line.startswith("log: KAGE VITA OGG READAHEAD WORKER:"):
                pending.append(line)
            elif line.startswith("[") and "] reads=" in line:
                name = line[1:line.index("]")]
                if len(pending) == 2:
                    blocks[name] = pending
                pending = []
        missing = {"suspend", "suspend-late-open"} - set(blocks)
        if missing:
            sys.exit(f"{oracle_log}: no receipt pair for {sorted(missing)}")
        return blocks
    name = None
    for line in ORACLE_LINES.read_text(encoding="utf-8").splitlines():
        if line.startswith("[") and line.endswith("]"):
            name = line[1:-1]
            blocks[name] = []
        elif name and line.strip():
            blocks[name].append(line)
    return blocks


def suspend_log(oracle: list[str], *, recover: str | None, later_hits: bool,
                patch: dict | None = None) -> str:
    """A suspend run: healthy STATS before the suspend, the oracle's own
    STATS/WORKER pair after it, a DESCRIPTOR RECOVER line, then (later_hits)
    one more STATS pair with hits grown and the recovered stream registered."""
    after = gates.kv(oracle[0].split("STATS:", 1)[1])
    worker = gates.kv(oracle[1].split("WORKER:", 1)[1])
    if patch:
        after.update({k: v for k, v in patch.items() if k in after})
        worker.update({k: v for k, v in patch.items() if k in worker})
    before = {k: 0 for k in STATS_KEYS}
    before.update(reads=10, hits=9, miss=1, reg=1)
    lines = [BANNER_A, START, stats_line(before), worker_line({"fills": 12})]
    # the module's own lines, verbatim, unless a counter is patched
    lines += [stats_line(after), worker_line(worker)] if patch else oracle[:2]
    if recover:
        lines.append(recover)
    if later_hits:
        grown = dict(after)
        grown.update(reads=after["reads"] + 64, hits=after["hits"] + 60,
                     miss=after["miss"] + 4, reg=after["reg"] + 1)
        lines += [stats_line(grown), worker_line(worker)]
    return "\n".join(lines) + "\n"


def run_cases(rules: str, p2b: str | None,
              oracle_log: str | None = None) -> int:
    cases = Cases(rules)
    windows = load_fixture()
    a = gates.parse("A", render_run(windows, knob=False))

    # The fixture and its rendering are the measurement.
    if p2b:
        real = gates.parse(p2b)
        rows = [window_row(w) for w in gates.complete_windows(real)]
        fixed = [window_row(w) for w in gates.complete_windows(
            gates.parse("fixture", render_run(windows, knob=False)))]
        cases.check("fixture == P2b log", rows == fixed,
                    f"{len(rows)} windows of {p2b} against {FIXTURE.name}")
    verbatim = gates.parse("verbatim", BANNER_A + "\n" + "\n".join(VERBATIM_W1))
    rendered = gates.parse("rendered", render_run(windows[:1], knob=False))
    cases.check("rendering == device format", window_row(verbatim.windows[1])
                == window_row(rendered.windows[1]),
                "window 1 rendered from the CSV parses like the P2b lines")
    for kind, (samples, loops) in gates.P2B_REFERENCE.items():
        got = gates.class_rate(a, kind, "lo")[:2]
        cases.check(f"P2b {kind} reference", got == (samples, loops),
                    f"{got} == P2B_REFERENCE {samples, loops}")
    lo, hi = gates.parse_top("1:9,2:8,3:7,4:6,5:5,6:5,7:4,8:4,9:3,a:3,b:2,c:2",
                             60)
    cases.check("top-12 cut", (lo, hi) == (0, 2),
                f"absent from a full list -> (0, smallest)={lo, hi}")
    lo, hi = gates.parse_top("1:9,2:8", 17)
    cases.check("top short list", (lo, hi) == (0, 0),
                f"absent from a list that is not full -> {lo, hi}")

    # Defect 1: G4 and the saving.  The scenario: every steady
    # refill hits except 10 % (G3's own pass level), windows 1, 73 and 74
    # (boot, two menu bursts of 292 and 431 ms) unchanged.
    floor = frozenset({1, 73, 74})
    working = gates.parse("B-working", render_run(
        windows, knob=True, scale=0.10, keep=floor))
    judge4 = r0_g4 if rules == "r0" else (lambda x, y: r1("G4", x, y))
    cases.expect("G4 working read-ahead", judge4(a, working), "PASS",
                 "90 % of the fread time gone outside windows 1/73/74: "
                 "r0 defect 1")
    inert = gates.parse("B-inert", render_run(windows, knob=True))
    cases.expect("G4 inert knob", judge4(a, inert), "FAIL",
                 "B's fread equals A's: the saving is not real")
    partial = gates.parse("B-partial", render_run(
        windows, knob=True, scale=0.40, keep=floor))
    cases.expect("G4 partial read-ahead", judge4(a, partial), "INCONCLUSIVE",
                 "60 % down in both classes: neither pass nor 'not real'")
    route = gates.parse("B-route", render_run(windows, knob=False,
                                              drop=frozenset({73, 74})))
    cases.expect("G4 route mix only", judge4(a, route), "FAIL",
                 "no knob effect, B's route skips the two burst windows")
    down = r0_g4_down(a, route)
    cases.check("route mix moves the whole-route figure", down > 0.20,
                f"r0 arithmetic shows {down * 100:.0f} % 'saving' from the "
                "route alone")
    if rules == "r1":
        detail = r1_detail("G4", a, route)
        cases.check("route mix leaves classes alone", "game not-real (0 %" in
                    detail and "menu not-real (0 %" in detail, detail)

    # G3 has the same flaw: the cumulative line mixes stream starts in.
    g3run = gates.parse("B-g3", render_run(
        windows, knob=True, scale=0.10, keep=floor,
        hit_ratio={"game": 0.95, "menu": 0.80, "transition": 0.40,
                   "burst": 0.10}))
    judge3 = r0_g3 if rules == "r0" else (lambda x: r1("G3", x))
    cases.expect("G3 game hit ratio", judge3(g3run), "PASS",
                 "95 % hits in game windows, stream starts in menus and "
                 "transitions pull the cumulative ratio under 0.90")
    if rules == "r1":
        g = gates.game_intervals(g3run)
        cases.check("G3 has game-only intervals", g.intervals >= 10,
                    f"{g.intervals} of {g.lines - 1} STATS intervals inside "
                    "game windows at 16 reads per line")
        sparse = gates.parse("B-g3-64", render_run(
            windows, knob=True, scale=0.10, keep=floor, period=64,
            hit_ratio={"game": 0.95, "menu": 0.80, "transition": 0.40,
                       "burst": 0.10}))
        g64 = gates.game_intervals(sparse)
        cases.check("G3 cadence 16 against 64", g.intervals >= 4 * g64.intervals,
                    f"game-only intervals {g.intervals} ({g.hits + g.misses} "
                    f"reads) at 16 reads per line, {g64.intervals} "
                    f"({g64.hits + g64.misses} reads) at 64: why "
                    "ISAAC_OGG_RA_REPORT_PERIOD is 16")

        # G5 and G7, so that every device gate has a host case.
        calm = gates.parse("B-calm", render_run(
            windows, knob=True, scale=0.10, keep=floor, svc_cap=6000))
        cases.expect("G5 spikes gone", r1("G5", a, calm), "PASS",
                     r1_detail("G5", a, calm))
        cases.expect("G5 spikes kept", r1("G5", a, working), "FAIL",
                     "the fread time fell but every 16 ms svc max stayed")
        few = gates.parse("A-few", render_run(windows, knob=False,
                                              svc_cap=6000))
        cases.expect("G5 A without spikes", r1("G5", few, calm),
                     "INCONCLUSIVE", "A needs 3 spike windows to decide")
        text = render_run(windows, knob=True, scale=0.10, keep=floor)
        last = text.rindex("wait_to=0 ")
        for timeouts, want in ((1, "PASS"), (2, "FAIL")):
            run = gates.parse(f"B-wait{timeouts}", text[:last] +
                              f"wait_to={timeouts} " +
                              text[last + len("wait_to=0 "):])
            cases.expect(f"G7 wait_to={timeouts}", r1("G7", run), want,
                         r1_detail("G7", run))

    # Defect 2: G8 against G2 on the suspend run.
    oracle = suspend_blocks(oracle_log)
    if oracle_log:
        print(f"suspend receipts: live, from {oracle_log}")
    judge8 = r0_g8 if rules == "r0" else (lambda x: r1("G8", x))
    ok_recover = recover_line(1, 1)
    for name in ("suspend", "suspend-late-open"):
        run = gates.parse(name, suspend_log(oracle[name], recover=ok_recover,
                                            later_hits=True))
        s = gates.last_stats(run)
        cases.expect(f"G8 correct {name} run", judge8(run), "PASS",
                     f"oracle counters sync_fail={s['sync_fail']} "
                     f"real_fail={s['real_fail']} pass={s['pass']}: r0 "
                     "defect 2")
    if rules == "r1":
        bad = gates.parse("foreign", suspend_log(
            oracle["suspend"], recover=ok_recover, later_hits=True,
            patch={"foreign": 1}))
        cases.expect("G8 foreign still counts", r1("G8", bad), "FAIL",
                     "the suspend exemption covers only the four counters")
        bad = gates.parse("norecover", suspend_log(
            oracle["suspend"], recover=recover_line(1, 0, 1),
            later_hits=True))
        cases.expect("G8 failed recovery", r1("G8", bad), "FAIL",
                     "recovered=0 reopen_fail=1")
        none = gates.parse("nosuspend", suspend_log(
            oracle["suspend"], recover=recover_line(0, 0), later_hits=True))
        cases.expect("G8 without a suspend", r1("G8", none), "INCONCLUSIVE",
                     "enodev=0: the run did not suspend a live descriptor")
        clean = gates.parse("clean", suspend_log(
            oracle["suspend"], recover=recover_line(0, 0), later_hits=False))
        cases.expect("G2 sync_fail without a suspend", r1("G2", clean),
                     "FAIL", "the zero rule still holds for runs without a "
                     "suspend")
        hidden = gates.parse("hidden", suspend_log(
            oracle["suspend"], recover=ok_recover, later_hits=False))
        cases.expect("G2 refuses a suspend run", r1("G2", hidden),
                     "INCONCLUSIVE", "enodev=1: judged by `suspend`")
        healthy = gates.parse("healthy", render_run(
            windows, knob=True, scale=0.10, keep=floor))
        cases.expect("G2 healthy run", r1("G2", healthy), "PASS",
                     "no failure counter moved")
        cases.expect("G1 started", r1("G1on", healthy), "PASS",
                     "start line once, worker=on rc=0")
        cases.expect("G1 absent", r1("G1on", a), "FAIL",
                     "no start line: nothing registered")

    print(f"{cases.count - len(cases.failed)}/{cases.count} cases as expected "
          f"under rules {rules}")
    if cases.failed:
        print("failed: " + ", ".join(cases.failed))
        return 1
    print(f"ogg readahead gates ({rules}): PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rules", choices=("r0", "r1"), default="r1")
    parser.add_argument("--p2b", help="the real P2b native log")
    parser.add_argument("--refresh-fixture", metavar="LOG")
    parser.add_argument("--oracle-log", metavar="LOG",
                        help="take the suspend receipts from a live oracle "
                        "log instead of the fixture")
    args = parser.parse_args()
    if args.refresh_fixture:
        refresh_fixture(args.refresh_fixture)
        return 0
    return run_cases(args.rules, args.p2b, args.oracle_log)


if __name__ == "__main__":
    sys.exit(main())
