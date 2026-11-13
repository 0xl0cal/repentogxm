#!/usr/bin/env python3
"""Reader for ISAAC_VITA_UPDATE_HITCH_PROBE records (ph120.uh / ph120.uhs).

  python update_hitch.py NATIVE.log [--bid BID] [--json] [--allow-unpaired]

One ph120.uh line = one Game::Update that lasted at least the build's threshold; the paired
ph120.uhs line (same bid and n) lists the guest sampler's 1 kHz samples that fell inside that
update.  Field legend: recomp/vita/README.md "Update hitch probe"; contract:
recomp/runtime/kage_vita_update_hitch.h.

The log on the device is cumulative across boots, so the build id matters: without --bid the
records of every build are printed, grouped by bid in order of first appearance; with --bid only
that build.  Events are never filtered by size or content; the count printed is the number of
records read.

A group printed as 'off' (seam not compiled into that build) or 'na' (a snapshot could not be
read) stays 'off'/'na' here.  It is not a zero.

Pairing: a ph120.uh whose smp(seq) group printed a number was followed by a ph120.uhs in the
build (kage_vita_update_hitch_record.h prints it under exactly that condition).  If the log lacks
it, a line was lost (async logger drop, truncated file): that raises, unless --allow-unpaired,
which counts it and prints a WARNING instead.  smp(seq)=off / =na events have no ph120.uhs.

us= is NOT the window's ph120.sp upd.  ph120.sp upd is the whole upd phase (Manager::Update entry
to render entry) and contains the scheduler's Game::Update floor wait; ph120.uh us= starts after
that wait and ends when Game::Update returns.  So us <= upd for the same loop, and the table
'ph120.sp kind=upd cross-check' prints the difference.  A window whose worst upd is at or above
the threshold but has no ph120.uh for that loop is not a probe defect: Game::Update itself was
under the threshold and the time is the floor wait or Manager::Update outside Game::Update.

Sampler keys are translated-function RVAs in hex, except three markers copied from
recomp/runtime/kage_vita_guest_sampler.c (KVGS_RVA_UNKNOWN / KVGS_RVA_EXTERNAL /
KVGS_RVA_PRESENT), shown as ind / ext / present.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# kage_vita_guest_sampler.c: #define KVGS_RVA_UNKNOWN UINT32_C(0x0fffffff),
# KVGS_RVA_EXTERNAL UINT32_C(0x0ffffffe), KVGS_RVA_PRESENT UINT32_C(0x0ffffffd).
# recomp/vita/test_kage_vita_update_hitch.py (parser leg) re-reads the three values from that file.
SAMPLER_MARKERS = {0x0FFFFFFF: "ind", 0x0FFFFFFE: "ext", 0x0FFFFFFD: "present"}

UH = re.compile(
    r"ph120\.uh bid=(?P<bid>\S+) abi=(?P<abi>\d+) n=(?P<n>\d+) win=(?P<win>\d+) "
    r"loop=(?P<loop>\d+) tid=(?P<tid>[0-9a-f]{8}) thr_us=(?P<thr_us>\d+) us=(?P<us>\d+) "
    r"begin=(?P<begin>[0-9a-f]{16}) frame=(?P<frame_before>\d+)>(?P<frame_after>\d+)"
    r"(?P<groups>(?: \w+\([\w,]+\)=\S+)+)\s*$")
GROUP = re.compile(r" (\w+)\(([\w,]+)\)=(\S+)")
UHS = re.compile(
    r"ph120\.uhs bid=(?P<bid>\S+) abi=(?P<abi>\d+) n=(?P<n>\d+) samples=(?P<samples>\d+) "
    r"lost=(?P<lost>\d+) self=(?P<self>\S+) caller=(?P<caller>\S+)\s*$")
BANNER = re.compile(
    r"KAGE VITA UPDATE HITCH PROBE: bid=(?P<bid>\S+) abi=(?P<abi>\d+) "
    r"threshold_us=(?P<threshold_us>\d+) trail=(?P<trail>\d+) top=(?P<top>\d+) "
    r"seams: (?P<seams>.*\S)\s*$")

# kage_vita_phase_profile.c, kage_vita_profile_log_spikes: "[kage-vita] ph120.sp bid=%.32s win=%u
# loops=%u kind=%.3s loop=%u at_ms=%u all=%u svc=%u upd=%u ...".  Only kind=upd is read.  Other
# builds in a cumulative log may print another shape; a non-matching ph120.sp line is counted,
# not an error (it is not this probe's record).
SP_UPD = re.compile(
    r"ph120\.sp bid=(?P<bid>\S+) win=(?P<win>\d+) loops=(?P<loops>\d+) kind=upd "
    r"loop=(?P<loop>\d+) at_ms=(?P<at_ms>\d+) all=(?P<all>\d+) svc=(?P<svc>\d+) "
    r"upd=(?P<upd>\d+) ")

SUPPORTED_ABI = 1


class HitchLogError(ValueError):
    pass


def _top(text: str) -> list[tuple[str, int]]:
    if text == "-":
        return []
    out = []
    for item in text.split(","):
        key, count = item.split(":")
        value = int(key, 16)
        out.append((SAMPLER_MARKERS.get(value, f"{value:x}"), int(count)))
    return out


def parse(lines, allow_unpaired: bool = False) -> dict:
    """{'banners': [...], 'events': [...], 'sp': [...], 'unpaired': [...], 'sp_other': n} in
    log order.  Raises HitchLogError on a line that carries a probe tag but does not match the
    record grammar (a truncated or merged line must not silently become a missing event), and
    on a ph120.uh that should have a ph120.uhs and has none (unless allow_unpaired).

    'boot' on events and sp records counts that bid's banners seen so far: the banner is printed
    at the first Game::Update of every boot, so equal (bid, boot, loop) means the same update."""
    banners, events, by_key = [], [], {}
    sp, sp_other, boots = [], 0, {}
    for number, raw in enumerate(lines, 1):
        line = raw.rstrip("\r\n")
        if "ph120.sp " in line:
            m = SP_UPD.search(line)
            if m:
                sp.append({"line": number, "bid": m["bid"], "boot": boots.get(m["bid"], 0),
                           "win": int(m["win"]), "loop": int(m["loop"]),
                           "upd": int(m["upd"])})
            elif " kind=upd " in line or " kind=" not in line:
                sp_other += 1
            continue
        if "ph120.uhs " in line:
            m = UHS.search(line)
            if not m:
                raise HitchLogError(f"line {number}: malformed ph120.uhs: {line[:160]}")
            if int(m["abi"]) != SUPPORTED_ABI:
                raise HitchLogError(f"line {number}: ph120.uhs abi {m['abi']}")
            event = by_key.get((m["bid"], int(m["n"])))
            if event is None or "samples" in event:
                raise HitchLogError(
                    f"line {number}: ph120.uhs n={m['n']} bid={m['bid']} has no single ph120.uh")
            event["samples"] = int(m["samples"])
            event["lost"] = int(m["lost"])
            event["self"] = _top(m["self"])
            event["caller"] = _top(m["caller"])
        elif "ph120.uh " in line:
            m = UH.search(line)
            if not m:
                raise HitchLogError(f"line {number}: malformed ph120.uh: {line[:160]}")
            if int(m["abi"]) != SUPPORTED_ABI:
                raise HitchLogError(f"line {number}: ph120.uh abi {m['abi']}")
            event = {
                "line": number, "bid": m["bid"], "n": int(m["n"]), "win": int(m["win"]),
                "loop": int(m["loop"]), "tid": m["tid"], "thr_us": int(m["thr_us"]),
                "us": int(m["us"]), "begin_us": int(m["begin"], 16),
                "frame_before": int(m["frame_before"]), "frame_after": int(m["frame_after"]),
                "boot": boots.get(m["bid"], 0), "groups": {},
            }
            for name, fields, values in GROUP.findall(m["groups"]):
                names = fields.split(",")
                if values in ("off", "na"):
                    event["groups"][name] = values
                    continue
                numbers = values.split(",")
                if len(numbers) != len(names):
                    raise HitchLogError(
                        f"line {number}: group {name} has {len(numbers)} values for "
                        f"{len(names)} fields")
                event["groups"][name] = {k: int(v) for k, v in zip(names, numbers)}
            # A reboot of the same build restarts n at 1: key on the latest.
            by_key[(m["bid"], event["n"])] = event
            events.append(event)
        elif "UPDATE HITCH PROBE" in line:
            m = BANNER.search(line)
            if not m:
                raise HitchLogError(f"line {number}: malformed probe banner: {line[:160]}")
            banners.append({
                "line": number, "bid": m["bid"], "abi": int(m["abi"]),
                "threshold_us": int(m["threshold_us"]), "trail": int(m["trail"]),
                "top": int(m["top"]),
                "seams": dict(item.split("=") for item in m["seams"].split()),
            })
            boots[m["bid"]] = boots.get(m["bid"], 0) + 1
    # The build prints ph120.uhs exactly when smp(seq) printed a number.
    unpaired = [e for e in events
                if isinstance(e["groups"].get("smp"), dict) and "samples" not in e]
    if unpaired and not allow_unpaired:
        e = unpaired[0]
        raise HitchLogError(
            f"line {e['line']}: ph120.uh n={e['n']} bid={e['bid']} has smp(seq)="
            f"{e['groups']['smp']['seq']} but no ph120.uhs follows ({len(unpaired)} such "
            "event(s)): a log line was lost; --allow-unpaired reads the rest")
    return {"banners": banners, "events": events, "sp": sp,
            "unpaired": [{"line": e["line"], "bid": e["bid"], "n": e["n"]} for e in unpaired],
            "sp_other": sp_other}


def cross_check(parsed: dict, bid: str | None) -> list[dict]:
    """One row per ph120.sp kind=upd record: the ph120.uh of the same (bid, boot, loop) if
    any.  verdict: 'uh' (matched, us <= upd), 'INCONSISTENT' (matched, us > upd: cannot
    happen, the probe's interval lies inside the upd phase), 'outside' (no ph120.uh although
    upd >= threshold: the time is not in Game::Update), 'below' (upd under the threshold),
    'unknown' (no threshold known for that bid)."""
    by_loop, threshold = {}, {}
    for e in parsed["events"]:
        by_loop[(e["bid"], e["boot"], e["loop"])] = e
        threshold[e["bid"]] = e["thr_us"]
    for b in parsed["banners"]:
        threshold[b["bid"]] = b["threshold_us"]
    rows = []
    for s in parsed["sp"]:
        if bid not in (None, s["bid"]):
            continue
        e = by_loop.get((s["bid"], s["boot"], s["loop"]))
        row = {"bid": s["bid"], "line": s["line"], "win": s["win"], "loop": s["loop"],
               "upd": s["upd"], "n": None, "us": None}
        if e is not None and e["line"] < s["line"]:
            row.update(n=e["n"], us=e["us"],
                       verdict="uh" if e["us"] <= s["upd"] else "INCONSISTENT")
        elif s["bid"] not in threshold:
            row["verdict"] = "unknown"
        else:
            row["verdict"] = "outside" if s["upd"] >= threshold[s["bid"]] else "below"
        rows.append(row)
    return rows


def _value(event: dict, group: str, field: str) -> str:
    data = event["groups"].get(group)
    if data is None:
        return "?"
    if isinstance(data, str):
        return data
    return str(data[field])


def _tops(items: list[tuple[str, int]] | None) -> str:
    if items is None:
        return "(no ph120.uhs)"
    return " ".join(f"{key}:{count}" for key, count in items) or "-"


def render(parsed: dict, bid: str | None) -> str:
    out = []
    banners = [b for b in parsed["banners"] if bid in (None, b["bid"])]
    events = [e for e in parsed["events"] if bid in (None, e["bid"])]
    for b in banners:
        seams = " ".join(f"{k}={v}" for k, v in b["seams"].items())
        out.append(f"banner line {b['line']}: bid={b['bid']} threshold_us={b['threshold_us']} "
                   f"trail={b['trail']} seams: {seams}")
    if not banners:
        out.append("banner: NONE (the probe is not in this build, or the log does not reach "
                   "the first Game::Update)")
    out.append(f"events: {len(events)} ph120.uh records"
               + (f" for bid {bid}" if bid else " (all builds)"))
    lost = [u for u in parsed.get("unpaired", []) if bid in (None, u["bid"])]
    if lost:
        out.append(f"WARNING: {len(lost)} ph120.uh record(s) with smp(seq) numbers but no "
                   f"ph120.uhs (log lines lost), n = {', '.join(str(u['n']) for u in lost)}")
    seen = []
    for e in events:
        if e["bid"] not in seen:
            seen.append(e["bid"])
    for name in seen:
        out.append(f"--- bid {name}")
        out.append("   n  win  loop   upd_ms  tex  fh_miss  pngi  png_img  sfx_load(c/us)  "
                   "sfx_play_us  lua_pc_us  lua_gc_us  samples/lost")
        for e in (x for x in events if x["bid"] == name):
            sfx = e["groups"].get("sfx")
            if isinstance(sfx, dict):
                # RIFF parsing is inside the WAV scope (host_vita_static_sfx_profile.c
                # header comment), so it is not added a second time.
                load = (f"{sfx['wav_c'] + sfx['ogg_c']}/"
                        f"{sfx['wav_us'] + sfx['ogg_us'] + sfx['up_us']}")
            else:
                load = sfx or "?"
            out.append(
                f"{e['n']:4d} {e['win']:4d} {e['loop']:5d} {e['us'] / 1000.0:8.1f} "
                f"{_value(e, 'gl', 'tex'):>4} {_value(e, 'fh', 'miss'):>8} "
                f"{_value(e, 'pngi', 'attempt'):>5} {_value(e, 'png', 'images'):>8} "
                f"{load:>15} {_value(e, 'sfx', 'play_us'):>12} "
                f"{_value(e, 'lua', 'pc_us'):>10} {_value(e, 'lua', 'gc_us'):>10}  "
                f"{e.get('samples', '?')}/{e.get('lost', '?')}")
            out.append(f"       self:   {_tops(e.get('self'))}")
            out.append(f"       caller: {_tops(e.get('caller'))}")
    rows = cross_check(parsed, bid)
    if rows:
        out.append("--- ph120.sp kind=upd cross-check: sp upd = the window's worst upd PHASE "
                   "(Manager::Update with the floor wait and Game::Update in it); uh us = "
                   "Game::Update only, so us <= upd")
        out.append(" win  loop  sp_upd_ms  uh_n   uh_ms  outside_ms  reading")
        text = {
            "uh": "same loop; outside_ms = floor wait + Manager::Update around Game::Update",
            "INCONSISTENT": "INCONSISTENT: us > sp upd for the same loop (report it)",
            "outside": "NO ph120.uh: upd >= threshold but Game::Update was under it; the time "
                       "is the floor wait or Manager::Update (not a probe defect)",
            "below": "no ph120.uh expected: the window's worst upd is under the threshold",
            "unknown": "no banner or event for this bid: threshold unknown",
        }
        for r in rows:
            if r["n"] is None:
                mid = f"{'-':>5} {'-':>7} {'-':>11}"
            else:
                mid = f"{r['n']:5d} {r['us'] / 1000.0:7.1f} {(r['upd'] - r['us']) / 1000.0:11.1f}"
            out.append(f"{r['win']:4d} {r['loop']:5d} {r['upd'] / 1000.0:10.1f} {mid}  "
                       f"{text[r['verdict']]}")
    if parsed.get("sp_other"):
        out.append(f"note: {parsed['sp_other']} ph120.sp kind=upd line(s) of another shape "
                   "were not read")
    return "\n".join(out)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("log", type=Path)
    parser.add_argument("--bid", help="only this build id (the log is cumulative across boots)")
    parser.add_argument("--json", action="store_true", help="print the parsed records as JSON")
    parser.add_argument("--allow-unpaired", action="store_true",
                        help="a ph120.uh that lost its ph120.uhs is a counted WARNING, not an error")
    args = parser.parse_args(argv)
    with args.log.open("r", encoding="utf-8", errors="replace", newline="") as handle:
        try:
            parsed = parse(handle, args.allow_unpaired)
        except HitchLogError as error:
            print(f"update_hitch: {error}", file=sys.stderr)
            return 2
    if args.json:
        if args.bid:
            parsed = {k: ([x for x in v if x["bid"] == args.bid] if isinstance(v, list) else v)
                      for k, v in parsed.items()}
        print(json.dumps(parsed, indent=1))
    else:
        print(render(parsed, args.bid))
    return 0


if __name__ == "__main__":
    sys.exit(main())
