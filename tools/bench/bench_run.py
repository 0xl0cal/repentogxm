#!/usr/bin/env python3
"""Input-free Isaac Vita benchmark, host half.  Never talks to the console.

The device steps are run by hand with a device-control tool that can list, download, upload and
delete files over FTP, deploy an eboot, and launch and kill the app (one step at a time, plain
sleeps, no polling, no input).  procedure.txt names each step after that tool's operation
(vita_list, vita_download, vita_deploy_eboot, vita_launch, vita_kill, ...).  This script makes the
files to upload, writes those steps for each run and reads what was downloaded:

  python bench_run.py prepare --out RUN [--tag B] [--eboot EBOOT] [--download-root DIR]
                              [--installed-sha256 HEX --installed-bytes N]
                                                       save2.dat (+nonce), main.lua, launch-args.txt,
                                                       manifest.json and procedure.txt in RUN
  python bench_run.py killrecord --run RUN --audit AUDIT.jsonl
                                                       RUN/kill.json from the device-control tool's
                                                       audit log (JSON lines), after vita_kill
                                                       (NO_HARNESS_KILL otherwise); bound to this
                                                       run's own audited RUN/save2.dat upload
  python bench_run.py analyze --manifest RUN/manifest.json --native RUN/native.log --game RUN/game.log
                              [--json RUN/result.json] [--eid-heap-pin KB:BYTES]
                                                       diagnosis + per-phase table (the pin override
                                                       is for a lane that changes Lua memory)
  python bench_run.py compare A.json B.json            same rooms? + per-phase deltas of two analyze
                                                       --json results
  python bench_run.py s5gates --off RUN --off RUN (--observe RUN | --on RUN --on RUN)
                                                       S5 run-comparison gates R1.6 / R8.5 / R8.8 /
                                                       R8.9 (the floor-debt mode); RUN = a run dir
                                                       or an analyze --json result
  python bench_run.py listing --before RUN/before --after RUN/after
                                                       File 1/2 listing check -> restore actions
  python bench_run.py verify --before RUN/before --after RUN/after
                                                       backup files by sha256 (after a restore)
  python bench_run.py selftest                         bench_run_test.py

Every number the analyzer prints is read from the two logs (ph120.* records, roomlog markers,
the game's log.txt); it derives, it never estimates.
"""
from __future__ import annotations

import argparse
import functools
import hashlib
import json
import math
import os
import re
import statistics
import sys
import time
import uuid
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Device paths.
TITLE = "ISAACR001"
ROOT = "ux0:/data/isaacr001"
SAVE_DIR = ROOT + "/Documents/My Games/Binding of Isaac Repentance"
GAME_LOG = SAVE_DIR + "/log.txt"
NATIVE_LOG = ROOT + "/first-arm-fault.log"
ARGS_FILE = ROOT + "/launch-args.txt"
MOD_MAIN = ROOT + "/mods/repro_console/main.lua"
MOD_DATA = ROOT + "/data/repro_console"
EID_DATA = ROOT + "/data/836319872"
EBOOT = f"ux0:/app/{TITLE}/eboot.bin"
APP_DIR = f"ux0:/app/{TITLE}"
SLOT = 2  # File 2; File 1 is the player's own save and is never written
# The eboot installed before the run is downloaded first and deployed back at the end.  prepare
# --installed-sha256 / --installed-bytes (no default) record what it is expected to be; the
# procedure then names those values in its download and restore steps.  prepare --download-root
# (no default) is the directory the device-control tool may download into; prepare warns when RUN
# is outside it.
# Native log size 60 s after launch: healthy runs 176-230 KB, stalled ones <= 24 KB (smokes).
HEALTHY_NATIVE_BYTES_AT_60S = 100_000

# Downloaded before the run (local name -> remote path), compared and restored after it.
BACKUPS = {
    "gamestate2.dat": SAVE_DIR + "/gamestate2.dat",
    "persistentgamedata2.dat": SAVE_DIR + "/persistentgamedata2.dat",
    "eid-save2.dat": EID_DATA + "/save2.dat",
    "repro-main.lua": MOD_MAIN,
}
# Listed before and after (local json name -> remote dir).
LISTINGS = {
    "list-save.json": SAVE_DIR,
    "list-eid.json": EID_DATA,
    "list-repro.json": MOD_DATA,
    "list-root.json": ROOT,
}
# Lua scripts a bench run may run (log.txt "Running Lua Script:"); anything else is a mod that can
# act on game start (repro_tour starts a tour on every new run) and makes the run invalid.
LUA_ALLOWED = (
    "resources/scripts/enums.lua",
    "resources/scripts/main.lua",
    "ux0:data/isaacr001/mods/836319872/main.lua",
    "ux0:data/isaacr001/mods/repro_console/main.lua",
)
# The phases a run must have when its manifest names none (prepared before the dry section);
# prepare writes the prepared list's own phases into manifest.json ("phases").
PHASES_EXPECTED = ("P1", "P2", "P3", "P4", "P5")
# Dry section: P6/P7/P8 repeat P5/P2/P3 on Basement I of the same start seed,
# without water.  Information only: no decision set names them (FD_STEADY, RG_STEADY, RG_DECIDE,
# ab_gate.PHASES, the device-gate scripts' --phases P2,P3).  'DRY START' ends P5 (bench-save.txt).
DRY_PHASES = ("P6", "P7", "P8")
DRY_START = "DRY START"
# Basement I = Level::Init m_Stage 1, m_StageType 0 (StageType.STAGETYPE_ORIGINAL = 0,
# the game's resources/scripts/enums.lua:1612); its rooms' backdrop is BackdropType.BASEMENT = 1
# (enums.lua:4583; DOWNPOUR = 31 at :4613).  enums.lua sha256 dce1892f51e4e645...
DRY_STAGE = (1, 0)
DRY_BACKDROP = 1
# Run length (procedure step 'sleep'): the list before the dry section waits 2745 updates and its
# second sleep is 150 s.  Each update beyond them adds 1 / DRY_UPDATES_PER_S s, rounded up to 10 s.
# MEASURED on the 40 aligned October runs (a one-off timing script over their logs): the dry
# section's 1605 updates take 60.1-70.0 s (estimate from each run's own lead-in, P5, P2 and P3 spans
# plus one room load), i.e. 22.94 updates/s at the slowest; 22 keeps a margin.
OLD_LIST_UPDATES = 2745
OLD_SECOND_SLEEP_S = 150
DRY_UPDATES_PER_S = 22.0
EID_MAIN, REPRO_MAIN = LUA_ALLOWED[2], LUA_ALLOWED[3]
# Bench validity gate.  The first 'Lua mem usage' line after
# EID's 'Running Lua Script' line, per EID version: "11215 KB and 166 bytes" in 43 of 43 good
# October bench runs over 22 builds (bench-D4A4Pb-20261026/game.log:58).  A run whose EID
# heap differs did not load the same Lua (a partial load: a language pack missing, a module "not
# found").  analyze --eid-heap-pin KB:BYTES overrides it for a lane that changes Lua memory.
EID_HEAP_PIN = {"5.23_bc0551a": (11215, 166)}
# ISAAC_VITA_LUA_LOADFILE_RETRY event results that leave a failed load, and the
# loading-complete STATS counters that count them ('budget' results count in budget_hit).
LOADFILE_BAD_RESULTS = ("unrecovered", "changed", "observed", "budget", "isdir")
LOADFILE_BAD_COUNTERS = ("unrecovered", "changed", "observed", "budget_hit", "isdir")
# The full SCE code of a descriptor dropped across a suspend (ENODEV): 0x80010000 | errno 19.
# The loadfile receipts carry the full code (sce=), never a low-byte errno.
SCE_ENODEV = 0x80010013
# The buttons bit of an 'xinput snapshot' transition (kage_vita_input.c:509-513, buttons = the raw
# SceCtrlData buttons) that the gate counts as a system intercept.  vitasdk names it twice,
# psp2common/ctrl.h:50-51: SCE_CTRL_INTERCEPTED = 0x00010000 ("Input not available because
# intercepted by another application") and SCE_CTRL_PSBUTTON = SCE_CTRL_INTERCEPTED.  So the rule
# flags anything that takes the input away from the game (the header's "another application": any
# input-taking system overlay), not only a PS-button press.  Seen newly set at a real PS-button
# press with synthetic=0 (bench-D4A4Pc-20261026/native.log last line) and at the harness's vita_kill
# at the end of every run.  42 of 44 October runs have exactly one, R2BS1 none, Pc two (counted
# 28.10).
INTERCEPT_BIT = 0x00010000
# Positional rule: the harness's vita_kill ends the log, so its intercept is the last
# thing logged.  An intercept is timed by the first timestamped line AFTER it (the xinput
# lines carry none), or by the log's end when none follows.  Measured 28.10 on the 42 October runs
# with one intercept: a tail of at most 0.055 s (bench-A2S-20261025; the other 41 read 0.000), Pc's
# mid-run one 208.158 s.  Timed by the last timestamp at or before it (4b43fce) the same tails read up
# to 2.079 s, which was the logging gap before the line, not time after the intercept.  The
# estimator can under-read a mid-run tail by the gap after the line: post-boot gaps between
# timestamped lines reach 2.581 s on good runs (bench-A2R0-20261025 at 102.0 s) and 4.433 s on
# bench-R2BS1-20261026.  A lone intercept with more log than this after it is mid-run (the kill's
# own was not logged): FAIL.
INTERCEPT_TAIL_S = 5.0
# NO_HARNESS_KILL tolerance: |(kill start - launch end) - last native timestamp|.  The run-bound
# kill_record replayed on the device-control tool's audit log for all 43 October runs other than
# Pc (10 with a separate launch, 33 launched by the deploy) gives -0.55..+1.06 s with the kill's
# 'started' row (measured twice, independently, with the same spread).
KILL_TOLERANCE_S = 2.0


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


# ------------------------------------------------------------------------------------- parsing
RE_TS = re.compile(r"^\[(\d+\.\d+) thr 0x[0-9a-f]+\] ")
RE_BUILD = re.compile(r"\] build (\S+?); guest base=")
RE_REC = re.compile(r"\[kage-vita\] (ph120\.[a-z0-9]+|roomlog) bid=(\S*) (.*)$")
RE_ARGS = re.compile(r"ISAAC VITA LAUNCH ARGS: (.*)$")
# The scheduler banner names the S5 floor-debt mode: the R1/R8 gates below
# read the residual of the build's own mode.
RE_CAD = re.compile(r"KAGE VITA CADENCE30: bid=(\S*) .*?\bfloor-debt=(observe|on) cap=(\d+)us")
# The GL admit wait mode (ISAAC_VITA_GL_THREAD_ADMIT_WAIT, recomp/vita/README.md) comes from the same
# bid-checked scheduler banner (kage_vita_fullspeed_scheduler.c, " gl-admit-wait=on backoff=30" /
# " gl-admit-wait=observe"): the backend receipt below carries no build id, so a stale log tail could
# name the mode of another build.  The receipt is kept as aw_receipt and reported only when it disagrees.
RE_CAD_AW = re.compile(r"KAGE VITA CADENCE30: bid=(\S*) .*?\bgl-admit-wait=(observe|on)\b")
RE_AW_MODE = re.compile(r"KAGE VITA GL ADMIT WAIT: mode=(\w+)")
# GL admit behind (ISAAC_VITA_GL_THREAD_ADMIT_BEHIND): the mode and the bound from the same bid-checked
# banner (" gl-admit-behind=on bytes=65536 backoff=30" / " gl-admit-behind=observe bytes=65536
# backoff=30"); the bid-less backend receipt is kept and reported only when it disagrees.
RE_CAD_AB = re.compile(r"KAGE VITA CADENCE30: bid=(\S*) .*?\bgl-admit-behind=(observe|on) bytes=(\d+)\b")
RE_AB_MODE = re.compile(r"KAGE VITA GL ADMIT BEHIND: mode=(\w+) bytes=(\d+)")
RE_SLOTEV = re.compile(r"ISAAC VITA LAUNCH SAVE SLOT: event=(\w+) slot=(\d+) was=0x([0-9a-f]+) "
                       r"manager=0x([0-9a-f]+) (waited|restored)=(\d+) trigger=(.*)$")
RE_MODSAVE = re.compile(r"/data/[^/]+/save\d\.dat$")
RE_ENODEV = re.compile(r"errno=19\b")
RE_ROOM = re.compile(r"\] - Room (\d+)\.(\d+)\(")
RE_MARK = re.compile(r"\[repro_console\] MARK (.+?) \|")
# The whole MARK line: name | game state | entity counts (repro_console main.lua).  Values may hold
# spaces ("level=Downpour I"): a value runs to the next " key=".
RE_MARK_FULL = re.compile(r"\[repro_console\] MARK (.+?) \| (.*)$")
RE_MARK_KV = re.compile(r"(\w+)=(.*?)(?=\s+\w+=|$)")
RE_RNG = re.compile(r"RNG Start Seed: (.+?) \((\d+)\) \[(New|Continue), (\d+)\]")
RE_LOADSLOT = re.compile(r"Loading (PersistentData|GameState) (\d+)\b")
RE_LUA = re.compile(r"Running Lua Script: (\S+)")
RE_LEVEL = re.compile(r"Level::Init m_Stage (\d+), m_StageType (\d+) Seed (\d+)")
# Mod / Lua load failures and the EID load fingerprint in log.txt.  Shapes: the failing run
# bench-D4A4Pc-20261026/game.log:55-61 (EID ERROR lines, a TAB-led 'cannot read <path>: Success'
# continuation from lauxlib.c:664, the main-chunk 'ERR: ' line and RunScript's 'There was an
# error running the lua file'), the good run bench-D4A4Pb-20261026/game.log:56,58 (banner,
# heap).  Paths may hold spaces: no \S+ for a path.
RE_LUA_RUN_ERR = re.compile(r"There was an error running the lua file: (.+)$")
RE_EID_ERR = re.compile(r"Lua Debug: EID ERROR: (.*)$")
RE_LUA_FILE_ERR = re.compile(r"\bcannot (open|read|reopen) (.+): ([^:]*)$")
RE_LUA_MAIN_ERR = re.compile(r"\] - ERR: (.*)$")
RE_EID_LOADED = re.compile(r"Lua Debug: External Item Descriptions v(\S+) loaded\.")
RE_LUA_MEM = re.compile(r"Lua mem usage: (\d+) KB and (\d+) bytes")
# native.log: ISAAC_VITA_LUA_LOADFILE_RETRY receipts (why= is last because the strerror
# tail holds spaces, path= may hold spaces), xinput transitions, the Lua cost profiler's windows.
RE_LF_RETRY = re.compile(r"KAGE VITA LUA LOADFILE RETRY: (.*?) path=(.*) why=(.*)$")
RE_LF_STATS = re.compile(r"KAGE VITA LUA LOADFILE STATS: (.*)$")
# ISAAC_VITA_IMAGE_PRELOAD (+ _PIN) receipts, host_vita_image_retain.c: the kind is the
# first token ('armed', 'totals', 'level=<n>', 'image', 'hit', 'miss', 'evicted-unused'; PIN also
# 'present', 'lost').  Keys are printable, values carry no spaces (names are [A-Za-z0-9_./-]).
RE_IRP = re.compile(r"KAGE VITA IMAGE PRELOAD( PIN)?: (\S+)(.*)$")
IRP_PIN_LINES_KEEP = 24  # present / miss / lost lines kept for the report (the module prints <= 64)
RE_XINPUT = re.compile(r"xinput snapshot seq=(\d+) buttons=([0-9a-fA-F]+)->([0-9a-fA-F]+)\b")
RE_LUA_COST = re.compile(r"\[isaac-lua\] cost win=(\d+) .*?\blost=(\d+)\b")
# Sampler list records (kage_vita_guest_sampler.c): ph120.hot/hotc/hoti/hotr/hotu "phase=P
# samples=N top=items", ph120.hotw "phase=rnd samples=N w=items", ph120.hotl "phase=rnd samples=N
# inv=RVA o=items", ph120.fe "phase=P e=items" (or "ids=STALE").  A record may span several lines,
# each with the full header and whole items, so a window's items are the union of its lines.
TOP_RECORDS = ("ph120.hot", "ph120.hotc", "ph120.hoti", "ph120.hotr", "ph120.hotu", "ph120.hotw",
               "ph120.hotl", "ph120.fe")
RE_TOP = re.compile(r"\bphase=(\w+) (?:samples=(\d+) )?(?:inv=[0-9a-f]+ )?(?:top|w|o|e)=(\S*)$")

# ------------------------------------------------------------------------------------- seeds
# Isaac v1.7.9b seed arithmetic, transcribed from the frozen PE (RVAs, base 0x400000):
# Seeds::String2Seed 0x53e080 (alphabet at VA 0xb5c940, xor 0x0fef7ffd, rotating checksum byte),
# Seeds::set_start_seed 0x53e220 (xorshift with the shift triple at VA 0xa08be4 = 3, 23, 25; the
# 14 stage seeds at Seeds+0x18), Level::Init 0x311e60 (0x312052..0x31209f: the floor's seed is
# stage seed [stage + 1] for stage types 4 and 5, [stage] otherwise).  Checked against six
# start-seed / Level::Init pairs of the 2026-10-25 device logs (bench_run_test.py, leg seed).
SEED_ALPHABET = "ABCDEFGHJKLMNPQRSTWXYZ01234V6789"
SEED_SHIFTS = (3, 23, 25)


def string2seed(s: str) -> int:
    """'BVPF Q3ET' -> 20261017; 0 for a malformed string or a bad checksum (as the game)."""
    if len(s) != 9 or s[4] != " " or any(c not in SEED_ALPHABET for c in s[:4] + s[5:]):
        return 0
    v = [SEED_ALPHABET.index(c) for c in s[:4] + s[5:]]
    x = 0
    for k in range(6):
        x = (x << 5) | v[k]
    x = (((x << 2) | (v[6] >> 3)) & 0xFFFFFFFF) ^ 0x0FEF7FFD
    want = ((v[6] << 5) | v[7]) & 0xFF
    c, e = 0, x
    while e:
        c = (c + (e & 0xFF)) & 0xFF
        e >>= 5
        c = ((c << 1) | (c >> 7)) & 0xFF
    return x if c == want else 0


def stage_seeds(start: int) -> list[int]:
    a, b, c = SEED_SHIFTS
    out, x = [], start
    for _ in range(14):
        x ^= x >> a
        x ^= (x << b) & 0xFFFFFFFF
        x ^= x >> c
        out.append(x)
    return out


def level_seed(start: int, stage: int, stage_type: int) -> int | None:
    """The 'Level::Init ... Seed' a run with this start seed logs for a floor (None: no such stage)."""
    i = stage + (1 if stage_type in (4, 5) else 0)
    seeds = stage_seeds(start)
    return seeds[i] if start and 0 <= i < len(seeds) else None


def kv(rest: str) -> dict:
    """'k=v k(a,b)=x,y' -> {k: 'v', k: 'x,y'}; a bare 'us(50,95,max)' header token is dropped."""
    out = {}
    for tok in rest.split():
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        out[k.split("(", 1)[0]] = v
    return out


def ints(v: str | None) -> list[int]:
    return [int(x) for x in re.split(r"[/,]", v)] if v else []


def _n(v) -> int:
    """A receipt counter ('12') as an int; 0 when absent or not a plain decimal."""
    return int(v) if isinstance(v, str) and v.isdigit() else 0


def parse_irp(irp: dict, pin: bool, head: str, rest: str, ts) -> None:
    """One 'KAGE VITA IMAGE PRELOAD[ PIN]:' line into parse_native's n["irp"]."""
    if head.startswith("level="):
        kind, rest = "level", head + rest
    else:
        kind = head
    f = dict(kv(rest), _ts=ts)
    if pin:
        if kind == "armed":
            irp["pin_armed"] = f
        elif kind == "level":
            irp["pin_levels"].append(f)
        elif kind == "totals":
            irp["pin_totals"].append(f)
        elif kind in ("present", "miss", "lost"):
            irp["pin_lines_n"] += 1
            if len(irp["pin_lines"]) < IRP_PIN_LINES_KEEP:
                irp["pin_lines"].append(f"{kind}{rest}".strip())
        return
    if kind == "armed":
        irp["armed"] = f
    elif kind == "level":
        irp["levels"].append(f)
    elif kind == "totals":
        irp["totals"].append(f)
    elif kind == "image":
        irp["images"].append(f)
    elif kind == "hit":
        irp["hit"] += 1
    elif kind == "miss":
        irp["miss"] += 1
    elif kind == "evicted-unused":
        irp["evicted_unused"] += 1


def irp_summary(irp: dict) -> dict | None:
    """The preload (+ pin) receipts of a run for the analyze JSON; None when the build has no preload."""
    if irp["armed"] is None and not irp["levels"] and not irp["totals"]:
        return None

    def last(rows: list) -> dict | None:
        # the totals are cumulative since the arm: the last report of the boot is the run's
        # (periodic every 64 offers, loading-complete, teardown -- whichever came last)
        return {k: v for k, v in rows[-1].items() if not k.startswith("_")} if rows else None

    def strip(rows: list) -> list:
        return [{k: v for k, v in r.items() if not k.startswith("_")} for r in rows]

    return {"armed": last([irp["armed"]] if irp["armed"] else []),
            "pin_armed": last([irp["pin_armed"]] if irp["pin_armed"] else []),
            "levels": strip(irp["levels"]), "pin_levels": strip(irp["pin_levels"]),
            "images": strip(irp["images"]), "totals": last(irp["totals"]),
            "pin_totals": last(irp["pin_totals"]),
            "hit_lines": irp["hit"], "miss_lines": irp["miss"],
            "evicted_unused_lines": irp["evicted_unused"],
            "pin_lines": list(irp["pin_lines"]), "pin_lines_n": irp["pin_lines_n"]}


def parse_native(text: str) -> dict:
    """Boot receipts plus ph120 windows (win -> {record: kv}) and roomlog markers."""
    n = {"build": None, "args": None, "slot_events": [], "last_ts": 0.0, "enodev": [],
         "stops": [], "wins": {}, "rooms": {}, "rl_drop": 0, "bytes": len(text.encode("utf-8", "replace")),
         "lines": 0, "fd_mode": None, "fd_cap": None, "tops": {}, "fe_stale": 0, "aw_mode": None,
         "aw_receipt": None, "ab_mode": None, "ab_bytes": None, "ab_receipt": None,
         # bench validity gate
         "lf_retry": [], "lf_stats": [], "intercepts": [], "cost_lost": [],
         # image preload (+ pin) receipts of the first build's boot(s); see parse_irp
         "irp": {"armed": None, "pin_armed": None, "levels": [], "pin_levels": [], "totals": [],
                 "pin_totals": [], "images": [], "hit": 0, "miss": 0, "evicted_unused": 0,
                 "pin_lines": [], "pin_lines_n": 0}}
    lines = text.splitlines()
    n["lines"] = len(lines)
    for line in lines:
        m = RE_BUILD.search(line)
        if m and n["build"] is None:
            n["build"] = m.group(1)
    cur_build = None  # the build of the boot the current line belongs to (the receipts carry no bid)
    bid32 = n["build"][:32] if n["build"] else None
    group_seen: dict = {}  # (win, rec, phase) -> (header, item names) of the current instance
    untimed: list = []  # intercepts waiting for the next timestamped line
    for idx, line in enumerate(lines):
        m = RE_TS.match(line)
        ts = float(m.group(1)) if m else None
        if ts is not None:
            n["last_ts"] = max(n["last_ts"], ts)
            for x in untimed:
                x["ts"] = ts
            untimed = []
        m = RE_REC.search(line)
        if m:
            rec, bid, rest = m.groups()
            if bid32 is not None and bid != bid32:
                continue  # another build's line (a cumulative log)
            f = kv(rest)
            if rec == "roomlog":
                hi, lo = f["at"].split(":")
                n["rooms"][int(f["seq"])] = ((int(hi, 16) << 32) | int(lo, 16), tuple(ints(f["pair"])))
                continue
            if rec == "ph120.rl":
                n["rl_drop"] = max(n["rl_drop"], int(f.get("drop", "0")) + int(f.get("reset_lost", "0")))
            if rec == "ph120.rop":
                # several per window (ph120.ro lines=N of them): kept by part number
                if "win" in f and f.get("part", "").isdigit():
                    n["wins"].setdefault(int(f["win"]), {}).setdefault(rec, {})[int(f["part"])] = f.get("op", "")
                continue
            if rec in TOP_RECORDS and "win" in f:
                if f.get("ids") == "STALE":
                    n["fe_stale"] += 1
                mt = RE_TOP.search(rest)
                if mt:
                    phase, _, items = mt.groups()
                    # A long record is split over lines with the same header and whole, distinct
                    # items; they union.  A line whose header differs, or that repeats an item of
                    # the group, is another print of the window (a cumulative log with the same
                    # build id): it restarts the group, the last instance wins, as in n["wins"].
                    names = [it.rpartition(":")[0] for it in filter(None, items.split(","))]
                    gkey = (int(f["win"]), rec, phase)
                    header = rest[:mt.start(3)]
                    seen = group_seen.get(gkey)
                    per_rec = n["tops"].setdefault(gkey[0], {}).setdefault(rec, {})
                    if seen is None or seen[0] != header or any(x in seen[1] for x in names):
                        seen = group_seen[gkey] = (header, set())
                        per_rec[phase] = {}
                    acc = per_rec[phase]
                    seen[1].update(names)
                    for item in filter(None, items.split(",")):
                        name, _, vals = item.rpartition(":")
                        v = ints(vals)
                        old = acc.get(name)
                        acc[name] = v if old is None else [x + y for x, y in zip(old, v)]
            if "win" in f:
                n["wins"].setdefault(int(f["win"]), {})[rec] = f
            continue
        m = RE_CAD_AW.search(line)
        if m and (bid32 is None or m.group(1) == bid32):
            n["aw_mode"] = m.group(2)
        m = RE_CAD_AB.search(line)
        if m and (bid32 is None or m.group(1) == bid32):
            n["ab_mode"], n["ab_bytes"] = m.group(2), int(m.group(3))
        m = RE_AB_MODE.search(line)
        if m:
            n["ab_receipt"] = f"{m.group(1)}/{m.group(2)}"
            continue
        m = RE_CAD.search(line)
        if m:
            if bid32 is None or m.group(1) == bid32:
                n["fd_mode"], n["fd_cap"] = m.group(2), int(m.group(3))
            continue
        m = RE_AW_MODE.search(line)
        if m:
            n["aw_receipt"] = m.group(1)
            continue
        m = RE_ARGS.search(line)
        if m and n["args"] is None:
            n["args"] = dict(kv(m.group(1)), _line=idx, _ts=ts, _raw=m.group(1))
            n["args"]["cmdline"] = m.group(1).split("cmdline=", 1)[1] if "cmdline=" in m.group(1) else ""
            continue
        m = RE_SLOTEV.search(line)
        if m:
            n["slot_events"].append({"event": m.group(1), "slot": int(m.group(2)), "was": int(m.group(3), 16),
                                     "manager": int(m.group(4), 16), m.group(5): int(m.group(6)),
                                     "trigger": m.group(7), "ts": ts, "line": idx})
            continue
        m = RE_LF_RETRY.search(line)
        if m:
            n["lf_retry"].append(dict(kv(m.group(1)), path=m.group(2), why=m.group(3).rstrip(), _ts=ts))
            continue
        m = RE_LF_STATS.search(line)
        if m:
            n["lf_stats"].append(dict(kv(m.group(1)), _ts=ts))
            continue
        m = RE_BUILD.search(line)
        if m:
            cur_build = m.group(1)
        m = RE_IRP.search(line)
        if m:
            if cur_build is None or cur_build == n["build"]:
                parse_irp(n["irp"], bool(m.group(1)), m.group(2), m.group(3), ts)
            continue
        m = RE_XINPUT.search(line)
        if m:
            old, new = int(m.group(2), 16), int(m.group(3), 16)
            if new & INTERCEPT_BIT and not old & INTERCEPT_BIT:
                # timed by the first timestamp AFTER it (the xinput lines carry none): the
                # last one before it would add the logging gap before the line to its tail
                x = {"seq": int(m.group(1)), "line": idx + 1, "ts": ts}
                n["intercepts"].append(x)
                if ts is None:
                    untimed.append(x)
            continue
        m = RE_LUA_COST.search(line)
        if m and int(m.group(2)) > 0:
            n["cost_lost"].append((int(m.group(1)), int(m.group(2)), ts))
        if RE_ENODEV.search(line):
            n["enodev"].append((ts, line.strip()[:200]))
        if "guest stop" in line or "FIRST ARM FAULT" in line or "guest fault" in line:
            n["stops"].append((ts, line.strip()[:200]))
    for x in untimed:  # no timestamped line after it: the log ends there
        x["ts"] = n["last_ts"]
    n["rooms"] = [n["rooms"][k] for k in sorted(n["rooms"])]
    return n


def parse_game(text: str) -> dict:
    g = {"pairs": [], "marks": [], "lua": [], "rng": [], "loads": [], "starts": 0, "repro": [],
         "nosave": False, "levels": [], "mark_fields": [],
         # bench validity gate: error lines as (log line, text),
         # banner versions, and the EID heap = the first 'Lua mem usage' between EID's first
         # 'Running Lua Script' line and the next 'Running Lua Script' line (or EOF)
         "lua_err": [], "eid_err": [], "lua_file_err": [], "lua_main_err": [], "eid_loaded": [],
         "eid_heap": None, "eid_runs": 0}
    eid_span = False
    for no, line in enumerate(text.splitlines(), 1):
        m = RE_LUA_RUN_ERR.search(line)
        if m:
            g["lua_err"].append((no, m.group(1).strip()))
        m = RE_EID_ERR.search(line)
        if m:
            g["eid_err"].append((no, m.group(1).strip()))
        m = RE_LUA_FILE_ERR.search(line)
        if m:
            g["lua_file_err"].append((no, f"cannot {m.group(1)} {m.group(2)}: {m.group(3).strip()}"))
        m = RE_LUA_MAIN_ERR.search(line)
        if m:
            g["lua_main_err"].append((no, m.group(1).strip()))
        m = RE_EID_LOADED.search(line)
        if m:
            g["eid_loaded"].append(m.group(1))
        if eid_span:
            m = RE_LUA_MEM.search(line)
            if m:
                g["eid_heap"] = (int(m.group(1)), int(m.group(2)))
                eid_span = False
        m = RE_MARK.search(line)
        if m:
            g["marks"].append((m.group(1), len(g["pairs"])))
            f = RE_MARK_FULL.search(line)
            g["mark_fields"].append((m.group(1), {k: v.strip() for part in (f.group(2) if f else "").split(" | ")
                                                  for k, v in RE_MARK_KV.findall(part)}))
        m = RE_LEVEL.search(line)
        if m:  # (stage, stage type, seed, repro messages before it, room lines before it)
            g["levels"].append((int(m.group(1)), int(m.group(2)), int(m.group(3)), len(g["repro"]),
                                len(g["pairs"])))
        m = RE_ROOM.search(line)
        if m:
            g["pairs"].append((int(m.group(1)), int(m.group(2))))
        m = RE_LUA.search(line)
        if m:
            g["lua"].append(m.group(1))
            if m.group(1) == EID_MAIN:
                g["eid_runs"] += 1
            eid_span = m.group(1) == EID_MAIN and g["eid_runs"] == 1
        m = RE_RNG.search(line)
        if m:
            g["rng"].append((m.group(1), m.group(3), int(m.group(4))))
        m = RE_LOADSLOT.search(line)
        if m:
            g["loads"].append((m.group(1), int(m.group(2))))
        if "[repro_console] " in line:
            msg = line.split("[repro_console] ", 1)[1].strip()
            g["repro"].append(msg)
            if msg.startswith("save data: "):
                g["starts"] += 1  # one per MC_POST_GAME_STARTED with save data, every main.lua version
            if msg.startswith("no save data"):
                g["nosave"] = True
    return g


# ------------------------------------------------------------------------------------- phases
def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def phase_windows(nat: dict, game: dict, skip_s: float = 1.0):
    """[(phase name, [window numbers], span us)] or raises ValueError with the reason."""
    w, rl = nat["wins"], nat["rooms"]
    pairs = game["pairs"]
    npairs = [p for _, p in rl]
    # The k-th game-log Room line is the k-th native roomlog marker counted from the end (the game
    # log can lose its first lines, the native log cannot); prove it before using it.
    if not pairs or not rl:
        raise ValueError(f"no room changes (game {len(pairs)}, native {len(rl)})")
    if npairs[-len(pairs):] != pairs:
        raise ValueError(f"room sequences disagree: game {len(pairs)} {pairs[-6:]} native {len(npairs)} {npairs[-6:]}")
    rl = rl[len(rl) - len(pairs):]
    marks = [(name, k) for name, k in game["marks"] if not name.startswith("NONCE")]
    last = {}
    for name, k in marks:  # the last occurrence counts (an @seed restart replays the list)
        last[name] = k
    seq = sorted(((k, name) for name, k in last.items()), key=lambda x: x[0])
    wins = sorted(k for k in w if "ph120.s" in w[k] and "ph120.t" in w[k])
    at = {k: int(w[k]["ph120.s"]["at"], 16) for k in wins}
    out = []
    for i, (k, name) in enumerate(seq):
        if not re.match(r"P\d", name):
            continue
        k2 = seq[i + 1][0] if i + 1 < len(seq) else len(rl)
        if k >= len(rl) or k2 >= len(rl):
            raise ValueError(f"phase {name}: no room change after its marker (room line {k2} of {len(rl)})")
        if k2 == k:
            out.append((name, [], 0))  # no room change of its own
            continue
        t0, t1 = rl[k][0], rl[k2][0]
        skip = 0 if "transition" in name else int(skip_s * 1e6)
        # whole windows only: the previous window ended after the phase start (+skip), this one
        # ended before the phase end
        sel = [x for j, x in enumerate(wins) if j > 0 and at[wins[j - 1]] >= t0 + skip and at[x] <= t1]
        out.append((name, sel, t1 - t0))
    return out, at


def gpu_decision(r: dict) -> str:
    """The bench's GPU-or-CPU decision rules, applied to the phase medians."""
    if r.get("gf95") is None:
        return "no ph120.gpu (build without ISAAC_VITA_GPU_FRAME_PROFILE)"
    if r["gf95"] <= 14.0:
        verdict = "GPU not the limit (gf p95 <= 14 ms): the 60-fps plan is CPU-only"
    elif 15.0 <= r["gf50"] <= 17.0:
        verdict = "GPU at the 60-fps edge (gf p50 15-17 ms): 60 needs GPU unloading"
    elif r["gf50"] > 17.0:
        verdict = "GPU-bound (gf p50 > 17 ms)"
    else:
        verdict = "GPU p95 above 14 ms with p50 below 15 ms: GPU spikes, read the worst windows"
    if r.get("rbusy_pf") is not None and r["rbusy_pf"] > 1.0:
        verdict += "; rbusy/f > 1 ms: part of rnd waits for the GPU inside GL calls"
    if r.get("q2", 0) > 0 and r.get("swpq2_pf", 0) > 2 * r.get("swpoth_pf", 0):
        verdict += "; swp(q2) >> swp(oth): the display queue holds the frame"
    return verdict


# GL recorder opcode census (ph120.ro + ph120.rop, ISAAC_VITA_GL_RECORD_OPCODE_CENSUS;
# recomp/runtime/gl_record_backend.h documents the fields).
CENSUS_FIELDS = ("n", "b", "e", "eus", "rs", "rus", "ps", "pst", "pus", "sy", "sw", "swus")
CENSUS_K = 64      # 1/64 sampling of the random replay timing and of the game-thread appends
CENSUS_TOP = 16    # opcodes printed per phase
SURFACE_INC = HERE.parents[1] / "recomp" / "runtime" / "gl_record_surface.inc"
# Opcodes the recorder appends after the generated enum (S4: REC_ATTRIBS_ENABLE /
# REC_ATTRIBS_DISABLE = REC_WORKER_CALL + 1 / + 2, ISAAC_VITA_GL_RECORD_ATTRIB_BATCH).
BACKEND_C = HERE.parents[1] / "recomp" / "runtime" / "gl_record_backend.c"


@functools.lru_cache(maxsize=None)
def opcode_names(path: Path = SURFACE_INC, backend: Path = BACKEND_C) -> dict:
    """Recorder opcode -> name from the generated enum (REC_ dropped); {} without the file.

    Opcodes the backend appends as `REC_X = REC_Y + k` (an anonymous enum in gl_record_backend.c)
    are added on top; a build without their knob never emits them, so naming them is harmless."""
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return {}
    m = re.search(r"enum IsaacGlRecordOpcode \{(.*?)\};", text, re.S)
    if not m:
        return {}
    names, value = {}, -1
    for tok in m.group(1).split(","):
        name, _, explicit = tok.strip().partition("=")
        if not name.strip():
            continue
        value = int(explicit) if explicit.strip() else value + 1
        names[value] = name.strip().removeprefix("REC_")
    try:
        extra = backend.read_text(encoding="utf-8")
    except OSError:
        return names
    by_name = {v: k for k, v in names.items()}
    for name, base, k in re.findall(r"\bREC_(\w+)\s*=\s*REC_(\w+)\s*\+\s*(\d+)\b", extra):
        if base in by_name and name not in by_name:
            names[by_name[base] + int(k)] = name
            by_name[name] = by_name[base] + int(k)
    return names


def census_window(ro: dict, parts: dict) -> dict | None:
    """slot -> the 12 fields of one window, or None when a rop part is missing or malformed."""
    if ro.get("f") != ",".join(CENSUS_FIELDS) or not ro.get("lines", "").isdigit():
        return None
    lines = int(ro["lines"])
    if sorted(parts) != list(range(lines)):
        return None
    out = {}
    for k in range(lines):
        for item in parts[k].split(";"):
            slot, _, vals = item.partition(":")
            try:
                v = ints(vals)
            except ValueError:
                return None
            if not slot.isdigit() or len(v) != len(CENSUS_FIELDS) or int(slot) in out:
                return None
            out[int(slot)] = v
    return out if str(len(out)) == ro.get("ops") else None


def census_phase(w: dict, sel: list, names: dict) -> dict | None:
    """Per-opcode figures per present over the phase's windows (None without ph120.ro).

    A window counts only when its census is complete and agrees with the same take's ph120.rt /
    ph120.rw: sum n = r (every record replayed once), sum ps = ap samples, sum sy over the
    non-closure opcodes = s(n).  rnd = render-thread replay us: the exact replays plus the random
    1/64 sample scaled to the rest (flag x: no random sample, scaled from the exact replays,
    which are each window's first 32 of the opcode); game = game-thread us per record (1/64
    sample, admission check or append start to append end) times n.  Each timed span holds one
    clock read (stamped game spans two): subtracted with the clock cost measured on that thread
    (rw ck / ap samples; ro ck16 / 16).  Flags p / r: a sampler more than 4 sigma off 1/64."""
    wins = [x for x in sel if "ph120.ro" in w[x]]
    if not wins:
        return None
    closure = next((k for k, v in names.items() if v == "WORKER_CALL"), 78)
    tot, presents, ck_us, ck_n, ck16, used, incomplete, mismatch = {}, 0, 0, 0, 0, 0, 0, 0

    def first(f, key, i=0):
        v = ints(f.get(key))
        return v[i] if len(v) > i else None

    for x in wins:
        c = census_window(w[x]["ph120.ro"], w[x].get("ph120.rop", {}))
        rt, rw = w[x].get("ph120.rt"), w[x].get("ph120.rw")
        if c is None or rt is None or rw is None:
            incomplete += 1
            continue
        if (sum(v[0] for v in c.values()) != first(rt, "r") or
                sum(v[6] for v in c.values()) != first(rw, "ap") or
                sum(v[9] for s_, v in c.items() if s_ != closure) != first(rt, "s")):
            mismatch += 1
            continue
        used += 1
        presents += first(rt, "f") or 0
        ck_us += first(rw, "ck") or 0
        ck_n += first(rw, "ap") or 0
        ck16 += int(w[x]["ph120.ro"].get("ck16", "0"))
        for slot, v in c.items():
            acc = tot.setdefault(slot, [0] * len(CENSUS_FIELDS))
            for i, val in enumerate(v):
                acc[i] += val
    out = {"windows": used, "incomplete": incomplete, "mismatch": mismatch, "presents": presents,
           "ck_game_us": ck_us / ck_n if ck_n else 0.0, "ck_render_us": ck16 / (16.0 * used) if used else 0.0,
           "render_us_pf": None, "game_us_pf": None, "ops": []}
    if not used:
        return out
    P = max(presents, 1)
    ckg, ckr = out["ck_game_us"], out["ck_render_us"]
    for slot, v in sorted(tot.items()):
        n_, b_, e, eus, rs, rus, ps, pst, pus, sy, sw, swus = v
        flags = ""
        rnd = eus - e * ckr
        if n_ > e:
            if rs:
                rnd += (n_ - e) * (rus - rs * ckr) / rs
            elif e:
                rnd = (eus - e * ckr) / e * n_
                flags += "x"
        game = None
        if ps:
            game = (pus - ckg * (ps + pst)) / ps * n_
        elif n_ and slot != closure:
            flags += "-"
        for got, pop, tag in ((ps, 0 if slot == closure else n_, "p"), (rs, n_ - e, "r")):
            if pop >= 4 * CENSUS_K:
                mu = pop / CENSUS_K
                sd = (pop * (1 / CENSUS_K) * (1 - 1 / CENSUS_K)) ** 0.5
                if abs(got - mu) > 4 * sd:
                    flags += tag
        out["ops"].append({
            "op": slot, "name": names.get(slot, f"op{slot}"), "n_pf": n_ / P, "b_pf": b_ / P,
            "render_us_pf": rnd / P, "game_us_pf": None if game is None else game / P,
            "s_pf": sy / P, "sblk_pf": sw / P, "sblk_us_pf": swus / P, "flags": flags,
            "raw": dict(zip(CENSUS_FIELDS, v))})
    out["ops"].sort(key=lambda o: -o["render_us_pf"])
    out["render_us_pf"] = sum(o["render_us_pf"] for o in out["ops"])
    out["game_us_pf"] = sum(o["game_us_pf"] or 0.0 for o in out["ops"])
    return out


def phase_row(name: str, w: dict, sel: list, span_us: int, at: dict, wins_all: list,
              cap: int | None = None, tops: dict | None = None) -> dict:
    row = {"phase": name, "span_s": span_us / 1e6, "windows": len(sel)}
    if not sel:
        return row
    prev = {x: wins_all[wins_all.index(x) - 1] for x in sel}
    dt = {x: (at[x] - at[prev[x]]) / 1e6 for x in sel}
    # prev[x] is the previous window with both ph120.s and ph120.t.  When a
    # record of window x - 1 was lost (an async-log drop), x's at-delta and stamp delta span two
    # windows while its receipts hold one; such windows are left out of every sum and identity
    # below that differences at= or floor= (gapw counts them).
    cons = {x: prev[x] == x - 1 for x in sel}
    T = {x: w[x]["ph120.t"] for x in sel}
    S = {x: w[x]["ph120.s"] for x in sel}

    def tq(key, i):
        return med([ints(T[x].get(key))[i] / 1e3 for x in sel if T[x].get(key)])

    gaps = {x: ints(T[x].get("gap")) for x in sel}
    cad = {x: ints(S[x].get("cad")) for x in sel}
    wait = {x: ints(S[x].get("wait")) for x in sel}
    row.update(
        fps=1e6 / med([gaps[x][2] for x in sel]),
        gap50=med([gaps[x][2] for x in sel]) / 1e3,
        gap95=med([gaps[x][3] for x in sel]) / 1e3,
        gap95w=max(gaps[x][3] for x in sel) / 1e3,
        gapmax=max(gaps[x][4] for x in sel) / 1e3,
        upd50=tq("upd", 0), upd95=tq("upd", 1), rnd50=tq("rnd", 0), rnd95=tq("rnd", 1),
        swp50=tq("swp", 0), svc50=tq("svc", 0),
        ups=med([cad[x][4] / dt[x] for x in sel if len(cad[x]) > 5 and dt[x] > 0]),
        rps=med([cad[x][5] / dt[x] for x in sel if len(cad[x]) > 5 and dt[x] > 0]),
        sleep_ms=med([wait[x][1] / 1e3 / dt[x] for x in sel if len(wait[x]) > 1 and dt[x] > 0]),
    )
    # Mean presents, renders and Game updates per s over the phase: sum cad p / r / g over its
    # windows / sum of their at-deltas, as the one-off rate script used on the device results did
    # (fps above is 1e6 / median gap, which picks one mode when a phase alternates between one and
    # two renders per update).  Every selected window with its at-delta to the previous window
    # (prev[x]); None when a window has no 8-field cad.
    mus = sum(at[x] - at[prev[x]] for x in sel)
    full = mus > 0 and all(len(cad[x]) > 7 for x in sel)
    row.update(pps_mean=sum(cad[x][7] for x in sel) / mus * 1e6 if full else None,
               rps_mean=sum(cad[x][5] for x in sel) / mus * 1e6 if full else None,
               ups_mean=sum(cad[x][4] for x in sel) / mus * 1e6 if full else None)
    # Update/render split from the 1 ms game-thread sampler (ph120.hs, wall ms per phase) and the
    # window's game updates (cad g) and renders (cad r): ms per update and per render.
    U, R, SW, SL, BUSY = [], [], [], [], []
    for x in sel:
        hs = w[x].get("ph120.hs")
        if not hs or len(cad[x]) < 6:
            continue
        smp = dict(zip(("svc", "upd", "rnd", "swp", "lim", "oth", "pace"), ints(hs.get("samples"))))
        g, r = cad[x][4], cad[x][5]
        total = sum(smp.values())
        sleep = wait[x][1] / 1e3 if len(wait[x]) > 1 else 0.0
        if g:
            U.append(smp["upd"] / g)
            SL.append(sleep / g)
        if r:
            R.append(smp["rnd"] / r)
            SW.append(smp["swp"] / r)
        if total:
            BUSY.append(100.0 * (total - sleep) / total)
    row.update(U=med(U), R=med(R), swp_pr=med(SW), sleep_pu=med(SL), busy_pct=med(BUSY))
    # The inputs of the run-comparison gates R1.6/R8.5/R8.9 (run_gates).  Quiet
    # windows are consecutive windows whose ph120.s has drop(t) = 0 and reset = 0: no stale wrapper
    # drop and no clock reset, the stock receipts of a load, a pause or a long stall.  Every build
    # (OFF, OBSERVE, ON) prints them, so both arms of a comparison use the same window filter.
    # pps_q / ups_q = sum p / sum at-delta and sum g / sum at-delta over the quiet windows (cad p =
    # presents, g = Game updates); gap95_q = median window present-gap p95 in ms.  viol/reset sum
    # the window receipts; emit50 = median ph120.w emit (the window's synchronous emission block on
    # the game thread) in ms.
    def flag(x, key):
        return (ints(S[x].get(key)) or [0])[0]
    Q = [x for x in sel if cons[x] and dt[x] > 0 and len(cad[x]) > 7 and not flag(x, "drop")
         and not flag(x, "reset")]
    qdt = sum(dt[x] for x in Q)
    EM = [ints(w[x]["ph120.w"].get("emit"))[0] for x in sel if w[x].get("ph120.w", {}).get("emit")]
    row.update(
        gapw=sum(1 for x in sel if not cons[x]), quiet=len(Q),
        pps_q=sum(cad[x][7] for x in Q) / qdt if Q else None,
        ups_q=sum(cad[x][4] for x in Q) / qdt if Q else None,
        gap95_q=med([gaps[x][3] for x in Q]) / 1e3 if Q else None,
        viol=sum(sum(ints(S[x].get("viol"))) for x in sel), reset=sum(flag(x, "reset") for x in sel),
        emit50=med(EM) / 1e3 if EM else None, emit_n=len(EM),
    )
    # GPU (ph120.gpu, ISAAC_VITA_GPU_FRAME_PROFILE).
    G = [w[x]["ph120.gpu"] for x in sel if "ph120.gpu" in w[x]]
    if G:
        def gv(f, k, i=0):
            v = ints(f.get(k))
            return v[i] if len(v) > i else None
        fr = [max(gv(f, "f") or 0, 1) for f in G]
        row.update(
            gpu_windows=len(G),
            gpu_pct=med([100.0 * gv(f, "busy") / gv(f, "wall") for f in G if gv(f, "wall")]),
            gpu_pf=med([gv(f, "busy") / n / 1e3 for f, n in zip(G, fr)]),
            gf50=med([gv(f, "gf", 0) / 1e3 for f in G]), gf95=med([gv(f, "gf", 1) / 1e3 for f in G]),
            gf95w=max(gv(f, "gf", 1) for f in G) / 1e3, gfmax=max(gv(f, "gf", 2) for f in G) / 1e3,
            rbusy_pf=med([gv(f, "rbusy") / n / 1e3 for f, n in zip(G, fr)]),
            rcpu_pf=med([(gv(f, "rwall") - gv(f, "rbusy")) / n / 1e3 for f, n in zip(G, fr)]),
            lag50=med([gv(f, "lag", 1) / 1e3 for f in G]),
            q2=sum(gv(f, "q", 2) or 0 for f in G),
            q2_share=med([(gv(f, "q", 2) or 0) / max(sum(ints(f.get("q"))), 1) for f in G]),
            swpq2_pf=med([gv(f, "swp", 0) / n / 1e3 for f, n in zip(G, fr)]),
            swpoth_pf=med([gv(f, "swp", 1) / n / 1e3 for f, n in zip(G, fr)]),
            pend=max(gv(f, "pend") or 0 for f in G),
        )
    row["gpu_verdict"] = gpu_decision(row)
    # GL render thread (ph120.rt / ph120.rw, ISAAC_VITA_GL_THREAD_WORKER).
    RT = [(x, w[x]["ph120.rt"]) for x in sel if "ph120.rt" in w[x]]
    if RT:
        def rv(f, k, i=0):
            v = ints(f.get(k))
            return v[i] if len(v) > i else 0
        fn = {x: max(rv(f, "f"), 1) for x, f in RT}
        row.update(
            rt_windows=len(RT),
            rt_busy_pct=med([100.0 * rv(f, "busy") / rv(f, "wall") for _, f in RT if rv(f, "wall")]),
            fr50=med([rv(f, "fr", 0) / 1e3 for _, f in RT]), fr95=med([rv(f, "fr", 1) / 1e3 for _, f in RT]),
            frmax=max(rv(f, "fr", 2) for _, f in RT) / 1e3,
            sw_pf=med([rv(f, "sw") / fn[x] / 1e3 for x, f in RT]),
            held=sum(rv(f, "h", 0) for _, f in RT), held_ms=sum(rv(f, "h", 1) for _, f in RT) / 1e3,
            sblk_pf=med([rv(f, "s", 2) / fn[x] / 1e3 for x, f in RT]),
            ringfull=sum(rv(f, "rf") for _, f in RT), hw=max(rv(f, "hw") for _, f in RT),
            rec_pf=med([rv(f, "r") / fn[x] for x, f in RT]),
        )
        RW = [(x, w[x]["ph120.rw"], f) for x, f in RT if "ph120.rw" in w[x]]
        if RW:
            def prod_us(rw, rt):
                ap_s, ap_us = rv(rw, "ap", 0), rv(rw, "ap", 1)
                mb_n, mb_s, mb_us = rv(rw, "mb", 0), rv(rw, "mb", 1), rv(rw, "mb", 2)
                ck = rv(rw, "ck")  # 0 on builds before a2c1ece (no ck field)
                recs = rv(rt, "r")
                if not ap_s or not mb_s or not recs:
                    return None
                return (ap_us - ck) / ap_s + (mb_us / mb_s) * mb_n / recs
            row.update(
                wk_pf=med([rv(rw, "wk") / fn[x] for x, rw, _ in RW]),
                mq_share=med([rv(rw, "mq") / max(rv(rw, "mb", 0), 1) for _, rw, _ in RW]),
                prod_us=med([prod_us(rw, rt) for _, rw, rt in RW]),
                refused=sum(rv(rw, "rs") for _, rw, _ in RW),
            )
        # GL admit wait (ph120.aw, ISAAC_VITA_GL_THREAD_ADMIT_WAIT=OBSERVE|ON):
        # sums over the phase's windows, and the windows
        # breaking a per-window identity: I1 rw.rs = n - ok - tt, I2 n >= ok + to + nw + bo + tt,
        # I3 fit.e <= n - ok - tt - nw + 1 (one arm carried across the window edge).
        AW = [(x, w[x]["ph120.aw"]) for x, _ in RT if "ph120.aw" in w[x]]
        if AW:
            def aw_bad(x, a):
                n, ok, tt, nw = rv(a, "n"), rv(a, "ok"), rv(a, "tt"), rv(a, "nw")
                rw = w[x].get("ph120.rw")
                i1 = rw is None or "rs" not in rw or rv(rw, "rs") == n - ok - tt
                i2 = n >= ok + rv(a, "to") + nw + rv(a, "bo") + tt
                i3 = rv(a, "fit", 0) <= n - ok - tt - nw + 1
                return not (i1 and i2 and i3)
            row.update(
                aw_windows=len(AW),
                **{f"aw_{k}": sum(rv(a, k) for _, a in AW) for k in ("n", "ok", "to", "nw", "bo", "tt")},
                aw_lt=sum(rv(a, "lt", 0) for _, a in AW),
                fit_e=sum(rv(a, "fit", 0) for _, a in AW), fit_a=sum(rv(a, "fit", 1) for _, a in AW),
                aw_ms=sum(rv(a, "us", 0) for _, a in AW) / 1e3,
                aw_max=max(rv(a, "us", 1) for _, a in AW) / 1e3,
                aw_lt_ms=sum(rv(a, "lt", 1) for _, a in AW) / 1e3,
                miss_ms=sum(rv(a, "miss", 0) for _, a in AW) / 1e3,
                # hl(c,us) = loop heads late after a Present held on an ok's closure,
                # ov = worst wake past a timed-out wait's deadline (us), hd(n,us) = those holds
                aw_hl=sum(rv(a, "hl", 0) for _, a in AW),
                aw_hl_ms=sum(rv(a, "hl", 1) for _, a in AW) / 1e3,
                aw_ov=max(rv(a, "ov", 0) for _, a in AW) / 1e3,
                aw_hd=sum(rv(a, "hd", 0) for _, a in AW),
                aw_hd_ms=sum(rv(a, "hd", 1) for _, a in AW) / 1e3,
                aw_bad=sum(1 for x, a in AW if aw_bad(x, a)),
                aw_missing=sum(1 for x, _ in RT if "ph120.rw" in w[x] and "ph120.aw" not in w[x]),
            )
        # GL admit behind (ph120.ab + ph120.abp, ISAAC_VITA_GL_THREAD_ADMIT_BEHIND=OBSERVE|ON):
        # sums over the phase's windows, and the windows breaking a
        # per-window identity: J1 n = st p + f + x, J2 rw.rs = n - ok, J3 ov + wa <= n and
        # ok <= wa, J4 (with abp) pm ok <= pm c <= n - ok + 1 (one refusal carried across the
        # window edge), wp c <= pm c, wp ok <= wp c, phl <= pm c.
        AB = [(x, w[x]["ph120.ab"], f) for x, f in RT if "ph120.ab" in w[x]]
        if AB:
            def ab_bad(x, a):
                n, ok = rv(a, "n"), rv(a, "ok")
                rw, p = w[x].get("ph120.rw"), w[x].get("ph120.abp")
                j1 = n == rv(a, "st", 0) + rv(a, "st", 1) + rv(a, "st", 2)
                j2 = rw is None or "rs" not in rw or rv(rw, "rs") == n - ok
                j3 = rv(a, "ov") + rv(a, "wa") <= n and ok <= rv(a, "wa")
                j4 = p is None or (rv(p, "pm", 1) <= rv(p, "pm", 0) <= n - ok + 1
                                   and rv(p, "wp", 0) <= rv(p, "pm", 0)
                                   and rv(p, "wp", 1) <= rv(p, "wp", 0)
                                   and rv(p, "phl") <= rv(p, "pm", 0))
                return not (j1 and j2 and j3 and j4)
            ABP = [w[x]["ph120.abp"] for x, _, _ in AB if "ph120.abp" in w[x]]
            ab_n = sum(rv(a, "n") for _, a, _ in AB)
            pm_c, wp_c = sum(rv(p, "pm", 0) for p in ABP), sum(rv(p, "wp", 0) for p in ABP)
            row.update(
                ab_windows=len(AB), ab_n=ab_n,
                ab_stp=sum(rv(a, "st", 0) for _, a, _ in AB),
                ab_stf=sum(rv(a, "st", 1) for _, a, _ in AB),
                ab_stx=sum(rv(a, "st", 2) for _, a, _ in AB),
                **{f"ab_{k}": sum(rv(a, k) for _, a, _ in AB) for k in ("ov", "wa", "ok", "bo", "tt")},
                # the share of behind gates the stock time test would pass (p + x), and the share
                # ON renders outside a back-off (wa): the device rule's two numerators
                ab_pass=(sum(rv(a, "st", 0) + rv(a, "st", 2) for _, a, _ in AB) / ab_n) if ab_n else None,
                ab_wa_share=(sum(rv(a, "wa") for _, a, _ in AB) / ab_n) if ab_n else None,
                ab_ob50=med([rv(a, "ob", 0) for _, a, _ in AB if rv(a, "n")]),
                ab_obmax=max(rv(a, "ob", 1) for _, a, _ in AB),
                ab_hl=sum(rv(a, "hl", 0) for _, a, _ in AB),
                ab_hl_ms=sum(rv(a, "hl", 1) for _, a, _ in AB) / 1e3,
                ab_hd=sum(rv(a, "hd", 0) for _, a, _ in AB),
                ab_hd_ms=sum(rv(a, "hd", 1) for _, a, _ in AB) / 1e3,
                # the render thread's own sync blocks over the same windows (ph120.rt s(n,w,us))
                ab_s_w=sum(rv(f, "s", 1) for _, _, f in AB),
                ab_s_ms=sum(rv(f, "s", 2) for _, _, f in AB) / 1e3,
                pm_c=pm_c, pm_ok=sum(rv(p, "pm", 1) for p in ABP),
                pm_ms=sum(rv(p, "pm", 2) for p in ABP) / 1e3,
                pm_max=max([rv(p, "pm", 3) for p in ABP], default=0) / 1e3,
                wp_c=wp_c, wp_ok=sum(rv(p, "wp", 1) for p in ABP),
                wp_ms=sum(rv(p, "wp", 2) for p in ABP) / 1e3,
                wp_max=max([rv(p, "wp", 3) for p in ABP], default=0) / 1e3,
                ab_phl=sum(rv(p, "phl") for p in ABP),
                ab_bad=sum(1 for x, a, _ in AB if ab_bad(x, a)),
                ab_missing=sum(1 for x, _ in RT if "ph120.rw" in w[x]
                               and ("ph120.ab" not in w[x] or "ph120.abp" not in w[x])),
            )
        # GL draw packet (ph120.dp, ISAAC_VITA_GL_DRAW_PACKET=VERIFY|ON, recomp/vita/README.md):
        # sums over the phase's windows.  dp_path_pd = ring records
        # for the draws and their staged state calls, per draw: (pk + p1 + d - f) / d (ON: the
        # lever's "records per draw" gate, <= 2; VERIFY also publishes every call as its own
        # record, so there it is not the lever's number); dp_rec_pd = every ring record per draw
        # (ph120.rt r / d over the same windows); dp_sub_pd = calls staged per draw; dp_bad =
        # windows with mm or late > 0 (VERIFY: a packet differed from the separate records, or a
        # record overtook staged calls); dp_tot = the largest tot (mm + late since boot).
        # VERIFY coverage: dp_cov = windows with |v(p) - pk| > 1.
        # The producer copies its counts (record_dp_take_produce) before the take's own
        # closure flush, which the render thread verifies before it copies its half
        # (record_dp_take_replay), so that one packet is in this window's v(p) and the next
        # window's pk: at most 1 apart per window.  dp_vsx = windows with mm = late = 0 where
        # v(s) != sub + f: every staged call (sub) and every fused draw (f) is one compared
        # record, and the take moves no staged call across windows (the same flush), so the
        # two are equal per window; a WARN, not a FAIL, because mm already proves the bytes.
        DP = [(x, w[x]["ph120.dp"], f) for x, f in RT if "ph120.dp" in w[x]]
        DPV = [p for _, p, _ in DP if p.get("mode") == "VERIFY"]
        if DP:
            dp = {k: sum(rv(p, k) for _, p, _ in DP)
                  for k in ("d", "f", "pk", "p1", "sub", "B", "mm", "late")}
            dd = dp["d"]
            row.update(
                dp_windows=len(DP),
                dp_mode=",".join(sorted({p.get("mode", "?") for _, p, _ in DP})),
                **{f"dp_{k}": v for k, v in dp.items()},
                dp_mx=max(rv(p, "mx") for _, p, _ in DP),
                **{f"dp_fr_{k}": sum(rv(p, "fr", i) for _, p, _ in DP) for i, k in enumerate("rcsf")},
                dp_vp=sum(rv(p, "v", 0) for _, p, _ in DP),
                dp_vs=sum(rv(p, "v", 1) for _, p, _ in DP),
                dp_tot=max(rv(p, "tot") for _, p, _ in DP),
                dp_path_pd=(dp["pk"] + dp["p1"] + dd - dp["f"]) / dd if dd else None,
                dp_rec_pd=sum(rv(f, "r") for _, _, f in DP) / dd if dd else None,
                dp_sub_pd=dp["sub"] / dd if dd else None,
                dp_bad=sum(1 for _, p, _ in DP if rv(p, "mm") or rv(p, "late")),
                dp_verify_windows=len(DPV),
                dp_cov=sum(1 for p in DPV if abs(rv(p, "v", 0) - rv(p, "pk")) > 1),
                dp_vsx=sum(1 for p in DPV if not rv(p, "mm") and not rv(p, "late")
                           and rv(p, "v", 1) != rv(p, "sub") + rv(p, "f")),
                dp_missing=sum(1 for x, _ in RT if "ph120.rw" in w[x] and "ph120.dp" not in w[x]),
            )
    census = census_phase(w, sel, opcode_names())
    if census is not None:
        row["census"] = census
    # Guest timeGetTime calls (ph120.a tgt) and the KAGE Clear expiry clock latch (ph120.tl,
    # ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH=OBSERVE|ON): phase sums, and per
    # render with r = cad[5] summed over the same windows.
    A = [x for x in sel if "tgt" in w[x].get("ph120.a", {}) and len(cad[x]) > 5]
    if A:
        row["tgt_pr"] = sum(int(w[x]["ph120.a"]["tgt"]) for x in A) / max(sum(cad[x][5] for x in A), 1)
    TL = [x for x in sel if "ph120.tl" in w[x] and len(cad[x]) > 5]
    if TL:
        def tl(x, k, i):
            v = ints(w[x]["ph120.tl"].get(k))
            return v[i] if len(v) > i else 0
        sums = {f"tl_{name}": sum(tl(x, k, i) for x in TL)
                for k, names in (("site", "nhfs"), ("obs", "txdw"), ("clk", ("clk",)))
                for i, name in enumerate(names)}
        renders = max(sum(cad[x][5] for x in TL), 1)
        modes = sorted({int(w[x]["ph120.tl"].get("mode", "0")) for x in TL})
        tgt_known = all("tgt" in w[x].get("ph120.a", {}) for x in TL)
        row.update(sums, tl_windows=len(TL), tl_mode=modes[0] if len(modes) == 1 else None, tl_modes=modes,
                   tl_tgt=sum(int(w[x]["ph120.a"]["tgt"]) for x in TL) if tgt_known else None,
                   tl_n_pr=sums["tl_n"] / renders, tl_h_pr=sums["tl_h"] / renders,
                   tl_clk_pr=sums["tl_clk"] / renders,
                   tl_hit_pct=100.0 * sums["tl_h"] / sums["tl_n"] if sums["tl_n"] else None)
        row["tl_verdict"] = latch_decision(row)
    # S5 floor debt (ph120.fd / ph120.fs, ISAAC_VITA_FULLSPEED_FLOOR_DEBT=OBSERVE|ON; gates in
    # floor_debt_gates).  loss = at-delta - g * 33,333.33 us: how far the window's Game floors fell
    # behind one period per update.  OBSERVE should explain it by late + stale (res_obs), ON by
    # over + stale (res_on); each up to the boundary phase between report heads and wake overshoot.
    FD = [x for x in sel if "ph120.fd" in w[x] and len(cad[x]) > 5 and dt[x] > 0]
    if FD:
        def fv(x, k, i=0, rec="ph120.fd"):
            v = ints(w[x].get(rec, {}).get(k))
            return v[i] if len(v) > i else 0
        # at-delta based values read only windows whose predecessor is x - 1
        FDc = [x for x in FD if cons[x]]
        loss = {x: dt[x] * 1e6 - cad[x][4] * 1e6 / 30.0 for x in FDc}
        late_us = sum(fv(x, "late", 1) for x in FD)
        obs = [abs(loss[x] - fv(x, "late", 1) - fv(x, "stale", 2)) / 1e3 for x in FDc]
        on = [abs(loss[x] - fv(x, "over", 1) - fv(x, "stale", 2)) / 1e3 for x in FDc]
        slack_l = sum(fv(x, "slack", 1, "ph120.fs") for x in FD)
        row.update(
            fd_windows=len(FD),
            loss_ms=med([loss[x] / 1e3 for x in FDc]),
            late_ms=med([fv(x, "late", 1) / 1e3 for x in FD]),
            res_obs=med(obs), res_obsw=max(obs, default=None), res_on=med(on), res_onw=max(on, default=None),
            kept_late=(sum(fv(x, "kept") for x in FD) / late_us) if late_us else None,
            h5=sum(fv(x, "h", 5, "ph120.fs") for x in FD), h6=sum(fv(x, "h", 6, "ph120.fs") for x in FD),
            stale=sum(fv(x, "stale") for x in FD),
            gfpos=sum(fv(x, "gf", 1) for x in FD),
            gfmax=max(fv(x, "gf", 2) for x in FD) / 1e3,
            # gf(n,pos,max,min,last) per window, the R1 reference distribution for R8
            # (R1.5/R8.7): gf_pos median/worst, gf_max median and nearest-rank
            # p99, windows whose gf_max is above 0.5/1/2/3 ms; and gfstuck, windows with the
            # stuck-gf signature -- every sample > 0 and the last above 1 ms (gf frozen across
            # late paces), which must be 0.
            gfpos50=med([fv(x, "gf", 1) for x in FD]),
            gfposw=max(fv(x, "gf", 1) for x in FD),
            gfmax50=med([fv(x, "gf", 2) / 1e3 for x in FD]),
            # nearest rank: the ceil(0.99 n)-th smallest window maximum (FD is never empty here)
            gfmax99=sorted(fv(x, "gf", 2) for x in FD)[-(-99 * len(FD) // 100) - 1] / 1e3,
            gf05=sum(1 for x in FD if fv(x, "gf", 2) > 500),
            gf1=sum(1 for x in FD if fv(x, "gf", 2) > 1000),
            gf2=sum(1 for x in FD if fv(x, "gf", 2) > 2000),
            gf3=sum(1 for x in FD if fv(x, "gf", 2) > 3000),
            gfstuck=sum(1 for x in FD if fv(x, "gf", 0) > 0 and fv(x, "gf", 1) == fv(x, "gf", 0)
                        and fv(x, "gf", 4) > 1000),
            slack_b=med([fv(x, "slack", 0, "ph120.fs") for x in FD]),
            slack_ms=(sum(fv(x, "slack", 2, "ph120.fs") for x in FD) / slack_l / 1e3)
            if slack_l else None,
            # R1.2: OBSERVE never admits by slack (l = 0 and us = 0 in every window)
            slack_l=slack_l, slack_lus=sum(fv(x, "slack", 2, "ph120.fs") for x in FD),
            pg=max(abs(fv(x, "p") - cad[x][4]) for x in FD),
        )
        # The R1.3/R8.3 identity from the floor stamps (ph120.fs floor=, the Game
        # floor the next pace waits for, in us, taken with the window's receipt).  Between two
        # stamps the floor moves only at the window's paces, so dfloor = p * 33,333.33 us + late +
        # stale (OBSERVE) or + over + stale (ON) exactly, up to 2/3 us of rounding per pace: no
        # boundary term.  res_obs/res_on above difference the report time at=, which is read at the
        # loop head before the head wait, so each window boundary carries the boundary tick's work
        # time with opposite signs in adjacent windows; they are kept as information.  Excluded
        # (fx): no stamp at either end (a build without floor stamps, or no live floor: start, reset,
        # fail-open) or p != g (a pace that rebuilt the floor without a live one: a clock fault).
        def stamp(x):
            v = w[x].get("ph120.fs", {}).get("floor")
            return int(v, 16) if v else 0

        fobs, fon, fx, fxgap = [], [], 0, 0
        for x in FD:
            f1, f0 = stamp(x), stamp(prev[x])
            if not cons[x]:  # the stamp delta spans the lost window too
                fx += 1
                fxgap += 1
                continue
            if not f1 or not f0 or f1 <= f0 or fv(x, "p") != cad[x][4]:
                fx += 1
                continue
            moved = f1 - f0 - fv(x, "p") * 1e6 / 30.0
            fobs.append(abs(moved - fv(x, "late", 1) - fv(x, "stale", 2)) / 1e3)
            fon.append(abs(moved - fv(x, "over", 1) - fv(x, "stale", 2)) / 1e3)
        stamped = [x for x in FD if stamp(x)]
        # Per contiguous run of windows, |sum loss - sum attribution| / windows: the at-based
        # residual with the boundary terms telescoped to the run's two ends (information).  A run
        # ends at a window left out of FD and at a lost record.
        runs = []
        for x in FDc:
            if runs and x == runs[-1][-1] + 1:
                runs[-1].append(x)
            else:
                runs.append([x])

        def psum(key):
            return max(abs(sum(loss[x] - fv(x, key, 1) - fv(x, "stale", 2) for x in r)) / len(r)
                       for r in runs) / 1e3 if runs else None
        # What decides R1.4 -- lateness above the cap (sum late - sum min(late,
        # cap) = sum late - sum(kept + clamp)), windows whose largest late pace is above the cap,
        # and the largest late pace.
        above = late_us - sum(fv(x, "kept") + fv(x, "clamp", 1) for x in FD)
        row.update(
            fres_obs=max(fobs) if fobs else None, fres_on=max(fon) if fon else None,
            fn=len(fobs), fx=fx, fxgap=fxgap,
            above_ms=above / 1e3, latemax=max(fv(x, "late", 2) for x in FD) / 1e3,
            capw=sum(1 for x in FD if fv(x, "late", 2) > cap) if cap else None,
            # at - floor >= -33.333 ms: the stamp comes from a pace that already ran (sanity)
            bmin=min((at[x] - stamp(x)) / 1e3 for x in stamped) if stamped else None,
            psum_obs=psum("late"), psum_on=psum("over"),
            # R1.4's numerator.  OBSERVE evaluates the grid clamp on stock's
            # trajectory, where a waited pace realigns the grid by its whole wake, so its kept is
            # not ON's; kept + clamp = sum min(late, cap), the part of the lateness the cap lets ON
            # keep.  clampn = late paces cut by the clamp (information under OBSERVE).
            capk=((sum(fv(x, "kept") for x in FD) + sum(fv(x, "clamp", 1) for x in FD)) / late_us)
            if late_us else None,
            clampn=sum(fv(x, "clamp") for x in FD),
            # R8.2: UPS = sum g / sum at-delta (the whole phase, not a median of windows), over
            # the windows whose predecessor record is present
            ups_sum=sum(cad[x][4] for x in FDc) / sum(dt[x] for x in FDc) if FDc else None,
        )
    # Render residual (ISAAC_VITA_GUEST_SAMPLER_RESIDUAL ph120.hotw/hotl/hotu/hcl and
    # ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS ph120.fe).
    if tops:
        res = residual(tops, w, sel, cad)
        if res:
            row["residual"] = res
    return row


def latch_decision(r: dict) -> str:
    """The clock-latch gates on one phase's ph120.tl sums: the tripwires s and w, the counting
    identities n = h + f and clk = tgt (OBSERVE) / clk = tgt - h (ON), then what ON does or would do.
    OBSERVE d = 0 means ON on this run's clock would have taken the same expiry decisions."""
    if not r.get("tl_windows"):
        return ""
    msgs = []
    if r["tl_s"] or r["tl_w"]:
        msgs.append(f"TRIPWIRE s={r['tl_s']} w={r['tl_w']} (both must be 0): keep the latch OFF")
    if r["tl_n"] != r["tl_h"] + r["tl_f"]:
        msgs.append(f"PROBE n != h + f ({r['tl_n']} != {r['tl_h']} + {r['tl_f']})")
    mode, tgt = r.get("tl_mode"), r.get("tl_tgt")
    if mode is None:
        return "; ".join(msgs + [f"PROBE mixed latch modes {r.get('tl_modes')} in one phase"])
    want = None if tgt is None else (tgt if mode == 1 else tgt - r["tl_h"])
    if want is not None and r["tl_clk"] != want:
        msgs.append(f"PROBE clk {r['tl_clk']} != {'tgt' if mode == 1 else 'tgt - h'} {want}")
    hits = (f"h={r['tl_h']} of n={r['tl_n']} site calls" +
            (f" ({r['tl_hit_pct']:.1f}%)" if r.get("tl_hit_pct") is not None else "") +
            f", {r['tl_h_pr']:.1f}/render")
    if mode == 1:
        if r["tl_d"]:
            msgs.append(f"OBSERVE: would-be hits {hits}; d={r['tl_d']}: ON would move {r['tl_d']} expiry "
                        f"decision(s) by one Clear (heap only; needs a maintainer's sign-off)")
        else:
            msgs.append(f"OBSERVE: would-be hits {hits}; d=0: ON would take the same expiry decisions")
    elif mode == 2:
        if r["tl_t"] or r["tl_d"]:
            msgs.append(f"PROBE t={r['tl_t']} d={r['tl_d']} in ON (always 0 there)")
        msgs.append(f"ON: latch hits {hits}; clk {r['tl_clk_pr']:.1f}/render")
    else:
        msgs.append(f"PROBE latch mode {mode}")
    return "; ".join(msgs)


# |dfloor - p * T - attribution| per window, the limit of R1.3/R8.3.  Rounding is
# 2/3 us per pace (60 per window) plus 1 us per stamp: 0.04 ms; the limit leaves room for 150.
FD_FRES_LIMIT_MS = 0.1
# A stamp can be at most one Game period after its report time (the floor comes from a pace that
# already ran): at - floor >= -33.333 ms.
FD_BMIN_MS = -33.334
FD_CAPK_MIN = 0.9
FD_UPS_MIN = 29.9
FD_STEADY = ("P1", "P2", "P3", "P5")


def steady_windows(w: dict, sel: list) -> list:
    """The windows of a phase without a stale-cut pace (ph120.fd stale = 0): no pace a Game tick or
    more late and no 60-Hz grid jump, so no load, pause, Continue or long stall.  These are the
    windows the floor debt can act on ('P3-steady')."""
    return [x for x in sel if "ph120.fd" in w[x] and (ints(w[x]["ph120.fd"].get("stale")) or [0])[0] == 0]


def floor_debt_gates(rows: list, steady: list, mode: str | None, cap: int | None = None) -> list[str]:
    """The single-run R1 (OBSERVE) / R8 (ON) floor-debt gate lines,
    computed from the phase rows ('steady' = rows over steady_windows, where a phase has stale
    windows).  The gates that compare runs (R1.6, R8.5, R8.8, R8.9) are run_gates'."""
    fd = [r for r in rows if r.get("fd_windows")]
    if not fd:
        return []
    out = []
    if mode is None:
        out.append("mode: UNKNOWN (no 'KAGE VITA CADENCE30: ... floor-debt=' banner for this build); "
                   "every line below is information (n/a), none is a verdict")

    def verdict(ok: bool) -> str:
        return ("PASS" if ok else "FAIL") if mode else "n/a"
    pick = {}
    for r in fd + list(steady):
        key = r["phase"].split()[0]
        if key in FD_STEADY + DRY_PHASES and (key not in pick or r["phase"].endswith(" steady")):
            pick[key] = r
    # the dry phases are reported (information), never part of a verdict
    info = FD_STEADY + tuple(k for k in DRY_PHASES if k in pick)
    dry = [k for k in DRY_PHASES if k in pick]
    names = ", ".join(f"{pick[k]['phase']} ({pick[k]['windows']} win)" for k in FD_STEADY if k in pick)
    fn, fx = sum(r.get("fn", 0) for r in fd), sum(r.get("fx", 0) for r in fd)
    fxgap = sum(r.get("fxgap", 0) for r in fd)
    if mode in ("observe", None):
        pg = max(r.get("pg", 0) for r in fd)
        sl, slus = sum(r.get("slack_l", 0) for r in fd), sum(r.get("slack_lus", 0) for r in fd)
        out.append(f"R1.2 decisions: pg = max |p - g| <= 1 and slack(l, us) = 0, 0 in every phase "
                   f"(OBSERVE never admits by slack): {verdict(pg <= 1 and sl == 0 and slus == 0)} -- "
                   f"pg {pg}, slack l {sl}, us {slus}")
    bmins = [r["bmin"] for r in fd if r.get("bmin") is not None]
    bmin = min(bmins) if bmins else None
    for key, gate, what in (("fres_obs", "R1.3", "late + stale"), ("fres_on", "R8.3", "over + stale")):
        vals = [r[key] for r in fd if r.get(key) is not None]
        own = mode is None or (mode == "observe") == (gate == "R1.3")
        if not own:
            continue
        head = f"{gate} identity |dfloor - p*T - ({what})| per window (floor stamps)"
        if not vals:
            out.append(f"{head}: NO DATA (no window with a floor stamp at both ends: a build without "
                       f"floor stamps?) -- {fx} windows excluded")
            continue
        ok = max(vals) <= FD_FRES_LIMIT_MS and bmin is not None and bmin >= FD_BMIN_MS
        out.append(f"{head}: {verdict(ok)} -- worst "
                   f"{max(vals):.3f} ms over {fn} windows (limit {FD_FRES_LIMIT_MS} ms), {fx} excluded "
                   f"({fx - fxgap} no stamp / p != g, {fxgap} lost predecessor); stamp sanity at - "
                   f"floor >= {bmin:.3f} ms (limit {FD_BMIN_MS} ms)")
    if mode in ("observe", None):
        # A phase without a late pace has no capk (nothing to keep): n/a, not a
        # fail.  The line also says what decides it -- lateness above the cap, the
        # windows whose largest late pace is above the cap, the largest late pace -- so one hitch
        # in an idle phase is told apart from a cap that binds throughout.
        vals = {k: pick[k].get("capk") for k in FD_STEADY if k in pick}
        have = {k: v for k, v in vals.items() if v is not None}
        ok = len(vals) == len(FD_STEADY) and all(v >= FD_CAPK_MIN for v in have.values())
        why = ", ".join(f"{k} above cap {pick[k]['above_ms']:.1f} ms in "
                        f"{'?' if pick[k].get('capw') is None else pick[k]['capw']} win "
                        f"(max late {pick[k]['latemax']:.1f} ms)" for k in have) or "no late pace in any phase"
        out.append(f"R1.4 cap: capk = sum(kept + clamp) / sum late >= {FD_CAPK_MIN} in {names}: "
                   f"{verdict(ok)} -- " + ", ".join(f"{k} {fmt3(v)}" if v is not None else
                                                    f"{k} n/a (no late pace)" for k, v in vals.items()) +
                   f"; what decides it: {why} (kept_late and clampn are information: OBSERVE's clamp is "
                   "stock's trajectory)")
        if dry:
            out.append("R1.4 dry phases (information, not part of the verdict): " + ", ".join(
                f"{k} capk {fmt3(pick[k].get('capk'))}" for k in dry))
        out.append("R1.5 gf: record only -- " + ", ".join(
            f"{k} gfmax {pick[k]['gfmax']:.2f} ms gfstuck {pick[k]['gfstuck']}" for k in info
            if k in pick))
    if mode in ("on", None):
        vals = {k: pick[k].get("ups_sum") for k in FD_STEADY if k in pick}
        ok = len(vals) == len(FD_STEADY) and all(v is not None and v >= FD_UPS_MIN for v in vals.values())
        out.append(f"R8.2 UPS = sum g / sum at-delta >= {FD_UPS_MIN} in {names}: "
                   f"{verdict(ok)} -- " + ", ".join(f"{k} {fmt3(v)}" for k, v in vals.items()))
        if dry:
            out.append("R8.2 dry phases (information, not part of the verdict): " + ", ".join(
                f"{k} {fmt3(pick[k].get('ups_sum'))}" for k in dry))
        stuck = sum(r.get("gfstuck", 0) for r in fd)
        out.append(f"R8.7 gfstuck = 0 in every phase: {verdict(stuck == 0)} -- {stuck}")
        out.append("R8 clamp cost (information, ON's own receipts): " + ", ".join(
            f"{k} kept_late {fmt3(pick[k].get('kept_late'))} clampn {pick[k].get('clampn')}"
            for k in info if k in pick))
    return out


def fmt3(v) -> str:
    return "-" if v is None else f"{v:.3f}"


# ------------------------------------------------------------------------------------- run gates
# R1.6, R8.5, R8.8 and R8.9 compare runs.  The same eboot and seed run twice
# (bench-A2S-20261025 vs bench-A2R0-20261025, MEASURED) differ by up to 0.8 presents/s in P1/P2,
# 3.8 in P5 and 1.4-6.7 ms of gap95, and their MARK lines differ in npc/projectiles: stock is not
# replay-deterministic.  A comparison of one run against one baseline with a tolerance taken from
# one other pair of runs fails an identical build with probability 1/2 (the two pairs are
# exchangeable), and with no tolerance it fails it 1/2 of the time per phase.  So:
#  - R8.5 (presents/s) and R8.9 (gap95) are a pooled two-sample t rule on an interleaved session of
#    at least two OFF and two ON runs (ABAB): FAIL when mean(ON) - mean(OFF) is worse than
#    max(t975(df) * sp * sqrt(1/n_off + 1/n_on), floor).  One-sided 2.5 % per phase; P1, P2, P3
#    decide, P5 (3 windows) is information.
#  - R1.6 (OBSERVE) gates what OBSERVE itself can cost: its own viol/reset, and its ph120.w emit
#    time against the OFF run (two more records per window).  Its decisions are stock's on the
#    host (oracle) and on the device through R1.2/R1.3; UPS, presents/s, U and R are information.
#  - R8.8a gates only the MARK fields the bench script pins (greed, difficulty, stage, stagetype,
#    level, roomtype, roomidx, frame, familiars) and says so; R8.8b prints the divergence of the
#    simulation counts (ON-OFF against OFF-OFF and ON-ON) as information for the maintainer.
# Student-t 0.975 quantiles: scipy 1.15.2 scipy.stats.t.ppf(0.975, df), df 1..20 (bench_run_test.py
# re-checks them against scipy or a numeric integration of the density).
T975 = {1: 12.7062, 2: 4.3027, 3: 3.1824, 4: 2.7764, 5: 2.5706, 6: 2.4469, 7: 2.3646, 8: 2.306,
        9: 2.2622, 10: 2.2281, 11: 2.201, 12: 2.1788, 13: 2.1604, 14: 2.1448, 15: 2.1314,
        16: 2.1199, 17: 2.1098, 18: 2.1009, 19: 2.093, 20: 2.086}
RG_STEADY = ("P1", "P2", "P3", "P5")
RG_DECIDE = ("P1", "P2", "P3")  # R8.5/R8.9 verdict phases; P5 has 3 windows (information)
# Practical floors: a loss below one present per 2-s window, or 1 ms of gap95, is not a decision
# (and a session whose runs agree by chance does not turn a 0.1/s difference into a FAIL).
RG_PRESENTS_FLOOR = 0.5
RG_GAP95_FLOOR_MS = 1.0
# R1.6b: OBSERVE adds two records (ph120.fd/fs, <= 362 + 239 B) to a window that emits ~49
# (recipe A).  ESTIMATED cost 0.1-0.3 ms per window (5.5-6.2 ms per 49 records, MEASURED A2S
# ph120.w); the same-build emit spread is <= 0.18 ms per phase (A2S vs A2R0, MEASURED).
RG_EMIT_LIMIT_MS = 1.0
SCRIPT_FIELDS = ("greed", "difficulty", "stage", "stagetype", "level", "roomtype", "roomidx", "frame",
                 "familiars")
SIM_FIELDS = ("entities", "npc", "tears", "projectiles", "effects", "lasers", "other")


def t975(df: int) -> float:
    """Two-sided 95 % (one-sided 97.5 %) Student-t quantile; beyond the table the df-20 value,
    which is larger than every quantile with more degrees of freedom (conservative)."""
    return T975[min(df, 20)]


def abab(off: list, on: list, higher_better: bool, floor: float) -> dict:
    """Pooled two-sample t rule (see above).  verdict NO DATA with fewer than two runs per arm."""
    out = {"n_off": len(off), "n_on": len(on), "delta": None, "sp": None, "df": None, "tol": None}
    if len(off) < 2 or len(on) < 2:
        return dict(out, verdict="NO DATA")
    mo, mn = sum(off) / len(off), sum(on) / len(on)
    df = len(off) + len(on) - 2
    sp = ((sum((x - mo) ** 2 for x in off) + sum((y - mn) ** 2 for y in on)) / df) ** 0.5
    tol = max(t975(df) * sp * (1 / len(off) + 1 / len(on)) ** 0.5, floor)
    delta = mn - mo
    bad = delta < -tol if higher_better else delta > tol
    return dict(out, delta=delta, sp=sp, df=df, tol=tol, verdict="FAIL" if bad else "PASS")


def _phases(r: dict) -> dict:
    return {p["phase"].split()[0]: p for p in r.get("phases", [])}


def run_gates(off: list, test: list, mode: str, expect_mode: bool = True) -> list[str]:
    """The run-comparison gates: R1.6a/b/c for mode
    'observe' (test = OBSERVE run(s)), R8.5, R8.8a/b and R8.9 for mode 'on' (test = ON runs).
    off/test are diagnose() results of seeded runs of one session (analyze --json, or run dirs)."""
    tag = mode.upper()
    out = [f"run gates: OFF x{len(off)}, {tag} x{len(test)}"]
    named = [(f"OFF{i + 1}", r) for i, r in enumerate(off)] + [(f"{tag}{i + 1}", r) for i, r in enumerate(test)]
    for name, r in named:
        out.append(f"  {name}: build {r.get('build')}, floor-debt {r.get('fd_mode') or 'off'}; "
                   f"{r.get('verdict', '')[:90]}")
    if expect_mode:
        wrong = [f"{n} is {r.get('fd_mode') or 'off'}" for n, r in named
                 if (r.get("fd_mode") or "off") != ("off" if n.startswith("OFF") else mode)]
        out.append(f"MODE OFF runs without floor debt, {tag} runs floor-debt={mode}: "
                   f"{'FAIL -- ' + ', '.join(wrong) if wrong else 'PASS'}")
    # Comparable: every run a BENCH OK with a proven seed and the first OFF run's rooms.
    why = [f"{n}: {r.get('verdict', '')[:60]}" for n, r in named if not r.get("ok")]
    base = off[0] if off else None
    for n, r in named[1:]:
        line = rooms_line(base, r)
        if not line.startswith("rooms: identical"):
            why.append(f"{n} vs OFF1 {line}")
    comparable = base is not None and not why
    if not comparable:
        out.append("NOT COMPARABLE: " + "; ".join(why or ["no OFF run"]) +
                   " -- the comparison gates below read NO DATA")
    nodata = "NO DATA (runs not comparable)"
    P = {n: _phases(r) for n, r in named}
    # the dry phases any run has are reported after RG_STEADY, as information
    rg_info = RG_STEADY + tuple(p for p in DRY_PHASES if any(p in P[n] for n in P))

    def vals(names, key, ph):
        return [P[n][ph][key] for n in names if ph in P[n] and P[n][ph].get(key) is not None]
    offn = [n for n, _ in named if n.startswith("OFF")]
    tstn = [n for n, _ in named if not n.startswith("OFF")]

    def spread(names, key, ph):
        v = vals(names, key, ph)
        return f"{max(v) - min(v):.2f}" if len(v) >= 2 else "-"

    def fmtv(names, key, ph, f="{:.2f}"):
        v = vals(names, key, ph)
        return "/".join(f.format(x) for x in v) if v else "-"

    if mode == "observe":
        for n in tstn:
            r = P[n]
            miss = [p for p in RG_STEADY if p not in r]
            bad = [f"{p} viol {r[p].get('viol')} reset {r[p].get('reset')}" for p in RG_STEADY
                   if p in r and (r[p].get("viol") or r[p].get("reset"))]
            ok_run = dict(named)[n].get("ok")
            word = "NO DATA (bench run failed)" if not ok_run else \
                ("FAIL -- " + ", ".join(bad + [f"{p} missing" for p in miss]) if bad or miss else
                 "PASS -- " + ", ".join(f"{p} viol 0 reset 0" for p in RG_STEADY))
            out.append(f"R1.6a {n} decisions: viol = 0 and reset = 0 in P1, P2, P3, P5 (the floor and "
                       f"admission decisions themselves are R1.2/R1.3): {word}")
            e_off = [dict(named)[m].get("emit50") for m in offn]
            e_off = [e for e in e_off if e is not None]
            e = dict(named)[n].get("emit50")
            if not comparable:
                word = nodata
            elif e is None or not e_off:
                word = "NO DATA (no ph120.w emit records)"
            else:
                d = e - statistics.median(e_off)
                word = (f"{'PASS' if d <= RG_EMIT_LIMIT_MS else 'FAIL'} -- {d:+.3f} ms "
                        f"({n} {e:.3f}, OFF {'/'.join(f'{x:.3f}' for x in e_off)})")
            out.append(f"R1.6b {n} cost: median ph120.w emit per window - OFF's <= {RG_EMIT_LIMIT_MS} ms "
                       f"(two more records, ESTIMATED +0.1..0.3 ms): {word}")
        out.append("R1.6c information, not a gate (one run against one baseline cannot resolve these: "
                   "the same eboot and seed twice moved P1/P5 U by up to 0.12 ms and quiet presents/s by "
                   "0.45/4.05, MEASURED A2S vs A2R0); OBSERVE vs OFF (OFF-OFF spread when two OFF runs):")
        for p in rg_info:
            out.append(f"  {p} ups_q {fmtv(tstn, 'ups_q', p, '{:.3f}')} vs {fmtv(offn, 'ups_q', p, '{:.3f}')}"
                       f" ({spread(offn, 'ups_q', p)}), presents/s {fmtv(tstn, 'pps_q', p)} vs "
                       f"{fmtv(offn, 'pps_q', p)} ({spread(offn, 'pps_q', p)}), U {fmtv(tstn, 'U', p)} vs "
                       f"{fmtv(offn, 'U', p)} ({spread(offn, 'U', p)}), R {fmtv(tstn, 'R', p)} vs "
                       f"{fmtv(offn, 'R', p)} ({spread(offn, 'R', p)}), quiet windows "
                       f"{fmtv(tstn, 'quiet', p, '{:.0f}')} vs {fmtv(offn, 'quiet', p, '{:.0f}')}")
        return out

    # mode on
    for gate, key, hb, floor, unit, what in (
            ("R8.5", "pps_q", True, RG_PRESENTS_FLOOR, "/s", "presents/s (quiet windows) not lower than OFF"),
            ("R8.9", "gap95_q", False, RG_GAP95_FLOOR_MS, " ms", "pacing gap95 (quiet windows) not worse than OFF")):
        head = (f"{gate} {what}, ABAB pooled t (one-sided 2.5 % per phase; P1, P2, P3 decide, P5 "
                f"information; floor {floor}{unit})")
        if not comparable:
            out.append(f"{head}: {nodata}")
            continue
        res = {p: abab(vals(offn, key, p), vals(tstn, key, p), hb, floor) for p in rg_info}
        parts = []
        for p in rg_info:
            g = res[p]
            if g["verdict"] == "NO DATA":
                parts.append(f"{p} - ({g['n_off']} OFF / {g['n_on']} ON runs)")
            else:
                parts.append(f"{p} {g['delta']:+.2f} (tol {g['tol']:.2f}, df {g['df']}"
                             f"{', information' if p not in RG_DECIDE else ''}{', LOSS' if g['verdict'] == 'FAIL' else ''})")
        dec = [res[p]["verdict"] for p in RG_DECIDE]
        word = ("NO DATA (needs >= 2 OFF and >= 2 ON runs of one session, interleaved: ABAB)"
                if "NO DATA" in dec else "FAIL" if "FAIL" in dec else "PASS")
        out.append(f"{head}: {word} -- mean(ON) - mean(OFF): " + ", ".join(parts))
    # R8.8a: script-pinned MARK fields, every ON run against every OFF run
    head = ("R8.8a scripted path: MARK " + "/".join(SCRIPT_FIELDS) + " equal to every OFF run at every "
            "MARK (pinned by the bench script; says nothing about the 366 timeGetTime readers)")
    marks = {n: dict(named)[n].get("marks_fields") or {} for n in offn + tstn}
    if not comparable:
        out.append(f"{head}: {nodata}")
    elif not all(marks.values()):
        out.append(f"{head}: NO DATA (a run without MARK fields)")
    else:
        diffs = []
        for t in tstn:
            for o in offn:
                for m in sorted(set(marks[t]) | set(marks[o])):
                    a, c = marks[o].get(m), marks[t].get(m)
                    if a is None or c is None:
                        diffs.append(f"{t} vs {o}: MARK {m} only in {'OFF' if c is None else 'ON'}")
                        continue
                    diffs += [f"{t} vs {o}: {m} {f} {a.get(f)} -> {c.get(f)}" for f in SCRIPT_FIELDS
                              if a.get(f) != c.get(f)]
        nm = len(marks[offn[0]])
        out.append(f"{head}: " + (f"FAIL -- {'; '.join(diffs[:6])}" if diffs else
                                  f"PASS -- {nm} MARKs x {len(SCRIPT_FIELDS)} fields x {len(tstn) * len(offn)} pairs"))
    # R8.8b: divergence of the simulation counts (information)
    if comparable and all(marks.values()):
        def div(a, c, f):
            return sum(abs(int(marks[a][m].get(f, 0)) - int(marks[c][m].get(f, 0)))
                       for m in marks[a] if m in marks[c])

        def mean_pairs(pairs, f):
            return f"{sum(div(a, c, f) for a, c in pairs) / len(pairs):.2f}" if pairs else "-"
        oo = [(a, c) for i, a in enumerate(offn) for c in offn[i + 1:]]
        tt = [(a, c) for i, a in enumerate(tstn) for c in tstn[i + 1:]]
        to = [(a, c) for a in tstn for c in offn]
        out.append("R8.8b divergence (information, not a gate: the same eboot and seed twice already differ "
                   "in npc/projectiles/entities, MEASURED A2S vs A2R0) -- sum over MARKs of |difference|, "
                   "mean over pairs, ON-OFF (OFF-OFF, ON-ON): " +
                   ", ".join(f"{f} {mean_pairs(to, f)} (OFF-OFF {mean_pairs(oo, f)}, ON-ON {mean_pairs(tt, f)})"
                             for f in SIM_FIELDS))
    out.append("R8 information (the UPS gate is R8.2 in each ON run's own analyze report): ON vs OFF")
    for p in rg_info:
        out.append(f"  {p} ups_q {fmtv(tstn, 'ups_q', p, '{:.3f}')} vs {fmtv(offn, 'ups_q', p, '{:.3f}')}, "
                   f"presents/s {fmtv(tstn, 'pps_q', p)} vs {fmtv(offn, 'pps_q', p)}, gap95_q "
                   f"{fmtv(tstn, 'gap95_q', p)} vs {fmtv(offn, 'gap95_q', p)}, quiet windows "
                   f"{fmtv(tstn, 'quiet', p, '{:.0f}')} vs {fmtv(offn, 'quiet', p, '{:.0f}')}")
    return out


def load_run(path: Path) -> dict:
    """A run for run_gates: a run directory (native.log, game.log, manifest.json; analyzed here) or
    an analyze --json result written by this version (with marks_fields and the quiet columns)."""
    if path.is_dir():
        return diagnose_dir(path)
    res = json.loads(path.read_text(encoding="utf-8"))
    if "marks_fields" not in res or any("quiet" not in p for p in res.get("phases", []) if p.get("windows")):
        raise SystemExit(f"{path}: written by an older bench_run.py (no marks_fields / quiet columns); "
                         "pass the run directory instead")
    if "eid_version" not in res:
        # a stored verdict from before the bench validity gate (the
        # 'BENCH OK' in bench-D4A4Pc-20261026/result.json) must not reach the run gates
        raise SystemExit(f"{path}: written by an older bench_run.py (no eid_version: before the bench "
                         "validity gate); pass the run directory instead")
    return res


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else None


def parse_heap_pin(text: str) -> tuple:
    """'11215:166' -> (11215, 166) (analyze / ab_gate --eid-heap-pin)."""
    kb, sep, by = text.partition(":")
    if not sep or not kb.isdigit() or not by.isdigit():
        raise argparse.ArgumentTypeError(f"--eid-heap-pin wants KB:BYTES (e.g. 11215:166), got {text!r}")
    return int(kb), int(by)


def diagnose_dir(path: Path, eid_heap_pin: tuple | None = None) -> dict:
    """A bench run directory (native.log, game.log; manifest.json and kill.json when present) ->
    diagnose(): the nonce from the manifest, NO_HARNESS_KILL from kill_record + kill.json."""
    man = read_json(path / "manifest.json") or {}
    return diagnose((path / "native.log").read_text(encoding="utf-8", errors="replace"),
                    (path / "game.log").read_text(encoding="utf-8", errors="replace"), man.get("nonce"),
                    manifest=man, kill=read_json(path / "kill.json"), eid_heap_pin=eid_heap_pin)


def residual(tops: dict, w: dict, sel: list, cad: dict) -> dict | None:
    """The render watch list per render over the phase's windows: ms per render = rnd samples
    (1 ms sampler period) / renders (cad r), calls per render = translated body entries in the rnd
    phase (ph120.fe) / renders, us per call = inclusive (or self) ms per render / calls.  Sums
    over the windows first, then divides (a window with few renders weighs little).

    With ISAAC_VITA_RENDER_CHAIN_NATIVE the --wrap replays of sub_0025af60 and sub_0055f610 do
    not run the translated body (no census entry) unless they decline, and an inlined
    sub_0055f630 never enters its body: their calls then come from ph120.rn (25af60 = f,
    55f610 = r, 55f630 = its census entries + ri).  ph120.rn counts every phase of the window,
    the census only rnd; the chain runs in the render phase.

    Only the windows that carry the records (`have`) count, renders included: a window whose
    records were dropped would otherwise deflate every per-render value.

    Roots (ph120.hotr keys: root RVA, +800000 = the sample is inside a Lua callback that root
    started, +1000000 = native sample).  Root 0 with +800000 is a sample whose walk crossed a live
    boundary and then broke before any root: it has no root, so it is no-root here, also shown
    as no_root_crossed_*; lua_crossed is root != 0 only.

    ph120.hotr is printed by every ISAAC_VITA_GUEST_SAMPLER_CHAIN build, A2 included, so the root
    figures do not wait for hotw/fe: they are computed over the windows that carry hotr
    (`have_r`, root_windows / root_renders), and the watch table over the windows that carry
    hotw/fe (`have`, windows / renders).  An A2-class log (hotr only) gets the no-root line and
    no table.  Without ISAAC_VITA_GUEST_SAMPLER_RESIDUAL (no ph120.hotw/hcl in the
    hotr windows) the walk never links a Lua boundary and hotr has no +800000 bit, so the
    crossed-first and Lua-crossed figures are None (not measured), not 0."""
    have = [x for x in sel if any(r in tops.get(x, {}) for r in ("ph120.hotw", "ph120.fe"))]
    have_r = [x for x in sel if "ph120.hotr" in tops.get(x, {})]

    def renders_of(wins):
        return sum(cad[x][5] for x in wins if x in cad and len(cad[x]) > 5)

    renders, root_renders = renders_of(have), renders_of(have_r)
    if not renders and not root_renders:
        return None

    def acc(rec, wins, phase="rnd"):
        out = {}
        for x in wins:
            for name, v in tops.get(x, {}).get(rec, {}).get(phase, {}).items():
                old = out.get(name)
                out[name] = list(v) if old is None else [a + b for a, b in zip(old, v)]
        return out

    res = {"renders": renders, "windows": len(have) if renders else 0, "root_renders": root_renders,
           "root_windows": len(have_r) if root_renders else 0, "functions": [], "lua": {},
           "chain_native": None, "no_root_t": None, "no_root_n": None, "no_root_crossed_t": None,
           "no_root_crossed_n": None, "lua_crossed": None, "hotl": {}, "hotu": []}
    if renders:
        hotw, fe = acc("ph120.hotw", have), acc("ph120.fe", have)
        rn_recs = [w[x]["ph120.rn"] for x in have if "ph120.rn" in w.get(x, {})]
        rn = {k: sum(int(f.get(k, 0)) for f in rn_recs) for k in ("f", "fd", "gn", "gc", "r", "ri", "rd")}
        calls = {name: (v or [0])[0] for name, v in fe.items()}
        if rn_recs:
            calls["25af60"], calls["55f610"] = rn["f"], rn["r"]
            calls["55f630"] = calls.get("55f630", 0) + rn["ri"]
        fns = []
        for name in sorted(set(hotw) | set(calls)):
            ms = [v / renders for v in (hotw.get(name, []) + [0] * 7)[:7]]
            cpr = calls.get(name, 0) / renders
            fns.append({"fn": name, "self": ms[0], "caller": ms[1], "incl": ms[2], "root_t": ms[3],
                        "root_n": ms[4], "lua_t": ms[5], "lua_n": ms[6], "calls": cpr,
                        "us_incl": 1e3 * ms[2] / cpr if cpr else None,
                        "us_self": 1e3 * ms[0] / cpr if cpr else None})
        fns.sort(key=lambda r: (-r["incl"], -r["calls"], r["fn"]))
        hcl = [w[x]["ph120.hcl"] for x in have if "ph120.hcl" in w.get(x, {})]
        res.update(
            functions=fns,
            lua=({k: sum(int(f.get(k, 0)) for f in hcl) / renders for k in ("lb", "lbm", "lbs", "lbx")}
                 if hcl else {}),
            chain_native={k: v / renders for k, v in rn.items()} if rn_recs else None,
            hotl={k: [x / renders for x in v] for k, v in sorted(acc("ph120.hotl", have).items())},
            hotu=sorted(((k, v[0] / renders) for k, v in acc("ph120.hotu", have).items()),
                        key=lambda t: (-t[1], t[0]))[:8])
    if root_renders:
        hotr = acc("ph120.hotr", have_r)

        def rooted(key, native, crossed):  # hotr samples of keys with this root class
            return sum((v or [0])[0] for k, v in hotr.items()
                       if bool(int(k, 16) & 0x1000000) == native and bool(int(k, 16) & 0x800000) == crossed
                       and (int(k, 16) & 0x7fffff) == key)

        rootless = {(nat, cr): rooted(0, nat, cr) for nat in (False, True) for cr in (False, True)}
        res.update(no_root_t=(rootless[(False, False)] + rootless[(False, True)]) / root_renders,
                   no_root_n=(rootless[(True, False)] + rootless[(True, True)]) / root_renders)
        if any("ph120.hotw" in tops.get(x, {}) or "ph120.hcl" in w.get(x, {}) for x in have_r):
            lua_rooted = sum((v or [0])[0] for k, v in hotr.items()
                             if int(k, 16) & 0x800000 and int(k, 16) & 0x7fffff)
            res.update(no_root_crossed_t=rootless[(False, True)] / root_renders,
                       no_root_crossed_n=rootless[(True, True)] / root_renders,
                       lua_crossed=lua_rooted / root_renders)
    return res


# ------------------------------------------------------------------------------------- analysis
def iso_time(s):
    """An audit log time ('2026-10-26T15:08:54.617396+00:00') -> datetime, None when malformed."""
    try:
        return datetime.fromisoformat(s)
    except (TypeError, ValueError):
        return None


def kill_check(kill: dict | None, last_ts: float) -> str | None:
    """NO_HARNESS_KILL for a run whose manifest has kill_record:
    None when kill.json shows an ok deploy, launch (when separate) and kill, and the native log ends
    within KILL_TOLERANCE_S of the kill; else the reason.  The native clock starts at the launch:
    the launch's end when the run launched separately (the procedure's deploy launch=false +
    vita_launch), else the deploy's end (a deploy with launch=true)."""
    if kill is None:
        return "kill.json missing: the run has no record of the harness's vita_kill"
    steps = [("deploy", kill.get("deploy")), ("kill", kill.get("kill"))]
    if kill.get("launch") is not None:
        steps.insert(1, ("launch", kill["launch"]))
    for name, step in steps:
        if not isinstance(step, dict) or step.get("status") != "ok":
            return f"kill.json {name} status is {(step or {}).get('status') if isinstance(step, dict) else None!r}, not ok"
    t0 = iso_time((kill.get("launch") or kill["deploy"]).get("end"))
    tk = iso_time(kill["kill"].get("start"))
    if t0 is None or tk is None:
        return "kill.json has no parsable launch/deploy end or kill start time"
    dt = (tk - t0).total_seconds()
    if abs(dt - last_ts) > KILL_TOLERANCE_S:
        return (f"the kill came {dt:.1f} s after the {'launch' if kill.get('launch') else 'deploy'} but the "
                f"native log ends at {last_ts:.1f} s (|difference| > {KILL_TOLERANCE_S} s): the process "
                "stopped logging before the harness killed it, or ran on after it")
    return None


def mark_seq(game: dict) -> list:
    """[(room line index, marker name, MARK fields)]: the last occurrence of every marker but NONCE,
    in phase_windows' order (an @seed restart replays the list)."""
    last = {}
    for (name, k), (_, fields) in zip(game["marks"], game["mark_fields"]):
        if not name.startswith("NONCE"):
            last[name] = (k, fields)
    return sorted(((k, name, f) for name, (k, f) in last.items()), key=lambda x: x[0])


def dry_fields(opening: dict, closing: dict | None, name: str, nxt: str | None) -> dict:
    """One dry phase's floor and water, from its MARK lines.  A MARK line
    describes the room it is logged in, i.e. the room before the room change that starts its phase,
    so the phase's own room is the one the NEXT marker (closing) describes; the floor must be Basement
    I at both.  water = Room:GetWaterAmount (absent in 1.7.9b: always '?'), haswater = Room:HasWater
    (water amount > 0.0f in the PE), backdrop = Room:GetBackdropType.  verdict: 'dry', 'not dry' or
    'unproven', with the facts behind it."""
    out = {"phase": name, "closing": nxt, "not_dry": [], "unproven": []}
    for tag, mk, f in (("opening", name, opening), ("closing", nxt, closing)):
        if f is None:
            out["unproven"].append(f"no marker after {name}: its room is not described")
            continue
        st, ty = f.get("stage", "?"), f.get("stagetype", "?")
        out[f"{tag}_stage"] = f"{st}/{ty}"
        if not (st.isdigit() and ty.isdigit()):
            out["unproven"].append(f"MARK {mk} stage={st} stagetype={ty}")
        elif (int(st), int(ty)) != DRY_STAGE:
            out["not_dry"].append(f"MARK {mk} stage={st} stagetype={ty}, not Basement I "
                                  f"({DRY_STAGE[0]}/{DRY_STAGE[1]})")
    if closing is not None:
        bd, wa, hw = (closing.get(k, "?") for k in ("backdrop", "water", "haswater"))
        out.update(backdrop=bd, water=wa, haswater=hw)
        known = False
        try:
            amount = float(wa)
        except ValueError:
            amount = None
        if amount is not None and amount == amount:  # a number, not NaN
            known = True
            if amount > 0:
                out["not_dry"].append(f"MARK {nxt} water={wa} > 0")
        if hw in ("true", "false"):
            known = True
            if hw == "true":
                out["not_dry"].append(f"MARK {nxt} haswater=true (water amount > 0)")
        if not known:
            out["unproven"].append(f"MARK {nxt} water={wa} haswater={hw}: no water field")
        if bd.isdigit():
            if int(bd) != DRY_BACKDROP:
                out["not_dry"].append(f"MARK {nxt} backdrop={bd}, not BASEMENT ({DRY_BACKDROP})")
        else:
            out["unproven"].append(f"MARK {nxt} backdrop={bd}")
    out["verdict"] = "not dry" if out["not_dry"] else "unproven" if out["unproven"] else "dry"
    return out


def diagnose(native_text: str, game_text: str, nonce: str | None = None,
             expect: tuple | None = None, *, manifest: dict | None = None,
             kill: dict | None = None, eid_heap_pin: tuple | None = None) -> dict:
    """manifest/kill: the run's manifest.json and kill.json (None: absent), read by diagnose_dir;
    eid_heap_pin: (KB, bytes) replacing EID_HEAP_PIN (analyze --eid-heap-pin).  expect: the phases
    the run must have; None = the manifest's 'phases' (prepare writes them from the prepared list),
    else PHASES_EXPECTED (a manifest from before the dry section, or none), plus DRY_PHASES when
    the log itself has 'MARK DRY START' (a run of the dry list analyzed without its manifest)."""
    listed = expect is not None or bool((manifest or {}).get("phases"))
    if expect is None:
        expect = tuple((manifest or {}).get("phases") or ()) or PHASES_EXPECTED
    nat, game = parse_native(native_text), parse_game(game_text)
    if not listed and any(n == DRY_START for n, _ in game["marks"]):
        # without this a dry-list log that lost MARK P7 read
        # 'BENCH OK: 7 phases'; logs without the marker (every run before the dry section) keep P1-P5
        expect = PHASES_EXPECTED + DRY_PHASES
    d = []  # (level, code, message)

    def add(level, code, msg):
        d.append((level, code, msg))

    # --- boot / identity
    if nat["build"] is None:
        add("WARN", "NO_BUILD_LINE", "no 'build ...;' line: records are not filtered by build id")
    a = nat["args"]
    if a is None:
        add("FAIL", "NO_LAUNCH_ARGS", "no 'ISAAC VITA LAUNCH ARGS:' receipt: eboot built without "
            "ISAAC_VITA_LAUNCH_ARGS_FILE, or the log is not this launch's")
    else:
        if a.get("status") != "ok":
            add("FAIL", "LAUNCH_ARGS_STATUS", f"launch args status={a.get('status')} (argv unchanged, "
                "normal menu boot): " + a["_raw"][:160])
        if "slot" not in a:
            add("FAIL", "PRE_SLOT_EBOOT", "receipt has no slot= field: this eboot predates "
                "--vita-save-slot (4e7dc84 or older); a --set-stage run there uses save slot 0 = "
                "File 1's mod data (EID save1.dat)")
        elif int(a["slot"]) != SLOT:
            add("FAIL", "SLOT_NOT_2", f"receipt slot={a['slot']}, the bench needs slot={SLOT}")
        if "--set-stage=" not in a.get("cmdline", ""):
            add("FAIL", "NO_SET_STAGE", "guest command line has no --set-stage=: " + a.get("cmdline", ""))
    applied = [e for e in nat["slot_events"] if e["event"] == "applied"]
    restored = [e for e in nat["slot_events"] if e["event"] == "restored"]
    if a is not None and "slot" in a and int(a["slot"]) != 0:
        if not applied:
            add("FAIL", "SLOT_NOT_APPLIED", "no 'ISAAC VITA LAUNCH SAVE SLOT: event=applied' line: the "
                "slot was never stored (Manager/Game pointer never set before the log ends)")
        else:
            e = applied[0]
            if RE_MODSAVE.search(e["trigger"].replace("\\", "/")):
                add("FAIL", "SLOT_LATE", f"slot applied at a mod save file ({e['trigger']}): that open "
                    "used the old slot")
            else:
                add("OK", "SLOT_APPLIED", f"slot {e['slot']} stored at Manager+0x10 (was {e['was']}, "
                    f"waited {e.get('waited')} fopen calls, trigger {e['trigger']}) at {e['ts']} s")
    if restored:
        n = max(e.get("restored", 0) for e in restored)
        add("FAIL", "SLOT_RESTORED", f"the game changed Manager+0x10 {n}+ time(s) after the bench "
            f"stored it (first: was={restored[0]['was']} at {restored[0]['ts']} s, trigger "
            f"{restored[0]['trigger']}); a mod save path built in between used that slot - check "
            "File 1's listing")
    for kind, s in game["loads"]:
        if s != SLOT:
            add("FAIL", "FILE1_LOAD", f"log.txt 'Loading {kind} {s}': the game loaded save file {s}")
    for seed, how, s in game["rng"]:
        if s != SLOT:
            add("FAIL", "RNG_SLOT", f"log.txt 'RNG Start Seed: {seed} [{how}, {s}]': game start on slot {s}")
        else:
            add("OK", "RNG_SLOT", f"game start on slot {s} ({how}, seed {seed})")
    extra = [p for p in game["lua"] if p not in LUA_ALLOWED]
    if extra:
        add("FAIL", "EXTRA_LUA", "unexpected Lua scripts ran (disable those mods): " + ", ".join(sorted(set(extra))))
    # --- mod / Lua load health: a run in which a mod did
    # not load, or in which the console was touched, is never BENCH OK.
    def first(errs):
        no, text = errs[0]
        more = f" (+{len(errs) - 1} more)" if len(errs) > 1 else ""
        return f"log.txt:{no}: {text[:160]}{more}"
    if game["lua_err"]:
        add("FAIL", "MOD_LUA_ERROR", "a mod's main.lua failed (RunScript 'There was an error running the lua "
            "file'), the mod is absent for the whole run: " + first(game["lua_err"]))
    if game["lua_file_err"]:
        add("FAIL", "LUA_FILE_ERROR", "a Lua file failed to load (lauxlib 'cannot open/read/reopen'): "
            + first(game["lua_file_err"]))
    if game["eid_err"]:
        add("FAIL", "EID_ERROR", "EID reported an error: " + first(game["eid_err"]))
    if game["lua_main_err"]:
        add("FAIL", "MOD_MAIN_CHUNK_ERROR", "a main chunk raised (RunScript 'ERR: '): " + first(game["lua_main_err"]))
    eid_ran = EID_MAIN in game["lua"]
    if REPRO_MAIN in game["lua"] and not eid_ran:
        add("FAIL", "NO_EID", "EID (mods/836319872) did not run: not the bench configuration")
    eid_version = game["eid_loaded"][0] if game["eid_loaded"] else None
    if eid_ran and game["eid_heap"] is None:
        add("FAIL", "EID_HEAP_MISSING", "EID's main.lua ran but no 'Lua mem usage' line follows it before "
            "the next script: EID did not finish loading")
    pin = eid_heap_pin if eid_heap_pin is not None else EID_HEAP_PIN.get(eid_version)
    recovered = 0
    lf_done = [s for s in nat["lf_stats"] if s.get("why") == "loading-complete"]
    if lf_done:
        recovered = _n(lf_done[-1].get("recovered"))
    else:
        recovered = sum(1 for e in nat["lf_retry"] if e.get("result") == "recovered")
    if eid_version is not None and game["eid_heap"] is not None:
        heap = game["eid_heap"]
        pin_src = f"--eid-heap-pin {pin[0]}:{pin[1]}" if eid_heap_pin is not None else f"pin of EID v{eid_version}"
        if pin is None:
            add("WARN", "EID_HEAP_UNPINNED", f"EID v{eid_version} is not in EID_HEAP_PIN: its heap "
                f"{heap[0]} KB and {heap[1]} bytes is not checked (pin it from a known-good run)")
        elif tuple(heap) != tuple(pin):
            msg = (f"EID's first Lua heap line reads {heap[0]} KB and {heap[1]} bytes, the {pin_src} is "
                   f"{pin[0]} KB and {pin[1]} bytes: EID loaded differently (a partial load)")
            if recovered:
                add("WARN", "EID_HEAP_MISMATCH", msg + f"; {recovered} Lua load(s) recovered by "
                    "ISAAC_VITA_LUA_LOADFILE_RETRY, whose failed attempt can move the heap")
            else:
                add("FAIL", "EID_HEAP_MISMATCH", msg)
    for e in nat["lf_retry"]:
        if e.get("result") in LOADFILE_BAD_RESULTS:
            add("FAIL", "LUA_LOADFILE_UNRECOVERED", f"KAGE VITA LUA LOADFILE RETRY n={e.get('n')} "
                f"result={e.get('result')} status={e.get('status')} sce={e.get('sce')} path={e['path']} "
                f"why={e['why']}")
            break
    else:
        for s in lf_done[-1:]:
            bad = [f"{k}={s.get(k)}" for k in LOADFILE_BAD_COUNTERS if _n(s.get(k)) > 0]
            if bad:
                add("FAIL", "LUA_LOADFILE_UNRECOVERED", "KAGE VITA LUA LOADFILE STATS at loading-complete: "
                    + " ".join(bad))
    if recovered:
        retry_us = sum(_n(e.get("retry_us")) for e in nat["lf_retry"] if e.get("result") == "recovered")
        add("WARN", "LUA_LOADFILE_RECOVERED", f"{recovered} Lua load(s) failed a read and recovered on a "
            f"retry (the run stays valid; the boot was delayed by {retry_us} us retry_us)")
    open_err = _n(lf_done[-1].get("open_err")) if lf_done else \
        sum(1 for e in nat["lf_retry"] if e.get("result") == "open-error")
    if open_err:
        add("WARN", "LUA_LOADFILE_OPEN_ERROR", f"{open_err} Lua load(s) failed to open (definitive "
            "open errors, not retried)")
    if any(s.get("why") == "first-load" for s in nat["lf_stats"]) and not lf_done:
        add("WARN", "LUA_LOADFILE_NO_REPORT", "a 'KAGE VITA LUA LOADFILE STATS: why=first-load' banner "
            "but no why=loading-complete line: the counters were never reported")
    if len(nat["intercepts"]) > 1:
        add("FAIL", "SYSTEM_INTERCEPT", f"{len(nat['intercepts'])} system intercepts (buttons bit "
            f"0x{INTERCEPT_BIT:08x} newly set) in native.log, the harness's kill accounts for one: someone or "
            "something acted on the console during the run (native.log lines "
            + ", ".join(str(x["line"]) for x in nat["intercepts"][:4]) + ")")
    elif nat["intercepts"] and nat["last_ts"] - nat["intercepts"][0]["ts"] > INTERCEPT_TAIL_S:
        x = nat["intercepts"][0]
        add("FAIL", "SYSTEM_INTERCEPT", f"a system intercept (buttons bit 0x{INTERCEPT_BIT:08x} newly set) at "
            f"native.log line {x['line']} with {nat['last_ts'] - x['ts']:.1f} s of log after it (the harness's "
            f"kill ends the log, at most {INTERCEPT_TAIL_S:g} s after its intercept): a mid-run intercept, and "
            "the kill's own was not logged")
    if manifest and manifest.get("kill_record"):
        why = kill_check(kill, nat["last_ts"])
        if why:
            add("FAIL", "NO_HARNESS_KILL", why)
    if nat["cost_lost"]:
        add("WARN", "LUA_COST_LOST", f"{len(nat['cost_lost'])} [isaac-lua] cost window(s) with lost>0 "
            f"(first: win={nat['cost_lost'][0][0]} lost={nat['cost_lost'][0][1]}): do not use that window's "
            "per-closure counts")
    if nonce:
        if not any(name == f"NONCE {nonce}" for name, _ in game["marks"]):
            add("FAIL", "NONCE", f"log.txt has no 'MARK NONCE {nonce}': not this run's game log "
                "(or the script never ran)")
    else:
        add("WARN", "NO_NONCE", "no nonce given: the game log is not proven to be this run's")
    if game["nosave"]:
        add("FAIL", "NO_SAVE_DATA", "repro_console found no save data: save2.dat missing, or the "
            "active slot was not 2 at game start")

    # --- how far the run got
    stopped = None
    if not nat["rooms"] and not nat["wins"]:
        stopped = nat["last_ts"]
        msg = (f"native log stops at {stopped:.3f} s during boot (no room, no ph120 window), "
               f"{nat['bytes']} B, no fault/stop line")
        if nat["stops"]:
            msg = f"native log stops at {stopped:.3f} s during boot after: {nat['stops'][0][1]}"
        add("FAIL", "STOPPED_IN_BOOT", msg + ": the process stopped running (suspended, put in the "
            "background, or killed); a boot that keeps running reaches the start room at ~28 s")
    lf_enodev = [e for e in nat["lf_retry"] if e.get("sce", "").lower() == f"0x{SCE_ENODEV:08x}"]
    if nat["enodev"]:
        ts, line = nat["enodev"][0]
        add("FAIL", "SUSPEND_ENODEV", f"errno 19 (ENODEV) at {ts} s: file descriptors were invalidated "
            f"= the console suspended/resumed or the app was put in the background ({line[:120]})")
    elif lf_enodev:
        e = lf_enodev[0]
        add("FAIL", "SUSPEND_ENODEV", f"a Lua file read answered sce=0x{SCE_ENODEV:08x} (ENODEV) at {e['_ts']} s "
            f"(KAGE VITA LUA LOADFILE RETRY n={e.get('n')} path={e['path']}): file descriptors were "
            "invalidated = the console suspended/resumed or the app was put in the background")
    if (EID_MAIN in game["lua"] or stopped is None) and not game["eid_loaded"]:
        add("FAIL", "EID_NOT_LOADED", "no 'External Item Descriptions v... loaded.' banner in log.txt"
            + (": EID's main.lua ran and did not finish" if EID_MAIN in game["lua"] else
               ": the run got past boot without EID"))
    for ts, line in nat["stops"][:1]:
        if stopped is None:
            add("FAIL", "GUEST_STOP", f"at {ts} s: {line}")
    if nat["rl_drop"]:
        add("WARN", "ROOMLOG_DROP", f"ph120.rl reports {nat['rl_drop']} lost room markers")
    if nat["fe_stale"]:
        add("WARN", "CENSUS_STALE", f"{nat['fe_stale']} ph120.fe windows say ids=STALE: the entry "
            "census ids were generated from another corpus, no calls/render")

    # --- seed
    # main.lua sets the seed in place ("seed X -> W: set in place ...") and the
    # next "stage" command rebuilds the floor: the first Level::Init after that line must carry W's
    # stage seed.  Older logs used a "seed W" restart ("restarting the run"), which a --set-stage
    # boot ignores (every start goes through Game::StartDebug and rolls a random seed).
    asked = [m for m in game["repro"] if "restarting the run" in m]
    active = [m for m in game["repro"] if m.startswith("seed ") and " active " in m]
    fallback = [m for m in game["repro"] if m.startswith("ERROR seed")]
    inplace = [(i, m) for i, m in enumerate(game["repro"]) if m.startswith("seed ") and " set in place " in m]
    seed_level, seed_rooms = None, None
    seed_want = None  # the start seed string the run proved (set in place, or already active)
    # 'MARK DRY START' (repro message index) opens the dry section, whose
    # "stage 1" builds the second floor of the run (Basement I)
    dry_at = [i for i, m in enumerate(game["repro"]) if m.startswith(f"MARK {DRY_START} ")
              or m == f"MARK {DRY_START}"]
    # a dry-list run that lost 'MARK DRY START' still has its dry markers; P5 would run
    # on over the stage load to MARK P6, and the seed check would take the Basement I floor
    dry_mk = [i for i, m in enumerate(game["repro"]) if m.startswith("MARK ") and m[5:].split(" ", 1)[0]
              in DRY_PHASES]
    if dry_mk and not dry_at:
        add("FAIL", "DRY_START_MISSING", f"dry phase markers ({game['repro'][dry_mk[0]][:22]}..) but no "
            f"'MARK {DRY_START}': P5 would end at the next dry marker, over the stage load")
        # the seeded floor is built before the first phase marker: cut there instead
        first_p = [i for i, m in enumerate(game["repro"]) if m.startswith("MARK P")]
        if first_p:
            dry_at = [first_p[0]]
    if asked and game["starts"] < 2 and not fallback:
        add("FAIL", "SEED_NOT_TAKEN", f"'{asked[0]}' but no second game start and no fallback: the "
            "seed restart was lost and the list stopped (smoke1: seed run from MC_POST_GAME_STARTED)")
    elif fallback:
        add("WARN", "SEED_FALLBACK", fallback[0] + " (rooms differ between runs; compare phases, not rooms)")
    elif inplace:
        i, msg = inplace[-1]
        want = msg.split(" -> ", 1)[1].split(":", 1)[0]
        after_all = [lv for lv in game["levels"] if lv[3] > i]
        # the seeded floor is built before 'MARK DRY
        # START'.  A Level::Init after it is the dry section's Basement I, which matches the start
        # seed's own (1, 0) stage seed: taking it would turn a run whose "stage 1c" built no floor
        # (P1-P5 on the random boot floor) into OK SEED.
        after = [lv for lv in after_all if not dry_at or lv[3] <= dry_at[-1]]
        if not after:
            add("WARN", "SEED_NO_LEVEL", f"'{msg[:60]}' but no Level::Init after it: no stage command "
                "rebuilt the floor, so the rooms are still the random boot seed's")
        else:
            st, ty, got, _, k = after[0]
            exp = level_seed(string2seed(want), st, ty)
            seed_level = {"want": want, "stage": st, "stage_type": ty, "seed": got, "expected": exp}
            seed_want = want
            if got == exp:
                # the seeded floor's rooms end at the next Level::Init (the dry
                # section's "stage 1"); before the dry section there was none, so nothing is cut.
                # The unfiltered list: the dry floor's Level::Init is the cut.
                k_end = after_all[1][4] if len(after_all) > 1 else len(game["pairs"])
                # the Downpour rooms end at the room change after 'MARK DRY START' (the
                # list's "@room 3", the room that ended P5 before the dry section), whatever "stage 1"
                # did: a failed "stage 1" must not append the dry section's rooms
                dry_k = [kk for n, kk in game["marks"] if n == DRY_START]
                if dry_k:
                    k_end = min(k_end, dry_k[-1] + 1)
                seed_rooms = [list(p) for p in game["pairs"][k:k_end]]  # json-shaped: compare reads both
                add("OK", "SEED", f"{want} set in place; the floor after it is its stage seed "
                    f"(Level::Init m_Stage {st}, m_StageType {ty} Seed {got}): rooms repeat across runs")
            else:
                add("WARN", "SEED_LEVEL", f"{want} set in place, but the first floor after it has "
                    f"Level::Init m_Stage {st}, m_StageType {ty} Seed {got}, not {want}'s stage seed {exp} "
                    "(rooms differ between runs; compare phases, not rooms)")
    elif active:
        add("OK", "SEED", active[-1])
        m = re.match(r"seed (.+?) active ", active[-1])
        seed_want = m.group(1) if m else None
    # "stage 1" after 'MARK DRY START' builds Basement I from the start seed's
    # stage seed (Level::Init reads _stageSeeds[stage + (type in 4,5)]).
    seed_level_dry, seed_rooms_dry = None, None
    if dry_at:
        after = [lv for lv in game["levels"] if lv[3] > dry_at[-1]]
        if not after:
            add("WARN", "SEED_DRY_NO_LEVEL", f"'MARK {DRY_START}' but no Level::Init after it: the dry "
                "section's 'stage 1' built no floor, so P6-P8 did not run on Basement I")
        else:
            st, ty, got, _, k = after[0]
            exp = level_seed(string2seed(seed_want), *DRY_STAGE) if seed_want else None
            seed_level_dry = {"want": seed_want, "stage": st, "stage_type": ty, "seed": got, "expected": exp}
            head = f"the floor after 'MARK {DRY_START}' is Level::Init m_Stage {st}, m_StageType {ty} Seed {got}"
            if (st, ty) != DRY_STAGE:
                add("WARN", "SEED_DRY_LEVEL", f"{head}, not Basement I (m_Stage {DRY_STAGE[0]}, m_StageType "
                    f"{DRY_STAGE[1]}): the dry phases are not the Basement fights")
            elif exp is None:
                add("WARN", "SEED_DRY_LEVEL", f"{head}: the run proved no start seed (see the seed line), so "
                    "the Basement floor is unchecked (dry rooms may differ between runs)")
            elif got != exp:
                add("WARN", "SEED_DRY_LEVEL", f"{head}, not {seed_want}'s Basement I stage seed {exp} (dry "
                    "rooms differ between runs; compare dry phases, not rooms)")
            else:
                k_end = after[1][4] if len(after) > 1 else len(game["pairs"])
                seed_rooms_dry = [list(p) for p in game["pairs"][k:k_end]]
                add("OK", "SEED_DRY", f"{head} = {seed_want}'s Basement I stage seed: dry rooms repeat "
                    "across runs")

    # --- markers and phases
    names = [n for n, _ in game["marks"]]
    done = any(n == "BENCH DONE" for n in names)
    if done and not nat["intercepts"]:
        # The intercept detector is blind on such a run, so SYSTEM_INTERCEPT could not have
        # fired.  The 'xinput snapshot' line is printed only while the XInput path is active and the
        # guest consumes a changed packet (kage_vita_input.c).  Every October run
        # that reached BENCH DONE logged at least one (43 of 43, Pc included; R2BS1 never got there),
        # so this WARNs on none of them.
        add("WARN", "INTERCEPT_UNSEEN", "the run reached MARK BENCH DONE but native.log has no system "
            f"intercept (buttons bit 0x{INTERCEPT_BIT:08x} newly set in an 'xinput snapshot' line), not even the "
            "harness's kill: the detector saw nothing, so SYSTEM_INTERCEPT could not have fired on this run")
    rows, steady, phase_err, emits = [], [], None, []
    if game["marks"]:
        try:
            pw, at = phase_windows(nat, game)
            wins_all = sorted(at)
            rows = [phase_row(name, nat["wins"], sel, span, at, wins_all, nat["fd_cap"], nat["tops"])
                    for name, sel, span in pw]
            # "<phase> steady": the phase's windows without a stale-cut pace, only where that
            # drops a window (floor-debt builds; the R1/R8 gates read these rows).
            for (name, sel, span), row in zip(pw, rows):
                st = steady_windows(nat["wins"], sel)
                if row.get("fd_windows") and st and len(st) < len(sel):
                    steady.append(phase_row(f"{name.split()[0]} steady", nat["wins"], st, span, at,
                                            wins_all, nat["fd_cap"]))
                # R1.6b: ph120.w emit over every phase window of the run
                emits += [ints(nat["wins"][x]["ph120.w"].get("emit"))[0] for x in sel
                          if nat["wins"][x].get("ph120.w", {}).get("emit")]
            # GL admit behind: the bid-checked banner says the knob
            # is OBSERVE or ON, so every phase window with a ph120.rt and a ph120.rw owes a
            # ph120.ab and a ph120.abp.  A run without them measured nothing and would read as
            # "no behind gates" (the device rule's ab_pass / ab_phl would be empty, not bad), so
            # it is not a BENCH OK.  A broken identity (ab_bad) is a warning: the columns say where.
            if nat["ab_mode"]:
                w = nat["wins"]
                owed = {name: [x for x in sel if "ph120.rt" in w[x] and "ph120.rw" in w[x]]
                        for name, sel, _ in pw}
                lost = {name: sum(1 for x in xs if "ph120.ab" not in w[x] or "ph120.abp" not in w[x])
                        for name, xs in owed.items()}
                if not any(owed.values()):
                    add("FAIL", "AB_RECORD_MISSING", f"gl-admit-behind {nat['ab_mode']} in the banner "
                        "and no phase window has a ph120.rw: nothing was measured")
                elif any(lost.values()):
                    add("FAIL", "AB_RECORD_MISSING", f"gl-admit-behind {nat['ab_mode']} in the banner, "
                        "but windows with ph120.rw have no ph120.ab or no ph120.abp: " + ", ".join(
                            f"{name.split()[0]} {n} of {len(owed[name])}" for name, n in lost.items() if n))
                broken = [(r["phase"].split()[0], r["ab_bad"]) for r in rows if r.get("ab_bad")]
                if broken:
                    add("WARN", "AB_IDENTITY", "ph120.ab / ph120.abp windows breaking a per-window "
                        "identity (ab_bad): " + ", ".join(f"{p} {n}" for p, n in broken))
            # GL draw packet: a build that prints ph120.dp in one phase window
            # owes it in every window with a ph120.rw; VERIFY is the draw packet's correctness gate, so a
            # differing packet (mm) or an overtaking record (late) is not a BENCH OK.
            if any(r.get("dp_windows") for r in rows):
                lost = [(r["phase"].split()[0], r["dp_missing"]) for r in rows if r.get("dp_missing")]
                if lost:
                    add("FAIL", "DP_RECORD_MISSING", "ph120.dp in the run, but windows with ph120.rw "
                        "have none: " + ", ".join(f"{p} {n}" for p, n in lost))
                bad = [(r["phase"].split()[0], r["dp_bad"], r["dp_mm"], r["dp_late"])
                       for r in rows if r.get("dp_bad")]
                # tot (mm + late since boot, never reset) over EVERY ph120.dp of the run, so a
                # mismatch between phase windows (a floor transition, the menu, after the last
                # phase) is not a BENCH OK either
                run_tot = max((ints(f["ph120.dp"].get("tot"))[:1] or [0])[0]
                              for f in nat["wins"].values() if "ph120.dp" in f)
                if bad or run_tot:
                    add("FAIL", "DP_VERIFY", "draw packet VERIFY: " + "; ".join(
                        (["windows with mm or late > 0: " + ", ".join(
                            f"{p} {n} (mm {m}, late {lt})" for p, n, m, lt in bad)] if bad else []) +
                        ([f"tot (mm + late since boot) = {run_tot} at the run's last ph120.dp"]
                         if run_tot else [])))
                # a VERIFY build that compared nothing proves nothing
                empty = [r["phase"].split()[0] for r in rows if r.get("dp_verify_windows")
                         and r.get("dp_d", 0) > 0 and not r.get("dp_vp")]
                if empty:
                    add("FAIL", "DP_VERIFY_EMPTY", "draw packet VERIFY phases with draws and no packet "
                        "compared (v(p) = 0): " + ", ".join(empty))
                cov = [(r["phase"].split()[0], r["dp_cov"], r["dp_verify_windows"])
                       for r in rows if r.get("dp_cov")]
                if cov:
                    add("FAIL", "DP_VERIFY_COVERAGE", "draw packet VERIFY windows where the packets "
                        "compared (v(p)) and published (pk) differ by more than the take's one: " +
                        ", ".join(f"{p} {n} of {nw}" for p, n, nw in cov))
                vsx = [(r["phase"].split()[0], r["dp_vsx"], r["dp_verify_windows"])
                       for r in rows if r.get("dp_vsx")]
                if vsx:
                    add("WARN", "DP_VERIFY_SUBS", "draw packet VERIFY windows (mm = late = 0) where the "
                        "records compared (v(s)) are not sub + f: " +
                        ", ".join(f"{p} {n} of {nw}" for p, n, nw in vsx))
                fat = [(r["phase"].split()[0], r["dp_path_pd"]) for r in rows
                       if "ON" in r.get("dp_mode", "").split(",") and (r.get("dp_path_pd") or 0) > 2]
                if fat:
                    add("WARN", "DP_RECORDS_PER_DRAW", "draw packet ON with more than 2 ring records "
                        "per draw on the draw path: " + ", ".join(f"{p} {v:.2f}" for p, v in fat))
        except ValueError as exc:
            phase_err = str(exc)
            add("FAIL", "ROOM_ALIGN", phase_err)
    if stopped is None:
        if not done:
            last = names[-1] if names else "none"
            add("FAIL", "INCOMPLETE", f"no 'MARK BENCH DONE' (last marker: {last}; native log ends at "
                f"{nat['last_ts']:.1f} s)")
        got = {r["phase"].split()[0] for r in rows}
        miss = [p for p in expect if p not in got]
        if miss and phase_err is None:
            add("FAIL", "NO_PHASES" if not rows else "MISSING_PHASES", "phases without data: " + ", ".join(miss))
        for r in rows:
            if r["windows"] == 0:
                add("FAIL", "PHASE_EMPTY", f"phase {r['phase']}: no whole 120-loop window inside it "
                    f"(span {r['span_s']:.1f} s)")
        if done and not any(m.startswith("remove [") for m in game["repro"]):
            add("WARN", "NOT_REMOVED", "the script did not remove save2.dat: delete it by FTP")
    for r in rows:
        c = r.get("census")
        if c and (c["incomplete"] or c["mismatch"]):
            add("WARN", "CENSUS_WINDOWS", f"phase {r['phase']}: opcode census {c['incomplete']} window(s) "
                f"incomplete (a ph120.rop part missing), {c['mismatch']} disagreeing with ph120.rt/rw; "
                f"{c['windows']} used")
    # --- dry phases: P6-P8 on Basement I, in rooms without water
    seq = mark_seq(game)
    dry = []
    for i, (_, name, fields) in enumerate(seq):
        if name.split()[0] not in DRY_PHASES or not re.match(r"P\d", name):
            continue
        nxt = seq[i + 1] if i + 1 < len(seq) else None
        x = dry_fields(fields, nxt[2] if nxt else None, name, nxt[1] if nxt else None)
        dry.append(x)
        where = (f"opening {x.get('opening_stage', '?')}, closing {x.get('closing_stage', '?')}, "
                 f"backdrop {x.get('backdrop', '?')}, water {x.get('water', '?')}, haswater {x.get('haswater', '?')}")
        if x["verdict"] == "not dry":
            add("WARN", "DRY_PHASE_NOT_DRY", f"phase {name}: " + "; ".join(x["not_dry"]) + f" ({where})")
        elif x["verdict"] == "unproven":
            add("WARN", "DRY_PHASE_UNPROVEN", f"phase {name}: " + "; ".join(x["unproven"]) + f" ({where}): "
                "its room is not proven dry (a main.lua without the room fields, or a binding that failed)")
        else:
            add("OK", "DRY_PHASE", f"phase {name}: Basement I, no water ({where}; MARK {name} and MARK {x['closing']})")
    if dry:
        # Did the item build survive "stage 1"?  The census's
        # familiars count is the measurable part of it: the list's familiar entities come from its
        # giveitem lines only, and all 43 October bench runs with markers log familiars=5 at all 7
        # markers BENCH START..BENCH DONE and 0 at NONCE, before giveitem.  P6's own marker and the
        # markers that close P6, P7 and P8 are logged on the dry floor; each must repeat MARK BENCH
        # START's count (a proxy: it sees the items that make familiars, not the others).
        fam = {n: f.get("familiars", "?") for _, n, f in seq}
        ref = fam.get("BENCH START", "?")
        on_dry = [(n, fam.get(n, "?")) for n in [x["phase"] for x in dry] + ["BENCH DONE"]]
        for x in dry:
            x.update(familiars_start=ref, familiars_opening=fam.get(x["phase"], "?"),
                     familiars=fam.get(x["closing"], "?") if x["closing"] else "?")
        listed_f = ", ".join(f"MARK {n} familiars={v}" for n, v in on_dry)
        moved = [(n, v) for n, v in on_dry if v.isdigit() and ref.isdigit() and int(v) != int(ref)]
        if moved:
            add("WARN", "DRY_ITEMS_CHANGED", f"the item build did not survive 'stage 1': MARK BENCH START "
                f"familiars={ref}, but {listed_f}: the dry phases are not the same build as P1-P5")
        elif not ref.isdigit() or any(not v.isdigit() for _, v in on_dry):
            add("WARN", "DRY_ITEMS_UNPROVEN", f"MARK BENCH START familiars={ref}, {listed_f}: a count is "
                "missing, so the item build across 'stage 1' is not proven")
        elif int(ref) == 0:
            add("WARN", "DRY_ITEMS_UNPROVEN", "MARK BENCH START familiars=0: the census cannot see the item "
                "build (the bench list's logs 5 on the device), so its survival across 'stage 1' is not proven")
        else:
            add("OK", "DRY_ITEMS", f"familiars={ref} at MARK BENCH START and at every dry-floor marker "
                f"({listed_f}): the item build survived 'stage 1'")
    rank = {"FAIL": 0, "WARN": 1, "OK": 2}
    d.sort(key=lambda x: (rank[x[0]], PRIORITY.index(x[1]) if x[1] in PRIORITY else len(PRIORITY)))
    fails = [x for x in d if x[0] == "FAIL"]
    verdict = (f"BENCH FAILED ({', '.join(x[1] for x in fails)}): {fails[0][2]}" if fails else
               f"BENCH OK: {len(rows)} phases, save slot {SLOT} proven, no File 1 access in the logs")
    if eid_heap_pin is not None:
        verdict += f" [EID heap pin overridden: --eid-heap-pin {eid_heap_pin[0]}:{eid_heap_pin[1]}]"
    lua_errors = sorted([(no, code, text) for code, key in (
        ("MOD_LUA_ERROR", "lua_err"), ("LUA_FILE_ERROR", "lua_file_err"), ("EID_ERROR", "eid_err"),
        ("MOD_MAIN_CHUNK_ERROR", "lua_main_err")) for no, text in game[key]])
    loadfile = None
    if nat["lf_stats"]:
        last = (lf_done or nat["lf_stats"])[-1]
        loadfile = {k: (_n(v) if v.isdigit() else v) for k, v in last.items() if not k.startswith("_")}
    # MARK fields for R8.8: name -> fields of its last occurrence (an @seed restart
    # replays the list), NONCE marks left out (their name differs per run)
    marks_fields = {}
    for name, fields in game["mark_fields"]:
        if not name.startswith("NONCE"):
            marks_fields[name] = fields
    return {"verdict": verdict, "ok": not fails, "diagnostics": d, "phases": rows,
            "fd_steady": steady, "fd_mode": nat["fd_mode"], "fd_cap": nat["fd_cap"], "aw_mode": nat["aw_mode"],
            "aw_receipt": nat["aw_receipt"],
            "ab_mode": nat["ab_mode"], "ab_bytes": nat["ab_bytes"], "ab_receipt": nat["ab_receipt"],
            "fd_gates": floor_debt_gates(rows, steady, nat["fd_mode"], nat["fd_cap"]),
            "marks_fields": marks_fields, "emit50": med(emits) / 1e3 if emits else None,
            "emit_n": len(emits),
            "build": nat["build"], "native_bytes": nat["bytes"], "native_last_s": nat["last_ts"],
            "windows": len(nat["wins"]), "room_markers": len(nat["rooms"]), "game_rooms": len(game["pairs"]),
            "markers": names, "game_starts": game["starts"], "seed_level": seed_level,
            "seed_rooms": seed_rooms,
            # the dry floor (Basement I after MARK DRY START) and its phases
            "seed_level_dry": seed_level_dry, "seed_rooms_dry": seed_rooms_dry, "dry": dry,
            "expected_phases": list(expect),
            # bench validity gate: read by ab_gate, compare and
            # load_run (an analyze JSON without eid_version predates the gate)
            "eid_version": eid_version,
            "eid_heap": list(game["eid_heap"]) if game["eid_heap"] is not None else None,
            "lua_errors": [[code, f"log.txt:{no}: {text}"] for no, code, text in lua_errors],
            "loadfile": loadfile, "intercepts": [x["line"] for x in nat["intercepts"]],
            "image_preload": irp_summary(nat["irp"])}


# The verdict names the failure that explains the others first.
PRIORITY = ("STOPPED_IN_BOOT", "SUSPEND_ENODEV", "GUEST_STOP", "NO_LAUNCH_ARGS", "PRE_SLOT_EBOOT",
            "LAUNCH_ARGS_STATUS", "SLOT_NOT_2", "NO_SET_STAGE", "SLOT_NOT_APPLIED", "SLOT_LATE",
            "SLOT_RESTORED", "FILE1_LOAD", "RNG_SLOT", "EXTRA_LUA",
            # bench validity gate: a mod that failed to load names
            # the run first
            "MOD_LUA_ERROR", "LUA_FILE_ERROR", "EID_ERROR", "EID_NOT_LOADED", "NO_EID", "EID_HEAP_MISSING",
            "EID_HEAP_MISMATCH", "LUA_LOADFILE_UNRECOVERED", "MOD_MAIN_CHUNK_ERROR", "SYSTEM_INTERCEPT",
            "NO_HARNESS_KILL", "NO_SAVE_DATA", "NONCE",
            "SEED_NOT_TAKEN", "ROOM_ALIGN", "INCOMPLETE", "DRY_START_MISSING", "NO_PHASES", "MISSING_PHASES", "PHASE_EMPTY")

COLS = [("fps", "{:6.1f}"), ("gap50", "{:6.1f}"), ("gap95", "{:6.1f}"), ("gap95w", "{:7.1f}"),
        ("gapmax", "{:7.1f}"), ("ups", "{:5.1f}"), ("U", "{:6.2f}"), ("R", "{:6.2f}"),
        ("sleep_pu", "{:6.2f}"), ("upd50", "{:6.2f}"), ("rnd50", "{:6.2f}"), ("rnd95", "{:6.2f}"),
        ("swp50", "{:6.2f}")]
GPU_COLS = [("gpu_pct", "{:5.1f}"), ("gpu_pf", "{:6.2f}"), ("gf50", "{:6.2f}"), ("gf95", "{:6.2f}"),
            ("gf95w", "{:6.2f}"), ("rbusy_pf", "{:6.2f}"), ("rcpu_pf", "{:6.2f}"), ("lag50", "{:6.2f}"),
            ("q2_share", "{:5.2f}"), ("swpq2_pf", "{:6.2f}"), ("swpoth_pf", "{:6.2f}"), ("pend", "{:4.0f}")]
FD_COLS = [("loss_ms", "{:7.2f}"), ("late_ms", "{:7.2f}"), ("res_obs", "{:7.2f}"),
           ("res_obsw", "{:8.2f}"), ("res_on", "{:7.2f}"), ("res_onw", "{:7.2f}"),
           ("kept_late", "{:9.2f}"), ("h5", "{:4.0f}"), ("h6", "{:4.0f}"), ("stale", "{:5.0f}"),
           ("gfpos", "{:5.0f}"), ("gfmax", "{:6.2f}"), ("gfstuck", "{:7.0f}"),
           ("slack_b", "{:7.1f}"), ("slack_ms", "{:8.2f}"),
           ("pg", "{:3.0f}")]
FDA_COLS = [("fres_obs", "{:8.3f}"), ("fres_on", "{:7.3f}"), ("fn", "{:3.0f}"), ("fx", "{:3.0f}"),
            ("bmin", "{:7.2f}"), ("psum_obs", "{:8.2f}"), ("psum_on", "{:7.2f}"), ("capk", "{:5.3f}"),
            ("clampn", "{:6.0f}"), ("ups_sum", "{:7.3f}"), ("fxgap", "{:5.0f}"), ("above_ms", "{:8.1f}"),
            ("capw", "{:4.0f}"), ("latemax", "{:7.1f}")]
# The per-run inputs of the run-comparison gates (bench_run.py s5gates)
RUNCMP_COLS = [("quiet", "{:5.0f}"), ("gapw", "{:4.0f}"), ("pps_q", "{:7.2f}"), ("ups_q", "{:7.3f}"),
               ("gap95_q", "{:7.2f}"), ("emit50", "{:6.2f}"), ("viol", "{:4.0f}"), ("reset", "{:5.0f}")]
FDGF_COLS = [("gfpos50", "{:7.1f}"), ("gfposw", "{:6.0f}"), ("gfmax50", "{:7.3f}"),
             ("gfmax99", "{:7.3f}"), ("gf05", "{:4.0f}"), ("gf1", "{:4.0f}"), ("gf2", "{:4.0f}"),
             ("gf3", "{:4.0f}")]
RT_COLS = [("rt_busy_pct", "{:5.1f}"), ("fr50", "{:6.2f}"), ("fr95", "{:6.2f}"), ("frmax", "{:7.1f}"),
           ("sw_pf", "{:6.2f}"), ("held", "{:5.0f}"), ("sblk_pf", "{:6.2f}"), ("rec_pf", "{:6.0f}"),
           ("wk_pf", "{:5.2f}"), ("mq_share", "{:5.2f}"), ("prod_us", "{:6.2f}"), ("refused", "{:5.0f}")]
# GL admit wait (ph120.aw, ISAAC_VITA_GL_THREAD_ADMIT_WAIT)
AW_COLS = [("aw_n", "{:5.0f}"), ("aw_ok", "{:5.0f}"), ("aw_to", "{:5.0f}"), ("aw_nw", "{:5.0f}"),
           ("aw_bo", "{:5.0f}"), ("aw_tt", "{:5.0f}"), ("aw_ms", "{:7.1f}"), ("aw_max", "{:6.2f}"),
           ("aw_lt", "{:5.0f}"), ("aw_lt_ms", "{:8.2f}"), ("aw_hl", "{:5.0f}"), ("aw_hl_ms", "{:8.2f}"),
           ("aw_ov", "{:5.2f}"), ("aw_hd", "{:5.0f}"), ("aw_hd_ms", "{:8.2f}"), ("fit_e", "{:5.0f}"),
           ("fit_a", "{:5.0f}"), ("miss_ms", "{:7.1f}"), ("aw_bad", "{:6.0f}"), ("aw_missing", "{:10.0f}")]
# GL admit behind (ph120.ab + ph120.abp, ISAAC_VITA_GL_THREAD_ADMIT_BEHIND)
AB_COLS = [("ab_n", "{:5.0f}"), ("ab_stp", "{:6.0f}"), ("ab_stf", "{:6.0f}"), ("ab_stx", "{:6.0f}"),
           ("ab_pass", "{:7.3f}"), ("ab_ov", "{:5.0f}"), ("ab_wa", "{:5.0f}"), ("ab_wa_share", "{:11.3f}"),
           ("ab_ok", "{:5.0f}"), ("ab_bo", "{:5.0f}"), ("ab_tt", "{:5.0f}"), ("ab_ob50", "{:7.0f}"),
           ("ab_obmax", "{:8.0f}"), ("ab_hl", "{:5.0f}"), ("ab_hl_ms", "{:8.2f}"), ("ab_hd", "{:5.0f}"),
           ("ab_hd_ms", "{:8.2f}"), ("ab_s_w", "{:6.0f}"), ("ab_s_ms", "{:8.1f}")]
ABP_COLS = [("pm_c", "{:5.0f}"), ("pm_ok", "{:5.0f}"), ("pm_ms", "{:7.1f}"), ("pm_max", "{:6.2f}"),
            ("wp_c", "{:5.0f}"), ("wp_ok", "{:5.0f}"), ("wp_ms", "{:7.1f}"), ("wp_max", "{:6.2f}"),
            ("ab_phl", "{:6.0f}"), ("ab_bad", "{:6.0f}"), ("ab_missing", "{:10.0f}")]
# GL draw packet (ph120.dp, ISAAC_VITA_GL_DRAW_PACKET)
DP_COLS = [("dp_d", "{:7.0f}"), ("dp_f", "{:7.0f}"), ("dp_pk", "{:7.0f}"), ("dp_p1", "{:6.0f}"),
           ("dp_sub", "{:7.0f}"), ("dp_sub_pd", "{:9.2f}"), ("dp_path_pd", "{:10.3f}"),
           ("dp_rec_pd", "{:9.2f}"), ("dp_mx", "{:5.0f}"), ("dp_fr_r", "{:7.0f}"), ("dp_fr_c", "{:7.0f}"),
           ("dp_fr_s", "{:7.0f}"), ("dp_fr_f", "{:7.0f}"), ("dp_vp", "{:7.0f}"), ("dp_vs", "{:7.0f}"),
           ("dp_mm", "{:5.0f}"), ("dp_late", "{:7.0f}"), ("dp_tot", "{:6.0f}"), ("dp_bad", "{:6.0f}"),
           ("dp_cov", "{:6.0f}"), ("dp_vsx", "{:6.0f}"), ("dp_missing", "{:10.0f}")]
MEAN_COLS = [("pps_mean", "{:8.2f}"), ("rps_mean", "{:8.2f}"), ("ups_mean", "{:8.3f}")]
TL_COLS = [("tgt_pr", "{:6.1f}"), ("tl_n_pr", "{:7.1f}"), ("tl_h_pr", "{:7.1f}"), ("tl_hit_pct", "{:10.1f}"),
           ("tl_clk_pr", "{:9.1f}"), ("tl_x", "{:6.0f}"), ("tl_t", "{:6.0f}"), ("tl_d", "{:4.0f}"),
           ("tl_s", "{:4.0f}"), ("tl_w", "{:4.0f}")]


def table(rows: list, cols: list) -> list[str]:
    out = [f"  {'phase':22} {'win':>3} " + " ".join(f"{c:>{len(f.format(0))}}" for c, f in cols)]
    for r in rows:
        vals = " ".join(f.format(r[c]) if r.get(c) is not None else " " * len(f.format(0)) for c, f in cols)
        out.append(f"  {r['phase']:22} {r['windows']:3d} {vals}")
    return out


IRP_LEVEL_KEYS = ("loaded", "retained", "dropped", "present", "fail", "skipped", "bytes", "us", "hr_kb",
                  "need_kb", "fault")
IRP_PIN_LEVEL_KEYS = ("pins", "pinned_bytes", "share", "released", "pinned", "own", "exact", "alias", "held")
IRP_TOTAL_KEYS = ("levels", "loaded", "retained", "dropped", "present", "fail", "hit", "miss",
                  "evicted_unused", "skip", "breach", "bytes", "us", "why")
IRP_PIN_TOTAL_KEYS = ("mode", "pinned", "refused", "used", "floor", "yield", "stale", "dropped", "skip", "cskip",
                      "own", "exact", "alias", "held", "on_free", "lost", "twins", "pins", "pinned_bytes", "why")


def irp_report(irp: dict | None) -> list[str]:
    """The image preload (+ pin) block of the report; nothing when the build has no preload."""
    if not irp:
        return []

    def pick(f: dict | None, keys: tuple) -> str:
        return " ".join(f"{k}={f[k]}" for k in keys if f and k in f) if f else "-"

    armed = irp.get("armed") or {}
    pin_armed = irp.get("pin_armed")
    out = ["", f"image preload (KAGE VITA IMAGE PRELOAD; list={armed.get('list', '?')} "
               f"reserve_mb={armed.get('reserve_mb', '?')} max_kb={armed.get('max_kb', '?')}; pin "
               f"{pin_armed.get('mode', '?') if pin_armed else 'not built'}; skipped = gates s,l,t,v,h,k; "
               f"hr_kb before..after against need_kb):"]
    pins = {p.get("level"): p for p in irp.get("pin_levels", [])}
    for lv in irp.get("levels", []):
        line = f"  level {lv.get('level', '?'):>3}: {pick(lv, IRP_LEVEL_KEYS)}"
        if lv.get("level") in pins:
            line += f" | pin {pick(pins[lv.get('level')], IRP_PIN_LEVEL_KEYS)}"
        out.append(line)
    for im in irp.get("images", []):
        out.append(f"  decode: name={im.get('name', '?')} result={im.get('result', '?')} "
                   f"bytes={im.get('bytes', '?')} dim={im.get('dim', '?')} flags={im.get('flags', '?')} "
                   f"us={im.get('us', '?')} hr_kb={im.get('hr_kb', '?')} key={im.get('key', '?')}")
    out.append(f"  totals: {pick(irp.get('totals'), IRP_TOTAL_KEYS)} "
               f"(hit lines {irp.get('hit_lines', 0)}, miss lines {irp.get('miss_lines', 0)}, "
               f"evicted-unused lines {irp.get('evicted_unused_lines', 0)})")
    if pin_armed is not None or irp.get("pin_totals"):
        out.append(f"  pin totals: {pick(irp.get('pin_totals'), IRP_PIN_TOTAL_KEYS)}")
        lines = irp.get("pin_lines", [])
        if lines:
            out.append(f"  pin present/miss/lost lines ({len(lines)} of {irp.get('pin_lines_n', len(lines))}):")
            out += [f"    {x}" for x in lines]
    return out


def report(res: dict) -> str:
    out = [res["verdict"], ""]
    for level, code, msg in res["diagnostics"]:
        out.append(f"  {level:4} {code}: {msg}")
    out.append("")
    out.append(f"build {res['build']}; native log {res['native_bytes']} B to {res['native_last_s']:.1f} s; "
               f"{res['windows']} ph120 windows; room markers native {res['room_markers']} / game "
               f"{res['game_rooms']}; game starts {res['game_starts']}; markers {res['markers']}"
               + (f"; gl-admit-wait {res['aw_mode']}" if res.get("aw_mode") else "")
               + (f"; GL ADMIT WAIT receipt mode={res['aw_receipt']} disagrees with the banner"
                  if res.get("aw_receipt") and res.get("aw_receipt") != res.get("aw_mode") else "")
               + (f"; gl-admit-behind {res['ab_mode']} bytes={res['ab_bytes']}" if res.get("ab_mode") else "")
               + (f"; GL ADMIT BEHIND receipt mode/bytes={res['ab_receipt']} disagrees with the banner"
                  if res.get("ab_receipt")
                  and res.get("ab_receipt") != f"{res.get('ab_mode')}/{res.get('ab_bytes')}" else ""))
    out += irp_report(res.get("image_preload"))
    rows = res["phases"]
    if rows:
        out.append("")
        out.append("frame times per phase (ms; fps = 1e6 / median gap p50; gap95 = median window p95, "
                   "gap95w = worst window p95; ups = game updates/s; U = ms per update, R = ms per "
                   "render, sleep_pu = scheduler sleep ms per update, from ph120.hs/ph120.s; "
                   "upd50/rnd50/rnd95/swp50 = ph120.t call percentiles):")
        out += table(rows, COLS)
        if any(r.get("pps_mean") is not None for r in rows):
            out.append("")
            out.append("mean rates per phase (sum of cad p / r / g over the phase's windows / sum of "
                       "their at-deltas; fps above is 1e6 / median gap, which picks one mode when a phase "
                       "alternates between one and two renders per update):")
            out += table(rows, MEAN_COLS)
        if res.get("dry"):
            out.append("")
            out.append("dry phases (floor stage/type at the phase's own MARK and at the "
                       "next one, room fields at the next one = the phase's room; water = GetWaterAmount, "
                       "absent in 1.7.9b; haswater = water amount > 0; familiars at the two MARKs vs "
                       "MARK BENCH START's = the item build across 'stage 1'):")
            for x in res["dry"]:
                fam = (f", familiars {x['familiars_opening']} -> {x['familiars']} (start {x['familiars_start']})"
                       if "familiars" in x else "")
                out.append(f"  {x['phase']:22} {x['verdict']:9} floor {x.get('opening_stage', '?')} -> "
                           f"{x.get('closing_stage', '?')}, backdrop {x.get('backdrop', '?')}, water "
                           f"{x.get('water', '?')}, haswater {x.get('haswater', '?')}{fam} (closing MARK {x['closing']})")
        out.append("")
        out.append("run-comparison inputs (read by bench_run.py s5gates: quiet = consecutive "
                   "windows with ph120.s drop(t) = 0 and reset = 0, gapw = windows whose predecessor "
                   "record is lost, pps_q/ups_q = presents/updates per s over the quiet windows, "
                   "gap95_q = their median gap p95 in ms, emit50 = median ph120.w emit in ms):")
        out += table(rows, RUNCMP_COLS)
        if any(r.get("gpu_windows") for r in rows):
            out.append("")
            out.append("GPU per phase (ph120.gpu; ms per frame unless %; gf = GPU ms per frame; rbusy/rcpu "
                       "= render time with / without a scene pending; q2_share = presents with the queue full):")
            out += table(rows, GPU_COLS)
        for r in rows:
            if r.get("gpu_verdict"):
                out.append(f"  {r['phase']:22} GPU: {r['gpu_verdict']}")
        if any(r.get("rt_windows") for r in rows):
            out.append("")
            out.append("GL render thread per phase (ph120.rt/rw; fr = render-thread busy ms per present, "
                       "held = presents that waited for the previous one, wk_pf = producer wakes per "
                       "present, prod_us = producer us per record, refused = interpolated frames declined):")
            out += table(rows, RT_COLS)
        if any(r.get("aw_n") is not None for r in rows):
            out.append("")
            out.append("GL admit wait per phase (ph120.aw, gl-admit-wait "
                       f"{res.get('aw_mode') or 'unknown'}; n = interpolated gates that found the render "
                       "thread behind; ok = waited, then rendered; to = waited to latest, closure still "
                       "running, refused; nw = no window (no live floor, or the stock time test already "
                       "fails), refused; bo = refused inside a back-off; tt = closure done, then refused by "
                       "the tick-due re-check, the stock time test or a clock anomaly; aw_ms/aw_max = waited "
                       "ms (sum, worst); aw_lt/aw_lt_ms = Game::Update starts past the floor on the full tick "
                       "right after an ok whose loop head did not sleep; aw_hl/aw_hl_ms = loop heads that did "
                       "not sleep after a Present held on an ok's closure, ms past their due time (lt and hl "
                       "each arm a 30-phase back-off); aw_ov = worst wake past a timed-out wait's deadline, "
                       "ms; aw_hd/aw_hd_ms = Present holds on an ok's closure; fit_e/fit_a = refusals with a "
                       "window checked at the next Present / of them finished by latest; miss_ms = how late "
                       "the others were; OBSERVE never waits (ok = to = bo = tt = lt = hl = hd = 0); the mode "
                       "is the bid-checked CADENCE30 banner's; aw_bad = windows "
                       "breaking rs = n - ok - tt, n >= ok+to+nw+bo+tt or fit_e <= n - ok - tt - nw + 1; "
                       "aw_missing = windows with ph120.rw and no ph120.aw):")
            out += table(rows, AW_COLS)
        if any(r.get("ab_n") is not None for r in rows):
            out.append("")
            out.append("GL admit behind per phase (ph120.ab, gl-admit-behind "
                       f"{res.get('ab_mode') or 'unknown'} bytes={res.get('ab_bytes') or 'unknown'}; "
                       "n = interpolated gates that found the render thread behind; ab_stp/stf/stx = the "
                       "stock time test there: passing, failing, not live (stock admits); ab_pass = "
                       "(stp + stx) / n; ov = outstanding ring bytes over the bound; wa = within the bound "
                       "and the time test not failing (what ON renders outside a back-off), ab_wa_share = "
                       "wa / n; ok = rendered while behind; bo = within the bound, refused inside a "
                       "back-off; tt = within the bound, refused by the stock time test; ab_ob50 = median "
                       "of the windows' median outstanding bytes (first 64 gates each), ab_obmax = the "
                       "largest reading; ab_hl/ab_hl_ms = loop heads that did not sleep after a Present "
                       "hold on a behind-admitted frame, ms past their due time (each arms a 30-phase "
                       "back-off); ab_hd/ab_hd_ms = Present holds on or right after a behind-admitted "
                       "frame; ab_s_w/ab_s_ms = the render thread's sync waits over the same windows "
                       "(ph120.rt s); OBSERVE renders nothing extra (ok = bo = tt = hl = hd = 0); the mode "
                       "and the bound are the bid-checked CADENCE30 banner's):")
            out += table(rows, AB_COLS)
            out.append("")
            out.append("GL admit behind, refusals checked at the next Present (ph120.abp; pm_c = checked, "
                       "pm_ok = the closure was done by the refused frame's predicted Present, pm_ms/pm_max "
                       "= the hold of the others, sum and worst, ms; wp_* = the same over the wa frames; "
                       "ab_phl = of pm_c, the hold would have carried the Render return to or past the next "
                       "loop head's due time; ab_bad = windows breaking n = stp + stf + stx, rs = n - ok, "
                       "ov + wa <= n, ok <= wa, pm_ok <= pm_c <= n - ok + 1, wp_c <= pm_c, wp_ok <= wp_c or "
                       "phl <= pm_c; ab_missing = windows with ph120.rw and no ph120.ab or ph120.abp):")
            out += table(rows, ABP_COLS)
        if any(r.get("dp_windows") for r in rows):
            modes = sorted({m for r in rows for m in r.get("dp_mode", "").split(",") if m})
            out.append("")
            out.append(f"GL draw packet per phase (ph120.dp, mode {','.join(modes) or 'unknown'} as the "
                       "recorder prints it; d = glDrawElements records requested, f = of them carried in a "
                       "packet, pk = packet records, p1 = staged runs of exactly one call (ON publishes each "
                       "as its own record, VERIFY as a one-call packet, also in pk), "
                       "sub = state calls staged, sub_pd = per draw; path_pd = (pk + p1 + d - f) / d, the "
                       "ring records of the draw path per draw (ON gate <= 2; VERIFY also publishes every "
                       "call alone); rec_pd = every ring record per draw (ph120.rt r / d); mx = most bytes "
                       "staged at one publish; fr_r/c/s/f = publishes ahead of another record, a closure, a "
                       "drain/outstanding query/stop, a full buffer or a draw too large to fuse; VERIFY: vp/vs = "
                       "packets compared and "
                       "the records they were compared with, mm = packets that differed, late = records "
                       "that overtook staged calls, tot = mm + late since boot, bad = windows with mm or "
                       "late, cov = VERIFY windows with |vp - pk| > 1, vsx = VERIFY windows with mm = late = 0 "
                       "and vs != sub + f; dp_missing = windows with ph120.rw and no ph120.dp):")
            out += table(rows, DP_COLS)
        if any(r.get("census") for r in rows):
            out.append("")
            out.append("GL recorder opcode census per phase (ph120.ro/rop; per present: n records, B payload "
                       "bytes, rnd = render-thread replay us, game = game-thread us (1/64 sampled, admission "
                       "check to append end, clock cost removed), S = S calls, Sblk = blocked S-wait us; "
                       "flags x = rnd scaled from the exact replays only, - = no game-thread sample, p/r = a "
                       f"sampler > 4 sigma off 1/{CENSUS_K}; top {CENSUS_TOP} by rnd):")
            for r in rows:
                c = r.get("census")
                if not c:
                    continue
                out.append(f"  {r['phase']:22} windows {c['windows']} (incomplete {c['incomplete']}, "
                           f"disagreeing {c['mismatch']}), presents {c['presents']}, clock read "
                           f"game/render {c['ck_game_us']:.2f}/{c['ck_render_us']:.2f} us"
                           + ("" if c["render_us_pf"] is None else
                              f"; all opcodes rnd {c['render_us_pf']:.0f} us, game {c['game_us_pf']:.0f} us "
                              "per present"))
                if not c["ops"]:
                    continue
                out.append(f"    {'opcode':30} {'n':>7} {'B':>8} {'rnd':>8} {'game':>8} {'S':>6} {'Sblk':>8} flags")
                for o in c["ops"][:CENSUS_TOP]:
                    game = "" if o["game_us_pf"] is None else f"{o['game_us_pf']:.1f}"
                    out.append(f"    {o['op']:2d} {o['name']:27} {o['n_pf']:7.1f} {o['b_pf']:8.0f} "
                               f"{o['render_us_pf']:8.1f} {game:>8} {o['s_pf']:6.2f} {o['sblk_us_pf']:8.1f} "
                               f"{o['flags']}")
        if any(r.get("tl_windows") for r in rows):
            out.append("")
            out.append("Clear expiry clock latch per phase (ph120.a tgt + ph120.tl; per render = per cad r; "
                       "n = calls from the two Clear loop sites, h = latch hits (ON) or would-be hits "
                       "(OBSERVE), clk = clock reads, x = expiries, t/d = OBSERVE ticks/decision flips, "
                       "s/w = tripwires):")
            out += table(rows, TL_COLS)
            for r in rows:
                if r.get("tl_verdict"):
                    out.append(f"  {r['phase']:22} latch: {r['tl_verdict']}")
        if any(r.get("fd_windows") for r in rows):
            fdrows = rows + res.get("fd_steady", [])
            out.append("")
            out.append(f"floor debt per phase (ph120.fd/fs, mode {res.get('fd_mode') or 'unknown'}; ms per "
                       "window, medians unless w = worst window; '<P> steady' = the phase's windows "
                       "without a stale-cut pace; loss = at-delta - g * 33.333 ms; res_obs = |loss - "
                       "late - stale|, res_on = |loss - over - stale| (information: at= carries the "
                       "boundary tick's work time); kept_late = sum kept / sum late; "
                       "h5/h6 = late paces 5-16.7 / 16.7-33.3 ms; stale = stale-cut paces; gfpos = gf > 0 "
                       "samples, gfmax = largest gf, gfstuck = windows with every gf > 0 and the last "
                       "above 1 ms (must be 0); slack_b = slack-band frames per window, slack_ms = "
                       "mean lateness after a slack admit; pg = max |p - g|):")
            out += table(fdrows, FD_COLS)
            out.append("")
            out.append("gf per window (ph120.fd gf; the R1 reference for R8: gfpos50/gfposw = median/worst "
                       "samples > 0 per window, gfmax50/gfmax99 = median/nearest-rank p99 of the window "
                       "maximum in ms, gf05/gf1/gf2/gf3 = windows whose maximum is above 0.5/1/2/3 ms):")
            out += table(fdrows, FDGF_COLS)
            out.append("")
            out.append("floor-debt attribution (ph120.fs floor stamps: fres_obs/fres_on = worst "
                       "window |dfloor - p * 33.333 ms - (late | over) - stale| in ms, the R1.3/R8.3 "
                       "identity; fn/fx = windows checked / excluded (no stamp, p != g); bmin = smallest "
                       "at - floor in ms (>= -33.333); psum_obs/psum_on = worst contiguous run's |sum "
                       "loss - sum attribution| / windows in ms; capk = sum(kept + clamp) / sum late, "
                       "R1.4; clampn = late paces cut by the grid clamp; ups_sum = sum g / sum at-delta, "
                       "R8.2; fxgap = windows excluded for a lost predecessor record, above_ms "
                       "= lateness above the cap, capw = windows whose largest late pace is above the "
                       "cap, latemax = the largest late pace in ms):")
            out += table(fdrows, FDA_COLS)
            if res.get("fd_gates"):
                out.append("")
                out.append("floor-debt gates:")
                out += [f"  {g}" for g in res["fd_gates"]]
        if any(r.get("residual") for r in rows):
            out.append("")
            out += residual_lines(rows)
    return "\n".join(out)


def residual_lines(rows: list, top: int = 24) -> list[str]:
    def f(v, width=6, digits=2):
        return f"{v:{width}.{digits}f}" if v is not None else " " * width
    out = ["render residual per phase (ph120.hotr; ph120.hotw/hotl/hotu/hcl, ph120.fe; ms per render = "
           "rnd samples at 1 ms / renders; no root (all) = hotr root-0 samples (t = translated sample, "
           "n = native) over the windows that carry hotr, every GUEST_SAMPLER_CHAIN build; it includes "
           "samples that crossed a live Lua boundary and then lost the walk; root = the sample's "
           "innermost chain root is this function, lua = that root started the Lua callback the sample "
           "is in; lbx = boundary-valued words at addresses the Lua bridge did not publish (stale, not "
           "linked); calls = translated body entries per render; us/call = inclusive (self) ms per "
           "render / calls):"]
    for r in rows:
        res = r.get("residual")
        if not res:
            continue
        lua = res["lua"]
        if res.get("no_root_t") is not None:
            line = (f"  {r['phase']} ({res.get('root_windows', res['windows'])} windows, "
                    f"{res.get('root_renders', res['renders'])} renders): no root (all) "
                    f"{res['no_root_t']:.2f} t + {res['no_root_n']:.2f} n ms/render")
            if res.get("no_root_crossed_t") is not None:
                line += (f", of which crossed a boundary first {res['no_root_crossed_t']:.2f} t + "
                         f"{res['no_root_crossed_n']:.2f} n; Lua-crossed rooted {res['lua_crossed']:.2f}; "
                         f"boundary words per render lb {lua.get('lb', 0):.2f} lbm {lua.get('lbm', 0):.2f} "
                         f"lbs {lua.get('lbs', 0):.2f} lbx {lua.get('lbx', 0):.2f}")
            else:
                line += ("; crossed-first and Lua-crossed not measured (no ph120.hotw/hcl: a build "
                         "without ISAAC_VITA_GUEST_SAMPLER_RESIDUAL has no +800000 hotr keys)")
            out.append(line)
        else:
            out.append(f"  {r['phase']} ({res['windows']} windows, {res['renders']} renders): no ph120.hotr, "
                       "no root figures")
        if not res["functions"]:
            continue
        if res["windows"] != res.get("root_windows", res["windows"]) or \
                res["renders"] != res.get("root_renders", res["renders"]):
            out.append(f"    watch table over the hotw/fe windows: {res['windows']} windows, "
                       f"{res['renders']} renders")
        cn = res.get("chain_native")
        if cn:
            out.append("    render chain native per render (ph120.rn; calls of 25af60/55f610/55f630 below "
                       "come from it): " + " ".join(f"{k} {cn[k]:.1f}" for k in
                                                    ("f", "fd", "gn", "gc", "r", "ri", "rd")))
        out.append(f"    {'fn':>8} {'self':>6} {'incl':>6} {'root_t':>6} {'root_n':>6} {'lua_t':>6} "
                   f"{'lua_n':>6} {'calls':>7} {'us/call':>7} {'self_us':>7}")
        for fn in res["functions"][:top]:
            out.append(f"    {fn['fn']:>8} {f(fn['self'])} {f(fn['incl'])} {f(fn['root_t'])} "
                       f"{f(fn['root_n'])} {f(fn['lua_t'])} {f(fn['lua_n'])} {f(fn['calls'], 7, 1)} "
                       f"{f(fn['us_incl'], 7)} {f(fn['us_self'], 7)}")
        if res["hotl"]:
            out.append("    invoker 40eef0 by next root out (ms/render t/n/lt/ln, 0 = none): " +
                       ", ".join(f"{k} " + "/".join(f"{x:.2f}" for x in v) for k, v in res["hotl"].items()))
        if res["hotu"]:
            out.append("    rootless by outermost frame (ms/render, +1000000 = native sample): " +
                       ", ".join(f"{k} {v:.2f}" for k, v in res["hotu"]))
    return out


def rooms_line(a: dict, b: dict) -> str:
    """Whether A and B played the same floor: both seeded (OK SEED) with one stage seed and the
    same room loads (game log 'Room <stage>.<variant>' lines) after the seeded Level::Init."""
    ra, rb = a.get("seed_rooms"), b.get("seed_rooms")
    if ra is None or rb is None:
        who = " and ".join(n for n, r in (("A", ra), ("B", rb)) if r is None)
        return f"rooms: NOT COMPARABLE ({who} without a proven seed: no OK SEED line; compare phases, not rooms)"
    sa, sb = a["seed_level"]["seed"], b["seed_level"]["seed"]
    if sa != sb:
        return f"rooms: DIFFER (floor seed A {sa}, B {sb})"
    if ra != rb:
        k = next((j for j, (x, y) in enumerate(zip(ra, rb)) if x != y), min(len(ra), len(rb)))
        ax = ra[k] if k < len(ra) else "end"
        bx = rb[k] if k < len(rb) else "end"
        return f"rooms: DIFFER at room load {k + 1} (A {ax}, B {bx}; floor seed {sa} in both)"
    line = f"rooms: identical (floor seed {sa}, {len(ra)} room loads)"
    # the dry floor (information; the decision phases are on the seeded floor)
    da, db = a.get("seed_rooms_dry"), b.get("seed_rooms_dry")
    if da is None and db is None:
        return line
    if da is None or db is None:
        return line + f"; dry floor: only {'A' if db is None else 'B'} has a proven one (OK SEED_DRY)"
    xa, xb = a["seed_level_dry"]["seed"], b["seed_level_dry"]["seed"]
    if xa != xb:
        return line + f"; dry floor: DIFFER (seed A {xa}, B {xb})"
    if da != db:
        k = next((j for j, (x, y) in enumerate(zip(da, db)) if x != y), min(len(da), len(db)))
        return line + (f"; dry floor: DIFFER at room load {k + 1} (A {da[k] if k < len(da) else 'end'}, "
                       f"B {db[k] if k < len(db) else 'end'}; seed {xa} in both)")
    return line + f"; dry floor identical (seed {xa}, {len(da)} room loads)"


def no_root_line(ra: dict, rb: dict) -> str:
    """The no-root class of A and B side by side, its sum and
    the B - A difference, the crossed-first part and the Lua-crossed roots (n/a = the build has
    no RESIDUAL records), over each side's hotr windows."""
    def v(res, k):
        x = res.get(k)
        return "n/a" if x is None else f"{x:.2f}"
    ta, na, tb, nb = ra["no_root_t"], ra["no_root_n"], rb["no_root_t"], rb["no_root_n"]
    return (f"  {'':22} no root (all) ms/render B vs A: t {ta:.2f}->{tb:.2f}, n {na:.2f}->{nb:.2f}, "
            f"t+n {ta + na:.2f}->{tb + nb:.2f} ({(tb + nb) - (ta + na):+.2f}); crossed-first t "
            f"{v(ra, 'no_root_crossed_t')}->{v(rb, 'no_root_crossed_t')}, n {v(ra, 'no_root_crossed_n')}->"
            f"{v(rb, 'no_root_crossed_n')}; Lua-crossed rooted {v(ra, 'lua_crossed')}->{v(rb, 'lua_crossed')} "
            f"(A {ra.get('root_windows', ra['windows'])} windows {ra.get('root_renders', ra['renders'])} renders, "
            f"B {rb.get('root_windows', rb['windows'])} windows {rb.get('root_renders', rb['renders'])} renders)")


def eid_line(a: dict, b: dict) -> str:
    """EID version and heap of both runs: runs that did not load
    the same Lua are not comparable."""
    def h(r):
        v = r.get("eid_heap")
        return f"{v[0]} KB and {v[1]} bytes" if v else "none"
    va, vb = a.get("eid_version"), b.get("eid_version")
    txt = f"EID: A v{va} heap {h(a)}, B v{vb} heap {h(b)}"
    if va is None or vb is None:
        return (txt + " -- NOT COMPARABLE (" + ("A" if va is None else "B") + " has no EID banner: EID did "
                "not load, or a JSON from before the bench validity gate)")
    if va != vb:
        return txt + " -- NOT COMPARABLE (EID versions differ)"
    return txt + (" (same Lua heap)" if a.get("eid_heap") == b.get("eid_heap") else
                  " (heaps differ: a lane that changes Lua memory, or a partial load)")


def compare(a: dict, b: dict) -> str:
    out = [f"A: {a['verdict']}", f"B: {b['verdict']}", rooms_line(a, b), eid_line(a, b),
           "per phase B - A (ms; fps in fps):"]
    keys = ("fps", "pps_mean", "rps_mean", "gap50", "gap95", "U", "R", "sleep_pu", "rnd50", "gf50", "gf95", "fr50")
    bp = {r["phase"]: r for r in b["phases"]}
    for r in a["phases"]:
        o = bp.get(r["phase"])
        if not o:
            out.append(f"  {r['phase']:22} missing in B")
            continue
        parts = []
        for k in keys:
            if r.get(k) is not None and o.get(k) is not None:
                parts.append(f"{k} {r[k]:.2f}->{o[k]:.2f} ({o[k] - r[k]:+.2f})")
        out.append(f"  {r['phase']:22} " + ", ".join(parts))
        ra, rb = r.get("residual"), o.get("residual")
        if bool(ra) != bool(rb):
            out.append(f"  {'':22} residual only in {'A' if ra else 'B'}: {'B' if ra else 'A'}'s log has no "
                       "ph120.hotr/hotw/fe in this phase, or its JSON is from an older analyzer "
                       "(re-run analyze on it)")
        if ra and rb:
            if ra.get("no_root_t") is not None and rb.get("no_root_t") is not None:
                out.append(no_root_line(ra, rb))
            fb = {fn["fn"]: fn for fn in rb["functions"]}
            items = []
            for fn in ra["functions"][:10]:
                other = fb.get(fn["fn"])
                if other:
                    items.append(f"{fn['fn']} {fn['incl']:.2f}->{other['incl']:.2f}")
            if items:
                out.append(f"  {'':22} residual incl ms/render B vs A: " + ", ".join(items))
            # The render chain A/B: inclusive us per call, which does not move
            # with the fight's draw count the way ms/render does.
            per_call = []
            for name in ("3e50", "4b60", "4760", "55f610", "55f630", "25af60", "25b0f0", "55f990"):
                a_fn = next((fn for fn in ra["functions"] if fn["fn"] == name), None)
                b_fn = fb.get(name)
                if a_fn and b_fn and a_fn["us_incl"] is not None and b_fn["us_incl"] is not None:
                    per_call.append(f"{name} {a_fn['us_incl']:.2f}->{b_fn['us_incl']:.2f}")
            if per_call:
                out.append(f"  {'':22} chain us/call incl B vs A: " + ", ".join(per_call))
    return "\n".join(out)


# ------------------------------------------------------------------------------------- prepare
def check_inputs(bench: Path) -> tuple[str, bytes, bytes]:
    template = (bench / "bench-save.txt").read_text(encoding="utf-8").replace("\r\n", "\n")
    lines = template.split("\n")
    assert "@continue" in lines, "bench-save.txt lost its @continue line"
    assert "@marker BENCH DONE" in lines, "bench-save.txt lost its BENCH DONE marker"
    assert lines[-2:] == ["@remove", ""], "bench-save.txt must end with '@remove'"
    args = (bench / "launch-args.txt").read_bytes()
    toks = [t for ln in args.decode("ascii").splitlines() for t in ln.split("#", 1)[0].split()]
    assert f"--vita-save-slot={SLOT}" in toks, "launch-args.txt must carry --vita-save-slot=2"
    assert any(t.startswith("--set-stage=") for t in toks), "launch-args.txt must carry --set-stage="
    assert len(args) <= 512 and len(toks) <= 8, "launch-args.txt over the eboot's 512 B / 8 token cap"
    return template, args, (bench / "main.lua").read_bytes()


def list_phases(save: str) -> list[str]:
    """The phases a command list makes, in order: the first word of each '@marker P<n> ...' line
    (phase_windows takes every marker whose name starts with P and a digit)."""
    out = []
    for line in save.split("\n"):
        m = re.match(r"@marker\s+(.+)$", line.strip(), re.I)
        if m and re.match(r"P\d", m.group(1).strip()):
            key = m.group(1).split()[0]
            if key not in out:
                out.append(key)
    return out


def list_updates(save: str) -> int:
    """Game updates the command list waits in all: main.lua runs '@wait n' as max(n, 1) updates."""
    return sum(max(int(m.group(1)), 1) for line in save.split("\n")
               for m in [re.match(r"@wait\s+(\d+)$", line.strip())] if m)


def second_sleep_s(updates: int) -> int:
    """The procedure's second sleep: 150 s for the list before the dry section (2745 updates), plus
    the updates beyond them at DRY_UPDATES_PER_S, rounded up to 10 s."""
    extra = max(0, updates - OLD_LIST_UPDATES)
    return OLD_SECOND_SLEEP_S + 10 * math.ceil(extra / DRY_UPDATES_PER_S / 10)


def procedure(run: Path, man: dict) -> str:
    """The device steps for this run, one per line, in order."""
    up = {f["remote"]: f for f in man["upload"]}
    b, a = run / "before", run / "after"
    ie = man.get("installed_eboot") or {}
    L = []

    def s(text):
        L.append(f"{len(L) + 1:2d}. {text}")
    s("vita_status: FTP and app control must both be available.  Then "
      f"vita_app_status {TITLE} -> running=false.  running=true: STOP, someone may be playing.")
    for name, remote in LISTINGS.items():
        s(f"vita_list {remote} -> save the JSON result verbatim as {b / name}"
          + ("  (an error 'cannot list' = absent: write [] )" if remote == MOD_DATA else ""))
    s(f"check {b / 'list-root.json'}: no launch-args.txt.  Present: STOP (another bench left it).")
    for name, remote in BACKUPS.items():
        s(f"vita_download {remote} -> {b / name}"
          + ("  (skip if the 'before' listing does not show it)" if name != "repro-main.lua" else ""))
    s(f"if {b / 'list-repro.json'} lists save2.dat: vita_download {MOD_DATA}/save2.dat -> {b / 'repro-save2.dat'}")
    s(f"vita_download {EBOOT} -> {b / 'eboot.bin'}"
      + (f"; on the host its sha256 should be {ie['sha256'][:16]}... (prepare --installed-sha256)"
         if ie.get("sha256") else "")
      + "; whatever it is, this file is what goes back in step 'restore eboot'.")
    s(f"if {MOD_DATA} was absent: vita_mkdir {MOD_DATA}")
    for remote, f in up.items():
        s(f"vita_upload {run / f['local']} -> {remote}   ({f['bytes']} B, sha256 {f['sha256'][:16]}...)")
    eb = man.get("eboot") or {}
    s(f"vita_deploy_eboot {TITLE} {eb.get('path', '<bench eboot>')} launch=false"
      + (f" ({eb['bytes']} B, sha256 {eb['sha256'][:16]}...)" if eb else "")
      + "; note where it backed up the old eboot.bin, if it does.  On an error (e.g. an FTP timeout) "
      "repeat this vita_deploy_eboot until it is ok, or STOP; never launch after a failed deploy "
      "(killrecord binds the first ok deploy after this run's upload).")
    s(f"vita_nosleep on (keep the console awake); then vita_launch {TITLE} (app control only, no FTP).")
    s("sleep 60 s (one plain sleep, no polling).")
    s(f"vita_app_status {TITLE} (app control) and vita_stat {NATIVE_LOG} (FTP).  Healthy: running=true, "
      f"foreground=true, size >= {HEALTHY_NATIVE_BYTES_AT_60S} B.  Otherwise: record both values, skip to "
      "'vita_kill' and analyze (the analyzer names the stop).")
    updates = int(man.get("list_updates", OLD_LIST_UPDATES))
    nap = second_sleep_s(updates)
    s("sleep 150 s." if nap == OLD_SECOND_SLEEP_S else
      f"sleep {nap} s (150 s for the list's first {OLD_LIST_UPDATES} updates, + {nap - OLD_SECOND_SLEEP_S} s for "
      f"its other {updates - OLD_LIST_UPDATES} at {DRY_UPDATES_PER_S:g} updates/s: the dry phases).")
    s(f"vita_tail_file {GAME_LOG} 4096 B -> must show '[repro_console] MARK BENCH DONE' and 'remove [start]'.  "
      "Missing: sleep 60 s and repeat this step at most twice more, then go on.")
    s("sleep 10 s (the window after the last room change closes).")
    s(f"vita_kill {TITLE} (app control); then vita_nosleep off.  On an error: vita_app_status "
      f"{TITLE} first, and repeat vita_kill only while it says running=true.  A kill that errored but "
      "stopped the app (an outcome-unknown error) stays this run's kill: analyze FAILs NO_HARNESS_KILL "
      "on its 'error' status, which names the cause, where a retry would become the bound kill "
      "(killrecord takes the last one) and FAIL on a time that is not the kill's.  Either way the run "
      "is repeated.")
    s(f"host, after the kill: python tools/bench/bench_run.py killrecord --run {run} --audit <audit.jsonl>  "
      f"-> writes {run / 'kill.json'} (status, start and end of this run's deploy, launch and kill, from the "
      f"device-control tool's audit log, found from this run's own upload of {run / 'save2.dat'}: the first "
      f"ok deploy after it, the last kill before the next deploy; refused if another file was uploaded to "
      f"{MOD_DATA}/save2.dat after it and before the kill, even by an upload that errored).  Run it now, "
      "before the next run's upload.  Without it analyze FAILs NO_HARNESS_KILL.")
    s(f"vita_download {NATIVE_LOG} -> {run / 'native.log'}")
    s(f"vita_download {GAME_LOG} -> {run / 'game.log'}")
    s(f"host: python tools/bench/bench_run.py analyze --manifest {run / 'manifest.json'} --native "
      f"{run / 'native.log'} --game {run / 'game.log'} --json {run / 'result.json'}")
    s(f"vita_upload {b / 'repro-main.lua'} -> {MOD_MAIN}")
    s(f"vita_delete {MOD_DATA}/save2.dat (skip if the script removed it: analyze shows no NOT_REMOVED "
      f"and the step after next lists no save2.dat); if {b / 'repro-save2.dat'} exists upload it back instead.")
    s(f"vita_delete {ARGS_FILE}")
    s(f"restore eboot: vita_deploy_eboot {TITLE} {b / 'eboot.bin'} launch=false; then vita_stat {EBOOT} "
      + (f"-> size {ie['bytes']} (prepare --installed-bytes)." if ie.get("bytes") is not None
         else f"-> the size of {b / 'eboot.bin'}."))
    for name, remote in LISTINGS.items():
        s(f"vita_list {remote} -> {a / name}")
    s(f"host: python tools/bench/bench_run.py listing --before {b} --after {a}  -> prints each restore "
      "action; 'STOP' lines (File 1 changed) go to the player before anything else.")
    s("for each 'restore' action: vita_upload <backup> -> <remote>, one at a time, then vita_download "
      f"<remote> -> {a}/<name> and python tools/bench/bench_run.py verify --before {b} --after {a} "
      "-> MATCH; each 'delete' action: vita_delete <remote>.")
    s(f"vita_list {APP_DIR}; vita_delete each eboot.bin backup that the two deploy_eboot calls "
      "created, one at a time.")
    return "\n".join(L) + "\n"


def cmd_prepare(a) -> int:
    template, args, main_lua = check_inputs(HERE)
    run = Path(a.out).resolve()
    run.mkdir(parents=True, exist_ok=True)
    (run / "before").mkdir(exist_ok=True)
    (run / "after").mkdir(exist_ok=True)
    nonce = a.nonce or uuid.uuid4().hex[:12]
    save = template.replace("\n@continue\n", f"\n@continue\n@marker NONCE {nonce}\n", 1)
    (run / f"save{SLOT}.dat").write_bytes(save.encode("utf-8"))
    (run / "launch-args.txt").write_bytes(args)
    (run / "main.lua").write_bytes(main_lua)
    files = [(f"save{SLOT}.dat", f"{MOD_DATA}/save{SLOT}.dat"), ("main.lua", MOD_MAIN),
             ("launch-args.txt", ARGS_FILE)]
    man = {"tag": a.tag, "nonce": nonce, "created": time.strftime("%Y-%m-%d %H:%M:%S"), "slot": SLOT,
           "upload": [{"local": n, "remote": r, "bytes": (run / n).stat().st_size,
                       "sha256": sha256(run / n)} for n, r in files],
           "backups": BACKUPS, "listings": LISTINGS,
           "native_log": NATIVE_LOG, "game_log": GAME_LOG,
           # this run records its harness kill in kill.json (procedure step 'killrecord');
           # analyze FAILs NO_HARNESS_KILL without it
           "kill_record": True,
           # analyze expects these phases (MISSING_PHASES otherwise); the procedure's second
           # sleep follows the list's length
           "phases": list_phases(save), "list_updates": list_updates(save)}
    if a.installed_sha256 or a.installed_bytes is not None:
        man["installed_eboot"] = {k: v for k, v in (("sha256", a.installed_sha256),
                                                    ("bytes", a.installed_bytes)) if v is not None}
    if a.eboot:
        eb = Path(a.eboot).resolve()
        man["eboot"] = {"path": str(eb), "bytes": eb.stat().st_size, "sha256": sha256(eb)}
    (run / "manifest.json").write_text(json.dumps(man, indent=1) + "\n", encoding="utf-8")
    text = procedure(run, man)
    (run / "procedure.txt").write_text(text, encoding="utf-8")
    print(f"prepared {run} (nonce {nonce})")
    if not a.eboot:
        # without the eboot, killrecord cannot tell this run's deploy from a non-bench one
        print("WARN no --eboot: killrecord cannot check that the deploy it binds installed this run's eboot "
              "(an ok non-bench deploy after an errored one of this run's would be taken as this run's); "
              "prepare a device run with --eboot")
    if a.download_root:
        try:
            run.relative_to(Path(a.download_root).resolve())
        except ValueError:
            print(f"WARN {run} is not under {a.download_root} (--download-root): the device-control tool "
                  "may refuse to download into it")
    print(text)
    return 0


# ------------------------------------------------------------------------------------- kill record
# The device-control tool's audit log: JSON lines, one 'started' row before each operation and one
# 'ok' or 'error' row after it, each with its 'operation', 'title_id', an ISO 'time' and, for file
# operations, 'local_path' and 'remote_path'.  The tool results themselves carry no times, so
# kill.json is built from these rows.


def audit_steps(rows: list, op: str, title: str = TITLE) -> list:
    """[(start row index or None, end row index)] of one operation for one title, in log order."""
    out, pending = [], None
    for i, r in enumerate(rows):
        if r.get("operation") != op or str(r.get("title_id", "")).upper() != title.upper():
            continue
        if r.get("status") == "started":
            pending = i
        else:
            out.append((pending, i))
            pending = None
    return out


def _first_row(step: tuple) -> int:
    """An audit_steps() entry's first row: its 'started' row, or its end row when it has none."""
    return step[1] if step[0] is None else step[0]


def _norm_path(p) -> str | None:
    return os.path.normcase(os.path.abspath(p)) if p else None


def kill_record(rows: list, run: Path, eboot: str | None = None, title: str = TITLE, source: str = "") -> dict:
    """kill.json bound to THIS run: every bench run uploads its own RUN/save2.dat and the
    device-control tool audits that upload with its local_path.  From the last ok upload of
    RUN/save2.dat: the first deploy_eboot of `title` after it whose audit status is ok (this run's
    deploy; a deploy that errored, e.g. an FTP timeout, and was retried is skipped, not taken as the
    end of the window), the LAST kill of `title` before the next deploy_eboot of `title` after that
    one, whatever its status (or the end of the log), and the last launch between that deploy and
    that kill (absent when the deploy launched the app itself).
    eboot: the manifest's eboot path, which the deploy must have used; None skips that test.  Who is
    exposed when it is None: not the older manifests without kill_record (diagnose applies
    kill_check only to a manifest with kill_record), but new runs prepared without --eboot
    (kill_record true, no eboot), for which a non-bench ok deploy after this run's errored one can be
    taken as this run's.  prepare warns about a run without --eboot.
    Raises ValueError when there is no such upload, ok deploy or kill (a run the harness never
    killed), when another file is uploaded to MOD_DATA/save2.dat (any local file other than this
    RUN/save2.dat, ok or errored, so another run's or a restore of a backup; not EID's save2.dat)
    after this run's upload and before the kill, or when the deploy installed another eboot.
    What this excludes and what it does not (the claim is not 'never another run's kill'):
    - excluded: a deploy or kill of a LATER bench run.  That run uploads its own save2.dat before its
      deploy: an upload by another run after this run's upload and before the kill is refused (also
      when this run's own deploy errored and was not retried, so that the first ok deploy is the
      later run's), and when this run has its own ok deploy, the later run's deploy ends the window.
    - not excluded: a non-bench kill of `title` (and a launch before it; no save2.dat upload, no
      deploy) after this run's kill and before the next deploy.  It is taken as this run's kill
      (that launch then starts the clock); kill_check's time test decides (NO_HARNESS_KILL unless
      launch-to-kill matches native.log's length within KILL_TOLERANCE_S), and NONCE decides
      whether the downloaded logs are this run's.
    - two runs whose save2.dat uploads both precede the same deploy (prepared out of order): the
      earlier run is refused (its save2.dat was overwritten before any deploy), the later one binds
      to that deploy and kill."""
    mine = _norm_path(str(Path(run) / "save2.dat"))
    ups = [i for i, r in enumerate(rows) if r.get("operation") == "upload" and r.get("status") == "ok"
           and _norm_path(r.get("local_path")) == mine]
    if not ups:
        raise ValueError(f"no ok upload of {Path(run) / 'save2.dat'} in the audit log: this run's upload is "
                         "the anchor, so the record cannot be bound to the run")
    up = ups[-1]
    deploys = [s for s in audit_steps(rows, "deploy_eboot", title) if _first_row(s) > up]
    done = [s for s in deploys if rows[s[1]].get("status") == "ok"]
    if not done:
        raise ValueError(f"no ok deploy_eboot of {title} after this run's save2.dat upload (audit row {up + 1})"
                         + (f"; {len(deploys)} deploy(s) after it, none ok" if deploys else ""))
    ds, de = done[0]
    later = [s for s in deploys if _first_row(s) > de]
    end = _first_row(later[0]) if later else len(rows)
    kills = [s for s in audit_steps(rows, "kill", title) if _first_row(s) > de and s[1] < end]
    if not kills:
        raise ValueError(f"no kill of {title} after this run's deploy (audit row {de + 1}) and before "
                         + (f"the next deploy_eboot (audit row {end + 1})" if end < len(rows) else "the end of the log")
                         + ": the harness did not kill this run")
    ks, ke = kills[-1]
    k0 = _first_row(kills[-1])
    # Scan from this run's UPLOAD, not from the chosen deploy.  When this run's own deploy errored
    # and was not retried, the first ok deploy can be the next bench run's, and that run's save2.dat
    # upload lies before it.
    # Only uploads to the file the bench script runs from, MOD_DATA/save2.dat (the audit keeps the
    # case the remote path was given in; compared without case, so a spelling that differs only in
    # case is refused too), so EID's data/836319872/save2.dat that the procedure restores is not one.
    # Errored uploads count: an upload that errored after it installed its file (a failed backup
    # cleanup, or a rollback that itself failed) can still have replaced this run's save2.dat.  An
    # orphan 'started' row with no end row is not counted.
    target = (MOD_DATA + "/save2.dat").lower()
    other = [i + 1 for i in range(up + 1, k0) if rows[i].get("operation") == "upload"
             and rows[i].get("status") in ("ok", "error") and str(rows[i].get("remote_path", "")).lower() == target
             and _norm_path(rows[i].get("local_path")) != mine]
    if other:
        # the fact, not who did it (the deploy and the kill can be this run's own)
        raise ValueError(f"another save2.dat upload (audit rows {other}) between this run's save2.dat upload "
                         f"(audit row {up + 1}) and the kill (audit row {k0 + 1}): this run's save2.dat on the "
                         "console was replaced before the kill (another run's upload, or a restore; an errored "
                         "upload can still have installed its file), so the record is not bound to this run")
    launches = [s for s in audit_steps(rows, "launch", title) if de < _first_row(s) and s[1] < k0]

    def step(s, e):
        out = {"status": rows[e].get("status"), "start": rows[s].get("time") if s is not None else None,
               "end": rows[e].get("time"), "audit_rows": [None if s is None else s + 1, e + 1]}
        if rows[e].get("error"):
            out["error"] = rows[e]["error"]
        return out
    rec = {"source": source, "title_id": title, "deploy": dict(step(ds, de), local_path=rows[de].get("local_path")),
           "launch": step(*launches[-1]) if launches else None, "kill": step(ks, ke)}
    if eboot is not None and _norm_path(rec["deploy"]["local_path"]) != _norm_path(eboot):
        raise ValueError(f"the deploy after this run's upload installed {rec['deploy']['local_path']}, "
                         f"not this run's eboot {eboot}")
    return rec


def cmd_killrecord(a) -> int:
    run = Path(a.run)
    man = read_json(run / "manifest.json") or {}
    audit = Path(a.audit)
    rows = []
    for no, line in enumerate(audit.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        try:
            rows.append(json.loads(line))
        except ValueError:
            rows.append({"_malformed": no})
    try:
        rec = kill_record(rows, run, (man.get("eboot") or {}).get("path"), source=str(audit))
    except ValueError as exc:
        print(f"killrecord: NOT WRITTEN -- {exc}")
        return 1
    (run / "kill.json").write_text(json.dumps(rec, indent=1) + "\n", encoding="utf-8")
    for name in ("deploy", "launch", "kill"):
        st = rec[name]
        print(f"{name:6} " + ("(by the deploy)" if st is None else
                              f"{st['status']} {st['start']} -> {st['end']} (audit rows {st['audit_rows']})"))
    bad = [n for n in ("deploy", "launch", "kill") if rec[n] is not None and rec[n]["status"] != "ok"]
    print(f"killrecord: wrote {run / 'kill.json'}" + (f"; NOT ok: {', '.join(bad)}" if bad else ""))
    return 1 if bad else 0


# ------------------------------------------------------------------------------------- listings
FILE1 = re.compile(r"(^|\.)(gamestate1|persistentgamedata1)\.dat$|^save1\.dat$")


def load_listing(path: Path) -> dict:
    """A vita_list JSON result (a list of entries, or a dict holding one) -> name -> entry."""
    data = json.loads(path.read_text(encoding="utf-8"))
    if isinstance(data, dict):
        for k in ("entries", "items", "result", "files", "list"):
            if isinstance(data.get(k), list):
                data = data[k]
                break
    if not isinstance(data, list):
        raise SystemExit(f"{path}: not a listing (want a JSON list of entries)")
    return {e.get("name") or e["path"].rsplit("/", 1)[-1]: e for e in data if e.get("type", "file") == "file"}


def listing_actions(before: dict, after: dict, remote_dir: str) -> list[tuple[str, str]]:
    """STOP = a File 1 file changed (the player decides), restore = upload the backup, delete = a
    file the run created, CHECK = changed and no backup (look first).  Logs are expected to change."""
    backup_of = {remote: name for name, remote in BACKUPS.items()}
    acts = []
    for name in sorted(set(before) | set(after)):
        b, a = before.get(name), after.get(name)
        remote = f"{remote_dir}/{name}"
        if b and a and (b.get("size"), b.get("modify")) == (a.get("size"), a.get("modify")):
            continue
        what = "new" if not b else "deleted" if not a else "changed"
        if FILE1.search(name):
            acts.append(("STOP", f"File 1 file {what}: {remote} ({b} -> {a}); tell the player, do not touch it"))
        elif name.endswith(".log") or name == "log.txt":
            continue  # the game's and the port's logs
        elif what == "new":
            acts.append(("delete", remote))
        elif remote in backup_of:
            acts.append(("restore", f"{remote} ({what}) from before/{backup_of[remote]}"))
        else:
            acts.append(("CHECK", f"{remote} {what}, no backup ({b} -> {a})"))
    return acts


def cmd_listing(a) -> int:
    rc = 0
    for name, remote in LISTINGS.items():
        bp, ap = Path(a.before) / name, Path(a.after) / name
        if not bp.exists() or not ap.exists():
            print(f"MISSING {name}: list {remote} before and after")
            rc = 1
            continue
        for act, what in listing_actions(load_listing(bp), load_listing(ap), remote):
            print(f"{act:7} {what}")
            rc = 2 if act == "STOP" else max(rc, 1)
    print("listing: " + ("no changes outside the bench files" if rc == 0 else "actions above"))
    return rc


def cmd_verify(a) -> int:
    rc = 0
    for name, remote in BACKUPS.items():
        bp, ap = Path(a.before) / name, Path(a.after) / name
        if not bp.exists():
            print(f"NO-BACKUP {name} ({remote})")
            rc = 1
        elif not ap.exists():
            print(f"NOT-READ  {name}: vita_download {remote} -> {ap}")
            rc = 1
        elif sha256(bp) == sha256(ap):
            print(f"MATCH     {name} {sha256(ap)[:16]}")
        else:
            print(f"CHANGED   {name}: vita_upload {bp} -> {remote}")
            rc = 1
    return rc


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--out", required=True)
    p.add_argument("--tag", default="bench")
    p.add_argument("--nonce")
    p.add_argument("--eboot", help="the bench eboot.bin to deploy (sha256 and size go into the manifest)")
    p.add_argument("--installed-sha256", metavar="HEX",
                   help="sha256 of the eboot installed before the run, if known (named in the download step)")
    p.add_argument("--installed-bytes", type=int, metavar="N",
                   help="size of the eboot installed before the run, if known (named in the restore step)")
    p.add_argument("--download-root", metavar="DIR",
                   help="the directory the device-control tool may download into; WARN when RUN is outside it")
    p = sub.add_parser("analyze")
    p.add_argument("--native", required=True, type=Path)
    p.add_argument("--game", required=True, type=Path)
    p.add_argument("--manifest", type=Path)
    p.add_argument("--nonce")
    p.add_argument("--json", type=Path)
    p.add_argument("--eid-heap-pin", type=parse_heap_pin, metavar="KB:BYTES",
                   help="expected EID heap for a lane that changes Lua memory (replaces EID_HEAP_PIN; "
                        "printed in the verdict)")
    p = sub.add_parser("killrecord", help="write RUN/kill.json from the device-control tool's audit log "
                                          "(procedure step after vita_kill; bound to RUN/save2.dat's "
                                          "audited upload)")
    p.add_argument("--run", required=True)
    p.add_argument("--audit", required=True, help="the audit log, JSON lines")
    p = sub.add_parser("compare")
    p.add_argument("a", type=Path)
    p.add_argument("b", type=Path)
    p = sub.add_parser("s5gates", help="S5 run-comparison gates R1.6 / R8.5 / R8.8 / R8.9")
    p.add_argument("--off", action="append", type=Path, required=True,
                   help="a seeded OFF run of the session (run dir or analyze --json); repeat for ABAB")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--observe", action="append", type=Path, help="the OBSERVE run(s) (R1.6)")
    g.add_argument("--on", action="append", type=Path, help="the ON runs (R8.5/R8.8/R8.9); ABAB needs two")
    p.add_argument("--any-mode", action="store_true",
                   help="do not check the floor-debt banner of each run (noise studies on OFF builds)")
    for name in ("listing", "verify"):
        p = sub.add_parser(name)
        p.add_argument("--before", required=True)
        p.add_argument("--after", required=True)
    sub.add_parser("selftest")
    a = ap.parse_args(argv)
    if a.cmd == "prepare":
        return cmd_prepare(a)
    if a.cmd == "analyze":
        # the manifest's directory holds kill.json (NO_HARNESS_KILL without it)
        man = json.loads(a.manifest.read_text(encoding="utf-8")) if a.manifest else {}
        nonce = a.nonce or man.get("nonce")
        kill = read_json(a.manifest.parent / "kill.json") if a.manifest else None
        res = diagnose(a.native.read_text(encoding="utf-8", errors="replace"),
                       a.game.read_text(encoding="utf-8", errors="replace"), nonce,
                       manifest=man, kill=kill, eid_heap_pin=a.eid_heap_pin)
        print(report(res))
        if a.json:
            a.json.write_text(json.dumps(res, indent=1) + "\n", encoding="utf-8")
        return 0 if res["ok"] else 1
    if a.cmd == "compare":
        print(compare(json.loads(a.a.read_text(encoding="utf-8")), json.loads(a.b.read_text(encoding="utf-8"))))
        return 0
    if a.cmd == "s5gates":
        mode, tests = ("observe", a.observe) if a.observe else ("on", a.on)
        lines = run_gates([load_run(x) for x in a.off], [load_run(x) for x in tests], mode,
                          expect_mode=not a.any_mode)
        print("\n".join(lines))
        return 1 if any(": FAIL" in ln for ln in lines) else 0
    if a.cmd == "killrecord":
        return cmd_killrecord(a)
    if a.cmd == "listing":
        return cmd_listing(a)
    if a.cmd == "verify":
        return cmd_verify(a)
    if a.cmd == "selftest":
        sys.path.insert(0, str(HERE))
        import bench_run_test
        return bench_run_test.main([])
    return 2


if __name__ == "__main__":
    sys.exit(main())
