#!/usr/bin/env python3
"""Proof of ISAAC_VITA_GFUEL_SEED_NATIVE (host_vita_gfuel_seed_native.c).

1. Frozen inputs: the bodies of sub_0039cb10, sub_0039cb80 and sub_000206b0,
   the 0x39cc68 jump table and the two call/return words the replay writes,
   pinned against the frozen PE.
2. Corpus contract: each body defined once; sub_0039cb10 and sub_0039cb80
   share one owner unit, and the only direct edge to either inside it is
   0x39cb8b (GNU ld --wrap does not see edges inside the defining unit).
3. Differential oracle (MSVC x86, /LARGEADDRESSAWARE): the translated bodies
   against the wrapper, GPR-local and memory-mode bodies, pinned result line.
4. Build gate: default OFF, prerequisites, owner census, the two --wrap
   options; OFF adds nothing.

usage: test_vita_gfuel_seed_native.py --pe PE --generated-dir DIR [--keep DIR]
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
from test_vita_kage_quad_fastpath import (  # noqa: E402
    BODY_RE, pe_section_reader, require, run)

PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
# {rva: (end_rva, body_sha256)}, bytes of the frozen PE.
PINS = {
    0x0039CB10: (0x0039CB79,
                 "73cf8ebe4fa3358563f8f6e94e63b09eba211d8f267cc2193b613521c5912e27"),
    0x0039CB80: (0x0039CC67,
                 "9288906e71a0a338c28dbaf9c026abdc1a18969c98f8d748b1842bb9b5362386"),
    0x000206B0: (0x000206FB,
                 "2b170eaee73292815a3552d9e402eb3604ee8cbd95aa3b75b7b472883aeda849"),
}
TABLE_RVA = 0x0039CC68
TABLE_SHA256 = "3bf6583437b2c11f67f734f150753e2d792d4c8a1e5f00db760781b12397f856"
# call site -> (target, return word the replay writes)
CALLS = {0x0039CB37: (0x000206B0, 0x0039CB3C),
         0x0039CB8B: (0x0039CB10, 0x0039CB90)}
RNG_WORDS_RVA = 0x00608B54
BODIES = ["sub_0039cb10", "sub_0039cb80", "sub_000206b0"]
WRAPPED = ("sub_0039cb10", "sub_0039cb80")
ORACLE_RESULT = (
    "Vita G FUEL seed oracle: PASS; cases=4088; handled=3987; declines=101; "
    "decline-probes=10; present=289; walks=3896; logger=170")
# The ph120.gf counters after the run (the oracle checks they sum to its own
# verdicts), and the sub_000206b0 entry counter of the --wrap flavour:
# (4088 - 10 suppressed) reference runs + (101 - 10) declines.
ORACLE_COUNTERS = ("counters: q(n,h)=2029,2021 m(n,h)=2059,1966 w=3801 "
                   "p=1338 d(s,n,w,b)=4,10,2,85")
ORACLE_LOOKUP = "lookup counter: t=4169 o=0 (translated sub_0039cb10 runs 4169)"
# The profiling flavour's timers and site tables on the +1-per-read stub
# clock: wrapper ordinals 0,64,.. timed = ceil(2029/64), ceil(2059/64), empty
# brackets at 32,96,.. = 32 + 32; HasCollectible: 841 entries over 100 call
# sites (top ten = the heaviest twelve minus two), 169 with ignoreModifiers
# (calls % 5 == 0; a 0x100 word does not count), 121 negative ids, 14 timed
# spans of 1 + 7 steps, one of exactly 100 steps (the outlier bound) left
# out; 3000 call sites fill the 1024 slots and the rest is counted;
# sub_000206b0 from two other call sites keeps the translated result.
ORACLE_PROFILE = [
    "wrapper timers: q=32/32/0 m=33/33/0 z=64/64/0",
    "profile has: h(n,i,g)=841,169,121 us(h)=14/104/1 z=13/13 n=100 "
    "top=400037:61,40000a:52",
    "profile overflow: n=1024 x=1976",
    # Nesting (ph120.gn/gb): A = 130 entries whose body calls
    # GetTrinketMultiplier, which calls HasCollectible back, + 10 plain:
    # 270 entries and bodies, 130 nested, 3 top-level spans of 1+3+5+3;
    # B = GetTrinketMultiplier outside (70, spans 1+5 at 0 and 64), from
    # elsewhere inside (5), sub_0039ba90 from HasCollectible (4 nested) and
    # from elsewhere (2 not), GetCollectible from other sites (no body).
    "profile nest A: h(n,x,b)=270,130,270 t(n,h,p)=130,130,0 us(h)=3/36 z=2 "
    "gb=410000:140,38d6c2:130",
    "profile nest B: h(n,x,b)=15,4,15 t(n,h,p)=75,0,70 us(t)=2/12 us(h)=1/9 "
    "z=1 gb=39baac:6,430000:5",
    # C = bodies entered inside the unit (no wrapper): the nested entries
    # are known only through the GetTrinketMultiplier / sub_0039ba90 depth.
    "profile nest C: h(n,x,b)=5,5,10 t(n,h,p)=3,3,0 us(h)=0/0 n=4",
    "profile lookup: o=4 t=1 top=123456:3,654321:1 us(o)=1/1/0",
]

COMMON = ["/nologo", "/W4", "/WX", "/O2", "/Gy", "/std:c11",
          "/wd4310", "/wd4702", "/wd4996", "/DGUEST_IMAGE_BASE=0x98000000u"]
DISPATCH_DEFINES = ["/DGUEST_STACK_REQUIRED=1",
                    "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
                    "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                    "/DISAAC_VITA_PHASE_PROFILE=1",
                    "/DISAAC_VITA_PROFILE_IMPORT_KINDS=1"]
# recomp/vita/CMakeLists.txt compile definitions of a generated unit.
BODY_DEFINES = ["/DGUEST_STACK_REQUIRED=1", "/DGUEST_GENERATED_STACK_GUARD=0",
                "/DGUEST_FLAGS_LOCAL=1", "/DGUEST_COVERAGE_HOOKS=0",
                "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
                "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                "/DISAAC_VITA_PHASE_PROFILE=1",
                "/DISAAC_VITA_PROFILE_IMPORT_KINDS=1"]


def verify_frozen(pe_path: Path) -> bytes:
    pe = pe_path.read_bytes()
    require(hashlib.sha256(pe).hexdigest() == PE_SHA256, "frozen PE changed")
    read = pe_section_reader(pe)
    for rva, (end, sha) in PINS.items():
        require(hashlib.sha256(read(rva, end - rva)).hexdigest() == sha,
                f"body changed at {rva:#x}")
    table = read(TABLE_RVA, 32)
    require(hashlib.sha256(table).hexdigest() == TABLE_SHA256,
            "0x39cc68 jump table changed")
    for site, (target, ret) in CALLS.items():
        b = read(site, 5)
        require(b[0] == 0xE8 and ret == site + 5 and
                site + 5 + int.from_bytes(b[1:], "little", signed=True) ==
                target, f"call identity changed at {site:#x}")
    # The constants the replay copies from the listing.
    text = (RUNTIME / "host_vita_gfuel_seed_native.c").read_text(
        encoding="utf-8")
    for name, value in (("GFUEL_G_GAME", 0x007FD65C),
                        ("GFUEL_SEEDS_SET", 0x0001BB58),
                        ("GFUEL_KEY", 0x4F), ("GFUEL_VEC_BEGIN", 0x1ECC),
                        ("GFUEL_VEC_END", 0x1ED0), ("GFUEL_INDEX", 0x8F),
                        ("GFUEL_RET_LOOKUP", 0x0039CB3C),
                        ("GFUEL_RET_QUERY", 0x0039CB90)):
        m = re.search(rf"#define {name} 0x([0-9A-Fa-f]+)U", text)
        require(m is not None and int(m.group(1), 16) == value,
                f"{name} drifted from the listing")
    for word, what in ((0x007FD65C, "mov ecx, dword ptr [0x987fd65c]"),):
        require(read(0x0039CB20, 6) == b"\x8b\x0d" +
                (0x00400000 + word).to_bytes(4, "little"), what)
    require(read(0x0039CB19, 7) == bytes.fromhex("c745fc4f000000"),
            "the G FUEL key 0x4f moved")
    require(read(0x0039CB2A, 6) == bytes.fromhex("81c1" "58bb0100"),
            "the seed set offset 0x1bb58 moved")
    return table + read(RNG_WORDS_RVA, 12)


def extract(generated_dir: Path) -> dict[str, str]:
    owners: dict[str, list[Path]] = {name: [] for name in BODIES}
    bodies: dict[str, str] = {}
    for unit in sorted(generated_dir.glob("guest_[0-9][0-9][0-9][0-9].c")):
        text = unit.read_text(encoding="utf-8")
        for name in owners:
            if f"\nvoid {name}(CPU *__restrict c)\n" in text:
                owners[name].append(unit)
                for match in BODY_RE.finditer(text):
                    if match.group(1) == name:
                        bodies[name] = match.group(0)
    for name, units in owners.items():
        require(len(units) == 1 and name in bodies, f"{name}: owners {units}")
    require(owners["sub_0039cb10"] == owners["sub_0039cb80"],
            "the two wrapped roots must share their owner unit")
    owner_text = owners["sub_0039cb10"][0].read_text(encoding="utf-8")
    inside = re.findall(r"/\* ([0-9a-f]{8})  (?:call|jmp) 0x39cb(?:10|80) \*/",
                        owner_text)
    require(inside == ["0039cb8b"],
            f"edges inside the owner unit (invisible to --wrap): {inside}")
    for name in BODIES:
        require("#if defined(__vita__)" not in bodies[name],
                f"{name}: unexpected seam in the frozen body")
    return bodies


def bodies_source(bodies: dict[str, str], lookup_wrap: bool) -> str:
    source = ["/* test_vita_gfuel_seed_native.py: frozen corpus bodies */",
              '#include "guest.h"',
              "void sub_0055e330(CPU *__restrict c);"]
    for name in BODIES:
        source.append(f"void {name}(CPU *__restrict c);")
    texts = dict(bodies)
    if lookup_wrap:
        # The --wrap=sub_000206b0 view of the profiling build: the call at
        # 0x39cb37 (another unit) reaches the counter, the body is __real_.
        # A GUEST_GPR_FLUSH_MASK=1 corpus may spell the publication as a
        # mask (recomp/gpr_flush_mask.py); the wrapped call keeps it as is.
        call = re.compile(r"(GUEST_GPR_FLUSH\(c\);|"
                          r"GUEST_GPR_FLUSH_MASK\(c, 0x[0-9a-f]{2}U\);) "
                          r"sub_000206b0\(c\);")
        require(len(call.findall(texts["sub_0039cb10"])) == 1,
                "sub_0039cb10 no longer calls sub_000206b0 once")
        texts["sub_0039cb10"] = call.sub(
            lambda m: m.group(1) + " __wrap_sub_000206b0(c);",
            texts["sub_0039cb10"])
        head = "\nvoid sub_000206b0(CPU *__restrict c)\n"
        require(texts["sub_000206b0"].count(head) == 1, "sub_000206b0 header")
        texts["sub_000206b0"] = texts["sub_000206b0"].replace(
            head, "\nvoid __real_sub_000206b0(CPU *__restrict c)\n")
        source += ["void __wrap_sub_000206b0(CPU *__restrict c);",
                   "void __real_sub_000206b0(CPU *__restrict c);"]
    source += [texts[name] for name in BODIES]
    return "\n".join(source)


def build_and_run(env, compiler: str, work: Path, bodies: dict[str, str],
                  frozen: bytes) -> list[str]:
    results = []
    flavours = [("gpr", "1", False), ("mem", "0", False),
                ("gpr-profile", "1", True)]
    if any("GUEST_GPR_FLUSH_MASK(c, 0x" in text for text in bodies.values()):
        # GUEST_GPR_FLUSH_MASK=1 corpus: also run the masked publications
        # (header knob GUEST_GPR_FLUSH_MASKED=1, the CMake knob's define).
        flavours += [("gpr-masked", "1", False),
                     ("gpr-masked-profile", "1", True)]
    for flavour, gpr, lookup in flavours:
        masked = ["/DGUEST_GPR_FLUSH_MASKED=1"] if "masked" in flavour else []
        bodies_c = work / f"gfuel_bodies_{flavour}.c"
        bodies_c.write_text(bodies_source(bodies, lookup), encoding="utf-8",
                            newline="\n")
        obj = work / f"gfuel_bodies_{flavour}.obj"
        run([compiler, "/nologo", "/W3", "/O2", "/std:c11", "/wd4310",
             "/wd4996", "/DGUEST_IMAGE_BASE=0x98000000u",
             f"/DGUEST_GPR_LOCAL={gpr}"] + masked + BODY_DEFINES +
            [f"/D{name}=real_{name}" for name in WRAPPED] +
            ["/I", str(RUNTIME), "/c", str(bodies_c), "/Fo:" + str(obj)], env)
        exe = work / f"gfuel-oracle-{flavour}.exe"
        objdir = work / f"obj-{flavour}"
        objdir.mkdir()
        run([compiler] + COMMON + DISPATCH_DEFINES +
            (["/DISAAC_VITA_GFUEL_SEED_NATIVE_PROFILE=1"] if lookup else []) + [
            "/I", str(RUNTIME),
            str(RUNTIME / "host_vita_gfuel_seed_native_oracle.c"), str(obj),
            "/Fe:" + str(exe), "/Fo:" + str(objdir) + os.sep,
            "/link", "/LARGEADDRESSAWARE", "/OPT:REF", "/INCREMENTAL:NO"], env)
        completed = subprocess.run([str(exe), frozen.hex()], env=env,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT)
        out = completed.stdout.decode("cp866", errors="replace")
        (work / f"oracle-{flavour}.out").write_text(out, encoding="utf-8")
        lines = out.strip().splitlines()
        result = lines[-1] if lines else ""
        if (completed.returncode != 0 or result != ORACLE_RESULT or
                ORACLE_COUNTERS not in lines or
                (ORACLE_LOOKUP in lines) != lookup or
                any((line in lines) != lookup for line in ORACLE_PROFILE)):
            print(out[-6000:])
            raise AssertionError(
                f"oracle result changed ({flavour}, rc "
                f"{completed.returncode:#x}): {result!r}")
        results.append(f"[{flavour}] {result}")
    return results


def verify_build_gate() -> None:
    cmake = VITA_CMAKE.read_text(encoding="utf-8")
    require(re.search(r"option\(ISAAC_VITA_GFUEL_SEED_NATIVE\s+\"[^\"]*\"\s+OFF\)",
                      cmake) is not None, "option missing or not default OFF")
    require("ISAAC_VITA_GFUEL_SEED_NATIVE requires ISAAC_VITA_TRANSLATED_CPU"
            in cmake and
            "ISAAC_VITA_GFUEL_SEED_NATIVE requires generated guest runtime"
            in cmake, "prerequisite gates missing")
    block = re.search(r"\n  if\(ISAAC_VITA_GFUEL_SEED_NATIVE\)\n(.*?)\n  endif\(\)\n",
                      cmake, re.S)
    require(block is not None, "knob block missing")
    body = block.group(1)
    for needle in ('"${ISAAC_RUNTIME}/host_vita_gfuel_seed_native.c"',
                   "-Wl,--wrap=sub_0039cb10", "-Wl,--wrap=sub_0039cb80",
                   "ISAAC_GFUEL_OWNER_COUNT EQUAL 1",
                   "ISAAC_VITA_GFUEL_SEED_NATIVE=1"):
        require(needle in body, f"knob block lacks {needle}")
    # The counting/timing seams: profiling builds only.
    profiled = re.search(r"\n    if\(ISAAC_VITA_PHASE_PROFILE\)\n(.*?)\n    endif\(\)\n",
                         body, re.S)
    profiling_wraps = ("sub_000206b0", "sub_0033af30", "sub_002fe5e0",
                       "sub_0038d4c0", "sub_0039ba90")
    require(profiled is not None and
            all(f"-Wl,--wrap={s}" in profiled.group(1) for s in profiling_wraps) and
            "ISAAC_VITA_GFUEL_SEED_NATIVE_PROFILE=1" in profiled.group(1),
            "the profiling seams must sit under ISAAC_VITA_PHASE_PROFILE")
    # ISAAC_VITA_HASCOLLECTIBLE_NATIVE's block (test_vita_hascoll_native.py)
    # passes its own --wrap=sub_0033af30 unless this profiling block owns the
    # wrapper, and then defines its knob on this TU (the chained wrapper).
    hascoll = re.search(
        r"\n  if\(ISAAC_VITA_HASCOLLECTIBLE_NATIVE\)\n(.*?)\n  endif\(\)\n",
        cmake, re.S)
    other = hascoll.group(1) if hascoll else ""
    require(cmake.count("host_vita_gfuel_seed_native.c") -
            other.count("host_vita_gfuel_seed_native.c") == 2 and
            cmake.count("--wrap=sub_0039cb10") == 1 and
            cmake.count("--wrap=sub_0039cb80") == 1 and
            all(cmake.count(f"--wrap={s}") - other.count(f"--wrap={s}") == 1
                for s in profiling_wraps),
            "the seam must be reachable from the knob block only")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    parser.add_argument("--generated-dir", type=Path, required=True)
    parser.add_argument("--keep", type=Path)
    arguments = parser.parse_args()

    frozen = verify_frozen(arguments.pe)
    bodies = extract(arguments.generated_dir)
    verify_build_gate()
    env, compiler = build_all.msvc_env()
    if arguments.keep:
        shutil.rmtree(arguments.keep, ignore_errors=True)
        arguments.keep.mkdir(parents=True)
        results = build_and_run(env, compiler, arguments.keep, bodies, frozen)
    else:
        with tempfile.TemporaryDirectory() as tmp:
            results = build_and_run(env, compiler, Path(tmp), bodies, frozen)
    for line in results:
        print(line)
    print("Vita G FUEL seed native: PASS; pins=3 bodies + table; "
          f"oracle flavours={len(results)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
