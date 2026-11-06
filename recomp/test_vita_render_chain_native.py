#!/usr/bin/env python3
"""Proof of ISAAC_VITA_RENDER_CHAIN_NATIVE (host_vita_render_chain_native.c).

1. Frozen inputs: the bodies of sub_0025af60, sub_0055f610, sub_0055f630 and
   the nine size-getter leaves, the vtable slots the replay recognises, and
   the constants it copies, pinned against the frozen PE.
2. Corpus contract: each body defined once, without a seam; neither wrapped
   root has a direct caller inside its own unit (GNU ld --wrap does not see
   those edges); no runtime file or gen_all seam replaces a leaf.
3. Differential oracle (MSVC x86, /LARGEADDRESSAWARE): translated bodies
   against the wrappers, GPR-local and memory-mode bodies.
4. Build gate: default OFF, the two --wrap options and the source only in
   the knob block; the raw allocator gate knows the new file.

usage: test_vita_render_chain_native.py --pe PE --generated-dir DIR [--keep DIR]
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
VITA_CMAKE = HERE / "vita" / "CMakeLists.txt"
sys.path.insert(0, str(HERE))

import build_all  # noqa: E402
from test_vita_kage_quad_fastpath import (  # noqa: E402
    BODY_RE, pe_section_reader, require, run)

PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
# leaf rva -> (disp, movzx word?)  -- `8B 81 d32 C3` / `0F B7 81 d32 C3`
LEAVES = {
    0x005A0BB0: (0x108, False), 0x005A0BC0: (0x10C, False),
    0x005A0DF0: (0x084, True), 0x005A0E00: (0x086, True),
    0x005A1A70: (0x084, False), 0x005A1A80: (0x088, False),
    0x005A1F10: (0x0C8, False), 0x005A1F20: (0x0CC, False),
    0x005B6270: (0x08C, False),
}
ROOTS = ["sub_0025af60", "sub_0055f610"]
BODIES = ROOTS + ["sub_0055f630"] + [f"sub_{r:08x}" for r in LEAVES]
ORACLE_RESULT = ("Vita render chain oracle: PASS; cases=6000; declines=388; "
                 "inlined=2134")
ORACLE_COUNTERS = "counters: f=3000 fd=186 gn=16620 gc=1964 rl=1840 r=3000 ri=2134 rd=202"

COMMON = ["/nologo", "/W4", "/WX", "/O2", "/Gy", "/std:c11",
          "/wd4310", "/wd4702", "/wd4996", "/DGUEST_IMAGE_BASE=0x98000000u"]
DISPATCH_DEFINES = ["/DGUEST_STACK_REQUIRED=1",
                    "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
                    "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                    "/DISAAC_VITA_RENDER_CHAIN_NATIVE=1",
                    "/DGUEST_FLAGS_LOCAL=1"]
BODY_DEFINES = ["/DGUEST_STACK_REQUIRED=1", "/DGUEST_GENERATED_STACK_GUARD=0",
                "/DGUEST_FLAGS_LOCAL=1", "/DGUEST_COVERAGE_HOOKS=0",
                "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
                "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1"]


def verify_frozen(pe_path: Path) -> None:
    pe = pe_path.read_bytes()
    require(hashlib.sha256(pe).hexdigest() == PE_SHA256, "frozen PE changed")
    read = pe_section_reader(pe)
    require(hashlib.sha256(read(0x0025AF60, 0x0025B0E8 - 0x0025AF60)).hexdigest()
            == "e45729fc962067568937c647eaeab66915ebb230be0fb6a3daad2b87c39ad300",
            "sub_0025af60 bytes changed")
    require(read(0x0055F610, 0x19).hex() ==
            "558bec8b45108b11505050" "50ff750cff7508ff52085dc20c00",
            "sub_0055f610 bytes changed")
    require(read(0x0055F630, 0x51).hex() == (
        "558bec83ec248b4508568bf10f10000f1145dc0f1040108b40208945fc0f1145ec"
        "83f801740a56518d4ddce870f5ffffff751c8d45dc8bceff7518ff7514ff7510"
        "ff750c50e8160300005e8be55dc21800"), "sub_0055f630 bytes changed")
    for rva, (disp, word) in LEAVES.items():
        want = (b"\x0f\xb7\x81" if word else b"\x8b\x81") + \
            struct.pack("<I", disp) + b"\xc3"
        require(read(rva, len(want)) == want, f"leaf {rva:#x} changed")
    # The constants the replay copies (operand bytes of the listing).
    for site, raw in ((0x0025AF65, "689d229f00"), (0x0025AF77, "a1b4a3ba00"),
                      (0x0025AF9C, "f30f1005f0edbf00"),
                      (0x0025AFA4, "f30f100d14a6b600"),
                      (0x0025AFB4, "f30f1005f4edbf00"),
                      (0x0025AFC1, "f30f1005f8edbf00"),
                      (0x0025AFCE, "f30f100548d6bf00"),
                      (0x0025B028, "f20f5804c540c9b600"),
                      (0x0025B0C2, "a12cd6bf00"), (0x0025B00F, "ff5030"),
                      (0x0025B045, "ff5034"), (0x0025B0BC, "ffd0"),
                      (0x0025B0CF, "ffd0")):
        require(read(site, len(raw) // 2).hex() == raw, f"operand at {site:#x}")
    text = (RUNTIME / "host_vita_render_chain_native.c").read_text(encoding="utf-8")
    for name, value in (("RC_FILL_SEH_HANDLER", 0x5F229D), ("RC_COOKIE", 0x7AA3B4),
                        ("RC_FILL_C0", 0x7FEDF0), ("RC_FILL_DEFAULT_SIZE", 0x76A614),
                        ("RC_FILL_C1", 0x7FEDF4), ("RC_FILL_C2", 0x7FEDF8),
                        ("RC_FILL_C3", 0x7FD648), ("RC_U32_BIAS_TABLE", 0x76C940),
                        ("RC_RELEASE_OBSERVER", 0x7FD62C),
                        ("RC_RET_FILL_W", 0x25B012), ("RC_RET_FILL_H", 0x25B048),
                        ("RC_RET_FILL_RELEASE", 0x25B0BE),
                        ("RC_RET_FILL_OBSERVER", 0x25B0D1),
                        ("RC_RENDER_UV_RVA", 0x55F630), ("RC_RET_RENDER", 0x55F625),
                        ("RC_RET_UV_TRANSFORM", 0x55F660),
                        ("RC_RET_PUSHQUAD", 0x55F67A)):
        m = re.search(rf"#define {name}\s+0x([0-9A-Fa-f]+)U", text)
        require(m is not None and int(m.group(1), 16) == value,
                f"{name} drifted from the listing")
    for rva, (disp, word) in LEAVES.items():
        require(f"{{ 0x{rva:08X}U, 0x{disp:03X}U, {int(word)}U }}" in text,
                f"leaf table entry for {rva:#x}")
    # The leaves are exactly the +0x30/+0x34 slots of the vtables that hold
    # Image::Render at +0xc (other than the abstract base's purecall).
    image = pe_section_reader(pe)
    found = set()
    blob = b"".join(image(0x0075D000 + i * 0x1000, 0x1000) for i in range(0x10))
    needle = struct.pack("<I", 0x0055F610 + 0x400000)
    at = blob.find(needle)
    while at >= 0:
        if at % 4 == 0:
            vt = 0x0075D000 + at - 0xC
            for off in (0x30, 0x34):
                found.add(struct.unpack_from("<I", image(vt + off, 4))[0] - 0x400000)
        at = blob.find(needle, at + 1)
    require(found - {0x005EC13A} == set(LEAVES), f"vtable leaves {sorted(found)}")


def extract(generated_dir: Path) -> dict[str, str]:
    owners: dict[str, list[Path]] = {name: [] for name in BODIES}
    bodies: dict[str, str] = {}
    texts: dict[Path, str] = {}
    for unit in sorted(generated_dir.glob("guest_[0-9][0-9][0-9][0-9].c")):
        text = unit.read_text(encoding="utf-8")
        texts[unit] = text
        for name in owners:
            if f"\nvoid {name}(CPU *__restrict c)\n" in text:
                owners[name].append(unit)
                for match in BODY_RE.finditer(text):
                    if match.group(1) == name:
                        bodies[name] = match.group(0)
    for name, units in owners.items():
        require(len(units) == 1 and name in bodies, f"{name}: owners {units}")
        require("#if" not in bodies[name], f"{name}: unexpected seam")
    for root in ROOTS:
        own = texts[owners[root][0]]
        require(own.count(f"{root}(c)") == 0,
                f"{root} has a direct caller in its own unit (invisible to --wrap)")
    table = (generated_dir / "guest_table.c").read_text(encoding="utf-8")
    for root in ROOTS:
        require(re.search(rf"\b{root}\b", table) is not None,
                f"{root} not in the dispatch table")
    # Nothing replaces a leaf or a wrapped root at run time.
    names = [f"{r:08x}" for r in LEAVES] + ["0025af60", "0055f610", "0055f630"]
    for path in RUNTIME.glob("*.c"):
        if path.name.startswith("host_vita_render_chain_native"):
            continue
        t = path.read_text(encoding="utf-8", errors="replace").lower()
        for n in names:
            require(f"__wrap_sub_{n}" not in t, f"{path.name} wraps sub_{n}")
    cmake = VITA_CMAKE.read_text(encoding="utf-8").lower()
    for n in names:
        if n in ("0025af60", "0055f610"):
            continue
        require(f"--wrap=sub_{n}" not in cmake, f"CMake wraps sub_{n}")
    return bodies


def bodies_source(bodies: dict[str, str]) -> str:
    source = ["/* test_vita_render_chain_native.py: frozen corpus bodies */",
              '#include "guest.h"',
              "void sub_0055ebd0(CPU *__restrict c);",
              "void sub_0055f990(CPU *__restrict c);"]
    for name in BODIES:
        source.append(f"void {name}(CPU *__restrict c);")
    source += [bodies[name] for name in BODIES]
    return "\n".join(source)


def build_and_run(env, compiler: str, work: Path, bodies: dict[str, str]) -> list[str]:
    results = []
    for flavour, gpr in (("gpr", "1"), ("mem", "0")):
        bodies_c = work / f"rc_bodies_{flavour}.c"
        bodies_c.write_text(bodies_source(bodies), encoding="utf-8", newline="\n")
        obj = work / f"rc_bodies_{flavour}.obj"
        run([compiler, "/nologo", "/W3", "/O2", "/std:c11", "/wd4310",
             "/wd4996", "/DGUEST_IMAGE_BASE=0x98000000u",
             f"/DGUEST_GPR_LOCAL={gpr}"] + BODY_DEFINES +
            [f"/D{name}=real_{name}" for name in ROOTS] +
            ["/I", str(RUNTIME), "/c", str(bodies_c), "/Fo:" + str(obj)], env)
        exe = work / f"rc-oracle-{flavour}.exe"
        objdir = work / f"obj-{flavour}"
        objdir.mkdir()
        run([compiler] + COMMON + DISPATCH_DEFINES + [
            "/I", str(RUNTIME),
            str(RUNTIME / "host_vita_render_chain_native_oracle.c"), str(obj),
            "/Fe:" + str(exe), "/Fo:" + str(objdir) + os.sep,
            "/link", "/LARGEADDRESSAWARE", "/OPT:REF", "/INCREMENTAL:NO"], env)
        completed = subprocess.run([str(exe)], env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT)
        out = completed.stdout.decode("cp866", errors="replace")
        (work / f"oracle-{flavour}.out").write_text(out, encoding="utf-8")
        lines = out.strip().splitlines()
        result = lines[-1] if lines else ""
        if completed.returncode != 0 or result != ORACLE_RESULT or \
                ORACLE_COUNTERS not in lines:
            print(out[-6000:])
            raise AssertionError(
                f"oracle result changed ({flavour}, rc {completed.returncode:#x}): "
                f"{result!r}")
        results.append(f"[{flavour}] {lines[-2]} | {result}")
    return results


def verify_build_gate() -> None:
    cmake = VITA_CMAKE.read_text(encoding="utf-8")
    require(re.search(r"option\(ISAAC_VITA_RENDER_CHAIN_NATIVE\s+\"[^\"]*\"\s+OFF\)",
                      cmake) is not None, "option missing or not default OFF")
    block = re.search(r"\n  if\(ISAAC_VITA_RENDER_CHAIN_NATIVE\)\n(.*?)\n  endif\(\)\n",
                      cmake, re.S)
    require(block is not None, "knob block missing")
    for needle in ('"${ISAAC_RUNTIME}/host_vita_render_chain_native.c"',
                   "-Wl,--wrap=sub_0025af60", "-Wl,--wrap=sub_0055f610",
                   "ISAAC_VITA_RENDER_CHAIN_NATIVE=1"):
        require(needle in block.group(1), f"knob block lacks {needle}")
    for needle in ("--wrap=sub_0025af60", "--wrap=sub_0055f610",
                   "host_vita_render_chain_native.c",
                   "ISAAC_VITA_RENDER_CHAIN_NATIVE=1"):
        require(cmake.count(needle) == block.group(1).count(needle) >= 1,
                f"{needle} must be reachable from the knob block only")
    gate = (HERE / "vita" / "vita_raw_allocator_gate.py").read_text(encoding="utf-8")
    require("host_vita_render_chain_native.c" in gate,
            "vita_raw_allocator_gate.py does not know the new runtime file")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    parser.add_argument("--generated-dir", type=Path, required=True)
    parser.add_argument("--keep", type=Path)
    arguments = parser.parse_args()

    verify_frozen(arguments.pe)
    bodies = extract(arguments.generated_dir)
    verify_build_gate()
    env, compiler = build_all.msvc_env()
    if arguments.keep:
        shutil.rmtree(arguments.keep, ignore_errors=True)
        arguments.keep.mkdir(parents=True)
        results = build_and_run(env, compiler, arguments.keep, bodies)
    else:
        with tempfile.TemporaryDirectory() as tmp:
            results = build_and_run(env, compiler, Path(tmp), bodies)
    for line in results:
        print(line)
    print("Vita render chain native: PASS; pins=3 bodies + 9 leaves; "
          f"oracle flavours={len(results)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
