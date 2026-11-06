#!/usr/bin/env python3
"""RENDER60 S1 host gates: ISAAC_VITA_GL_RECORD_ADMISSION_CACHE_STANDALONE
(the recorder's own sceKernelFreeMemBlock wrap, gl_record_free_wrap.c) and
ISAAC_VITA_GL_RECORD_ADMISSION_CACHE_VERIFY.

Legs (each prints one PASS line; any failure exits 1):
  oracle    gl_record_free_wrap_oracle.c (it #includes the production TU),
            clang host and MSVC x86 /W4 /WX; the LATE_BUMP mutant must FAIL
            on both.  The oracle prints its branch coverage.
  text      the wrap body is exactly bump-then-real-free, as the overlay's
            wrap is (host_vita_debug_overlay.c:411-421); no lock, stdio,
            log or allocator in the TU.
  cmake     the real CMakeLists.txt blocks (GL recorder/worker defines and
            the admission-cache block), executed by `cmake -P` with
            set_property/target_link_options recorded, for recipe shapes A,
            A+S1, A+S1+VERIFY, B, B+S1, A-noov+S1+VERIFY and the refused
            ones; prints every shape's per-source definition lists and
            asserts gl_record_backend.c gets the same list in A and B+S1.
  identity  clang -E (__vita__, stub system headers) of gl_record_backend.c
            and kage_vita_backend.c at the base commit and now, under the
            define sets the knobs-OFF builds use: token-equal.  Positive
            controls: the RECEIPT and VERIFY defines do change the text.
  census    elf_ref_census.py on an ld.lld ARM fixture linked with and
            without --wrap (calls, a tail call, MOVW/MOVT and a data-word
            address-take): exact counts and all four gate arms.
  readme    README.md documents both knobs.

--plant runs each leg once against a planted defect and requires every one
to fail (the test of the test).
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import io

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "recomp/runtime"
VITA = ROOT / "recomp/vita"
LLVM = Path(os.environ.get("ISAAC_LLVM_BIN", r"C:\Program Files\LLVM\bin"))
STANDALONE = "ISAAC_VITA_GL_RECORD_ADMISSION_CACHE_STANDALONE"
VERIFY = "ISAAC_VITA_GL_RECORD_ADMISSION_CACHE_VERIFY"


class LegFailure(Exception):
    pass


def need(ok: bool, message: str) -> None:
    if not ok:
        raise LegFailure(message)


def tool(name: str) -> str:
    path = LLVM / (name + ".exe")
    if path.exists():
        return str(path)
    found = shutil.which(name)
    if not found:
        raise LegFailure(f"{name} not found (LLVM bin {LLVM})")
    return found


def strip_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


# ---------------------------------------------------------------- oracle ----
ORACLE_DEFINES = ("ISAAC_GL_RECORD_FREE_WRAP_ORACLE=1", "ISAAC_GL_RECORD_FREE_WRAP_OWNER_RECORDER=1")
MUTANT = "ISAAC_GL_RECORD_FREE_WRAP_ORACLE_MUTATE_LATE_BUMP=1"
COV = re.compile(r"gl_record_free_wrap oracle: checks=(\d+) failures=(\d+) "
                 r"cov\(order_ok,order_bad,uid,result\)=(\d+),(\d+),(\d+),(\d+) "
                 r"long\(bumps,frees\)=(\d+),(\d+) mutant=(\d)")


def msvc_env():
    from setuptools.msvc import msvc14_get_vc_env
    vc = msvc14_get_vc_env("x86")
    env = {k: v for k, v in os.environ.items() if k.lower() not in vc}
    env.update(vc)
    cl = shutil.which("cl.exe", path=vc["path"])
    if not cl:
        raise LegFailure("enumerated MSVC environment has no cl.exe")
    return cl, env


def build_oracle(compiler: str, runtime: Path, out: Path, mutant: bool) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    defines = list(ORACLE_DEFINES) + ([MUTANT] if mutant else [])
    exe = out / ("oracle-%s%s.exe" % (compiler, "-mutant" if mutant else ""))
    source = runtime / "gl_record_free_wrap_oracle.c"
    if compiler == "clang":
        command = [tool("clang"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                   *("-D" + d for d in defines), "-I", str(runtime), str(source), "-o", str(exe)]
        env = None
    else:
        cl, env = msvc_env()
        command = [cl, "/nologo", "/std:c11", "/O2", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS",
                   *("/D" + d for d in defines), "/I" + str(runtime), str(source),
                   "/Fe" + str(exe), "/Fo" + str(out) + "\\"]
    done = subprocess.run(command, cwd=out, env=env, capture_output=True, text=True)
    need(done.returncode == 0, f"oracle {compiler} build failed:\n{done.stdout}{done.stderr}")
    return exe


def leg_oracle(runtime: Path, out: Path) -> None:
    for compiler in ("clang", "msvc"):
        for mutant in (False, True):
            exe = build_oracle(compiler, runtime, out / "oracle", mutant)
            done = subprocess.run([str(exe)], capture_output=True, text=True)
            m = COV.search(done.stdout)
            need(m is not None, f"oracle {compiler} printed no coverage line:\n{done.stdout}")
            checks, failures, ok_, bad, uid, res, bumps, frees, mut = (int(x) for x in m.groups())
            print(f"  oracle {compiler} mutant={mut}: checks={checks} failures={failures} "
                  f"cov(order_ok,order_bad,uid,result)={ok_},{bad},{uid},{res} "
                  f"long(bumps,frees)={bumps},{frees} exit={done.returncode}")
            if not mutant:
                need(done.returncode == 0 and failures == 0 and "PASS" in done.stdout,
                     f"oracle {compiler} failed:\n{done.stdout}")
                need((ok_, bad, uid, res, bumps, frees) == (16, 0, 16, 16, 1000, 1000),
                     f"oracle {compiler}: coverage {ok_},{bad},{uid},{res},{bumps},{frees}")
            else:
                need(done.returncode != 0 and failures > 0 and bad == 16,
                     f"oracle {compiler}: the LATE_BUMP mutant was not caught:\n{done.stdout}")
    print("PASS oracle: clang + MSVC x86 PASS, LATE_BUMP mutant FAILs on both (order_bad=16/16)")


# ------------------------------------------------------------------ text ----
def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    open_ = text.index("{", start)
    depth, i = 0, open_
    while True:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_ + 1:i]
        i += 1


def production_branch(body: str) -> str:
    """The wrap body with the oracle mutant branch removed (#if M ... #else KEEP #endif)."""
    m = re.search(r"#if defined\(ISAAC_GL_RECORD_FREE_WRAP_ORACLE_MUTATE_LATE_BUMP\)\n(.*?)"
                  r"#else\n(.*?)#endif", body, re.S)
    need(m is not None, "wrap body: no MUTATE_LATE_BUMP/#else/#endif shape")
    return body[:m.start()] + m.group(2) + body[m.end():]


def leg_text(wrap_text: str, overlay_text: str) -> None:
    code = strip_comments(wrap_text)
    body = production_branch(function_body(code, "int __wrap_sceKernelFreeMemBlock(SceUID uid)\n{"))
    statements = [s.strip() + ";" for s in body.split(";") if s.strip()]
    need(statements == ["isaac_gl_record_memblock_freeing();",
                        "return __real_sceKernelFreeMemBlock(uid);"],
         f"wrap body is not bump-then-real-free: {statements}")
    no_strings = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', code)   # #error texts say "free"
    forbidden = re.findall(r"\b(malloc|calloc|realloc|free|printf|sceClibPrintf|isaac_vita_log\w*|"
                           r"sceKernel\w*(?:Mutex|Sema|LwMutex|Cond)\w*|sceKernelAllocMemBlock)\b",
                           no_strings)
    need(not forbidden, f"gl_record_free_wrap.c calls {sorted(set(forbidden))}")
    need(code.count("__wrap_sceKernelFreeMemBlock(SceUID uid)\n{") == 1, "one wrap definition")
    need(re.search(r"#if !defined\(ISAAC_GL_RECORD_FREE_WRAP_OWNER_RECORDER\)\n# error", code) is not None,
         "the TU must #error unless CMake names it the owner")
    ov = function_body(strip_comments(overlay_text), "int __wrap_sceKernelFreeMemBlock(SceUID uid)\n{")
    bump, real = ov.find("isaac_gl_record_memblock_freeing();"), ov.find("__real_sceKernelFreeMemBlock(uid)")
    need(0 <= bump < real, "the overlay's wrap no longer bumps before the real free")
    print("PASS text: wrap = isaac_gl_record_memblock_freeing(); return __real_...(uid); "
          "same order as the overlay's wrap; no lock/stdio/log/allocator")


# ----------------------------------------------------------------- cmake ----
SIM_HEAD = r'''
cmake_minimum_required(VERSION 3.20)
set(ISAAC_RUNTIME "/rt")
set(ISAAC_VITA_RUNTIME_SOURCES "")
function(sim_option name doc default)
  if(NOT DEFINED ${name})
    set(${name} ${default} PARENT_SCOPE)
  endif()
endfunction()
function(sim_set_property)
  set(a ${ARGN})
  list(GET a 0 kind)
  if(NOT kind STREQUAL "SOURCE")
    message(FATAL_ERROR "sim: set_property ${kind}")
  endif()
  list(FIND a APPEND iappend)
  list(FIND a COMPILE_DEFINITIONS idefs)
  math(EXPR want_defs "${iappend} + 2")
  if(iappend LESS 2 OR NOT idefs EQUAL want_defs)
    message(FATAL_ERROR "sim: unexpected set_property shape: ${a}")
  endif()
  math(EXPR last "${iappend} - 1")
  math(EXPR first_value "${idefs} + 1")
  list(SUBLIST a ${first_value} -1 values)
  foreach(i RANGE 1 ${last})
    list(GET a ${i} file)
    get_filename_component(file "${file}" NAME)
    set_property(GLOBAL APPEND PROPERTY "sim_defs_${file}" ${values})
  endforeach()
endfunction()
function(sim_link_options target scope)
  set_property(GLOBAL APPEND PROPERTY sim_link "${target}:${ARGN}")
endfunction()
function(sim_link_libraries)
endfunction()
'''
SIM_TAIL = r'''
foreach(f gl_record_backend.c kage_vita_backend.c gl_record_free_wrap.c gl_record_ring.c)
  get_property(d GLOBAL PROPERTY "sim_defs_${f}")
  message(STATUS "SIM defs ${f} = ${d}")
endforeach()
set(names "")
foreach(s IN LISTS ISAAC_VITA_RUNTIME_SOURCES)
  get_filename_component(n "${s}" NAME)
  list(APPEND names "${n}")
endforeach()
message(STATUS "SIM sources = ${names}")
get_property(l GLOBAL PROPERTY sim_link)
message(STATUS "SIM link = ${l}")
'''
COMMON = {"ISAAC_VITA_GL_THREAD_RECORD": "ON", "ISAAC_VITA_GL_THREAD_WORKER": "ON",
          "ISAAC_VITA_GL_THREAD_WORKER_PROOF": "OFF", "ISAAC_VITA_GL_THREAD_RENDER_ADMISSION": "ON",
          "ISAAC_VITA_IMAGE_RETAIN": "ON"}
# The 2026-10-25 device recipes: A = COMMON+PROFILE (DEBUG_OVERLAY, PHASE_PROFILE,
# HEAP_CENSUS ON), B = COMMON+RELEASE (all three OFF).  The overlay block sets
# ISAAC_GL_FREE_WRAP_BY_OVERLAY iff DEBUG_OVERLAY (checked by leg_cmake).
SHAPES = {
    "A": dict(COMMON, ISAAC_VITA_HEAP_CENSUS="ON", ISAAC_VITA_PHASE_PROFILE="ON",
              ISAAC_GL_FREE_WRAP_BY_OVERLAY="ON"),
    "B": dict(COMMON, ISAAC_VITA_HEAP_CENSUS="OFF", ISAAC_VITA_PHASE_PROFILE="OFF"),
    "A-noov": dict(COMMON, ISAAC_VITA_HEAP_CENSUS="ON", ISAAC_VITA_PHASE_PROFILE="ON"),
}
RUNS = {
    "A": ("A", {}),
    "A+S1": ("A", {STANDALONE: "ON"}),
    "A+S1+V": ("A", {STANDALONE: "ON", VERIFY: "ON"}),
    "B": ("B", {}),
    "B+S1": ("B", {STANDALONE: "ON"}),
    "A-noov+S1+V": ("A-noov", {STANDALONE: "ON", VERIFY: "ON"}),
    "A-noov+S1": ("A-noov", {STANDALONE: "ON"}),
    # refused
    "B+S1+V": ("B", {STANDALONE: "ON", VERIFY: "ON"}),
    "A+V": ("A", {VERIFY: "ON"}),
    "S1-noworker": ("B", {STANDALONE: "ON", "ISAAC_VITA_GL_THREAD_WORKER": "OFF"}),
}
REFUSED = {
    "B+S1+V": "GL_RECORD_ADMISSION_CACHE_VERIFY requires GL_RECORD_ADMISSION_CACHE_STANDALONE and PHASE_PROFILE",
    "A+V": "GL_RECORD_ADMISSION_CACHE_VERIFY requires GL_RECORD_ADMISSION_CACHE_STANDALONE and PHASE_PROFILE",
    "S1-noworker": "GL_RECORD_ADMISSION_CACHE_STANDALONE requires GL_THREAD_WORKER",
}


def endif_after(text: str, start: int, indent: str) -> int:
    """Index just past the `endif()` line at `indent` closing the if() at start."""
    m = re.compile(r"^" + re.escape(indent) + r"endif\(\)\n", re.M).search(text, start)
    need(m is not None, "cmake: unterminated block")
    return m.end()


def cmake_blocks(text: str) -> str:
    # Options + their first FATAL (with the GL thread knobs).
    o = text.index(f"option({STANDALONE}\n")
    o_end = endif_after(text, text.index(f"if({STANDALONE} AND NOT ISAAC_VITA_GL_THREAD_WORKER)", o), "")
    options = text[o:o_end]
    need(f"option({VERIFY}\n" in options, "cmake: VERIFY option not next to STANDALONE")
    # The VERIFY check depends on PHASE_PROFILE: declared after its option.
    p = text.index("option(ISAAC_VITA_PHASE_PROFILE\n")
    v = text.index(f"if({VERIFY} AND\n")
    need(v > p, "cmake: the VERIFY FATAL precedes option(ISAAC_VITA_PHASE_PROFILE)")
    verify_check = text[v:endif_after(text, v, "")]
    # GL recorder / worker source definitions (up to the owner check).
    r = text.index("  if(ISAAC_VITA_GL_THREAD_RECORD)\n    list(APPEND ISAAC_VITA_RUNTIME_SOURCES\n"
                   "      \"${ISAAC_RUNTIME}/gl_record_backend.c\"")
    r_end = text.index("  if(ISAAC_VITA_GL_OWNER_CHECK)\n", r)
    record = text[r:r_end]
    # The overlay owns its wrap inside if(ISAAC_VITA_DEBUG_OVERLAY) only.
    flag = "        set(ISAAC_GL_FREE_WRAP_BY_OVERLAY ON)\n"
    need(text.count("set(ISAAC_GL_FREE_WRAP_BY_OVERLAY ON)") == 1, "cmake: BY_OVERLAY set more than once")
    ov = text.rindex("      if(ISAAC_VITA_DEBUG_OVERLAY)\n", 0, text.index(flag))
    need(text.index(flag) < endif_after(text, ov, "      "), "cmake: BY_OVERLAY set outside the overlay block")
    # The admission-cache block: after the overlay block, before the dispatch table.
    m = re.compile(r"^    if\(" + STANDALONE + r"[^\n]*\)\n", re.M).search(text, text.index(flag))
    need(m is not None, "cmake: no admission-cache block after the overlay block")
    c_end = text.index("    if(ISAAC_VITA_DISPATCH_TABLE)\n", m.start())
    cache = text[m.start():c_end]
    body = "\n".join((options, verify_check, record, cache))
    body = body.replace("option(", "sim_option(")
    body = re.sub(r"\bset_property\(SOURCE", "sim_set_property(SOURCE", body)
    body = body.replace("target_link_options(", "sim_link_options(")
    body = body.replace("target_link_libraries(", "sim_link_libraries(")
    need("set_property(" not in body.replace("sim_set_property(", ""), "cmake: unsimulated set_property")
    return SIM_HEAD + body + SIM_TAIL


def gl_record_backend_sites(text: str) -> int:
    return text.count('"${ISAAC_RUNTIME}/gl_record_backend.c"')


# RENDER60 W-pkt names gl_record_backend.c once more, outside the simulated
# slices: inside if(ISAAC_VITA_KAGE_BIND_PACKET) (default OFF, in no shape
# here) the seam TU and the recorder get that one define.  Only this exact
# statement is excused; any other new site still fails the count.
PKT_STATEMENT = re.compile(
    r'^    set_property\(SOURCE\n'
    r'      "\$\{ISAAC_RUNTIME\}/host_vita_kage_bind_packet\.c"\n'
    r'      "\$\{ISAAC_RUNTIME\}/gl_record_backend\.c"\n'
    r'      APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_KAGE_BIND_PACKET=1\)\n', re.M)


def pkt_sites(text: str) -> int:
    found = list(PKT_STATEMENT.finditer(text))
    need(len(found) <= 1, f"cmake: the W-pkt bind-packet statement appears {len(found)} times")
    for m in found:
        head = "  if(ISAAC_VITA_KAGE_BIND_PACKET)\n"
        start = text.rfind(head, 0, m.start())
        need(start >= 0 and endif_after(text, start, "  ") > m.end(),
             "cmake: the W-pkt bind-packet statement is outside if(ISAAC_VITA_KAGE_BIND_PACKET)")
    return len(found)


# RENDER60 S4 names gl_record_backend.c once more, outside the simulated
# slices: inside if(ISAAC_VITA_GL_RECORD_ATTRIB_BATCH) (default OFF, in no
# shape here) the replay TU and the recorder get that one define.  Only this
# exact statement is excused; any other new site still fails the count.
S4_STATEMENT = re.compile(
    r'^      set_property\(SOURCE\n'
    r'        "\$\{ISAAC_RUNTIME\}/host_vita_shader_attrib_fastpath\.c"\n'
    r'        "\$\{ISAAC_RUNTIME\}/gl_record_backend\.c"\n'
    r'        APPEND PROPERTY COMPILE_DEFINITIONS\n'
    r'          ISAAC_VITA_GL_RECORD_ATTRIB_BATCH=1\)\n', re.M)


def s4_sites(text: str) -> int:
    found = list(S4_STATEMENT.finditer(text))
    need(len(found) <= 1, f"cmake: the S4 attribute-batch statement appears {len(found)} times")
    for m in found:
        head = "    if(ISAAC_VITA_GL_RECORD_ATTRIB_BATCH)\n"
        start = text.rfind(head, 0, m.start())
        need(start >= 0 and endif_after(text, start, "    ") > m.end(),
             "cmake: the S4 attribute-batch statement is outside if(ISAAC_VITA_GL_RECORD_ATTRIB_BATCH)")
    return len(found)


def run_shape(cmake: str, script: Path, name: str):
    shape, extra = RUNS[name]
    values = dict(SHAPES[shape], **extra)
    command = [cmake, *(f"-D{k}={v}" for k, v in sorted(values.items())), "-P", str(script)]
    done = subprocess.run(command, capture_output=True, text=True)
    lines = (done.stdout + done.stderr).splitlines()
    result = {"rc": done.returncode, "text": done.stdout + done.stderr, "warning": "CMake Warning" in done.stderr}
    for line in lines:
        m = re.match(r"-- SIM (defs (\S+)|sources|link) = (.*)$", line)
        if m:
            key = m.group(2) or m.group(1)
            result[key] = [v for v in m.group(3).split(";") if v]
    return result


def leg_cmake(cmake_text: str, out: Path) -> dict:
    # Every per-source definition of gl_record_backend.c comes from the six
    # statements the simulation covers or the owner check (in no recipe).
    # The opcode census block (GL_RECORD_OPCODE_CENSUS, a measurement knob
    # OFF here) sits inside the simulated recorder slice; its own test is
    # test_gl_record_opcode_census.py.
    sites = gl_record_backend_sites(cmake_text) - s4_sites(cmake_text) - pkt_sites(cmake_text)
    need(sites == 7, f"cmake: gl_record_backend.c named {sites} times (expected 7: sources, RECORD, "
         "WORKER, OPCODE_CENSUS, OWNER_CHECK, CACHE, VERIFY; plus the S4 attribute-batch and the "
         "W-pkt bind-packet statements, excused); extend this simulation")
    out.mkdir(parents=True, exist_ok=True)
    script = out / "s1_cmake_sim.cmake"
    script.write_text(cmake_blocks(cmake_text), encoding="utf-8")
    cmake = shutil.which("cmake")
    need(cmake is not None, "cmake not on PATH")
    results = {name: run_shape(cmake, script, name) for name in RUNS}
    for name, r in results.items():
        if name in REFUSED:
            # CMake wraps a FATAL_ERROR message across lines: compare words.
            need(r["rc"] != 0 and " ".join(REFUSED[name].split()) in " ".join(r["text"].split()),
                 f"cmake {name}: not refused:\n{r['text']}")
            print(f"  cmake {name}: refused ({REFUSED[name]})")
            continue
        need(r["rc"] == 0, f"cmake {name}: failed:\n{r['text']}")
        print(f"  cmake {name}:")
        for key in ("gl_record_backend.c", "kage_vita_backend.c", "gl_record_free_wrap.c", "link"):
            print(f"    {key}: {' '.join(r.get(key, [])) or '-'}")
        print(f"    sources: {' '.join(r.get('sources', [])) or '-'}  warning={int(r['warning'])}")
    a, a1, av, b, b1 = (results[k] for k in ("A", "A+S1", "A+S1+V", "B", "B+S1"))
    nv, n1 = results["A-noov+S1+V"], results["A-noov+S1"]
    backend = ["ISAAC_VITA_GL_THREAD_RECORD=1", "ISAAC_VITA_GL_THREAD_WORKER=1",
               "ISAAC_GL_RECORD_ADMISSION_CACHE=1"]
    need(a["gl_record_backend.c"] == backend, f"A backend defs {a['gl_record_backend.c']}")
    need(b["gl_record_backend.c"] == backend[:2], f"B backend defs {b['gl_record_backend.c']}")
    for name, r in (("A+S1", a1), ("B+S1", b1), ("A-noov+S1", n1)):
        need(r["gl_record_backend.c"] == backend, f"{name}: gl_record_backend.c defs differ from A's")
    for name, r in (("A+S1+V", av), ("A-noov+S1+V", nv)):
        need(r["gl_record_backend.c"] == backend + [f"ISAAC_GL_RECORD_ADMISSION_CACHE_VERIFY=1"],
             f"{name}: backend defs {r['gl_record_backend.c']}")
    receipt = "ISAAC_GL_RECORD_ADMISSION_RECEIPT="
    need(not any(d.startswith(receipt) for d in a["kage_vita_backend.c"] + b["kage_vita_backend.c"]),
         "a receipt reaches kage_vita_backend.c with the knob OFF")
    need(a1["kage_vita_backend.c"] == a["kage_vita_backend.c"] + [receipt + '"overlay"'],
         f"A+S1 kvb defs {a1['kage_vita_backend.c']}")
    need(b1["kage_vita_backend.c"] == b["kage_vita_backend.c"] + [receipt + '"recorder"'],
         f"B+S1 kvb defs {b1['kage_vita_backend.c']}")
    need(av["kage_vita_backend.c"] == a1["kage_vita_backend.c"] +
         ["ISAAC_GL_RECORD_ADMISSION_CACHE_VERIFY=1"], "A+S1+V kvb defs")
    wrap_defs = ["ISAAC_VITA_GL_THREAD_WORKER=1", "ISAAC_GL_RECORD_FREE_WRAP_OWNER_RECORDER=1"]
    for name, r in (("B+S1", b1), ("A-noov+S1", n1), ("A-noov+S1+V", nv)):
        need(r.get("sources", []).count("gl_record_free_wrap.c") == 1, f"{name}: free-wrap TU not added once")
        need(r.get("gl_record_free_wrap.c") == wrap_defs, f"{name}: free-wrap defs {r.get('gl_record_free_wrap.c')}")
        need(r.get("link") == ["isaac_first_arm_fault:-Wl,--wrap=sceKernelFreeMemBlock"],
             f"{name}: link options {r.get('link')}")
    for name, r in (("A", a), ("A+S1", a1), ("A+S1+V", av), ("B", b)):
        need("gl_record_free_wrap.c" not in r.get("sources", []), f"{name}: free-wrap TU added")
        need(not r.get("link"), f"{name}: the simulated blocks added link options {r.get('link')}")
        need(not r.get("gl_record_free_wrap.c"), f"{name}: free-wrap defs set")
    need(b["warning"] and not b1["warning"] and not a["warning"], "cmake: the no-cache WARNING shape")
    print("PASS cmake: gl_record_backend.c defs identical in A, A+S1 and B+S1 "
          f"({' '.join(backend)}); receipt only with S1 (overlay|recorder); free-wrap TU + "
          "--wrap only without the overlay; VERIFY/STANDALONE refusals")
    return results


# -------------------------------------------------------------- identity ----
TARGET_WIDE = ["GUEST_IMAGE_BASE=0x81000000u", "GUEST_STACK_REQUIRED=1", "ISAAC_VITA_HEAP_MB=64",
               "ISAAC_VITA_HAS_RUNTIME=1", "ISAAC_VITA_RAW_ALLOCATOR_GATE=1", "ISAAC_VITA_LUA=1"]
REC = "ISAAC_VITA_GL_THREAD_RECORD=1"
WRK = "ISAAC_VITA_GL_THREAD_WORKER=1"
CACHE = "ISAAC_GL_RECORD_ADMISSION_CACHE=1"
# Knobs-OFF define sets per TU (recipe shapes plus neighbours); VERIFY and
# RECEIPT are never in them.
OFF_SETS = {
    "gl_record_backend.c": {   # it #errors without ISAAC_VITA_GL_THREAD_RECORD
        "record": [REC], "B": [REC, WRK], "A": [REC, WRK, CACHE],
        "A-proof": [REC, WRK, "ISAAC_VITA_GL_THREAD_WORKER_PROOF=1", CACHE],
        "owner-check": [REC, WRK, CACHE, "ISAAC_VITA_GL_OWNER_CHECK=1"],
    },
    "kage_vita_backend.c": {
        "none": [], "record": [REC],
        "B": [REC, WRK, "ISAAC_VITA_GL_THREAD_RENDER_ADMISSION=1", "ISAAC_VITA_GL_THREAD_VGL_MEM_SNAPSHOT=1"],
        "A": [REC, WRK, "ISAAC_VITA_GL_THREAD_RENDER_ADMISSION=1", "ISAAC_VITA_GL_THREAD_VGL_MEM_SNAPSHOT=1",
              "ISAAC_VITA_PHASE_PROFILE=1", "ISAAC_VITA_DEBUG_OVERLAY=1", "ISAAC_VITA_LOG_ASYNC=1"],
    },
}
# Positive controls: (TU, OFF set, extra defines, a string the output must then contain).
CONTROLS = (
    ("gl_record_backend.c", "A", ["ISAAC_GL_RECORD_ADMISSION_CACHE_VERIFY=1"], "isaac_gl_record_admission_selftest"),
    ("kage_vita_backend.c", "B", ['ISAAC_GL_RECORD_ADMISSION_RECEIPT="recorder"'], "KAGE VITA GL RECORD ADMISSION"),
    ("kage_vita_backend.c", "A", ['ISAAC_GL_RECORD_ADMISSION_RECEIPT="overlay"',
                                  "ISAAC_GL_RECORD_ADMISSION_CACHE_VERIFY=1"], "isaac_gl_record_admission_selftest"),
)


def base_available(base: str) -> bool:
    """The identity leg reads the base commit from git history; a checkout
    without that commit (the public snapshot) cannot run it."""
    return subprocess.run(["git", "cat-file", "-e", base + "^{commit}"], cwd=ROOT,
                          capture_output=True).returncode == 0


def export_base(base: str, out: Path) -> Path:
    dest = out / f"base-{base}"
    marker = dest / ".complete"
    if not marker.exists():
        if dest.exists():
            shutil.rmtree(dest)
        dest.mkdir(parents=True)
        archive = subprocess.run(["git", "archive", base, "recomp/runtime", "recomp/vita/isaac_vita_raw_allocator_poison.h"],
                                 cwd=ROOT, check=True, capture_output=True).stdout
        with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
            try:
                tar.extractall(dest, filter="data")
            except TypeError:   # Python without extraction filters; our own repository's archive
                tar.extractall(dest)
        marker.write_text(base)
    return dest / "recomp/runtime"


def preprocess(runtime: Path, name: str, defines: list[str], stubs: Path, source: Path | None = None) -> str:
    poison = runtime.parent / "vita/isaac_vita_raw_allocator_poison.h"
    for _ in range(100):
        command = [tool("clang"), "--target=armv7a-none-eabi", "-E", "-P", "-nostdinc", "-D__vita__",
                   "-include", str(poison), "-I", str(runtime), "-isystem", str(stubs),
                   *("-D" + d for d in TARGET_WIDE + defines), str(source or runtime / name)]
        done = subprocess.run(command, capture_output=True, text=True)
        if done.returncode == 0:
            return done.stdout
        m = re.search(r"fatal error: '([^']+)' file not found", done.stderr)
        need(m is not None, f"clang -E {name} failed:\n{done.stderr[-2000:]}")
        stub = stubs / m.group(1)   # an empty system header: tokens only matter on our side
        stub.parent.mkdir(parents=True, exist_ok=True)
        stub.write_text("")
    raise LegFailure(f"clang -E {name}: too many missing headers")


def leg_identity(cur_runtime: Path, base_runtime: Path, out: Path, plant: dict | None = None) -> None:
    stubs = out / "stub-include"
    stubs.mkdir(parents=True, exist_ok=True)
    planted = out / "planted"
    compared = 0
    for name, sets in OFF_SETS.items():
        source = None
        if plant and name in plant:
            planted.mkdir(parents=True, exist_ok=True)
            source = planted / name
            source.write_text(plant[name](( cur_runtime / name).read_text(encoding="utf-8")), encoding="utf-8")
        for label, defines in sets.items():
            base = preprocess(base_runtime, name, defines, stubs).split()
            cur = preprocess(cur_runtime, name, defines, stubs, source).split()
            need(base == cur, f"identity {name} [{label}]: preprocessed tokens differ from the base "
                 f"({len(base)} vs {len(cur)} tokens)")
            compared += 1
            print(f"  identity {name} [{label}]: {len(cur)} tokens = base")
    for name, label, extra, needle in CONTROLS:
        base = preprocess(base_runtime, name, OFF_SETS[name][label], stubs).split()
        on = preprocess(cur_runtime, name, OFF_SETS[name][label] + extra, stubs)
        need(on.split() != base and needle in on, f"identity control {name} {extra}: no effect")
        print(f"  control {name} [{label}] + {' '.join(extra)}: differs from base, contains {needle!r}")
    print(f"PASS identity: {compared} TU/define-set pairs token-equal to the base; "
          f"{len(CONTROLS)} positive controls change the text")


# ---------------------------------------------------------------- census ----
FIXTURE = {
    "stub.c": "int sceKernelFreeMemBlock(int uid) { return uid - 1; }\n",
    "caller.c": (
        "int sceKernelFreeMemBlock(int uid);\n"
        "typedef int (*free_fn)(int);\n"
        "free_fn table[2] = { sceKernelFreeMemBlock, 0 };\n"
        "volatile unsigned long taken;\n"
        "__attribute__((noinline)) int call_it(int uid) { return sceKernelFreeMemBlock(uid) + 3; }\n"
        "__attribute__((noinline)) int tail_it(int uid) { return sceKernelFreeMemBlock(uid); }\n"
        "__attribute__((noinline)) void take_it(void) { taken = (unsigned long)&sceKernelFreeMemBlock; }\n"
        "void _start(void) { take_it(); (void)call_it(1); (void)tail_it(2); (void)table[0](3); for (;;) {} }\n"),
    "wrap.c": (
        "int __real_sceKernelFreeMemBlock(int uid);\n"
        "volatile unsigned bumps;\n"
        "int __wrap_sceKernelFreeMemBlock(int uid) { ++bumps; return __real_sceKernelFreeMemBlock(uid); }\n"),
}


def census(elf: Path, *extra: str) -> tuple[int, str]:
    command = [sys.executable, str(VITA / "elf_ref_census.py"), "--readelf", tool("llvm-readelf"),
               "--objdump", tool("llvm-objdump"), *extra, str(elf), "sceKernelFreeMemBlock",
               "__wrap_sceKernelFreeMemBlock"]
    done = subprocess.run(command, capture_output=True, text=True)
    return done.returncode, done.stdout + done.stderr


def leg_census(out: Path, bypass: bool = False) -> None:
    """bypass (the --plant defect): one more object calls the real stub
    through __real_ from outside the wrap; `--gate wrapped` must refuse it."""
    fx = out / "census-fixture"
    fx.mkdir(parents=True, exist_ok=True)
    units = dict(FIXTURE)
    if bypass:
        units["bypass.c"] = ("int __real_sceKernelFreeMemBlock(int uid);\n"
                             "int bypass(int uid) { return __real_sceKernelFreeMemBlock(uid) + 1; }\n")
    for name, text in units.items():
        (fx / name).write_text(text)
        subprocess.run([tool("clang"), "--target=armv7a-none-eabi", "-mthumb", "-mcpu=cortex-a9", "-O2",
                        "-ffunction-sections", "-fdata-sections", "-c", name, "-o", name[:-2] + ".o"],
                       cwd=fx, check=True)
    lld = tool("ld.lld")
    for f in ("nowrap.elf", "wrap.elf"):
        (fx / f).unlink(missing_ok=True)
    subprocess.run([lld, "-q", "-e", "_start", "stub.o", "caller.o", "-o", "nowrap.elf"], cwd=fx, check=True)
    extra = ["bypass.o", "--undefined=bypass"] if bypass else []
    subprocess.run([lld, "-q", "-e", "_start", "--wrap=sceKernelFreeMemBlock", "stub.o", "caller.o", "wrap.o",
                    *extra, "-o", "wrap.elf"], cwd=fx, check=True)
    # The verdicts first (the plant must be refused by the gate itself).
    arms = (("wrap.elf", "wrapped", 5, 0), ("nowrap.elf", "unwrapped", 5, 0),
            ("nowrap.elf", "wrapped", 1, 1), ("wrap.elf", "unwrapped", 1, 1),
            ("wrap.elf", "wrapped", 6, 1))
    for elf, gate, min_sites, want in arms:
        rc, text = census(fx / elf, "--gate", gate, "--min-sites", str(min_sites))
        verdict = [l for l in text.splitlines() if l.startswith("census-gate")]
        need(len(verdict) == 1 and (rc != 0) == bool(want) and
             verdict[0].endswith("FAIL" if want else "PASS"),
             f"census gate {gate} on {elf} (min {min_sites}): rc={rc} {verdict}")
        print(f"  gate {elf} --gate {gate} --min-sites {min_sites}: {verdict[0].split(': ', 1)[1]}")
    # nowrap: call_it (THM_CALL), tail_it (THM_JUMP24), take_it (MOVW+MOVT),
    # table (ABS32 in .data); _start calls the three helpers and table[0].
    rc, text = census(fx / "nowrap.elf")
    print("\n".join("  nowrap " + l for l in text.splitlines()))
    expect_nowrap = (
        "census total sceKernelFreeMemBlock: relocs=5 call_relocs=2 address_relocs=3",
        "census site sceKernelFreeMemBlock R_ARM_ABS32 .rel.data in=@table n=1",
        "census site sceKernelFreeMemBlock R_ARM_THM_JUMP24 .rel.text in=tail_it n=1",
        "census site sceKernelFreeMemBlock R_ARM_THM_MOVW_ABS_NC .rel.text in=take_it n=1",
        "census symbol __wrap_sceKernelFreeMemBlock: defined=0 ABSENT",
        "AGREE",
    )
    for line in expect_nowrap:
        need(line in text, f"census nowrap: missing {line!r}")
    need(rc == 0, "census nowrap: exit status")
    rc, text = census(fx / "wrap.elf")
    print("\n".join("  wrap " + l for l in text.splitlines()))
    expect_wrap = (
        "census total __wrap_sceKernelFreeMemBlock: relocs=5 call_relocs=2 address_relocs=3",
        "census total sceKernelFreeMemBlock: relocs=1 call_relocs=1 address_relocs=0",
        "census site sceKernelFreeMemBlock R_ARM_THM_JUMP24 .rel.text in=__wrap_sceKernelFreeMemBlock n=1",
        "census site __wrap_sceKernelFreeMemBlock R_ARM_ABS32 .rel.data in=@table n=1",
        "AGREE",
    )
    for line in expect_wrap:
        need(line in text, f"census wrap: missing {line!r}")
    print("PASS census: ld.lld -q fixture, exact call/tail/MOVW-MOVT/ABS32 counts with and without "
          "--wrap; gate arms wrapped/unwrapped PASS and FAIL as expected")


# ---------------------------------------------------------------- readme ----
def leg_readme(readme: str) -> None:
    for knob in (STANDALONE, VERIFY, "gl_record_free_wrap.c", "test_gl_record_free_wrap.py"):
        need(knob in readme, f"README.md does not mention {knob}")
    print("PASS readme: both knobs, the TU and this gate are documented")


def move_verify_check_up(text: str) -> str:
    """Plant: the VERIFY FATAL back above option(ISAAC_VITA_PHASE_PROFILE)."""
    v = text.index(f"if({VERIFY} AND\n")
    block = text[v:endif_after(text, v, "")]
    text = text[:v] + text[v + len(block):]
    o = text.index(f"option({STANDALONE}\n")
    return text[:o] + block + text[o:]


# ------------------------------------------------------------------ main ----
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", default="e6b69f7", help="knobs-OFF identity base commit")
    parser.add_argument("--output", type=Path, default=ROOT / "build/gl-record-free-wrap")
    parser.add_argument("--plant", action="store_true", help="every leg must FAIL on a planted defect")
    parser.add_argument("--legs", default="oracle,text,cmake,identity,census,readme")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    legs = args.legs.split(",")
    wrap_text = (RUNTIME / "gl_record_free_wrap.c").read_text(encoding="utf-8")
    overlay_text = (RUNTIME / "host_vita_debug_overlay.c").read_text(encoding="utf-8")
    cmake_text = (VITA / "CMakeLists.txt").read_text(encoding="utf-8")
    readme = (VITA / "README.md").read_text(encoding="utf-8")
    have_base = base_available(args.base)
    if not have_base:
        print(f"SKIP identity: base commit {args.base} is not in this checkout's history "
              "(the leg needs it; give --base)")
    if not args.plant:
        try:
            if "oracle" in legs:
                leg_oracle(RUNTIME, out)
            if "text" in legs:
                leg_text(wrap_text, overlay_text)
            if "cmake" in legs:
                leg_cmake(cmake_text, out / "cmake")
            if "identity" in legs and have_base:
                leg_identity(RUNTIME, export_base(args.base, out), out / "identity")
            if "census" in legs:
                leg_census(out)
            if "readme" in legs:
                leg_readme(readme)
        except LegFailure as error:
            print(f"FAIL {error}")
            return 1
        print("PASS test_gl_record_free_wrap: " + ",".join(
            leg for leg in legs if leg != "identity" or have_base))
        return 0

    # --plant: each planted defect must make its leg fail.
    planted_runtime = out / "plant-runtime"
    if planted_runtime.exists():
        shutil.rmtree(planted_runtime)
    shutil.copytree(RUNTIME, planted_runtime, ignore=shutil.ignore_patterns("*.obj", "*.o", "*.exe"))
    (planted_runtime.parent / "vita").mkdir(exist_ok=True)
    shutil.copy2(VITA / "isaac_vita_raw_allocator_poison.h", planted_runtime.parent / "vita")
    swapped = wrap_text.replace(
        "    isaac_gl_record_memblock_freeing();\n    return __real_sceKernelFreeMemBlock(uid);\n#endif",
        "    {\n        int result = __real_sceKernelFreeMemBlock(uid);\n"
        "        isaac_gl_record_memblock_freeing();\n        return result;\n    }\n#endif")
    assert swapped != wrap_text
    plants = {
        "oracle: free before bump": lambda: (
            (planted_runtime / "gl_record_free_wrap.c").write_text(swapped, encoding="utf-8"),
            leg_oracle(planted_runtime, out / "plant-oracle")),
        "text: free before bump": lambda: leg_text(swapped, overlay_text),
        "text: a log call in the wrap": lambda: leg_text(wrap_text.replace(
            "    isaac_gl_record_memblock_freeing();\n    return",
            "    isaac_gl_record_memblock_freeing();\n    isaac_vita_log(\"free\");\n    return"), overlay_text),
        "cmake: second owner with the overlay": lambda: leg_cmake(cmake_text.replace(
            f"if({STANDALONE} AND NOT ISAAC_GL_FREE_WRAP_BY_OVERLAY)", f"if({STANDALONE})"), out / "plant-cmake1"),
        "cmake: cache only with the overlay": lambda: leg_cmake(cmake_text.replace(
            "(ISAAC_GL_FREE_WRAP_BY_OVERLAY OR ISAAC_GL_FREE_WRAP_BY_RECORDER))",
            "ISAAC_GL_FREE_WRAP_BY_OVERLAY)"), out / "plant-cmake2"),
        "cmake: VERIFY check before PHASE_PROFILE": lambda: leg_cmake(move_verify_check_up(cmake_text),
                                                                      out / "plant-cmake3"),
        "cmake: the S4 statement names a third unit": lambda: leg_cmake(cmake_text.replace(
            '        "${ISAAC_RUNTIME}/gl_record_backend.c"\n        APPEND PROPERTY COMPILE_DEFINITIONS\n'
            '          ISAAC_VITA_GL_RECORD_ATTRIB_BATCH=1)\n',
            '        "${ISAAC_RUNTIME}/gl_record_backend.c"\n        "${ISAAC_RUNTIME}/gl_record_ring.c"\n'
            '        APPEND PROPERTY COMPILE_DEFINITIONS\n          ISAAC_VITA_GL_RECORD_ATTRIB_BATCH=1)\n', 1),
            out / "plant-cmake4"),
        "cmake: the W-pkt statement names a third unit": lambda: leg_cmake(cmake_text.replace(
            '      "${ISAAC_RUNTIME}/gl_record_backend.c"\n'
            '      APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_KAGE_BIND_PACKET=1)\n',
            '      "${ISAAC_RUNTIME}/gl_record_backend.c"\n      "${ISAAC_RUNTIME}/gl_record_ring.c"\n'
            '      APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_KAGE_BIND_PACKET=1)\n', 1),
            out / "plant-cmake5"),
        "identity: receipt unguarded": lambda: leg_identity(RUNTIME, export_base(args.base, out), out / "plant-id1", {
            "kage_vita_backend.c": lambda t: t.replace(
                "#if defined(ISAAC_GL_RECORD_ADMISSION_RECEIPT)\n    {", "#if 1\n    {", 1)}),
        "identity: cache edit outside VERIFY": lambda: leg_identity(RUNTIME, export_base(args.base, out), out / "plant-id2", {
            "gl_record_backend.c": lambda t: t.replace("static uint32_t s_mb_next;", "static uint32_t s_mb_next, s_plant;", 1)}),
        "census: a caller of the real stub outside the wrap": lambda: leg_census(out / "plant-census", bypass=True),
        "readme: entry missing": lambda: leg_readme(readme.replace(VERIFY, "ISAAC_VITA_SOMETHING_ELSE")),
    }
    if not have_base:
        for name in [name for name in plants if name.startswith("identity:")]:
            del plants[name]
            print(f"PLANT {name}: SKIP (no base commit)")
    caught = 0
    for name, plant in plants.items():
        try:
            plant()
        except (LegFailure, subprocess.CalledProcessError) as error:
            caught += 1
            print(f"PLANT {name}: caught ({str(error).splitlines()[0][:160]})")
        else:
            print(f"PLANT {name}: NOT CAUGHT")
    print(f"{'PASS' if caught == len(plants) else 'FAIL'} --plant: {caught}/{len(plants)} planted defects caught")
    return 0 if caught == len(plants) else 1


if __name__ == "__main__":
    sys.exit(main())
