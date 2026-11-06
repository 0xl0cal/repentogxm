#!/usr/bin/env python3
"""Proof of ISAAC_VITA_CAMERA_RING_NATIVE (host_vita_camera_ring_native.c).

1. Frozen inputs: the sub_0049df90 body bytes, the four constant addresses
   and ring field offsets the replay copies, the seven callers, and the
   constants' PE values (passed to the oracle).
2. Corpus contract: one owner unit of sub_0049df90, one seam marker, every
   caller inside that unit (so --wrap could not see them), the seam text
   exactly as gen_all.py emits it, and the body without its seam blocks
   byte-identical to the frozen int3-off body (OFF identity of the text).
3. Differential oracle: the seam-stripped body against the seamed body with
   the replay, MSVC x86 (legacy xmm tokens) and clang i686 GUEST_SSE_LOWER=1
   (the production token set), GPR-local and memory-mode, one pinned result.
4. Build gate: default OFF, prerequisites, owner census, GUEST_FLAGS_LOCAL
   mirrored onto the replay TU, the source reachable from the knob only.

usage: test_vita_camera_ring_native.py --pe PE --generated-dir DIR [--keep DIR]
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
VITA_CMAKE = HERE / "vita" / "CMakeLists.txt"
sys.path.insert(0, str(HERE))

import build_all  # noqa: E402
import gen_all  # noqa: E402
from test_vita_kage_quad_fastpath import (  # noqa: E402
    BODY_RE, pe_section_reader, require, run)

PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
ROOT = "sub_0049df90"
ROOT_RVA, ROOT_END = 0x0049DF90, 0x0049E0EC
BODY_SHA256 = "4c789228913f8d2d3d16b0286044334d556479b21bb81513e7fb15a477979127"
# sub_0049df90 as generated-int3-off renders it (no seam).
FROZEN_TEXT_SHA256 = "a152d6db68b1a9399509f1d7d57f72c183948e66954313296dc5b7b000aac511"
CALLERS = (0x0049F16A, 0x0049F17F, 0x0049F194, 0x0049F1A6, 0x0049F2F7,
           0x0049F6A4, 0x0049FA93)
# {macro: (rva, instruction site, bytes before the disp32)}
CONSTANTS = {"CAM_RESET_X": (0x00802FEC, 0x0049DF9C, "f30f1005"),
             "CAM_RESET_Y": (0x00802FF0, 0x0049DFB8, "f30f1005"),
             "CAM_ONE": (0x0076A614, 0x0049DFD0, "f30f101d"),
             "CAM_LOW": (0x0076AE44, 0x0049DFF4, "f30f1015")}
# {macro: (offset, site, instruction bytes)}
RING_FIELDS = {"CAM_RING_DATA": (0x0, 0x0049E072, "8b01"),
               "CAM_RING_CAP": (0x4, 0x0049E06C, "f77904"),
               "CAM_RING_COUNT": (0x8, 0x0049DFA6, "8b4108"),
               "CAM_RING_START": (0xC, 0x0049E05D, "8b410c")}
SEAM = [gen_all.VITA_CAMERA_RING_GUARD,
        "    /* %s; exact translated fallback follows. */"
        % gen_all.VITA_CAMERA_RING_MARKER,
        "    GUEST_GPR_FLUSH(c);",
        "    if (%s(c))" % gen_all.VITA_CAMERA_RING_HELPER,
        "        return;",
        "    GUEST_GPR_RELOAD(c);",
        "#endif"]
DECL = [gen_all.VITA_CAMERA_RING_GUARD,
        "    extern int %s(CPU *__restrict);" % gen_all.VITA_CAMERA_RING_HELPER,
        "#endif"]
ORACLE_RESULT = (
    "Vita camera ring oracle: PASS; cases=3217; handled=2949; declined=262; "
    "probes=6; logger=5; faults=1; steps=1599898")
# ph120.cr totals summed from per-case takes (the oracle checks each case
# moves exactly one verdict counter); the 268 declines are 262 seam + 6 probes.
ORACLE_COUNTERS = ("counters: n=3217 h=2949 st=1599898 "
                   "d(f,o,r,k,l,x)=3,31,6,5,5,218")

COMMON = ["/nologo", "/W4", "/WX", "/O2", "/Gy", "/std:c11",
          "/wd4310", "/wd4702", "/wd4996", "/DGUEST_IMAGE_BASE=0x98000000u"]
DISPATCH_DEFINES = ["/DGUEST_STACK_REQUIRED=1",
                    "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
                    "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                    "/DISAAC_VITA_PHASE_PROFILE=1",
                    "/DISAAC_VITA_PROFILE_IMPORT_KINDS=1"]
# recomp/vita/CMakeLists.txt compile definitions of a generated unit.
BODY_DEFINES = ["GUEST_STACK_REQUIRED=1", "GUEST_GENERATED_STACK_GUARD=0",
                "GUEST_FLAGS_LOCAL=1", "GUEST_COVERAGE_HOOKS=0",
                "ISAAC_VITA_IMPORT_ID_DISPATCH=1",
                "ISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                "ISAAC_VITA_PHASE_PROFILE=1",
                "ISAAC_VITA_PROFILE_IMPORT_KINDS=1",
                "GUEST_IMAGE_BASE=0x98000000u"]
CLANG = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM" / "bin" / "clang.exe"
# Same expressions as oracle_layout() in host_vita_camera_ring_native_oracle.c.
LAYOUT = """    out[0] = (uint32_t)sizeof(CPU);
    out[1] = (uint32_t)sizeof(xmm_t);
    out[2] = (uint32_t)_Alignof(xmm_t);
    out[3] = (uint32_t)offsetof(CPU, eax);
    out[4] = (uint32_t)offsetof(CPU, esp);
    out[5] = (uint32_t)offsetof(CPU, x);
    out[6] = (uint32_t)offsetof(CPU, fl);
    out[7] = (uint32_t)offsetof(CPU, stack_floor);
    out[8] = (uint32_t)offsetof(CPU, stack_owner);
    out[9] = (uint32_t)offsetof(CPU, run_scope);
    out[10] = (uint32_t)offsetof(CPU, fault);
    out[11] = (uint32_t)offsetof(CPU, stop_kind);"""


def verify_frozen(pe_path: Path) -> str:
    pe = pe_path.read_bytes()
    require(hashlib.sha256(pe).hexdigest() == PE_SHA256, "frozen PE changed")
    read = pe_section_reader(pe)
    require(hashlib.sha256(read(ROOT_RVA, ROOT_END - ROOT_RVA)).hexdigest() ==
            BODY_SHA256, "sub_0049df90 body changed")
    source = (RUNTIME / "host_vita_camera_ring_native.c").read_text(
        encoding="utf-8")

    def macro(name: str) -> int:
        m = re.search(rf"#define {name} 0x([0-9A-Fa-f]+)U", source)
        require(m is not None, f"{name} missing")
        return int(m.group(1), 16)

    for name, (rva, site, opcode) in CONSTANTS.items():
        require(macro(name) == rva, f"{name} drifted from the listing")
        require(read(site, 8) == bytes.fromhex(opcode) +
                (0x00400000 + rva).to_bytes(4, "little"),
                f"{name}: {site:#x} no longer loads {rva:#x}")
    for name, (offset, site, insn) in RING_FIELDS.items():
        require(macro(name) == offset, f"{name} drifted from the listing")
        require(read(site, len(insn) // 2) == bytes.fromhex(insn),
                f"{name}: {site:#x} changed")
    require(read(0x0049E0E9, 3) == bytes.fromhex("c20800"), "ret 8 moved")
    for site in CALLERS:
        b = read(site, 5)
        require(b[0] == 0xE8 and site + 5 + int.from_bytes(
            b[1:], "little", signed=True) == ROOT_RVA, f"caller {site:#x} changed")
    # X0, Y0, ONE, LOW as the PE initialises them: the oracle's default world.
    return b"".join(read(rva, 4) for rva, _, _ in CONSTANTS.values()).hex()


def strip_seam(text: str) -> str:
    out, skipping = [], False
    for line in text.split("\n"):
        if line == gen_all.VITA_CAMERA_RING_GUARD:
            skipping = True
            continue
        if skipping:
            if line == "#endif":
                skipping = False
            continue
        out.append(line)
    return "\n".join(out)


def extract(generated_dir: Path) -> tuple[str, str]:
    owners, markers, body = [], [], None
    for unit in sorted(generated_dir.glob("guest_[0-9][0-9][0-9][0-9].c")):
        text = unit.read_text(encoding="utf-8")
        if gen_all.VITA_CAMERA_RING_MARKER in text:
            markers.append(unit)
        if f"\nvoid {ROOT}(CPU *__restrict c)\n" in text:
            owners.append(unit)
            for match in BODY_RE.finditer(text):
                if match.group(1) == ROOT:
                    body = match.group(0)
            calls = re.findall(r"/\* ([0-9a-f]{8})  call 0x49df90 \*/", text)
            require(tuple(int(x, 16) for x in calls) == CALLERS,
                    f"callers inside the owner unit changed: {calls}")
    require(len(owners) == 1 and body is not None, f"owners {owners}")
    require(markers == owners, f"seam marker units {markers}")
    lines = body.split("\n")
    decl_at = lines.index(DECL[0])
    require(lines[decl_at:decl_at + 3] == DECL, "seam declaration changed")
    seam_at = lines.index(SEAM[0], decl_at + 3)
    require(lines[seam_at:seam_at + len(SEAM)] == SEAM, "seam block changed")
    require(lines[seam_at + len(SEAM)] == "    /* 0049df90  push ebp */",
            "the seam is not at the first instruction")
    require(body.count(gen_all.VITA_CAMERA_RING_GUARD) == 2, "extra seam blocks")
    reference = strip_seam(body)
    require(hashlib.sha256(reference.encode()).hexdigest() == FROZEN_TEXT_SHA256,
            "the body without its seam differs from the frozen int3-off body")
    seamed = body.replace(gen_all.VITA_CAMERA_RING_GUARD, "#if 1")
    return (reference.replace(f"void {ROOT}(", f"void ref_{ROOT}(", 1),
            seamed.replace(f"void {ROOT}(", f"void seam_{ROOT}(", 1))


def build_and_run(env, compiler: str, work: Path, reference: str, seamed: str,
                  frozen: str) -> list[str]:
    bodies_c = work / "camera_bodies.c"
    bodies_c.write_text("\n".join([
        "/* test_vita_camera_ring_native.py: reference and seamed bodies */",
        '#include "guest.h"',
        "#include <stddef.h>",
        "void sub_0055e330(CPU *__restrict c);",
        f"void ref_{ROOT}(CPU *__restrict c);",
        f"void seam_{ROOT}(CPU *__restrict c);",
        # the oracle compares this with its own view (two compilers link here)
        "void oracle_bodies_layout(uint32_t *out);",
        "void oracle_bodies_layout(uint32_t *out)",
        "{",
        LAYOUT,
        "}",
        reference, seamed]), encoding="utf-8", newline="\n")
    replay = work / "camera_replay.obj"
    run([compiler] + COMMON + ["/DGUEST_STACK_REQUIRED=1", "/DGUEST_FLAGS_LOCAL=1",
         "/Disaac_vita_camera_ring_try=native_camera_ring_try",
         "/I", str(RUNTIME), "/c",
         str(RUNTIME / "host_vita_camera_ring_native.c"), "/Fo:" + str(replay)], env)
    flavours = []
    for gpr in ("1", "0"):
        obj = work / f"bodies_msvc_gpr{gpr}.obj"
        run([compiler, "/nologo", "/W3", "/O2", "/std:c11", "/wd4310", "/wd4996",
             f"/DGUEST_GPR_LOCAL={gpr}"] + [f"/D{d}" for d in BODY_DEFINES] +
            ["/I", str(RUNTIME), "/c", str(bodies_c), "/Fo:" + str(obj)], env)
        flavours.append((f"msvc-gpr{gpr}", obj))
        # GUEST_SSE_LOWER=1 needs the GCC vector extension.  The MSVC record
        # layout gives a union holding a vector 16-byte alignment (guest.h's
        # static assert wants the 8 that ARM GCC gives), so clang uses the
        # Itanium layout of the gnu target with the MSVC headers.
        obj = work / f"bodies_clang_sse_gpr{gpr}.obj"
        run([str(CLANG), "--target=i686-pc-windows-gnu", "-O2", "-std=gnu11",
             "-fms-extensions", "-D_MSC_VER=1938", "-D_WIN32", "-D_M_IX86=600",
             "-w", "-DGUEST_SSE_LOWER=1", f"-DGUEST_GPR_LOCAL={gpr}"] +
            [f"-D{d}" for d in BODY_DEFINES] +
            [arg for path in env.get("INCLUDE", "").split(";") if path
             for arg in ("-isystem", path)] +
            ["-I", str(RUNTIME), "-c", str(bodies_c), "-o", str(obj)], env)
        flavours.append((f"clang-sse-gpr{gpr}", obj))
    results = []
    for flavour, obj in flavours:
        exe = work / f"camera-oracle-{flavour}.exe"
        objdir = work / f"obj-{flavour}"
        objdir.mkdir()
        run([compiler] + COMMON + DISPATCH_DEFINES + [
            "/I", str(RUNTIME),
            str(RUNTIME / "host_vita_camera_ring_native_oracle.c"), str(obj),
            str(replay), "/Fe:" + str(exe), "/Fo:" + str(objdir) + os.sep,
            "/link", "/LARGEADDRESSAWARE", "/OPT:REF", "/INCREMENTAL:NO"], env)
        completed = subprocess.run([str(exe), frozen], env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = completed.stdout.decode("cp866", errors="replace")
        (work / f"oracle-{flavour}.out").write_text(out, encoding="utf-8")
        lines = [line for line in out.strip().splitlines()
                 if not line.startswith("guest: FAULT")]
        result = lines[-1] if lines else ""
        if (completed.returncode != 0 or result != ORACLE_RESULT or
                ORACLE_COUNTERS not in lines):
            print(out[-6000:])
            raise AssertionError(f"oracle result changed ({flavour}, rc "
                                 f"{completed.returncode:#x}): {result!r}")
        results.append(f"[{flavour}] {result}")
    return results


def verify_build_gate() -> None:
    cmake = VITA_CMAKE.read_text(encoding="utf-8")
    require(re.search(r"option\(ISAAC_VITA_CAMERA_RING_NATIVE\s+\"[^\"]*\"\s+OFF\)",
                      cmake) is not None, "option missing or not default OFF")
    for needle in ("ISAAC_VITA_CAMERA_RING_NATIVE requires ISAAC_VITA_TRANSLATED_CPU",
                   "ISAAC_VITA_CAMERA_RING_NATIVE requires generated guest runtime"):
        require(needle in cmake, f"prerequisite gate missing: {needle}")
    block = re.search(r"\n  if\(ISAAC_VITA_CAMERA_RING_NATIVE\)\n(.*?)\n  endif\(\)\n",
                      cmake, re.S)
    require(block is not None, "knob block missing")
    body = block.group(1)
    for needle in ('"${ISAAC_RUNTIME}/host_vita_camera_ring_native.c"',
                   f'"{gen_all.VITA_CAMERA_RING_MARKER}"',
                   "ISAAC_VITA_CAMERA_RING_NATIVE=1", "GUEST_FLAGS_LOCAL=1",
                   "ISAAC_CAMERA_RING_OWNER_COUNT EQUAL 1"):
        require(needle in body, f"knob block lacks {needle}")
    require(cmake.count("host_vita_camera_ring_native.c") == 3,
            "the replay source must be reachable from the knob block only")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    parser.add_argument("--generated-dir", type=Path, required=True)
    parser.add_argument("--keep", type=Path)
    arguments = parser.parse_args()

    frozen = verify_frozen(arguments.pe)
    reference, seamed = extract(arguments.generated_dir)
    verify_build_gate()
    require(CLANG.is_file(), f"clang not found at {CLANG}")
    env, compiler = build_all.msvc_env()
    if arguments.keep:
        shutil.rmtree(arguments.keep, ignore_errors=True)
        arguments.keep.mkdir(parents=True)
        results = build_and_run(env, compiler, arguments.keep, reference, seamed,
                                frozen)
    else:
        with tempfile.TemporaryDirectory() as tmp:
            results = build_and_run(env, compiler, Path(tmp), reference, seamed,
                                    frozen)
    for line in results:
        print(line)
    print("Vita camera ring native: PASS; pins=body+4 constants+4 fields+7 callers; "
          f"oracle flavours={len(results)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
