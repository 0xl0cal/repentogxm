#!/usr/bin/env python3
"""ISAAC_VITA_LUA_COST_PROFILE: unit oracle, mutations, the frozen
_RunCallback dispatch with the knob ON vs OFF, and OFF source identity.

usage: python recomp/test_vita_lua_cost.py --pe PE --core-scripts DIR
           --lua-source LUA-5.3.3 [--work DIR]
(--work defaults to a new temporary directory)
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
BASE_COMMIT = "33afc7e64cb37a1d1e21cac71618f3243ba4e763"
sys.path.insert(0, str(HERE))

import test_vita_lua_callback_dispatch as dispatch  # noqa: E402
import test_vita_lua_runtime as lua_runtime  # noqa: E402

UNIT_EXPECTED = """\
[isaac-lua] cost win=0 ms=0 pc=3/169 cb=3/46 nest=1 fin=1 out=1 lost=1 gcvm=2/13 gccb=1/7 gcext=1/9 al=130/2/2 clk=0 ovf=0 ps=40ef41:2/163,3fd66f:1/6
[isaac-lua] cost-top win=0 top=3f0001:1/37,3f0003:1/5,3f0005:1/4
[isaac-lua] cost win=1 ms=2000 pc=1/2000000 cb=72/142 nest=0 fin=0 out=0 lost=0 gcvm=0/0 gccb=0/0 gcext=0/0 al=0/0/0 clk=0 ovf=7 ps=40ef41:1/2000000
[isaac-lua] cost-top win=1 top=200:2/2,100:1/2,101:1/2,102:1/2,103:1/2,104:1/2,105:1/2,106:1/2
"""

# (old, new) on host_vita_lua_cost.c; each must change the unit transcript.
MUTATIONS = (
    ("    if (s_cost.gc_depth)\n        ++s_cost.fin;",
     "    if (s_cost.gc_depth)\n        ++s_cost.nest;"),
    ("    if (s_cost.cb_depth > mark->cb_depth) {",
     "    if (s_cost.cb_depth >= mark->cb_depth + 1U && 0) {"),
    (">= COST_WINDOW_US)", "> COST_WINDOW_US)"),
    ("(s->n == b->n && s->key < b->key)", "(s->n == b->n && s->key > b->key)"),
    ("% COST_ALLOC_SAMPLE != 0U", "% COST_ALLOC_SAMPLE != 1U"),
    ("s_cost.cb_depth ? GC_CB : GC_VM", "s_cost.cb_depth ? GC_VM : GC_CB"),
    # (dropping `!s_cost.pc_depth` is equivalent: a pcall inside a pcall is
    # always inside a callback or a GC step.)
    ("mark->top = !s_cost.pc_depth && !s_cost.cb_depth",
     "mark->top = !s_cost.pc_depth"),
    ("        if (s_cost.gc_open && mark->gc_depth == 0U)\n            cost_gc_close(now);",
     "        (void)0;"),
    ("    else if (!s_cost.pc_depth)\n        ++s_cost.out;",
     "    else if (0)\n        ++s_cost.out;"),
)

CL = ["cl", "/nologo", "/std:c11", "/O2", "/W4", "/WX", "/MT", "/wd4310"]


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def msvc(vcvars: Path, commands: list[str], cwd: Path, log: Path) -> None:
    line = f'call {lua_runtime.quote(vcvars)} x86 >nul && ' + " && ".join(commands)
    with log.open("a", encoding="utf-8", errors="replace") as out:
        done = subprocess.run(line, cwd=cwd, shell=True, stdout=out,
                              stderr=subprocess.STDOUT)
    require(done.returncode == 0, f"MSVC step failed, see {log}")


def run(exe: Path, args: list[str], cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run([str(exe), *args], cwd=cwd, text=True,
                          stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=False)


def unit_build(vcvars: Path, work: Path, source: Path, name: str) -> str:
    q = lua_runtime.quote
    exe = work / f"{name}.exe"
    msvc(vcvars, [" ".join(CL + ["/DISAAC_VITA_LUA_COST_PROFILE=1",
                                 "/DISAAC_VITA_LUA_COST_PROFILE_ORACLE=1",
                                 f"/I{q(RUNTIME)}", f"/Fe{q(exe)}",
                                 f"/Fo{q(work)}\\", q(source),
                                 q(RUNTIME / 'vita_lua_cost_oracle.c')])],
         work, work / f"{name}.log")
    done = run(exe, [], work)
    require(done.returncode == 0 and done.stderr == "",
            f"{name} exited {done.returncode}: {done.stderr}")
    return done.stdout.replace("\r\n", "\n")


def unit_and_mutations(vcvars: Path, work: Path) -> None:
    out = unit_build(vcvars, work, RUNTIME / "host_vita_lua_cost.c", "unit")
    if out != UNIT_EXPECTED:
        sys.stdout.write(out)
        fail("unit oracle transcript changed")
    print("unit oracle: PASS (4 lines exact)")
    text = (RUNTIME / "host_vita_lua_cost.c").read_text(encoding="utf-8")
    killed = 0
    for index, (old, new) in enumerate(MUTATIONS):
        require(text.count(old) == 1, f"mutation {index} anchor not unique: {old!r}")
        mutant = work / f"mutant{index}"
        mutant.mkdir(exist_ok=True)
        path = mutant / "host_vita_lua_cost.c"
        path.write_text(text.replace(old, new), encoding="utf-8")
        got = unit_build(vcvars, mutant, path, f"mutant{index}")
        if got == UNIT_EXPECTED:
            fail(f"mutation {index} survived: {new!r}")
        killed += 1
    print(f"mutations: {killed}/{len(MUTATIONS)} killed")


def off_identity(vcvars: Path, work: Path, lua_src: Path) -> None:
    """The bridge with the knob OFF preprocesses to the base commit's text."""
    if subprocess.run(["git", "cat-file", "-e", BASE_COMMIT + "^{commit}"],
                      cwd=HERE, capture_output=True).returncode != 0:
        print(f"OFF identity: SKIP (base commit {BASE_COMMIT[:7]} is not in this "
              "repository's history)")
        return
    base = work / "base_host_vita_lua.c"
    blob = subprocess.run(["git", "show", f"{BASE_COMMIT}:recomp/runtime/host_vita_lua.c"],
                          cwd=HERE, capture_output=True, check=True).stdout
    base.write_bytes(blob)
    q = lua_runtime.quote
    sets = ("", "/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1",
            "/DISAAC_VITA_LUA_NATIVE_INDEX=1 /DISAAC_VITA_LUA_NATIVE_GETCLASS=1 "
            "/DISAAC_VITA_LUA_CALL_ERROR_CATCH=1 /DISAAC_VITA_LUA_GCSTEP_CLAMP_KB=128")
    for index, defines in enumerate(sets):
        outputs = []
        for label, path in (("base", base), ("new", RUNTIME / "host_vita_lua.c")):
            target = work / f"pp-{index}-{label}.i"
            msvc(vcvars, [" ".join(["cl", "/nologo", "/EP", defines,
                                    f"/I{q(RUNTIME)}", f"/I{q(lua_src / 'src')}",
                                    q(path), ">", q(target)])],
                 work, work / "pp.log")
            # Blank lines only shift; any __LINE__ use would differ in text.
            outputs.append([l for l in target.read_bytes().splitlines() if l.strip()])
        require(outputs[0] == outputs[1], f"OFF preprocessing differs for set {index}")
    print(f"OFF identity: PASS ({len(sets)} define sets preprocess to {BASE_COMMIT[:7]}'s tokens; only blank lines differ)")


COST_LINE = re.compile(r"\[isaac-lua\] cost win=(\d+) ms=\d+ pc=(\d+)/\d+ cb=(\d+)/\d+ "
                       r"nest=(\d+) fin=(\d+) out=(\d+) lost=(\d+) gcvm=(\d+)/\d+ "
                       r"gccb=(\d+)/\d+ gcext=(\d+)/\d+ al=(\d+)/(\d+)/\d+ clk=\d+ "
                       r"ovf=(\d+) ps=(\S*)")
TOP_LINE = re.compile(r"\[isaac-lua\] cost-top win=\d+ top=(\S*)")


def dispatch_on_off(vcvars: Path, work: Path, pe: Path, core: Path,
                    lua_src: Path) -> None:
    sentinel = dispatch.DEFAULT_SENTINEL.resolve()
    dispatch.validate_inputs(pe, core, sentinel)
    manifest = lua_runtime.records()
    lua_sources = lua_runtime.validate_source(lua_src, manifest)
    generated = work / "run_callback_generated.c"
    dispatch.verify_and_emit(pe, generated)
    q = lua_runtime.quote
    bridge = [RUNTIME / "host_vita_lua.c", dispatch.ORACLE, generated]
    exes = {}
    for variant in ("off", "on"):
        build = work / f"dispatch-{variant}"
        build.mkdir(exist_ok=True)
        groups: dict[tuple[str, ...], list[Path]] = {}
        for path in lua_sources:
            defines: tuple[str, ...] = ()
            if variant == "on":
                # GNU ld --wrap semantics: only references from other objects.
                defines = (f"/DluaC_step={'__real' if path.name == 'lgc.c' else '__wrap'}_luaC_step",
                           f"/DluaM_realloc_={'__real' if path.name == 'lmem.c' else '__wrap'}_luaM_realloc_")
            groups.setdefault(defines, []).append(path)
        for defines, paths in groups.items():
            msvc(vcvars, [" ".join(["cl", "/nologo", "/std:c11", "/O2", "/W3", "/MT",
                                    "/D_CRT_SECURE_NO_WARNINGS", *defines,
                                    f"/I{q(lua_src / 'src')}", "/c",
                                    *[q(p) for p in paths]])],
                 build, build / "build.log")
        commands = []
        extra = []
        defines = ["/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1",
                   "/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH_ORACLE=1"]
        if variant == "on":
            extra = [RUNTIME / "host_vita_lua_cost.c",
                     RUNTIME / "vita_lua_cost_dispatch_shim.c"]
            defines += ["/DISAAC_VITA_LUA_COST_PROFILE=1",
                        "/DISAAC_VITA_LUA_COST_PROFILE_ORACLE=1"]
        commands.append(" ".join(CL + defines + [f"/I{q(lua_src / 'src')}",
                                                 f"/I{q(RUNTIME)}", "/c"] +
                                 [q(p) for p in bridge + extra]))
        exe = build / f"dispatch-{variant}.exe"
        objects = [f"{p.stem}.obj" for p in lua_sources + bridge + extra]
        commands.append(" ".join(["link", "/nologo", f"/out:{q(exe)}", *objects,
                                  "kernel32.lib"]))
        msvc(vcvars, commands, build, build / "build.log")
        exes[variant] = (exe, build)

    transcripts, stdouts = {}, {}
    for variant, (exe, build) in exes.items():
        tsv = build / "transcript.tsv"
        done = run(exe, [str(dispatch.SETUP_HARNESS), str(sentinel.parent),
                         str(core), str(tsv)], build)
        require(done.returncode == 0 and done.stderr == "",
                f"dispatch {variant} exited {done.returncode}:\n{done.stdout}{done.stderr}")
        transcripts[variant] = dispatch.verify_transcript(tsv)
        stdouts[variant] = done.stdout.replace("\r\n", "\n")
    require(transcripts["off"] == transcripts["on"], "sentinel transcript differs ON vs OFF")
    off_lines = stdouts["off"].splitlines()
    on_lines = stdouts["on"].splitlines()
    cost = [l for l in on_lines if l.startswith("[isaac-lua] cost")]
    rest = [l for l in on_lines if not l.startswith("[isaac-lua] cost")]
    require(rest == off_lines, f"oracle stdout differs ON vs OFF: {rest} vs {off_lines}")
    rows = [COST_LINE.fullmatch(l) for l in cost if " cost win=" in l]
    tops = [TOP_LINE.fullmatch(l) for l in cost if "cost-top" in l]
    require(rows and all(rows) and len(tops) == len(rows) and all(tops),
            f"cost lines malformed: {cost}")
    total = [sum(int(m.group(i)) for m in rows) for i in range(2, 14)]
    pc, cb, nest, fin, out, lost, gcvm, gccb, gcext, al, al_timed, ovf = total
    # 2 sentinel dispatches + 25 luaL_error + 25 direct-longjmp dispatches, each
    # one top-level lua_pcallk from the generated _RunCallback; each error
    # dispatch reaches exactly one translated callback; the 25 direct
    # longjmps leave their callback through the pcall import (lost).
    require(pc == 52, f"pc={pc}, expected 52 top-level pcalls")
    require(cb == 50, f"cb={cb}, expected 50 translated callbacks")
    require(lost == 25, f"lost={lost}, expected the 25 direct longjmps")
    require(nest == 0 and out == 0 and ovf == 0, f"nest/out/ovf={nest}/{out}/{ovf}")
    require(al > 0 and all(int(m.group(12)) == int(m.group(11)) // 64 for m in rows),
            f"allocator sampling al={al} timed={al_timed}")
    sites = ",".join(m.group(14) for m in rows)
    require("40ef41:" in sites, f"pcall site 0x40ef41 (frozen _RunCallback) missing: {sites}")
    targets = ",".join(m.group(1) for m in tops)
    print(f"dispatch ON vs OFF: PASS (transcript and stdout identical; pc={pc} cb={cb} "
          f"lost={lost} gcvm/gccb/gcext={gcvm}/{gccb}/{gcext} fin={fin} al={al}; "
          f"top={targets})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", required=True, type=Path)
    parser.add_argument("--core-scripts", required=True, type=Path)
    parser.add_argument("--lua-source", type=Path)
    parser.add_argument("--work", type=Path,
                        help="scratch directory (default: a new temporary directory)")
    args = parser.parse_args()
    work = (args.work or Path(tempfile.mkdtemp(prefix="isaac-lua-cost-"))).resolve()
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    vcvars = lua_runtime.find_vcvars()
    lua_src = lua_runtime.discover_source(args.lua_source)
    unit_and_mutations(vcvars, work)
    off_identity(vcvars, work, lua_src)
    dispatch_on_off(vcvars, work, args.pe.resolve(), args.core_scripts.resolve(), lua_src)
    print("PASS: test_vita_lua_cost")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
