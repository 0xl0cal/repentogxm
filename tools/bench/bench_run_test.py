#!/usr/bin/env python3
"""Host tests of the bench harness: python bench_run_test.py [--smoke-root DIR]

--smoke-root (default: the BENCH_SMOKE_ROOT environment variable) is a directory of downloaded
device run directories (bench-<label>-<date>/native.log, game.log, ...).  They are not in the
repository; without it every leg that re-reads them prints SKIP and the synthetic legs still run.

Legs:
  lua      bench_test.lua (main.lua + bench-save.txt) in three @seed modes via lupa (set, stuck,
           absent) and with the MARK room bindings present / absent / raising, plus mutants that
           must fail (no current-room skip, no @remove, no lead-in room, SetStartSeed not called, a
           console seed restart, stage before @seed; the dry section: no DRY START, P5 not ended
           by @room 3, dry floor on Downpour, no room fields, GetWaterAmount called, no pcall)
  dry      the dry section: P6-P8 on Basement I, P1-P5 rows and rooms
           equal to the list without it, the Basement seed, DRY_PHASE_NOT_DRY / _UNPROVEN from the
           closing marker's fields, expected phases from the manifest, the mean rates, and the
           dry phases as information in rooms_line, the floor-debt gates and the run gates
  seed     the transcribed seed arithmetic against six device start-seed / Level::Init pairs
           (2026-10-25 logs), the in-place seed rules, rooms_line, and the analyzer on the
           2026-10-25 full logs when they are present
  smoke    the analyzer on excerpts of the three 2026-10-17 smoke logs (verbatim lines) and, when
           --smoke-root holds them, on the full downloaded logs
  phases   a synthetic run built from real record lines (P2b window 41 ph120.t/hs/c/s, the GPU
           oracle's ph120.gpu, glthread-ab2 window 2 ph120.rt/rw) with hand-computed expectations
  admitwait the GL admit wait record (ISAAC_VITA_GL_THREAD_ADMIT_WAIT): one synthetic ph120.aw per
           window in OBSERVE and ON, the receipt's mode, the phase sums, aw_bad from one mutated rs
           and from I2 / I3, aw_missing from one window without the record
  admitbehind the GL admit behind records (ISAAC_VITA_GL_THREAD_ADMIT_BEHIND): one synthetic ph120.ab +
           ph120.abp per window in OBSERVE and ON, the banner's mode and bound, the phase sums and
           shares, ab_bad from one mutated rs and from J1 / J3 / J4, ab_missing from one window
           without either record
  drawpacket the GL draw packet record (ISAAC_VITA_GL_DRAW_PACKET): one synthetic ph120.dp per
           window in VERIFY and ON, the printed mode, the phase sums and per-draw ratios, FAIL
           DP_VERIFY from one window with mm or late, FAIL DP_RECORD_MISSING from one window
           without the record, WARN DP_RECORDS_PER_DRAW only in ON
  negative one mutation per File-1 / slot / completeness rule; each must FAIL with its code
  residual the sampler oracle's scenario 8 records (ph120.hotw/hotl/hotu/hcl/hotr, ph120.fe) in
           every window, hotw/fe split over lines: per-render values, union of split lines,
           stale census warning, report and compare lines
  residual review: rootless crossed hotr keys 800000/1800000 count as no root (a
           synthetic table and the oracle's scenario 8c window), lbx, per render over the
           windows that carry the records, a reprinted window replaces its records
  residual r2: a hotr-only log (a GUEST_SAMPLER_CHAIN build without RESIDUAL or
           the census, i.e. A2) gets the no-root line and the compare no-root line, from verbatim
           A2S hotr lines; and, when --smoke-root holds bench-A2S-20261025, the real log: the
           per-phase no-root table, the windows 15-21 figure, the analyze text
           before the residual section unchanged from the stored analyze.txt
  windows  the phase window arithmetic of 4e7dc84's test (skip, suffix alignment)
  prepare  prepare's files, nonce, manifest (phases, list length) and the input checks; the list
           minus its dry section is 58f4f9d's command lines; the second sleep
  listing  listing/verify restore actions
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bench_run as b  # noqa: E402

FAILS: list[str] = []


def check(name: str, cond: bool, detail="") -> None:
    if not cond:
        FAILS.append(f"{name}: {detail}")
        print(f"  FAIL {name}: {detail}")


def codes(res: dict, level: str = "FAIL") -> set:
    return {c for lv, c, _ in res["diagnostics"] if lv == level}


# ----------------------------------------------------------------------------------- lua
LUA_MUTANTS = [
    ("main.lua", "    if okCur and list[i].idx == cur and #list > 1 then\n",
     "    if false and okCur and list[i].idx == cur and #list > 1 then\n", "no current-room skip"),
    ("bench-save.txt", "@room 3\n@remove\n", "@room 3\n", "no @remove"),
    ("bench-save.txt", "@marker BENCH START\n@wait 30\n@room 1\n@wait 30\n", "@marker BENCH START\n@wait 30\n",
     "no lead-in room change before P1"),
    ("main.lua", "pcall(function() seeds:SetStartSeed(want) end)", "pcall(function() end)",
     "SetStartSeed not called"),
    ("main.lua", "pcall(function() seeds:SetStartSeed(want) end)",
     "pcall(function() seeds:SetStartSeed(want); Isaac.ExecuteCommand(\"seed \" .. want) end)",
     "console seed restart issued"),
    ("bench-save.txt", "@seed 20261017\nstage 1c\n", "stage 1c\n@seed 20261017\n", "stage before @seed"),
    # the dry section and the MARK room fields (5th element: bench_test.lua's room
    # API mode, default 'present')
    ("bench-save.txt", "@marker DRY START\n", "", "no DRY START marker (P5 would end at P6's room)"),
    ("bench-save.txt", "@marker DRY START\n@room 3\n", "@marker DRY START\n",
     "P5 not ended by @room 3 (its old end)"),
    ("bench-save.txt", "@wait 30\nstage 1\n", "@wait 30\nstage 1c\n", "dry section on Downpour"),
    ("main.lua", ", census(), roomFields()))", ", census()))", "MARK without the room fields"),
    ("main.lua", 'field("HasWater")', 'field("GetWaterAmount")', "Room:GetWaterAmount called"),
    ("main.lua", "        local ok, v = pcall(fn, room)\n", "        local ok, v = true, fn(room)\n",
     "room binding called without pcall", "error"),
    # a raising member lookup, and an
    # integral float with no integer representation ('%d' raises in Lua 5.3)
    ("main.lua", "        local okFn, fn = pcall(function() return room[name] end)\n",
     "        local okFn, fn = true, room[name]\n", "room member lookup without pcall", "lookup"),
    ("main.lua", "            local okFmt, s = pcall(string.format, \"%d\", v)\n",
     "            local okFmt, s = true, string.format(\"%d\", v)\n", "'%d' format without pcall", "huge"),
]
# (seed mode, room API mode) runs of bench_test.lua: the three @seed modes with the 1.7.9b room
# bindings, and the room bindings absent / raising / unreachable by lookup / returning 2^63
LUA_RUNS = (("set", "present"), ("stuck", "present"), ("absent", "present"), ("set", "absent"), ("set", "error"),
            ("set", "lookup"), ("set", "huge"))


def run_lua(main_path: Path, save_path: Path, mode: str, api: str = "present") -> tuple[bool, str]:
    import lupa.lua53 as L
    r = L.LuaRuntime()
    r.globals().arg = r.table_from({1: str(main_path), 2: str(save_path), 3: mode, 4: api})
    try:
        r.execute((HERE / "bench_test.lua").read_text(encoding="utf-8"))
        return True, "PASS"
    except Exception as exc:  # a failed assert in the Lua test
        return False, str(exc).splitlines()[0][:200]


def leg_lua() -> None:
    try:
        import lupa.lua53  # noqa: F401
    except ImportError:
        print("lua: SKIP (pip install lupa to run bench_test.lua)")
        return
    for mode, api in LUA_RUNS:
        ok, msg = run_lua(HERE / "main.lua", HERE / "bench-save.txt", mode, api)
        check(f"lua {mode} (room api {api})", ok, msg)
    killed = 0
    with tempfile.TemporaryDirectory() as td:
        for fname, old, new, what, *api in LUA_MUTANTS:
            for f in ("main.lua", "bench-save.txt"):
                shutil.copy(HERE / f, Path(td) / f)
            p = Path(td) / fname
            text = p.read_text(encoding="utf-8")
            check(f"lua mutant '{what}' applies", text.count(old) == 1, "anchor not unique")
            p.write_text(text.replace(old, new), encoding="utf-8")
            ok, _ = run_lua(Path(td) / "main.lua", Path(td) / "bench-save.txt", "set", api[0] if api else "present")
            check(f"lua mutant '{what}' killed", not ok, "the Lua test passed a broken script")
            killed += not ok
    print(f"lua: {len(LUA_RUNS)} runs (3 seed modes, room API present/absent/error/lookup/huge), "
          f"mutants {killed}/{len(LUA_MUTANTS)} killed")


# ----------------------------------------------------------------------------------- smoke
# EID's load banner and its first Lua heap line: bench-D4A4Pb-20261026/game.log:55-58 verbatim
# (leg_lua_health compares them with the file when --smoke-root holds it).  Without them every
# fixture run is EID_NOT_LOADED / EID_HEAP_MISSING.
EID_RULE = "[INFO] - Lua Debug: " + "~" * 50
EID_LOADED = [EID_RULE, "[INFO] - Lua Debug: External Item Descriptions v5.23_bc0551a loaded.", EID_RULE,
              "[INFO] - Lua mem usage: 11215 KB and 166 bytes"]
# The failing run's EID block, bench-D4A4Pc-20261026/game.log:55-61 verbatim (line 56 starts with a TAB).
EID_FAIL = [
    "[INFO] - Lua Debug: EID ERROR: Loading Languagepack 'cs_cz' (ab+) failed: error loading module "
    "'descriptions.ab+.cs_cz' from file 'ux0:data/isaacr001/mods/836319872/descriptions/ab+/cs_cz.lua':",
    "\tcannot read ux0:/data/isaacr001/mods/836319872/descriptions/ab+/cs_cz.lua: Success",
    "[INFO] - Lua Debug: EID ERROR: Loading Languagepack 'cs_cz' (modular) failed: "
    ".../isaacr001/mods/836319872/descriptions/modular/cs_cz.lua:3: attempt to index a nil value (field '?')",
    "[INFO] - Lua Debug: EID ERROR: Loading Languagepack 'cs_cz' (rep) failed: "
    "...data/isaacr001/mods/836319872/descriptions/rep/cs_cz.lua:442: attempt to index a nil value (field '?')",
    "[INFO] - Lua Debug: EID ERROR: Loading Languagepack 'cs_cz' (modular) failed: "
    ".../isaacr001/mods/836319872/descriptions/modular/cs_cz.lua:3: attempt to index a nil value (field '?')",
    "[INFO] - ERR: ux0:/data/isaacr001/mods/836319872/features/eid_mcm.lua:81: attempt to index a nil value "
    "(field '?')",
    "[INFO] - There was an error running the lua file: ux0:data/isaacr001/mods/836319872/main.lua",
]
# Verbatim lines of the downloaded smoke logs (bench-smoke*-20261017-* under --smoke-root).
S_BUILD = "[0.307 thr 0x40010003] build rel:v0.1.2-alpha-bench-p2b2-20261017; guest base=0x98000000; softfp ARMv7"
S_ARGS = ("[4.533 thr 0x40010003] ISAAC VITA LAUNCH ARGS: path=ux0:/data/isaacr001/launch-args.txt status=ok "
          "io=0x00000000 bytes=110 tokens=2 argc=3 cmdline=isaac-ng.exe --set-stage=1 --set-stage-type=4")
S_SEAM = ("[8.864 thr 0x40010003] [isaac-lua] getclass seam: getClass@003f8e30 getExact@003f8c80 identity=98804140 "
          "strings=9874f2b4,9874f324 pins=ok max_hops=16 verify=0 build=rel:v0.1.2-alpha-bench-p2b2-20261017")
S2_LAST = ("[14.277 thr 0x40010003] KAGE VITA INFLATE MEMO: blocks=3840 fixed_hits=0 dyn_hits=0 dyn_miss=1949 "
           "build_us_saved~=0 stored=1890 fixed=0 dyn=1950 dyn_stores=182 dyn_off=0 slots=16 cal(fb,fc,db,dc)=0,0,")
S3_ENODEV = ("[15.110 thr 0x40010003] archive cursor diag: version=1 anomaly=cache-reset seq=120228 key=repentance.a "
             "token=0x00000000 requested=0x00000000 origin=0 result=-1 actual=0xffffffff errno=19")
S3_LAST = ("[19.174 thr 0x40010003] KAGE VITA INFLATE MEMO: blocks=5632 fixed_hits=0 dyn_hits=91 dyn_miss=2743 "
           "build_us_saved~=2203 stored=2797 fixed=1 dyn=2834 dyn_stores=314 dyn_off=0 slots=16 cal(fb,fc,db,dc)=")
S1_NATIVE = [
    S_BUILD, S_ARGS,
    "[kage-vita] ph120.s bid=rel:v0.1.2-alpha-bench-p2b2-2026 win=1 loops=120 at=0000000001fd14d4 "
    "cad(w,s,d,m,g,r,b,p)=120,120,120,120,60,82,82,82 phase(f,n,i)=60,60,60 skip(a,l,n,f)=38,21,17,3",
    "[kage-vita] roomlog bid=rel:v0.1.2-alpha-bench-p2b2-2026 report_win=1 epoch=1 seq=1 at=00000000:01b04621 "
    "assigned=1 win=1 loop=1 pair=1,2 sat=0",
    "[kage-vita] ph120.t bid=rel:v0.1.2-alpha-bench-p2b2-2026 win=103 loops=12360 us(50,95,max) svc=45/489/6063 "
    "upd=1183/3733/9376 rnd=8435/9138/9756 swp=154/187/206 lim=0/0/0 all=16730/18978/24917 "
    "gap(n,min,50,95,max)=120/9553/16737/18901/24953 bad=0 clamp=0",
    "[240.914 thr 0x40010003] KAGE VITA (end of the smoke1 excerpt)",
]
S1_GAME = [
    "[INFO] - Running Lua Script: resources/scripts/enums.lua",
    "[INFO] - Running Lua Script: resources/scripts/main.lua",
    "[INFO] - LOADED MOD ux0:data/isaacr001/mods/836319872/content/",
    "[INFO] - LOADED MOD ux0:data/isaacr001/mods/repro_console/content/",
    "[INFO] - LOADED MOD ux0:data/isaacr001/mods/repro_tour/content/",
    "[INFO] - Running Lua Script: ux0:data/isaacr001/mods/836319872/main.lua",
    # bench-smoke-20261017-052919/run1-game.log:55-58 (the banner and EID's heap line; the same text
    # as EID_LOADED, checked against the file by leg_lua_health)
    *EID_LOADED,
    "[INFO] - Running Lua Script: ux0:data/isaacr001/mods/repro_console/main.lua",
    "[INFO] - Room 1.2(Start Room)",
    "[INFO] - Lua Debug: [repro_console] save data: 915 bytes, 52 command(s), once=false, continued=false",
    "[INFO] - Lua Debug: [repro_console] MARK NONCE 41f18707bf9f | greed=false difficulty=0 stage=1 stagetype=4 "
    "level=Downpour I roomtype=1 roomidx=84 frame=0 time=30387 | entities=1",
    "[INFO] - Lua Debug: [repro_console] seed Q9YH CA4F -> BVPF Q3ET: restarting the run (1)",
    "[INFO] - Lua Debug: [repro_console] exec [start]: seed BVPF Q3ET",
    "[INFO] - Lua Debug: [repro_console] result [seed BVPF Q3ET]: ",
]
S23_GAME = S1_GAME[:5]
SMOKES = [  # (name, native lines, game lines, nonce, expected FAIL codes, check)
    ("smoke1", S1_NATIVE, S1_GAME, "41f18707bf9f", {"PRE_SLOT_EBOOT", "SEED_NOT_TAKEN", "INCOMPLETE", "NO_PHASES"}),
    ("smoke2", [S_BUILD, S_ARGS, S_SEAM, S2_LAST], S23_GAME, "9d7c1b2a0e11",
     {"STOPPED_IN_BOOT", "PRE_SLOT_EBOOT", "NONCE"}),
    ("smoke3", [S_BUILD, S_ARGS, S_SEAM, S3_ENODEV, S3_LAST], S23_GAME, "9d7c1b2a0e11",
     {"STOPPED_IN_BOOT", "SUSPEND_ENODEV", "PRE_SLOT_EBOOT", "NONCE"}),
]
SMOKE_DIRS = {"smoke1": "bench-smoke-20261017-052919", "smoke2": "bench-smoke2-20261017-062435",
              "smoke3": "bench-smoke3-20261017-154314"}
SMOKE_STOP = {"smoke2": 14.277, "smoke3": 19.174}


def nonce_of(save: Path) -> str | None:
    for line in save.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("@marker NONCE "):
            return line.split()[2]
    return None


def leg_smoke(root: Path | None) -> None:
    for name, nat, game, nonce, want in SMOKES:
        res = b.diagnose("\n".join(nat) + "\n", "\n".join(game) + "\n", nonce)
        check(f"{name} excerpt codes", codes(res) == want, f"{sorted(codes(res))} != {sorted(want)}")
        check(f"{name} excerpt verdict", not res["ok"] and res["verdict"].startswith("BENCH FAILED"), res["verdict"])
    full = 0
    if root and root.is_dir():
        for name, _, _, _, want in SMOKES:
            d = root / SMOKE_DIRS[name]
            if not (d / "run1-native.log").exists():
                continue
            nonce = nonce_of(d / "run1-bench-save.dat")
            res = b.diagnose((d / "run1-native.log").read_text(encoding="utf-8", errors="replace"),
                             (d / "run1-game.log").read_text(encoding="utf-8", errors="replace"), nonce)
            check(f"{name} full-log codes", codes(res) == want, f"{sorted(codes(res))} != {sorted(want)}")
            first = min(want, key=b.PRIORITY.index)
            check(f"{name} full-log verdict names {first} first", f"({first}" in res["verdict"], res["verdict"])
            if name in SMOKE_STOP:
                check(f"{name} stop time", abs(res["native_last_s"] - SMOKE_STOP[name]) < 1e-6, res["native_last_s"])
            if name == "smoke1":
                check("smoke1 windows", res["windows"] == 103, res["windows"])
            full += 1
            print(f"  {name} full log: {res['verdict'][:140]}")
    print(f"smoke: 3 excerpts, {full} full logs" + ("" if full else " (full logs SKIP: no --smoke-root / "
                                                              "BENCH_SMOKE_ROOT, or the smoke runs are not in it)"))


# ----------------------------------------------------------------------------------- phases
BID = "rel:v0.1.2-alpha-bench-slot-2026"
BUILD = "[0.306 thr 0x40010003] build rel:v0.1.2-alpha-bench-slot-20261025; guest base=0x98000000; softfp ARMv7"
ARGS_OK = ("[4.533 thr 0x40010003] ISAAC VITA LAUNCH ARGS: path=ux0:/data/isaacr001/launch-args.txt status=ok "
           "io=0x00000000 bytes=402 tokens=3 slot=2 argc=3 cmdline=isaac-ng.exe --set-stage=1 --set-stage-type=4")
APPLIED = ("[6.100 thr 0x40010003] ISAAC VITA LAUNCH SAVE SLOT: event=applied slot=2 was=0x00000000 "
           "manager=0x9a001000 waited=4 trigger=ux0:/data/isaacr001/resources/stringtable.sta")
# p2b-native.log window 41, GPU oracle scenario A,
# glthread-ab2 window 2: the record shapes the analyzer must read.
W41 = [
    "[kage-vita] ph120.t bid=rel:v0.1.2-alpha-int4-session-p2 win=41 loops=4920 us(50,95,max) svc=50/972/9552 "
    "upd=4139/19399/99061 rnd=16296/20878/22073 swp=161/184/197 lim=0/0/0 all=12539/43156/99201 "
    "gap(n,min,50,95,max)=57/25159/34774/63544/154542 bad=0 clamp=0",
    "[kage-vita] ph120.hs bid=rel:v0.1.2-alpha-int4-session-p2 win=41 loops=4920 period_us=1000 "
    "samples(svc,upd,rnd,swp,lim,oth,pace)=46,653,944,12,0,432,70 total=2157 nomark=1 badesp=0 full=0 "
    "words=40976 deep=337 ticks=131775 spin=0",
    "[kage-vita] ph120.c bid=rel:v0.1.2-alpha-int4-session-p2 win=41 loops=4920 n(s,u,r,p,l,w)=120,120,57,57,0,120 "
    "g(c,l,i)=1080092,522469,0 gl(d,c,t,p,i,s,u,a,v,f,x)=4876,422,4881,1510,2,0,5284,35461,9996,693,2008 drop=0",
    "[kage-vita] ph120.s bid=rel:v0.1.2-alpha-int4-session-p2 win=41 loops=4920 at=000000000821ca9c "
    "cad(w,s,d,m,g,r,b,p)=120,120,120,120,60,57,57,57 phase(f,n,i)=60,60,60 skip(a,l,n,f)=63,6,57,4 "
    "viol(d,p,g,s,o)=0,0,0,0,0 reset=0 drop(t,us)=5,83334 wait(c,us)=67,384122 debt=376/82921",
    "[kage-vita] ph120.gpu bid=oracle win=41 loops=120 period_us=500 poll(n,late)=4203,0 wall=2101000 busy=990000 "
    "rwall=990000 rbusy=870000 f=60 gf(50,95,max)=16500,16500,16500 lag(n,50,95,max,lost)=60,2400,2400,2400,0 "
    "q(0,1,2)=60,0,0 swp(q2,oth)=0,6000 pend(max)=1",
    "[kage-vita] ph120.rt bid=rel:v0.1.2-alpha-int4-glthread-a win=2 loops=240 wall=1999584 busy=92870 f(n,o)=120,0 "
    "fr(50,95,max)=768,838,908 sw=12378 hw=20800 r=48721 s(n,w,us)=0,0,0 h(n,us)=0,0 c(n,us)=1,666 rf=0",
    "[kage-vita] ph120.rw bid=rel:v0.1.2-alpha-int4-glthread-a win=2 loops=240 wk=108 sl(n,t)=952,158 "
    "mb(n,s,us)=20040,313,578 ap(s,us)=760,1255 cb=1274528 mq=3240",
    # S6 OBSERVE shape (kage_vita_phase_profile.c kage_vita_profile_log_clear_latch): n = h + f, clk = tgt
    "[kage-vita] ph120.a bid=oracle win=41 loops=4920 loc(a,u,h,m)=0,0,0,0 tgt=21400",
    "[kage-vita] ph120.tl bid=oracle win=41 loops=4920 mode=1 site(n,h,f,s)=17000,16150,850,0 "
    "obs(t,x,d,w)=900,12,0,0 clk=21400",
]
ROOMS_RESTART = [(28, 2), (32, 2), (36, 2), (38, 60), (40, 2), (60, 85), (80, 70), (100, 60), (103, 71),
                 (106, 85), (109, 100), (112, 60), (115, 70), (118, 2), (128, 71)]  # (s, room), seed restart
# In-place seed: no restart, so one start room fewer before BENCH START (boot, then
# the "stage 1c" rebuild); the phase rooms and their times are the same.
ROOMS = [ROOMS_RESTART[0]] + ROOMS_RESTART[2:]
# BVPF Q3ET = Seeds.Seed2String(20261017) on the device; its Downpour I (stage 1, type 4) seed,
# from the transcribed arithmetic that leg_seed checks against six device pairs.
WANT_SEED, WANT_LEVEL = "BVPF Q3ET", 1028899765


def window_lines(k: int) -> list[str]:
    import re
    out = []
    for line in W41:
        line = re.sub(r"bid=\S+", f"bid={BID}", line)
        line = re.sub(r"\bwin=\d+", f"win={k}", line)
        line = re.sub(r"\bat=[0-9a-f]{16}", f"at={30_000_000 + 2_000_000 * k:016x}", line)
        out.append(line)
    return out


def good_native(rooms=None, windows: int = 55) -> list[str]:
    rooms = ROOMS if rooms is None else rooms
    lines = [BUILD, ARGS_OK, APPLIED]
    for k in range(1, windows + 1):
        lines += window_lines(k)
    for seq, (t, r) in enumerate(rooms, 1):
        lines.append(f"[kage-vita] roomlog bid={BID} report_win=1 epoch=1 seq={seq} at=00000000:{t * 1_000_000:08x} "
                     f"assigned=1 win=1 loop=1 pair=1,{r} sat=0")
    lines.append(f"[kage-vita] ph120.rl bid={BID} win=55 loops=6600 epoch=1 n=0 seq=15 drop=0 reset_lost=0 sat=0")
    return lines


def rc(msg: str) -> str:
    return f"[INFO] - Lua Debug: [repro_console] {msg}"


GAME_HEAD = ["[INFO] - Running Lua Script: resources/scripts/enums.lua",
             "[INFO] - Running Lua Script: resources/scripts/main.lua",
             "[INFO] - Running Lua Script: ux0:data/isaacr001/mods/836319872/main.lua",
             *EID_LOADED,
             "[INFO] - Running Lua Script: ux0:data/isaacr001/mods/repro_console/main.lua"]


def room_lines(rooms):
    room = iter(rooms)

    def r():
        _, n = next(room)
        names = {2: "Start Room"}
        return f"[INFO] - Room 1.{n}({names.get(n, 'New Room')})"
    return r


def bench_tail(r, nonce: str) -> list[str]:
    return [r(), rc("MARK BENCH START | x"), r(), rc("MARK P1 start-idle | x"), r(),
            rc("MARK P2 fight-maxroom | x"), r(), rc("MARK P3 spawn-heavy | x"), r(),
            rc("MARK P4 transitions | x"), r(), r(), r(), r(), r(), r(), rc("MARK P5 end-idle | x"), r(),
            rc("MARK BENCH DONE | x"), r(), rc("remove [start]: save data removed, HasModData=false")]


def good_game(nonce: str, level_seed: int = WANT_LEVEL) -> list[str]:
    """The in-place seed flow of main.lua, shaped like the 2026-10-25 device logs:
    one game start on the random boot seed (its Level::Init first), the seed set in place, then the
    "stage 1c" rebuild with the seed's stage seed.  A --set-stage start prints no 'RNG Start Seed'
    line (only Game::Start, NetStart, the daily, rerun and continue starts do)."""
    r = room_lines(ROOMS)
    g = list(GAME_HEAD)
    g += ["[INFO] - Level::Init m_Stage 1, m_StageType 4 Seed 1258606820", r(),
          rc("game start 1: seed=BS8Y SWXQ continued=false"), rc("save data: 1530 bytes"),
          rc(f"MARK NONCE {nonce} | x"), rc("wait 30 update(s) [start]: 54 command(s) deferred"),
          rc(f"seed BS8Y SWXQ -> {WANT_SEED}: set in place (SetStartSeed) [start]; the next stage command "
             "builds the floor from it"),
          rc("exec [start]: stage 1c"),
          f"[INFO] - Level::Init m_Stage 1, m_StageType 4 Seed {level_seed}"]
    g += bench_tail(r, nonce)
    return g


def good_game_restart(nonce: str) -> list[str]:
    """The older restart flow (hypothetical success: on a --set-stage boot it never took)."""
    r = room_lines(ROOMS_RESTART)
    g = list(GAME_HEAD)
    g += [r(), rc("game start 1: seed=AAAA AAAA continued=false seed_restarts=0"), rc("save data: 1530 bytes"),
          rc(f"MARK NONCE {nonce} | x"), rc("seed AAAA AAAA -> BBBB BBBB: restarting the run (1)"),
          "[INFO] - RNG Start Seed: BBBB BBBB (2932482944) [New, 2]",
          r(), rc("game start 2: seed=BBBB BBBB continued=false seed_restarts=1"), rc("save data: 1530 bytes"),
          rc(f"MARK NONCE {nonce} | x"), rc("seed BBBB BBBB active (restarts=1)"),
          r(), rc("MARK BENCH START | x"), r(), rc("MARK P1 start-idle | x"), r(),
          rc("MARK P2 fight-maxroom | x"), r(), rc("MARK P3 spawn-heavy | x"), r(),
          rc("MARK P4 transitions | x"), r(), r(), r(), r(), r(), r(), rc("MARK P5 end-idle | x"), r(),
          rc("MARK BENCH DONE | x"), r(), rc("remove [start]: save data removed, HasModData=false")]
    return g


def near(a, x, eps=1e-6):
    return a is not None and abs(a - x) < eps


def leg_phases() -> None:
    nonce = "0123456789ab"
    res = b.diagnose("\n".join(good_native()) + "\n", "\n".join(good_game(nonce)) + "\n", nonce)
    check("good run ok", res["ok"], res["verdict"] + " " + str([x for x in res["diagnostics"] if x[0] != "OK"]))
    check("good run OK codes", {"SLOT_APPLIED", "SEED"} <= codes(res, "OK"), codes(res, "OK"))
    # the fixture's native log has no 'xinput snapshot' line (it predates them), so the
    # completed run WARNs INTERCEPT_UNSEEN; with the harness kill's intercept, as every real run logs
    # it (INTERCEPT_LINES[2]), no WARN at all
    check("good run: only WARN INTERCEPT_UNSEEN (no intercept line in the fixture)",
          codes(res, "WARN") == {"INTERCEPT_UNSEEN"}, codes(res, "WARN"))
    killed = b.diagnose("\n".join(good_native() + INTERCEPT_LINES[2:]) + "\n", "\n".join(good_game(nonce)) + "\n",
                        nonce)
    check("good run with the harness kill's intercept: ok, no WARN", killed["ok"] and not codes(killed, "WARN"),
          codes(killed, "WARN"))
    old = b.diagnose("\n".join(good_native(ROOMS_RESTART)) + "\n", "\n".join(good_game_restart(nonce)) + "\n", nonce)
    check("restart-format run ok", old["ok"] and {"SLOT_APPLIED", "RNG_SLOT", "SEED"} <= codes(old, "OK"),
          old["verdict"] + " " + str(codes(old, "OK")))
    check("restart-format phases = in-place phases",
          [(r["phase"], r["windows"]) for r in old["phases"]] == [(r["phase"], r["windows"]) for r in res["phases"]],
          str(old["phases"])[:200])
    rows = {r["phase"]: r for r in res["phases"]}
    check("phase names", list(rows) == ["P1 start-idle", "P2 fight-maxroom", "P3 spawn-heavy", "P4 transitions",
                                        "P5 end-idle"], list(rows))
    want_w = {"P1 start-idle": 9, "P2 fight-maxroom": 9, "P3 spawn-heavy": 9, "P4 transitions": 9, "P5 end-idle": 4}
    for p, n in want_w.items():
        check(f"{p} windows", rows.get(p, {}).get("windows") == n, rows.get(p, {}).get("windows"))
    r = rows.get("P2 fight-maxroom", {})
    # hand values from window 41: gap p50 34774 us, hs upd 653 / g 60, rnd 944 / r 57, wait 384122 us / 60
    exp = {"fps": 1e6 / 34774, "gap50": 34.774, "gap95": 63.544, "gapmax": 154.542, "ups": 30.0, "rps": 28.5,
           "U": 653 / 60, "R": 944 / 57, "sleep_pu": 384.122 / 60, "upd50": 4.139, "rnd50": 16.296,
           "rnd95": 20.878, "busy_pct": 100 * (2157 - 384.122) / 2157,
           # GPU oracle line: busy 990000 us / f 60, rbusy 870000 / 60, (rwall - rbusy) / 60
           "gpu_pf": 16.5, "gf50": 16.5, "gf95": 16.5, "rbusy_pf": 14.5, "rcpu_pf": 2.0, "lag50": 2.4,
           "gpu_pct": 100 * 990000 / 2101000, "swpoth_pf": 0.1,
           # ab2 window 2: busy 92870 / wall 1999584, fr 768/838/908, sw 12378 / f 120, r 48721 / 120,
           # wk 108 / 120, mq 3240 / mb.n 20040, producer = ap.us/ap.s + mb.us/mb.s * mb.n / r
           "rt_busy_pct": 100 * 92870 / 1999584, "fr50": 0.768, "fr95": 0.838, "frmax": 0.908,
           "sw_pf": 12378 / 120 / 1e3, "rec_pf": 48721 / 120, "wk_pf": 108 / 120, "mq_share": 3240 / 20040,
           "prod_us": 1255 / 760 + (578 / 313) * 20040 / 48721}
    for k, v in exp.items():
        check(f"P2 {k}", near(r.get(k), v), f"{r.get(k)} != {v}")
    # S6 latch: 9 windows of the W41 ph120.a/ph120.tl pair, r = 57 per window
    tl_exp = {"tgt_pr": 21400 / 57, "tl_n_pr": 17000 / 57, "tl_h_pr": 16150 / 57, "tl_clk_pr": 21400 / 57,
              "tl_hit_pct": 100 * 16150 / 17000, "tl_n": 9 * 17000, "tl_f": 9 * 850, "tl_x": 9 * 12,
              "tl_t": 9 * 900, "tl_d": 0, "tl_s": 0, "tl_w": 0, "tl_mode": 1, "tl_windows": 9}
    for k, v in tl_exp.items():
        check(f"P2 {k}", near(r.get(k), v), f"{r.get(k)} != {v}")
    check("P2 latch verdict", r.get("tl_verdict", "").startswith("OBSERVE: would-be hits h=145350 of n=153000 site calls (95.0%), 283.3/render")
          and "d=0: ON would take the same expiry decisions" in r.get("tl_verdict", ""), r.get("tl_verdict"))
    L = b.latch_decision
    base = {k: r[k] for k in r if k.startswith("tl_")}
    for name, patch, want in (
            ("tripwire s", {"tl_s": 1}, "TRIPWIRE s=1 w=0"),
            ("tripwire w", {"tl_w": 2}, "TRIPWIRE s=0 w=2"),
            ("n != h + f", {"tl_f": 0}, "PROBE n != h + f"),
            ("observe clk != tgt", {"tl_clk": 1}, "PROBE clk 1 != tgt 192600"),
            ("observe d", {"tl_d": 3}, "d=3: ON would move 3 expiry decision(s)"),
            ("on identity", {"tl_mode": 2, "tl_t": 0, "tl_clk": 192600 - 145350}, "ON: latch hits h=145350 of n=153000"),
            ("on clk", {"tl_mode": 2, "tl_t": 0}, "PROBE clk 192600 != tgt - h 47250"),
            ("on t", {"tl_mode": 2, "tl_clk": 47250}, "PROBE t=8100 d=0 in ON"),
            ("mixed modes", {"tl_mode": None, "tl_modes": [1, 2]}, "PROBE mixed latch modes [1, 2]"),
            ("no tgt", {"tl_tgt": None, "tl_clk": 5}, "OBSERVE: would-be hits h=145350")):
        got = L({**base, **patch})
        check(f"latch rule {name}", want in got and ("PROBE clk" not in got or "clk" in want), got)
    check("latch rule no tl", L({}) == "", L({}))
    check("P2 GPU verdict", r.get("gpu_verdict", "").startswith("GPU at the 60-fps edge")
          and "rbusy/f > 1 ms" in r.get("gpu_verdict", ""), r.get("gpu_verdict"))
    rep = b.report(res)
    check("report answer first", rep.startswith("BENCH OK: 5 phases"), rep[:80])
    for head in ("frame times per phase", "GPU per phase", "GL render thread per phase",
                 "Clear expiry clock latch per phase", "P2 fight-maxroom       latch: OBSERVE"):
        check(f"report has '{head}'", head in rep, "")
    # FRAME-ANATOMY decision rule branches
    D = b.gpu_decision
    check("rule gf95<=14", D({"gf50": 9.0, "gf95": 14.0}).startswith("GPU not the limit"), D({"gf50": 9.0, "gf95": 14.0}))
    check("rule gf50>17", D({"gf50": 18.0, "gf95": 19.0}).startswith("GPU-bound"), "")
    check("rule spikes", D({"gf50": 12.0, "gf95": 20.0}).startswith("GPU p95 above 14"), "")
    check("rule queue", "display queue" in D({"gf50": 9.0, "gf95": 10.0, "q2": 5, "swpq2_pf": 3.0, "swpoth_pf": 0.5}), "")
    check("rule no gpu", D({}).startswith("no ph120.gpu"), "")
    # compare
    txt = b.compare(res, res)
    check("compare", "fps 28.76->28.76 (+0.00)" in txt, txt[:200])
    check("compare rooms identical", f"rooms: identical (floor seed {WANT_LEVEL}, 13 room loads)" in txt, txt[:200])
    print(f"phases: {len(rows)} phases, {len(exp)} hand-checked P2 values")


# ------------------------------------------------------------------------------------ census
# ph120.ro/rop (ISAAC_VITA_GL_RECORD_OPCODE_CENSUS) on top of W41, consistent with the same
# take's rt (r=48721, s(n)=0, f=120) and rw (ap(s)=760; ck=760 added: 1 us per clock read);
# ck16=16 (1 us on the render thread).  Hand-built so every estimator branch has one known
# answer, per present = per window / 120:
#   1  glActiveTexture n 29900: 2 us replays (32 exact + 467 random, +1 us clock each), 480
#      unstamped game samples of 1.5 us (+1 us clock)            -> rnd 59800, game 44850
#   24 glDrawElements  n 18000: 10 us replays (32 + 280), 280 game samples, all stamped at the
#      admission check (+2 us clock) of 4 us                      -> rnd 180000, game 72000
#   59 glUniform4fv    n 100: 32 exact replays of 5 us, no random one, no game sample
#                                                                 -> rnd 500, flags "x-"
#   78 WORKER_CALL     n 721: every closure exact, 100 us; sy 2, sw 1, 666 us -> rnd 72100
CENSUS_SLOTS = {
    1: (29900, 119600, 32, 96, 467, 1401, 480, 0, 1200, 0, 0, 0),
    24: (18000, 108000000, 32, 352, 280, 3080, 280, 280, 1680, 0, 0, 0),
    59: (100, 3200, 32, 192, 0, 0, 0, 0, 0, 0, 0, 0),
    78: (721, 5768, 721, 72821, 0, 0, 0, 0, 0, 2, 1, 666),
}


def census_lines(k: int, slots=None, drop_part=None) -> list[str]:
    slots = CENSUS_SLOTS if slots is None else slots
    items = [f"{op}:" + ",".join(map(str, v)) for op, v in sorted(slots.items())]
    parts = [items[i:i + 3] for i in range(0, len(items), 3)]
    out = [f"[kage-vita] ph120.ro bid={BID} win={k} loops={120 * k} ops={len(items)} lines={len(parts)} "
           "k=64 x=32 ck16=16 f=n,b,e,eus,rs,rus,ps,pst,pus,sy,sw,swus"]
    for i, part in enumerate(parts):
        if i != drop_part:
            out.append(f"[kage-vita] ph120.rop bid={BID} win={k} part={i} op=" + ";".join(part))
    return out


def census_native(slots=None, bad_window=None, slots_bad=None, drop_part=None) -> str:
    out = []
    for line in good_native():
        if " ph120.rw " in line:
            line = line.replace(" mq=3240", " mq=3240 ck=760")
        out.append(line)
        if " ph120.rw " in line:
            k = int(line.split(" win=")[1].split()[0])
            if k == bad_window:
                out += census_lines(k, slots_bad, drop_part)
            else:
                out += census_lines(k, slots)
    return "\n".join(out) + "\n"


def rows_of(res: dict) -> dict:
    return {r["phase"]: r for r in res["phases"]}


# GL admit wait (ISAAC_VITA_GL_THREAD_ADMIT_WAIT): one synthetic ph120.aw per window, printed after
# ph120.rw as kage_vita_profile_log_gl_admit_wait does, with rs on rw (kage_vita_profile_log_render_wake).
# OBSERVE: rs = n = 20, fit 20,18; ON: n = 20 ok = 18 to = 1 nw = 1, rs = 2.  Also hl, ov, hd
# (kage_vita_profile_log_gl_admit_wait), and the mode from the bid-checked CADENCE30 banner.
AW_MODES = {
    "observe": {"rs": 20, "n": 20, "ok": 0, "to": 0, "nw": 0, "bo": 0, "tt": 0, "us": (0, 0), "lt": (0, 0),
                "hl": (0, 0), "ov": 0, "fit": (20, 18), "miss": (3000, 500), "hd": (0, 0)},
    "on": {"rs": 2, "n": 20, "ok": 18, "to": 1, "nw": 1, "bo": 0, "tt": 0, "us": (18000, 2000), "lt": (1, 40),
           "hl": (2, 70), "ov": 310, "fit": (1, 0), "miss": (800, 800), "hd": (3, 900)},
}
AW_BANNER = ("[1.000 thr 0x40010003] KAGE VITA CADENCE30: bid={bid} tick_units=50000 game_tick_units=100000 "
             "max_catchup=4 stale_catchup=1 max_full_skip=4 parity=4a264 frame=1a30dc reserve=pre-game"
             "{aw} fps_lock=60")


def aw_native(mode: str, bad_window=None, missing_window=None, banner_bid=BID, receipt=None,
              banner=True) -> str:
    v = AW_MODES[mode]
    token = {"observe": " gl-admit-wait=observe", "on": " gl-admit-wait=on backoff=30"}.get(mode, "")
    out = []
    for line in good_native():
        if " ph120.rw " in line:
            k = int(line.split(" win=")[1].split()[0])
            rs = v["rs"] + (1 if k == bad_window else 0)
            out.append(line.replace(" mq=3240", f" mq=3240 ck=760 rs={rs}"))
            if k != missing_window:
                out.append(f"[kage-vita] ph120.aw bid={BID} win={k} loops={120 * k} n={v['n']} ok={v['ok']} "
                           f"to={v['to']} nw={v['nw']} bo={v['bo']} tt={v['tt']} us(sum,max)={v['us'][0]},"
                           f"{v['us'][1]} lt(c,us)={v['lt'][0]},{v['lt'][1]} hl(c,us)={v['hl'][0]},"
                           f"{v['hl'][1]} ov={v['ov']} fit(e,a)={v['fit'][0]},{v['fit'][1]} "
                           f"miss(us,max)={v['miss'][0]},{v['miss'][1]} hd(n,us)={v['hd'][0]},{v['hd'][1]}")
            continue
        out.append(line)
        if line is APPLIED:
            if banner:
                out.append(AW_BANNER.format(bid=banner_bid, aw=token))
            out.append(f"[6.200 thr 0x40010003] KAGE VITA GL ADMIT WAIT: mode={receipt or mode} "
                       "bound=time-test slice_us=2000 backoff=30 record=ph120.aw")
    return "\n".join(out) + "\n"


def leg_admit_wait() -> None:
    nonce = "0123456789ab"
    game = "\n".join(good_game(nonce)) + "\n"
    plain = rows_of(b.diagnose("\n".join(good_native()) + "\n", game, nonce))
    check("admit wait: no ph120.aw -> no aw columns", all("aw_n" not in r for r in plain.values()),
          str([k for r in plain.values() for k in r if k.startswith("aw_")]))
    nat0 = b.parse_native(aw_native("on"))
    pw, _ = b.phase_windows(nat0, b.parse_game(game))
    win = dict((name, sel) for name, sel, _ in pw)["P2 fight-maxroom"][0]
    for mode, v in AW_MODES.items():
        res = b.diagnose(aw_native(mode), game, nonce)
        check(f"admit wait {mode}: run ok, mode parsed", res["ok"] and res.get("aw_mode") == mode,
              f"{res['verdict']} {res.get('aw_mode')}")
        r = rows_of(res)["P2 fight-maxroom"]
        nw = r.get("windows")
        exp = {"aw_windows": nw, "aw_n": nw * v["n"], "aw_ok": nw * v["ok"], "aw_to": nw * v["to"],
               "aw_nw": nw * v["nw"], "aw_bo": nw * v["bo"], "aw_tt": nw * v["tt"], "aw_lt": nw * v["lt"][0],
               "fit_e": nw * v["fit"][0], "fit_a": nw * v["fit"][1], "aw_ms": nw * v["us"][0] / 1e3,
               "aw_max": v["us"][1] / 1e3, "aw_lt_ms": nw * v["lt"][1] / 1e3,
               "aw_hl": nw * v["hl"][0], "aw_hl_ms": nw * v["hl"][1] / 1e3, "aw_ov": v["ov"] / 1e3,
               "aw_hd": nw * v["hd"][0], "aw_hd_ms": nw * v["hd"][1] / 1e3,
               "miss_ms": nw * v["miss"][0] / 1e3, "aw_bad": 0, "aw_missing": 0,
               "refused": nw * v["rs"]}
        for k, want in exp.items():
            check(f"admit wait {mode}: P2 {k}", near(r.get(k), want), f"{r.get(k)} != {want}")
        text = b.report(res)
        check(f"admit wait {mode}: report table and build line",
              "GL admit wait per phase (ph120.aw, gl-admit-wait " + mode in text
              and f"; gl-admit-wait {mode}" in text and "aw_missing" in text and "aw_hd_ms" in text
              and "disagrees with the banner" not in text, text[-400:])
        # The mode comes from the bid-checked banner, never from the bid-less receipt.
        other = "on" if mode == "observe" else "observe"
        stale = b.diagnose(aw_native(mode, banner_bid="rel:another-build-0000000000000000"), game, nonce)
        check(f"admit wait {mode}: banner of another bid -> mode unknown",
              stale.get("aw_mode") is None and "gl-admit-wait unknown" in b.report(stale),
              f"{stale.get('aw_mode')}")
        nob = b.diagnose(aw_native(mode, banner=False), game, nonce)
        check(f"admit wait {mode}: receipt alone -> mode unknown", nob.get("aw_mode") is None,
              f"{nob.get('aw_mode')}")
        dis = b.diagnose(aw_native(mode, receipt=other), game, nonce)
        check(f"admit wait {mode}: receipt {other} vs banner -> banner wins, disagreement reported",
              dis.get("aw_mode") == mode
              and f"GL ADMIT WAIT receipt mode={other} disagrees with the banner" in b.report(dis),
              f"{dis.get('aw_mode')} {dis.get('aw_receipt')}")
        bad = rows_of(b.diagnose(aw_native(mode, bad_window=win), game, nonce))
        check(f"admit wait {mode}: one mutated rs -> P2 aw_bad 1",
              bad["P2 fight-maxroom"].get("aw_bad") == 1
              and sum(x.get("aw_bad", 0) for x in bad.values()) == 1,
              str({p: x.get("aw_bad") for p, x in bad.items()}))
        miss = rows_of(b.diagnose(aw_native(mode, missing_window=win), game, nonce))["P2 fight-maxroom"]
        check(f"admit wait {mode}: one window without ph120.aw -> aw_missing 1",
              miss.get("aw_missing") == 1 and miss.get("aw_windows") == nw - 1,
              f"{miss.get('aw_missing')} {miss.get('aw_windows')}")
    # I2 and I3 on their own (I1 holds): ok + to + nw + bo + tt > n, and fit e past n - ok - tt - nw + 1
    for what, patch in (("I2", {"bo": 1, "rs": 2}), ("I3", {"fit": (3, 0)})):
        AW_MODES["probe"] = dict(AW_MODES["on"], **patch)
        try:
            r = rows_of(b.diagnose(aw_native("probe", bad_window=None), game, nonce))["P2 fight-maxroom"]
        finally:
            del AW_MODES["probe"]
        check(f"admit wait: {what} broken in every window -> aw_bad = windows", r.get("aw_bad") == r.get("windows"),
              f"{r.get('aw_bad')} {r.get('windows')}")


# GL admit behind (ISAAC_VITA_GL_THREAD_ADMIT_BEHIND): one synthetic ph120.ab + ph120.abp per window, printed
# after ph120.rw as kage_vita_profile_log_gl_admit_behind does, with rs = n - ok on rw.
# OBSERVE: n = 20 = st 12 + 5 + 3, ov 4, wa 13, rs 20; pm 20 checked, 9 done in time; wp 13, 8.
# ON: the same gates, ok 9, bo 2, tt 5, rs 11; pm 11 checked, 4 done; wp 2, 1; one hl, three hd.
AB_MODES = {
    "observe": {"rs": 20, "n": 20, "st": (12, 5, 3), "ov": 4, "wa": 13, "ok": 0, "bo": 0, "tt": 0,
                "ob": (30000, 90000), "hl": (0, 0), "hd": (0, 0), "pm": (20, 9, 33000, 4100),
                "wp": (13, 8, 9000, 2600), "phl": 6},
    "on": {"rs": 11, "n": 20, "st": (12, 5, 3), "ov": 4, "wa": 13, "ok": 9, "bo": 2, "tt": 5,
           "ob": (28000, 70000), "hl": (1, 350), "hd": (3, 1200), "pm": (11, 4, 21000, 5200),
           "wp": (2, 1, 700, 700), "phl": 2},
}
AB_BANNER = ("[1.000 thr 0x40010003] KAGE VITA CADENCE30: bid={bid} tick_units=50000 game_tick_units=100000 "
             "max_catchup=4 stale_catchup=1 max_full_skip=4 parity=4a264 frame=1a30dc reserve=pre-game"
             "{ab} fps_lock=60")


def ab_native(mode: str, bad_window=None, missing_window=None, missing="ab", banner_bid=BID,
              receipt=None, banner=True, bytes_=65536) -> str:
    v = AB_MODES[mode]
    token = f" gl-admit-behind={'on' if v['ok'] or mode == 'on' else 'observe'} bytes={bytes_} backoff=30"
    out = []
    for line in good_native():
        if " ph120.rw " in line:
            k = int(line.split(" win=")[1].split()[0])
            rs = v["rs"] + (1 if k == bad_window else 0)
            out.append(line.replace(" mq=3240", f" mq=3240 ck=760 rs={rs}"))
            if k != missing_window or missing != "ab":
                out.append(f"[kage-vita] ph120.ab bid={BID} win={k} loops={120 * k} n={v['n']} "
                           f"st(p,f,x)={v['st'][0]},{v['st'][1]},{v['st'][2]} ov={v['ov']} wa={v['wa']} "
                           f"ok={v['ok']} bo={v['bo']} tt={v['tt']} ob(50,max)={v['ob'][0]},{v['ob'][1]} "
                           f"hl(c,us)={v['hl'][0]},{v['hl'][1]} hd(n,us)={v['hd'][0]},{v['hd'][1]}")
            if k != missing_window or missing != "abp":
                out.append(f"[kage-vita] ph120.abp bid={BID} win={k} loops={120 * k} "
                           f"pm(c,ok,us,max)={','.join(map(str, v['pm']))} "
                           f"wp(c,ok,us,max)={','.join(map(str, v['wp']))} phl={v['phl']}")
            continue
        if " ph120.rt " in line:
            # the render thread's sync waits, joined per window (ab_s_w, ab_s_ms)
            out.append(line.replace(" s(n,w,us)=0,0,0 ", " s(n,w,us)=7,3,4500 "))
            continue
        out.append(line)
        if line is APPLIED:
            if banner:
                out.append(AB_BANNER.format(bid=banner_bid, ab=token))
            out.append(f"[6.200 thr 0x40010003] KAGE VITA GL ADMIT BEHIND: mode={receipt or mode} "
                       f"bytes={bytes_} backoff=30 record=ph120.ab")
    return "\n".join(out) + "\n"


def leg_admit_behind() -> None:
    done = [0]

    def ck(name: str, cond: bool, detail="") -> None:
        done[0] += 1
        check(name, cond, detail)

    nonce = "0123456789ab"
    game = "\n".join(good_game(nonce)) + "\n"
    plain = rows_of(b.diagnose("\n".join(good_native()) + "\n", game, nonce))
    ck("admit behind: no ph120.ab -> no ab columns",
          all("ab_n" not in r and "pm_c" not in r for r in plain.values()),
          str([k for r in plain.values() for k in r if k.startswith("ab_")]))
    ck("admit behind: no ph120.ab -> no report table",
          "GL admit behind" not in b.report(b.diagnose("\n".join(good_native()) + "\n", game, nonce)), "")
    nat0 = b.parse_native(ab_native("on"))
    pw, _ = b.phase_windows(nat0, b.parse_game(game))
    win = dict((name, sel) for name, sel, _ in pw)["P2 fight-maxroom"][0]
    ck("admit behind: the synthetic ph120.rt carries s(n,w,us)=7,3,4500",
          " s(n,w,us)=7,3,4500 " in ab_native("on") and " s(n,w,us)=0,0,0 " not in ab_native("on"), "")
    for mode, v in AB_MODES.items():
        res = b.diagnose(ab_native(mode), game, nonce)
        ck(f"admit behind {mode}: run ok, mode and bound parsed",
              res["ok"] and res.get("ab_mode") == mode and res.get("ab_bytes") == 65536,
              f"{res['verdict']} {res.get('ab_mode')} {res.get('ab_bytes')}")
        r = rows_of(res)["P2 fight-maxroom"]
        nw = r.get("windows")
        exp = {"ab_windows": nw, "ab_n": nw * v["n"], "ab_stp": nw * v["st"][0], "ab_stf": nw * v["st"][1],
               "ab_stx": nw * v["st"][2], "ab_ov": nw * v["ov"], "ab_wa": nw * v["wa"],
               "ab_ok": nw * v["ok"], "ab_bo": nw * v["bo"], "ab_tt": nw * v["tt"],
               "ab_pass": (v["st"][0] + v["st"][2]) / v["n"], "ab_wa_share": v["wa"] / v["n"],
               "ab_ob50": v["ob"][0], "ab_obmax": v["ob"][1], "ab_hl": nw * v["hl"][0],
               "ab_hl_ms": nw * v["hl"][1] / 1e3, "ab_hd": nw * v["hd"][0],
               "ab_hd_ms": nw * v["hd"][1] / 1e3, "ab_s_w": nw * 3, "ab_s_ms": nw * 4.5,
               "pm_c": nw * v["pm"][0], "pm_ok": nw * v["pm"][1], "pm_ms": nw * v["pm"][2] / 1e3,
               "pm_max": v["pm"][3] / 1e3, "wp_c": nw * v["wp"][0], "wp_ok": nw * v["wp"][1],
               "wp_ms": nw * v["wp"][2] / 1e3, "wp_max": v["wp"][3] / 1e3, "ab_phl": nw * v["phl"],
               "ab_bad": 0, "ab_missing": 0, "refused": nw * v["rs"]}
        for k, want in exp.items():
            ck(f"admit behind {mode}: P2 {k}", near(r.get(k), want), f"{r.get(k)} != {want}")
        text = b.report(res)
        ck(f"admit behind {mode}: report tables and build line",
              f"GL admit behind per phase (ph120.ab, gl-admit-behind {mode} bytes=65536;" in text
              and "GL admit behind, refusals checked at the next Present (ph120.abp;" in text
              and f"; gl-admit-behind {mode} bytes=65536" in text and "ab_missing" in text
              and "ab_wa_share" in text and "disagrees with the banner" not in text, text[-400:])
        other = "on" if mode == "observe" else "observe"
        stale = b.diagnose(ab_native(mode, banner_bid="rel:another-build-0000000000000000"), game, nonce)
        ck(f"admit behind {mode}: banner of another bid -> mode unknown",
              stale.get("ab_mode") is None and stale.get("ab_bytes") is None
              and "gl-admit-behind unknown bytes=unknown" in b.report(stale), f"{stale.get('ab_mode')}")
        nob = b.diagnose(ab_native(mode, banner=False), game, nonce)
        ck(f"admit behind {mode}: receipt alone -> mode unknown", nob.get("ab_mode") is None,
              f"{nob.get('ab_mode')}")
        dis = b.diagnose(ab_native(mode, receipt=other), game, nonce)
        ck(f"admit behind {mode}: receipt {other} vs banner -> banner wins, disagreement reported",
              dis.get("ab_mode") == mode
              and f"GL ADMIT BEHIND receipt mode/bytes={other}/65536 disagrees with the banner" in b.report(dis),
              f"{dis.get('ab_mode')} {dis.get('ab_receipt')}")
        ck(f"admit behind {mode}: complete run -> no AB diagnostic",
              not any(code.startswith("AB_") for _, code, _ in res["diagnostics"]),
              str([x for x in res["diagnostics"] if x[1].startswith("AB_")]))
        bad_res = b.diagnose(ab_native(mode, bad_window=win), game, nonce)
        bad = rows_of(bad_res)
        ck(f"admit behind {mode}: one mutated rs -> P2 ab_bad 1",
              bad["P2 fight-maxroom"].get("ab_bad") == 1
              and sum(x.get("ab_bad", 0) for x in bad.values()) == 1,
              str({p: x.get("ab_bad") for p, x in bad.items()}))
        ck(f"admit behind {mode}: one mutated rs -> WARN AB_IDENTITY naming P2, run still ok",
              bad_res["ok"] and [(lv, msg) for lv, code, msg in bad_res["diagnostics"]
                                 if code == "AB_IDENTITY"] == [
                  ("WARN", "ph120.ab / ph120.abp windows breaking a per-window identity (ab_bad): P2 1")],
              str([x for x in bad_res["diagnostics"] if x[1].startswith("AB_")]))
        for record in ("ab", "abp"):
            miss_res = b.diagnose(ab_native(mode, missing_window=win, missing=record), game, nonce)
            ck(f"admit behind {mode}: one window without ph120.{record} -> BENCH FAILED (AB_RECORD_MISSING)",
                  not miss_res["ok"] and miss_res["verdict"].startswith("BENCH FAILED (AB_RECORD_MISSING)")
                  and f"P2 1 of {nw}" in miss_res["verdict"], miss_res["verdict"])
            miss = rows_of(b.diagnose(ab_native(mode, missing_window=win, missing=record), game,
                                      nonce))["P2 fight-maxroom"]
            ck(f"admit behind {mode}: one window without ph120.{record} -> ab_missing 1",
                  miss.get("ab_missing") == 1
                  and miss.get("ab_windows") == nw - (1 if record == "ab" else 0)
                  and near(miss.get("pm_c"), (nw - 1) * v["pm"][0]),
                  f"{miss.get('ab_missing')} {miss.get('ab_windows')} {miss.get('pm_c')}")
    # The banner says OBSERVE and no window has either record (a build without the phase-profile
    # records): every phase is named, the run is not a BENCH OK.
    none = b.diagnose("".join(line + "\n" for line in ab_native("observe").splitlines()
                              if " ph120.ab " not in line and " ph120.abp " not in line), game, nonce)
    ck("admit behind: banner and no ph120.ab at all -> BENCH FAILED (AB_RECORD_MISSING), every phase named",
          not none["ok"] and none["verdict"].startswith("BENCH FAILED (AB_RECORD_MISSING)")
          and all(f"{r['phase'].split()[0]} {r['windows']} of {r['windows']}" in none["verdict"]
                  for r in none["phases"]) and len(none["phases"]) >= 5, none["verdict"])
    # ... and without the banner the same log is an ordinary run (no knob, nothing owed).
    plain_ab = b.diagnose("".join(line + "\n" for line in ab_native("observe", banner=False).splitlines()
                                  if " ph120.ab " not in line and " ph120.abp " not in line), game, nonce)
    ck("admit behind: no banner and no ph120.ab -> nothing owed", plain_ab["ok"]
          and not any(code.startswith("AB_") for _, code, _ in plain_ab["diagnostics"]), plain_ab["verdict"])
    other_bound = b.diagnose(ab_native("on", bytes_=131072), game, nonce)
    ck("admit behind: the bound comes from the banner", other_bound.get("ab_bytes") == 131072
          and "gl-admit-behind on bytes=131072" in b.report(other_bound), f"{other_bound.get('ab_bytes')}")
    # J1, J3 and J4 on their own (J2 holds): each broken in every window.
    for what, patch in (("J1", {"st": (12, 5, 2)}), ("J3 ov + wa", {"ov": 8}), ("J3 ok", {"wa": 8, "ov": 0}),
                        ("J4 pm ok", {"pm": (11, 12, 0, 0)}), ("J4 pm c", {"pm": (13, 4, 0, 0)}),
                        ("J4 wp c", {"wp": (12, 1, 0, 0)}), ("J4 wp ok", {"wp": (2, 3, 0, 0)}),
                        ("J4 phl", {"phl": 12})):
        AB_MODES["probe"] = dict(AB_MODES["on"], **patch)
        try:
            r = rows_of(b.diagnose(ab_native("probe"), game, nonce))["P2 fight-maxroom"]
        finally:
            del AB_MODES["probe"]
        ck(f"admit behind: {what} broken in every window -> ab_bad = windows",
              r.get("ab_bad") == r.get("windows"), f"{r.get('ab_bad')} {r.get('windows')}")
    # ... and the carried refusal (pm c = n - ok + 1) is not a break.
    AB_MODES["probe"] = dict(AB_MODES["on"], pm=(12, 4, 0, 0))
    try:
        r = rows_of(b.diagnose(ab_native("probe"), game, nonce))["P2 fight-maxroom"]
    finally:
        del AB_MODES["probe"]
    ck("admit behind: pm c = n - ok + 1 (carried across the edge) is not bad", r.get("ab_bad") == 0,
          f"{r.get('ab_bad')}")
    print(f"admit behind: {done[0]} checks (OBSERVE and ON: mode and bound from the bid-checked banner, "
          "32 hand-checked P2 values each, report tables, stale / missing banner, receipt disagreement, "
          "ab_bad from one mutated rs and from J1 / J3 / J4, the carried refusal, ab_missing per record; "
          "a window or a whole run without ph120.ab / ph120.abp is BENCH FAILED (AB_RECORD_MISSING), "
          "ab_bad is WARN AB_IDENTITY)")


# GL draw packet (ISAAC_VITA_GL_DRAW_PACKET=VERIFY|ON): one synthetic
# ph120.dp per window, printed after ph120.rw as kage_vita_profile_log_draw_packet does (format copied:
# recomp/runtime/kage_vita_phase_profile.c, "ph120.dp bid=%.32s win=%u loops=%u mode=%s d=%u f=%u pk=%u
# p1=%u sub=%u B=%u mx=%u fr(r,c,s,f)=%u,%u,%u,%u v(p,s)=%u,%u mm=%u late=%u tot=%u").  Per window:
# 9700 draws, 9650 of them in 9650 packets, 40 calls published alone, 38600 staged calls (4 per draw:
# blend, bind packet, attrib enable, attrib disable), 2.4 MB staged; VERIFY compares every packet with
# the 5 separate records it stands for.  ph120.rt r = 48721 (the synthetic rt line of W41).
DP_MODES = {
    # VERIFY counts a single in p1 AND publishes it as a one-call packet (pk); v(p) = pk and
    # v(s) = sub + f per window: 9650 + 40, 38640 + 9650
    "VERIFY": {"d": 9700, "f": 9650, "pk": 9690, "p1": 40, "sub": 38640, "B": 2400000, "mx": 640,
               "fr": (30, 25, 12, 0), "v": (9690, 48290)},
    "ON": {"d": 9700, "f": 9650, "pk": 9650, "p1": 40, "sub": 38600, "B": 2400000, "mx": 640,
           "fr": (30, 25, 12, 0), "v": (0, 0)},
}


def dp_native(mode: str, bad_window=None, bad="mm", missing_window=None, p1=None, mode_text=None,
              edit=None, tail_tot=0) -> str:
    """edit = {window: {field: value}} overrides one window's fields; tail_tot appends one more
    ph120.dp after the last ph120.rw (a window no phase selects) whose tot is raised by it."""
    base = dict(DP_MODES[mode])
    if p1 is not None:
        base["p1"] = p1
    out = []
    tot = 0
    last = None
    for line in good_native():
        out.append(line)
        if " ph120.rw " in line:
            k = int(line.split(" win=")[1].split()[0])
            last = k
            v = dict(base, **((edit or {}).get(k, {})))
            mm = 1 if (k == bad_window and bad == "mm") else 0
            late = 2 if (k == bad_window and bad == "late") else 0
            tot += mm + late
            if k == missing_window:
                continue
            out.append(dp_line(mode_text or mode, k, v, mm, late, tot))
    if tail_tot:
        out.append(dp_line(mode_text or mode, last + 1, base, 0, 0, tot + tail_tot))
    return "\n".join(out) + "\n"


def dp_line(mode: str, k: int, v: dict, mm: int, late: int, tot: int) -> str:
    return (f"[kage-vita] ph120.dp bid={BID} win={k} loops={120 * k} mode={mode} "
            f"d={v['d']} f={v['f']} pk={v['pk']} p1={v['p1']} sub={v['sub']} B={v['B']} "
            f"mx={v['mx']} fr(r,c,s,f)={','.join(map(str, v['fr']))} "
            f"v(p,s)={','.join(map(str, v['v']))} mm={mm} late={late} tot={tot}")


def leg_draw_packet() -> None:
    done = [0]

    def ck(name: str, cond: bool, detail="") -> None:
        done[0] += 1
        check(name, cond, detail)

    nonce = "0123456789ab"
    game = "\n".join(good_game(nonce)) + "\n"
    base = b.diagnose("\n".join(good_native()) + "\n", game, nonce)
    ck("draw packet: no ph120.dp -> no dp columns",
       all(not any(k.startswith("dp_") for k in r) for r in rows_of(base).values()),
       str([k for r in rows_of(base).values() for k in r if k.startswith("dp_")]))
    ck("draw packet: no ph120.dp -> no report table and no DP diagnostic",
       "GL draw packet" not in b.report(base)
       and not any(code.startswith("DP_") for _, code, _ in base["diagnostics"]), "")
    nat0 = b.parse_native(dp_native("ON"))
    pw, _ = b.phase_windows(nat0, b.parse_game(game))
    win = dict((name, sel) for name, sel, _ in pw)["P2 fight-maxroom"][0]
    ck("draw packet: the synthetic line parses as one ph120.dp per window",
       all("ph120.dp" in w for k, w in nat0["wins"].items() if "ph120.rw" in w)
       and nat0["wins"][win]["ph120.dp"].get("fr") == "30,25,12,0"
       and nat0["wins"][win]["ph120.dp"].get("v") == "0,0", str(nat0["wins"][win].get("ph120.dp")))
    for mode, v in DP_MODES.items():
        res = b.diagnose(dp_native(mode), game, nonce)
        ck(f"draw packet {mode}: run ok, no DP diagnostic",
           res["ok"] and not any(code.startswith("DP_") for _, code, _ in res["diagnostics"]),
           f"{res['verdict']} {[x for x in res['diagnostics'] if x[1].startswith('DP_')]}")
        r = rows_of(res)["P2 fight-maxroom"]
        nw = r.get("windows")
        ck(f"draw packet {mode}: P2 has windows", isinstance(nw, int) and nw > 0, f"{nw}")
        exp = {"dp_windows": nw, "dp_d": nw * v["d"], "dp_f": nw * v["f"], "dp_pk": nw * v["pk"],
               "dp_p1": nw * v["p1"], "dp_sub": nw * v["sub"], "dp_B": nw * v["B"], "dp_mm": 0,
               "dp_late": 0, "dp_mx": v["mx"], "dp_fr_r": nw * v["fr"][0], "dp_fr_c": nw * v["fr"][1],
               "dp_fr_s": nw * v["fr"][2], "dp_fr_f": nw * v["fr"][3], "dp_vp": nw * v["v"][0],
               "dp_vs": nw * v["v"][1], "dp_tot": 0,
               # (pk + p1 + d - f) / d: VERIFY (9690 + 40 + 50) / 9700, ON (9650 + 40 + 50) / 9700
               "dp_path_pd": (v["pk"] + v["p1"] + v["d"] - v["f"]) / v["d"],
               "dp_rec_pd": 48721 / v["d"], "dp_sub_pd": v["sub"] / v["d"], "dp_bad": 0, "dp_missing": 0,
               "dp_verify_windows": nw if mode == "VERIFY" else 0, "dp_cov": 0, "dp_vsx": 0}
        for k, want in exp.items():
            ck(f"draw packet {mode}: P2 {k}", near(r.get(k), want), f"{r.get(k)} != {want}")
        ck(f"draw packet {mode}: P2 mode", r.get("dp_mode") == mode, f"{r.get('dp_mode')}")
        text = b.report(res)
        ck(f"draw packet {mode}: report table", f"GL draw packet per phase (ph120.dp, mode {mode} as the" in text
           and "dp_path_pd" in text and "dp_missing" in text, text[-400:])
        # one window with a differing packet / an overtaking record: FAIL DP_VERIFY naming P2
        for what in ("mm", "late"):
            bad_res = b.diagnose(dp_native(mode, bad_window=win, bad=what), game, nonce)
            bad = rows_of(bad_res)
            ck(f"draw packet {mode}: one window with {what} -> P2 dp_bad 1, only P2",
               bad["P2 fight-maxroom"].get("dp_bad") == 1
               and sum(x.get("dp_bad", 0) for x in bad.values()) == 1
               and bad["P2 fight-maxroom"].get(f"dp_{what}") == (1 if what == "mm" else 2),
               str({p: x.get("dp_bad") for p, x in bad.items()}))
            want = "(mm 1, late 0)" if what == "mm" else "(mm 0, late 2)"
            ck(f"draw packet {mode}: one window with {what} -> BENCH FAILED (DP_VERIFY)",
               not bad_res["ok"] and bad_res["verdict"].startswith("BENCH FAILED (DP_VERIFY)")
               and f"P2 1 {want}" in bad_res["verdict"], bad_res["verdict"])
        miss_res = b.diagnose(dp_native(mode, missing_window=win), game, nonce)
        ck(f"draw packet {mode}: one window without ph120.dp -> BENCH FAILED (DP_RECORD_MISSING)",
           not miss_res["ok"] and miss_res["verdict"].startswith("BENCH FAILED (DP_RECORD_MISSING)")
           and "P2 1" in miss_res["verdict"], miss_res["verdict"])
        miss = rows_of(miss_res)["P2 fight-maxroom"]
        ck(f"draw packet {mode}: one window without ph120.dp -> dp_missing 1, sums over the rest",
           miss.get("dp_missing") == 1 and miss.get("dp_windows") == nw - 1
           and near(miss.get("dp_d"), (nw - 1) * v["d"])
           and near(miss.get("dp_rec_pd"), 48721 / v["d"]),
           f"{miss.get('dp_missing')} {miss.get('dp_windows')} {miss.get('dp_d')} {miss.get('dp_rec_pd')}")
        # p1 so large that the draw path is 2.03 records per draw: WARN in ON only
        fat_res = b.diagnose(dp_native(mode, p1=10000), game, nonce)
        warns = [(lv, msg) for lv, code, msg in fat_res["diagnostics"] if code == "DP_RECORDS_PER_DRAW"]
        if mode == "ON":
            ck("draw packet ON: path 2.03 records per draw -> WARN DP_RECORDS_PER_DRAW, run still ok",
               fat_res["ok"] and len(warns) >= 1 and warns[0][0] == "WARN" and "P2 2.03" in warns[0][1],
               str(warns))
        else:
            ck("draw packet VERIFY: path 2.03 records per draw -> no DP_RECORDS_PER_DRAW (VERIFY "
               "publishes every call alone as well)", fat_res["ok"] and not warns, str(warns))
    # every VERIFY condition a device gate relies on must be one diagnose() checks
    def dp_codes(res, level):
        return [(code, msg) for lv, code, msg in res["diagnostics"] if lv == level and code.startswith("DP_")]
    zero = dict(DP_MODES["VERIFY"], v=(0, 0))
    res = b.diagnose(dp_native("VERIFY", edit={k: zero for k in nat0["wins"]}), game, nonce)
    ck("draw packet VERIFY: v(p,s) = 0,0 in every window -> BENCH FAILED (DP_VERIFY_EMPTY) naming P2",
       not res["ok"] and ("DP_VERIFY_EMPTY" in res["verdict"]) and any(
           code == "DP_VERIFY_EMPTY" and "P2" in msg for code, msg in dp_codes(res, "FAIL")), res["verdict"])
    res = b.diagnose(dp_native("ON", edit={k: zero for k in nat0["wins"]}), game, nonce)
    ck("draw packet ON: v(p,s) = 0,0 (ON compares nothing) -> no DP diagnostic",
       res["ok"] and not dp_codes(res, "FAIL") and not dp_codes(res, "WARN"), res["verdict"])
    res = b.diagnose(dp_native("VERIFY", tail_tot=1), game, nonce)
    ck("draw packet VERIFY: tot = 1 only after the last phase window (mm = late = 0 everywhere) -> "
       "BENCH FAILED (DP_VERIFY) with the run's tot",
       not res["ok"] and res["verdict"].startswith("BENCH FAILED (DP_VERIFY)")
       and "tot (mm + late since boot) = 1" in res["verdict"]
       and sum(r.get("dp_bad", 0) for r in rows_of(res).values()) == 0, res["verdict"])
    pk = DP_MODES["VERIFY"]["pk"]
    for dv, fail in ((1, False), (-1, False), (2, True), (-2, True)):
        vw = dict(DP_MODES["VERIFY"], v=(pk + dv, DP_MODES["VERIFY"]["v"][1]))
        res = b.diagnose(dp_native("VERIFY", edit={win: vw}), game, nonce)
        cov = [msg for code, msg in dp_codes(res, "FAIL") if code == "DP_VERIFY_COVERAGE"]
        if fail:
            ck(f"draw packet VERIFY: one P2 window with v(p) = pk {dv:+d} -> BENCH FAILED "
               "(DP_VERIFY_COVERAGE) naming P2 1",
               not res["ok"] and res["verdict"].startswith("BENCH FAILED (DP_VERIFY_COVERAGE)")
               and len(cov) == 1 and "P2 1 of" in cov[0]
               and rows_of(res)["P2 fight-maxroom"].get("dp_cov") == 1, res["verdict"])
        else:
            ck(f"draw packet VERIFY: one P2 window with v(p) = pk {dv:+d} (the take's packet) -> run ok, "
               "no DP diagnostic", res["ok"] and not dp_codes(res, "FAIL") and not dp_codes(res, "WARN"),
               res["verdict"])
    vw = dict(DP_MODES["VERIFY"], v=(pk, DP_MODES["VERIFY"]["v"][1] - 3))
    res = b.diagnose(dp_native("VERIFY", edit={win: vw}), game, nonce)
    warns = [msg for code, msg in dp_codes(res, "WARN") if code == "DP_VERIFY_SUBS"]
    ck("draw packet VERIFY: one P2 window with v(s) = sub + f - 3 -> WARN DP_VERIFY_SUBS naming P2 1, run ok",
       res["ok"] and len(warns) == 1 and "P2 1 of" in warns[0]
       and rows_of(res)["P2 fight-maxroom"].get("dp_vsx") == 1, f"{res['verdict']} {warns}")
    res = b.diagnose(dp_native("VERIFY", bad_window=win, bad="mm", edit={win: vw}), game, nonce)
    ck("draw packet VERIFY: the same v(s) gap in a window with mm > 0 -> DP_VERIFY only, no DP_VERIFY_SUBS",
       not res["ok"] and not [c for c, _ in dp_codes(res, "WARN") if c == "DP_VERIFY_SUBS"], res["verdict"])
    # a printed mode the recorder never prints (BAD) is reported as printed, not hidden
    odd = rows_of(b.diagnose(dp_native("ON", mode_text="BAD"), game, nonce))["P2 fight-maxroom"]
    ck("draw packet: mode=BAD is carried to the row", odd.get("dp_mode") == "BAD", f"{odd.get('dp_mode')}")
    print(f"draw packet: {done[0]} checks (VERIFY and ON: 22 hand-checked P2 values each, mode, report "
          "table; one window with mm or late, or tot > 0 anywhere in the run, is BENCH FAILED (DP_VERIFY); "
          "VERIFY with v(p) = 0 is DP_VERIFY_EMPTY, |v(p) - pk| > 1 is DP_VERIFY_COVERAGE, v(s) != sub + f "
          "is WARN DP_VERIFY_SUBS; one window "
          "without ph120.dp is BENCH FAILED (DP_RECORD_MISSING); more than 2 records per draw is WARN "
          "DP_RECORDS_PER_DRAW in ON only)")


def leg_census() -> None:
    nonce = "0123456789ab"
    game = "\n".join(good_game(nonce)) + "\n"
    res = b.diagnose(census_native(), game, nonce)
    # INTERCEPT_UNSEEN: the synthetic native log has no 'xinput snapshot' line
    check("census run ok", res["ok"] and codes(res, "WARN") == {"INTERCEPT_UNSEEN"},
          res["verdict"] + str(codes(res, "WARN")))
    names = b.opcode_names()
    check("opcode names from the generated enum", names.get(1) == "glActiveTexture" and
          names.get(24) == "glDrawElements" and names.get(78) == "WORKER_CALL" and
          names.get(79) == "ATTRIBS_ENABLE" and names.get(80) == "ATTRIBS_DISABLE" and
          names.get(81) == "KAGE_BIND_PACKET" and names.get(82) == "DRAW_PACKET" and max(names) == 82,
          str({k: names.get(k) for k in (1, 24, 78, 79, 80, 81, 82)}))
    c = rows_of(res)["P2 fight-maxroom"].get("census") or {}
    check("census windows", (c.get("windows"), c.get("incomplete"), c.get("mismatch"), c.get("presents"))
          == (9, 0, 0, 1080), str({k: c.get(k) for k in ("windows", "incomplete", "mismatch", "presents")}))
    check("census clock costs", near(c.get("ck_game_us"), 1.0) and near(c.get("ck_render_us"), 1.0),
          f"{c.get('ck_game_us')} {c.get('ck_render_us')}")
    ops = {o["op"]: o for o in c.get("ops", [])}
    exp = {1: (59800 / 120, 44850 / 120, ""), 24: (180000 / 120, 72000 / 120, ""),
           59: (500 / 120, None, "x-"), 78: (72100 / 120, None, "")}
    for op, (rnd, gus, flags) in exp.items():
        o = ops.get(op, {})
        check(f"census op {op} rnd", near(o.get("render_us_pf"), rnd), f"{o.get('render_us_pf')} != {rnd}")
        check(f"census op {op} game", (gus is None and o.get("game_us_pf") is None) or
              near(o.get("game_us_pf"), gus), f"{o.get('game_us_pf')} != {gus}")
        check(f"census op {op} flags", o.get("flags") == flags, f"{o.get('flags')!r} != {flags!r}")
    check("census op 24 per present", near(ops[24]["n_pf"], 150.0) and near(ops[24]["b_pf"], 900000.0),
          f"{ops[24]['n_pf']} {ops[24]['b_pf']}")
    check("census op 78 S waits", near(ops[78]["s_pf"], 2 / 120) and near(ops[78]["sblk_us_pf"], 666 / 120), "")
    check("census order by rnd", [o["op"] for o in c["ops"]] == [24, 78, 1, 59], [o["op"] for o in c["ops"]])
    check("census sums", near(c["render_us_pf"], (59800 + 180000 + 500 + 72100) / 120) and
          near(c["game_us_pf"], (44850 + 72000) / 120), f"{c['render_us_pf']} {c['game_us_pf']}")
    rep = b.report(res)
    check("report has the census table", "GL recorder opcode census per phase" in rep and
          " 24 glDrawElements " in rep, rep[-600:])
    # A broken game-thread sampler: all 760 samples on one opcode (sums still agree with rw).
    skew = dict(CENSUS_SLOTS)
    skew[1] = skew[1][:6] + (760, 0, 1900) + skew[1][9:]
    skew[24] = skew[24][:6] + (0, 0, 0) + skew[24][9:]
    o2 = {o["op"]: o for o in rows_of(b.diagnose(census_native(skew), game, nonce))["P2 fight-maxroom"]
          ["census"]["ops"]}
    check("census sampler flag p", "p" in o2[1]["flags"] and o2[24]["flags"] == "-p",
          f"{o2[1]['flags']!r} {o2[24]['flags']!r}")
    # One P2 window with a rop part missing, or disagreeing with ph120.rt r=: excluded, WARN.
    nat0 = b.parse_native(census_native())
    pw, _ = b.phase_windows(nat0, b.parse_game(game))
    win = dict((name, sel) for name, sel, _ in pw)["P2 fight-maxroom"][0]
    bad = dict(CENSUS_SLOTS)
    bad[59] = (101,) + bad[59][1:]
    for kind, kw in (("incomplete", {"drop_part": 1}), ("mismatch", {"slots_bad": bad})):
        r3 = b.diagnose(census_native(bad_window=win, **kw), game, nonce)
        c3 = rows_of(r3)["P2 fight-maxroom"]["census"]
        check(f"census {kind} window excluded", c3[kind] == 1 and c3["windows"] == 8 and
              "CENSUS_WINDOWS" in codes(r3, "WARN") and r3["ok"],
              str({k: c3[k] for k in ("windows", "incomplete", "mismatch")}) + str(codes(r3, "WARN")))
    # Builds without the knob: no census key, no table.
    plain = b.diagnose("\n".join(good_native()) + "\n", game, nonce)
    check("no census without ph120.ro", "census" not in rows_of(plain)["P2 fight-maxroom"] and
          "opcode census" not in b.report(plain), "")
    print(f"census: {len(exp)} opcodes x 3 estimators hand-checked, sampler flag, incomplete and "
          "disagreeing windows excluded, report table")


def floor_debt_native(gf) -> list:
    """good_native() with a ph120.fd/ph120.fs pair after every ph120.s; gf = the gf(...) field, or a
    function of the window number returning it."""
    nat = []
    for line in good_native():
        nat.append(line)
        m = re.search(r"\[kage-vita\] ph120\.s bid=(\S+) win=(\d+) loops=(\d+) ", line)
        if m:
            head = f"bid={m.group(1)} win={m.group(2)} loops={m.group(3)}"
            gfv = gf(int(m.group(2))) if callable(gf) else gf
            nat.append(f"[kage-vita] ph120.fd {head} p=60 late(c,us,max)=40,3000,900 kept=2500 "
                       f"over(c,us)=1,500 stale(c,g,us)=2,1,1200 gf(n,pos,max,min,last)={gfv}")
            nat.append(f"[kage-vita] ph120.fs {head} h=5,10,15,6,3,1,0 slack(b,l,us)=4,2,5200")
    return nat


def leg_floor_debt() -> None:
    """ph120.fd/ph120.fs (ISAAC_VITA_FULLSPEED_FLOOR_DEBT) after every ph120.s: hand-computed P2 row.
    Window at-delta 2,000,000 us with g=60, so loss = 0 and the residuals are the receipts' own sums."""
    nonce = "0123456789ab"
    nat = floor_debt_native("60,0,-120,-16000,-500")
    res = b.diagnose("\n".join(nat) + "\n", "\n".join(good_game(nonce)) + "\n", nonce)
    check("floor-debt run ok", res["ok"], res["verdict"])
    r = {x["phase"]: x for x in res["phases"]}.get("P2 fight-maxroom", {})
    exp = {"fd_windows": 9, "loss_ms": 0.0, "late_ms": 3.0, "res_obs": 4.2, "res_obsw": 4.2, "res_on": 1.7,
           "res_onw": 1.7, "kept_late": 2500 / 3000, "h5": 9, "h6": 0, "stale": 18, "gfpos": 0,
           "gfmax": -0.12, "gf1": 0, "gfstuck": 0, "slack_b": 4, "slack_ms": 2.6, "pg": 0}
    for k, v in exp.items():
        check(f"floor-debt P2 {k}", near(r.get(k), v), f"{r.get(k)} != {v}")
    rep = b.report(res)
    check("report has 'floor debt per phase'", "floor debt per phase" in rep, "")
    check("report has the gfstuck column", "gfstuck" in rep, "")
    plain = b.report(b.diagnose("\n".join(good_native()) + "\n", "\n".join(good_game(nonce)) + "\n", nonce))
    check("no floor-debt section without ph120.fd", "floor debt per phase" not in plain, "")
    print(f"floor debt: {len(exp)} hand-checked P2 values")


def leg_floor_debt_stuck() -> None:
    """gfstuck (R8): a window counts only when every gf sample is
    > 0 (pos == n > 0) and the last one is above 1000 us; gf1 counts windows whose max is above
    1000 us.  P2 has 9 windows, every one carrying the same gf field."""
    nonce = "0123456789ab"
    cases = (
        ("60,60,3100,3100,3100", 9, 9, "B1 signature: gf frozen at +3.1 ms"),
        ("60,60,1001,100,1001", 9, 9, "last 1001 us: just above the bound"),
        ("60,60,1000,100,1000", 0, 0, "last exactly 1000 us: not stuck, max not above 1 ms"),
        ("60,60,400,100,400", 0, 0, "legit small positive gf after waited paces"),
        ("60,59,3100,-500,3100", 0, 9, "one sample <= 0: not stuck, still a > 1 ms window"),
        ("60,60,3100,100,800", 0, 9, "last below 1 ms: not stuck"),
        ("0,0,0,0,0", 0, 0, "no samples"),
    )
    for gf, stuck, over1, what in cases:
        nat = floor_debt_native(gf)
        res = b.diagnose("\n".join(nat) + "\n", "\n".join(good_game(nonce)) + "\n", nonce)
        r = {x["phase"]: x for x in res["phases"]}.get("P2 fight-maxroom", {})
        check(f"gfstuck {gf} ({what})", r.get("gfstuck") == stuck, f"{r.get('gfstuck')} != {stuck}")
        check(f"gf1 {gf} ({what})", r.get("gf1") == over1, f"{r.get('gf1')} != {over1}")

    # Unequal windows.  P2 is windows 17..25; window w has k = w - 16 (1..9), pos = k - 1, max =
    # 250 k us, and window 25 is the B1 signature (pos = n = 60, last = max = 2250 us).
    def ramp(win: int) -> str:
        k = win - 16
        if not 1 <= k <= 9:
            return "60,0,-120,-16000,-500"
        if k == 9:
            return "60,60,2250,100,2250"
        return f"60,{k - 1},{250 * k},-500,-500"
    nat = floor_debt_native(ramp)
    res = b.diagnose("\n".join(nat) + "\n", "\n".join(good_game(nonce)) + "\n", nonce)
    r = {x["phase"]: x for x in res["phases"]}.get("P2 fight-maxroom", {})
    # pos 0..7 and 60: sum 88, median 4, worst 60; max 0.25..2.25 ms: median 1.25 (k = 5),
    # nearest-rank p99 = ceil(8.91) = 9th = 2.25; above 0.5 ms k >= 3 (7), above 1 ms k >= 5 (5),
    # above 2 ms k = 9 (1), above 3 ms none.
    exp = {"gfpos": 88, "gfmax": 2.25, "gfpos50": 4, "gfposw": 60, "gfmax50": 1.25, "gfmax99": 2.25,
           "gf05": 7, "gf1": 5, "gf2": 1, "gf3": 0, "gfstuck": 1}
    for k, v in exp.items():
        check(f"gf ramp P2 {k}", near(r.get(k), v), f"{r.get(k)} != {v}")
    rep = b.report(res)
    check("report has the gf per window table", "gf per window" in rep and "gfmax99" in rep, "")
    print(f"floor debt gfstuck/gf1: {len(cases)} cases; gf ramp: {len(exp)} hand-checked P2 values")


# Windows with floor stamps.  A correct scheduler moves the Game floor by p * T plus
# the receipt's attribution (late + stale under OBSERVE, over + stale under ON) per window, while
# the report time at= sits at a loop head before the head wait: at = floor + 20 ms + e(k), where
# e(k) is the boundary tick's work-time noise.  Window 25 (P2's last) keeps its old at, so the
# phase selection is the same as good_native()'s.
BANNER = ("[1.000 thr 0x40010003] KAGE VITA CADENCE30: bid={bid} tick_units=50000 game_tick_units=100000 "
          "max_catchup=4 stale_catchup=1 max_full_skip=4 parity=4a264 frame=1a30dc reserve=pre-game "
          "floor-debt={mode} cap=8000us admit-slack=2000us fps_lock=60")
FD_DEFAULT = {"late": (40, 3000, 900), "kept": 2500, "over": (1, 500), "clamp": (1, 500),
              "stale": (0, 0, 0)}


def stamped_native(mode="observe", fields=None, noise=None, moved=None, banner_bid=BID, stamps=True):
    """good_native() plus a CADENCE30 banner and ph120.fd/fs records with floor stamps.
    fields(k) -> overrides of FD_DEFAULT for window k; noise(k) -> e(k) us; moved(k) -> extra floor
    movement the receipts do not explain (a misattribution), us."""
    fields = fields or (lambda k: {})
    noise = noise or (lambda k: 0)
    moved = moved or (lambda k: 0)
    rec = {k: dict(FD_DEFAULT, **fields(k)) for k in range(1, 56)}

    def att(k):
        r = rec[k]
        return (r["late"][1] if mode == "observe" else r["over"][1]) + r["stale"][2] + moved(k)
    floor = {}
    for k in range(1, 56):  # floor(25) = 30e6 + 50e6 - 20 ms; each window moves it 2e6 + att(k)
        floor[k] = 30_000_000 + 2_000_000 * k - 20_000 + sum(att(j) for j in range(1, k + 1)) - \
            sum(att(j) for j in range(1, 26))
    nat = []
    for line in good_native():
        m = re.search(r"\[kage-vita\] ph120\.s bid=(\S+) win=(\d+) loops=(\d+) ", line)
        if not m:
            nat.append(line)
            if line == BUILD and banner_bid is not None:
                nat.append(BANNER.format(bid=banner_bid, mode=mode))
            continue
        k = int(m.group(2))
        at = floor[k] + 20_000 + noise(k)
        nat.append(re.sub(r"\bat=[0-9a-f]{16}", f"at={at:016x}", line))
        r, head = rec[k], f"bid={m.group(1)} win={k} loops={m.group(3)}"
        nat.append(f"[kage-vita] ph120.fd {head} p=60 late(c,us,max)={r['late'][0]},{r['late'][1]},"
                   f"{r['late'][2]} kept={r['kept']} over(c,us)={r['over'][0]},{r['over'][1]} "
                   f"clamp(c,us)={r['clamp'][0]},{r['clamp'][1]} stale(c,g,us)={r['stale'][0]},"
                   f"{r['stale'][1]},{r['stale'][2]} gf(n,pos,max,min,last)=60,0,-120,-16000,-500")
        # OBSERVE never admits by slack (l = us = 0, R1.2); ON admits 2 of the 4 band frames
        nat.append(f"[kage-vita] ph120.fs {head} h=5,10,15,6,3,1,0 slack(b,l,us)=" +
                   ("4,0,0" if mode == "observe" else "4,2,5200") +
                   (f" floor={floor[k]:016x}" if stamps else ""))
    return nat


def fd_run(nat):
    nonce = "0123456789ab"
    res = b.diagnose("\n".join(nat) + "\n", "\n".join(good_game(nonce)) + "\n", nonce)
    rows = {x["phase"]: x for x in res["phases"] + res["fd_steady"]}
    return res, rows, "\n".join(res["fd_gates"])


def gate(text: str, name: str) -> str:
    """The verdict word of gate line `name` (PASS/FAIL/n/a/NO DATA), or '' when absent."""
    for line in text.splitlines():
        if line.startswith(name + " "):
            for word in ("NO DATA", "PASS", "FAIL", "n/a"):
                if f": {word}" in line:
                    return word
    return ""


def leg_floor_debt_gates() -> None:
    """Each false-fail of the earlier gates reproduced on correct-code data, the new
    gate passing it, and the mutants the new gates must catch."""
    n = 0
    # Clean OBSERVE run: P2 = windows 17..25, nothing excluded, every identity exact.
    res, rows, gates = fd_run(stamped_native())
    p2 = rows.get("P2 fight-maxroom", {})
    check("fd gates: clean run ok", res["ok"], res["verdict"])
    check("fd gates: mode parsed", res["fd_mode"] == "observe" and res["fd_cap"] == 8000, res["fd_mode"])
    check("fd gates: P2 windows", p2.get("windows") == 9 and p2.get("fn") == 9 and p2.get("fx") == 0,
          f"{p2.get('windows')} {p2.get('fn')} {p2.get('fx')}")
    check("fd gates: clean fres 0", near(p2.get("fres_obs"), 0.0) and near(p2.get("res_obs"), 0.0),
          f"{p2.get('fres_obs')} {p2.get('res_obs')}")
    check("fd gates: bmin +20 ms (at - floor)", near(p2.get("bmin"), 20.0), str(p2.get("bmin")))
    check("fd gates: clean R1.3 PASS", gate(gates, "R1.3") == "PASS", gates)
    check("fd gates: OBSERVE prints no R8 line", gate(gates, "R8.3") == "" and gate(gates, "R8.2") == "", gates)
    n += 7

    # False-fail 1 (median): boundary noise of +-3 ms alternating per window.  The earlier
    # res_obs median (gate <= 2 ms) reads 6 ms; the floor stamps are untouched by it.
    res, rows, gates = fd_run(stamped_native(noise=lambda k: 3000 if k % 2 else -3000))
    p2 = rows["P2 fight-maxroom"]
    check("stamp identity, noise: old res_obs median 6 ms (earlier R1.3 false-fail)", near(p2["res_obs"], 6.0),
          str(p2["res_obs"]))
    check("stamp identity, noise: fres 0, R1.3 PASS", near(p2["fres_obs"], 0.0) and gate(gates, "R1.3") == "PASS", gates)
    # psum telescopes the noise to the run's two ends: |e(25) - e(16)| / 9 = 0
    check("stamp identity, noise: psum_obs 0", near(p2["psum_obs"], 0.0), str(p2["psum_obs"]))
    n += 3

    # False-fail 2 (worst window): one 12-ms hitch in the tick before window 20's report
    # (MODEL: light room with one 15-ms update hitch, windows -12.23 / +12.45 ms).
    res, rows, gates = fd_run(stamped_native(noise=lambda k: 12_000 if k == 20 else 0))
    p2 = rows["P2 fight-maxroom"]
    check("stamp identity, pair: old res_obsw 12 ms (earlier R1.3 false-fail)", near(p2["res_obsw"], 12.0),
          str(p2["res_obsw"]))
    check("stamp identity, pair: R1.3 PASS", near(p2["fres_obs"], 0.0) and gate(gates, "R1.3") == "PASS", gates)
    n += 2

    # Mutant: one window's floor moved 1 ms more than its receipt explains (a lost late_us,
    # an anchor that keeps what the receipt calls dropped).  The earlier gates pass it (median 0,
    # worst 1 ms <= 10); the stamp identity does not.
    res, rows, gates = fd_run(stamped_native(moved=lambda k: 1000 if k == 22 else 0))
    p2 = rows["P2 fight-maxroom"]
    check("stamp identity, mutant: earlier gates blind (res_obs 0, res_obsw 1)",
          near(p2["res_obs"], 0.0) and near(p2["res_obsw"], 1.0), f"{p2['res_obs']} {p2['res_obsw']}")
    check("stamp identity, mutant: fres 1 ms, R1.3 FAIL", near(p2["fres_obs"], 1.0) and gate(gates, "R1.3") == "FAIL", gates)
    # ... and 0.1 ms is enough to see it: rounding is 2/3 us per pace
    res, rows, gates = fd_run(stamped_native(moved=lambda k: 150 if k == 22 else 0))
    check("stamp identity, mutant 150 us: R1.3 FAIL", gate(gates, "R1.3") == "FAIL", gates)
    res, rows, gates = fd_run(stamped_native(moved=lambda k: 40 if k == 22 else 0))
    check("stamp identity, rounding 40 us: R1.3 PASS", gate(gates, "R1.3") == "PASS", gates)
    n += 4

    # Stamp a whole period ahead of its report time: the sanity bound at - floor >= -33.333 ms.
    def ahead(k):
        return -60_000 if k == 30 else 0  # at 40 ms before ... floor 20 ms after: b = -40 ms
    res, rows, gates = fd_run(stamped_native(noise=ahead))
    check("stamp sanity: bmin -40 ms -> R1.3 FAIL",
          gate(gates, "R1.3") == "FAIL" and "at - floor >= -40.000 ms" in gates, gates)
    n += 1

    # Excluded windows: p != g (a clock fault rebuilt the floor), and a missing stamp at one end.
    res, rows, gates = fd_run(stamped_native(fields=lambda k: {"late": (40, 3000, 900)} if k != 21 else {}))
    p2 = rows["P2 fight-maxroom"]
    check("fd gates: still 0 excluded", p2["fx"] == 0, str(p2["fx"]))
    nat = [ln.replace(" p=60 ", " p=59 ") if " win=21 " in ln and "ph120.fd" in ln else ln
           for ln in stamped_native()]
    res, rows, gates = fd_run(nat)
    p2 = rows["P2 fight-maxroom"]
    check("p != g window excluded", p2["fx"] == 1 and p2["fn"] == 8 and gate(gates, "R1.3") == "PASS",
          f"{p2['fx']} {p2['fn']} {gates}")
    nat = [re.sub(r" floor=0+([0-9a-f]+)$", " floor=0000000000000000", ln)
           if " win=21 " in ln and "ph120.fs" in ln else ln for ln in stamped_native()]
    res, rows, gates = fd_run(nat)
    p2 = rows["P2 fight-maxroom"]
    # window 21 has no stamp (excluded), and window 22 has none at its start (excluded)
    check("zero stamp: 2 windows excluded", p2["fx"] == 2 and gate(gates, "R1.3") == "PASS",
          f"{p2['fx']} {gates}")
    n += 3

    # An older build: no floor=, no clamp, no banner -> NO DATA / n/a, never PASS.
    res, rows, gates = fd_run(stamped_native(stamps=False, banner_bid=None))
    check("old build: mode unknown", res["fd_mode"] is None and "mode: UNKNOWN" in gates, gates)
    check("old build: R1.3 NO DATA", gate(gates, "R1.3") == "NO DATA", gates)
    check("old build: R8.3 NO DATA", gate(gates, "R8.3") == "NO DATA", gates)
    check("old build: R1.4 n/a", gate(gates, "R1.4") == "n/a", gates)
    # another build's banner in a cumulative log is not this build's mode
    res, rows, gates = fd_run(stamped_native(banner_bid="rel:another-build-entirely-000000"))
    check("other build's banner ignored", res["fd_mode"] is None, str(res["fd_mode"]))
    n += 5

    # capk false-fail: OBSERVE's clamp binds on stock's trajectory (kept 1800 of 3000 us, the cap
    # never reached).  The earlier kept_late reads 0.6 (R1.4 >= 0.9 false-fail; MODEL 0.36-0.99 while
    # ON keeps 0.998-1.000); capk = (kept + clamp) / late = 1.0.
    g2 = {"late": (40, 3000, 900), "kept": 1800, "over": (10, 1200), "clamp": (10, 1200)}
    res, rows, gates = fd_run(stamped_native(fields=lambda k: g2))
    p2 = rows["P2 fight-maxroom"]
    check("capk: old kept_late 0.6 (earlier R1.4 false-fail)", near(p2["kept_late"], 0.6), str(p2["kept_late"]))
    check("capk: capk 1.0, clampn 90, R1.4 PASS",
          near(p2["capk"], 1.0) and p2["clampn"] == 90 and gate(gates, "R1.4") == "PASS", gates)
    check("capk: R1.5 record only", "R1.5 gf: record only" in gates, gates)
    # The cap itself binding is still a fail: late 30 ms per window of which 2 paces above the cap
    # (12 ms each), kept 20000, clamp 2000 -> capk 22000 / 30000 = 0.733.
    capb = {"late": (40, 30000, 12000), "kept": 20000, "over": (4, 10000), "clamp": (2, 2000)}
    res, rows, gates = fd_run(stamped_native(fields=lambda k: capb))
    p2 = rows["P2 fight-maxroom"]
    check("cap binds: capk 0.733, R1.4 FAIL",
          near(p2["capk"], 22000 / 30000) and gate(gates, "R1.4") == "FAIL", gates)
    n += 4

    # P3-steady: P3 window 30 carries a load (a stale pace of 40 ms); its steady row drops it and
    # R1.4 reads the steady row.
    load = {"stale": (1, 0, 40_000)}
    res, rows, gates = fd_run(stamped_native(fields=lambda k: load if k == 30 else {}))
    p3, p3s = rows.get("P3 spawn-heavy", {}), rows.get("P3 steady", {})
    check("P3 steady row", p3s.get("windows") == p3.get("windows", 0) - 1 and p3s.get("stale") == 0,
          f"{p3.get('windows')} {p3s.get('windows')} {p3s.get('stale')}")
    check("R1.4 names P3 steady", "P3 steady (" in gates and gate(gates, "R1.3") == "PASS", gates)
    check("no steady row where nothing is stale", "P2 steady" not in rows, str(list(rows)))
    rep = b.report(res)
    check("report: attribution table and gates", "floor-debt attribution" in rep and
          "floor-debt gates" in rep and "P3 steady" in rep, "")
    n += 4

    # ON: the identity is over + stale; R8.2 UPS from sums, R8.7 gfstuck, R8 clamp-cost line.
    on = {"late": (40, 3000, 900), "kept": 3000, "over": (0, 0), "clamp": (0, 0)}
    res, rows, gates = fd_run(stamped_native(mode="on", fields=lambda k: on))
    p2 = rows["P2 fight-maxroom"]
    check("ON: mode on, R8.3 PASS, no R1 lines",
          res["fd_mode"] == "on" and gate(gates, "R8.3") == "PASS" and gate(gates, "R1.3") == "", gates)
    # ON keeps all 3 ms, so the floor moves exactly 2,000,000 us per window: UPS 30.000
    check("ON: ups_sum 30.000, R8.2 PASS", near(p2["ups_sum"], 30.0, 1e-9) and gate(gates, "R8.2") == "PASS",
          f"{p2['ups_sum']} {gates}")
    check("ON: R8.7 PASS", gate(gates, "R8.7") == "PASS", gates)
    res, rows, gates = fd_run(stamped_native(mode="on", fields=lambda k: on,
                                             moved=lambda k: 3000 if k == 22 else 0))
    check("ON mutant (anchor drops what the receipt keeps): R8.3 FAIL", gate(gates, "R8.3") == "FAIL", gates)
    # ON losing 1 ms per window more than it should: UPS 60 / 2.001 = 29.985, still >= 29.9; 7 ms:
    # 29.895 -> FAIL
    res, rows, gates = fd_run(stamped_native(mode="on", fields=lambda k: dict(on, over=(1, 7000), kept=0)))
    p2 = rows["P2 fight-maxroom"]
    check("ON: 7 ms dropped per window -> R8.2 FAIL", near(p2["ups_sum"], 60 / 2.007, 1e-9) and
          gate(gates, "R8.2") == "FAIL", f"{p2['ups_sum']} {gates}")
    n += 5
    print(f"floor debt gates: {n} checks")


def leg_floor_debt_edges() -> None:
    """Floor-debt edge cases: a lost record, a phase without a late pace, what decides R1.4."""
    n = 0
    # One lost record.  Window 21's ph120.t (or every record of window 21) is lost, so
    # window 22's predecessor in the log is window 20: its stamp delta spans two windows (the earlier
    # analyzer read fres_obs 2003 ms, R1.3 FAIL) and its at-delta 4 s holds one window's g (ups_sum 26.7).
    for what, drop in (("ph120.t of window 21", lambda ln: "ph120.t " in ln and " win=21 " in ln),
                       ("every record of window 21", lambda ln: " win=21 " in ln)):
        res, rows, gates = fd_run([ln for ln in stamped_native(mode="on", fields=lambda k: {
            "late": (40, 3000, 900), "kept": 3000, "over": (0, 0), "clamp": (0, 0)}) if not drop(ln)])
        p2 = rows["P2 fight-maxroom"]
        check(f"lost {what}: window 22 excluded (fx 1, gapw 1), R8.3 PASS",
              p2.get("fx") == 1 and p2.get("gapw") == 1 and near(p2.get("fres_on"), 0.0)
              and gate(gates, "R8.3") == "PASS", f"{p2.get('fx')} {p2.get('gapw')} {p2.get('fres_on')} {gates}")
        # 8 windows left (17-20, 22-25); 22 is not consecutive: 7 x 60 g over 7 x 2 s = 30.000
        check(f"lost {what}: ups_sum over consecutive windows 30.000",
              near(p2.get("ups_sum"), 30.0, 1e-9) and gate(gates, "R8.2") == "PASS", f"{p2.get('ups_sum')}")
        n += 2
    res, rows, gates = fd_run([ln for ln in stamped_native() if not ("ph120.t " in ln and " win=21 " in ln)])
    check("lost record, OBSERVE: R1.3 PASS", gate(gates, "R1.3") == "PASS" and "1 lost predecessor" in gates,
          gates)
    n += 1
    # A phase without a late pace has no capk; it reads n/a and does not fail R1.4.
    none = {"late": (0, 0, 0), "kept": 0, "over": (0, 0), "clamp": (0, 0)}
    res, rows, gates = fd_run(stamped_native(fields=lambda k: none))
    check("no late pace: R1.4 PASS, phases n/a", gate(gates, "R1.4") == "PASS" and
          "P1 n/a (no late pace)" in gates, gates)
    # ... and one phase without lateness does not hide another phase's binding cap
    capb = {"late": (40, 30000, 12000), "kept": 20000, "over": (4, 10000), "clamp": (2, 2000)}
    res, rows, gates = fd_run(stamped_native(fields=lambda k: capb if 17 <= k <= 25 else none))
    check("no late pace in P1, cap binds in P2: R1.4 FAIL", gate(gates, "R1.4") == "FAIL" and
          "P1 n/a (no late pace)" in gates and "P2 0.733" in gates, gates)
    n += 2
    # R1.4 prints what decides it: lateness above the cap (sum late - sum(kept + clamp)),
    # windows whose largest late pace is above the cap, the largest late pace.  capb: 8 ms above
    # the cap per window, 9 windows in P2, every window's max 12 ms > 8 ms.
    res, rows, gates = fd_run(stamped_native(fields=lambda k: capb))
    check("R1.4 above-cap detail", "P2 above cap 72.0 ms in 9 win (max late 12.0 ms)" in gates, gates)
    n += 1
    print(f"floor debt edges: {n} checks")


# ----------------------------------------------------------------------------------- run gates
# R1.6, R8.5, R8.8 and R8.9 compare runs.  An earlier version defined R1.6 and R8.9 against
# one seeded OFF run with a tolerance taken from one other pair of runs, R8.5 with no tolerance,
# and R8.8 on MARK fields that differ between two identical runs.  The R0 device pair (same eboot,
# same seed) reproduces both defects; the replacements (bench_run.run_gates) are an ABAB pooled-t
# rule, OBSERVE's own costs (emit time, viol/reset) and the script-pinned MARK fields.
R0_PAIR = ("bench-A2S-20261025", "bench-A2R0-20261025")  # int-final-a2 twice, seed 1028899765
A2_UNSEEDED, AOFFS = "bench-A2-20261025", "bench-AOFFS-20261025"
OLD_R88_FIELDS = ("level", "roomtype", "roomidx", "frame", "npc", "familiars", "projectiles")
T975_SCIPY = {1: 12.7062, 2: 4.3027, 3: 3.1824, 4: 2.7764, 5: 2.5706, 6: 2.4469, 7: 2.3646, 8: 2.306,
              9: 2.2622, 10: 2.2281, 11: 2.201, 12: 2.1788, 13: 2.1604, 14: 2.1448, 15: 2.1314,
              16: 2.1199, 17: 2.1098, 18: 2.1009, 19: 2.093, 20: 2.086}  # scipy 1.15.2 t.ppf(0.975, df)


def marks_of(game_text: str) -> dict:
    """Test-local MARK parser (independent of bench_run's): name -> {field: value}, NONCE excluded,
    the last occurrence of a name counts."""
    out = {}
    for line in game_text.splitlines():
        m = re.search(r"\[repro_console\] MARK (.+?) \| (.*)$", line)
        if m and not m.group(1).startswith("NONCE"):
            out[m.group(1)] = {k: v.strip() for part in m.group(2).split(" | ")
                               for k, v in re.findall(r"(\w+)=(.*?)(?=\s+\w+=|$)", part)}
    return out


def device_run(root: Path, name: str, game_edit=None) -> dict | None:
    d = root / name
    if not all((d / f).exists() for f in ("native.log", "game.log", "manifest.json")):
        return None
    game = (d / "game.log").read_text(encoding="utf-8", errors="replace")
    if game_edit:
        game = game_edit(game)
    res = b.diagnose((d / "native.log").read_text(encoding="utf-8", errors="replace"), game,
                     json.loads((d / "manifest.json").read_text(encoding="utf-8"))["nonce"])
    res["_game"] = game
    return res


def t_cdf(x: float, df: int) -> float:
    """Student-t CDF by Simpson integration of the density (no scipy needed)."""
    import math
    c = math.exp(math.lgamma((df + 1) / 2) - math.lgamma(df / 2)) / math.sqrt(df * math.pi)
    n, lo = 200_000, -400.0
    h = (x - lo) / n
    f = [c * (1 + (lo + i * h) ** 2 / df) ** (-(df + 1) / 2) for i in range(n + 1)]
    tail = c * df ** ((df + 1) / 2) / df * (-lo) ** (-df)  # integral of the density below lo
    return tail + h / 3 * (f[0] + f[-1] + 4 * sum(f[1:-1:2]) + 2 * sum(f[2:-1:2]))


def leg_run_gates_device(root: Path | None) -> None:
    runs = {}
    if root and root.is_dir():
        for name in R0_PAIR + (A2_UNSEEDED, AOFFS):
            r = device_run(root, name)
            if r is not None:
                runs[name] = r
    if len(runs) < 4:
        print(f"run gates, device: SKIP ({len(runs)} of 4 runs under --smoke-root)")
        return
    s, r0, a2, aoff = (runs[k] for k in R0_PAIR + (A2_UNSEEDED, AOFFS))
    n = 0
    check("R0 pair seeded and identical rooms", b.rooms_line(s, r0).startswith("rooms: identical"),
          b.rooms_line(s, r0))
    ph = {name: {p["phase"].split()[0]: p for p in x["phases"]} for name, x in runs.items()}
    S, R, A = ph[R0_PAIR[0]], ph[R0_PAIR[1]], ph[A2_UNSEEDED]
    steady = ("P1", "P2", "P3", "P5")
    # --- the earlier R1.6 (|R0 - A2S| <= |A2 - A2S| for UPS and presents/s, phase
    # medians) and the earlier R8.5 (presents/s >= the seeded OFF run, no tolerance) fail the same eboot.
    old16 = {(p, k) for p in steady for k in ("ups", "rps")
             if abs(R[p][k] - S[p][k]) > abs(A[p][k] - S[p][k])}
    check("earlier R1.6 reproduced: fails an identical build in P2/P5 UPS and P5 presents",
          old16 == {("P2", "ups"), ("P5", "ups"), ("P5", "rps")}, str(sorted(old16)))
    old85 = {p for p in steady if R[p]["rps"] < S[p]["rps"]}
    check("earlier R8.5 reproduced: fails an identical build in P1, P2, P5", old85 == {"P1", "P2", "P5"},
          str(sorted(old85)))
    # --- the earlier R8.8 'stable' fields differ between the two identical OFF runs.
    ms, mr = marks_of(s["_game"]), marks_of(r0["_game"])
    old88 = {(m, f) for m in ms for f in OLD_R88_FIELDS if ms[m].get(f) != mr.get(m, {}).get(f)}
    check("earlier R8.8 reproduced: 'stable' fields differ (P3 npc, P4 projectiles)",
          old88 == {("P3 spawn-heavy", "npc"), ("P4 transitions", "projectiles")}, str(sorted(old88)))
    check("R8.8: 7 MARKs besides NONCE", len(ms) == 7 and set(ms) == set(mr), str(sorted(ms)))
    check("bench_run's MARK parser agrees with the test's", s["marks_fields"] == ms and r0["marks_fields"] == mr, "")
    # Quiet-window inputs, MEASURED (scratch quiet.py, 2026-10-26): A2S P3 loses windows 23-25 and
    # 31 (drop(t) 12/210/151/4), A2R0 P3 windows 23-24 (35/8); P1 sums 54.35 / 54.80 presents/s.
    check("quiet windows: P3 5 (A2S) / 7 (A2R0), P1 9 / 9", (S["P3"]["quiet"], R["P3"]["quiet"],
          S["P1"]["quiet"], R["P1"]["quiet"]) == (5, 7, 9, 9), str((S["P3"]["quiet"], R["P3"]["quiet"])))
    check("quiet presents/s P1 54.35 / 54.80", near(S["P1"]["pps_q"], 54.35, 0.005) and
          near(R["P1"]["pps_q"], 54.80, 0.005), f"{S['P1']['pps_q']} {R['P1']['pps_q']}")
    n += 7
    # --- the current gates on the same runs
    txt = "\n".join(b.run_gates([s], [r0], "observe", expect_mode=False))
    check("R1 identical build (A2R0 as OBSERVE stand-in): R1.6a/R1.6b PASS, nothing FAILs",
          gate(txt, "R1.6a") == "PASS" and gate(txt, "R1.6b") == "PASS" and ": FAIL" not in txt, txt)
    txt = "\n".join(b.run_gates([s], [r0], "on", expect_mode=False))
    check("R8 one OFF + one ON run (a single pair): R8.5/R8.9 NO DATA, never FAIL",
          gate(txt, "R8.5") == "NO DATA" and gate(txt, "R8.9") == "NO DATA" and gate(txt, "R8.8a") == "PASS"
          and ": FAIL" not in txt, txt)
    txt = "\n".join(b.run_gates([s, r0], [r0, s], "on", expect_mode=False))
    check("R8 trivial negative control (the arms swapped): PASS", gate(txt, "R8.5") == "PASS" and
          gate(txt, "R8.9") == "PASS" and gate(txt, "R8.8a") == "PASS" and ": FAIL" not in txt, txt)
    n += 3
    # Positive control: AOFFS (recipe A minus the batch, same seed) really shows fewer presents and
    # worse pacing in P1 (MEASURED quiet-window presents/s 48.90 vs 54.35 / 54.80).  As a stand-in ON
    # arm (run duplicated: its within-arm spread is 0) it must fail R8.5 and R8.9 in P1.
    txt = "\n".join(b.run_gates([s, r0], [aoff, aoff], "on", expect_mode=False))
    check("R8 positive control (AOFFS twice): R8.5 FAIL in P1", gate(txt, "R8.5") == "FAIL" and
          "P1 -5.68" in txt, txt)
    check("R8 positive control: R8.9 FAIL in P1", gate(txt, "R8.9") == "FAIL" and "P1 +6.09" in txt, txt)
    check("R8.8a holds for AOFFS (script-pinned fields)", gate(txt, "R8.8a") == "PASS", txt)
    n += 3
    # Script-pinned fields: a different room at one MARK fails R8.8a; the unstable fields do not.
    bad = device_run(root, R0_PAIR[1], lambda g: g.replace("roomidx=72 frame=1290", "roomidx=73 frame=1290"))
    txt = "\n".join(b.run_gates([s], [bad], "on", expect_mode=False))
    check("R8.8a FAILs on a different room at P3", gate(txt, "R8.8a") == "FAIL" and "P3 spawn-heavy roomidx" in txt,
          txt)
    txt = "\n".join(b.run_gates([s], [r0], "on", expect_mode=False))
    check("R8.8b divergence lists npc/projectiles as information",
          "R8.8b" in txt and "npc" in txt and "projectiles" in txt and gate(txt, "R8.8b") == "", txt)
    # An unseeded run is not comparable: no comparison verdict, never PASS/FAIL.
    txt = "\n".join(b.run_gates([s], [a2], "on", expect_mode=False))
    check("unseeded run: NOT COMPARABLE", "NOT COMPARABLE" in txt and gate(txt, "R8.8a") == "NO DATA", txt)
    n += 3
    print(f"run gates, device logs (the earlier R1.6/R8.5/R8.8 reproduced and replaced): {n} checks")


MARK_STATE = "greed=false difficulty=0 stage=1 stagetype=4 level=Downpour I roomtype=1"
# Verbatim from bench-A2S-20261025/game.log (time= aside): roomidx, frame and the counts per MARK.
MARK_A2S = {
    "BENCH START": ("roomidx=84 frame=30 time=33859",
                    "entities=7 npc=0 tears=0 familiars=5 projectiles=0 effects=1 lasers=0 other=0"),
    "P1 start-idle": ("roomidx=56 frame=90 time=37160",
                      "entities=24 npc=2 tears=0 familiars=5 projectiles=0 effects=16 lasers=0 other=0"),
    "P2 fight-maxroom": ("roomidx=84 frame=690 time=58057",
                         "entities=10 npc=0 tears=0 familiars=5 projectiles=0 effects=4 lasers=0 other=0"),
    "P3 spawn-heavy": ("roomidx=72 frame=1290 time=79330",
                       "entities=25 npc=6 tears=0 familiars=5 projectiles=0 effects=13 lasers=0 other=0"),
    "P4 transitions": ("roomidx=69 frame=1905 time=108236",
                       "entities=48 npc=7 tears=0 familiars=5 projectiles=4 effects=29 lasers=2 other=0"),
    "P5 end-idle": ("roomidx=69 frame=2445 time=131174",
                    "entities=35 npc=4 tears=0 familiars=5 projectiles=0 effects=25 lasers=0 other=0"),
    "BENCH DONE": ("roomidx=84 frame=2745 time=141665",
                   "entities=15 npc=0 tears=0 familiars=5 projectiles=0 effects=9 lasers=0 other=0"),
}


def synth_run(mode: str, dp=0, emit: int = 5700, viol=(), edit=None, drop=(), reset=(), early=None) -> dict:
    """A synthetic bench run: 'off' (no banner, no ph120.fd/fs), 'observe' or 'on'.  Every window
    is quiet (drop 0) except those in drop (W41's drop(t,us)=5,83334 kept), presents p = 57 + dp
    per 2.003-s window (the same floor progression in every mode), ph120.w emit = emit us, viol
    p = 1 / reset = 1 in the windows listed, MARK lines with fields; edit(name, (state, counts)) ->
    (state, counts) changes one MARK; dp may be a function of the window; early = a MARK line put
    right after the NONCE marker (a replayed list: the last occurrence of a name counts)."""
    nat = stamped_native(mode="on" if mode == "on" else "observe", banner_bid=None if mode == "off" else BID,
                         fields=(lambda k: {"over": (40, 3000)}) if mode == "on" else None)
    if mode == "off":
        nat = [ln for ln in nat if "ph120.fd " not in ln and "ph120.fs " not in ln]
    out = []
    for ln in nat:
        m = re.search(r"\[kage-vita\] ph120\.s bid=(\S+) win=(\d+) loops=(\d+) ", ln)
        if m:
            k = int(m.group(2))
            ln = ln.replace("cad(w,s,d,m,g,r,b,p)=120,120,120,120,60,57,57,57",
                            f"cad(w,s,d,m,g,r,b,p)=120,120,120,120,60,57,57,{57 + (dp(k) if callable(dp) else dp)}")
            if k not in drop:
                ln = ln.replace("drop(t,us)=5,83334", "drop(t,us)=0,0")
            if k in reset:
                ln = ln.replace(" reset=0 ", " reset=1 ")
            if k in viol:
                ln = ln.replace("viol(d,p,g,s,o)=0,0,0,0,0", "viol(d,p,g,s,o)=0,1,0,0,0")
            out += [ln, f"[kage-vita] ph120.w bid={m.group(1)} win={k} loops={m.group(3)} emit(us,max)={emit},{emit}"]
            continue
        out.append(ln)
    nonce = "0123456789ab"
    game = []
    for g in good_game(nonce):
        m = re.search(r"MARK (.+?) \| x$", g)
        if m and m.group(1) in MARK_A2S:
            state, counts = MARK_A2S[m.group(1)]
            if edit:
                state, counts = edit(m.group(1), (state, counts))
            g = rc(f"MARK {m.group(1)} | {MARK_STATE} {state} | {counts}")
        game.append(g)
        if early and "MARK NONCE" in g:
            game.append(rc(early))
    return b.diagnose("\n".join(out) + "\n", "\n".join(game) + "\n", nonce)


def leg_run_gates() -> None:
    """The run gates on synthetic runs, the t table, the ABAB rule's hand values and its
    calibration (false fails on identical builds, power against a real loss)."""
    import random
    n = 0
    # t quantiles: the table equals scipy's, or (no scipy installed) the integrated CDF is 0.975
    try:
        from scipy.stats import t as T
        bad = {df: v for df, v in b.T975.items() if abs(T.ppf(0.975, df) - v) > 1e-4}
        how = "scipy"
    except ImportError:
        bad = {df: v for df, v in b.T975.items() if df in (1, 2, 4, 10) and abs(t_cdf(v, df) - 0.975) > 1e-4}
        how = "integrated CDF"
    check(f"T975 table ({how})", not bad and b.T975 == T975_SCIPY, str(bad))
    check("T975 beyond the table is conservative", b.t975(40) == b.T975[20], str(b.t975(40)))
    n += 2
    # ABAB hand values: OFF 54.35 / 54.80 (A2S / A2R0 P1 quiet presents/s), ON 54.0 / 55.0.
    # sp = sqrt((2 * 0.225^2 + 2 * 0.5^2) / 2) = 0.548293; tol = 4.3027 * sp * 1 = 2.35914
    g = b.abab([54.35, 54.80], [54.0, 55.0], higher_better=True, floor=0.5)
    check("abab hand: delta -0.075, tol 2.359, PASS", near(g["delta"], -0.075, 1e-9) and near(g["tol"], 2.35914, 1e-4)
          and g["df"] == 2 and g["verdict"] == "PASS", str(g))
    g = b.abab([54.35, 54.80], [51.0, 51.5], higher_better=True, floor=0.5)
    # sp = sqrt((0.10125 + 0.125) / 2) = 0.336341; tol = 1.44718; delta = -3.325
    check("abab hand: a 3.3/s loss FAILs", near(g["tol"], 1.44718, 1e-4) and g["verdict"] == "FAIL", str(g))
    g = b.abab([30.0, 30.0], [29.8, 29.8], higher_better=True, floor=0.5)
    check("abab: zero spread -> the practical floor decides (0.2 < 0.5: PASS)", g["tol"] == 0.5 and
          g["verdict"] == "PASS", str(g))
    g = b.abab([40.0, 41.0], [44.5, 45.0], higher_better=False, floor=1.0)
    check("abab lower-is-better: +4.25 ms vs tol 3.40 FAILs", g["verdict"] == "FAIL" and near(g["delta"], 4.25), str(g))
    check("abab: < 2 runs per arm -> NO DATA", b.abab([54.0], [50.0, 51.0], True, 0.5)["verdict"] == "NO DATA"
          and b.abab([54.0, 55.0], [50.0], True, 0.5)["verdict"] == "NO DATA", "")
    n += 5
    # Calibration on iid runs (sigma 10, so the 0.5 floor never decides): ABAB false fails 2.5 %
    # one-sided, the earlier rule (ON >= OFF) 50 %; power: a 5-sigma loss fails ~0.7 of the time.
    rnd = random.Random(20261026)
    trials = 20_000
    ff = sum(b.abab([rnd.gauss(50, 10) for _ in range(2)], [rnd.gauss(50, 10) for _ in range(2)],
                    True, 0.5)["verdict"] == "FAIL" for _ in range(trials)) / trials
    old = sum(rnd.gauss(50, 10) < rnd.gauss(50, 10) for _ in range(trials)) / trials
    pw = sum(b.abab([rnd.gauss(50, 10) for _ in range(2)], [rnd.gauss(0, 10) for _ in range(2)],
                    True, 0.5)["verdict"] == "FAIL" for _ in range(trials)) / trials
    check(f"calibration: ABAB false-fail {ff:.4f} in [0.018, 0.032]", 0.018 <= ff <= 0.032, str(ff))
    check(f"calibration: earlier R8.5 rule false-fail {old:.3f} ~ 0.5", 0.48 <= old <= 0.52, str(old))
    check(f"calibration: 5-sigma loss detected {pw:.3f} in [0.6, 0.8]", 0.6 <= pw <= 0.8, str(pw))
    print(f"  ABAB calibration: false-fail {ff:.4f} (earlier rule {old:.3f}), 5-sigma power {pw:.3f}")
    n += 3
    # --- R1 on synthetic runs: OBSERVE vs one OFF run
    off = synth_run("off")
    check("synthetic OFF run ok, no floor-debt mode", off["ok"] and off["fd_mode"] is None, off["verdict"])
    obs = synth_run("observe", emit=5950)
    txt = "\n".join(b.run_gates([off], [obs], "observe"))
    check("R1.6: OBSERVE +0.25 ms emit, no viol: R1.6a/R1.6b PASS", gate(txt, "R1.6a") == "PASS" and
          gate(txt, "R1.6b") == "PASS" and "+0.250 ms" in txt and ": FAIL" not in txt, txt)
    check("R1.6c is information", "R1.6c" in txt and gate(txt, "R1.6c") == "", txt)
    txt = "\n".join(b.run_gates([off], [synth_run("observe", emit=6800)], "observe"))
    check("R1.6b: +1.1 ms emit FAILs", gate(txt, "R1.6b") == "FAIL", txt)
    txt = "\n".join(b.run_gates([off], [synth_run("observe", viol=(20,))], "observe"))
    check("R1.6a: one viol in P2 FAILs", gate(txt, "R1.6a") == "FAIL" and "P2 viol 1" in txt, txt)
    txt = "\n".join(b.run_gates([off], [synth_run("observe", reset=(30,))], "observe"))
    check("R1.6a: one clock reset in P3 FAILs", gate(txt, "R1.6a") == "FAIL" and "P3 viol 0 reset 1" in txt, txt)
    txt = "\n".join(b.run_gates([off], [off], "observe"))
    check("mode check: an OFF build given as OBSERVE FAILs MODE", gate(txt, "MODE") == "FAIL", txt)
    # quiet windows: a window with a stale wrapper drop (drop(t) > 0) or a clock reset leaves the
    # presents/UPS/gap95 inputs; P2 = windows 17..25, 2.003 s each, p = 57
    r2 = {p["phase"].split()[0]: p for p in synth_run("off", drop=(20,), reset=(22,))["phases"]}["P2"]
    check("quiet: drop and reset windows excluded (9 -> 7), pps_q = 57 / 2.003",
          r2["quiet"] == 7 and near(r2["pps_q"], 57 / 2.003, 1e-6) and r2["reset"] == 1, str(r2.get("quiet")))
    n += 8
    # --- R8 on synthetic ABAB sessions
    offs = [synth_run("off", dp=0), synth_run("off", dp=1)]
    txt = "\n".join(b.run_gates(offs, [synth_run("on", dp=1), synth_run("on", dp=0)], "on"))
    check("R8 neutral ABAB: R8.5/R8.9/R8.8a PASS", gate(txt, "R8.5") == "PASS" and gate(txt, "R8.9") == "PASS"
          and gate(txt, "R8.8a") == "PASS" and ": FAIL" not in txt, txt)
    txt = "\n".join(b.run_gates(offs, [synth_run("on", dp=-6), synth_run("on", dp=-7)], "on"))
    check("R8.5: ON 3.2 presents/s down FAILs", gate(txt, "R8.5") == "FAIL", txt)
    txt = "\n".join(b.run_gates(offs, [synth_run("on")], "on"))
    check("R8.5: one ON run -> NO DATA (ABAB needs two per arm)", gate(txt, "R8.5") == "NO DATA", txt)
    npc = synth_run("on", edit=lambda name, sc: (sc[0], sc[1].replace("npc=6", "npc=7"))
                    if name == "P3 spawn-heavy" else sc)
    room = synth_run("on", edit=lambda name, sc: (sc[0].replace("roomidx=72", "roomidx=73"), sc[1])
                     if name == "P3 spawn-heavy" else sc)
    txt = "\n".join(b.run_gates(offs, [npc, npc], "on"))
    check("R8.8a ignores npc (unstable); R8.8b shows it", gate(txt, "R8.8a") == "PASS" and
          "npc 1.00 (OFF-OFF 0.00" in txt, txt)
    txt = "\n".join(b.run_gates(offs, [room, room], "on"))
    check("R8.8a: another room at P3 FAILs", gate(txt, "R8.8a") == "FAIL", txt)
    check("R8 lines in observe mode absent", "R8.5" not in "\n".join(b.run_gates([off], [obs], "observe")), "")
    # P5 (3 windows, 3.8 presents/s between identical runs) is information: a loss there alone
    # does not fail R8.5.  P5 = windows 41..44 here; -8 presents per window = -4.0/s.
    p5 = [synth_run("on", dp=lambda k: -8 if k >= 41 else 0) for _ in range(2)]
    txt = "\n".join(b.run_gates(offs, p5, "on"))
    check("R8.5: a loss in P5 only is information (PASS, P5 LOSS printed)", gate(txt, "R8.5") == "PASS" and
          "information, LOSS" in txt, txt)
    # A replayed MARK list (an @seed restart): R8.8a reads the last occurrence of each name.
    dup = synth_run("on", early=f"MARK BENCH START | {MARK_STATE} roomidx=99 frame=0 time=1 | entities=1")
    check("MARK fields: the last occurrence counts", dup["marks_fields"]["BENCH START"]["roomidx"] == "84"
          and dup["marks_fields"]["BENCH START"]["level"] == "Downpour I", str(dup["marks_fields"].get("BENCH START")))
    n += 8
    print(f"run gates, synthetic: {n} checks")


# ----------------------------------------------------------------------------------- negative
def mutate(nat: list, game: list, kind: str):
    nat, game = list(nat), list(game)
    if kind == "restored":
        nat.insert(3, "[40.000 thr 0x40010003] ISAAC VITA LAUNCH SAVE SLOT: event=restored slot=2 was=0x00000001 "
                      "manager=0x9a001000 restored=1 trigger=ux0:/data/isaacr001/data/836319872/save2.dat")
    elif kind == "late":
        nat[2] = nat[2].replace("resources/stringtable.sta", "data/836319872/save1.dat")
    elif kind == "noapplied":
        del nat[2]
    elif kind == "noslot":
        nat[1] = nat[1].replace("status=ok", "status=no-slot").replace(" slot=2", " slot=0")
    elif kind == "slot3":
        nat[1] = nat[1].replace(" slot=2", " slot=3")
    elif kind == "noargs":
        del nat[1]
    elif kind == "file1load":
        game.insert(4, "[INFO] - Loading GameState 1 from local folder!")
    elif kind == "rng1":
        game = [g.replace("[New, 2]", "[New, 1]") for g in game]
    elif kind == "tour":
        game.insert(4, "[INFO] - Running Lua Script: ux0:data/isaacr001/mods/repro_tour/main.lua")
    elif kind == "nonce":
        game = [g.replace("MARK NONCE 0123456789ab", "MARK NONCE ffffffffffff") for g in game]
    elif kind == "nosave":
        game.insert(5, rc("no save data (data/repro_console/save<slot>.dat missing) - nothing to do"))
    elif kind == "align":
        game = [g.replace("Room 1.85(", "Room 1.86(") for g in game]
    elif kind == "incomplete":
        i = game.index(rc("MARK BENCH DONE | x"))
        game = game[:i]
    elif kind == "emptyphase":  # P3 marker moved next to P4's: P3 has no room change of its own
        game.remove(rc("MARK P3 spawn-heavy | x"))
        game.insert(game.index(rc("MARK P4 transitions | x")), rc("MARK P3 spawn-heavy | x"))
    elif kind == "fault":
        nat.append("[50.000 thr 0x40010003] guest stop: FIRST ARM FAULT at pc=0x81234567")
    # bench validity gate
    elif kind == "eidfail":  # Pc game.log:55-61 in place of the banner and the heap line
        i = game.index(EID_LOADED[0])
        game[i:i + len(EID_LOADED)] = EID_FAIL
    elif kind == "eidnobanner":  # EID ran, its heap line is there, the banner is not
        game = [g for g in game if g not in EID_LOADED[:3]]
    elif kind == "noeid":
        game = [g for g in game if g not in EID_LOADED and not g.endswith(b.EID_MAIN)]
    elif kind == "heapdrift":
        game = [g.replace("Lua mem usage: 11215 KB", "Lua mem usage: 11214 KB") for g in game]
    elif kind == "retryfail":
        nat[3:3] = [lf_stats("first-load"), lf_retry("unrecovered"),
                    lf_stats("loading-complete", read_err=1, retries=3, unrecovered=1)]
    elif kind == "retrychanged":
        nat[3:3] = [lf_stats("first-load"), lf_retry("changed", status=3, attempts=2),
                    lf_stats("loading-complete", read_err=1, retries=1, changed=1)]
    elif kind == "retryok":
        nat[3:3] = [lf_stats("first-load"), lf_retry("recovered", status=0, attempts=2),
                    lf_stats("loading-complete", read_err=1, retries=1, recovered=1)]
    elif kind == "retryenodev":
        nat[3:3] = [lf_stats("first-load"), lf_retry("recovered", status=0, attempts=2, sce="0x80010013"),
                    lf_stats("loading-complete", read_err=1, retries=1, recovered=1)]
    elif kind == "intercept2":  # Pc's extra intercept at ~33.7 s, then the kill's
        nat += INTERCEPT_LINES
    elif kind == "intercept1":  # the harness kill alone
        nat += INTERCEPT_LINES[2:]
    return nat, game


# ISAAC_VITA_LUA_LOADFILE_RETRY receipts in the exact formats the runtime prints.
def lf_stats(why: str, loads: int = 117, **kw) -> str:
    c = dict(read_err=0, open_transient=0, open_err=0, isdir=0, retries=0, recovered=0, unrecovered=0,
             changed=0, observed=0, budget_hit=0, sleep_us=0, logged=0)
    c.update(kw)
    if why == "first-load":
        loads = 0
    return (f"[{'20.100' if why == 'first-load' else '30.019'} thr 0x40010003] KAGE VITA LUA LOADFILE STATS: "
            f"why={why} max=3 delay_us=2000,16000,128000 budget=16/1000000 probe=0 inject=- loads={loads} "
            + " ".join(f"{k}={v}" for k, v in c.items()))


def lf_retry(result: str, status: int = 7, attempts: int = 4, sce: str = "-",
             path: str = "ux0:/data/isaacr001/mods/836319872/descriptions/ab+/cs_cz.lua") -> str:
    return (f"[29.700 thr 0x40010003] KAGE VITA LUA LOADFILE RETRY: n=1 result={result} kind=read "
            f"attempts={attempts} status={status} fail_us=2100 retry_us=18200 sce={sce} sce_at=1/129 "
            f"sce_len=1024 sce_us=900 close=0x00000000 inj=0 path={path} why=Success")


# bench-D4A4Pc-20261026/native.log:272, :278 and its last line, verbatim: an intercept at the first
# sample (cleared 0.2 s later), and the end-of-run one.
INTERCEPT_LINES = [
    "[kage-vita-input] xinput snapshot seq=1 buttons=00000000->00010000 down=00010000 up=00000000 "
    "synthetic=0000/0/0->0000/0/0 sticks=128,128,128,128->127,132,134,119",
    "[kage-vita-input] xinput snapshot seq=2 buttons=00010000->00000000 down=00000000 up=00010000 "
    "synthetic=0000/0/0->0000/0/0 sticks=127,132,134,119->127,132,135,119",
    "[kage-vita-input] xinput snapshot seq=6238 buttons=00000000->00010000 down=00010000 up=00000000 "
    "synthetic=0000/0/0->0000/0/0 sticks=127,132,134,119->127,132,134,120",
]


NEGATIVE = {"restored": "SLOT_RESTORED", "late": "SLOT_LATE", "noapplied": "SLOT_NOT_APPLIED",
            "noslot": "LAUNCH_ARGS_STATUS", "slot3": "SLOT_NOT_2", "noargs": "NO_LAUNCH_ARGS",
            "file1load": "FILE1_LOAD", "rng1": "RNG_SLOT", "tour": "EXTRA_LUA", "nonce": "NONCE",
            "nosave": "NO_SAVE_DATA", "align": "ROOM_ALIGN", "incomplete": "INCOMPLETE",
            "emptyphase": "PHASE_EMPTY", "fault": "GUEST_STOP",
            # bench validity gate
            "eidfail": "MOD_LUA_ERROR", "eidnobanner": "EID_NOT_LOADED", "noeid": "NO_EID",
            "heapdrift": "EID_HEAP_MISMATCH", "retryfail": "LUA_LOADFILE_UNRECOVERED",
            "retrychanged": "LUA_LOADFILE_UNRECOVERED", "retryenodev": "SUSPEND_ENODEV",
            "intercept2": "SYSTEM_INTERCEPT"}
# The codes the bench validity gate added (a FAIL with one of these is the gate's, not older rules').
GATE_CODES = {"MOD_LUA_ERROR", "LUA_FILE_ERROR", "EID_ERROR", "MOD_MAIN_CHUNK_ERROR", "NO_EID", "EID_NOT_LOADED",
              "EID_HEAP_MISSING", "EID_HEAP_MISMATCH", "LUA_LOADFILE_UNRECOVERED", "SYSTEM_INTERCEPT",
              "NO_HARNESS_KILL"}
EIDFAIL_CODES = {"MOD_LUA_ERROR", "LUA_FILE_ERROR", "EID_ERROR", "EID_NOT_LOADED", "MOD_MAIN_CHUNK_ERROR",
                 "EID_HEAP_MISSING"}


def leg_negative() -> None:
    nonce = "0123456789ab"
    nat, game = good_native(), good_game(nonce)
    for kind, code in NEGATIVE.items():
        if kind == "rng1":  # a --set-stage start prints no 'RNG Start Seed' line: the restart-format log has one
            n2, g2 = mutate(good_native(ROOMS_RESTART), good_game_restart(nonce), kind)
        else:
            n2, g2 = mutate(nat, game, kind)
        check(f"negative {kind} mutates", (n2, g2) != (nat, game) or kind == "rng1", "mutation is a no-op")
        res = b.diagnose("\n".join(n2) + "\n", "\n".join(g2) + "\n", nonce)
        check(f"negative {kind}", (not res["ok"]) and code in codes(res), f"{code} not in {sorted(codes(res))}")
    print(f"negative: {len(NEGATIVE)} mutations")


def leg_validity() -> None:
    """The bench validity gate on the synthetic run: exact code
    sets, levels, the verdict order, the pin override, the loadfile receipts, the intercept count,
    the kill record, the stale-JSON refusal and compare's EID line."""
    nonce = "0123456789ab"
    nat, game = good_native(), good_game(nonce)

    def run(kind=None, **kw):
        n2, g2 = mutate(nat, game, kind) if kind else (nat, game)
        return b.diagnose("\n".join(n2) + "\n", "\n".join(g2) + "\n", nonce, **kw)
    good = run()
    check("validity: good run ok, EID fields", good["ok"] and good["eid_version"] == "5.23_bc0551a"
          and good["eid_heap"] == [11215, 166] and good["lua_errors"] == [] and good["intercepts"] == []
          and good["loadfile"] is None, {k: good[k] for k in ("verdict", "eid_version", "eid_heap", "lua_errors")})
    res = run("eidfail")
    check("eidfail: FAIL codes = the six (no others)", codes(res) == EIDFAIL_CODES, sorted(codes(res)))
    check("eidfail: verdict names MOD_LUA_ERROR first", res["verdict"].startswith("BENCH FAILED (MOD_LUA_ERROR"),
          res["verdict"][:80])
    check("eidfail: result fields", res["eid_version"] is None and res["eid_heap"] is None
          and [c for c, _ in res["lua_errors"]] == ["EID_ERROR", "LUA_FILE_ERROR", "EID_ERROR", "EID_ERROR",
                                                     "EID_ERROR", "MOD_MAIN_CHUNK_ERROR", "MOD_LUA_ERROR"],
          res["lua_errors"])
    check("eidfail: the 'cannot read' path keeps its 'ux0:' colon", any(
        t.endswith("cannot read ux0:/data/isaacr001/mods/836319872/descriptions/ab+/cs_cz.lua: Success")
        for c, t in res["lua_errors"] if c == "LUA_FILE_ERROR"), res["lua_errors"])
    res = run("eidnobanner")
    check("eidnobanner: EID_NOT_LOADED only", codes(res) == {"EID_NOT_LOADED"}, sorted(codes(res)))
    res = run("noeid")
    check("noeid: NO_EID is a FAIL now (was WARN), with EID_NOT_LOADED",
          codes(res) == {"NO_EID", "EID_NOT_LOADED"} and "NO_EID" not in codes(res, "WARN"), sorted(codes(res)))
    res = run("heapdrift")
    check("heapdrift: EID_HEAP_MISMATCH only", codes(res) == {"EID_HEAP_MISMATCH"}, sorted(codes(res)))
    res = run("heapdrift", eid_heap_pin=(11214, 166))
    check("heapdrift + --eid-heap-pin 11214:166: ok, override in the verdict",
          res["ok"] and "--eid-heap-pin 11214:166" in res["verdict"], res["verdict"])
    res = run(eid_heap_pin=(11214, 166))
    check("good run + a wrong --eid-heap-pin: EID_HEAP_MISMATCH", codes(res) == {"EID_HEAP_MISMATCH"}, sorted(codes(res)))
    unp = b.diagnose("\n".join(nat) + "\n", "\n".join(game).replace("v5.23_bc0551a loaded", "v9.99_test loaded")
                     + "\n", nonce)
    check("unpinned EID version: WARN EID_HEAP_UNPINNED, still ok",
          unp["ok"] and "EID_HEAP_UNPINNED" in codes(unp, "WARN"), unp["verdict"])
    res = run("retryfail")
    check("retryfail: LUA_LOADFILE_UNRECOVERED only", codes(res) == {"LUA_LOADFILE_UNRECOVERED"}, sorted(codes(res)))
    check("retryfail: loadfile = the loading-complete STATS", (res["loadfile"] or {}).get("why") == "loading-complete"
          and res["loadfile"].get("loads") == 117 and res["loadfile"].get("unrecovered") == 1, res["loadfile"])
    res = run("retrychanged")
    check("retrychanged: result=changed is a FAIL", codes(res) == {"LUA_LOADFILE_UNRECOVERED"}, sorted(codes(res)))
    res = run("retryok")
    check("retryok: ok, WARN LUA_LOADFILE_RECOVERED", res["ok"] and "LUA_LOADFILE_RECOVERED" in codes(res, "WARN"),
          res["verdict"] + str(codes(res, "WARN")))
    # a recovered load moves the heap: WARN, not FAIL (EID_HEAP_MISMATCH's level)
    n2, g2 = mutate(nat, game, "retryok")
    g2 = [g.replace("Lua mem usage: 11215 KB", "Lua mem usage: 11216 KB") for g in g2]
    res = b.diagnose("\n".join(n2) + "\n", "\n".join(g2) + "\n", nonce)
    check("recovered + heap drift: EID_HEAP_MISMATCH is a WARN", res["ok"] and "EID_HEAP_MISMATCH" in codes(res, "WARN"),
          res["verdict"])
    # STATS counters alone (no event line, e.g. past the 8 logged events) still FAIL
    n3 = nat[:3] + [lf_stats("first-load"), lf_stats("loading-complete", read_err=9, retries=27, budget_hit=1)] + nat[3:]
    res = b.diagnose("\n".join(n3) + "\n", "\n".join(game) + "\n", nonce)
    check("STATS budget_hit=1 without an event line: UNRECOVERED", codes(res) == {"LUA_LOADFILE_UNRECOVERED"},
          sorted(codes(res)))
    n3 = nat[:3] + [lf_stats("first-load")] + nat[3:]
    res = b.diagnose("\n".join(n3) + "\n", "\n".join(game) + "\n", nonce)
    check("first-load banner without loading-complete: WARN LUA_LOADFILE_NO_REPORT",
          res["ok"] and "LUA_LOADFILE_NO_REPORT" in codes(res, "WARN"), str(codes(res, "WARN")))
    n3 = nat[:3] + [lf_stats("first-load"), lf_stats("loading-complete", open_err=2)] + nat[3:]
    res = b.diagnose("\n".join(n3) + "\n", "\n".join(game) + "\n", nonce)
    check("open_err=2: WARN LUA_LOADFILE_OPEN_ERROR, ok", res["ok"] and "LUA_LOADFILE_OPEN_ERROR" in codes(res, "WARN"),
          str(codes(res, "WARN")))
    res = run("retryenodev")
    check("retryenodev: SUSPEND_ENODEV from sce=0x80010013 (no errno= text)", codes(res) == {"SUSPEND_ENODEV"},
          sorted(codes(res)))
    sp = b.parse_native(lf_retry("unrecovered", path="ux0:/data/isaacr001/My Mods/a b.lua") + "\n")
    check("RETRY path with spaces parsed whole", sp["lf_retry"] and sp["lf_retry"][0]["path"] ==
          "ux0:/data/isaacr001/My Mods/a b.lua" and sp["lf_retry"][0]["why"] == "Success"
          and sp["lf_retry"][0]["result"] == "unrecovered", sp["lf_retry"])
    res = run("intercept1")
    check("one intercept (the harness kill): ok", res["ok"] and res["intercepts"] == [len(nat) + 1], res["verdict"])
    check("one intercept: no INTERCEPT_UNSEEN", "INTERCEPT_UNSEEN" not in codes(res, "WARN"), sorted(codes(res, "WARN")))
    # a completed run (MARK BENCH DONE) with no intercept at all: the detector was blind,
    # WARN INTERCEPT_UNSEEN, never a FAIL; an incomplete run is not 'completed' and gets no such WARN
    res = b.diagnose("\n".join(nat) + "\n", "\n".join(game) + "\n", nonce)
    check("no intercept, BENCH DONE: WARN INTERCEPT_UNSEEN only, still ok", res["ok"] and res["intercepts"] == []
          and "INTERCEPT_UNSEEN" in codes(res, "WARN") and "INTERCEPT_UNSEEN" not in codes(res), sorted(codes(res, "WARN")))
    cut = [x for x in game if "MARK BENCH DONE" not in x]
    res = b.diagnose("\n".join(nat) + "\n", "\n".join(cut) + "\n", nonce)
    check("no intercept, no BENCH DONE: INCOMPLETE, no INTERCEPT_UNSEEN", "INCOMPLETE" in codes(res)
          and "INTERCEPT_UNSEEN" not in codes(res, "WARN"), (sorted(codes(res)), sorted(codes(res, "WARN"))))
    res = run("intercept2")
    check("intercept2: SYSTEM_INTERCEPT only", codes(res) == {"SYSTEM_INTERCEPT"} and len(res["intercepts"]) == 2,
          sorted(codes(res)))
    # ONE intercept, mid-run, the kill's own not logged.  The fixture's timestamped
    # lines are 0.306, 4.533 and 6.100 s; an intercept is timed by the first one AFTER it (an earlier
    # analyzer took the last one at or before it, so an intercept between 0.306 and 4.533 read 5.794 s),
    # and by the log's end when none follows.
    for after, tail, bad in ((0, 6.1 - 0.306, True), (1, 6.1 - 4.533, False), (2, 0.0, False), (len(nat), 0.0, False)):
        mid = nat[:after] + INTERCEPT_LINES[:2] + nat[after:]
        res = b.diagnose("\n".join(mid) + "\n", "\n".join(game) + "\n", nonce)
        x = b.parse_native("\n".join(mid) + "\n")["intercepts"]
        check(f"one intercept {tail:.3f} s before the log's end: timed", len(x) == 1 and near(good["native_last_s"]
              - x[0]["ts"], tail), x)
        if bad:
            check("one mid-run intercept, 5.794 s of log after it: SYSTEM_INTERCEPT only",
                  codes(res) == {"SYSTEM_INTERCEPT"} and "a mid-run intercept" in res["verdict"]
                  and f"native.log line {after + 1} with 5.8 s" in res["verdict"], res["verdict"])
        else:
            check(f"one intercept {tail:.3f} s before the end (within {b.INTERCEPT_TAIL_S:g} s): ok", res["ok"],
                  res["verdict"])
    cost = nat[:3] + ["[29.740 thr 0x40010003] [isaac-lua] cost win=0 ms=20406 pc=3/5397171 cb=207/5389098 nest=92 "
                      "fin=0 out=2 lost=4 gcvm=4/895"] + nat[3:]
    res = b.diagnose("\n".join(cost) + "\n", "\n".join(game) + "\n", nonce)
    check("cost window lost=4: WARN LUA_COST_LOST, ok", res["ok"] and "LUA_COST_LOST" in codes(res, "WARN"),
          str(codes(res, "WARN")))
    # NO_HARNESS_KILL: only for a manifest with kill_record; the fixture's native log ends at 6.100 s
    last = 6.1
    check("fixture native log ends at 6.1 s", near(good["native_last_s"], last), good["native_last_s"])

    def kill(dt_kill, launch=None, kill_status="ok"):
        rec = {"deploy": {"status": "ok", "start": "2026-10-28T10:00:00+00:00", "end": "2026-10-28T10:00:09+00:00"},
               "launch": launch,
               "kill": {"status": kill_status, "start": f"2026-10-28T10:00:{9 + dt_kill:06.3f}+00:00",
                        "end": f"2026-10-28T10:00:{9.7 + dt_kill:06.3f}+00:00"}}
        return rec
    man = {"kill_record": True}
    res = run(manifest=man)
    check("nokill: NO_HARNESS_KILL", codes(res) == {"NO_HARNESS_KILL"} and "missing" in res["verdict"], res["verdict"])
    check("legacy manifest (no kill_record): exempt", run(manifest={"nonce": nonce})["ok"], "")
    res = run(manifest=man, kill=kill(6.4))
    check("kill 0.3 s after the last line: ok", res["ok"], res["verdict"])
    res = run(manifest=man, kill=kill(8.2))
    check("kill 2.1 s after the last line: NO_HARNESS_KILL", codes(res) == {"NO_HARNESS_KILL"}, res["verdict"])
    res = run(manifest=man, kill=kill(6.4, kill_status="error"))
    check("kill status error: NO_HARNESS_KILL", codes(res) == {"NO_HARNESS_KILL"}, res["verdict"])
    sep = {"status": "ok", "start": "2026-10-28T10:00:20+00:00", "end": "2026-10-28T10:00:21+00:00"}
    res = run(manifest=man, kill=kill(18.3, launch=sep))
    check("separate launch: the clock starts at the launch's end", res["ok"], res["verdict"])
    res = run(manifest=man, kill=kill(6.4, launch=sep))
    check("separate launch: a kill before the log could end is NO_HARNESS_KILL", codes(res) == {"NO_HARNESS_KILL"},
          res["verdict"])
    # kill_record(): bound to the run's own audited save2.dat upload: the first ok deploy
    # after it, the last kill before the next deploy, the last launch between them
    tmp = Path(tempfile.gettempdir())
    r1, r2 = tmp / "bench-KR1-20261028", tmp / "bench-KR2-20261028"
    remote = f"{b.MOD_DATA}/save2.dat"

    def up(run_dir):
        return [{"operation": "upload", "status": s, "time": "u", "local_path": str(run_dir / "save2.dat"),
                 "remote_path": remote} for s in ("started", "ok")]

    def op(name, t0, t1, title="ISAACR001", status="ok", **kw):
        return [dict({"operation": name, "status": "started", "time": t0, "title_id": title}, **kw),
                dict({"operation": name, "status": status, "time": t1, "title_id": title}, **kw)]
    rows = (up(r2) + op("deploy_eboot", "t0", "t1", local_path="X") + op("launch", "t2", "t3")
            + op("kill", "t4", "t5", title="OTHER0001") + op("kill", "t6", "t7"))
    rec = b.kill_record(rows, r2)
    check("kill_record: steps and times", (rec["deploy"]["start"], rec["deploy"]["end"], rec["launch"]["end"],
                                           rec["kill"]["start"], rec["kill"]["end"]) == ("t0", "t1", "t3", "t6", "t7")
          and rec["deploy"]["audit_rows"] == [3, 4], rec)

    def refused(name, rows_, run_dir, eboot=None, why=""):
        try:
            got = b.kill_record(rows_, run_dir, eboot)
            check(name, False, f"accepted: deploy rows {got['deploy']['audit_rows']} kill rows "
                               f"{got['kill']['audit_rows']}")
        except ValueError as exc:
            check(name, why in str(exc), str(exc))
    refused("kill_record refuses another run's eboot", rows, r2, "Y", "not this run's eboot")
    refused("kill_record without a kill refuses", rows[:6], r2, None, "the harness did not kill this run")
    refused("kill_record without this run's upload refuses", rows[2:], r2, None, "no ok upload")
    refused("kill_record: an upload of another run's save2.dat is no anchor", rows, r1, None, "no ok upload")
    # A/A session: run 1 deploys X and is killed; run 2 uploads and deploys the
    # same X and is never killed.  An earlier record took the last kill and the last deploy before
    # it, i.e. run 1's, for run 2 -- with or without the manifest's eboot.
    aa = (up(r1) + op("deploy_eboot", "d1", "d1e", local_path="X") + op("kill", "k1", "k1e")
          + up(r2) + op("deploy_eboot", "d2", "d2e", local_path="X"))
    refused("A/A: run 2 never killed -> refused (eboot X)", aa, r2, "X", "the harness did not kill this run")
    refused("A/A: run 2 never killed -> refused (no eboot)", aa, r2, None, "the harness did not kill this run")
    rec = b.kill_record(aa + op("kill", "k2", "k2e"), r1, "X")
    check("A/A: run 1 keeps its own deploy and kill when run 2 is killed later",
          (rec["deploy"]["start"], rec["kill"]["start"]) == ("d1", "k1"), rec)
    rec = b.kill_record(aa + op("kill", "k2", "k2e"), r2, "X")
    check("A/A: run 2 gets its own deploy and kill once killed", (rec["deploy"]["start"], rec["kill"]["start"])
          == ("d2", "k2"), rec)
    # run 1 never killed; run 2's save2.dat is uploaded and THEN the app is killed before run 2's deploy:
    # that kill is inside run 1's window but belongs to run 2's preparation
    # the refusal states the fact (this run's save2.dat was replaced), not whose the rows are
    fact = (": this run's save2.dat on the console was replaced before the kill (another run's upload, or a "
            "restore; an errored upload can still have installed its file), so the record is not bound to this run")
    late = up(r1) + op("deploy_eboot", "d1", "d1e", local_path="X") + up(r2) + op("kill", "k", "ke")
    refused("kill_record: another run's save2.dat between deploy and kill refuses", late, r1, None,
            "another save2.dat upload (audit rows [6]) between this run's save2.dat upload (audit row 2) and the "
            "kill (audit row 7)" + fact)
    # the last kill of the window wins (a failed kill retried), a launch error retried -> the last launch
    retry = (up(r2) + op("deploy_eboot", "d", "de", local_path="X") + op("launch", "l1", "l1e", status="error")
             + op("launch", "l2", "l2e") + op("kill", "k1", "k1e", status="error") + op("kill", "k2", "k2e"))
    rec = b.kill_record(retry, r2)
    check("kill_record: the last launch and the last kill of the window", (rec["launch"]["start"], rec["kill"]["start"])
          == ("l2", "k2") and rec["launch"]["status"] == "ok", rec)
    # The run's deploy errors (an FTP timeout: the audit log had 6 deploy_eboot
    # errors in 353 deploys over all titles, 5 in 319 for ISAACR001) and is retried; then launch,
    # kill and the restore deploy.  An earlier version took the errored deploy as the run's and ended the
    # window at the retry: 'the harness did not kill this run' for a good run.  The record binds to
    # the first OK deploy; the window ends at the deploy after THAT one (the restore).
    p7 = (up(r2) + op("deploy_eboot", "d1", "d1e", status="error", local_path="X", error="FTP timed out")
          + op("deploy_eboot", "d2", "d2e", local_path="X") + op("launch", "l", "le") + op("kill", "k", "ke")
          + op("deploy_eboot", "r", "re", local_path="before/eboot.bin"))
    rec = b.kill_record(p7, r2, "X")
    check("errored deploy retried -> the ok retry's deploy rows [5, 6], launch [7, 8], kill [9, 10]",
          (rec["deploy"]["audit_rows"], rec["deploy"]["status"], rec["launch"]["audit_rows"], rec["kill"]["audit_rows"])
          == ([5, 6], "ok", [7, 8], [9, 10]), rec)
    refused("only the errored deploy (launch and kill after it): refused", p7[:4] + p7[6:10], r2, None,
            "no ok deploy_eboot of ISAACR001 after this run's save2.dat upload (audit row 2); 1 deploy(s) after it, "
            "none ok")
    # A/A session: run 1's deploy errors (an FTP timeout, as seen in the audit log) and is
    # NOT retried; run 1 is launched and the session pauses: no kill, no restore.  Run 2 then
    # uploads, deploys X ok, launches and is killed; killrecord --run 1 runs afterwards.  An earlier version bound
    # run 1 to run 2's deploy [9, 10], launch [11, 12] and kill [13, 14] (kill_check PASS), because the
    # foreign-upload scan started at the chosen deploy, after run 2's upload.  It starts at run 1's upload.
    p8a = (up(r1) + op("deploy_eboot", "d1", "d1e", status="error", local_path="X", error="FTP timed out")
           + op("launch", "l1", "l1e") + up(r2) + op("deploy_eboot", "d2", "d2e", local_path="X")
           + op("launch", "l2", "l2e") + op("kill", "k2", "k2e"))
    p8a_why = ("another save2.dat upload (audit rows [8]) between this run's save2.dat upload (audit row 2) and "
               "the kill (audit row 13)" + fact)
    refused("unretried deploy error: run 1's deploy errored, not retried -> refused (eboot X)", p8a, r1, "X", p8a_why)
    refused("unretried deploy error: run 1's deploy errored, not retried -> refused (no eboot)", p8a, r1, None, p8a_why)
    rec = b.kill_record(p8a, r2, "X")
    check("unretried deploy error: run 2 binds its own deploy [9, 10], launch [11, 12], kill [13, 14]",
          (rec["deploy"]["audit_rows"], rec["launch"]["audit_rows"], rec["kill"]["audit_rows"])
          == ([9, 10], [11, 12], [13, 14]), rec)
    # As above, but run 1 was killed and its restore deploy errored too (the audit log has such
    # consecutive deploy errors).  An earlier version bound run 1 to run 2's rows; now refused.
    p8b = (up(r1) + op("deploy_eboot", "d1", "d1e", status="error", local_path="X") + op("launch", "l1", "l1e")
           + op("kill", "k1", "k1e") + op("deploy_eboot", "r1", "r1e", status="error", local_path="before")
           + up(r2) + op("deploy_eboot", "d2", "d2e", local_path="X") + op("launch", "l2", "l2e")
           + op("kill", "k2", "k2e"))
    refused("unretried deploy and restore errors: run 1 killed, its deploy and restore deploy errored -> refused", p8b, r1, "X",
            "another save2.dat upload (audit rows [12]) between this run's save2.dat upload (audit row 2)")
    # Two runs whose save2.dat uploads both precede one deploy: the earlier run is
    # refused (its save2.dat was overwritten before any deploy), the later one binds to that deploy and kill
    both = up(r1) + up(r2) + op("deploy_eboot", "d", "de", local_path="X") + op("kill", "k", "ke")
    refused("two uploads before one deploy: the earlier run is refused", both, r1, None,
            "another save2.dat upload (audit rows [4]) between this run's save2.dat upload (audit row 2) and the "
            "kill (audit row 7)" + fact)
    got = b.kill_record(both, r2)["kill"]["audit_rows"]
    check("two uploads before one deploy: the later run binds to its kill [7, 8]", got == [7, 8], got)
    # The foreign upload is one to the bench's own MOD_DATA/save2.dat, ok or errored.
    # The previous run's EID save2.dat is restored (the procedure's restore step) after this run's
    # upload: not this run's file, so it is no foreign upload; an earlier version refused this run.
    prev = tmp / "bench-KR0-20261028"

    def upload(local, remote_, status="ok"):
        return [{"operation": "upload", "status": s, "time": "u", "local_path": str(local), "remote_path": remote_}
                for s in ("started", status)]
    def accept(rows_, run_dir, eboot=None):
        """(deploy, launch, kill) audit rows of an accepted record, or the refusal's text."""
        try:
            got = b.kill_record(rows_, run_dir, eboot)
        except ValueError as exc:
            return f"refused: {exc}"
        return tuple((got[k] or {}).get("audit_rows") for k in ("deploy", "launch", "kill"))
    q4 = op("deploy_eboot", "d", "de", local_path="X") + op("launch", "l", "le") + op("kill", "k", "ke")
    got = accept(up(r1) + upload(prev / "before" / "eid-save2.dat", f"{b.EID_DATA}/save2.dat") + q4, r1, "X")
    check("EID restore: an EID save2.dat restore after the upload -> accepted, deploy [5, 6], launch [7, 8], kill [9, 10]",
          got == ([5, 6], [7, 8], [9, 10]), got)
    # The previous run's REPRO save2.dat is restored into MOD_DATA, over this run's: refused
    refused("repro restore: a repro save2.dat restore over this run's -> refused", up(r1)
            + upload(prev / "before" / "repro-save2.dat", remote) + q4, r1, "X",
            "another save2.dat upload (audit rows [4]) between this run's save2.dat upload (audit row 2) and the "
            "kill (audit row 9)" + fact)
    # the same path spelled with another case is the same file to the bench: refused (fails closed)
    refused("case: MOD_DATA/save2.dat in another case -> refused", up(r1)
            + upload(r2 / "save2.dat", remote.replace("repro_console", "Repro_Console")) + q4, r1, "X",
            "another save2.dat upload (audit rows [4])")
    # As the unretried deploy error, but run 2's upload ERRORED and was not retried; an upload can report
    # an error with the new file installed.  An earlier version bound run 1 to run 2's rows (kill_check
    # PASS); refused now.
    q6 = p8a[:6] + upload(r2 / "save2.dat", remote, status="error") + p8a[8:]
    for eb in ("X", None):
        refused(f"errored foreign upload: run 2's upload errored -> run 1 refused (eboot {eb})", q6, r1, eb,
                "another save2.dat upload (audit rows [8]) between this run's save2.dat upload (audit row 2) and "
                "the kill (audit row 13)" + fact)
    refused("errored foreign upload: run 2 itself has no ok upload", q6, r2, "X", "no ok upload")
    # this run's own errored re-upload, and an orphan 'started' row with no end row, are not foreign
    got = accept(up(r1) + upload(r1 / "save2.dat", remote, status="error") + q4, r1, "X")
    check("this run's own errored re-upload is not foreign", got == ([5, 6], [7, 8], [9, 10]), got)
    got = accept(up(r1) + upload(r2 / "save2.dat", remote)[:1] + q4, r1, "X")
    check("an orphan 'started' upload row is not counted (documented limit)", got == ([4, 5], [6, 7], [8, 9]), got)
    # no refusal says whose the rows are
    for rows_, run_dir in ((late, r1), (p8a, r1), (p8b, r1), (both, r1), (q6, r1)):
        try:
            b.kill_record(rows_, run_dir)
            msg = "accepted"
        except ValueError as exc:
            msg = str(exc)
        check("the refusal states the fact, names no one", msg.endswith(fact) and "belong" not in msg, msg)
    # Who is exposed.  Run 1's deploy X errors and is not retried, run 1 is launched;
    # then a non-bench ok deploy of Y, a launch and a kill.  With the manifest's eboot it is refused; a run
    # prepared without --eboot (kill_record true, no eboot) takes Y's deploy (kill_check then decides by time).
    q1 = (up(r1) + op("deploy_eboot", "d1", "d1e", status="error", local_path="X") + op("launch", "l1", "l1e")
          + op("deploy_eboot", "d2", "d2e", local_path="Y") + op("launch", "l2", "l2e") + op("kill", "k", "ke"))
    refused("non-bench deploy, with the manifest's eboot X -> refused", q1, r1, "X", "not this run's eboot X")
    got = accept(q1, r1)
    check("non-bench deploy, without an eboot (a run prepared without --eboot) -> takes Y's deploy [7, 8] "
          "(the documented exposure)",
          got == ([7, 8], [9, 10], [11, 12]) and b.kill_record(q1, r1)["deploy"]["local_path"] == "Y", got)
    # stale analyze JSON (no eid_version) is refused by load_run; a new one is accepted
    with tempfile.TemporaryDirectory() as td:
        new = json.loads(json.dumps(good))
        (Path(td) / "new.json").write_text(json.dumps(new), encoding="utf-8")
        check("load_run accepts a gated JSON", b.load_run(Path(td) / "new.json")["eid_version"] == "5.23_bc0551a", "")
        del new["eid_version"]
        (Path(td) / "old.json").write_text(json.dumps(new), encoding="utf-8")
        try:
            b.load_run(Path(td) / "old.json")
            check("load_run refuses a JSON without eid_version", False, "accepted")
        except SystemExit as exc:
            check("stale JSON message", "eid_version" in str(exc) and "pass the run directory" in str(exc), str(exc))
    # compare: EID line
    txt = b.compare(good, good)
    check("compare EID line", "EID: A v5.23_bc0551a heap 11215 KB and 166 bytes, B v5.23_bc0551a heap 11215 KB "
          "and 166 bytes (same Lua heap)" in txt, txt[:400])
    txt = b.compare(good, run("eidfail"))
    check("compare: no banner on one side is NOT COMPARABLE", "-- NOT COMPARABLE (B has no EID banner" in txt, txt[:400])
    # prepare writes kill_record and the procedure has the killrecord step after the kill
    with tempfile.TemporaryDirectory() as td:
        with contextlib.redirect_stdout(io.StringIO()):
            b.main(["prepare", "--out", str(Path(td) / "r"), "--nonce", "feedfacecafe"])
        man = json.loads((Path(td) / "r" / "manifest.json").read_text(encoding="utf-8"))
        proc = (Path(td) / "r" / "procedure.txt").read_text(encoding="utf-8")
        check("prepare: manifest kill_record true", man.get("kill_record") is True, man)
        k, r = proc.find("vita_kill "), proc.find("bench_run.py killrecord --run")
        check("procedure: killrecord right after vita_kill", 0 <= k < r < proc.find("vita_download " + b.NATIVE_LOG),
              (k, r))
    print("validity: 6 eidfail/banner/heap rules, pin override, 9 loadfile receipt cases, intercepts, cost lost, "
          "positional intercept (timed by the next timestamp), INTERCEPT_UNSEEN, "
          "7 NO_HARNESS_KILL cases, 34 kill_record binding cases, "
          "stale JSON, compare EID line, prepare")


PC_RUN = "bench-D4A4Pc-20261026"
LUA_HEALTH_MIN = 40   # legacy October runs that must stay BENCH OK (42 today, see leg_lua_health)
# The legacy corpus of leg_lua_health: the 43 October runs with
# both logs, other than Pc, that predate the gate (28.10 listing of the device log root).  The
# zero-gate-code and stays-OK facts are facts about THESE logs; frozen so that a later run designed to
# FAIL (MOD_LUA_ERROR, SYSTEM_INTERCEPT) cannot turn the suite red.  A later run is
# checked against its own stored gated verdict instead (lua_health_scan).
LEGACY_OCTOBER_RUNS = frozenset((
    'bench-A-20261025', 'bench-A2-20261025', 'bench-A2R0-20261025', 'bench-A2S-20261025',
    'bench-AOFF-20261025', 'bench-AOFFS-20261025', 'bench-D3A2Ca-20261026', 'bench-D3A2Cb-20261026',
    'bench-D3A4Ca-20261026', 'bench-D3A4Cb-20261026', 'bench-D3A4CPa-20261026', 'bench-D3A4CPb-20261026',
    'bench-D4A2a-20261026', 'bench-D4A2b-20261026', 'bench-D4A2c-20261026', 'bench-D4A4a-20261026',
    'bench-D4A4b-20261026', 'bench-D4A4c-20261026', 'bench-D4A4Pa-20261026', 'bench-D4A4Pb-20261026',
    'bench-R10A1-20261026', 'bench-R10La-20261026', 'bench-R10Lb-20261026', 'bench-R1A1a-20261026',
    'bench-R1A1b-20261026', 'bench-R1A1C-20261026', 'bench-R1M1-20261026', 'bench-R2AS1V-20261026',
    'bench-R2BS1-20261026', 'bench-R2N-20261026', 'bench-R2NS1-20261026', 'bench-R2NS1V-20261026',
    'bench-R3A16-20261026', 'bench-R3A1a-20261026', 'bench-R3A1b-20261026', 'bench-R3A1c-20261026',
    'bench-R3A2a-20261026', 'bench-R3A2b-20261026', 'bench-R3A2c-20261026', 'bench-R4S8-20261026',
    'bench-R4S8V-20261026', 'bench-R5A2a-20261026', 'bench-R5S6-20261026',
))
# bench_run.diagnose's verdict note for an --eid-heap-pin override (a gated JSON written with one)
RE_PIN_NOTE = re.compile(r"\[EID heap pin overridden: --eid-heap-pin (\d+):(\d+)\]")


def stored_json(d: Path) -> dict | None:
    """The run's stored analyze --json result: result.json, or analysis.json (R4/R5/R10 dirs)."""
    for name in ("result.json", "analysis.json"):
        if (d / name).exists():
            return json.loads((d / name).read_text(encoding="utf-8"))
    return None


def stored_verdict(d: Path) -> str | None:
    return (stored_json(d) or {}).get("verdict")


def headline(verdict: str) -> str:
    """'BENCH FAILED (A, B): why ...' -> 'BENCH FAILED (A, B)'; 'BENCH OK: 5 phases ...' -> 'BENCH OK'."""
    return verdict.split(":", 1)[0]


def lua_health_scan(root: Path) -> dict:
    """The per-run checks of leg_lua_health over every bench-*-202610* dir with both logs, Pc aside.
    legacy (LEGACY_OCTOBER_RUNS): no bench-validity code; a stored BENCH OK stays BENCH OK with EID
    v5.23_bc0551a and the pinned heap.  gated (any other run whose stored analyze JSON has eid_version,
    i.e. was written by the gated analyzer): re-diagnosis reproduces the stored verdict's headline
    ('BENCH OK' or 'BENCH FAILED (codes)'), with the stored pin override if any -- a run designed to
    FAIL stays green while it FAILs the same way.  Any other run (no stored JSON, or one written
    before the gate) is listed, not checked."""
    n = {"legacy": 0, "legacy_ok": 0, "gated": 0, "unchecked": []}
    for d in sorted(root.glob("bench-*-202610*")):
        if d.name == PC_RUN or not d.is_dir() or not (d / "native.log").exists() or not (d / "game.log").exists():
            continue
        stored = stored_json(d)
        if d.name in LEGACY_OCTOBER_RUNS:
            res = b.diagnose_dir(d)
            n["legacy"] += 1
            gate = codes(res) & GATE_CODES
            check(f"{d.name}: no bench-validity code", not gate, sorted(gate))
            if (stored or {}).get("verdict", "").startswith("BENCH OK"):
                n["legacy_ok"] += 1
                check(f"{d.name}: stored BENCH OK stays BENCH OK", res["ok"], res["verdict"][:120])
                check(f"{d.name}: EID v5.23_bc0551a heap 11215/166",
                      (res["eid_version"], res["eid_heap"]) == ("5.23_bc0551a", [11215, 166]),
                      (res["eid_version"], res["eid_heap"]))
        elif stored is not None and "eid_version" in stored:
            m = RE_PIN_NOTE.search(stored.get("verdict", ""))
            res = b.diagnose_dir(d, eid_heap_pin=(int(m.group(1)), int(m.group(2))) if m else None)
            n["gated"] += 1
            check(f"{d.name}: re-diagnosis reproduces the stored gated verdict",
                  headline(res["verdict"]) == headline(stored.get("verdict", "")),
                  f"now {headline(res['verdict'])!r}, stored {headline(stored.get('verdict', ''))!r}")
        else:
            n["unchecked"].append(d.name)
    return n


def lua_health_future(root: Path) -> str:
    """A later October run designed to FAIL (stood in for by Pc's logs under a
    new name, with a result.json written by this analyzer) keeps lua_health_scan green; the gated
    check is live (a stored verdict the logs do not reproduce FAILs); a run with no stored JSON is
    only listed."""
    def scan(td: Path) -> tuple:
        n0 = len(FAILS)
        with contextlib.redirect_stdout(io.StringIO()):
            got = lua_health_scan(td)
        new = FAILS[n0:]
        del FAILS[n0:]
        return got, new
    with tempfile.TemporaryDirectory(prefix="lua-health-") as td:
        da = Path(td) / "bench-DA-20261029"
        da.mkdir()
        for f in ("native.log", "game.log", "manifest.json"):
            shutil.copy2(root / PC_RUN / f, da / f)
        res = b.diagnose_dir(da)
        (da / "result.json").write_text(json.dumps(res, indent=1) + "\n", encoding="utf-8")
        got, new = scan(Path(td))
        check("future D-A stand-in: BENCH FAILED (MOD_LUA_ERROR ...) stored", not res["ok"]
              and res["verdict"].startswith("BENCH FAILED (MOD_LUA_ERROR"), res["verdict"][:80])
        check("future D-A stand-in: gated, reproduced, no FAIL", got["gated"] == 1 and not new, (got, new))
        (da / "result.json").write_text(json.dumps(dict(res, verdict="BENCH OK: forged"), indent=1) + "\n",
                                        encoding="utf-8")
        got, new = scan(Path(td))
        check("future run whose stored gated verdict is not reproduced: exactly one FAIL",
              got["gated"] == 1 and len(new) == 1 and "reproduces the stored gated verdict" in new[0], new)
        (da / "result.json").unlink()
        got, new = scan(Path(td))
        check("future run without a stored JSON: listed, not checked", got["unchecked"] == [da.name]
              and got["gated"] == 0 and not new, (got, new))
    return "D-A stand-in green, forged stored verdict caught, JSON-less run listed"


def leg_lua_health(root: Path | None) -> None:
    """The bench validity gate on the real October logs: D4A4Pc FAILs with MOD_LUA_ERROR first
    and SYSTEM_INTERCEPT; every legacy run (LEGACY_OCTOBER_RUNS) gets no gate code, and every one of
    them whose stored verdict is BENCH OK stays BENCH OK (a broken regex cannot pass silently); a later
    run is held to its own stored gated verdict (lua_health_scan)."""
    if root is None or not (root / PC_RUN / "game.log").exists():
        print(f"lua health: SKIP (no {PC_RUN} under --smoke-root)")
        return

    def lines(rel: str) -> list:
        return (root / rel).read_text(encoding="utf-8", errors="replace").split("\n")
    check("fixture EID_LOADED = bench-D4A4Pb-20261026/game.log:55-58",
          lines("bench-D4A4Pb-20261026/game.log")[54:58] == EID_LOADED, "")
    smoke1 = f"{SMOKE_DIRS['smoke1']}/run1-game.log"
    if (root / smoke1).exists():
        check("fixture S1_GAME EID lines = smoke1 run1-game.log:55-58", lines(smoke1)[54:58] == EID_LOADED, "")
    check(f"fixture EID_FAIL = {PC_RUN}/game.log:55-61", lines(f"{PC_RUN}/game.log")[54:61] == EID_FAIL, "")
    pc = b.diagnose_dir(root / PC_RUN)
    check("Pc: BENCH FAILED, MOD_LUA_ERROR first", not pc["ok"] and pc["verdict"].startswith("BENCH FAILED (MOD_LUA_ERROR"),
          pc["verdict"][:120])
    check("Pc: the six EID codes and SYSTEM_INTERCEPT, nothing else",
          codes(pc) == EIDFAIL_CODES | {"SYSTEM_INTERCEPT"}, sorted(codes(pc)))
    check("Pc: WARN LUA_COST_LOST", "LUA_COST_LOST" in codes(pc, "WARN"), sorted(codes(pc, "WARN")))
    # What Pc's STORED result.json says is a fact about a file under --smoke-root, and the
    # obvious cleanup (re-running analyze --json on Pc) changes it.  While it is the pre-gate one (no
    # eid_version) it must be the defect and be refused; once rewritten by the gated analyzer, its
    # headline must be the one the logs give (a forged gated 'BENCH OK' still FAILs here).
    # Deleting it is a cleanup too; with no stored JSON there is nothing to check.
    pc_file = next((root / PC_RUN / n for n in ("result.json", "analysis.json") if (root / PC_RUN / n).exists()), None)
    pc_stored = json.loads(pc_file.read_text(encoding="utf-8")) if pc_file else {}
    if pc_file is None:
        pass
    elif "eid_version" not in pc_stored:
        check("Pc: stored result.json still says BENCH OK (the defect)",
              pc_stored.get("verdict", "").startswith("BENCH OK"), pc_stored.get("verdict"))
        try:
            b.load_run(pc_file)
            check("Pc: the stale result.json is refused by load_run", False, "accepted")
        except SystemExit:
            pass
    else:
        check("Pc: the gated stored result.json reproduces the logs' verdict headline",
              headline(pc_stored.get("verdict", "")) == headline(pc["verdict"]),
              f"stored {headline(pc_stored.get('verdict', ''))!r}, now {headline(pc['verdict'])!r}")
    n = lua_health_scan(root)
    check(f"lua health: at least {LUA_HEALTH_MIN} stored-OK legacy October runs", n["legacy_ok"] >= LUA_HEALTH_MIN,
          n["legacy_ok"])
    future = lua_health_future(root)
    print(f"lua health: {PC_RUN} {pc['verdict'][:60]}... (stored result.json: "
          + ("absent" if pc_file is None else "gated, same headline" if "eid_version" in pc_stored
             else "pre-gate, refused")
          + f"); {n['legacy']} legacy October runs without a gate "
          f"code, {n['legacy_ok']} stored BENCH OK still BENCH OK; {n['gated']} later gated runs reproduce "
          f"their stored verdict, {len(n['unchecked'])} later runs not checked (no gated analyze JSON)"
          + (f": {', '.join(n['unchecked'][:4])}" if n["unchecked"] else "") + f"; {future}")


# ----------------------------------------------------------------------------------- seed
# Verbatim from the 2026-10-25 device logs (bench-A-20261025 and
# bench-AOFF-20261025, game.log): the start seed of each "game start N" line and the
# "Level::Init m_Stage 1, m_StageType 4 Seed" line of the same start, which precedes it.
DEVICE_PAIRS = [("BS8Y SWXQ", 1258606820), ("VWSN M6ZS", 771675230), ("38TQ 6MPK", 1891709745),
                ("2DQV S893", 2699283828), ("37QF HVQ1", 41147546), ("FJGN STFE", 67550731)]
DEVICE_RUNS = ("bench-A-20261025", "bench-AOFF-20261025")
# The type-0 index is device-backed.  Verbatim from device logs (found by file name under
# --smoke-root): (file, line of 'RNG Start Seed: <string> (<n>)', string, n, [(line, stage,
# type, 'Level::Init ... Seed')]) for runs whose boot floor is a type-0 Basement I and whose later
# floor came from a stage command of the same start seed.  stage_seeds steps between the floors:
# (1, 0) -> (1, 4) 1, (1, 0) -> (7, 0) 6, (1, 0) -> (8, 0) 7.
DEVICE_TYPE0 = [
    ("eid17-treasure-baseline-20261010-game.log", 263, "Z6BR 8J46", 2699919582,
     [(405, 1, 0, 2436773619), (434, 1, 4, 2509893735)]),
    ("log-rc3-greed-repro-1.txt", 261, "GALJ LAVN", 1073478398,
     [(402, 1, 0, 1108612430), (427, 7, 0, 1994034164)]),
    ("log-nt2p-run1.txt", 260, "MMRP HX0C", 1429318707,
     [(402, 1, 0, 4046809777), (430, 8, 0, 2234407646)]),
]


def device_pairs(text: str) -> list:
    out, last = [], None
    for line in text.splitlines():
        m = b.RE_LEVEL.search(line)
        if m:
            last = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
        m = re.search(r"\[repro_console\] game start \d+: seed=(\S+ \S+) ", line)
        if m and last is not None:
            out.append((m.group(1), last))
            last = None
    return out


def leg_seed(root: Path | None) -> None:
    for s, lv in DEVICE_PAIRS:
        n = b.string2seed(s)
        check(f"seed {s} -> Level::Init {lv}", n != 0 and b.level_seed(n, 1, 4) == lv,
              f"string2seed {n}, level_seed {b.level_seed(n, 1, 4) if n else None}")
    check("Seed2String(20261017) = BVPF Q3ET (device)", b.string2seed(WANT_SEED) == 20261017, b.string2seed(WANT_SEED))
    check("BVPF Q3ET Downpour I seed", b.level_seed(20261017, 1, 4) == WANT_LEVEL, b.level_seed(20261017, 1, 4))
    for bad in ("BVPF Q3EA", "BVPFQ3ET", "BVPF Q3EI", "BVPF  Q3ET", ""):
        check(f"string2seed rejects {bad!r}", b.string2seed(bad) == 0, b.string2seed(bad))
    # the type-0 index (stage + 0) on device logs: the chain from the Basement I boot floor to the
    # later floor by stage_seeds steps, and each floor from the logged start seed
    idx = {(st, ty): st + (1 if ty in (4, 5) else 0) for _, _, _, _, fl in DEVICE_TYPE0 for _, st, ty, _ in fl}
    steps = []
    for name, _, s, n, floors in DEVICE_TYPE0:
        (_, st0, ty0, s0), (_, st1, ty1, s1) = floors
        k = idx[(st1, ty1)] - idx[(st0, ty0)]
        steps.append(k)
        check(f"{name}: ({st0},{ty0}) -> ({st1},{ty1}) is {k} stage_seeds step(s)", b.stage_seeds(s0)[k - 1] == s1,
              b.stage_seeds(s0)[:8])
        check(f"{name}: string2seed({s!r}) = {n} (device)", b.string2seed(s) == n, b.string2seed(s))
        check(f"{name}: level_seed of the logged start seed", all(b.level_seed(n, st, ty) == sd for _, st, ty, sd in floors),
              [(st, ty, b.level_seed(n, st, ty)) for _, st, ty, _ in floors])
    check("type-0 chains: 1, 6, 7 steps", steps == [1, 6, 7], steps)
    eid = DEVICE_TYPE0[0][4]
    check("type 0 and type 4 of stage 1 are different stage seeds (index stage + 0 vs stage + 1)",
          eid[0][3] != eid[1][3] and b.level_seed(20261017, 1, 0) != b.level_seed(20261017, 1, 4), "")
    check("BVPF Q3ET Basement I = stage_seeds[1] = 4128373806", b.level_seed(20261017, 1, 0) == 4128373806
          == b.stage_seeds(20261017)[1], b.level_seed(20261017, 1, 0))
    type0_files = 0
    if root and root.is_dir():
        for name, sl, s, n, floors in DEVICE_TYPE0:
            p = next(iter(sorted(root.rglob(name))), None)
            if p is None or not p.is_file():
                continue
            lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
            got = [lines[sl - 1]] + [lines[ln - 1] for ln, *_ in floors]
            want = [f"[INFO] - RNG Start Seed: {s} ({n}) [New, 2]"] + [
                f"[INFO] - Level::Init m_Stage {st}, m_StageType {ty} Seed {sd}" for _, st, ty, sd in floors]
            check(f"{name}: lines re-read verbatim", got == want, got)
            type0_files += 1
    nonce = "0123456789ab"
    nat = "\n".join(good_native()) + "\n"

    def run(game):
        return b.diagnose(nat, "\n".join(game) + "\n", nonce)
    good = run(good_game(nonce))
    check("in-place OK SEED", "SEED" in codes(good, "OK") and good["seed_level"] == {
        "want": WANT_SEED, "stage": 1, "stage_type": 4, "seed": WANT_LEVEL, "expected": WANT_LEVEL},
        str(good["seed_level"]))
    check("in-place seed_rooms after the seeded floor", good["seed_rooms"] == [list(p) for p in
          [(1, r) for _, r in ROOMS[1:]]], str(good["seed_rooms"])[:120])
    wrong = run(good_game(nonce, level_seed=WANT_LEVEL + 1))
    check("wrong floor seed -> WARN SEED_LEVEL", wrong["ok"] and "SEED_LEVEL" in codes(wrong, "WARN")
          and "SEED" not in codes(wrong, "OK") and wrong["seed_rooms"] is None, str(wrong["diagnostics"])[:200])
    g = [x for x in good_game(nonce) if f"Seed {WANT_LEVEL}" not in x]
    nolevel = run(g)
    check("no rebuild -> WARN SEED_NO_LEVEL", nolevel["ok"] and "SEED_NO_LEVEL" in codes(nolevel, "WARN"),
          str(nolevel["diagnostics"])[:200])
    # the boot floor's Level::Init comes before the seed line: it must not count as the seeded floor
    g = [x.replace("Seed 1258606820", f"Seed {WANT_LEVEL}") for x in good_game(nonce) if f"Seed {WANT_LEVEL}" not in x]
    check("boot Level::Init is not the seeded floor", "SEED_NO_LEVEL" in codes(run(g), "WARN"), "")
    g = [x if "set in place" not in x else rc("ERROR seed: SetStartSeed(BVPF Q3ET) left BS8Y SWXQ (no effect) - "
                                                 "continuing unseeded") for x in good_game(nonce)]
    check("ERROR seed -> WARN SEED_FALLBACK", "SEED_FALLBACK" in codes(run(g), "WARN"), "")
    other = dict(good, seed_rooms=[list(p) for p in good["seed_rooms"]])
    other["seed_rooms"][3] = [1, 999]
    check("rooms_line identical", b.rooms_line(good, good).startswith("rooms: identical"), b.rooms_line(good, good))
    check("rooms_line differ", b.rooms_line(good, other).startswith("rooms: DIFFER at room load 4"),
          b.rooms_line(good, other))
    check("rooms_line floor seed differs", b.rooms_line(good, dict(good, seed_level=dict(
        good["seed_level"], seed=1))).startswith("rooms: DIFFER (floor seed"), "")
    check("rooms_line unseeded", b.rooms_line(good, wrong).startswith("rooms: NOT COMPARABLE (B "), b.rooms_line(good, wrong))
    check("json round trip keeps rooms identical", b.rooms_line(json.loads(json.dumps(good)), good).startswith(
        "rooms: identical"), "")
    full = []
    if root and root.is_dir():
        found = []
        for run_dir in DEVICE_RUNS:
            d = root / run_dir
            if not (d / "game.log").exists() or not (d / "native.log").exists():
                continue
            gt = (d / "game.log").read_text(encoding="utf-8", errors="replace")
            pairs = device_pairs(gt)
            found += [(s, lv[2]) for s, lv in pairs]
            check(f"{run_dir} starts are Downpour I", all(lv[:2] == (1, 4) for _, lv in pairs), str(pairs))
            nonce_d = json.loads((d / "manifest.json").read_text(encoding="utf-8"))["nonce"]
            res = b.diagnose((d / "native.log").read_text(encoding="utf-8", errors="replace"), gt, nonce_d)
            check(f"{run_dir} analyze: OK with WARN SEED_FALLBACK", res["ok"] and "SEED_FALLBACK" in codes(res, "WARN")
                  and res["seed_rooms"] is None, res["verdict"])
            full.append(res)
        if len(full) == 2:
            check("device pairs re-extracted verbatim", found == DEVICE_PAIRS, str(found))
            check("device A vs AOFF not comparable", b.rooms_line(full[0], full[1]).startswith("rooms: NOT COMPARABLE"),
                  b.rooms_line(full[0], full[1]))
    print(f"seed: {len(DEVICE_PAIRS)} device pairs, {len(DEVICE_TYPE0)} type-0 chains (files re-read "
          f"{type0_files}), in-place rules, rooms_line; full device logs {len(full)}"
          + ("" if full else " (SKIP: not found under --smoke-root)"))


# ----------------------------------------------------------------------------------- residual
# Verbatim records of kage_vita_guest_sampler_oracle.c scenario 8 (RESIDUAL + ENTRY_CENSUS
# flavour, test_kage_vita_guest_sampler.py), bid/win rewritten; ph120.hotw and ph120.fe are split
# over two lines each the way the sampler splits a long record (full header, whole items).
# test_kage_vita_guest_sampler.py checks the hotr/hotu/hotw/hotl/hcl lines here and in
# RESIDUAL_STALE_LINES against the oracle's windows 10/11 item for item.  The
# ph120.fe lines are hand-made: the oracle's window 10 prints empty fe records (its census
# scenario is 9).
RESIDUAL_LINES = [
    "[kage-vita] ph120.hotr bid=sampler:oracle win=10 loops=1200 phase=rnd samples=7 "
    "top=140eef0:2,c0eef0:2,0:1,1000000:1,1c0eef0:1",
    "[kage-vita] ph120.hotu bid=sampler:oracle win=10 loops=1200 phase=rnd samples=7 top=562e00:1,13f8e30:1",
    "[kage-vita] ph120.hotw bid=sampler:oracle win=10 loops=1200 phase=rnd samples=7 "
    "w=48bc50:0/0/4/0/0/0/0,562e00:0/1/1/0/0/0/0",
    "[kage-vita] ph120.hotw bid=sampler:oracle win=10 loops=1200 phase=rnd samples=7 "
    "w=40eef0:0/3/5/0/2/2/1,3ffe10:0/0/4/0/0/0/0,3f8e30:1/2/3/0/0/0/0,439e80:0/1/1/0/0/0/0",
    "[kage-vita] ph120.hotl bid=sampler:oracle win=10 loops=1200 phase=rnd samples=7 inv=40eef0 "
    "o=3ffe10:0/1/2/1,0:0/1/0/0",
    "[kage-vita] ph120.hcl bid=sampler:oracle win=10 lb=2 lbm=1 lbs=1 lbx=0 watch=64",
    "[kage-vita] ph120.fe bid=sampler:oracle win=10 loops=1200 phase=upd e=3e50:3",
    "[kage-vita] ph120.fe bid=sampler:oracle win=10 loops=1200 phase=rnd e=40eef0:19,3e50:57",
    "[kage-vita] ph120.fe bid=sampler:oracle win=10 loops=1200 phase=rnd e=3f8e30:38",
]


def leg_residual() -> None:
    nonce = "0123456789ab"
    nat = good_native()
    out = []
    for line in nat:
        out.append(line)
        m = re.search(r"\[kage-vita\] ph120\.hs bid=\S+ win=(\d+) ", line)
        if m:  # after each window's ph120.hs, like the sampler's report order
            k = int(m.group(1))
            for r in RESIDUAL_LINES:
                r = r.replace("bid=sampler:oracle", f"bid={BID}")
                out.append(re.sub(r"\bwin=\d+", f"win={k}", r))
    res = b.diagnose("\n".join(out) + "\n", "\n".join(good_game(nonce)) + "\n", nonce)
    check("residual run ok", res["ok"], res["verdict"])
    rows = {r["phase"]: r for r in res["phases"]}
    p2 = rows.get("P2 fight-maxroom", {})
    rr = p2.get("residual") or {}
    check("residual present", bool(rr), str(p2.keys()))
    if not rr:
        return
    n = 57  # W41 cad r: renders per window; every value is per window, so per render = v / 57
    check("residual renders", rr["renders"] == 9 * n and rr["windows"] == 9, (rr["renders"], rr["windows"]))
    fns = {f["fn"]: f for f in rr["functions"]}
    inv = fns.get("40eef0", {})
    exp = {"self": 0, "caller": 3 / n, "incl": 5 / n, "root_t": 0, "root_n": 2 / n, "lua_t": 2 / n,
           "lua_n": 1 / n, "calls": 19 / n, "us_incl": 1e3 * 5 / 19, "us_self": 0}
    for k, v in exp.items():
        check(f"residual 40eef0 {k}", near(inv.get(k), v), f"{inv.get(k)} != {v}")
    # split lines are a union: 3f8e30 from the second hotw line and the third fe line
    bind = fns.get("3f8e30", {})
    check("residual split union", near(bind.get("self"), 1 / n) and near(bind.get("calls"), 38 / n)
          and near(bind.get("us_self"), 1e3 / 38), str(bind))
    # census-only row (3e50: entries, no samples) and the upd entries stay out of rnd calls
    rf = fns.get("3e50", {})
    check("residual entries without samples", near(rf.get("calls"), 1.0) and near(rf.get("incl"), 0)
          and near(rf.get("us_incl"), 0), str(rf))
    check("residual order", [f["fn"] for f in rr["functions"]][:3] == ["40eef0", "3ffe10", "48bc50"],
          [f["fn"] for f in rr["functions"]][:4])
    # scenario 8: no key 800000/1800000, so every crossed key has a root (140eef0, c0eef0, 1c0eef0)
    check("residual roots", near(rr["no_root_t"], 1 / n) and near(rr["no_root_n"], 1 / n)
          and near(rr.get("no_root_crossed_t"), 0) and near(rr.get("no_root_crossed_n"), 0)
          and near(rr["lua_crossed"], 3 / n),
          str({k: rr.get(k) for k in ("no_root_t", "no_root_n", "no_root_crossed_t", "no_root_crossed_n",
                                      "lua_crossed")}))
    check("residual hcl", near(rr["lua"].get("lb"), 2 / n) and near(rr["lua"].get("lbm"), 1 / n)
          and near(rr["lua"].get("lbs"), 1 / n) and near(rr["lua"].get("lbx"), 0), str(rr["lua"]))
    check("residual hotl", set(rr["hotl"]) == {"3ffe10", "0"} and
          all(near(a, x / n) for a, x in zip(rr["hotl"]["3ffe10"], (0, 1, 2, 1))), str(rr["hotl"]))
    check("residual hotu", [k for k, _ in rr["hotu"]] == ["13f8e30", "562e00"] and
          all(near(v, 1 / n) for _, v in rr["hotu"]), str(rr["hotu"]))
    rep = b.report(res)
    check("residual report", "render residual per phase" in rep and
          "invoker 40eef0 by next root out" in rep, rep[-400:])
    cmp_txt = b.compare(res, res)
    check("residual compare", "residual incl ms/render B vs A: 40eef0 0.09->0.09" in cmp_txt, cmp_txt[-300:])
    plain = b.diagnose("\n".join(nat) + "\n", "\n".join(good_game(nonce)) + "\n", nonce)
    check("no residual without records", not any(r.get("residual") for r in plain["phases"]), "")
    stale = [line for line in out if "ph120.fe" not in line] + [
        f"[kage-vita] ph120.fe bid={BID} win=3 loops=360 ids=STALE"]
    st = b.diagnose("\n".join(stale) + "\n", "\n".join(good_game(nonce)) + "\n", nonce)
    check("stale census warns", "CENSUS_STALE" in codes(st, "WARN"), codes(st, "WARN"))
    # ISAAC_VITA_RENDER_CHAIN_NATIVE: the replayed bodies' calls come from ph120.rn
    # (25af60 = f, 55f610 = r, 55f630 = census entries + ri), not from ph120.fe.
    chain = [
        "[kage-vita] ph120.hotw bid=sampler:oracle win=10 loops=1200 phase=rnd samples=7 "
        "w=25af60:2/0/3/0/0/0/0,55f630:1/0/1/0/0/0/0",
        "[kage-vita] ph120.fe bid=sampler:oracle win=10 loops=1200 phase=rnd e=55f630:2,25af60:1",
        "[kage-vita] ph120.rn bid=sampler:oracle win=10 loops=1200 f=5 fd=1 gn=40 gc=0 rl=5 r=7 ri=6 rd=1",
    ]
    out_rn = []
    for line in out:
        out_rn.append(line)
        m = re.search(r"\[kage-vita\] ph120\.hs bid=\S+ win=(\d+) ", line)
        if m:
            k = int(m.group(1))
            for r in chain:
                out_rn.append(re.sub(r"\bwin=\d+", f"win={k}",
                                     r.replace("bid=sampler:oracle", f"bid={BID}")))
    rres = b.diagnose("\n".join(out_rn) + "\n", "\n".join(good_game(nonce)) + "\n", nonce)
    rrn = ({r["phase"]: r for r in rres["phases"]}.get("P2 fight-maxroom") or {}).get("residual") or {}
    fn_rn = {f["fn"]: f for f in rrn.get("functions", [])}
    for name, calls, us in (("25af60", 5, 1e3 * 3 / 5), ("55f610", 7, 0.0), ("55f630", 8, 1e3 * 1 / 8)):
        got = fn_rn.get(name, {})
        check(f"chain native {name} calls", near(got.get("calls"), calls / n) and near(got.get("us_incl"), us),
              str(got))
    cn = rrn.get("chain_native") or {}
    check("chain native counters", near(cn.get("f"), 5 / n) and near(cn.get("ri"), 6 / n)
          and near(cn.get("rd"), 1 / n), str(cn))
    check("chain native absent without ph120.rn", rr.get("chain_native") is None, str(rr.get("chain_native")))
    rep_rn = b.report(rres)
    check("chain native report", "render chain native per render (ph120.rn" in rep_rn, rep_rn[-400:])
    cmp_rn = b.compare(rres, rres)
    # 3e50: 57 entries and no samples per window = 0 us per call; 4b60/4760 have no row.
    check("chain native compare", "chain us/call incl B vs A: 3e50 0.00->0.00, 55f610 0.00->0.00, "
          "55f630 125.00->125.00, 25af60 600.00->600.00" in cmp_rn, cmp_rn[-300:])
    print(f"residual: {len(rr['functions'])} watch rows, hand-checked 40eef0 / split / roots / Lua; "
          f"render chain native calls from ph120.rn")


# Verbatim records of the sampler oracle's scenario 8c (stale boundary words),
# window 11: hotr 800000 is a sample that crossed a live boundary and then lost its walk (no root).
RESIDUAL_STALE_LINES = [
    "[kage-vita] ph120.hotr bid=sampler:oracle win=11 loops=1320 phase=rnd samples=4 "
    "top=48bc50:2,800000:1,1c0eef0:1",
    "[kage-vita] ph120.hotu bid=sampler:oracle win=11 loops=1320 phase=rnd samples=4 top=3f8e30:1",
    "[kage-vita] ph120.hotw bid=sampler:oracle win=11 loops=1320 phase=rnd samples=4 "
    "w=48bc50:0/1/3/2/0/0/0,7af0:1/2/3/0/0/0/0,40eef0:0/0/1/0/0/0/1,3ffe10:0/0/1/0/0/0/0,"
    "3f8e30:0/1/2/0/0/0/0",
    "[kage-vita] ph120.hotl bid=sampler:oracle win=11 loops=1320 phase=rnd samples=4 inv=40eef0 "
    "o=3ffe10:0/0/0/1",
    "[kage-vita] ph120.hcl bid=sampler:oracle win=11 lb=2 lbm=0 lbs=0 lbx=3 watch=64",
]


def leg_residual_review() -> None:
    """Rootless crossed keys are no-root, per render over the windows that carry the records, and
    a reprinted window replaces its records instead of adding to them."""
    # Rootless crossed keys, a synthetic hotr: 10 renders, 0:10 1000000:10 800000:30 1800000:20 c0eef0:5.
    tops = {1: {"ph120.hotr": {"rnd": {"0": [10], "1000000": [10], "800000": [30], "1800000": [20],
                                       "c0eef0": [5]}},
                "ph120.hotw": {"rnd": {"48bc50": [0, 0, 1, 0, 0, 0, 0]}}}}
    res = b.residual(tops, {1: {}}, [1], {1: [0, 0, 0, 0, 0, 10]}) or {}
    got = {k: res.get(k) for k in ("no_root_t", "no_root_n", "no_root_crossed_t", "no_root_crossed_n",
                                   "lua_crossed")}
    check("B2 rootless crossed is no root", all(near(got[k], v) for k, v in (
        ("no_root_t", 4.0), ("no_root_n", 3.0), ("no_root_crossed_t", 3.0), ("no_root_crossed_n", 2.0),
        ("lua_crossed", 0.5)) if got[k] is not None) and None not in got.values(), str(got))
    # the oracle's 8c window through the whole analyzer: 800000:1 -> no root (crossed), 1c0eef0 -> Lua
    out = []
    for line in good_native():
        out.append(line)
        m = re.search(r"\[kage-vita\] ph120\.hs bid=\S+ win=(\d+) ", line)
        if m:
            k = int(m.group(1))
            for r in RESIDUAL_STALE_LINES:
                out.append(re.sub(r"\bwin=\d+", f"win={k}", r.replace("bid=sampler:oracle", f"bid={BID}")))
    nonce = "0123456789ab"
    full = b.diagnose("\n".join(out) + "\n", "\n".join(good_game(nonce)) + "\n", nonce)
    rr = ({r["phase"]: r for r in full["phases"]}.get("P2 fight-maxroom") or {}).get("residual") or {}
    n = 57
    check("B2 8c roots", near(rr.get("no_root_t"), 1 / n) and near(rr.get("no_root_n"), 0)
          and near(rr.get("no_root_crossed_t"), 1 / n) and near(rr.get("lua_crossed"), 1 / n),
          str({k: rr.get(k) for k in ("no_root_t", "no_root_n", "no_root_crossed_t", "lua_crossed")}))
    check("B2 8c lbx", near((rr.get("lua") or {}).get("lbx"), 3 / n), str(rr.get("lua")))
    rep = b.report(full)
    check("B2 report names crossed-rootless and lbx",
          "no root (all) 0.02 t + 0.00 n ms/render, of which crossed a boundary first 0.02 t + 0.00 n" in rep
          and "Lua-crossed rooted 0.02" in rep and "lbx 0.05" in rep, rep[-700:])
    # renders of the windows that carry the records, not of every selected window
    tops = {1: {"ph120.hotw": {"rnd": {"48bc50": [0, 0, 10, 0, 0, 0, 0]}}}, 2: {}}
    w = {1: {"ph120.hcl": {"lb": "5", "lbm": "0", "lbs": "0", "lbx": "20"}}, 2: {}}
    res = b.residual(tops, w, [1, 2], {1: [0, 0, 0, 0, 0, 10], 2: [0, 0, 0, 0, 0, 30]}) or {}
    fn = {f["fn"]: f for f in res.get("functions", [])}.get("48bc50", {})
    check("residual per render over record windows", res.get("renders") == 10 and res.get("windows") == 1
          and near(fn.get("incl"), 1.0) and near((res.get("lua") or {}).get("lbx"), 2.0),
          str({"renders": res.get("renders"), "incl": fn.get("incl"), "lua": res.get("lua")}))
    # one window printed twice (a cumulative log with the same build id) is counted once,
    # the last instance wins; split lines of one record (same header, whole items) still union.
    head = "[kage-vita] ph120.hoti bid=X win=5 loops=600 phase=rnd samples=10 top="
    hs = "[kage-vita] ph120.hs bid=X win=5 loops=600 phase=rnd samples=10 x=1"
    cases = (
        ("split", [head + "48bc50:4", head + "7af0:3"], {"48bc50": [4], "7af0": [3]}),
        ("reprint", [head + "48bc50:4,7af0:3", hs, head + "48bc50:4,7af0:3"], {"48bc50": [4], "7af0": [3]}),
        ("rerun", [head + "48bc50:4,7af0:3", head.replace("samples=10", "samples=12") + "7af0:5"],
         {"7af0": [5]}),
        ("reprint split", [head + "48bc50:4", head + "7af0:3", head + "48bc50:6", head + "7af0:1"],
         {"48bc50": [6], "7af0": [1]}),
    )
    for name, lines, want in cases:
        nat = b.parse_native("\n".join(lines) + "\n")
        have = nat["tops"].get(5, {}).get("ph120.hoti", {}).get("rnd")
        check(f"parse_native duplicate window: {name}", have == want, f"{have} != {want}")
    print("residual review: rootless crossed keys, record-window renders, reprinted windows")


# Verbatim: bench-A2S-20261025/native.log lines 338-340, window 1's ph120.hotr
# (int-final-a2-20261025: GUEST_SAMPLER_CHAIN, no RESIDUAL, no census; the log has 282 hotr lines
# and no ph120.hotw/fe/hcl line).  Root 0 = 64 translated samples, 1000000 = 46 native ones.
A2S_HOTR_LINES = [
    "[kage-vita] ph120.hotr bid=rel:v0.1.2-alpha-int-final-a2-20 win=1 loops=120 phase=rnd samples=856 "
    "top=156d500:311,0:64,140eef0:61,12ce770:54,1000000:46,3e50:35,56d500:33,25af60:29,55f610:27,55f990:25,"
    "3cad40:19,2ce770:16",
    "[kage-vita] ph120.hotr bid=rel:v0.1.2-alpha-int-final-a2-20 win=1 loops=120 phase=rnd samples=856 "
    "top=14a90:16,1564f50:14,4b60:13,14b0600:13,3dbfe0:13,6dd0:9,504610:6,40eef0:6,125af60:6,5190:6,508710:5,"
    "3ca670:4",
    "[kage-vita] ph120.hotr bid=rel:v0.1.2-alpha-int-final-a2-20 win=1 loops=120 phase=rnd samples=856 "
    "top=155f990:3,155f610:3,51e0:2,1e9810:2,1014a90:2,13cad40:2,13dbfe0:2,1006dd0:2,4b0600:2,13ca670:1,"
    "564f50:1,560eb0:1",
]
A2S_RUN = "bench-A2S-20261025"
# MEASURED by a probe analyzer (bench_run.py with ph120.hotr accepted) on that log: phase ->
# (windows, renders, t, n) as printed.
A2S_NO_ROOT = {"P1 start-idle": (9, 995, "0.97", "0.74"), "P2 fight-maxroom": (9, 524, "1.16", "0.87"),
               "P3 spawn-heavy": (9, 429, "1.42", "1.00"), "P4 transitions": (8, 478, "1.16", "0.91"),
               "P5 end-idle": (3, 330, "1.05", "0.79")}
NOT_MEASURED = ("; crossed-first and Lua-crossed not measured (no ph120.hotw/hcl: a build without "
                "ISAAC_VITA_GUEST_SAMPLER_RESIDUAL has no +800000 hotr keys)")
RESIDUAL_HEAD = "\n\nrender residual per phase"
S5_RUNCMP_HEAD = "\n\nrun-comparison inputs ("
MEAN_HEAD = "\n\nmean rates per phase ("  # the mean-rate table


def drop_section(text: str, head: str) -> str:
    """text without the section that starts at `head` (blank line + title) and runs to the next
    blank line or the end; unchanged when `head` is absent."""
    i = text.find(head)
    if i < 0:
        return text
    j = text.find("\n\n", i + len(head))
    return text[:i] + (text[j:] if j >= 0 else "")


def inject(records: list[str]) -> list[str]:
    """good_native() with `records` after every window's ph120.hs (the sampler's report order),
    bid and win rewritten."""
    out = []
    for line in good_native():
        out.append(line)
        m = re.search(r"\[kage-vita\] ph120\.hs bid=\S+ win=(\d+) ", line)
        if m:
            k = int(m.group(1))
            for r in records:
                out.append(re.sub(r"\bwin=\d+", f"win={k}", re.sub(r"\bbid=\S+", f"bid={BID}", r)))
    return out


def leg_residual_r2(root: Path | None) -> None:
    """residual() used to return None without ph120.hotw/fe, so an A2-class log printed no
    no-root figure and compare never printed one.  Every check here fails on 44b406d."""
    nonce = "0123456789ab"
    game = "\n".join(good_game(nonce)) + "\n"
    a2 = b.diagnose("\n".join(inject(A2S_HOTR_LINES)) + "\n", game, nonce)
    rr = ({r["phase"]: r for r in a2["phases"]}.get("P2 fight-maxroom") or {}).get("residual") or {}
    n = 57  # W41 cad r
    check("r2 hotr-only: residual present", bool(rr), "residual() returned None without hotw/fe")
    if rr:
        check("r2 hotr-only: root denominators", rr["root_windows"] == 9 and rr["root_renders"] == 9 * n
              and rr["windows"] == 0 and rr["renders"] == 0 and rr["functions"] == [],
              str({k: rr[k] for k in ("root_windows", "root_renders", "windows", "renders")}))
        check("r2 hotr-only: no root", near(rr["no_root_t"], 64 / n) and near(rr["no_root_n"], 46 / n)
              and rr["no_root_crossed_t"] is None and rr["no_root_crossed_n"] is None
              and rr["lua_crossed"] is None,
              str({k: rr.get(k) for k in ("no_root_t", "no_root_n", "no_root_crossed_t", "lua_crossed")}))
    rep = b.report(a2)
    want = f"  P2 fight-maxroom (9 windows, {9 * n} renders): no root (all) 1.12 t + 0.81 n ms/render" + NOT_MEASURED
    check("r2 hotr-only: report line", want in rep.splitlines(), rep[-600:])
    check("r2 hotr-only: no watch table", "self_us" not in rep, rep[-300:])  # the table's header word
    cmp_aa = b.compare(a2, a2)
    check("r2 compare no-root line", "no root (all) ms/render B vs A: t 1.12->1.12, n 0.81->0.81, t+n 1.93->1.93 "
          "(+0.00); crossed-first t n/a->n/a, n n/a->n/a; Lua-crossed rooted n/a->n/a (A 9 windows 513 renders, "
          "B 9 windows 513 renders)" in cmp_aa, cmp_aa[-500:])
    check("r2 compare: no empty incl line", "residual incl ms/render B vs A: \n" not in cmp_aa + "\n"
          and "residual incl ms/render B vs A:" not in cmp_aa, cmp_aa[-300:])
    # A2 (hotr only) vs M (RESIDUAL + census: the oracle's scenario 8 records)
    m = b.diagnose("\n".join(inject(RESIDUAL_LINES)) + "\n", game, nonce)
    cmp_am = b.compare(a2, m)
    check("r2 compare A2 vs M", "no root (all) ms/render B vs A: t 1.12->0.02, n 0.81->0.02, t+n 1.93->0.04 "
          "(-1.89); crossed-first t n/a->0.00, n n/a->0.00; Lua-crossed rooted n/a->0.05" in cmp_am, cmp_am[-500:])
    plain = b.diagnose("\n".join(good_native()) + "\n", game, nonce)
    cmp_pa = b.compare(plain, a2)
    check("r2 compare names a missing side", "residual only in B: A's log has no ph120.hotr/hotw/fe" in cmp_pa,
          cmp_pa[-400:])
    # the real A2S log
    d = root / A2S_RUN if root else None
    if d is None or not (d / "native.log").exists():
        print(f"residual r2: hotr-only synthetic, compare lines; real A2S log SKIP (no {A2S_RUN} under --smoke-root)")
        return
    nt = (d / "native.log").read_text(encoding="utf-8", errors="replace")
    res = b.diagnose(nt, (d / "game.log").read_text(encoding="utf-8", errors="replace"),
                     json.loads((d / "manifest.json").read_text(encoding="utf-8"))["nonce"])
    check("r2 A2S analyze ok", res["ok"], res["verdict"])
    rep = b.report(res)
    for phase, (nw, nr, t, nn) in A2S_NO_ROOT.items():
        line = f"  {phase} ({nw} windows, {nr} renders): no root (all) {t} t + {nn} n ms/render" + NOT_MEASURED
        check(f"r2 A2S {phase} no-root line", line in rep.splitlines(),
              [x for x in rep.splitlines() if x.startswith(f"  {phase} (")])
    rows = {r["phase"]: r for r in res["phases"]}
    p2 = (rows.get("P2 fight-maxroom") or {}).get("residual") or {}
    if p2:  # the sample counts behind the printed 1.16 / 0.87 (hotr keys 0 and 1000000, windows 13-21)
        check("r2 A2S P2 no-root samples", near(p2["no_root_t"] * p2["root_renders"], 606)
              and near(p2["no_root_n"] * p2["root_renders"], 457),
              (p2["no_root_t"] * p2["root_renders"], p2["no_root_n"] * p2["root_renders"]))
    # The earlier one-off analysis's 1.15 + 0.87 is windows 15-21: the repo function
    # gives the same over those windows.
    nat = b.parse_native(nt)
    sel = list(range(15, 22))
    cad = {k: b.ints(nat["wins"][k]["ph120.s"].get("cad")) for k in sel}
    w1521 = b.residual(nat["tops"], nat["wins"], sel, cad) or {}
    check("r2 A2S windows 15-21 = the earlier analysis", w1521.get("root_windows") == 7 and w1521.get("root_renders") == 415
          and f"{w1521.get('no_root_t', 0):.2f} {w1521.get('no_root_n', 0):.2f}" == "1.15 0.87",
          {k: w1521.get(k) for k in ("root_windows", "root_renders", "no_root_t", "no_root_n")})
    # Nothing else in the analyze text moves: before the residual section it is the stored
    # analyze.txt (written by the analyzer of the device session), CRLF folded.
    stored = (d / "analyze.txt").read_bytes().decode("utf-8", "replace").replace("\r\n", "\n")
    # The S5 run-comparison table (printed for every run) postdates the
    # stored file: drop that one section, header to blank line, and compare the rest.
    # The mean-rate table postdates it too: dropped the same way.
    head = drop_section(drop_section(rep.partition(RESIDUAL_HEAD)[0], S5_RUNCMP_HEAD), MEAN_HEAD) + "\n"
    check("r2 A2S S5 run-comparison table present and dropped",
          S5_RUNCMP_HEAD in rep and S5_RUNCMP_HEAD not in head and S5_RUNCMP_HEAD not in stored, "")
    check("r2 A2S mean-rate table present and dropped",
          MEAN_HEAD in rep and MEAN_HEAD not in head and MEAN_HEAD not in stored, "")
    check("r2 A2S text before the residual section = stored analyze.txt",
          head == stored.partition(RESIDUAL_HEAD)[0].rstrip("\n") + "\n" and RESIDUAL_HEAD in rep,
          f"{len(head)} vs {len(stored)} chars")
    cmp_txt = b.compare(res, res)
    check("r2 A2S compare P2", "no root (all) ms/render B vs A: t 1.16->1.16, n 0.87->0.87, t+n 2.03->2.03 (+0.00)"
          in cmp_txt, cmp_txt[-400:])
    print(f"residual r2: hotr-only synthetic, compare lines; real A2S log: {len(A2S_NO_ROOT)} phases pinned, "
          f"windows 15-21 = the earlier analysis, text before the residual section = stored analyze.txt")


# --------------------------------------------------------------------------------------- dry
# The dry section.  The Downpour part is good_game's (ROOMS; its last load,
# (128, 71), is the '@room 3' after 'DRY START' that ends P5, as BENCH DONE's did); then 'stage 1'
# builds Basement I: start room, lead-in @room 1, P6 start, P7 max, P8 2nd, BENCH DONE 3rd.
ROOMS_DRY = ROOMS + [(130, 2), (132, 60), (134, 2), (154, 85), (174, 70), (194, 71)]
DRY_WINDOWS = 90  # window k ends at 30 + 2k s
# Basement I of BVPF Q3ET = _stageSeeds[1] (Level::Init index stage + (type in 4, 5)); the
# arithmetic leg_seed checks against six device pairs (Downpour I = _stageSeeds[2]).
WANT_DRY = 4128373806
DRY_PHASE_NAMES = ["P6 dry-start-idle", "P7 dry-fight-maxroom", "P8 dry-spawn-heavy"]
PHASES_NEW = ["P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"]
# sha256 of the command lines (no comments, no blank lines, '\n'-joined) of the bench list before
# the dry section was added; the current file minus its dry section ('@marker DRY START' up to '@marker BENCH DONE')
# must give the same lines (P1-P5 byte-identical).
OLD_LIST_SHA = "968890474d8fcf6d066fa52449a74f974faa2dda02fe2561113944fd50dd2bb1"


def mark(name: str, stagetype: int = 4, backdrop="31", water="?", haswater="true", fields=True,
         familiars="0") -> str:
    """A MARK line as main.lua logs it (state | census | room fields); fields=False = the
    older main.lua shape without the room fields; familiars=None = a census without it."""
    level = "Downpour I" if stagetype == 4 else "Basement I"
    fam = "" if familiars is None else f"familiars={familiars} "
    head = (f"MARK {name} | greed=false difficulty=0 stage=1 stagetype={stagetype} level={level} roomtype=1 "
            f"roomidx=84 frame=1 time=1 | entities=3 npc=1 tears=0 {fam}projectiles=0 effects=1 lasers=0 other=0")
    return rc(head if not fields else f"{head} | backdrop={backdrop} water={water} haswater={haswater}")


# familiars= at all 7 MARKs from BENCH START to BENCH DONE on 43 of 43 October bench runs with
# markers (0 at NONCE, before giveitem)
BENCH_FAMILIARS = "5"


def good_game_dry(nonce: str, dry_seed: int = WANT_DRY, dry_type: int = 0, basement=None, level=True) -> list[str]:
    """good_game with the dry section and MARK room fields; basement(name) -> mark() keyword
    overrides for the four markers logged on the dry floor (P6, P7, P8, BENCH DONE)."""
    r = room_lines(ROOMS_DRY)

    def mark(name, **kw):  # the device's item build: familiars=5 at every marker
        return globals()["mark"](name, **dict({"familiars": BENCH_FAMILIARS}, **kw))
    g = list(GAME_HEAD)
    g += ["[INFO] - Level::Init m_Stage 1, m_StageType 4 Seed 1258606820", r(),
          rc("game start 1: seed=BS8Y SWXQ continued=false"), rc("save data: 2300 bytes"),
          rc(f"MARK NONCE {nonce} | x"), rc("wait 30 update(s) [start]: 80 command(s) deferred"),
          rc(f"seed BS8Y SWXQ -> {WANT_SEED}: set in place (SetStartSeed) [start]; the next stage command "
             "builds the floor from it"),
          rc("exec [start]: stage 1c"),
          f"[INFO] - Level::Init m_Stage 1, m_StageType 4 Seed {WANT_LEVEL}"]
    g += [r(), mark("BENCH START"), r(), mark("P1 start-idle"), r(), mark("P2 fight-maxroom"), r(),
          mark("P3 spawn-heavy"), r(), mark("P4 transitions"), r(), r(), r(), r(), r(), r(), mark("P5 end-idle"),
          r(), mark("DRY START"), r(), rc("exec [start]: stage 1"),
          *([f"[INFO] - Level::Init m_Stage 1, m_StageType {dry_type} Seed {dry_seed}"] if level else []),
          r(), rc("result [stage 1]: Changed stage."), r()]
    for name in DRY_PHASE_NAMES + ["BENCH DONE"]:
        kw = {"stagetype": dry_type, "backdrop": "1", "haswater": "false"}
        kw.update(basement(name) if basement else {})
        g += [mark(name, **kw), r()]
    g.append(rc("remove [start]: save data removed, HasModData=false"))
    return g


def dry_native(edit=None) -> str:
    """good_native over ROOMS_DRY and 90 windows, every window quiet (drop 0); edit(k, line) may
    change a window's ph120.s line."""
    out = []
    for ln in good_native(ROOMS_DRY, DRY_WINDOWS):
        m = re.search(r"\[kage-vita\] ph120\.s bid=\S+ win=(\d+) ", ln)
        if m:
            ln = ln.replace("drop(t,us)=5,83334", "drop(t,us)=0,0")
            ln = edit(int(m.group(1)), ln) if edit else ln
        out.append(ln)
    return "\n".join(out) + "\n"


def fd_rows(keys, **kw) -> list[dict]:
    """Minimal floor-debt phase rows for floor_debt_gates (every field it reads)."""
    base = dict(windows=9, fd_windows=9, fn=9, fx=0, fxgap=0, pg=0, slack_l=0, slack_lus=0, bmin=-1.0,
                fres_obs=0.01, fres_on=0.01, capk=0.95, above_ms=1.0, capw=0, latemax=4.0, gfmax=0.2,
                gfstuck=0, ups_sum=29.95, kept_late=0.9, clampn=0)
    return [dict(base, phase=k, **kw.get(k, {})) for k in keys]


def leg_dry() -> None:
    nonce = "0123456789ab"
    man = {"phases": PHASES_NEW}
    nat = dry_native()

    def run(game=None, native=None, manifest=man):
        return b.diagnose(native or nat, "\n".join(game or good_game_dry(nonce)) + "\n", nonce, manifest=manifest)
    res = run()
    check("dry run ok", res["ok"], res["verdict"] + " " + str([x for x in res["diagnostics"] if x[0] != "OK"]))
    check("dry run: OK SEED, SEED_DRY, DRY_PHASE, DRY_ITEMS", {"SEED", "SEED_DRY", "DRY_PHASE", "DRY_ITEMS"} <= codes(res, "OK"),
          codes(res, "OK"))
    check("dry run: only WARN INTERCEPT_UNSEEN", codes(res, "WARN") == {"INTERCEPT_UNSEEN"}, codes(res, "WARN"))
    check("dry run: 3 OK DRY_PHASE", sum(1 for x in res["diagnostics"] if x[1] == "DRY_PHASE") == 3, "")
    rows = {r["phase"]: r for r in res["phases"]}
    check("dry phase names", list(rows) == ["P1 start-idle", "P2 fight-maxroom", "P3 spawn-heavy", "P4 transitions",
                                            "P5 end-idle"] + DRY_PHASE_NAMES, list(rows))
    for p in DRY_PHASE_NAMES:
        check(f"{p} windows", rows.get(p, {}).get("windows") == 9, rows.get(p, {}).get("windows"))
    # P1-P5 exactly as the list without the dry section (same rooms and windows up to P5's end)
    old = b.diagnose("\n".join(ln.replace("drop(t,us)=5,83334", "drop(t,us)=0,0") for ln in good_native()) + "\n",
                     "\n".join(good_game(nonce)) + "\n", nonce)
    orow = {r["phase"]: r for r in old["phases"]}
    check("dry run: P1-P5 rows identical to the old list's", all(rows[k] == orow[k] for k in orow) and len(orow) == 5,
          [k for k in orow if rows.get(k) != orow[k]])
    check("dry run: Downpour rooms = the old list's (cut at the dry Level::Init)",
          res["seed_rooms"] == old["seed_rooms"] and len(res["seed_rooms"]) == 13, res["seed_rooms"])
    check("dry run: dry rooms", res["seed_rooms_dry"] == [[1, n] for _, n in ROOMS_DRY[-6:]]
          and res["seed_rooms_dry"] == [[1, 2], [1, 60], [1, 2], [1, 85], [1, 70], [1, 71]], res["seed_rooms_dry"])
    check("Basement I seed arithmetic", b.level_seed(b.string2seed(WANT_SEED), 1, 0) == WANT_DRY
          and b.level_seed(b.string2seed(WANT_SEED), 1, 4) == WANT_LEVEL, "")
    check("dry run: seed_level_dry", res["seed_level_dry"] == {"want": WANT_SEED, "stage": 1, "stage_type": 0,
                                                               "seed": WANT_DRY, "expected": WANT_DRY},
          res["seed_level_dry"])
    check("dry run: dry verdicts", [(x["phase"], x["verdict"], x["closing"]) for x in res["dry"]] ==
          [("P6 dry-start-idle", "dry", "P7 dry-fight-maxroom"), ("P7 dry-fight-maxroom", "dry", "P8 dry-spawn-heavy"),
           ("P8 dry-spawn-heavy", "dry", "BENCH DONE")], res["dry"])
    check("dry run: expected phases from the manifest", res["expected_phases"] == PHASES_NEW, res["expected_phases"])
    # mean rates: W41's cad g 60, r 57, p 57 per 2-s window; P2 = windows 17..25
    for p in ("P2 fight-maxroom", "P7 dry-fight-maxroom"):
        r = rows[p]
        check(f"{p} mean rates", near(r.get("pps_mean"), 28.5) and near(r.get("rps_mean"), 28.5)
              and near(r.get("ups_mean"), 30.0), (r.get("pps_mean"), r.get("rps_mean"), r.get("ups_mean")))
    two = b.diagnose(dry_native(lambda k, ln: ln.replace(",57,57,57 ", ",57,57,114 ") if k == 25 else ln),
                     "\n".join(good_game_dry(nonce)) + "\n", nonce, manifest=man)
    r2 = {r["phase"]: r for r in two["phases"]}["P2 fight-maxroom"]
    check("P2 pps_mean = sum p / sum at-delta (one window 114 presents)", near(r2.get("pps_mean"), (8 * 57 + 114) / 18)
          and near(r2.get("fps"), rows["P2 fight-maxroom"]["fps"]), (r2.get("pps_mean"), r2.get("fps")))
    short = b.diagnose(dry_native(lambda k, ln: ln.replace("cad(w,s,d,m,g,r,b,p)=120,120,120,120,60,57,57,57",
                                                           "cad(w,s,d,m,g,r,b,p)=120,120,120,120,60,57,57") if k == 20 else ln),
                       "\n".join(good_game_dry(nonce)) + "\n", nonce, manifest=man)
    r3 = {r["phase"]: r for r in short["phases"]}["P2 fight-maxroom"]
    check("P2 mean rates None with a 7-field cad", r3.get("pps_mean") is None and r3.get("ups_mean") is None, r3.get("pps_mean"))
    rep = b.report(res)
    check("report: mean-rate table", "mean rates per phase (sum of cad p" in rep and any(
        ln.startswith("  P7 dry-fight-maxroom") and "28.50" in ln for ln in rep.splitlines()), "")
    check("report: dry block", "  P6 dry-start-idle      dry       floor 1/0 -> 1/0, backdrop 1, water ?, haswater false, "
          "familiars 5 -> 5 (start 5) (closing MARK P7 dry-fight-maxroom)" in rep.splitlines(),
          [x for x in rep.splitlines() if "P6 dry" in x])
    # expected phases: from the manifest, else PHASES_EXPECTED
    gone = run(manifest={"phases": PHASES_NEW + ["P9"]})
    check("manifest phase without data: MISSING_PHASES", "MISSING_PHASES" in codes(gone)
          and "phases without data: P9" in gone["verdict"], gone["verdict"])
    # without a manifest list, a log with MARK DRY START expects
    # the dry phases too; a log without it (every run before the dry section) keeps P1-P5
    nolist = run(manifest={})
    check("manifest without phases, dry log: P1-P8", nolist["ok"] and nolist["expected_phases"] == PHASES_NEW,
          nolist["expected_phases"])
    no_p7 = [x for x in good_game_dry(nonce) if "MARK P7 " not in x]
    for what, mf in (("no manifest", None), ("manifest {}", {}), ("manifest phases []", {"phases": []})):
        x = run(no_p7, manifest=mf)
        check(f"dry log without MARK P7, {what}: FAIL MISSING_PHASES P7", not x["ok"] and "MISSING_PHASES" in codes(x)
              and "phases without data: P7" in x["verdict"], x["verdict"])
    x = run(no_p7)
    check("dry log without MARK P7, manifest P1-P8: FAIL MISSING_PHASES P7", "phases without data: P7" in x["verdict"],
          x["verdict"])
    old_nolist = b.diagnose("\n".join(good_native()) + "\n", "\n".join(good_game(nonce)) + "\n", nonce)
    check("old log without a manifest: P1-P5, ok", old_nolist["ok"]
          and old_nolist["expected_phases"] == list(b.PHASES_EXPECTED), old_nolist["expected_phases"])
    short_list = b.diagnose("\n".join(good_native()) + "\n", "\n".join(good_game(nonce)) + "\n", nonce, manifest=man)
    check("old list against a new manifest: MISSING_PHASES P6, P7, P8",
          "phases without data: P6, P7, P8" in short_list["verdict"], short_list["verdict"])
    check("old list: no dry checks, no dry floor", short_list["dry"] == [] and short_list["seed_level_dry"] is None
          and not any(x[1].startswith(("DRY_", "SEED_DRY")) for x in short_list["diagnostics"]), short_list["dry"])
    # the dry floor's seed
    bad = run(good_game_dry(nonce, dry_seed=123))
    check("wrong Basement seed: WARN SEED_DRY_LEVEL", bad["ok"] and "SEED_DRY_LEVEL" in codes(bad, "WARN")
          and bad["seed_rooms_dry"] is None and bad["seed_rooms"] == res["seed_rooms"]
          and any(f"not {WANT_SEED}'s Basement I stage seed {WANT_DRY}" in x[2] for x in bad["diagnostics"]), codes(bad, "WARN"))
    wet = run(good_game_dry(nonce, dry_seed=WANT_LEVEL, dry_type=4,
                            basement=lambda n: {"stagetype": 4, "backdrop": "31", "haswater": "true"}))
    check("Downpour after DRY START: SEED_DRY_LEVEL + 3 x DRY_PHASE_NOT_DRY", "SEED_DRY_LEVEL" in codes(wet, "WARN")
          and sum(1 for x in wet["diagnostics"] if x[1] == "DRY_PHASE_NOT_DRY") == 3
          and any("not Basement I (m_Stage 1, m_StageType 0)" in x[2] for x in wet["diagnostics"]), codes(wet, "WARN"))
    nolv = run(good_game_dry(nonce, level=False))
    check("no Level::Init after DRY START: WARN SEED_DRY_NO_LEVEL", "SEED_DRY_NO_LEVEL" in codes(nolv, "WARN")
          and nolv["seed_level_dry"] is None, codes(nolv, "WARN"))
    # "stage 1c" built no floor (P1-P5 on the random boot
    # floor), "stage 1" did.  The Basement floor after MARK DRY START matches the start seed's own
    # (1, 0) stage seed, so taking it as the seeded floor made this OK SEED; 58f4f9d's list (no dry
    # section) says SEED_NO_LEVEL for the same failure, and ab_gate refuses it (no proven floor seed).
    c2 = run([x for x in good_game_dry(nonce) if x != f"[INFO] - Level::Init m_Stage 1, m_StageType 4 Seed {WANT_LEVEL}"])
    check("C2: stage 1c built no floor -> WARN SEED_NO_LEVEL, not OK SEED", "SEED_NO_LEVEL" in codes(c2, "WARN")
          and "SEED" not in codes(c2, "OK") and c2["seed_level"] is None and c2["seed_rooms"] is None,
          (codes(c2, "OK"), codes(c2, "WARN"), c2["seed_level"]))
    check("C2: the dry floor is unchecked (the run proved no start seed)", "SEED_DRY" not in codes(c2, "OK") and any(
        x[1] == "SEED_DRY_LEVEL" and "the run proved no start seed" in x[2] for x in c2["diagnostics"])
        and c2["seed_rooms_dry"] is None, [x for x in c2["diagnostics"] if x[1].startswith("SEED")])
    # a failed "stage 1" must not append the dry section's rooms to the
    # Downpour seeded rooms, or ab_gate refuses the set (room sequences differ across runs)
    check("R2-1: stage 1 built no floor -> seed_rooms = the good run's", nolv["seed_rooms"] == res["seed_rooms"]
          and nolv["seed_rooms"] is not None, (len(nolv["seed_rooms"] or []), len(res["seed_rooms"] or [])))
    # dry markers without 'MARK DRY START' fail closed; the Downpour floor
    # is still the seeded one and P5 does not run on over the stage load
    nodry = run([x for x in good_game_dry(nonce) if "MARK DRY START" not in x])
    check("R2-2: lost DRY START -> FAIL DRY_START_MISSING", "DRY_START_MISSING" in codes(nodry)
          and not nodry["ok"], codes(nodry))
    check("R2-2: lost DRY START keeps the Downpour seeded floor and rooms", nodry["seed_level"] is not None
          and (nodry["seed_level"]["stage"], nodry["seed_level"]["stage_type"]) == (1, 4)
          and nodry["seed_rooms"] == res["seed_rooms"], nodry["seed_level"])
    c2nodry = run([x for x in good_game_dry(nonce) if "MARK DRY START" not in x
                   and x != f"[INFO] - Level::Init m_Stage 1, m_StageType 4 Seed {WANT_LEVEL}"])
    check("R2-2: C2 + lost DRY START -> FAIL DRY_START_MISSING, WARN SEED_NO_LEVEL, not OK SEED",
          "DRY_START_MISSING" in codes(c2nodry) and "SEED_NO_LEVEL" in codes(c2nodry, "WARN")
          and "SEED" not in codes(c2nodry, "OK") and c2nodry["seed_level"] is None,
          (codes(c2nodry), codes(c2nodry, "WARN"), c2nodry["seed_level"]))
    check("the seeded floor is Downpour I (1, 4), the floor 'stage 1c' builds",
          (res["seed_level"]["stage"], res["seed_level"]["stage_type"]) == (1, 4), res["seed_level"])
    # the item build across "stage 1", from the census's familiars
    check("intact build: OK DRY_ITEMS", "DRY_ITEMS" in codes(res, "OK") and not any(
        x[1].startswith("DRY_ITEMS_") for x in res["diagnostics"]), [x for x in res["diagnostics"] if "ITEMS" in x[1]])
    check("dry entries carry familiars", [(x.get("familiars_start"), x.get("familiars_opening"), x.get("familiars")) for x in res["dry"]]
          == [("5", "5", "5")] * 3, [(x.get("familiars_start"), x.get("familiars")) for x in res["dry"]])
    on_dry = set(DRY_PHASE_NAMES + ["BENCH DONE"])
    for what, over, want in (
            ("lost at stage 1 (0 on every dry-floor marker)", {n: {"familiars": "0"} for n in on_dry}, "DRY_ITEMS_CHANGED"),
            ("lost by BENCH DONE only", {"BENCH DONE": {"familiars": "0"}}, "DRY_ITEMS_CHANGED"),
            ("one more at P6", {"P6 dry-start-idle": {"familiars": "6"}}, "DRY_ITEMS_CHANGED"),
            ("census without familiars at P7", {"P7 dry-fight-maxroom": {"familiars": None}}, "DRY_ITEMS_UNPROVEN"),
            ("'?' at P8", {"P8 dry-spawn-heavy": {"familiars": "?"}}, "DRY_ITEMS_UNPROVEN")):
        x = run(good_game_dry(nonce, basement=lambda n, o=over: o.get(n, {})))
        got = [d for d in x["diagnostics"] if "ITEMS" in d[1]]
        check(f"items {what}: WARN {want}", x["ok"] and [(d[0], d[1]) for d in got] == [("WARN", want)]
              and sum(1 for d in x["diagnostics"] if d[1] == "DRY_PHASE") == 3, got)
    lost = run(good_game_dry(nonce, basement=lambda n: {"familiars": "0"}))
    check("items lost: the message names BENCH START's count and the dry markers'", any(
        x[1] == "DRY_ITEMS_CHANGED" and "MARK BENCH START familiars=5" in x[2] and "MARK P7 dry-fight-maxroom familiars=0"
        in x[2] and "MARK BENCH DONE familiars=0" in x[2] for x in lost["diagnostics"]), codes(lost, "WARN"))
    check("items lost: the dry entries carry the counts", [(x.get("familiars_start"), x.get("familiars_opening"),
          x.get("familiars")) for x in lost["dry"]] == [("5", "0", "0")] * 3, [x.get("familiars") for x in lost["dry"]])
    zero = run([x.replace("familiars=5 ", "familiars=0 ") for x in good_game_dry(nonce)])
    check("familiars=0 throughout: the dry entries carry start 0", [x.get("familiars_start") for x in zero["dry"]]
          == ["0"] * 3, [x.get("familiars_start") for x in zero["dry"]])
    check("familiars=0 from BENCH START on: WARN DRY_ITEMS_UNPROVEN (the census cannot see the build)",
          any(x[1] == "DRY_ITEMS_UNPROVEN" and "BENCH START familiars=0" in x[2] for x in zero["diagnostics"])
          and "DRY_ITEMS" not in codes(zero, "OK"), codes(zero, "WARN"))
    nostart = run([x for x in good_game_dry(nonce) if "MARK BENCH START " not in x])
    check("no MARK BENCH START: not OK DRY_ITEMS", "DRY_ITEMS" not in codes(nostart, "OK")
          and "DRY_ITEMS_UNPROVEN" in codes(nostart, "WARN"), codes(nostart, "WARN"))
    # the room fields of each dry phase come from the NEXT marker
    for what, over, want in (
            ("haswater=true at BENCH DONE", {"BENCH DONE": {"haswater": "true"}},
             {"P8 dry-spawn-heavy": "MARK BENCH DONE haswater=true (water amount > 0)"}),
            ("water=0.25 at P7", {"P7 dry-fight-maxroom": {"water": "0.25"}},
             {"P6 dry-start-idle": "MARK P7 dry-fight-maxroom water=0.25 > 0"}),
            ("backdrop 31 at P8", {"P8 dry-spawn-heavy": {"backdrop": "31"}},
             {"P7 dry-fight-maxroom": "MARK P8 dry-spawn-heavy backdrop=31, not BASEMENT (1)"}),
            ("stage type 4 at P7", {"P7 dry-fight-maxroom": {"stagetype": 4}},
             {"P6 dry-start-idle": "MARK P7 dry-fight-maxroom stage=1 stagetype=4, not Basement I (1/0)",
              "P7 dry-fight-maxroom": "MARK P7 dry-fight-maxroom stage=1 stagetype=4, not Basement I (1/0)"})):
        x = run(good_game_dry(nonce, basement=lambda n, o=over: o.get(n, {})))
        got = {d[2].split(":", 1)[0][len("phase "):]: d[2] for d in x["diagnostics"] if d[1] == "DRY_PHASE_NOT_DRY"}
        check(f"not dry: {what}", x["ok"] and set(got) == set(want) and all(want[k] in got[k] for k in want),
              got)
    numeric = run(good_game_dry(nonce, basement=lambda n: {"water": "0.0", "haswater": "?"}))
    check("water=0.0 with haswater=? proves dry", sum(1 for x in numeric["diagnostics"] if x[1] == "DRY_PHASE") == 3,
          codes(numeric, "WARN"))
    for what, over in (("bindings absent (main.lua '?')", {"backdrop": "?", "haswater": "?"}),
                       ("older main.lua (no room fields)", {"fields": False})):
        x = run(good_game_dry(nonce, basement=lambda n, o=over: o))
        check(f"unproven: {what}", x["ok"] and sum(1 for d in x["diagnostics"] if d[1] == "DRY_PHASE_UNPROVEN") == 3
              and "DRY_PHASE_NOT_DRY" not in codes(x, "WARN"), codes(x, "WARN"))
    # rooms_line: the dry floor is a suffix of an identical seeded floor
    L = b.rooms_line
    check("rooms_line dry/dry", L(res, res) == f"rooms: identical (floor seed {WANT_LEVEL}, 13 room loads); dry floor "
          f"identical (seed {WANT_DRY}, 6 room loads)", L(res, res))
    check("rooms_line old/old unchanged", L(old, old) == f"rooms: identical (floor seed {WANT_LEVEL}, 13 room loads)",
          L(old, old))
    check("rooms_line dry/old", L(res, old).endswith("; dry floor: only A has a proven one (OK SEED_DRY)")
          and L(res, old).startswith("rooms: identical"), L(res, old))
    moved = dict(res, seed_rooms_dry=[[1, 2], [1, 61]] + res["seed_rooms_dry"][2:])
    check("rooms_line dry rooms differ", L(res, moved).endswith(f"; dry floor: DIFFER at room load 2 (A [1, 60], B [1, 61]; "
                                                                f"seed {WANT_DRY} in both)"), L(res, moved))
    other = dict(res, seed_level_dry=dict(res["seed_level_dry"], seed=5))
    check("rooms_line dry seeds differ", L(res, other).endswith(f"; dry floor: DIFFER (seed A {WANT_DRY}, B 5)"),
          L(res, other))
    # floor-debt gates: dry phases are information, the verdict lines do not move
    steady = ["P1 start-idle", "P2 fight-maxroom", "P3 spawn-heavy", "P5 end-idle"]
    for mode in ("observe", "on"):
        g0 = b.floor_debt_gates(fd_rows(steady), [], mode)
        g1 = b.floor_debt_gates(fd_rows(steady + ["P6 dry-start-idle"], **{"P6 dry-start-idle": {"capk": 0.1,
                                                                                               "ups_sum": 20.0}}), [], mode)
        verdicts0 = [x for x in g0 if x.startswith(("R1.4 cap", "R8.2 UPS"))]
        verdicts1 = [x for x in g1 if x.startswith(("R1.4 cap", "R8.2 UPS"))]
        check(f"fd gates {mode}: R1.4/R8.2 verdict lines unchanged by a bad dry phase",
              verdicts0 == verdicts1 and all("PASS" in x for x in verdicts1), verdicts1)
        info = [x for x in g1 if " dry phases (information" in x]
        check(f"fd gates {mode}: dry information line", len(info) == 1 and "P6" in info[0], info)
        rec = [x for x in g1 if x.startswith(("R1.5", "R8 clamp"))]
        check(f"fd gates {mode}: record lines list P6", rec and all("P6" in x for x in rec), rec)
    # run gates: the dry phases are listed as information, P1-P3 decide
    lines = b.run_gates([res, res], [res, res], "on", expect_mode=False)
    r85 = next((x for x in lines if x.startswith("R8.5")), "")
    check("run gates: R8.5 PASS, P6-P8 information", r85.split(": ", 1)[-1].startswith("PASS")
          and all(f"{p} +0.00 (tol 0.50, df 2, information)" in r85 for p in ("P5", "P6", "P7", "P8"))
          and "P1 +0.00 (tol 0.50, df 2)" in r85, r85)
    check("run gates: R8 information lists P8", any(x.startswith("  P8 ups_q") for x in lines), "")
    obs = b.run_gates([res], [res], "observe", expect_mode=False)
    check("run gates observe: R1.6a unchanged (P1, P2, P3, P5)", any(
        x.startswith("R1.6a OBSERVE1 decisions") and "PASS -- P1 viol 0 reset 0, P2 viol 0 reset 0, P3 viol 0 reset 0, "
        "P5 viol 0 reset 0" in x for x in obs), [x for x in obs if x.startswith("R1.6a")])
    check("run gates observe: R1.6c lists P7", any(x.startswith("  P7 ups_q") for x in obs), "")
    print(f"dry: 8 phases, P1-P5 = the old list's rows and rooms, Basement seed, {len(DRY_PHASE_NAMES)} dry checks "
          "x (dry / not dry / unproven), C2 (no Downpour floor), items across stage 1, expected phases without a "
          "manifest, mean rates, report, rooms_line, floor-debt and run gates as information")


# ----------------------------------------------------------------------------------- windows
def leg_windows() -> None:
    """4e7dc84's synthetic phase test, on the new parser."""
    B = f"bid={BID}"

    def native(rooms, nwin=60):
        lines = [BUILD]
        for w in range(1, nwin + 1):
            gap = 33_000 if w < 20 else 16_700
            lines.append(f"[kage-vita] ph120.t {B} win={w} loops={w*120} us(50,95,max) svc=1/2/3 upd=1700/2500/9000 "
                         f"rnd=15000/20000/30000 swp=100/200/300 lim=0/0/0 all=16000/30000/40000 "
                         f"gap(n,min,50,95,max)=60/10000/{gap}/{gap*2}/{gap*5} bad=0 clamp=0")
            lines.append(f"[kage-vita] ph120.s {B} win={w} loops={w*120} at={w * 2_000_000:016x} "
                         "cad(w,s,d,m,g,r,b,p)=120,120,120,120,60,60,60,60 x")
        for seq, (t, a, r) in enumerate(rooms, 1):
            lines.append(f"[kage-vita] roomlog {B} report_win=1 epoch=1 seq={seq} at=00000000:{t:08x} assigned=1 "
                         f"win=1 loop=1 pair={a},{r} sat=0")
        return b.parse_native("\n".join(lines))
    game = b.parse_game("\n".join([
        "[INFO] - Room 1.2(Start Room)", rc("MARK NONCE abc | x"), rc("MARK P1 a | x"),
        "[INFO] - Room 1.2(Start Room)", rc("MARK P1 a | x"), "[INFO] - Room 1.2(Start Room)", rc("MARK P2 b | x"),
        "[INFO] - Room 1.40(New Room)", rc("MARK P4 transitions | x"), "[INFO] - Room 1.41(New Room)",
        "[INFO] - Room 1.42(New Room)", rc("MARK BENCH DONE | x"), "[INFO] - Room 1.43(New Room)"]))
    rooms = [(1_000_000, 1, 99), (3_000_000, 1, 2), (5_000_000, 1, 2), (10_000_000, 1, 2), (40_000_000, 1, 40),
             (70_000_000, 1, 41), (80_000_000, 1, 42), (100_000_000, 1, 43)]
    nat = native(rooms)
    pw, at = b.phase_windows(nat, game)
    rows = {name: b.phase_row(name, nat["wins"], sel, span, at, sorted(at)) for name, sel, span in pw}
    check("windows names", list(rows) == ["P1 a", "P2 b", "P4 transitions"], list(rows))
    check("windows P1", rows["P1 a"]["windows"] == 14 and near(rows["P1 a"]["fps"], 1e6 / 33_000), rows["P1 a"])
    check("windows P2", rows["P2 b"]["windows"] == 14 and near(rows["P2 b"]["gap50"], 16.7), rows["P2 b"])
    check("windows P2 max", near(rows["P2 b"]["gapmax"], 16_700 * 5 / 1e3), rows["P2 b"]["gapmax"])
    check("windows P4 no skip", rows["P4 transitions"]["windows"] == 15, rows["P4 transitions"]["windows"])
    try:
        b.phase_windows(native(rooms[:-2] + [(100_000_000, 1, 43)]), game)
        check("windows mismatch rejected", False, "accepted")
    except ValueError:
        pass
    print("windows: 4e7dc84 arithmetic holds")


# ----------------------------------------------------------------------------------- prepare / listing
def leg_prepare() -> None:
    with tempfile.TemporaryDirectory() as td:
        run = Path(td) / "run"
        out0 = io.StringIO()
        with contextlib.redirect_stdout(out0):
            rcode = b.main(["prepare", "--out", str(run), "--tag", "T", "--nonce", "feedfacecafe"])
        check("prepare rc", rcode == 0, rcode)
        man = json.loads((run / "manifest.json").read_text(encoding="utf-8"))
        check("manifest nonce", man["nonce"] == "feedfacecafe", man["nonce"])
        save = (run / "save2.dat").read_text(encoding="utf-8")
        lines = save.split("\n")
        check("nonce right after @continue", lines[lines.index("@continue") + 1] == "@marker NONCE feedfacecafe", "")
        nonces = [ln for ln in lines if ln.startswith("@marker NONCE")]
        check("nonce once", len(nonces) == 1, nonces)
        cmds = [ln for ln in lines if ln and not ln.startswith("#")]
        check("seed deferred by @wait", cmds[:4] == ["@continue", "@marker NONCE feedfacecafe", "@wait 30",
                                                     "@seed 20261017"], cmds[:4])
        check("last command @remove", cmds[-1] == "@remove", cmds[-1])
        for f in man["upload"]:
            check(f"manifest sha {f['local']}", b.sha256(run / f["local"]) == f["sha256"], "")
        remotes = {f["remote"] for f in man["upload"]}
        check("uploads", remotes == {b.MOD_DATA + "/save2.dat", b.MOD_MAIN, b.ARGS_FILE}, remotes)
        check("no other slot uploaded", not any("save1" in r or "save3" in r for r in remotes), remotes)
        proc = (run / "procedure.txt").read_text(encoding="utf-8")
        for needle in ("vita_deploy_eboot", "launch=false", "sleep 60 s", "this file is what goes back", "vita_kill",
                       "bench_run.py analyze", "bench_run.py listing", "vita_delete " + b.ARGS_FILE,
                       "--audit <audit.jsonl>"):
            check(f"procedure mentions {needle}", needle in proc, "")
        check("procedure: no install_vpk", "install_vpk" not in proc and "isaac-manager" not in proc, "")
        check("procedure: no input", not any(x in proc for x in ("vita_tap", "input_script", "screenshot")), "")
        check("no --eboot: placeholder", "<bench eboot>" in proc and "eboot" not in man, "")
        # a run prepared without --eboot has kill_record and no eboot, so killrecord cannot test the
        # deploy's eboot; prepare says so
        check("no --eboot: kill_record true and no eboot (the exposed runs)",
              man.get("kill_record") is True and "eboot" not in man, man)
        check("no --eboot: prepare WARNs", "WARN no --eboot: killrecord cannot check that the deploy it binds "
              "installed this run's eboot" in out0.getvalue(), out0.getvalue()[:300])
        # the kill step says what to do on a kill error
        kstep = next((s for s in proc.splitlines() if f". vita_kill {b.TITLE} (app control)" in s), "")
        check("procedure: on a kill error, vita_app_status first, repeat only while running",
              f"On an error: vita_app_status {b.TITLE} first, and repeat vita_kill only while it says running=true"
              in kstep and "a retry would become the bound kill" in kstep, kstep)
        check("procedure: killrecord's refusal names the repro save2.dat and errored uploads",
              f"refused if another file was uploaded to {b.MOD_DATA}/save2.dat after it and before the kill, even "
              "by an upload that errored" in proc, "")
        # the manifest names the list's phases and length; the second sleep follows it
        check("manifest phases", man.get("phases") == PHASES_NEW, man.get("phases"))
        check("manifest list_updates", man.get("list_updates") == 4350, man.get("list_updates"))
        check("procedure: second sleep for the dry list", "sleep 230 s (150 s for the list's first 2745 updates, + 80 s "
              "for its other 1605 at 22 updates/s: the dry phases)." in proc, "")
        check("second sleep arithmetic", (b.second_sleep_s(2745), b.second_sleep_s(0), b.second_sleep_s(2746),
                                          b.second_sleep_s(4350)) == (150, 150, 160, 230), "")
        tmpl = (HERE / "bench-save.txt").read_text(encoding="utf-8")
        cmds_all = [ln for ln in tmpl.split("\n") if ln and not ln.startswith("#")]
        i, j = cmds_all.index("@marker DRY START"), cmds_all.index("@marker BENCH DONE")
        old_list = "\n".join(cmds_all[:i] + cmds_all[j:])
        check("bench-save.txt minus the dry section = the list before the dry section",
              hashlib.sha256(old_list.encode("utf-8")).hexdigest() == OLD_LIST_SHA, "")
        check("old list: 2745 updates, P1-P5", (b.list_updates(old_list), b.list_phases(old_list)) == (2745, PHASES_NEW[:5]),
              (b.list_updates(old_list), b.list_phases(old_list)))
        for what, m in (("old list", dict(man, list_updates=2745)), ("manifest without list_updates",
                                                                     {k: v for k, v in man.items() if k != "list_updates"})):
            steps = b.procedure(run, m).splitlines()
            check(f"procedure for the {what}: 'sleep 150 s.'", any(s.endswith(". sleep 150 s.") for s in steps)
                  and not any("sleep 230" in s for s in steps), "")
        # --eboot records the deployed file; a run dir outside --download-root is warned about
        fake = Path(td) / "fake-eboot.bin"
        fake.write_bytes(b"SCE\0bench" * 7)
        run2 = Path(td) / "run2"
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rcode = b.main(["prepare", "--out", str(run2), "--nonce", "0badc0de0bad", "--eboot", str(fake),
                            "--download-root", str(Path(td) / "elsewhere")])
        check("prepare --eboot rc", rcode == 0, rcode)
        man2 = json.loads((run2 / "manifest.json").read_text(encoding="utf-8"))
        check("manifest eboot", man2.get("eboot") == {"path": str(fake.resolve()), "bytes": 63,
                                                      "sha256": b.sha256(fake)}, man2.get("eboot"))
        proc2 = (run2 / "procedure.txt").read_text(encoding="utf-8")
        check("procedure deploys the recorded eboot",
              f"vita_deploy_eboot {b.TITLE} {fake.resolve()} launch=false (63 B, sha256 {b.sha256(fake)[:16]}"
              in proc2 and "<bench eboot>" not in proc2, "")
        check("out-of-root warning", "WARN" in out.getvalue() and "(--download-root)" in out.getvalue(), "")
        check("no --download-root: no root warning", "--download-root" not in out0.getvalue(), out0.getvalue()[:300])
        check("no warning text in procedure", "WARN" not in proc2, "")
        check("--eboot: no 'no --eboot' WARN", "WARN no --eboot" not in out.getvalue(), "")
        # --installed-sha256 / --installed-bytes (no default): named in the download and restore steps
        # when given; without them the restore step checks the downloaded file's size
        check("no --installed-*: no installed_eboot, restore against before/eboot.bin",
              "installed_eboot" not in man and f"-> the size of {run.resolve() / 'before' / 'eboot.bin'}." in proc
              and "should be" not in proc, "")
        inst = "ab" * 32
        run3 = Path(td) / "run3"
        with contextlib.redirect_stdout(io.StringIO()):
            rcode = b.main(["prepare", "--out", str(run3), "--nonce", "0badc0de0bae", "--installed-sha256", inst,
                            "--installed-bytes", "1234", "--download-root", td])
        man3 = json.loads((run3 / "manifest.json").read_text(encoding="utf-8"))
        proc3 = (run3 / "procedure.txt").read_text(encoding="utf-8")
        check("prepare --installed-* rc and manifest", rcode == 0 and man3.get("installed_eboot")
              == {"sha256": inst, "bytes": 1234}, man3.get("installed_eboot"))
        check("procedure names the installed eboot's sha256 and size",
              f"on the host its sha256 should be {inst[:16]}... (prepare --installed-sha256)" in proc3
              and "-> size 1234 (prepare --installed-bytes)." in proc3, "")
        # input checks
        bad = Path(td) / "bad"
        bad.mkdir()
        for f in ("bench-save.txt", "main.lua", "launch-args.txt"):
            shutil.copy(HERE / f, bad / f)
        (bad / "launch-args.txt").write_text("--set-stage=1 --set-stage-type=4\n", encoding="ascii")
        try:
            b.check_inputs(bad)
            check("launch-args without slot rejected", False, "accepted")
        except AssertionError:
            pass
        shutil.copy(HERE / "launch-args.txt", bad / "launch-args.txt")
        (bad / "bench-save.txt").write_text((HERE / "bench-save.txt").read_text(encoding="utf-8")
                                            .replace("@remove\n", ""), encoding="utf-8")
        try:
            b.check_inputs(bad)
            check("bench-save without @remove rejected", False, "accepted")
        except AssertionError:
            pass
    print("prepare: files, nonce, manifest, procedure, input checks")


def entry(name, size, modify):
    return {"name": name, "path": f"x/{name}", "type": "file", "size": size, "modify": modify}


def leg_listing() -> None:
    before = {"gamestate1.dat": entry("gamestate1.dat", 10, "1"), "gamestate2.dat": entry("gamestate2.dat", 20, "1"),
              "log.txt": entry("log.txt", 5, "1"), "options.ini": entry("options.ini", 6, "1")}
    after = dict(before)
    after.update({"gamestate2.dat": entry("gamestate2.dat", 21, "2"), "log.txt": entry("log.txt", 9, "2"),
                  "20261025.persistentgamedata2.dat": entry("20261025.persistentgamedata2.dat", 7, "2"),
                  "options.ini": entry("options.ini", 6, "2")})
    acts = b.listing_actions(before, after, b.SAVE_DIR)

    def has(kind, remote):
        return any(k == kind and (w == remote or w.startswith(remote + " ")) for k, w in acts)
    check("listing restore File 2", has("restore", b.SAVE_DIR + "/gamestate2.dat"), acts)
    check("listing delete new", has("delete", b.SAVE_DIR + "/20261025.persistentgamedata2.dat"), acts)
    check("listing CHECK no-backup", has("CHECK", b.SAVE_DIR + "/options.ini"), acts)
    check("listing nothing else", len(acts) == 3, acts)
    check("listing ignores log.txt", not any("log.txt" in w for _, w in acts), acts)
    check("listing quiet on File 1", not any(k == "STOP" for k, _ in acts), acts)
    after["gamestate1.dat"] = entry("gamestate1.dat", 11, "2")
    eid_b = {"save1.dat": entry("save1.dat", 3, "1")}
    eid_a = {"save1.dat": entry("save1.dat", 3, "2")}
    stops = [k for k, _ in b.listing_actions(before, after, b.SAVE_DIR) + b.listing_actions(eid_b, eid_a, b.EID_DATA)
             if k == "STOP"]
    check("listing STOP on File 1 (save dir and EID save1.dat)", len(stops) == 2, stops)
    with tempfile.TemporaryDirectory() as td:
        bd, ad = Path(td) / "before", Path(td) / "after"
        bd.mkdir(), ad.mkdir()
        for name in b.LISTINGS:
            data = list(before.values()) if name == "list-save.json" else []
            (bd / name).write_text(json.dumps(data), encoding="utf-8")
            data = {"entries": list(after.values())} if name == "list-save.json" else []
            (ad / name).write_text(json.dumps(data), encoding="utf-8")
        with contextlib.redirect_stdout(io.StringIO()):
            rc_listing = b.main(["listing", "--before", str(bd), "--after", str(ad)])
        check("listing cmd rc STOP", rc_listing == 2, rc_listing)
        for name in b.BACKUPS:
            (bd / name).write_bytes(b"x" + name.encode())
            (ad / name).write_bytes(b"x" + name.encode())
        with contextlib.redirect_stdout(io.StringIO()):
            rc_same = b.main(["verify", "--before", str(bd), "--after", str(ad)])
            (ad / "gamestate2.dat").write_bytes(b"changed")
            rc_changed = b.main(["verify", "--before", str(bd), "--after", str(ad)])
        check("verify all MATCH", rc_same == 0, rc_same)
        check("verify CHANGED", rc_changed == 1, rc_changed)
    print("listing: actions and verify")


# Image preload (+ pin): the receipts of host_vita_image_retain.c in the shapes its printf formats produce
# (the module strings: 'armed' irp_arm, 'present' irpp_present, 'image' irp_one, PIN 'level'
# irpp_level_end, 'level' isaac_vita_image_preload_level_init, 'hit'/'miss' irp_offer,
# 'totals' irp_report/irpp_report, 'lost' irpp_settle).
IRP_KEY = "resources/gfx/BulletAtlas.png"
IRP_LINES = [
    "KAGE VITA IMAGE PRELOAD: armed n=1 truncated=0 max_kb=2048 reserve_mb=4 heap_factor=2 "
    f"list=gfx/BulletAtlas.png build={BID}",
    f"KAGE VITA IMAGE PRELOAD PIN: armed mode=on share=1/2 set_max=4 lines_max=64 build={BID}",
    "KAGE VITA IMAGE PRELOAD PIN: present name=gfx/BulletAtlas.png level=1 k=1/1 image=9a000040 strong=1 "
    "flags=00000010 bytes=2097152 rec=0 own=0 key=resources/mods/x/gfx/BulletAtlas.png",
    "KAGE VITA IMAGE PRELOAD: image name=gfx/BulletAtlas.png result=retained image=9a100000 bytes=2097152 "
    f"dim=1024x512x4 flags=00000000 us=48211 hr_kb=11807..9705 live=12/30000000 key={IRP_KEY}",
    "KAGE VITA IMAGE PRELOAD PIN: level=1 mode=on pins=1 pinned_bytes=2097152 share=16777216 released=0 "
    "pinned=1 own=0 exact=0 alias=1 held=0",
    "KAGE VITA IMAGE PRELOAD: level=1 n=1 loaded=1 retained=1 dropped=0 present=1 fail=0 skipped=0 "
    "bytes=2097152 us=48530 hr_kb=11807..9705 need_kb=8192 fault=0",
    "KAGE VITA IMAGE PRELOAD: totals levels=1 loaded=1 retained=1 dropped=0 present=1 fail=0 hit=0 miss=0 "
    "evicted_unused=0 skip(s,l,t,v,h,k)=0,0,0,0,0,0 breach=0 bytes=2097152 us=48530 why=periodic",
    "KAGE VITA IMAGE PRELOAD PIN: totals mode=on pinned=1 refused=0 used=0 floor=0 yield=0 stale=0 "
    "dropped=0 skip=2 cskip=0 own=0 exact=0 alias=1 held=0 on_free=0 lost=0 twins=0 pins=1 "
    "pinned_bytes=2097152 why=periodic",
    "KAGE VITA IMAGE PRELOAD: hit name=gfx/BulletAtlas.png image=9a100000 hit=1 miss=0",
    "KAGE VITA IMAGE PRELOAD PIN: miss name=gfx/BulletAtlas.png level=1 image=9a200000 flags=00000010 "
    f"state=2 pinned=0 twins=2 key={IRP_KEY}",
    "KAGE VITA IMAGE PRELOAD: miss name=gfx/BulletAtlas.png image=9a200000 state=2 flags=00000010 hit=1 miss=1",
    "KAGE VITA IMAGE PRELOAD: level=2 n=1 loaded=0 retained=0 dropped=0 present=0 fail=0 skipped=1 "
    "bytes=0 us=3 hr_kb=7484..7484 need_kb=8192 fault=0",
    "KAGE VITA IMAGE PRELOAD PIN: level=2 mode=on pins=0 pinned_bytes=0 share=16777216 released=0 pinned=0 "
    "own=0 exact=0 alias=0 held=0",
    "KAGE VITA IMAGE PRELOAD PIN: lost name=gfx/BulletAtlas.png level=2 image=9a200000 flags=00000010 "
    "lost=1 exit=ineligible",
    "KAGE VITA IMAGE PRELOAD: totals levels=2 loaded=1 retained=1 dropped=0 present=1 fail=0 hit=1 miss=1 "
    "evicted_unused=0 skip(s,l,t,v,h,k)=0,0,0,0,1,0 breach=0 bytes=2097152 us=48533 why=loading-complete",
    "KAGE VITA IMAGE PRELOAD PIN: totals mode=on pinned=1 refused=0 used=1 floor=0 yield=0 stale=0 "
    "dropped=0 skip=2 cskip=1 own=0 exact=0 alias=1 held=0 on_free=0 lost=1 twins=1 pins=0 pinned_bytes=0 "
    "why=loading-complete",
]


def irp_native(lines: list[str], tail: list[str] = ()) -> str:
    nat = good_native()
    stamped = [f"[{40.0 + k:.3f} thr 0x40010003] {x}" for k, x in enumerate(lines)]
    return "\n".join(nat[:3] + stamped + nat[3:] + list(tail)) + "\n"


def leg_image_preload() -> None:
    nonce = "0123456789ab"
    game = "\n".join(good_game(nonce)) + "\n"
    plain = b.diagnose("\n".join(good_native()) + "\n", game, nonce)
    check("image preload: none in the log -> None, no report block",
          plain.get("image_preload") is None and "image preload (" not in b.report(plain),
          str(plain.get("image_preload")))
    res = b.diagnose(irp_native(IRP_LINES), game, nonce)
    irp = res.get("image_preload") or {}
    check("image preload: run still ok", res["ok"], res["verdict"])
    check("image preload: armed + pin armed",
          (irp.get("armed") or {}).get("reserve_mb") == "4" and (irp.get("pin_armed") or {}).get("mode") == "on",
          f"{irp.get('armed')} {irp.get('pin_armed')}")
    check("image preload: two levels, two pin levels, keyed by level",
          [x.get("level") for x in irp.get("levels", [])] == ["1", "2"]
          and [x.get("level") for x in irp.get("pin_levels", [])] == ["1", "2"]
          and irp["levels"][0].get("hr_kb") == "11807..9705" and irp["levels"][1].get("skipped") == "1"
          and irp["pin_levels"][0].get("alias") == "1", str(irp.get("levels")))
    check("image preload: the decode line",
          len(irp.get("images", [])) == 1 and irp["images"][0].get("result") == "retained"
          and irp["images"][0].get("key") == IRP_KEY and irp["images"][0].get("dim") == "1024x512x4",
          str(irp.get("images")))
    check("image preload: totals = the last report (cumulative), skip tuple kept",
          (irp.get("totals") or {}).get("why") == "loading-complete" and irp["totals"].get("hit") == "1"
          and irp["totals"].get("skip") == "0,0,0,0,1,0"
          and (irp.get("pin_totals") or {}).get("used") == "1" and irp["pin_totals"].get("twins") == "1"
          and irp["pin_totals"].get("cskip") == "1" and irp["pin_totals"].get("lost") == "1",
          f"{irp.get('totals')} {irp.get('pin_totals')}")
    check("image preload: hit / miss lines counted, pin present+miss+lost lines kept raw",
          irp.get("hit_lines") == 1 and irp.get("miss_lines") == 1 and irp.get("pin_lines_n") == 3
          and irp.get("pin_lines", [""])[0].startswith("present name=gfx/BulletAtlas.png level=1 k=1/1")
          and irp["pin_lines"][1].startswith("miss name=gfx/BulletAtlas.png level=1")
          and irp["pin_lines"][2] == "lost name=gfx/BulletAtlas.png level=2 image=9a200000 flags=00000010 "
          "lost=1 exit=ineligible", str(irp.get("pin_lines")))
    text = b.report(res)
    check("image preload: report block",
          "image preload (KAGE VITA IMAGE PRELOAD; list=gfx/BulletAtlas.png reserve_mb=4" in text
          and "pin on;" in text
          and "  level   1: loaded=1 retained=1 dropped=0 present=1 fail=0 skipped=0" in text
          and "| pin pins=1 pinned_bytes=2097152 share=16777216 released=0 pinned=1 own=0 exact=0 alias=1" in text
          and "  pin totals: mode=on pinned=1 refused=0 used=1 floor=0 yield=0 stale=0 dropped=0 skip=2 cskip=1 "
          "own=0" in text
          and f"  decode: name=gfx/BulletAtlas.png result=retained bytes=2097152 dim=1024x512x4" in text
          and "(hit lines 1, miss lines 1, evicted-unused lines 0)" in text, text[:1500])
    # A second boot of another build appended to the same log: its receipts are not this run's.
    other = "[0.306 thr 0x40010003] build rel:another-build-0000000000000000; guest base=0x98000000; softfp ARMv7"
    mixed = b.diagnose(irp_native(IRP_LINES, [other, IRP_LINES[0], IRP_LINES[5]]), game, nonce)
    m = mixed.get("image_preload") or {}
    check("image preload: lines after another build's banner are ignored",
          len(m.get("levels", [])) == 2 and m.get("images") and len(m["images"]) == 1, str(m.get("levels")))
    # PRELOAD without PIN: no pin block.
    nopin = [x for x in IRP_LINES if "PRELOAD PIN:" not in x]
    np_res = b.diagnose(irp_native(nopin), game, nonce)
    np_text = b.report(np_res)
    check("image preload: no PIN receipts -> 'pin not built', no pin totals line",
          (np_res.get("image_preload") or {}).get("pin_totals") is None and "pin not built;" in np_text
          and "pin totals:" not in np_text, np_text[:800])
    # More present/miss/lost lines than the report keeps: counted, the first ones kept.
    flood = IRP_LINES + [IRP_LINES[9]] * (b.IRP_PIN_LINES_KEEP + 5)
    fl = b.diagnose(irp_native(flood), game, nonce).get("image_preload") or {}
    check("image preload: pin line cap",
          len(fl.get("pin_lines", [])) == b.IRP_PIN_LINES_KEEP and fl.get("pin_lines_n") == b.IRP_PIN_LINES_KEEP + 8,
          f"{len(fl.get('pin_lines', []))} {fl.get('pin_lines_n')}")
    print("bench image preload (+ pin) receipts: parsed and reported")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    env = os.environ.get("BENCH_SMOKE_ROOT")
    ap.add_argument("--smoke-root", type=Path, default=Path(env) if env else None,
                    help="downloaded device run directories (default: $BENCH_SMOKE_ROOT); without it the "
                         "device legs SKIP")
    a = ap.parse_args(argv)
    FAILS.clear()
    leg_lua()
    leg_smoke(a.smoke_root)
    leg_phases()
    leg_admit_wait()
    leg_image_preload()
    leg_admit_behind()
    leg_draw_packet()
    leg_census()
    leg_floor_debt()
    leg_floor_debt_stuck()
    leg_floor_debt_gates()
    leg_floor_debt_edges()
    leg_run_gates()
    leg_run_gates_device(a.smoke_root)
    leg_negative()
    leg_validity()
    leg_lua_health(a.smoke_root)
    leg_seed(a.smoke_root)
    leg_residual()
    leg_residual_review()
    leg_residual_r2(a.smoke_root)
    leg_windows()
    leg_dry()
    leg_prepare()
    leg_listing()
    if FAILS:
        print(f"bench_run_test: FAIL ({len(FAILS)})")
        return 1
    print("bench_run_test: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
