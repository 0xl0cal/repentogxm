#!/usr/bin/env python3
"""Host proofs of ISAAC_VITA_GL_DRAW_PACKET (RENDER60 lane C, ROAD60-SYNTHESIS.md row 1).

The knob makes the game thread stage the per-draw state records (glBlendFuncSeparate,
REC_ATTRIBS_ENABLE/_DISABLE, REC_KAGE_BIND_PACKET) and publish them with the next draw as
ONE ring record (REC_DRAW_PACKET), or alone in front of anything else (gl_record_backend.h).

1. OFF surface: for every touched C file, the preprocessed text (MSVC /EP) without the
   knob equals the base revision's under four define sets of the recorder (plain record,
   worker, worker + W-pkt + S4, the same + the opcode census).  A rename or a moved
   token in a shared path fails here, before any box build.
2. Scenario (host_tests/gl_record_scenario.c, MSVC x86, the real recorder against the
   vitaGL oracle on a jittered render thread): the layer-2 script, scenario 14 ("defer")
   and the lane's own script ("drawpkt"), poisoned and not, built with the knob OFF (the
   reference), VERIFY, ON, and ON + the opcode census.  Every native call is traced with
   the return RVA it ran with (SCN_TRACE_RVA); the traces must be identical byte for
   byte.  The drawpkt script states the lane's rule itself (which draws are fused); the
   recorder's window must agree, VERIFY must report 0 mismatches and 0 late records
   with every packet compared, and ON must stay within 2 ring records per draw on the
   draw path.
3. Mutants: each must fail the check it is aimed at (trace, VERIFY counters, or the
   fused count).

usage: python test_gl_draw_packet.py [--base REV] [--keep DIR] [--quick]
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tarfile
import io
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
RECOMP = HERE.parent
ROOT = RECOMP.parent
RUNTIME = RECOMP / "runtime"
SCENARIO_C = HERE / "host_tests" / "gl_record_scenario.c"
KNOB = "ISAAC_VITA_GL_DRAW_PACKET"
DEFAULT_BASE = "d874a40397640d856791c65f0bfc7ff18314a159"   # int-020-20261112 (IHEAD)
sys.path.insert(0, str(RECOMP))


def require(condition, message):
    if not condition:
        raise AssertionError(message)


# ---- 1. OFF surface ---------------------------------------------------------------
# The device C files the lane touches, and two device TUs that include the touched
# header.  (The phase-profile oracle and the scenario are host tests: CHECK carries
# __LINE__, so their token streams move with any edit and are not compared.)
OFF_UNITS = {
    "gl_record_backend.c": "recomp/runtime/gl_record_backend.c",
    "kage_vita_phase_profile.c": "recomp/runtime/kage_vita_phase_profile.c",
    "gl_bridge.c": "recomp/runtime/gl_bridge.c",
    "gl_vita_backend.c": "recomp/runtime/gl_vita_backend.c",
}
OFF_DEFINE_SETS = {
    "record": ["/DISAAC_VITA_GL_THREAD_RECORD=1"],
    "worker": ["/DISAAC_VITA_GL_THREAD_RECORD=1", "/DISAAC_VITA_GL_THREAD_WORKER=1"],
    "packed": ["/DISAAC_VITA_GL_THREAD_RECORD=1", "/DISAAC_VITA_GL_THREAD_WORKER=1",
               "/DISAAC_VITA_GL_RECORD_ATTRIB_BATCH=1", "/DISAAC_VITA_KAGE_BIND_PACKET=1",
               "/DISAAC_VITA_GL_SHIM_FASTDISPATCH=1", "/DISAAC_VITA_SHADER_ATTRIB_FASTPATH=1",
               "/DISAAC_VITA_PHASE_PROFILE=1", "/DISAAC_VITA_GFUEL_SEED_NATIVE=1",
               "/DISAAC_VITA_CAMERA_RING_NATIVE=1"],
    "census": ["/DISAAC_VITA_GL_THREAD_RECORD=1", "/DISAAC_VITA_GL_THREAD_WORKER=1",
               "/DISAAC_VITA_GL_RECORD_ATTRIB_BATCH=1", "/DISAAC_VITA_KAGE_BIND_PACKET=1",
               "/DISAAC_GL_RECORD_OPCODE_CENSUS=1", "/DISAAC_VITA_PHASE_PROFILE=1",
               "/DISAAC_VITA_GFUEL_SEED_NATIVE=1", "/DISAAC_VITA_CAMERA_RING_NATIVE=1"],
}


def tree_at(rev: str, dest: Path) -> Path:
    """recomp/ of `rev`, extracted (git archive), for its headers and units."""
    data = subprocess.run(["git", "-C", str(ROOT), "archive", "--format=tar", rev, "recomp/runtime",
                           "recomp/vita/host_tests"], stdout=subprocess.PIPE, check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        tar.extractall(dest)
    return dest / "recomp"


MISSING_HEADER = re.compile(r"C1083: [^:]*: ([A-Za-z0-9_./]+\.h): No such file")


def preprocess(env, compiler, recomp: Path, unit: str, defines: list[str], work: Path) -> str:
    """/EP of one unit.  vitasdk headers (psp2/..., vitaGL.h) are not on this host: each
    one /EP asks for becomes an empty stub, the same for base and head, so the
    comparison covers every token this repository contributes."""
    source = recomp.parent / OFF_UNITS[unit]
    stubs = work / "stub-include"
    for _ in range(64):
        run = subprocess.run([compiler, "/nologo", "/EP", "/D_CRT_SECURE_NO_WARNINGS",
                              "/DISAAC_GL_VITA_BACKEND_ORACLE=1", "/DISAAC_GL_RECORD_SCENARIO=1",
                              "/DGUEST_IMAGE_BASE=0x98000000u", *defines,
                              "/I", str(recomp / "runtime"), "/I", str(work / "obj-generated"),
                              "/I", str(stubs), str(source)], env=env, cwd=work,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        said = run.stderr.decode("cp866", errors="replace")
        missing = MISSING_HEADER.search(said) if run.returncode else None
        if not missing:
            break
        stub = stubs / missing.group(1)
        require(not stub.exists(), f"stub {stub} exists and is still missing")
        stub.parent.mkdir(parents=True, exist_ok=True)
        stub.write_bytes(b"")
    require(run.returncode == 0, f"/EP failed: {unit} {defines}\n" +
            run.stderr.decode("cp866", errors="replace")[-3000:])
    text = run.stdout.decode("latin-1")
    # whitespace-insensitive token stream (comments are gone under /EP)
    return " ".join(text.split())


def check_off_surface(env, compiler, work: Path, base: str) -> None:
    base_recomp = tree_at(base, work / "base")
    total = 0
    for unit in OFF_UNITS:
        for name, defines in OFF_DEFINE_SETS.items():
            head = preprocess(env, compiler, RECOMP, unit, defines, work)
            old = preprocess(env, compiler, base_recomp, unit, defines, work)
            if head != old:
                at = next(i for i, (a, b) in enumerate(zip(head, old)) if a != b) \
                    if any(a != b for a, b in zip(head, old)) else min(len(head), len(old))
                raise AssertionError(f"OFF surface: {unit} [{name}] differs from {base[:7]} at "
                                     f"char {at}:\n  head {head[max(0, at - 120):at + 120]}\n"
                                     f"  base {old[max(0, at - 120):at + 120]}")
            require("__LINE__" not in head, f"{unit}: __LINE__ survived /EP")
            total += len(head)
    # control: the same comparison must see the knob (else it compares nothing)
    for unit in ("gl_record_backend.c", "kage_vita_phase_profile.c"):
        knob = OFF_DEFINE_SETS["packed"] + [f"/D{KNOB}=2"]
        require(preprocess(env, compiler, RECOMP, unit, knob, work) !=
                preprocess(env, compiler, base_recomp, unit, knob, work),
                f"OFF surface control: {unit} with {KNOB}=2 equals the base")
    print(f"draw packet OFF surface: PASS ({len(OFF_UNITS)} units x {len(OFF_DEFINE_SETS)} define "
          f"sets, /EP token streams equal to {base[:7]}, {total} chars; control: {KNOB}=2 "
          f"differs in gl_record_backend.c and kage_vita_phase_profile.c)")


# ---- 1b. the knob's CMake block, evaluated by cmake -P -----------------------------
# The block from its first comment line to the next option(); the per-source
# definitions further down need a project and are proven on the box (only the
# recorder and the phase profile objects change).
CMAKE_FIRST = "# RENDER60 lane C (ROAD60-SYNTHESIS.md row 1)"
CMAKE_NEXT = "option(ISAAC_VITA_RUNTIME_SETTINGS"
PREREQS = ("ISAAC_VITA_GL_THREAD_WORKER", "ISAAC_VITA_KAGE_BIND_PACKET",
           "ISAAC_VITA_GL_RECORD_ATTRIB_BATCH", "ISAAC_VITA_PHASE_PROFILE")
MUST_BE = "ISAAC_VITA_GL_DRAW_PACKET must be OFF, VERIFY or ON"
NEEDS = "ISAAC_VITA_GL_DRAW_PACKET requires ISAAC_VITA_GL_THREAD_WORKER, ISAAC_VITA_KAGE_BIND_PACKET and"
NEEDS_PP = "ISAAC_VITA_GL_DRAW_PACKET=VERIFY requires ISAAC_VITA_PHASE_PROFILE=ON"


def check_cmake(work: Path) -> None:
    cmake = shutil.which("cmake") or r"C:\Program Files\CMake\bin\cmake.exe"
    require(Path(cmake).is_file() or shutil.which("cmake"), "no cmake on this host")
    lines = (HERE / "CMakeLists.txt").read_text(encoding="utf-8").splitlines()
    first = [i for i, l in enumerate(lines) if l.startswith(CMAKE_FIRST)]
    require(len(first) == 1, f"CMake block anchor: {len(first)}")
    end = next(i for i in range(first[0], len(lines)) if lines[i].startswith(CMAKE_NEXT))
    (work / "dp-block.cmake").write_text("\n".join(lines[first[0]:end]) + "\n", encoding="utf-8")
    (work / "dp-drive.cmake").write_text(
        "cmake_policy(SET CMP0126 NEW)\ninclude(${CMAKE_CURRENT_LIST_DIR}/dp-block.cmake)\n"
        'message(STATUS "DP-MODE=${ISAAC_VITA_GL_DRAW_PACKET_MODE}")\n', encoding="utf-8")
    on = {k: "ON" for k in PREREQS}
    cases = [  # (value or None, prerequisite overrides, expected mode or the FATAL text)
        (None, {}, 0), ("OFF", {}, 0), ("off", {}, 0), ("NO", {}, 0), ("", {}, 0),
        ("OFF", {k: "OFF" for k in PREREQS}, 0),
        ("VERIFY", on, 1), ("verify", on, 1), ("ON", on, 2), ("on", on, 2),
        ("BAD", on, MUST_BE), ("1", on, MUST_BE), ("2", on, MUST_BE), ("YES", on, MUST_BE),
    ] + [("ON", dict(on, **{k: "OFF"}), 2 if k == "ISAAC_VITA_PHASE_PROFILE" else NEEDS)
         for k in PREREQS] + [("VERIFY", dict(on, ISAAC_VITA_KAGE_BIND_PACKET="OFF"), NEEDS),
                              # 0.2.0 release recipe: ON builds without the profile, VERIFY does not
                              ("VERIFY", dict(on, ISAAC_VITA_PHASE_PROFILE="OFF"), NEEDS_PP)]
    for value, pre, want in cases:
        args = [cmake] + [f"-D{k}={v}" for k, v in pre.items()]
        if value is not None:
            args.append(f"-DISAAC_VITA_GL_DRAW_PACKET={value}")
        run = subprocess.run(args + ["-P", str(work / "dp-drive.cmake")], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT)
        said = " ".join(run.stdout.decode("utf-8", "replace").split())
        if isinstance(want, int):
            require(run.returncode == 0 and f"-- DP-MODE={want}" in said, f"cmake {value!r} {pre}: {said}")
        else:
            require(run.returncode != 0 and want in said and "DP-MODE=" not in said,
                    f"cmake {value!r} {pre}: wanted FATAL '{want}', got rc={run.returncode} {said}")
    print(f"draw packet CMake block: PASS ({len(cases)} cases under cmake -P: unset/OFF/off/NO/'' -> 0 even "
          "without the prerequisites, VERIFY -> 1, ON -> 2 (any case), BAD/1/2/YES FATAL, each of the three "
          "recorder prerequisites OFF FATAL, PHASE_PROFILE=OFF: ON -> 2, VERIFY FATAL)")


# ---- 2. scenario -------------------------------------------------------------------
# copied: recomp/vita/test_fbo_deferred_attach.py:842-850 (RECORD_SOURCES, PACKED) and
# :358-366 test_gl_vita_backend.ps1 worker jitter, as there.
RECORD_SOURCES = ("gl_record_backend.c", "gl_record_ring.c", "gl_record_locations.c")
PACKED = ["/DISAAC_VITA_GL_SHIM_FASTDISPATCH=1",
          "/DISAAC_VITA_SHADER_ATTRIB_FASTPATH=1",
          "/DISAAC_VITA_GL_THREAD_RECORD=1",
          "/DISAAC_VITA_GL_RECORD_ATTRIB_BATCH=1",
          "/DISAAC_VITA_KAGE_BIND_PACKET=1",
          "/DISAAC_VITA_GL_THREAD_WORKER=1",
          "/DISAAC_GL_RECORD_WORKER_JITTER=1",
          # IA4 runs admit behind ON (test_gl_vita_backend.ps1:371, its 'admitbehind' variant)
          "/DISAAC_VITA_GL_THREAD_RENDER_ADMISSION=1",
          "/DISAAC_VITA_GL_THREAD_ADMIT_BEHIND=2",
          "/DISAAC_VITA_GL_THREAD_ADMIT_BEHIND_BYTES=65536",
          "/DSCN_TRACE_RVA=1"]
VARIANTS = {
    "off": [],
    "verify": [f"/D{KNOB}=1"],
    "on": [f"/D{KNOB}=2"],
    "on-census": [f"/D{KNOB}=2", "/DISAAC_GL_RECORD_OPCODE_CENSUS=1"],
}
MODES = ("", "defer", "drawpkt")
# Mutants: name -> (variant, extra defines, backend text edit or None, what must fail)
BLEND_COVERED = "    return opcode == REC_glBlendFuncSeparate ||\n"
NRF = "/DISAAC_GL_RECORD_ORACLE_MUTATE_DP_NO_RECORD_FLUSH=1"
NSG = "/DISAAC_GL_RECORD_ORACLE_MUTATE_DP_NO_SYNC_GUARD=1"
RVA = "/DISAAC_GL_RECORD_ORACLE_MUTATE_DP_RVA=1"
DROP_LAST = "/DISAAC_GL_RECORD_ORACLE_MUTATE_DP_DROP_LAST=1"
FRAMING = "/DISAAC_GL_RECORD_ORACLE_MUTATE_DP_FRAMING=1"
FUSE0 = "/DISAAC_GL_RECORD_ORACLE_DP_FUSE_CLIENT_MAX=0"
MUTANTS = {
    # a record overtakes the staged calls: record_sync's fail-closed check stops the
    # process at the first synchronous call (guest_gl_backend_fault -> abort on the host)
    "no-record-flush-on": ("on", [NRF], None, "abort"),
    # the same without that check: ON replays out of order, VERIFY counts it as late
    "no-record-flush-unguarded-on": ("on", [NRF, NSG], None, "trace"),
    "no-record-flush-unguarded-verify": ("verify", [NRF, NSG], None, "late"),
    "rva-on": ("on", [RVA], None, "trace"),
    "drop-last-on": ("on", [DROP_LAST], None, "trace"),
    "corrupt-verify": ("verify", ["/DISAAC_GL_RECORD_ORACLE_MUTATE_DP_VERIFY_CORRUPT=1"], None, "mm"),
    # the ON walker's own defects, seen by VERIFY's walk of the same code (review item 4):
    # an op step off by 8, the packet's RVA for every op, a lost last op
    "framing-on": ("on", [FRAMING], None, "trace"),
    "framing-verify": ("verify", [FRAMING], None, "mm"),
    "rva-verify": ("verify", [RVA], None, "mm"),
    "drop-last-verify": ("verify", [DROP_LAST], None, "mm"),
    # not defects: the big-draw guard (review item 6) with its bound lowered to 0, so every
    # draw that would fuse goes alone behind a full-buffer flush; output must equal OFF
    "fuse-guard-on": ("on", [FUSE0], None, "guard"),
    "fuse-guard-verify": ("verify", [FUSE0], None, "guard"),
    # blend no longer staged: still correct output, but the rule's fused count breaks
    "blend-not-covered-on": ("on", [], (BLEND_COVERED, "    return 0 ||\n"), "fused"),
}


def scenario_build(env, compiler, work: Path, generated: Path, variant: str, mutant: str = "") -> Path:
    """copied: recomp/vita/test_fbo_deferred_attach.py:919-971 (scenario_build), the
    variants and mutants replaced by this lane's."""
    tag = f"dp-{variant}" + (f"-{mutant}" if mutant else "")
    recorder = RUNTIME / "gl_record_backend.c"
    extra = []
    if mutant:
        mv, extra, edit, _ = MUTANTS[mutant]
        require(mv == variant, f"mutant {mutant} is built as {mv}")
        if edit:
            text = recorder.read_bytes().decode("utf-8")
            require(text.count(edit[0]) == 1, f"mutant anchor: {mutant}")
            recorder = work / f"mutant-{tag}" / "gl_record_backend.c"
            recorder.parent.mkdir()
            recorder.write_bytes(text.replace(edit[0], edit[1]).encode("utf-8"))
    exe = work / f"{tag}.exe"
    objdir = work / f"obj-{tag}"
    objdir.mkdir()
    # test_gl_vita_backend.ps1:331-344 (the layer-2 scenario command).
    command = [
        compiler, "/nologo", "/std:c11", "/O2", "/W4", "/WX", "/wd4310",
        "/D_CRT_SECURE_NO_WARNINGS", "/DISAAC_GL_VITA_BACKEND_ORACLE=1",
        "/DISAAC_GL_RECORD_SCENARIO=1", "/DISAAC_VITA_IO_PROFILE=1",
    ] + PACKED + VARIANTS[variant] + extra + [
        "/I", str(generated), "/DGUEST_IMAGE_BASE=0x98000000u",
        "/I", str(RUNTIME),
        str(RUNTIME / "gl_bridge.c"), str(RUNTIME / "gl_vita_backend.c"), str(recorder),
    ] + [str(RUNTIME / unit) for unit in RECORD_SOURCES[1:]] + [
        str(RUNTIME / "guest_stack_legacy_oracle_stub.c"),
        str(RUNTIME / "gl_vita_backend_oracle.c"),
        str(SCENARIO_C),
        "/Fe:" + str(exe), "/Fo:" + str(objdir) + "\\",
    ]
    built = subprocess.run(command, env=env, cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = built.stdout.decode("cp866", errors="replace")
    (work / f"build-{tag}.log").write_text(log, encoding="utf-8")
    if built.returncode != 0 or not exe.is_file():
        print(log[-6000:])
        raise AssertionError(f"scenario build failed: {tag}")
    return exe


def scenario_run(work: Path, exe: Path, mode: str, poison: int):
    """(trace lines, stdout lines, return code)."""
    trace = work / f"trace-{exe.stem}-{mode or 'main'}-p{poison}.txt"
    ran = subprocess.run([str(exe), str(trace), str(poison)] + ([mode] if mode else []),
                         cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = ran.stdout.decode("ascii", errors="replace").replace("\r\n", "\n")
    lines = trace.read_text(encoding="ascii").splitlines() if trace.is_file() else []
    return lines, out.splitlines(), ran.returncode


WINDOW = re.compile(r"^draw packet window mode=(\d) d=(\d+) f=(\d+) pk=(\d+) p1=(\d+) sub=(\d+) "
                    r"B=(\d+) mx=(\d+) fr\(r,c,s,f\)=(\d+),(\d+),(\d+),(\d+) v\(p,s\)=(\d+),(\d+) "
                    r"mm=(\d+) late=(\d+) tot=(\d+)$")
KEYS = ("mode", "d", "f", "pk", "p1", "sub", "B", "mx", "fr_r", "fr_c", "fr_s", "fr_f", "vp", "vs",
        "mm", "late", "tot")
SCRIPT = re.compile(r"^drawpkt script draws=(\d+) fused=(\d+) probes=(\d+) drains=(\d+) ")
AFTER_WRAP = re.compile(r"^draw packet after-wrap=(\d+)$")
SUMMARY = {}   # (mode, poison, variant) -> (window, script, trace lines), from check_scenario


def after_wrap_of(out: list[str], tag: str) -> int:
    rows = [AFTER_WRAP.match(l) for l in out if l.startswith("draw packet after-wrap=")]
    require(len(rows) == 1 and rows[0], f"{tag}: one draw packet after-wrap line")
    return int(rows[0].group(1))


def window_of(out: list[str], tag: str) -> dict:
    rows = [WINDOW.match(l) for l in out if l.startswith("draw packet window ")]
    require(len(rows) == 1 and rows[0], f"{tag}: one draw packet window line\n" + "\n".join(out[-20:]))
    return dict(zip(KEYS, map(int, rows[0].groups())))


def script_of(out: list[str], tag: str) -> dict:
    rows = [SCRIPT.match(l) for l in out if l.startswith("drawpkt script ")]
    require(len(rows) == 1 and rows[0], f"{tag}: one drawpkt script line")
    return dict(zip(("draws", "fused", "probes", "drains"), map(int, rows[0].groups())))


def check_window(w: dict, variant: str, mode: str, script: dict | None, tag: str) -> None:
    require(w["mode"] == (1 if variant == "verify" else 2), f"{tag}: mode {w}")
    require(w["mm"] == 0 and w["late"] == 0 and w["tot"] == 0, f"{tag}: VERIFY counters {w}")
    require(w["d"] > 0 and 0 < w["f"] <= w["d"] and w["pk"] >= w["f"] and w["sub"] > 0 and
            w["B"] > 0 and 0 < w["mx"] <= 2048, f"{tag}: window {w}")
    if variant == "verify":
        # every packet compared, against its calls plus its fused draw
        require(w["vp"] == w["pk"] and w["vs"] == w["sub"] + w["f"], f"{tag}: VERIFY coverage {w}")
    else:
        require(w["vp"] == 0 and w["vs"] == 0, f"{tag}: ON has no VERIFY words {w}")
    if script is not None:
        # the script's own statement of the rule
        require(w["d"] == script["draws"] and w["f"] == script["fused"],
                f"{tag}: draws/fused {w['d']}/{w['f']} vs script {script}")
        require(w["fr_r"] > 0 and w["fr_c"] >= script["probes"] and w["fr_s"] >= script["drains"]
                and w["fr_f"] > 0, f"{tag}: flush reasons {w} vs script {script}")
        if variant != "verify":
            path = (w["pk"] + w["p1"] + w["d"] - w["f"]) / w["d"]
            require(w["p1"] > 0 and path <= 2.0, f"{tag}: path {path:.3f} records per draw {w}")


def check_scenario(env, compiler, work: Path, generated: Path, quick: bool) -> dict:
    exes = {v: scenario_build(env, compiler, work, generated, v) for v in VARIANTS}
    summary = {}
    for mode in MODES:
        for poison in ((0,) if quick else (0, 1)):
            ref, ref_out, rc = scenario_run(work, exes["off"], mode, poison)
            tag0 = f"[{mode or 'main'} p{poison} off]"
            require(rc == 0 and ref and "results" in ref, f"{tag0} rc={rc}\n" + "\n".join(ref_out[-20:]))
            require(not any(l.startswith("draw packet window") for l in ref_out), f"{tag0}: window line")
            for variant in VARIANTS:
                if variant == "off":
                    continue
                tag = f"[{mode or 'main'} p{poison} {variant}]"
                lines, out, rc = scenario_run(work, exes[variant], mode, poison)
                require(rc == 0, f"{tag} rc={rc}\n" + "\n".join(out[-20:]))
                if lines != ref:
                    first = next((i for i, (a, b) in enumerate(zip(ref, lines)) if a != b),
                                 min(len(ref), len(lines)))
                    raise AssertionError(f"{tag}: trace differs from OFF at line {first + 1}:\n"
                                         f"  off {ref[first] if first < len(ref) else '<end>'}\n"
                                         f"  {variant} {lines[first] if first < len(lines) else '<end>'}")
                w = window_of(out, tag)
                script = script_of(out, tag) if mode == "drawpkt" else None
                check_window(w, variant, mode, script, tag)
                if variant == "on-census":
                    require(any(l.startswith("census coverage: ") for l in out), f"{tag}: census ran")
                # review item 5: packets replayed from right behind a ring wrap marker (the
                # main and drawpkt scripts publish more than the 512 KiB ring)
                w["after_wrap"] = after_wrap_of(out, tag)
                if mode in ("", "drawpkt"):
                    require(w["after_wrap"] > 0, f"{tag}: no draw packet after a ring wrap {w}")
                summary[(mode, poison, variant)] = (w, script, len(ref))
    for mode in MODES:
        w, script, n = summary[(mode, 0, "on")]
        v, _, _ = summary[(mode, 0, "verify")]
        path = (w["pk"] + w["p1"] + w["d"] - w["f"]) / w["d"]
        print(f"draw packet scenario [{mode or 'main'}]: PASS (off = verify = on = on+census, poison "
              f"{'0' if quick else '0/1'}, {n} trace lines with RVAs; ON d={w['d']} f={w['f']} "
              f"pk={w['pk']} p1={w['p1']} sub={w['sub']} B={w['B']} mx={w['mx']} "
              f"fr(r,c,s,f)={w['fr_r']},{w['fr_c']},{w['fr_s']},{w['fr_f']} path {path:.3f}/draw; "
              f"VERIFY packets compared {v['vp']} with {v['vs']} records, mm 0 late 0; packets "
              f"after a ring wrap ON {w['after_wrap']} VERIFY {v['after_wrap']}"
              + (f"; script draws={script['draws']} fused={script['fused']}" if script else "") + ")")
    SUMMARY.update(summary)
    return exes


def check_mutants(env, compiler, work: Path, generated: Path) -> None:
    ref = (work / "trace-dp-off-drawpkt-p0.txt").read_text(encoding="ascii").splitlines()
    for mutant, (variant, _, _, aim) in MUTANTS.items():
        exe = scenario_build(env, compiler, work, generated, variant, mutant)
        lines, out, rc = scenario_run(work, exe, "drawpkt", 0)
        tag = f"mutant {mutant}"
        if aim == "abort":
            failed = rc != 0
            how = f"rc=0x{rc & 0xFFFFFFFF:08X}"
        elif aim == "trace":
            failed = rc != 0 or lines != ref
            first = next((f"line {i + 1}: '{a}' vs '{b}'" for i, (a, b) in enumerate(zip(ref, lines))
                          if a != b), f"rc={rc}" if rc else f"{len(lines)} vs {len(ref)} lines")
            how = f"rc={rc}" if rc else first
        else:
            require(rc == 0, f"{tag}: did not run rc={rc}\n" + "\n".join(out[-10:]))
            w = window_of(out, tag)
            if aim == "fused":
                s = script_of(out, tag)
                failed = w["f"] != s["fused"]
                how = f"fused {w['f']} vs script {s['fused']} (trace {'equal' if lines == ref else 'differs'})"
            elif aim == "guard":
                # every would-be fused draw becomes one more full-buffer flush, nothing else moves
                s = script_of(out, tag)
                plain = SUMMARY[("drawpkt", 0, variant)][0]
                require(lines == ref and w["f"] == 0 and w["d"] == s["draws"] and s["fused"] > 0
                        and w["fr_f"] == plain["fr_f"] + s["fused"] and w["mm"] == 0 and w["late"] == 0
                        and w["tot"] == 0 and (variant != "verify" or (w["vp"] == w["pk"] and
                                                                         w["vs"] == w["sub"])),
                        f"{tag}: {w} vs plain {plain} script {s} (trace "
                        f"{'equal' if lines == ref else 'differs'})")
                print(f"draw packet {tag} [{variant}]: PASS (trace equal to OFF, f 0 of d {w['d']}, "
                      f"fr f {plain['fr_f']} + {s['fused']} = {w['fr_f']}"
                      + (f", VERIFY vp {w['vp']} = pk, vs {w['vs']} = sub" if variant == "verify" else "")
                      + ")")
                continue
            else:
                failed = w[aim] > 0 and w["tot"] > 0
                how = f"{aim}={w[aim]} tot={w['tot']} (trace {'equal' if lines == ref else 'differs'})"
        require(failed, f"{tag} passed ({how})")
        print(f"draw packet {tag} [{variant}]: rejected ({how[:200]})")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default=DEFAULT_BASE)
    parser.add_argument("--keep", type=Path, default=None)
    parser.add_argument("--quick", action="store_true", help="poison 0 only")
    args = parser.parse_args()
    import build_all as BA

    env, compiler = BA.msvc_env()

    def go(work: Path) -> None:
        # gl_vita_backend_oracle.c includes the attribute owner extraction (generated;
        # copied: test_fbo_deferred_attach.py:1104-1113)
        generated = work / "obj-generated"
        generated.mkdir()
        made = subprocess.run([sys.executable, "-B", str(HERE / "test_vita_attrib_boundaries.py"),
                               "--output", str(generated)], cwd=RECOMP, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT)
        require(made.returncode == 0 and (generated / "gl_vita_attrib_boundaries_generated.inc").is_file(),
                "attribute owner extraction failed:\n" + made.stdout.decode("utf-8", "replace")[-2000:])
        check_off_surface(env, compiler, work, args.base)
        check_cmake(work)
        check_scenario(env, compiler, work, generated, args.quick)
        check_mutants(env, compiler, work, generated)

    if args.keep:
        if args.keep.exists():
            shutil.rmtree(args.keep)
        args.keep.mkdir(parents=True)
        go(args.keep)
    else:
        with tempfile.TemporaryDirectory(prefix="drawpkt-") as tmp:
            go(Path(tmp))
    print("draw packet: PASS")


if __name__ == "__main__":
    main()
