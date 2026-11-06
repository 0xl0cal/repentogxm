#!/usr/bin/env python3
"""Proof of ISAAC_VITA_HASCOLLECTIBLE_NATIVE (host_vita_hascoll_native.c).

1. Frozen inputs: the bodies of Entity_Player::HasCollectible (sub_0033af30)
   and the ten callees the native replay reproduces, the item predicate's
   tables, the G FUEL mode table and the RNG words the declined paths read,
   pinned against the frozen PE.
2. Cited constants: every `#define HC_* 0x..U` in the replay names the
   instruction it was copied from; the instruction must be in the corpus
   listing comments, verbatim, and must contain the value.
3. Corpus contract: sub_0033af30 defined once; no jmp (tail call) to it
   anywhere; its direct call sites census (inside the owner unit GNU ld
   --wrap does not see them); the dispatch table references it; no other
   runtime file defines a HasCollectible wrapper except the G FUEL
   profiling TU (chained).
4. Differential oracle (MSVC x86, /LARGEADDRESSAWARE): the translated body
   against the wrapper on random and targeted player states, GPR-local and
   memory-mode bodies, and the chained G FUEL profiling wrapper; branch
   coverage on handled calls, every decline reason, the deepest stack write.
5. Mutation test: each cited constant changed in turn (offset +4, id +1, bit
   <<1, return word +1) must make the oracle fail; the three equivalent
   mutants are listed with their reasons (EQUIVALENT).
6. Build gate: default OFF, prerequisites, census, the --wrap (or the
   chained profiling wrapper), the phase-profile record, the raw allocator
   gate; OFF adds nothing.

usage: test_vita_hascoll_native.py --pe PE --generated-dir DIR [--keep DIR]
       [--no-mutants]
"""
from __future__ import annotations

import argparse
import concurrent.futures
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
NATIVE = RUNTIME / "host_vita_hascoll_native.c"
ORACLE = RUNTIME / "host_vita_hascoll_native_oracle.c"
sys.path.insert(0, str(HERE))

import build_all  # noqa: E402
from test_vita_kage_quad_fastpath import (  # noqa: E402
    BODY_RE, pe_section_reader, require, run)

PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
# {rva: (end_rva, body_sha256)}, bytes of the frozen PE (end = after the
# last ret, from the corpus listings).
PINS = {
    0x0033AF30: (0x0033B3A4,
                 "569453e98ab352d63008047cf8269ed79de2fae6fbf60682a5246be3a7612de4"),
    0x002FE5E0: (0x002FE632,
                 "a85a4a62030b525c7d2998aa9c19b20bd38ea8e14933a7fc82bc07953c44f1f1"),
    0x002FF070: (0x002FF0B9,
                 "3e53f6604d478861f6339a846ba360e96d07e6cadaeac6240c190dec884304c4"),
    0x0048B570: (0x0048B5B7,
                 "47610c1571df0cad3ea59103e9e539b7512f1c0430ac8be1278c9a6ba95750c2"),
    0x00535FA0: (0x00535FC7,
                 "e88f4e6bc7aab06138498339a4cc8875170d4f602c0fdfa5bdbfba64f04205ea"),
    0x0019FCE0: (0x0019FD05,
                 "ef3865af997a6e79bd82dc0ea414e7c7d7bfd2d76d3e45392bcce0634f7ff013"),
    0x0019FF20: (0x0019FF44,
                 "f6f25dbe778660f803177311b26eddccd4aea1b3c4146af2ab898d391c2d96d1"),
    0x00144990: (0x001449D3,
                 "58ceff334a825a60c5c6090c1392b3e64fc637ccda3bfca4bf5bcedcd982cd42"),
    # the three G FUEL pins of test_vita_gfuel_seed_native.py
    0x000206B0: (0x000206FB,
                 "2b170eaee73292815a3552d9e402eb3604ee8cbd95aa3b75b7b472883aeda849"),
    0x0039CB10: (0x0039CB79,
                 "73cf8ebe4fa3358563f8f6e94e63b09eba211d8f267cc2193b613521c5912e27"),
    0x0039CB80: (0x0039CC67,
                 "9288906e71a0a338c28dbaf9c026abdc1a18969c98f8d748b1842bb9b5362386"),
}
# (rva, bytes) handed to the oracle, in its install_frozen order.
FROZEN = [(0x002FF0BC, 8), (0x002FF0C4, 200), (0x0039CC68, 32),
          (0x00608AD0, 12), (0x00608B00, 12), (0x00608B54, 12),
          (0x0074D554, 24)]
FROZEN_SHA256 = "c8a8377e4e771636e1e391e40e5335add62e13cfc029338f02bdc495c5a3de89"
# The replayed bodies (the oracle compares them) and the ones only the
# declined paths reach (the translated reference runs them).
REPLAYED = ["sub_0033af30", "sub_002fe5e0", "sub_002ff070", "sub_0048b570",
            "sub_00535fa0", "sub_0019fce0", "sub_0019ff20", "sub_00144990",
            "sub_000206b0", "sub_0039cb10", "sub_0039cb80"]
DECLINED_ONLY = ["sub_00012750", "sub_00535e00", "sub_0048b600",
                 "sub_003a7d50", "sub_0039ba90", "sub_000277e0"]
BODIES = REPLAYED + DECLINED_ONLY
# sub_0039ba90 lives in another unit than HasCollectible: its call reaches
# the wrapper (--wrap), the oracle routes it by run.
OTHER_UNIT_CALLERS = {"sub_0039ba90": 1}
# The corpus census (comments `/* xxxxxxxx  call 0x33af30 */`).
CENSUS = "census: call sites=2798 owner-unit=120 in-body=2 tail-jumps=0"

ORACLE_RESULT = (
    'Vita HasCollectible oracle: PASS; cases=48912; handled=40518; declined'
    '=8394; fault-probes=144; true=27227; gtm=5196; logger=0')
ORACLE_LINES: list[str] = [
    ('coverage (handled calls): 41/41 markers, rarest 14: 2871 49686 2538 '
     '7202 4936 9574 8115 8084 4278 10451 3576 1107 7970 591 4493 2306 392'
     '7 2274 1900 5777 400 430 428 438 14 810 15 571 3881 3679 4321 2146 2'
     '132 10451 5410 1767 1261 7309 1264 1304 18070'),
    'verdicts: d(s,c,i,g,t,n,w,x) cases=128,1312,4371,523,573,96,128,16',
    ('counters: n=50184 h=41661 b=54620 y=22619 d=128,1438,5527,557,633,96'
     ',128,16'),
    'deepest handled write: e-0xbc (bound 0xd0)',
]
# The mutants no state can kill (see mutate()), each with its reason.
EQUIVALENT = {
    "HC_GFUEL_MAX": "0x33b1dd `cmp eax, 8` runs after sub_0039cb80, whose "
                    "replay declines every byte above 7 (HC_GF_MODE_MAX); "
                    "the query returns that same byte, so 8 and 9 cannot "
                    "differ",
    "HC_RET_PRED": "the return word of 0x33b320 `call 0x2ff070` lands at "
                   "[s-4], and both paths after the call push ebx there "
                   "again (0x33b329 before the effect call, 0x33b339 before "
                   "the slot call); a TABLE decline re-runs the translated "
                   "body, so the word never survives to the final state",
    "HC_TAGS_HI": "sub_0019fce0 ands [item+0xbc] with its hi argument, and "
                  "both call sites push 0 for it (0x33b265, 0x33b2a2): "
                  "any in-object word reads the same zero",
}

COMMON = ["/nologo", "/W4", "/WX", "/O2", "/Gy", "/std:c11",
          "/wd4310", "/wd4702", "/wd4996", "/DGUEST_IMAGE_BASE=0x98000000u"]
DISPATCH_DEFINES = ["/DGUEST_STACK_REQUIRED=1",
                    "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
                    "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                    "/DISAAC_VITA_HASCOLLECTIBLE_NATIVE=1",
                    "/DGUEST_FLAGS_LOCAL=1"]
# recomp/vita/CMakeLists.txt compile definitions of a generated unit.
BODY_DEFINES = ["/DGUEST_STACK_REQUIRED=1", "/DGUEST_GENERATED_STACK_GUARD=0",
                "/DGUEST_FLAGS_LOCAL=1", "/DGUEST_COVERAGE_HOOKS=0",
                "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
                "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1"]
GFUEL_RENAMES = ["/D__real_sub_0039cb10=sub_0039cb10",
                 "/D__real_sub_0039cb80=sub_0039cb80",
                 "/D__real_sub_000206b0=sub_000206b0",
                 "/D__real_sub_002fe5e0=sub_002fe5e0",
                 "/D__real_sub_0038d4c0=sub_0038d4c0",
                 "/D__real_sub_0039ba90=sub_0039ba90",
                 "/D__real_sub_0033af30=oracle_real_hc"]

DEFINE_RE = re.compile(
    r"^#define (HC_\w+) +0x([0-9A-F]+)U( +/\* ([0-9a-f]{8})  (.+?) \*/)?$", re.M)


# ---- 1. frozen inputs -------------------------------------------------------

def verify_frozen(pe_path: Path) -> bytes:
    pe = pe_path.read_bytes()
    require(hashlib.sha256(pe).hexdigest() == PE_SHA256, "frozen PE changed")
    read = pe_section_reader(pe)
    for rva, (end, sha) in PINS.items():
        require(hashlib.sha256(read(rva, end - rva)).hexdigest() == sha,
                f"body changed at {rva:#x}")
    blob = b"".join(read(rva, size) for rva, size in FROZEN)
    require(len(blob) == sum(size for _, size in FROZEN), "short frozen read")
    if FROZEN_SHA256 != "filled-in-below":
        require(hashlib.sha256(blob).hexdigest() == FROZEN_SHA256,
                "frozen tables changed")
    # the predicate's two targets and the 0x74d554 ids, as the replay reads
    targets = [int.from_bytes(blob[i:i + 4], "little") for i in (0, 4)]
    require(targets == [0x006FF0B6, 0x006FF0B3],
            f"item predicate targets moved: {[hex(t) for t in targets]}")
    require(set(blob[8:208]) <= {0, 1}, "item predicate byte table grew")
    return blob


# ---- 2. cited constants -------------------------------------------------------

def numbers_in(text: str) -> set[int]:
    out: set[int] = set()
    for sign, hexv in re.findall(r"(-\s*)?0x([0-9a-f]+)", text):
        v = int(hexv, 16)
        if sign:
            v = (-v) & 0xFFFFFFFF
        out.add(v)
        if v >= 0x98000000:
            out.add(v - 0x98000000)
    for dec in re.findall(r"(?<![0-9a-fx])(\d+)(?![0-9a-fx])", text):
        out.add(int(dec))
    return out


def verify_citations(bodies: dict[str, str]) -> int:
    text = NATIVE.read_text(encoding="utf-8")
    corpus = "\n".join(bodies.values())
    count = 0
    for m in DEFINE_RE.finditer(text):
        name, value = m.group(1), int(m.group(2), 16)
        if name == "HC_STACK_BYTES":
            require(m.group(3) is None, "HC_STACK_BYTES is a bound, not a copy")
            continue
        require(m.group(3) is not None, f"{name} cites no instruction")
        addr, insn = m.group(4), m.group(5)
        comment = f"/* {addr}  {insn} */"
        require(comment in corpus, f"{name}: {comment} is not in the corpus")
        site = int(addr, 16)
        candidates = numbers_in(insn) | {site}
        if insn.startswith("call "):
            candidates.add(site + 5)
        require(value in candidates,
                f"{name} = {value:#x} is not in `{insn}` at {addr}")
        count += 1
    require(count >= 80, f"only {count} cited constants")
    return count


# ---- 3. corpus contract -------------------------------------------------------

def extract(generated_dir: Path) -> tuple[dict[str, str], str]:
    owners: dict[str, list[Path]] = {name: [] for name in BODIES}
    bodies: dict[str, str] = {}
    sites = owner_sites = tail = 0
    owner_unit = None
    texts = {}
    for unit in sorted(generated_dir.glob("guest_[0-9][0-9][0-9][0-9].c")):
        t = unit.read_text(encoding="utf-8")
        texts[unit] = t
        for name in owners:
            if f"\nvoid {name}(CPU *__restrict c)\n" in t:
                owners[name].append(unit)
                for match in BODY_RE.finditer(t):
                    if match.group(1) == name:
                        bodies[name] = match.group(0)
    for name, units in owners.items():
        require(len(units) == 1 and name in bodies, f"{name}: owners {units}")
    owner_unit = owners["sub_0033af30"][0]
    for unit, t in texts.items():
        n = len(re.findall(r"/\* [0-9a-f]{8}  call 0x33af30 \*/", t))
        sites += n
        if unit == owner_unit:
            owner_sites = n
        tail += len(re.findall(r"/\* [0-9a-f]{8}  j[a-z]+ 0x33af30 \*/", t))
    in_body = re.findall(r"/\* ([0-9a-f]{8})  call 0x33af30 \*/",
                         bodies["sub_0033af30"])
    require(in_body == ["0033b284", "0033b356"],
            f"HasCollectible's own calls moved: {in_body}")
    census = (f"census: call sites={sites} owner-unit={owner_sites} "
              f"in-body={len(in_body)} tail-jumps={tail}")
    require(census == CENSUS, f"corpus census changed: {census}")
    for name in BODIES:
        require("#if" not in bodies[name], f"{name}: preprocessor seam in the body")
    table = (generated_dir / "guest_table.c").read_text(encoding="utf-8")
    require(re.search(r"\bsub_0033af30\b", table) is not None,
            "sub_0033af30 not in the dispatch table")
    for name, n in OTHER_UNIT_CALLERS.items():
        require(owners[name][0] != owner_unit and
                bodies[name].count("sub_0033af30(c)") == n,
                f"{name}: expected {n} other-unit HasCollectible call(s)")
    for path in RUNTIME.glob("*.c"):
        if path.name.startswith(("host_vita_hascoll_native",
                                 "host_vita_gfuel_seed_native")):
            continue
        t = path.read_text(encoding="utf-8", errors="replace")
        require("__wrap_sub_0033af30" not in t, f"{path.name} wraps HasCollectible")
    return bodies, census


def bodies_source(bodies: dict[str, str]) -> str:
    source = ["/* test_vita_hascoll_native.py: frozen corpus bodies */",
              '#include "guest.h"',
              "void sub_0055e330(CPU *__restrict c);",
              "void sub_0038d4c0(CPU *__restrict c);",
              "void oracle_hc_other_unit(CPU *__restrict c);"]
    for name in BODIES:
        source.append(f"void {name}(CPU *__restrict c);")
    for name in BODIES:
        text = bodies[name]
        if name in OTHER_UNIT_CALLERS:
            text = text.replace("sub_0033af30(c)", "oracle_hc_other_unit(c)")
        source.append(text)
    return "\n".join(source)


# ---- 4. oracle ----------------------------------------------------------------

def coverage_markers() -> int:
    text = NATIVE.read_text(encoding="utf-8")
    marks = [int(n) for n in re.findall(r"\bHC_COV\((\d+)\);", text)]
    require(sorted(marks) == list(range(len(marks))),
            f"HC_COV markers are not 0..n-1 once each: {sorted(marks)}")
    return len(marks)


def stack_bound() -> int:
    m = re.search(r"^#define HC_STACK_BYTES 0x([0-9A-F]+)U$",
                  NATIVE.read_text(encoding="utf-8"), re.M)
    require(m is not None, "HC_STACK_BYTES missing")
    return int(m.group(1), 16)


class Build:
    def __init__(self, env, compiler: str, work: Path, bodies: dict[str, str]):
        self.env, self.cl, self.work = env, compiler, work
        self.markers, self.bound = coverage_markers(), stack_bound()
        src = work / "hc_bodies.c"
        src.write_text(bodies_source(bodies), encoding="utf-8", newline="\n")
        self.bodies = {}
        for flavour, gpr in (("gpr", "1"), ("mem", "0")):
            obj = work / f"hc_bodies_{flavour}.obj"
            run([compiler, "/nologo", "/W3", "/O2", "/std:c11", "/wd4310",
                 "/wd4996", "/DGUEST_IMAGE_BASE=0x98000000u",
                 f"/DGUEST_GPR_LOCAL={gpr}"] + BODY_DEFINES +
                ["/Dsub_0033af30=real_sub_0033af30",
                 "/I", str(RUNTIME), "/c", str(src), "/Fo:" + str(obj)], env)
            self.bodies[flavour] = obj
        self.oracle = {}
        for chained in (False, True):
            obj = work / f"hc_oracle{'_chained' if chained else ''}.obj"
            run([self.cl] + COMMON + DISPATCH_DEFINES +
                [f"/DHC_COV_MARKERS={self.markers}",
                 f"/DHC_ORACLE_STACK_BYTES={self.bound:#x}u"] +
                # The chained oracle takes the counters of the G FUEL
                # profiling TU (self.gfuel below), so it must see the same
                # IsaacVitaGfuelSeedCounters layout: be5bfd5 made the
                # ph120.gn/gb members exist only under
                # ISAAC_VITA_GFUEL_SEED_NATIVE_PROFILE / _PHASE_PROFILE.
                (["/DHC_ORACLE_CHAINED=1",
                  "/DISAAC_VITA_GFUEL_SEED_NATIVE_PROFILE=1"]
                 if chained else []) +
                ["/I", str(RUNTIME), "/c", str(ORACLE), "/Fo:" + str(obj)], env)
            self.oracle[chained] = obj
        self.gfuel = work / "gfuel_chained.obj"
        run([self.cl] + COMMON + DISPATCH_DEFINES +
            ["/DISAAC_VITA_GFUEL_SEED_NATIVE_PROFILE=1"] + GFUEL_RENAMES +
            ["/I", str(RUNTIME), "/c",
             str(RUNTIME / "host_vita_gfuel_seed_native.c"),
             "/Fo:" + str(self.gfuel)], env)

    def native(self, source: Path, tag: str, chained: bool) -> Path:
        obj = self.work / f"hc_native_{tag}.obj"
        run([self.cl] + COMMON + DISPATCH_DEFINES +
            ["/DHC_ORACLE_COVERAGE=1", "/D__real_sub_0033af30=oracle_real_hc"] +
            (["/DISAAC_VITA_HASCOLL_NATIVE_CHAINED=1"] if chained else []) +
            ["/I", str(RUNTIME), "/c", str(source), "/Fo:" + str(obj)], self.env)
        return obj

    def link(self, tag: str, flavour: str, native_obj: Path, chained: bool) -> Path:
        exe = self.work / f"hc-oracle-{tag}.exe"
        objs = [str(self.oracle[chained]), str(self.bodies[flavour]), str(native_obj)]
        if chained:
            objs.append(str(self.gfuel))
        run([self.cl, "/nologo"] + objs +
            ["/Fe:" + str(exe), "/link", "/LARGEADDRESSAWARE", "/OPT:REF",
             "/INCREMENTAL:NO"], self.env)
        return exe


def run_oracle(exe: Path, blob: bytes, env, fast: bool, timeout: int):
    args = [str(exe), blob.hex()] + (["fast"] if fast else [])
    try:
        completed = subprocess.run(args, env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "timeout"
    return completed.returncode, completed.stdout.decode("cp866", errors="replace")


def oracle_flavours(build: Build, blob: bytes) -> list[str]:
    results = []
    plain = build.native(NATIVE, "plain", False)
    chained = build.native(NATIVE, "chained", True)
    for flavour, obj, is_chained in (("gpr", plain, False), ("mem", plain, False),
                                     ("gpr-chained", chained, True)):
        exe = build.link(flavour, flavour.split("-")[0], obj, is_chained)
        rc, out = run_oracle(exe, blob, build.env, False, 600)
        (build.work / f"oracle-{flavour}.out").write_text(out, encoding="utf-8")
        lines = out.strip().splitlines()
        result = lines[-1] if lines else ""
        print(f"[{flavour}] rc={rc}")
        for line in lines[-6:]:
            print(f"  {line}")
        if rc != 0 or not result.startswith("Vita HasCollectible oracle: PASS"):
            print(out[-8000:])
            raise AssertionError(f"oracle failed ({flavour}, rc {rc}): {result!r}")
        if ORACLE_RESULT != "filled-in-below":
            require(result == ORACLE_RESULT, f"oracle result changed: {result!r}")
            for line in ORACLE_LINES:
                require(line in lines, f"oracle line missing: {line!r}")
        results.append(f"[{flavour}] {result}")
    return results


# ---- 5. mutation test --------------------------------------------------------

def mutate(name: str, value: int) -> int:
    if name.startswith(("HC_RET_", "HC_GF_RET_")):
        return value + 1
    if name.startswith(("HC_BIT_", "HC_TAG_")):
        return (value << 1) & 0xFFFFFFFF or 1
    if name.startswith(("HC_ID_", "HC_TYPE_")) or name in (
            "HC_PRED_HIGH", "HC_PRED_1FC", "HC_PRED_2DC", "HC_PRED_BASE",
            "HC_PRED_SPAN", "HC_GF_KEY", "HC_GF_INDEX", "HC_GF_MODE_MAX",
            "HC_GFUEL_MAX", "HC_SLOT_COUNT"):
        return value + 1
    return value + 4


def mutation_test(build: Build, blob: bytes) -> str:
    text = NATIVE.read_text(encoding="utf-8")
    defines = [(m.group(1), int(m.group(2), 16), m.start(), m.end())
               for m in DEFINE_RE.finditer(text)]
    mutdir = build.work / "mutants"
    mutdir.mkdir()

    def one(item):
        name, value, start, end = item
        line = text[start:end]
        new = mutate(name, value)
        mutated = (text[:start] +
                   re.sub(r"0x[0-9A-F]+U", f"0x{new:X}U", line, count=1) +
                   text[end:])
        require(mutated != text, f"{name}: mutation did not apply")
        src = mutdir / f"{name}.c"
        src.write_text(mutated, encoding="utf-8", newline="\n")
        obj = build.native(src, f"mut_{name}", False)
        exe = build.link(f"mut_{name}", "gpr", obj, False)
        rc, out = run_oracle(exe, blob, build.env, True, 300)
        killed = rc != 0
        tail = out.strip().splitlines()[:1] if out else []
        return name, value, new, killed, (tail[0] if tail else str(rc))

    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        outcomes = list(pool.map(one, defines))
    survivors = []
    for name, value, new, killed, first in outcomes:
        print(f"  mutant {name} {value:#x}->{new:#x}: "
              f"{'killed' if killed else 'SURVIVED'} ({first[:90]})")
        if not killed:
            survivors.append(name)
    unexpected = [s for s in survivors if s not in EQUIVALENT]
    stale = [s for s in EQUIVALENT if s not in survivors]
    require(not unexpected, f"mutants survived: {unexpected}")
    require(not stale, f"listed as equivalent but killed: {stale}")
    for s in survivors:
        print(f"  equivalent: {s}: {EQUIVALENT[s]}")
    return (f"mutants={len(outcomes)} killed={len(outcomes) - len(survivors)} "
            f"equivalent={len(survivors)}")


# ---- 6. build gate -------------------------------------------------------------

def verify_build_gate() -> None:
    cmake = VITA_CMAKE.read_text(encoding="utf-8")
    require(re.search(r"option\(ISAAC_VITA_HASCOLLECTIBLE_NATIVE\s+\"[^\"]*\"\s+OFF\)",
                      cmake) is not None, "option missing or not default OFF")
    require("ISAAC_VITA_HASCOLLECTIBLE_NATIVE requires the generated guest runtime"
            in cmake, "prerequisite gate missing")
    block = re.search(r"\n  if\(ISAAC_VITA_HASCOLLECTIBLE_NATIVE\)\n(.*?)\n  endif\(\)\n",
                      cmake, re.S)
    require(block is not None, "knob block missing")
    body = block.group(1)
    for needle in ('"${ISAAC_RUNTIME}/host_vita_hascoll_native.c"',
                   "-Wl,--wrap=sub_0033af30",
                   "ISAAC_VITA_HASCOLL_NATIVE_CHAINED=1",
                   "ISAAC_VITA_HASCOLLECTIBLE_NATIVE=1",
                   "GUEST_FLAGS_LOCAL=1",
                   "ISAAC_HASCOLL_DEFINITIONS EQUAL 1",
                   "ISAAC_VITA_GFUEL_SEED_NATIVE AND ISAAC_VITA_PHASE_PROFILE"):
        require(needle in body, f"knob block lacks {needle}")
    for needle in ("host_vita_hascoll_native.c", "ISAAC_VITA_HASCOLL_NATIVE_CHAINED=1"):
        require(cmake.count(needle) == body.count(needle) >= 1,
                f"{needle} must be reachable from the knob block only")
    # one --wrap here, one in the G FUEL profiling block, never both linked
    require(cmake.count("--wrap=sub_0033af30") == 2 and
            body.count("--wrap=sub_0033af30") == 1,
            "--wrap=sub_0033af30: the knob block and the G FUEL profiling block only")
    gate = (HERE / "vita" / "vita_raw_allocator_gate.py").read_text(encoding="utf-8")
    require('("ISAAC_VITA_HASCOLLECTIBLE_NATIVE", "host_vita_hascoll_native.c")' in gate,
            "vita_raw_allocator_gate.py does not know the new runtime file")
    readme = (HERE / "vita" / "README.md").read_text(encoding="utf-8")
    require("ISAAC_VITA_HASCOLLECTIBLE_NATIVE" in readme, "README entry missing")
    gfuel = (RUNTIME / "host_vita_gfuel_seed_native.c").read_text(encoding="utf-8")
    require(gfuel.count("ISAAC_VITA_GFUEL_HAS_BODY(c)") == 2 and
            "__real_sub_0033af30(c);" not in gfuel,
            "the G FUEL profiling wrapper must reach the body through "
            "ISAAC_VITA_GFUEL_HAS_BODY")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    parser.add_argument("--generated-dir", type=Path, required=True)
    parser.add_argument("--keep", type=Path)
    parser.add_argument("--no-mutants", action="store_true")
    arguments = parser.parse_args()

    blob = verify_frozen(arguments.pe)
    bodies, census = extract(arguments.generated_dir)
    cited = verify_citations(bodies)
    verify_build_gate()
    env, compiler = build_all.msvc_env()

    def go(work: Path) -> list[str]:
        build = Build(env, compiler, work, bodies)
        out = oracle_flavours(build, blob)
        if not arguments.no_mutants:
            out.append("mutation: " + mutation_test(build, blob))
        return out

    if arguments.keep:
        shutil.rmtree(arguments.keep, ignore_errors=True)
        arguments.keep.mkdir(parents=True)
        results = go(arguments.keep)
    else:
        with tempfile.TemporaryDirectory() as tmp:
            results = go(Path(tmp))
    print(census)
    print(f"frozen tables sha256={hashlib.sha256(blob).hexdigest()}")
    for line in results:
        print(line)
    print(f"Vita HasCollectible native: PASS; pins={len(PINS)} bodies + "
          f"{len(FROZEN)} tables; cited constants={cited}; "
          f"oracle flavours={sum(1 for r in results if r.startswith('['))}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
