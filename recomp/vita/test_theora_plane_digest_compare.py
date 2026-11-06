#!/usr/bin/env python3
"""Inline fixtures for theora_plane_digest_compare.py."""

from __future__ import annotations

import sys

import theora_plane_digest_compare as cmp


HOST = """# theora_plane_digest archive='Xiph.Org libtheora 1.2.0 20250329 (Ptalarbvorm)' file='a.ogv'
packet=0 ret=0 gp=0 y=11111111 cb=22222222 cr=33333333
packet=1 ret=0 gp=1 y=aaaaaaaa cb=bbbbbbbb cr=cccccccc
packet=2 ret=1 gp=2 y=aaaaaaaa cb=bbbbbbbb cr=cccccccc
packet=3 ret=0 gp=3 y=44444444 cb=55555555 cr=66666666
packet=4 ret=0 gp=4 y=77777777 cb=88888888 cr=99999999
"""

DEVICE = """[12.001 thr 0x40010003] KAGE VITA NATIVE THEORA: alloc result=1 version=3.2.1 frame=960x544
[12.002 thr 0x40010003] KAGE VITA NATIVE THEORA: archive=Xiph.Org libtheora 1.2.0 20250329 (Ptalarbvorm) native_th_info=60
[12.003 thr 0x40010003] KAGE VITA NATIVE THEORA POOL: alloc mspace=0x81000000 bytes=2621440 live=1 peak=1
[12.040 thr 0x40010003] KAGE VITA NATIVE THEORA DIGEST: packet=0 ret=0 gp=0 y=11111111 cb=22222222 cr=33333333
[12.070 thr 0x40010003] KAGE VITA NATIVE THEORA: packet ctx=0x81000000 packets=1 (dup=0 err=0)
[12.071 thr 0x40010003] KAGE VITA NATIVE THEORA DIGEST: packet=1 ret=0 gp=1 y=aaaaaaaa cb=bbbbbbbb cr=cccccccc
[12.090 thr 0x40010003] KAGE VITA NATIVE THEORA DIGEST: packet=2 ret=1 gp=2 y=aaaaaaaa cb=bbbbbbbb cr=cccccccc
[12.120 thr 0x40010003] KAGE VITA NATIVE THEORA DIGEST: packet=3 ret=0 gp=3 y=44444444 cb=55555555 cr=66666666
[12.150 thr 0x40010003] KAGE VITA NATIVE THEORA DIGEST: packet=4 ret=0 gp=4 y=77777777 cb=88888888 cr=99999999
"""

# The device receipts of a second play: the alloc receipt, then the records.
ALLOC = "[20.000 thr 0x40010003] KAGE VITA NATIVE THEORA: alloc result=1 version=3.2.1 frame=960x544\n"
RECORDS = "\n".join(line for line in DEVICE.splitlines() if "DIGEST: " in line) + "\n"


def lines_without(text: str, needle: str) -> str:
    return "\n".join(line for line in text.splitlines() if needle not in line) + "\n"


def main() -> int:
    # Parsing: comment line ignored, device prefix stripped, one run each.
    host = cmp.parse_digests(HOST)
    device = cmp.parse_digests(DEVICE)
    assert len(host) == 1 and len(device) == 1, (host, device)
    assert sorted(host[0].records) == [0, 1, 2, 3, 4]
    assert host[0].records[2].ret == 1 and host[0].dup_frames == 1
    assert device[0].records[4].planes == (0x77777777, 0x88888888, 0x99999999)
    assert device[0].records[3].line == 8

    # Identical streams pass, host vs host and device log vs host.
    ok, report = cmp.compare_texts(HOST, HOST)
    assert ok and "planes: identical" in report and "verdict: PASS" in report, report
    ok, report = cmp.compare_texts(HOST, DEVICE, strict_ret=True, require_complete=True)
    assert ok, report

    # A plane mismatch at packet 3 fails and is the first one reported.
    bad = HOST.replace("packet=3 ret=0 gp=3 y=44444444", "packet=3 ret=0 gp=3 y=44444445")
    ok, report = cmp.compare_texts(HOST, bad)
    assert not ok and "PLANE MISMATCH x1: first at packet=3" in report, report
    assert "expected [packet=3 ret=0 gp=3 y=44444444 cb=55555555 cr=66666666]" in report, report
    assert "actual [packet=3 ret=0 gp=3 y=44444445 cb=55555555 cr=66666666]" in report, report

    # 1.2.0 answering TH_DUPFRAME where 1.1.1 answered 0: allowed by default,
    # counted as a dup delta, rejected with --strict-ret.
    dup = HOST.replace("packet=4 ret=0 gp=4", "packet=4 ret=1 gp=4")
    ok, report = cmp.compare_texts(HOST, dup)
    assert ok and "0->1 (TH_DUPFRAME gained) x1 [allowed" in report and "(delta +1)" in report, report
    ok, report = cmp.compare_texts(HOST, dup, strict_ret=True)
    assert not ok and "[FAIL: --strict-ret]" in report, report
    # The other direction (a dup frame lost) is never exempt: only 0 -> 1 is
    # the documented 1.1.1 -> 1.2.0 delta.
    ok, report = cmp.compare_texts(dup, HOST)
    assert not ok and "RET MISMATCH: 1->0 (TH_DUPFRAME lost) x1 [FAIL]" in report, report
    assert "(delta -1)" in report and "verdict: FAIL" in report, report

    # Any other ret delta (an error code) fails.
    err = HOST.replace("packet=4 ret=0 gp=4", "packet=4 ret=-20 gp=4")
    ok, report = cmp.compare_texts(HOST, err)
    assert not ok and "RET MISMATCH x1: first at packet=4" in report, report

    # A granule position delta fails.
    gp = HOST.replace("packet=1 ret=0 gp=1", "packet=1 ret=0 gp=7")
    ok, report = cmp.compare_texts(HOST, gp)
    assert not ok and "GRANPOS MISMATCH x1: first at packet=1" in report, report

    # The device log ended early: reported, passes unless --require-complete.
    short = "\n".join(DEVICE.splitlines()[:7]) + "\n"
    ok, report = cmp.compare_texts(HOST, short)
    assert ok and "missing in actual x2: first packet=3, last packet=4" in report, report
    ok, report = cmp.compare_texts(HOST, short, require_complete=True)
    assert not ok and "[FAIL: --require-complete]" in report, report

    # Two plays in one device log: the alloc receipt and the ordinal restart
    # at 0 -> two runs, each compared against the single oracle run; a
    # mismatch in the second is found.
    second = DEVICE.replace("y=77777777", "y=70707070")
    two = DEVICE + second
    runs = cmp.parse_digests(two)
    assert len(runs) == 2 and len(runs[0].records) == 5 and len(runs[1].records) == 5, runs
    ok, report = cmp.compare_texts(HOST, two)
    assert not ok and "run 0:" in report and "run 1:" in report, report
    assert report.index("verdict: PASS") < report.index("PLANE MISMATCH x1: first at packet=4"), report
    ok, report = cmp.compare_texts(HOST, two, runs=1)
    assert ok, report

    # The alloc receipt alone separates runs, even when the second play's
    # packet=0 line never made it into the log (async logger drop) and its
    # records are all wrong: nothing is folded into run 0, run 1 fails.
    corrupted = RECORDS.replace("cccccccc", "deadbeef").replace("66666666", "deadbeef") \
        .replace("99999999", "deadbeef")
    dropped = DEVICE + ALLOC + lines_without(corrupted, "packet=0 ")
    runs = cmp.parse_digests(dropped)
    assert len(runs) == 2 and sorted(runs[1].records) == [1, 2, 3, 4], runs
    assert runs[0].duplicates == 0 and runs[1].duplicates == 0, runs
    ok, report = cmp.compare_texts(HOST, dropped, require_complete=True)
    assert not ok and "run 1:" in report, report
    assert "PLANE MISMATCH x4: first at packet=1" in report, report
    assert "missing in actual x1: first packet=0, last packet=0 [FAIL: --require-complete]" in report, report

    # No alloc receipt either (the oracle run twice, or every receipt of the
    # restart dropped): an ordinal that went backwards to a non-zero value
    # still opens a new run instead of being counted as a repeated line.
    backwards = HOST + "\n".join(HOST.splitlines()[2:]).replace("aaaaaaaa", "deadbeef") + "\n"
    runs = cmp.parse_digests(backwards)
    assert len(runs) == 2 and sorted(runs[1].records) == [1, 2, 3, 4], runs
    ok, report = cmp.compare_texts(HOST, backwards)
    assert not ok and "run 1:" in report and "PLANE MISMATCH x2: first at packet=1" in report, report
    assert "repeated packet line" not in report, report
    # A second play of the same clip whose first records were dropped and whose
    # kept records are identical is harmless (they agree with run 0); the first
    # record that differs opens a run of its own and is compared.
    same_then_bad = HOST + "packet=3 ret=0 gp=3 y=44444444 cb=55555555 cr=66666666\n" + \
        "packet=4 ret=0 gp=4 y=deadbeef cb=88888888 cr=99999999\n"
    runs = cmp.parse_digests(same_then_bad)
    assert len(runs) == 2 and runs[0].duplicates == 1 and sorted(runs[1].records) == [4], runs
    ok, report = cmp.compare_texts(HOST, same_then_bad)
    assert not ok and "PLANE MISMATCH x1: first at packet=4" in report, report

    # Two full oracle runs pasted together are two runs (packet=0 always
    # restarts, identical or not).
    assert len(cmp.parse_digests(HOST + HOST)) == 2

    # A repeated line inside a run (log replay) is ignored, not a restart.
    replay = HOST + "packet=2 ret=1 gp=2 y=aaaaaaaa cb=bbbbbbbb cr=cccccccc\n"
    runs = cmp.parse_digests(replay)
    assert len(runs) == 1 and runs[0].duplicates == 1, runs
    ok, report = cmp.compare_texts(HOST, replay)
    assert ok and "1 identical repeated packet line(s) ignored" in report, report

    # The device's "(no digest)" receipt (th_decode_ycbcr_out failed after a
    # successful packetin) is a record and a failure, not a silently missing
    # packet; it still keys the run.
    nodigest = DEVICE.replace(
        "KAGE VITA NATIVE THEORA DIGEST: packet=3 ret=0 gp=3 y=44444444 cb=55555555 cr=66666666",
        "KAGE VITA NATIVE THEORA DIGEST: packet=3 ret=0 ycbcr_out=-1 (no digest)")
    runs = cmp.parse_digests(nodigest)
    assert len(runs) == 1 and sorted(runs[0].records) == [0, 1, 2, 3, 4], runs
    assert not runs[0].records[3].digested and runs[0].records[3].ycbcr_out == -1
    assert runs[0].records[3].text() == "packet=3 ret=0 ycbcr_out=-1 (no digest)"
    ok, report = cmp.compare_texts(HOST, nodigest)
    assert not ok and "NO DIGEST x1: first [packet=3 ret=0 ycbcr_out=-1 (no digest)] (line 8)" in report, report
    assert "PLANE MISMATCH" not in report and "missing in actual" not in report, report
    # ... and a second play whose packet 0 is a "(no digest)" record still
    # starts a run at 0, so its later records are compared as run 1.
    restart = DEVICE + ALLOC + RECORDS.replace(
        "packet=0 ret=0 gp=0 y=11111111 cb=22222222 cr=33333333",
        "packet=0 ret=0 ycbcr_out=-1 (no digest)")
    runs = cmp.parse_digests(restart)
    assert len(runs) == 2 and sorted(runs[1].records) == [0, 1, 2, 3, 4], runs
    ok, report = cmp.compare_texts(HOST, restart)
    assert not ok and "run 1:" in report and "NO DIGEST x1: first [packet=0 ret=0 ycbcr_out=-1 (no digest)]" in report, report

    # Streams without records are unusable.
    ok, report = cmp.compare_texts("nothing here\n", HOST)
    assert not ok and "expected stream has no digest record" == report, report
    ok, report = cmp.compare_texts(HOST, "# only a comment\n")
    assert not ok and "actual stream has no digest record" == report, report

    # Two expected runs against one actual run is a usage error.
    ok, report = cmp.compare_texts(HOST + HOST, HOST)
    assert not ok and "expected stream has 2 runs, actual 1" in report, report

    # Record text round-trips in the device receipt format (minus the prefix).
    record = host[0].records[0]
    assert record.text() == "packet=0 ret=0 gp=0 y=11111111 cb=22222222 cr=33333333"
    print("PASS test_theora_plane_digest_compare")
    return 0


if __name__ == "__main__":
    sys.exit(main())
