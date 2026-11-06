#!/usr/bin/env python3
"""Host tests of sfx_fight_list.py: python sfx_fight_list_test.py [--artifacts DIR]

Legs:
  parse    first-play dedup, the MARK P2..P4 fight segment (and its refusal), the Downpour
           floor toggle on Level::Init lines
  sessions sha256 dedup, a strict-prefix copy dropped, the 10-sound floor, scripted detection
  rule     tiers F/D/R on hand-built evidence, each threshold one step either side, floor ids
           excluded, the walk order
  frozen   the shipped sfx_fight_evidence.json re-derives its own list, sizes add up to the
           module's SFXP_FIGHT_KIB
  fit      the cost fit recovers known coefficients from exact rows
  ids      the frozen evidence names its logs by neutral ids only (bench-01.., downpour-01..,
           log-01..), and extract() numbers them in argument / path order
  phases   with --artifacts (or BENCH_SMOKE_ROOT): the phase tables of bench-01 and bench-02
           (found under the root by the evidence's sha256 pins, native.log beside game.log)
           reproduce the measured numbers (P3 WAV 25 files 244.7 ms / 23 files 281.8 ms, the
           sound-28 windows 31/24 at 41.7/57.4 ms with worst updates 55.7/69.7 ms)
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sfx_fight_list as f  # noqa: E402

FAILS: list[str] = []


def check(name: str, cond: bool, detail="") -> None:
    if not cond:
        FAILS.append(f"{name}: {detail}")
        print(f"FAIL {name} {detail}")


def warn(n: int) -> str:
    return f"[WARN] - [warn] Sound {n} was not preloaded.\n"


def leg_parse() -> None:
    text = warn(5) + warn(7) + warn(5) + warn(9)
    check("first_plays dedup/order", f.first_plays(text) == [5, 7, 9], f.first_plays(text))
    game = (warn(1) + "[repro_console] MARK P1 a |\n" + warn(2) + "[repro_console] MARK P2 b |\n"
            + warn(3) + "[repro_console] MARK P3 c |\n" + warn(4) + "[repro_console] MARK P4 d |\n"
            + warn(6))
    check("bench_fight P2..P4", f.bench_fight(game) == [3, 4], f.bench_fight(game))
    try:
        f.bench_fight(warn(1) + "[repro_console] MARK P2 b |\n" + warn(3))
        check("bench_fight refuses a run without P4", False)
    except ValueError:
        pass
    dp = ("Level::Init m_Stage 1, m_StageType 0 Seed 1\n" + warn(10) +
          "Level::Init m_Stage 1, m_StageType 4 Seed 2\n" + warn(11) + warn(11) +
          "Level::Init m_Stage 2, m_StageType 4 Seed 3\n" + warn(12) +
          "Level::Init m_Stage 3, m_StageType 4 Seed 4\n" + warn(13))
    check("downpour toggle", f.downpour_plays(dp) == [11, 12], f.downpour_plays(dp))


def leg_sessions() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        body = "".join(warn(i) for i in range(1, 12))       # 11 sounds
        (root / "a").mkdir()
        (root / "a" / "log.txt").write_text(body, encoding="utf-8")
        (root / "a" / "copy.log").write_text(body, encoding="utf-8")            # same sha
        (root / "a" / "head.log").write_text(body[: len(body) // 2], encoding="utf-8")  # prefix
        (root / "b").mkdir()
        (root / "b" / "few.log").write_text("".join(warn(i) for i in range(1, 10)) + "x",
                                            encoding="utf-8")    # 9 sounds: below the floor
        (root / "b" / "bench.log").write_text("[repro_console] exec [start]\n" + body + "y",
                                              encoding="utf-8")
        (root / "b" / "other.bin").write_text(body + "z", encoding="utf-8")      # not .log/.txt
        ss = f.sessions(root)
        paths = sorted(s["path"] for s in ss)
        check("sessions kept", paths == ["a/copy.log", "b/bench.log"] or
              paths == ["a/log.txt", "b/bench.log"], paths)
        check("scripted flag", {s["path"].split("/")[0]: s["scripted"] for s in ss} ==
              {"a": False, "b": True}, ss)


def evidence(bench, dp, real, n_real=10) -> dict:
    sessions = [{"path": f"s{i}", "sha256": "", "first_play": []} for i in range(n_real)]
    for sid, k in real.items():
        for i in range(k):
            sessions[i]["first_play"].append(sid)
    return {"floor18": list(f.FLOOR18),
            "bench": {f"r{i}": {"sha256": "", "fight_first_play": ids} for i, ids in enumerate(bench)},
            "downpour": {f"d{i}": {"sha256": "", "downpour_first_play": ids}
                         for i, ids in enumerate(dp)},
            "sessions": {"real": sessions, "scripted": 0}}


def leg_rule() -> None:
    # 100: F (one bench run); 472: a floor id, never listed; 101: D (3/3 Downpour, 10 %);
    # 102: 3/3 Downpour but 0 % real -> none; 103: 2/3 Downpour, 10 % -> none;
    # 104: R at 20 %; 105: 10 % only -> none; 106: R, sorts after 104 (fewer Downpour logs)
    ev = evidence(bench=[[100, 472], [], []],
                  dp=[[101, 102, 103, 104], [101, 102, 103, 104], [101, 102]],
                  real={101: 1, 103: 1, 104: 2, 105: 1, 106: 3, 472: 9})
    got = [(r["id"], r["tier"]) for r in f.derive(ev)]
    check("rule tiers and order", got == [(100, "F"), (101, "D"), (104, "R"), (106, "R")], got)
    # equal counts sort by id; 8 (a floor id) never appears
    ev2 = evidence(bench=[[], [], []], dp=[[], [], []], real={9: 2, 8: 2, 7: 2, 6: 2})
    got2 = [r["id"] for r in f.derive(ev2)]
    check("tie order by id", got2 == [6, 7, 9], got2)


def leg_frozen() -> None:
    ev = json.loads(f.EVIDENCE.read_text(encoding="utf-8"))
    lst = [(r["id"], r["tier"]) for r in f.derive(ev)]
    check("frozen list re-derived", lst == [(r["id"], r["tier"]) for r in ev["list"]], lst)
    check("frozen counts", (len(ev["sessions"]["real"]), len(ev["bench"]), len(ev["downpour"]))
          == (39, 3, 3), (len(ev["sessions"]["real"]), len(ev["bench"]), len(ev["downpour"])))
    module = (HERE.parents[1] / "recomp" / "runtime" / "host_vita_sfx_floor_preload.c").read_text(
        encoding="utf-8")
    m = re.search(r"#define SFXP_FIGHT_KIB (\d+)U", module)
    kib = sum(r["kib"] for r in ev["list"])
    check("SFXP_FIGHT_KIB", m is not None and int(m.group(1)) == kib, (m and m.group(1), kib))
    check("kib = ceil(bytes/1024) + files", all(
        z["kib"] == -(-z["bytes"] // 1024) + z["files"] for z in ev["sizes"].values()))
    sizes = f.floor_sizes()
    check("module comments parsed", len(sizes) == 18 + len(ev["list"]), len(sizes))


def leg_ids() -> None:
    ev = json.loads(f.EVIDENCE.read_text(encoding="utf-8"))
    check("frozen bench ids", list(ev["bench"]) == ["bench-01", "bench-02", "bench-03"], list(ev["bench"]))
    check("frozen Downpour ids", list(ev["downpour"]) == ["downpour-01", "downpour-02", "downpour-03"],
          list(ev["downpour"]))
    real = ev["sessions"]["real"]
    check("frozen session ids", [s.get("id") for s in real] == [f"log-{i:02d}" for i in range(1, len(real) + 1)]
          and all(set(s) == {"id", "sha256", "first_play"} for s in real), real[:1])
    check("frozen sha256 pins", all(re.fullmatch(r"[0-9a-f]{64}", r["sha256"]) for part in ("bench", "downpour")
                                    for r in ev[part].values()) and all(re.fullmatch(r"[0-9a-f]{64}", s["sha256"])
                                                                        for s in real))
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        body = "".join(warn(i) for i in range(1, 12))
        fight = (warn(1) + "[repro_console] MARK P2 b |\n" + warn(3) + warn(4) + "[repro_console] MARK P4 d |\n")
        dp = "Level::Init m_Stage 1, m_StageType 4 Seed 2\n" + warn(11) + warn(12)
        (root / "z").mkdir()
        (root / "z" / "real.log").write_text(body + "r", encoding="utf-8")
        (root / "a").mkdir()
        (root / "a" / "real.log").write_text(body + "q", encoding="utf-8")
        (root / "b1.txt").write_text(fight, encoding="utf-8")
        (root / "b2.txt").write_text(fight + "x", encoding="utf-8")
        (root / "d1.txt").write_text(dp, encoding="utf-8")
        names: dict = {}
        ev2 = f.extract(root, None, [root / "b2.txt", root / "b1.txt"], [root / "d1.txt"], names)
        check("extract: bench ids in argument order", list(ev2["bench"]) == ["bench-01", "bench-02"]
              and names["bench-01"].endswith("b2.txt"), names)
        check("extract: Downpour ids", list(ev2["downpour"]) == ["downpour-01"]
              and ev2["downpour"]["downpour-01"]["downpour_first_play"] == [11, 12], ev2["downpour"])
        check("extract: session ids follow the paths", [s["id"] for s in ev2["sessions"]["real"]] == ["log-01", "log-02"]
              and names["log-01"] == "a/real.log" and names["log-02"] == "z/real.log", names)
        check("extract: no path in the evidence", "real.log" not in json.dumps(ev2) and "b2.txt" not in json.dumps(ev2))
        got = f.locate(root, ev2)
        check("locate: the pinned logs in id order", got == ([root / "b2.txt", root / "b1.txt"], [root / "d1.txt"]), got)
        (root / "b1.txt").write_text(fight + "changed", encoding="utf-8")
        try:
            f.locate(root, ev2)
            check("locate refuses a missing pin", False)
        except ValueError as exc:
            check("locate names the missing id", "bench-02" in str(exc), exc)


def leg_fit() -> None:
    rows = [(fl, k, 13.5 + 1.25 * fl + 0.13 * k) for fl, k in
            ((1, 10.0), (2, 300.0), (3, 50.0), (4, 3500.0), (1, 800.0), (5, 5.0))]
    a, b, c, res = f.fit(rows)
    check("fit", abs(a - 13.5) < 1e-6 and abs(b - 1.25) < 1e-6 and abs(c - 0.13) < 1e-9
          and res < 1e-6, (a, b, c, res))


def leg_phases(art: Path) -> None:
    ev = json.loads(f.EVIDENCE.read_text(encoding="utf-8"))
    bench, _ = f.locate(art, ev)
    want = {"bench-01": (25, 244.7, {31: (2, 41.7, 55.7)}),
            "bench-02": (23, 281.8, {24: (2, 57.4, 69.7)})}
    for run, (c, ms, wins) in want.items():
        game = bench[list(ev["bench"]).index(run)]
        nt = (game.parent / "native.log").read_text(encoding="utf-8", errors="replace")
        gt = game.read_text(encoding="utf-8", errors="replace")
        p3 = [r for r in f.phases(nt, gt) if r["phase"].startswith("P3")]
        check(f"{run} P3 present", len(p3) == 1)
        if not p3:
            continue
        r = p3[0]
        check(f"{run} P3 WAV", (r["wav_c"], round(r["wav_ms"], 1)) == (c, ms),
              (r["wav_c"], r["wav_ms"]))
        got = {w: (n, round(us / 1000.0, 1), round(u / 1000.0, 1)) for w, n, us, u in r["loads"]}
        for w, v in wins.items():
            check(f"{run} sound-28 window {w}", got.get(w) == v, got)
        check(f"{run} P3 first plays include 28", 28 in r["first_play"], r["first_play"])
        rec = f.receipts(nt)
        check(f"{run} receipts", any(x.startswith("armed ") for x in rec) and
              any(x.startswith("level=1 ") for x in rec), rec[:3])


def main() -> int:
    ap = argparse.ArgumentParser()
    env = os.environ.get("BENCH_SMOKE_ROOT")
    ap.add_argument("--artifacts", type=Path, default=Path(env) if env else None,
                    help="root of the downloaded device logs (default: $BENCH_SMOKE_ROOT); without it "
                         "the phases leg is skipped")
    a = ap.parse_args()
    leg_parse()
    leg_sessions()
    leg_rule()
    leg_frozen()
    leg_ids()
    leg_fit()
    if a.artifacts:
        leg_phases(a.artifacts)
    else:
        print("phases: SKIP (no --artifacts / BENCH_SMOKE_ROOT: the device logs are not in the repository)")
    if FAILS:
        print(f"sfx_fight_list_test: FAIL ({len(FAILS)})")
        return 1
    print(f"sfx_fight_list_test: PASS (parse, sessions, rule, frozen, ids, fit"
          f"{', phases' if a.artifacts else ''})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
