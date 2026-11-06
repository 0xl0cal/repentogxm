#!/usr/bin/env python3
"""Compare two theora plane-digest streams (ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST).

A stream is the stdout of recomp/vita/theora_plane_digest.c (host oracle) or a
device log whose lines carry "KAGE VITA NATIVE THEORA DIGEST: " somewhere
(the seam's receipt, behind the log's timestamp/thread prefix).  Either way a
record is

    packet=<n> ret=<r> gp=<g> y=<8 hex> cb=<8 hex> cr=<8 hex>

or, when th_decode_ycbcr_out failed after a successful packetin (the device
only; the oracle prints those to stderr),

    packet=<n> ret=<r> ycbcr_out=<k> (no digest)

and every other line is ignored.  Records are keyed by the packet ordinal,
which the device resets at every th_decode_alloc (per clip or theoraplayer
restart) and which otherwise only grows, so a *run* ends at

  * a "KAGE VITA NATIVE THEORA: alloc" receipt (the hard separator),
  * an ordinal that went backwards (a restart whose first lines the async
    logger dropped, or whose packet 0 produced no record), or
  * a record for an ordinal already seen whose content differs (never folded
    into the earlier run; only a byte-identical repeat is a log replay and is
    ignored).

The oracle prints one clip, so a device log with two plays of the same clip
compares as two runs against the oracle's one run.  Two different clips in one
log fail on planes by construction: give a per-clip oracle stream with as many
runs, or cut the log to one clip (--runs N keeps the first N runs).

The gate: y/cb/cr must agree packet by packet (the decoded planes are the
picture; a difference means the archive is not bit-exact or the oracle is
wrong).  gp must agree too.  A "(no digest)" record fails.  ret may differ
only 0 -> 1 (libtheora 1.2.0 answers TH_DUPFRAME for inter frames with no
coded blocks where 1.1.1 answers 0; the planes are the same either way) and
only without --strict-ret; a dup frame lost (1 -> 0) or any other ret delta
fails.  --strict-ret is mandatory whenever both streams come from the same
libtheora version (device 1.2.0 vs the host 1.2.0 oracle, or two device runs
of one eboot): same-version decoding is deterministic, so any ret delta there
is a wrong oracle archive or a decoder difference; the lenient default is for
the 1.1.1-vs-1.2.0 leg of the gate only.  A stream that stops early (the
device log ended before the clip) is reported, and fails only with
--require-complete.  Exit status 0 when the gate passes, 1 when it fails, 2
for unusable input.

usage: theora_plane_digest_compare.py [--strict-ret] [--require-complete]
                                       [--runs N] <expected> <actual>
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


RECORD = re.compile(
    r"packet=(\d+) ret=(-?\d+) gp=(-?\d+) y=([0-9a-fA-F]{8}) cb=([0-9a-fA-F]{8}) cr=([0-9a-fA-F]{8})\b")
NO_DIGEST = re.compile(r"packet=(\d+) ret=(-?\d+) ycbcr_out=(-?\d+) \(no digest\)")
DEVICE_PREFIX = "KAGE VITA NATIVE THEORA DIGEST: "
# The seam's th_decode_alloc receipt ("alloc result=", "alloc FAILED", "alloc
# info=... answers NULL"): the ordinal restarts here, whatever follows.
ALLOC_RECEIPT = "KAGE VITA NATIVE THEORA: alloc"


@dataclass(frozen=True)
class Record:
    packet: int
    ret: int
    gp: int
    y: int
    cb: int
    cr: int
    line: int   # 1-based line number in the source text
    ycbcr_out: int | None = None   # set for a "(no digest)" record; planes are then meaningless

    @property
    def planes(self) -> tuple[int, int, int]:
        return (self.y, self.cb, self.cr)

    @property
    def digested(self) -> bool:
        return self.ycbcr_out is None

    def same(self, other: "Record") -> bool:
        """Equal apart from the source line (a log replay)."""
        return (self.packet, self.ret, self.gp, self.planes, self.ycbcr_out) == \
            (other.packet, other.ret, other.gp, other.planes, other.ycbcr_out)

    def text(self) -> str:
        if not self.digested:
            return f"packet={self.packet} ret={self.ret} ycbcr_out={self.ycbcr_out} (no digest)"
        return (f"packet={self.packet} ret={self.ret} gp={self.gp} "
                f"y={self.y:08x} cb={self.cb:08x} cr={self.cr:08x}")


@dataclass
class Run:
    records: dict[int, Record] = field(default_factory=dict)
    duplicates: int = 0     # a byte-identical repeat of a packet line (log replay)

    @property
    def dup_frames(self) -> int:
        return sum(1 for record in self.records.values() if record.ret == 1)

    @property
    def last(self) -> int:
        return max(self.records) if self.records else -1


def parse_record(line: str, number: int) -> Record | None:
    match = RECORD.search(line)
    if match is not None:
        return Record(packet=int(match.group(1)), ret=int(match.group(2)),
                      gp=int(match.group(3)), y=int(match.group(4), 16),
                      cb=int(match.group(5), 16), cr=int(match.group(6), 16),
                      line=number)
    match = NO_DIGEST.search(line)
    if match is not None:
        return Record(packet=int(match.group(1)), ret=int(match.group(2)), gp=-1,
                      y=-1, cb=-1, cr=-1, line=number, ycbcr_out=int(match.group(3)))
    return None


def parse_digests(text: str) -> list[Run]:
    """Runs of records from oracle stdout or a device log; other lines ignored."""
    runs: list[Run] = []
    current: Run | None = None
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw
        if ALLOC_RECEIPT in line:
            current = None      # the device ordinal restarts at every th_decode_alloc
            continue
        at = line.find(DEVICE_PREFIX)
        if at >= 0:
            line = line[at + len(DEVICE_PREFIX):]
        elif line.lstrip().startswith("#"):
            continue
        record = parse_record(line, number)
        if record is None:
            continue
        if current is not None and record.packet <= current.last:
            seen = current.records.get(record.packet)
            if record.packet != 0 and seen is not None and seen.same(record):
                # The same line twice inside a run (log replay): keep the first.
                current.duplicates += 1
                continue
            # packet=0 again (the ordinal reset), an ordinal that went
            # backwards, or the same ordinal with other content: a restart
            # whose alloc receipt / first records are not in the text.  Never
            # fold it into the run before it.
            current = None
        if current is None:
            current = Run()
            runs.append(current)
        current.records[record.packet] = record
    return runs


@dataclass
class Comparison:
    expected_count: int
    actual_count: int
    common: int
    plane_mismatches: list[tuple[Record, Record]]
    gp_mismatches: list[tuple[Record, Record]]
    no_digest: list[Record]  # "(no digest)" records in either stream
    ret_gained_dup: int      # expected 0, actual 1 (1.2.0 TH_DUPFRAME on zero-coded inter frames)
    ret_lost_dup: int        # expected 1, actual 0: never exempt
    ret_other: list[tuple[Record, Record]]
    missing: list[int]       # packets in expected, absent in actual
    extra: list[int]         # packets in actual, absent in expected
    expected_dups: int
    actual_dups: int

    def passes(self, strict_ret: bool, require_complete: bool) -> bool:
        if self.plane_mismatches or self.gp_mismatches or self.no_digest or self.ret_other:
            return False
        if self.ret_lost_dup:
            return False
        if strict_ret and self.ret_gained_dup:
            return False
        if require_complete and (self.missing or self.extra):
            return False
        return self.common > 0


def compare_runs(expected: Run, actual: Run) -> Comparison:
    planes: list[tuple[Record, Record]] = []
    gps: list[tuple[Record, Record]] = []
    others: list[tuple[Record, Record]] = []
    no_digest = sorted((record for run in (expected, actual) for record in run.records.values()
                        if not record.digested), key=lambda record: (record.packet, record.line))
    gained = lost = 0
    common = 0
    for packet in sorted(expected.records):
        want = expected.records[packet]
        got = actual.records.get(packet)
        if got is None:
            continue
        common += 1
        if not (want.digested and got.digested):
            continue    # already a failure through no_digest; the planes mean nothing
        if want.planes != got.planes:
            planes.append((want, got))
        if want.gp != got.gp:
            gps.append((want, got))
        if want.ret != got.ret:
            if want.ret == 0 and got.ret == 1:
                gained += 1
            elif want.ret == 1 and got.ret == 0:
                lost += 1
            else:
                others.append((want, got))
    return Comparison(
        expected_count=len(expected.records), actual_count=len(actual.records),
        common=common, plane_mismatches=planes, gp_mismatches=gps, no_digest=no_digest,
        ret_gained_dup=gained, ret_lost_dup=lost, ret_other=others,
        missing=sorted(set(expected.records) - set(actual.records)),
        extra=sorted(set(actual.records) - set(expected.records)),
        expected_dups=expected.dup_frames, actual_dups=actual.dup_frames)


def _first(pairs: list[tuple[Record, Record]]) -> str:
    want, got = pairs[0]
    return (f"first at packet={want.packet}: expected [{want.text()}] (line {want.line}) "
            f"actual [{got.text()}] (line {got.line})")


def format_report(index: int, comparison: Comparison, strict_ret: bool,
                  require_complete: bool) -> str:
    c = comparison
    lines = [f"run {index}: expected {c.expected_count} packets, actual {c.actual_count}, "
             f"common {c.common}; dup frames expected={c.expected_dups} actual={c.actual_dups} "
             f"(delta {c.actual_dups - c.expected_dups:+d})"]
    if c.plane_mismatches:
        lines.append(f"  PLANE MISMATCH x{len(c.plane_mismatches)}: {_first(c.plane_mismatches)}")
    else:
        lines.append("  planes: identical on every common packet")
    if c.gp_mismatches:
        lines.append(f"  GRANPOS MISMATCH x{len(c.gp_mismatches)}: {_first(c.gp_mismatches)}")
    if c.no_digest:
        first = c.no_digest[0]
        lines.append(f"  NO DIGEST x{len(c.no_digest)}: first [{first.text()}] (line {first.line})")
    if c.ret_gained_dup:
        lines.append(f"  ret deltas: 0->1 (TH_DUPFRAME gained) x{c.ret_gained_dup}"
                     + (" [FAIL: --strict-ret]" if strict_ret
                        else " [allowed: 1.1.1 -> 1.2.0 leg only]"))
    if c.ret_lost_dup:
        lines.append(f"  RET MISMATCH: 1->0 (TH_DUPFRAME lost) x{c.ret_lost_dup} [FAIL]")
    if c.ret_other:
        lines.append(f"  RET MISMATCH x{len(c.ret_other)}: {_first(c.ret_other)}")
    if c.missing:
        lines.append(f"  missing in actual x{len(c.missing)}: first packet={c.missing[0]}, "
                     f"last packet={c.missing[-1]}"
                     + (" [FAIL: --require-complete]" if require_complete else ""))
    if c.extra:
        lines.append(f"  extra in actual x{len(c.extra)}: first packet={c.extra[0]}"
                     + (" [FAIL: --require-complete]" if require_complete else ""))
    if c.common == 0:
        lines.append("  FAIL: no common packet")
    lines.append("  verdict: " + ("PASS" if c.passes(strict_ret, require_complete) else "FAIL"))
    return "\n".join(lines)


def compare_texts(expected_text: str, actual_text: str, strict_ret: bool = False,
                  require_complete: bool = False, runs: int | None = None
                  ) -> tuple[bool, str]:
    """(passes, report) for two streams; every actual run compares against the
    expected run of the same index, or the only expected run when there is one."""
    expected = parse_digests(expected_text)
    actual = parse_digests(actual_text)
    if not expected:
        return False, "expected stream has no digest record"
    if not actual:
        return False, "actual stream has no digest record"
    if len(expected) > 1 and len(expected) != len(actual):
        return False, (f"expected stream has {len(expected)} runs, actual {len(actual)}; "
                       "give a single-run oracle stream or matching runs")
    selected = actual if runs is None else actual[:runs]
    reports: list[str] = []
    ok = True
    for index, run in enumerate(selected):
        reference = expected[0] if len(expected) == 1 else expected[index]
        comparison = compare_runs(reference, run)
        if run.duplicates:
            reports.append(f"run {index}: {run.duplicates} identical repeated packet line(s) ignored")
        reports.append(format_report(index, comparison, strict_ret, require_complete))
        ok = ok and comparison.passes(strict_ret, require_complete)
    return ok, "\n".join(reports)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare two theora plane-digest streams (host oracle vs device log)")
    parser.add_argument("expected", type=Path, help="reference stream (host oracle stdout)")
    parser.add_argument("actual", type=Path, help="stream under test (oracle stdout or device log)")
    parser.add_argument("--strict-ret", action="store_true",
                        help="fail on any th_decode_packetin result delta, also 0 -> TH_DUPFRAME "
                             "(mandatory when both streams come from the same libtheora version)")
    parser.add_argument("--require-complete", action="store_true",
                        help="fail when the actual stream lacks or adds packets")
    parser.add_argument("--runs", type=int, default=None,
                        help="compare only the first N runs of the actual stream")
    arguments = parser.parse_args()
    try:
        expected_text = arguments.expected.read_text(encoding="utf-8", errors="replace")
        actual_text = arguments.actual.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        print(f"theora_plane_digest_compare: {exc}", file=sys.stderr)
        return 2
    ok, report = compare_texts(expected_text, actual_text, arguments.strict_ret,
                               arguments.require_complete, arguments.runs)
    print(report)
    if not parse_digests(expected_text) or not parse_digests(actual_text):
        return 2
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
