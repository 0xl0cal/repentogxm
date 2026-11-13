#!/usr/bin/env python3
"""Host gates for ISAAC_VITA_UPDATE_HITCH_PROBE (ph120.uh / ph120.uhs).

  python recomp/vita/test_kage_vita_update_hitch.py [--base REV] [--no-mutants]

Everything is compiled from the sources on disk on every run; nothing here
reads a previously generated artefact.

Legs:
  record     production kage_vita_phase_profile.c + kage_vita_update_hitch*.h,
             every seam stubbed ("full") and none ("bare"): threshold boundary
             (29999 silent, 30000 and 30001 reported), per-update deltas,
             off/na markers, sampler ring wrap, one newline per record, banner
  scheduler  production kage_vita_fullspeed_scheduler.c with the probe ON
             (begin/end call sites carry the scheduler's own clock values and
             frame numbers) and OFF; the two "sched:" lines must be equal:
             same clock-read count, waits and scheduler snapshot.  Not
             executed here: the scheduler's early returns (runtime disabled,
             failed pace wait), where no begin is made and the end is dropped
  lua-cost   production host_vita_lua_cost.c: lifetime totals survive the
             unit's own window roll-over; its own records are unchanged
  sampler    production kage_vita_guest_sampler.c through its existing oracle:
             stdout with the probe ON equals stdout OFF, and the trail holds
             exactly the samples the oracle's ph120.hs records count, with
             the self keys of its ph120.hot lists and the caller keys of its
             ph120.hotc lists (as multisets)
  sfx-peek   production host_vita_static_sfx_profile.c through the existing
             vita_audio_import_oracle.c: the peek is repeatable, resets
             nothing, leaves the skipped count to take_window, and is refused
             while a scope holds the lock; the oracle's other output is what
             it is without the probe
  off        each touched unit preprocessed WITHOUT the define contains no
             probe identifier; with --base REV also: its preprocessed text is
             identical to REV's (the host proxy of the box's object compare)
  cmake      option default OFF, the requirement FATALs, the define scope
  parser     tools/bench/update_hitch.py on the records the full record leg
             just printed; malformed lines raise; sampler markers re-read
             from kage_vita_guest_sampler.c
  mutants    each listed one-line change to a production file must make its
             leg fail at run time (not at compile time)
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

VITA = Path(__file__).resolve().parent
RECOMP = VITA.parent
ROOT = RECOMP.parent
RUNTIME = RECOMP / "runtime"
HOST_TESTS = VITA / "host_tests"

DEFINE = "ISAAC_VITA_UPDATE_HITCH_PROBE"
PROBE_IDENTIFIERS = (
    "kage_vita_update_hitch", "KageVitaUpdateHitch", "kvuh_", "s_kvuh",
    "isaac_vita_lua_cost_hitch_peek", "s_cost_hitch_base", "cost_hitch_window",
    "isaac_vita_static_sfx_profile_peek_window", "s_hitch_trail",
    "kage_vita_guest_sampler_trail",
)
SAMPLER_FLAGS = [
    "-DGUEST_IMAGE_BASE=0x98000000u",
    "-DISAAC_KAGE_VITA_GUEST_SAMPLER_ORACLE=1",
    "-DISAAC_VITA_GUEST_SAMPLER=1",
    "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
    "-DISAAC_VITA_PROFILE_IMPORT_KINDS=1",
    '-DISAAC_VITA_FULLSPEED_SCHEDULER_BUILD_ID="sampler:oracle"',
]
# Touched translation units and the defines each needs to preprocess on a
# host (their own oracle modes; the probe define is deliberately absent).
TOUCHED_UNITS = {
    "kage_vita_phase_profile.c": [
        "-DISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE=1", "-DISAAC_VITA_PHASE_PROFILE=1",
        "-DISAAC_VITA_STATIC_SFX_PROFILE=1", "-DISAAC_VITA_GUEST_SAMPLER=1",
        "-DISAAC_VITA_CRT_FILE_LOOKUP_HINT=1", "-DISAAC_VITA_FRAME_SPIKE_PROFILE=1"],
    "kage_vita_fullspeed_scheduler.c": [
        "-DISAAC_VITA_FULLSPEED_SCHEDULER=1", "-DISAAC_VITA_GUEST_SAMPLER=1"],
    "host_vita_lua_cost.c": [
        "-DISAAC_VITA_LUA_COST_PROFILE=1", "-DISAAC_VITA_LUA_COST_PROFILE_ORACLE=1"],
    "host_vita_static_sfx_profile.c": ["-DISAAC_VITA_STATIC_SFX_PROFILE=1"],
    "kage_vita_guest_sampler.c": SAMPLER_FLAGS,
}
TOUCHED_HEADERS = ("host_vita_lua_cost.h", "host_vita_static_sfx_profile.h")
NEW_HEADERS = ("kage_vita_update_hitch.h", "kage_vita_update_hitch_record.h")
# Host-only oracle sources with probe-only cases (the sfx-peek leg).
ORACLE_USERS = ("vita_audio_import_oracle.c",)


def compiler() -> str:
    for name in ("cc", "gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    candidates = (
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
        "LLVM" / "bin" / "clang.exe",
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
        "Microsoft Visual Studio" / "2022" / "Community" / "VC" /
        "Tools" / "Llvm" / "x64" / "bin" / "clang.exe",
    )
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise RuntimeError("no host C compiler found")


class Fail(AssertionError):
    pass


def run(command, check=True):
    done = subprocess.run([str(x) for x in command], text=True, capture_output=True)
    if check and done.returncode != 0:
        raise Fail(f"{Path(str(command[0])).name} failed ({done.returncode}):\n"
                   f"{' '.join(str(x) for x in command)}\n{done.stdout}\n{done.stderr}")
    return done


class Builder:
    """Compiles the legs.  `shadow` is a directory holding replacement copies
    of production files (a mutant); it is searched before recomp/runtime, and
    a production .c listed in a leg is taken from it when present."""

    def __init__(self, cc: str, out: Path, shadow: Path | None = None):
        self.cc, self.out, self.shadow = cc, out, shadow
        self.common = [cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror"]
        self.include = ([f"-I{shadow}"] if shadow else []) + [f"-I{RUNTIME}", f"-I{VITA}"]

    def source(self, name: str) -> Path:
        if self.shadow and (self.shadow / name).is_file():
            return self.shadow / name
        return RUNTIME / name

    def exe(self, name: str) -> Path:
        return self.out / (name + (".exe" if os.name == "nt" else ""))

    def build(self, name: str, flags, sources) -> Path:
        exe = self.exe(name)
        run(self.common + list(flags) + self.include + list(sources) + ["-o", exe])
        return exe

    # ---- legs: each returns its evidence line(s) or raises Fail ----------
    def record(self, leg: str) -> str:
        flags = ["-DKVUH_TEST_FULL=1"] if leg == "full" else []
        exe = self.build(f"record-{leg}", flags,
                         [HOST_TESTS / "kage_vita_update_hitch_record_test.c"])
        out = run([exe]).stdout.strip()
        if f"update hitch record: PASS leg={leg} " not in out:
            raise Fail(f"record {leg}: {out}")
        return out

    def record_dump(self) -> str:
        return run([self.exe("record-full"), "dump"]).stdout

    def scheduler(self) -> str:
        lines = {}
        for mode, flags in (("off", []), ("on", [f"-D{DEFINE}=1"])):
            exe = self.build(f"sched-{mode}", ["-DISAAC_VITA_FULLSPEED_SCHEDULER=1"] + flags,
                             [self.source("kage_vita_fullspeed_scheduler.c"),
                              HOST_TESTS / "kage_vita_update_hitch_scheduler_test.c"])
            out = run([exe]).stdout
            sched = [x for x in out.splitlines() if x.startswith("sched: ")]
            probe = [x for x in out.splitlines() if x.startswith("probe: ")]
            if len(sched) != 1 or len(probe) != (mode == "on"):
                raise Fail(f"scheduler {mode}: unexpected output\n{out}")
            lines[mode] = (sched[0], probe[0] if probe else "")
        if lines["on"][0] != lines["off"][0]:
            raise Fail("scheduler: probe ON changed the scheduler\n"
                       f" off {lines['off'][0]}\n on  {lines['on'][0]}")
        return f"{lines['on'][1]} | ON == OFF: {lines['on'][0]}"

    def lua_cost(self) -> str:
        exe = self.build("lua-cost", [
            "-DISAAC_VITA_LUA_COST_PROFILE=1", "-DISAAC_VITA_LUA_COST_PROFILE_ORACLE=1",
            f"-D{DEFINE}=1"],
            [self.source("host_vita_lua_cost.c"),
             HOST_TESTS / "kage_vita_update_hitch_lua_cost_test.c"])
        out = run([exe]).stdout.strip()
        if "update hitch lua cost: PASS" not in out:
            raise Fail(f"lua-cost: {out}")
        return out

    def sampler(self) -> str:
        units = [self.source("kage_vita_guest_sampler.c"),
                 self.source("kage_vita_fullspeed_scheduler.c"),
                 RUNTIME / "kage_vita_guest_sampler_oracle.c"]
        off = self.build("sampler-off", ["-O2"] + SAMPLER_FLAGS, units)
        on = self.build("sampler-on", ["-O2"] + SAMPLER_FLAGS + [
            f"-D{DEFINE}=1", "-Dmain=kvuh_sampler_oracle_main"],
            units + [HOST_TESTS / "kage_vita_update_hitch_sampler_test.c"])
        off_out = run([off]).stdout
        on_out = run([on]).stdout
        trail = [x for x in on_out.splitlines() if x.startswith("trail: ")]
        rest = "".join(x + "\n" for x in on_out.splitlines()
                       if not x.startswith(("trail: ", "trail-pair: ")))
        if len(trail) != 1:
            raise Fail(f"sampler: no single trail line\n{on_out}")
        if rest != "".join(x + "\n" for x in off_out.splitlines()):
            raise Fail("sampler: the oracle's own records differ with the probe ON")
        totals = [int(x) for x in re.findall(r"ph120\.hs .* total=(\d+) ", off_out)]
        if not totals or sum(totals) == 0:
            raise Fail("sampler: the oracle printed no ph120.hs totals")
        m = re.fullmatch(r"trail: seq=(\d+) copied=(\d+) lost=(\d+) nonzero=(\d+)", trail[0])
        if not m:
            raise Fail(f"sampler: {trail[0]}")
        seq, copied, lost, nonzero = (int(x) for x in m.groups())
        if seq != sum(totals):
            raise Fail(f"sampler: trail seq {seq} != sum of ph120.hs totals {sum(totals)}")
        if copied + lost != seq or copied == 0 or nonzero != copied or lost != 0:
            raise Fail(f"sampler: {trail[0]}")
        # Content: the trail's self keys are what ph120.hot counted and its
        # caller keys what ph120.hotc counted.  The marker spellings are
        # read from the sampler source, not recalled.
        source = self.source("kage_vita_guest_sampler.c").read_text(encoding="utf-8")
        marker = {}
        for name, label in (("UNKNOWN", "ind"), ("EXTERNAL", "ext")):
            m = re.search(rf"#define KVGS_RVA_{name}\s+UINT32_C\((0x[0-9a-f]+)\)", source)
            if not m:
                raise Fail(f"sampler: KVGS_RVA_{name} not found")
            marker[label] = int(m.group(1), 16)

        def listed(record: str) -> dict:
            got: dict = {}
            rows = re.findall(rf"ph120\.{record} bid=\S+ win=\d+ loops=\d+ phase=\w+ "
                              r"samples=(\d+) top=(\S+)", off_out)
            for samples, top in rows:
                counts = [item.split(":") for item in top.split(",")]
                if sum(int(c) for _, c in counts) != int(samples):
                    raise Fail(f"sampler: a ph120.{record} top list does not cover its "
                               f"samples ({top} vs {samples}); the comparison would be partial")
                for key, count in counts:
                    value = marker[key] if key in marker else int(key, 16)
                    got[value] = got.get(value, 0) + int(count)
            return got

        pairs = [x.split()[1:] for x in on_out.splitlines() if x.startswith("trail-pair: ")]
        if len(pairs) != copied:
            raise Fail(f"sampler: {len(pairs)} trail-pair lines for copied={copied}")
        got_self: dict = {}
        got_caller: dict = {}
        for s, c in pairs:
            got_self[int(s, 16)] = got_self.get(int(s, 16), 0) + 1
            got_caller[int(c, 16)] = got_caller.get(int(c, 16), 0) + 1
        want_self, want_caller = listed("hot"), listed("hotc")
        if sum(want_self.values()) != seq or want_self == want_caller:
            raise Fail("sampler: the oracle's hot/hotc lists cannot tell self from caller")
        show = lambda d: ",".join(f"{k:x}:{v}" for k, v in sorted(d.items()))  # noqa: E731
        if got_self != want_self:
            raise Fail(f"sampler: trail self keys {show(got_self)} != ph120.hot {show(want_self)}")
        if got_caller != want_caller:
            raise Fail(f"sampler: trail caller keys {show(got_caller)} != ph120.hotc "
                       f"{show(want_caller)}")
        return (f"{trail[0]} == sum of {len(totals)} ph120.hs totals; self keys == ph120.hot "
                f"({len(want_self)} distinct), caller keys == ph120.hotc ({len(want_caller)} "
                "distinct); other stdout ON == OFF")

    def sfx_peek(self) -> str:
        # The flags of test_audio_imports.sh's static-SFX host oracle, less
        # -Werror (gcc 11 warns in guest.h there, unrelated) and the
        # linker's --gc-sections (not every host linker has it).
        flags = ["-std=gnu11", "-O2", "-fno-strict-aliasing", "-Wall", "-Wextra",
                 "-DISAAC_VITA_AUDIO_ORACLE=1", "-DISAAC_VITA_STATIC_SFX_PROFILE=1",
                 "-DGUEST_IMAGE_BASE=0x27000000u", f"-I{VITA / 'audio_oracle_include'}"]
        units = [RUNTIME / "host_vita_audio.c",
                 self.source("host_vita_static_sfx_profile.c"),
                 RUNTIME / "guest_stack_legacy_oracle_stub.c",
                 RUNTIME / "vita_audio_import_oracle.c"]
        outs = {}
        for mode, extra in (("off", []), ("on", [f"-D{DEFINE}=1"])):
            exe = self.exe(f"sfx-{mode}")
            run([self.cc] + flags + extra + self.include + units + ["-o", exe])
            done = run([exe], check=False)
            if done.returncode != 0:
                raise Fail(f"sfx-peek {mode}: oracle rc={done.returncode}\n"
                           f"{done.stdout[-600:]}\n{done.stderr[-600:]}")
            outs[mode] = done.stdout.splitlines()
        peek = [x for x in outs["on"] if x.startswith("Static SFX peek: PASS")]
        if len(peek) != 1 or any("Static SFX peek" in x for x in outs["off"]):
            raise Fail(f"sfx-peek: peek line on={len(peek)}, or present with the probe OFF")
        if [x for x in outs["on"] if x not in peek] != outs["off"]:
            raise Fail("sfx-peek: the oracle's own output differs with the probe ON")
        if not any(x.startswith("Static SFX scopes: PASS") for x in outs["off"]):
            raise Fail("sfx-peek: the static SFX oracle did not run")
        return peek[0]


def preprocess(cc: str, path: Path, flags, include) -> str:
    # -P leaves a blank line where a long skipped region was, and how many
    # depends on the region's length; blank lines are not tokens.
    text = run([cc, "-std=c11", "-E", "-P"] + list(flags) + list(include) + [path]).stdout
    return "".join(line + "\n" for line in text.splitlines() if line.strip())


def leg_off(cc: str, tmp: Path, base: str | None) -> list[str]:
    lines = []
    include = [f"-I{RUNTIME}", f"-I{VITA}"]
    base_dir = None
    if base:
        base_dir = tmp / "base"
        base_dir.mkdir()
        for name in list(TOUCHED_UNITS) + list(TOUCHED_HEADERS):
            blob = subprocess.run(["git", "-C", str(ROOT), "show",
                                   f"{base}:recomp/runtime/{name}"],
                                  capture_output=True, check=False)
            if blob.returncode != 0:
                raise Fail(f"off: git show {base}:recomp/runtime/{name} failed: "
                           f"{blob.stderr.decode(errors='replace')}")
            (base_dir / name).write_bytes(blob.stdout)
        for name in NEW_HEADERS:
            probe = subprocess.run(["git", "-C", str(ROOT), "cat-file", "-e",
                                    f"{base}:recomp/runtime/{name}"], capture_output=True)
            if probe.returncode == 0:
                raise Fail(f"off: {name} already exists at {base}; wrong base")
    for name, flags in TOUCHED_UNITS.items():
        text = preprocess(cc, RUNTIME / name, flags, include)
        if len(text) < 2000:
            raise Fail(f"off: {name} preprocessed to only {len(text)} bytes")
        hits = [x for x in PROBE_IDENTIFIERS if x in text]
        if hits:
            raise Fail(f"off: {name} without the define still contains {hits}")
        on = preprocess(cc, RUNTIME / name, list(flags) + [f"-D{DEFINE}=1"], include)
        if not any(x in on for x in PROBE_IDENTIFIERS):
            raise Fail(f"off: {name} with the define contains no probe identifier "
                       "(the check above would pass vacuously)")
        note = f"{name}: OFF {len(text)} bytes, no probe identifier; ON differs"
        if base_dir:
            old = preprocess(cc, base_dir / name, flags, [f"-I{base_dir}"] + include)
            if old != text:
                raise Fail(f"off: {name} preprocessed text differs from {base}")
            note += f"; == {base[:12]}"
        lines.append(note)
    return lines


def leg_cmake() -> str:
    text = (VITA / "CMakeLists.txt").read_text(encoding="utf-8")
    if len(re.findall(r"^option\(ISAAC_VITA_UPDATE_HITCH_PROBE\n\s+\"[^\"]+\" OFF\)$",
                      text, re.M)) != 1:
        raise Fail("cmake: the option is not declared exactly once with default OFF")
    if len(re.findall(r'^set\(ISAAC_VITA_UPDATE_HITCH_PROBE_THRESHOLD_US "30000" CACHE STRING$',
                      text, re.M)) != 1:
        raise Fail("cmake: threshold cache variable (default 30000) missing")
    header = (RUNTIME / "kage_vita_update_hitch.h").read_text(encoding="utf-8")
    if "# define ISAAC_VITA_UPDATE_HITCH_PROBE_THRESHOLD_US 30000\n" not in header:
        raise Fail("cmake: the header's fallback threshold is not CMake's default")
    need = (
        "if(NOT ISAAC_VITA_PHASE_PROFILE OR NOT ISAAC_VITA_FULLSPEED_SCHEDULER OR",
        'ISAAC_VITA_UPDATE_HITCH_PROBE_THRESHOLD_US MATCHES "^[1-9][0-9]*$"',
        "ISAAC_VITA_UPDATE_HITCH_PROBE_THRESHOLD_US LESS 1000",
        "ISAAC_VITA_UPDATE_HITCH_PROBE_THRESHOLD_US GREATER 10000000",
        "ISAAC_VITA_UPDATE_HITCH_PROBE_THRESHOLD_US=${ISAAC_VITA_UPDATE_HITCH_PROBE_THRESHOLD_US})",
        "APPEND PROPERTY COMPILE_DEFINITIONS KAGE_VITA_UPDATE_HITCH_LUA_COST=1)",
        'message(STATUS "Isaac Vita update hitch probe: ph120.uh/uhs per Game::Update >= '
        "${ISAAC_VITA_UPDATE_HITCH_PROBE_THRESHOLD_US} us",
    )
    for item in need:
        if text.count(item) != 1:
            raise Fail(f"cmake: expected exactly once: {item}")
    block = re.search(
        r"set_property\(SOURCE\n((?:\s+\"\$\{ISAAC_RUNTIME\}/[\w.]+\"\n)+)"
        r"\s+APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_UPDATE_HITCH_PROBE=1\)", text)
    if not block:
        raise Fail("cmake: the define's source list was not found")
    listed = sorted(re.findall(r"/([\w.]+)\"", block.group(1)))
    if listed != sorted(TOUCHED_UNITS):
        raise Fail(f"cmake: define goes to {listed}, the touched units are "
                   f"{sorted(TOUCHED_UNITS)}")
    # No other spelling may hand the define to anything (a directory- or
    # target-wide definition would break OFF identity of nothing but would
    # put the probe in units this test does not check).
    if text.count("ISAAC_VITA_UPDATE_HITCH_PROBE=1") != 1:
        raise Fail("cmake: the define is appended in more than one place")
    # Every #if in the tree that names the knob is in a touched file.
    users = sorted(p.name for p in RUNTIME.iterdir()
                   if p.suffix in (".c", ".h") and DEFINE in
                   p.read_text(encoding="utf-8", errors="replace"))
    # A host oracle may name the knob (its probe-only cases); it must not be
    # a unit of the Vita build, where the name would be a sixth compiled user.
    for name in ORACLE_USERS:
        if name in text:
            raise Fail(f"cmake: {name} is listed in the Vita CMakeLists.txt")
    allowed = sorted(set(TOUCHED_UNITS) | set(TOUCHED_HEADERS) | set(NEW_HEADERS)
                     | set(ORACLE_USERS))
    if users != allowed:
        raise Fail(f"cmake: runtime files naming the knob {users} != {allowed}")
    return f"option OFF, threshold 30000 == header fallback, define -> {', '.join(listed)}"


def leg_parser(dump: str, tmp: Path) -> str:
    sys.path.insert(0, str(ROOT / "tools" / "bench"))
    import update_hitch as uh  # noqa: E402
    parsed = uh.parse(dump.splitlines(keepends=True))
    events, banners = parsed["events"], parsed["banners"]
    if len(events) != 8 or len(banners) != 1:
        raise Fail(f"parser: {len(events)} events, {len(banners)} banners")
    if banners[0]["threshold_us"] != 30000 or banners[0]["seams"] != {
            "gl": "on", "fh": "on", "pngi": "on", "png": "off", "sfx": "on",
            "lua": "on", "smp": "on"}:
        raise Fail(f"parser: banner {banners[0]}")
    e = events[2]
    want = {"n": 3, "win": 1, "loop": 5, "us": 120000, "thr_us": 30000,
            "frame_before": 105, "frame_after": 106, "samples": 100, "lost": 0}
    got = {k: e[k] for k in want}
    if got != want:
        raise Fail(f"parser: event 3 {got}")
    if e["groups"]["gl"] != {"tex": 2, "draw": 7, "clear": 0, "fbo": 3} or \
            e["groups"]["fh"] != {"hit": 5, "miss": 14} or \
            e["groups"]["png"] != "off" or \
            e["groups"]["sfx"] != {"wav_c": 1, "wav_us": 5000, "ogg_c": 3, "ogg_us": 6100,
                                   "riff_c": 2, "riff_us": 4100, "up_c": 5, "up_us": 7300,
                                   "play_c": 4, "play_us": 2100} or \
            e["groups"]["lua"]["gc_us"] != 450 or \
            e["groups"]["pngi"]["bytes"] != 262144:
        raise Fail(f"parser: event 3 groups {e['groups']}")
    if e["self"] != [("5b1500", 60), ("5c2fd0", 30), ("2120", 5), ("9b40", 5)] or \
            e["caller"] != [("5a0e10", 90), ("5c30b0", 10)]:
        raise Fail(f"parser: event 3 tops {e['self']} {e['caller']}")
    if events[3]["groups"]["sfx"] != "na" or events[4]["groups"]["sfx"] != "na":
        raise Fail("parser: na group not preserved")
    if (events[5]["samples"], events[5]["lost"]) != (511, 89):
        raise Fail("parser: wrapped trail")
    if (events[6]["us"], events[6]["frame_before"]) != (40000, 7) or \
            events[7]["us"] != 4294967295 or events[7]["lost"] != 4294966784 or \
            events[7]["groups"]["lua"]["gc_us"] != 4294967295:
        raise Fail("parser: double-end / widest events")
    text = uh.render(parsed, None)
    if "events: 8 ph120.uh records" not in text or " 120.0 " not in text or \
            " 4/18400 " not in text or "WARNING" in text:
        raise Fail(f"parser: render\n{text}")
    # ph120.sp cross-check, on the production ph120.sp records of the same
    # run: us <= upd (the floor wait is in upd only), and a window whose
    # worst upd is over the threshold without a ph120.uh reads "outside".
    rows = [(r["win"], r["loop"], r["upd"], r["n"], r["us"], r["verdict"])
            for r in uh.cross_check(parsed, "hitch-test")]
    if rows != [(1, 5, 127000, 3, 120000, "uh"), (2, 132, 700000, 6, 700000, "uh"),
                (3, 250, 51000, None, None, "outside")]:
        raise Fail(f"parser: sp cross-check {rows}")
    if "   1     5      127.0     3   120.0         7.0  same loop" not in text or \
            "   3   250       51.0     -       -           -  NO ph120.uh: " not in text or \
            "INCONSISTENT" in text:
        raise Fail(f"parser: cross-check render\n{text}")
    # us > upd for the same loop cannot happen; the reader must say so.
    bent = [x.replace(" upd=127000 ", " upd=119999 ") for x in dump.splitlines(keepends=True)]
    if [r["verdict"] for r in uh.cross_check(uh.parse(bent), None)][0] != "INCONSISTENT":
        raise Fail("parser: us > sp upd not flagged")
    # The same loop number in an earlier boot of the same build is another
    # update: a second banner in between separates them.
    banner_line = next(x for x in dump.splitlines(keepends=True) if "UPDATE HITCH PROBE" in x)
    sp_lines = [x for x in dump.splitlines(keepends=True) if "ph120.sp " in x]
    rest = [x for x in dump.splitlines(keepends=True) if "ph120.sp " not in x]
    rebooted = uh.cross_check(uh.parse(rest + [banner_line] + sp_lines), None)
    if [r["verdict"] for r in rebooted] != ["outside", "outside", "outside"]:
        raise Fail(f"parser: events of an earlier boot matched: {rebooted}")
    if "events: 0 ph120.uh records for bid other" not in uh.render(parsed, "other"):
        raise Fail("parser: --bid filter")
    # A truncated or merged record must raise, not vanish.
    lines = dump.splitlines(keepends=True)
    uh_line = next(x for x in lines if "ph120.uh " in x)
    for bad in (uh_line[:120] + "\n", uh_line.replace(" us=", " u=")):
        try:
            uh.parse([bad])
        except uh.HitchLogError:
            continue
        raise Fail(f"parser: accepted a malformed record: {bad[:80]}")
    try:
        uh.parse([x for x in lines if "ph120.uh " not in x])
    except uh.HitchLogError:
        pass
    else:
        raise Fail("parser: accepted ph120.uhs without its ph120.uh")
    # A ph120.uh that lost its ph120.uhs (smp(seq) printed a number, so the
    # build printed one): raises; --allow-unpaired counts it instead.
    third = next(x for x in lines if "ph120.uhs " in x and " n=3 " in x)
    holed = [x for x in lines if x is not third]
    try:
        uh.parse(holed)
    except uh.HitchLogError as error:
        if "n=3" not in str(error) or "no ph120.uhs" not in str(error):
            raise Fail(f"parser: unpaired message: {error}")
    else:
        raise Fail("parser: accepted a ph120.uh without its ph120.uhs")
    lenient = uh.parse(holed, allow_unpaired=True)
    if [u["n"] for u in lenient["unpaired"]] != [3] or \
            "WARNING: 1 ph120.uh record(s)" not in uh.render(lenient, None):
        raise Fail("parser: --allow-unpaired did not count the lost record")
    # smp(seq)=off (sampler not in the build) has no ph120.uhs by design.
    off = [x.replace("smp(seq)=100", "smp(seq)=off") for x in holed]
    if uh.parse(off)["unpaired"]:
        raise Fail("parser: smp(seq)=off event reported as unpaired")
    # Sampler markers: copied constants, re-read from the source.
    source = (RUNTIME / "kage_vita_guest_sampler.c").read_text(encoding="utf-8")
    for name, label in (("UNKNOWN", "ind"), ("EXTERNAL", "ext"), ("PRESENT", "present")):
        m = re.search(rf"#define KVGS_RVA_{name}\s+UINT32_C\((0x[0-9a-f]+)\)", source)
        if not m or uh.SAMPLER_MARKERS.get(int(m.group(1), 16)) != label:
            raise Fail(f"parser: KVGS_RVA_{name} drifted")
    if uh._top("ffffffe:3,5b1500:1") != [("ext", 3), ("5b1500", 1)]:
        raise Fail("parser: marker rendering")
    # The command line, on a file.
    log = tmp / "native.log"
    log.write_text(dump, encoding="utf-8", newline="")
    done = run([sys.executable, "-I", ROOT / "tools" / "bench" / "update_hitch.py", log,
                "--bid", "hitch-test"])
    if "events: 8 ph120.uh records for bid hitch-test" not in done.stdout:
        raise Fail(f"parser: cli\n{done.stdout}")
    holed_log = tmp / "holed.log"
    holed_log.write_text("".join(holed), encoding="utf-8", newline="")
    tool = [sys.executable, "-I", ROOT / "tools" / "bench" / "update_hitch.py", holed_log]
    strict, loose = run(tool, check=False), run(tool + ["--allow-unpaired"], check=False)
    if strict.returncode != 2 or loose.returncode != 0 or "WARNING: 1 " not in loose.stdout:
        raise Fail(f"parser: cli unpaired rc={strict.returncode}/{loose.returncode}")
    return ("8 events + banner + 3 ph120.sp upd rows parsed from the production records; "
            "3 malformed inputs and a lost ph120.uhs raise; sp cross-check uh/outside/"
            "INCONSISTENT/boot separation")


# (id, production file, exact old text, new text, leg that must fail)
MUTANTS = (
    ("M1-threshold-gt", "kage_vita_update_hitch.h",
     "end_us - begin_us >= threshold_us;", "end_us - begin_us > threshold_us;", "record"),
    ("M2-delta-swapped", "kage_vita_update_hitch_record.h",
     "(unsigned)(end.v[g][f] - s_kvuh_begin.v[g][f])",
     "(unsigned)(s_kvuh_begin.v[g][f] - end.v[g][f])", "record"),
    ("M3-no-begin-snapshot", "kage_vita_update_hitch_record.h",
     "    kvuh_snapshot(&s_kvuh_begin);\n", "", "record"),
    ("M4-top-tie-order", "kage_vita_update_hitch.h",
     "if (run > counts[at])", "if (run >= counts[at])", "record"),
    ("M5-trail-overwrite-ignored", "kage_vita_update_hitch.h",
     "overwritten = after - first - KAGE_VITA_UPDATE_HITCH_TRAIL + 1u;",
     "overwritten = 0u;", "record"),
    ("M6-na-ignores-begin", "kage_vita_update_hitch_record.h",
     "if (((s_kvuh_begin.valid & end.valid) >> g & 1u) == 0u) {",
     "if (((end.valid) >> g & 1u) == 0u) {", "record"),
    ("M7-na-ignores-end", "kage_vita_update_hitch_record.h",
     "if (((s_kvuh_begin.valid & end.valid) >> g & 1u) == 0u) {",
     "if (((s_kvuh_begin.valid) >> g & 1u) == 0u) {", "record"),
    ("M8-unarmed-end-reported", "kage_vita_update_hitch_record.h",
     "if (!armed || begin_us != s_kvuh_begin_us)",
     "if (armed && begin_us != s_kvuh_begin_us)", "record"),
    ("M8b-stale-begin-reported", "kage_vita_update_hitch_record.h",
     "if (!armed || begin_us != s_kvuh_begin_us)", "if (!armed)", "record"),
    ("M9-top-drops-last", "kage_vita_update_hitch.h",
     "            if (used < top)\n                ++used;\n", "", "record"),
    ("M10-line-buffer-short", "kage_vita_update_hitch_record.h",
     "    char line[768];\n", "    char line[640];\n", "record"),
    ("M11-armed-not-cleared", "kage_vita_update_hitch_record.h",
     "    s_kvuh_armed = 0u;\n", "", "record"),
    ("M12-sfx-ogg-is-upload", "kage_vita_update_hitch_record.h",
     "s->v[KVUH_G_SFX][2] = w.ogg.count;", "s->v[KVUH_G_SFX][2] = w.upload.count;", "record"),
    ("M13-sfx-ogg-us-is-max", "kage_vita_update_hitch_record.h",
     "s->v[KVUH_G_SFX][3] = w.ogg.sum_us;", "s->v[KVUH_G_SFX][3] = w.ogg.max_us;", "record"),
    ("M14-sfx-up-is-play", "kage_vita_update_hitch_record.h",
     "s->v[KVUH_G_SFX][6] = w.upload.count;", "s->v[KVUH_G_SFX][6] = w.play.count;", "record"),
    ("S1-end-begin-is-end", "kage_vita_fullspeed_scheduler.c",
     "kage_vita_update_hitch_end(s_update_begin_us, end_us,",
     "kage_vita_update_hitch_end(end_us, end_us,", "scheduler"),
    ("S2-begin-not-called", "kage_vita_fullspeed_scheduler.c",
     "    kage_vita_update_hitch_begin(now_us);\n", "", "scheduler"),
    ("S3-frame-before-is-after", "kage_vita_fullspeed_scheduler.c",
     "                                       s_game_frame_before, game_frame);",
     "                                       game_frame, game_frame);", "scheduler"),
    ("L1-base-not-accumulated", "host_vita_lua_cost.c",
     "            s_cost_hitch_base[i] += closing[i];", "            (void)closing;",
     "lua_cost"),
    ("L2-gc-kinds-dropped", "host_vita_lua_cost.c",
     "out[4] = s_cost.gc_n[0] + s_cost.gc_n[1] + s_cost.gc_n[2];",
     "out[4] = s_cost.gc_n[0] + s_cost.gc_n[1];", "lua_cost"),
    ("G1-trail-not-pushed", "kage_vita_guest_sampler.c",
     "        kage_vita_update_hitch_trail_push(\n"
     "            &s_hitch_trail, callee, kvgs_function_containing(value));\n", "",
     "sampler"),
    ("G2-trail-self-caller-swapped", "kage_vita_guest_sampler.c",
     "            &s_hitch_trail, callee, kvgs_function_containing(value));\n",
     "            &s_hitch_trail, kvgs_function_containing(value), callee);\n", "sampler"),
    ("G3-trail-raw-return-address", "kage_vita_guest_sampler.c",
     "            &s_hitch_trail, callee, kvgs_function_containing(value));\n",
     "            &s_hitch_trail, value, value);\n", "sampler"),
    ("P1-peek-resets-window", "host_vita_static_sfx_profile.c",
     "    *out = s_window;\n    atomic_flag_clear_explicit(",
     "    *out = s_window;\n    memset(&s_window, 0, sizeof s_window);\n"
     "    atomic_flag_clear_explicit(", "sfx_peek"),
    ("P2-peek-takes-skipped", "host_vita_static_sfx_profile.c",
     "    *out = s_window;\n    atomic_flag_clear_explicit(",
     "    *out = s_window;\n    out->skipped = atomic_exchange_explicit(\n"
     "        &s_skipped, 0U, memory_order_relaxed);\n    atomic_flag_clear_explicit(",
     "sfx_peek"),
    ("P3-peek-ignores-scope-lock", "host_vita_static_sfx_profile.c",
     "    if (out == NULL || atomic_flag_test_and_set_explicit(\n"
     "            &s_scope_lock, memory_order_acquire))\n        return 0;\n"
     "    *out = s_window;\n    atomic_flag_clear_explicit(",
     "    if (out == NULL)\n        return 0;\n"
     "    *out = s_window;\n    if (0) atomic_flag_clear_explicit(", "sfx_peek"),
)
# Files a shadow directory must hold so that a mutated header is the one
# found: an #include "x.h" looks next to the including file first.
SHADOW_SET = ("kage_vita_phase_profile.c", "kage_vita_update_hitch.h",
              "kage_vita_update_hitch_record.h")


def leg_mutants(cc: str, tmp: Path) -> list[str]:
    lines = []
    for ident, name, old, new, leg in MUTANTS:
        shadow = tmp / f"shadow-{ident}"
        shadow.mkdir()
        for item in SHADOW_SET + (name,):
            shutil.copyfile(RUNTIME / item, shadow / item)
        text = (shadow / name).read_bytes().decode("utf-8")
        if text.count(old) != 1:
            raise Fail(f"mutant {ident}: anchor occurs {text.count(old)} times in {name}")
        (shadow / name).write_bytes(text.replace(old, new).encode("utf-8"))
        out = tmp / f"out-{ident}"
        out.mkdir()
        builder = Builder(cc, out, shadow)
        try:
            if leg == "record":
                builder.record("bare")
                builder.record("full")
            else:
                getattr(builder, leg)()
        except Fail as error:
            message = str(error)
            if " -o " in message and "error:" in message:
                raise Fail(f"mutant {ident} did not compile; not a kill:\n{message}")
            first = next((x for x in message.splitlines()
                          if x.strip() and not x.startswith(str(cc))), message)
            detail = [x for x in message.splitlines() if x.strip()]
            lines.append(f"{ident}: KILLED by {leg} ({detail[-1][:110] if detail else first})")
            continue
        raise Fail(f"mutant {ident} SURVIVED the {leg} leg")
    return lines


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", help="git revision to compare OFF preprocessed text with")
    parser.add_argument("--no-mutants", action="store_true")
    args = parser.parse_args(argv)
    cc = compiler()
    version = run([cc, "--version"]).stdout.splitlines()[0]
    print(f"compiler: {version}")
    with tempfile.TemporaryDirectory(prefix="isaac-update-hitch-") as value:
        tmp = Path(value)
        out = tmp / "real"
        out.mkdir()
        builder = Builder(cc, out)
        print("record bare:", builder.record("bare"))
        print("record full:", builder.record("full"))
        print("scheduler:", builder.scheduler())
        print("lua-cost:", builder.lua_cost())
        print("sampler:", builder.sampler())
        print("sfx-peek:", builder.sfx_peek())
        for line in leg_off(cc, tmp, args.base):
            print("off:", line)
        print("cmake:", leg_cmake())
        print("parser:", leg_parser(builder.record_dump(), tmp))
        if not args.no_mutants:
            killed = leg_mutants(cc, tmp)
            for line in killed:
                print("mutant", line)
            print(f"mutants: {len(killed)}/{len(MUTANTS)} killed")
    print("update hitch probe: PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as error:
        print(f"update hitch probe: FAIL\n{error}", file=sys.stderr)
        sys.exit(1)
