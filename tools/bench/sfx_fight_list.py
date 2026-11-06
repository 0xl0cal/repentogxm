#!/usr/bin/env python3
"""Evidence for ISAAC_VITA_SFX_FIGHT_PRELOAD's sound list, and its device gate report.

The native audio manager loads every sound effect on its first Play; the game log then prints
'[warn] Sound N was not preloaded.' (format string at RVA 0x757A48, logged by SFXManager::Play
0x488de0 right before SoundEffect::Load).  One such line = one first-play load of sound N.

  python sfx_fight_list.py extract ARTIFACTS [--bench GAME.log]... [--downpour GAME.log]...
                                   [--packed DIR] [-o sfx_fight_evidence.json]
      Reads the device logs under ARTIFACTS (a directory tree of downloaded game logs) and
      freezes, with each log's sha256: the sounds first-played inside the fight phases of the
      seeded bench runs (--bench), on the hand-played Downpour floors (--downpour), and in every
      real (not scripted) session under ARTIFACTS; with --packed also each listed sound's stock
      bytes and files from sounds.xml + archives.  Without --bench / --downpour, the logs under
      ARTIFACTS whose sha256 the current evidence pins are used.  The evidence names logs by
      neutral ids only (bench-01.., downpour-01.. in argument order, log-01.. in the order of
      the sessions' paths below ARTIFACTS); extract prints which file each id is.
  python sfx_fight_list.py derive [--evidence JSON] [--packed DIR] [--c]
      Applies the selection rule to the frozen evidence and prints the list in walk order (with
      --c as the C table of host_vita_sfx_floor_preload.c); fails when it disagrees with the list
      frozen in the JSON, or (--packed) when the sizes do.
  python sfx_fight_list.py cost NATIVE.log [NATIVE.log ...] [--evidence JSON]
      Fits t_ms = a + b*files + c*KiB to the floor preload's per-sound receipts ('KAGE VITA SFX
      PRELOAD: sound=N samples=F us=T') and predicts the fight list's load time.
  python sfx_fight_list.py phases NATIVE.log GAME.log
      Per bench phase: WAV loads (ph120.sfx wav c/us/max), the worst update loop (ph120.t upd
      max), the lowest guest heap headroom (ph120.mem hr), the sounds first-played in it, and the
      preload receipts (armed / level / fight lines).

Selection rule, over ids not in the floor preload's 18:
  F  first-played between 'MARK P2' and 'MARK P4' (the fight phases) of at least one seeded
     bench run;
  D  first-played on a Downpour floor (Level::Init m_StageType 4, m_Stage 1-2) in all three
     hand-played Downpour logs, and in >= 10 % of the real sessions;
  R  first-played in >= 20 % of the real sessions.
Order: (-bench runs, -Downpour logs, -real sessions, id); tier = the first of F, D, R that holds.
Sessions follow tools' earlier survey rule: game logs holding warn lines, deduplicated by sha256,
a log that is a strict prefix of another dropped, at least 10 distinct first-play sounds;
'scripted' = it ran the repro_console mod's command list ('exec [start]' or 'MARK BENCH').
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
EVIDENCE = HERE / "sfx_fight_evidence.json"

# host_vita_sfx_floor_preload.c s_water + s_generic
FLOOR18 = (472, 473, 397, 398, 221, 8, 30, 286, 35, 36, 233, 234, 5, 55, 150, 37, 153, 258)
MIN_SOUNDS = 10
D_SHARE = 0.10
R_SHARE = 0.20
ARCHIVES = ("sfx.a", "afterbirth.a", "afterbirthp.a", "repentance.a")   # engine mount order
PREFIXES = ("resources/", "resources-dlc3/")

RE_SOUND = re.compile(r"Sound (\d+) was not preloaded")
RE_LEVEL = re.compile(r"Level::Init m_Stage (\d+), m_StageType (\d+)")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ------------------------------------------------------------------------------------ evidence
def first_plays(text: str) -> list[int]:
    seen, out = set(), []
    for m in RE_SOUND.finditer(text):
        s = int(m.group(1))
        if s not in seen:
            seen.add(s)
            out.append(s)
    return out


def bench_fight(text: str) -> list[int]:
    """Sounds first-played between MARK P2 and MARK P4 (the bench's two fight phases)."""
    if "MARK P2" not in text or "MARK P4" not in text.split("MARK P2", 1)[1]:
        raise ValueError("no MARK P2 .. MARK P4 segment")
    seg = text.split("MARK P2", 1)[1].split("MARK P4", 1)[0]
    return [int(m.group(1)) for m in RE_SOUND.finditer(seg)]


def downpour_plays(text: str) -> list[int]:
    on, out = False, []
    for line in text.splitlines():
        m = RE_LEVEL.search(line)
        if m:
            on = int(m.group(2)) == 4 and int(m.group(1)) in (1, 2)
            continue
        m = RE_SOUND.search(line)
        if m and on and int(m.group(1)) not in out:
            out.append(int(m.group(1)))
    return out


def sessions(root: Path) -> list[dict]:
    logs: dict[str, tuple[str, bytes]] = {}
    for dirpath, dirs, files in os.walk(root):
        dirs.sort()
        for f in sorted(files):
            if not (f.endswith(".log") or f.endswith(".txt")):
                continue
            p = Path(dirpath) / f
            try:
                data = p.read_bytes()
            except OSError:
                continue
            if b"was not preloaded" in data:
                logs.setdefault(sha256(data), (p.relative_to(root).as_posix(), data))
    items = sorted(logs.values(), key=lambda t: (len(t[1]), t[0]))
    out = []
    for i, (rel, data) in enumerate(items):
        if any(other.startswith(data) for _, other in items[i + 1:]):
            continue  # a strict prefix of a longer copy of the same session
        text = data.decode("utf-8", "replace")
        ids = first_plays(text)
        if len(ids) < MIN_SOUNDS:
            continue
        scripted = "[repro_console] " in text and ("exec [start]" in text or "MARK BENCH" in text)
        out.append({"path": rel, "sha256": sha256(data), "scripted": scripted,
                    "first_play": sorted(ids)})
    return sorted(out, key=lambda s: s["path"])


def locate(root: Path, ev: dict) -> tuple[list[Path], list[Path]]:
    """The bench and Downpour logs under ROOT whose sha256 the evidence pins, in its id order."""
    want = {rec["sha256"]: key for part in ("bench", "downpour") for key, rec in ev[part].items()}
    found: dict[str, Path] = {}
    for dirpath, dirs, files in os.walk(root):
        dirs.sort()
        for f in sorted(files):
            if not (f.endswith(".log") or f.endswith(".txt")):
                continue
            p = Path(dirpath) / f
            try:
                key = want.get(sha256(p.read_bytes()))
            except OSError:
                continue
            if key is not None:
                found.setdefault(key, p)
    missing = [k for k in want.values() if k not in found]
    if missing:
        raise ValueError(f"no log under {root} has the sha256 the evidence pins for {', '.join(missing)}")
    return [found[k] for k in ev["bench"]], [found[k] for k in ev["downpour"]]


def extract(root: Path, packed: Path | None, bench_logs: list[Path], downpour_logs: list[Path],
            names: dict | None = None) -> dict:
    """The evidence from the logs.  names, when given, receives {neutral id: log path}."""
    names = {} if names is None else names
    ev = {"rule": "tools/bench/sfx_fight_list.py, 'Selection rule'",
          "floor18": list(FLOOR18), "bench": {}, "downpour": {}}
    for i, path in enumerate(bench_logs, 1):
        data = Path(path).read_bytes()
        names[f"bench-{i:02d}"] = str(path)
        ev["bench"][f"bench-{i:02d}"] = {"sha256": sha256(data),
                                         "fight_first_play": bench_fight(data.decode("utf-8", "replace"))}
    for i, path in enumerate(downpour_logs, 1):
        data = Path(path).read_bytes()
        names[f"downpour-{i:02d}"] = str(path)
        ev["downpour"][f"downpour-{i:02d}"] = {"sha256": sha256(data),
                                               "downpour_first_play": downpour_plays(data.decode("utf-8", "replace"))}
    ss = sessions(root)
    real = [s for s in ss if not s["scripted"]]
    ev["sessions"] = {"real": [], "scripted": len(ss) - len(real)}
    for i, s in enumerate(real, 1):   # sessions() sorts by path: the ids follow it
        names[f"log-{i:02d}"] = s["path"]
        ev["sessions"]["real"].append({"id": f"log-{i:02d}", "sha256": s["sha256"],
                                       "first_play": s["first_play"]})
    lst = derive(ev)
    ev["list"] = [{"id": r["id"], "tier": r["tier"]} for r in lst]
    if packed is not None:
        sz = Sizes(packed)
        ev["sizes"] = {str(r["id"]): sz.of(r["id"]) for r in lst}
        for r in ev["list"]:
            r["kib"] = ev["sizes"][str(r["id"])]["kib"]
    return ev


# -------------------------------------------------------------------------------------- derive
def counts(ev: dict) -> tuple[dict, dict, dict, int]:
    bench: dict[int, int] = {}
    for rec in ev["bench"].values():
        for s in set(rec["fight_first_play"]):
            bench[s] = bench.get(s, 0) + 1
    dp: dict[int, int] = {}
    for rec in ev["downpour"].values():
        for s in set(rec["downpour_first_play"]):
            dp[s] = dp.get(s, 0) + 1
    real: dict[int, int] = {}
    for rec in ev["sessions"]["real"]:
        for s in set(rec["first_play"]):
            real[s] = real.get(s, 0) + 1
    return bench, dp, real, len(ev["sessions"]["real"])


def derive(ev: dict) -> list[dict]:
    bench, dp, real, nreal = counts(ev)
    if nreal == 0:
        raise ValueError("no real sessions in the evidence")
    floor = set(ev["floor18"])
    ndp = len(ev["downpour"])
    out = []
    for s in sorted(set(bench) | set(dp) | set(real)):
        if s in floor:
            continue
        share = real.get(s, 0) / nreal
        tier = ("F" if bench.get(s, 0) >= 1 else
                "D" if dp.get(s, 0) == ndp and share >= D_SHARE else
                "R" if share >= R_SHARE else None)
        if tier:
            out.append({"id": s, "tier": tier, "bench": bench.get(s, 0), "dp": dp.get(s, 0),
                        "real": real.get(s, 0)})
    out.sort(key=lambda r: (-r["bench"], -r["dp"], -r["real"], r["id"]))
    return out


# --------------------------------------------------------------------------------------- sizes
class Sizes:
    """Stock bytes and files per sound id: sounds.xml (resources-dlc3 overriding) and the archive
    entries, last archive in mount order winning (tools/isaac_vita_halve_sfx.py codec)."""

    def __init__(self, packed: Path) -> None:
        sys.path.insert(0, str(HERE.parent))
        import isaac_vita_halve_sfx as H  # noqa: E402
        self.H = H
        self.arcs = [H.Archive(str(packed / n)) for n in ARCHIVES]
        self.ids: dict[int, list[str]] = {}
        for ar in self.arcs:
            for prefix, xml in H.find_catalogues(ar, PREFIXES):
                H.merge_catalogue_index(self.ids, xml, override=(prefix == "resources-dlc3/"))

    def of(self, sid: int) -> dict:
        size = files = 0
        names = []
        for path in self.ids.get(sid, []):
            hit = None
            for ar in self.arcs:
                for p in PREFIXES:
                    e = ar.by_key.get(self.H.key_of(p + path))
                    if e is not None:
                        hit = e
            if hit is not None:
                size += hit.size
                files += 1
                names.append(path)
        # the module's guard bound: ceil(bytes / 1024) + 1 KiB per file (sample objects)
        return {"bytes": size, "files": files, "kib": -(-size // 1024) + files, "names": names}


def c_table(ev: dict) -> str:
    rows = []
    for r in ev["list"]:
        z = ev["sizes"][str(r["id"])]
        name = z["names"][0].rsplit("/", 1)[-1] if z["names"] else "?"
        files = f", {z['files']} files" if z["files"] != 1 else ""
        rows.append(f"    {{ {r['id']}, {z['kib']} }},  /* {r['tier']} {name}: {z['bytes']:,} B{files} */")
    total = sum(ev["sizes"][str(r["id"])]["bytes"] for r in ev["list"])
    nfiles = sum(ev["sizes"][str(r["id"])]["files"] for r in ev["list"])
    kib = sum(r["kib"] for r in ev["list"])
    return "\n".join(rows) + f"\n/* {len(rows)} sounds, {total:,} B in {nfiles} files, {kib:,} KiB */"


# ---------------------------------------------------------------------------------------- cost
RE_RECEIPT = re.compile(r"KAGE VITA SFX PRELOAD: sound=(\d+) samples=(\d+) us=(\d+)")


def solve3(a: list[list[float]], b: list[float]) -> list[float]:
    m = [row[:] + [v] for row, v in zip(a, b)]
    for i in range(3):
        p = max(range(i, 3), key=lambda r: abs(m[r][i]))
        m[i], m[p] = m[p], m[i]
        for r in range(3):
            if r != i:
                f = m[r][i] / m[i][i]
                m[r] = [x - f * y for x, y in zip(m[r], m[i])]
    return [m[i][3] / m[i][i] for i in range(3)]


def fit(rows: list[tuple[int, float, float]]) -> tuple[float, float, float, float]:
    """rows (files, KiB, ms) -> a, b, c, max |residual| of t = a + b*files + c*KiB."""
    x = [(1.0, f, k) for f, k, _ in rows]
    y = [t for _, _, t in rows]
    a = [[sum(r[i] * r[j] for r in x) for j in range(3)] for i in range(3)]
    b = [sum(r[i] * t for r, t in zip(x, y)) for i in range(3)]
    c0, c1, c2 = solve3(a, b)
    res = max(abs(t - (c0 + c1 * f + c2 * k)) for f, k, t in rows)
    return c0, c1, c2, res


# -------------------------------------------------------------------------------------- phases
def phases(native_text: str, game_text: str) -> list[dict]:
    sys.path.insert(0, str(HERE))
    import bench_run as b  # noqa: E402
    nat = b.parse_native(native_text)
    game = b.parse_game(game_text)
    sel, _at = b.phase_windows(nat, game, skip_s=0.0)
    # first-play sounds between consecutive phase marks of the game log (last occurrence)
    marks = [(m.start(), m.group(1)) for m in re.finditer(r"\[repro_console\] MARK (P\d.*?) \|", game_text)]
    last = {}
    for pos, name in marks:
        last[name] = pos
    order = sorted(last.items(), key=lambda kv: kv[1])
    plays = {}
    for i, (name, pos) in enumerate(order):
        end = order[i + 1][1] if i + 1 < len(order) else len(game_text)
        plays[name] = [int(m.group(1)) for m in RE_SOUND.finditer(game_text[pos:end])]
    out = []
    for name, wins, span in sel:
        wav_c = wav_us = wav_max = upd_max = 0
        hr_min = None
        worst = None
        loads = []      # (window, WAV files, WAV us, that window's worst update loop us)
        for w in wins:
            rec = nat["wins"].get(w, {})
            wav = b.ints(rec.get("ph120.sfx", {}).get("wav"))
            upd = b.ints(rec.get("ph120.t", {}).get("upd"))
            if wav:
                wav_c += wav[0]
                wav_us += wav[1]
                wav_max = max(wav_max, wav[2])
                if wav[0]:
                    loads.append((w, wav[0], wav[1], upd[-1] if upd else None))
            if upd and upd[-1] > upd_max:
                upd_max, worst = upd[-1], w
            hr = rec.get("ph120.mem", {}).get("hr")
            if hr is not None:
                hr_min = int(hr) if hr_min is None else min(hr_min, int(hr))
        out.append({"phase": name, "windows": f"{wins[0]}-{wins[-1]}" if wins else "-",
                    "span_ms": span // 1000, "wav_c": wav_c, "wav_ms": wav_us / 1000.0,
                    "wav_max_ms": wav_max / 1000.0, "upd_max_ms": upd_max / 1000.0,
                    "upd_max_win": worst, "hr_min_kib": None if hr_min is None else hr_min >> 10,
                    "first_play": plays.get(name, []), "loads": loads})
    return out


def receipts(native_text: str) -> list[str]:
    return [line.split("KAGE VITA SFX PRELOAD: ", 1)[1].strip()
            for line in native_text.splitlines()
            if "KAGE VITA SFX PRELOAD: " in line and " sound=" not in line]


# ---------------------------------------------------------------------------------------- main
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("extract")
    p.add_argument("artifacts", type=Path)
    p.add_argument("--bench", type=Path, action="append",
                   help="a seeded bench run's game log (repeat; default: the logs under ARTIFACTS whose "
                        "sha256 --like pins)")
    p.add_argument("--downpour", type=Path, action="append",
                   help="a hand-played Downpour game log (repeat; default as for --bench)")
    p.add_argument("--like", type=Path, default=EVIDENCE,
                   help="the evidence whose sha256 pins locate the bench and Downpour logs")
    p.add_argument("--packed", type=Path)
    p.add_argument("-o", "--out", type=Path, default=EVIDENCE)
    p = sub.add_parser("derive")
    p.add_argument("--evidence", type=Path, default=EVIDENCE)
    p.add_argument("--packed", type=Path)
    p.add_argument("--c", action="store_true")
    p = sub.add_parser("cost")
    p.add_argument("native", type=Path, nargs="+")
    p.add_argument("--evidence", type=Path, default=EVIDENCE)
    p = sub.add_parser("phases")
    p.add_argument("native", type=Path)
    p.add_argument("game", type=Path)
    a = ap.parse_args(argv)

    if a.cmd == "extract":
        bench, downpour = a.bench, a.downpour
        if bench is None or downpour is None:
            found = locate(a.artifacts, json.loads(a.like.read_text(encoding="utf-8")))
            bench = bench if bench is not None else found[0]
            downpour = downpour if downpour is not None else found[1]
        names: dict = {}
        ev = extract(a.artifacts, a.packed, bench, downpour, names)
        for key, path in names.items():
            print(f"{key:12} {path}")
        a.out.write_text(json.dumps(ev, indent=1) + "\n", encoding="utf-8", newline="\n")
        print(f"{a.out}: {len(ev['list'])} sounds; real sessions {len(ev['sessions']['real'])}, "
              f"scripted {ev['sessions']['scripted']}")
        return 0
    if a.cmd == "derive":
        ev = json.loads(a.evidence.read_text(encoding="utf-8"))
        lst = derive(ev)
        bench, dp, real, nreal = counts(ev)
        print(f"real sessions {nreal}; bench runs {len(ev['bench'])}; Downpour logs {len(ev['downpour'])}")
        print(f"{'id':>4} tier bench dp  real   kib")
        for r in lst:
            print(f"{r['id']:4d}  {r['tier']}   {r['bench']:3d} {r['dp']:3d} {r['real'] / nreal:5.0%} "
                  f"{ev.get('sizes', {}).get(str(r['id']), {}).get('kib', '?'):>5}")
        frozen = [(r["id"], r["tier"]) for r in ev["list"]]
        if [(r["id"], r["tier"]) for r in lst] != frozen:
            print("FAIL: the rule does not reproduce the frozen list")
            return 1
        if a.packed is not None:
            sz = Sizes(a.packed)
            bad = [s for s in ev["sizes"] if sz.of(int(s)) != ev["sizes"][s]]
            if bad:
                print(f"FAIL: sizes differ from the archives for {bad}")
                return 1
            print(f"sizes: {len(ev['sizes'])} ids match the archives")
        if a.c:
            print(c_table(ev))
        print(f"derive: PASS ({len(lst)} sounds, {sum(r['kib'] for r in ev['list'])} KiB)")
        return 0
    if a.cmd == "cost":
        ev = json.loads(a.evidence.read_text(encoding="utf-8"))
        rows = []
        for path in a.native:
            for m in RE_RECEIPT.finditer(path.read_text(encoding="utf-8", errors="replace")):
                rows.append(tuple(map(int, m.groups())))
        # floor-list sizes: the module's table comments (bytes, files) are the archive values
        known = floor_sizes()
        data = []
        for sid, files, us in rows:
            if sid not in known:
                continue
            nbytes, nfiles = known[sid]
            if nfiles != files:
                raise SystemExit(f"sound {sid}: receipt samples={files}, table files={nfiles}")
            data.append((files, nbytes / 1024.0, us / 1000.0))
        c0, c1, c2, res = fit(data)
        print(f"{len(data)} receipts: t_ms = {c0:.2f} + {c1:.2f}*files + {c2:.4f}*KiB "
              f"(max |residual| {res:.1f} ms)")
        tot = 0.0
        for r in ev["list"]:
            z = ev["sizes"][str(r["id"])]
            t = c0 + c1 * z["files"] + c2 * z["bytes"] / 1024.0
            tot += t
            print(f"  {r['id']:4d} {r['tier']} files {z['files']} {z['bytes'] / 1024:7.1f} KiB  model {t:5.1f} ms")
        print(f"fight list: {len(ev['list'])} sounds, model {tot:.0f} ms")
        return 0
    if a.cmd == "phases":
        nt = a.native.read_text(encoding="utf-8", errors="replace")
        gt = a.game.read_text(encoding="utf-8", errors="replace")
        for line in receipts(nt):
            print(f"receipt: {line}")
        print(f"{'phase':6s} {'windows':9s} {'wav_c':>5s} {'wav_ms':>8s} {'wmax':>6s} {'updmax':>7s} "
              f"{'@win':>4s} {'hr_min':>7s} first-play")
        for r in phases(nt, gt):
            print(f"{r['phase']:6s} {r['windows']:9s} {r['wav_c']:5d} {r['wav_ms']:8.1f} "
                  f"{r['wav_max_ms']:6.1f} {r['upd_max_ms']:7.1f} {r['upd_max_win'] or '-':>4} "
                  f"{r['hr_min_kib'] if r['hr_min_kib'] is not None else '-':>7} "
                  f"{','.join(map(str, r['first_play'])) or '-'}")
            for w, c, us, upd in r["loads"]:
                worst_ms = "-" if upd is None else f"{upd / 1000.0:.1f}"
                print(f"         win {w}: wav {c} files {us / 1000.0:.1f} ms, worst update {worst_ms} ms")
        return 0
    return 2


def floor_sizes() -> dict[int, tuple[int, int]]:
    """{id: (bytes, files)} from the comments of s_water/s_generic/s_fight in the module."""
    text = (HERE.parents[1] / "recomp" / "runtime" / "host_vita_sfx_floor_preload.c").read_text(
        encoding="utf-8")
    out = {}
    for m in re.finditer(r"\{ (\d+), (\d+) \},\s*/\*[^:]*: ([\d,]+) B(?:, (\d+) files)? \*/", text):
        out[int(m.group(1))] = (int(m.group(3).replace(",", "")), int(m.group(4) or 1))
    return out


if __name__ == "__main__":
    raise SystemExit(main())
