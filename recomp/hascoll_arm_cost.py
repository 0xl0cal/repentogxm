#!/usr/bin/env python3
"""ARM cost probe for ISAAC_VITA_HASCOLLECTIBLE_NATIVE (not a gate).

Executed Thumb-2 instructions and data accesses per HasCollectible call,
native replay vs the translated body, on the same synthetic player states.

Both sides are compiled by clang --target=thumbv7a-none-eabi with the
production flags (-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -O2
-fno-strict-aliasing -ffunction-sections) -- clang, not the vitasdk GCC of the
device build, so the counts are an estimate of the device code:
  - the native: runtime/host_vita_hascoll_native.c;
  - the translated: the frozen corpus bodies test_vita_hascoll_native.py
    extracts, one object per production unit (guest_0092.c HasCollectible,
    guest_0080.c GetCollectible + predicate, ...) so nothing inlines across
    what are separate units on the Vita; GUEST_GPR_LOCAL=1, FLAGS_LOCAL=1,
    GENERATED_STACK_GUARD=0, COVERAGE_HOOKS=0 as the production corpus;
  - host_vita_gfuel_seed_native.c: the translated body's G FUEL queries reach
    the native seams, as on the device (ISAAC_VITA_GFUEL_SEED_NATIVE=ON);
    with --profile also its profiling seams (GetCollectible, sub_000206b0,
    GetTrinketMultiplier, sub_0039ba90), i.e. the build ph120.gu measured.
GNU ld --wrap rewrites only undefined references; ld.lld rewrites all of
them, so the wraps are spelled as renames per object instead.

Unicorn runs the entries on identical start states, counts, and compares the
whole CPU struct and the guest stack words afterwards (an ARM-compiled
differential next to the MSVC oracle), and fails on any mismatch, stub entry
or write outside the guest stack, the CPU struct and the C stack.

usage: hascoll_arm_cost.py --pe PE --generated-dir DIR [--profile]
       [--nogfuel] [--calls N]   (needs clang + ld.lld from LLVM, unicorn, pyelftools)
"""
from __future__ import annotations

import argparse
import bisect
import collections
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
sys.path.insert(0, str(HERE))
import test_vita_hascoll_native as T  # noqa: E402

# production owner unit of each replayed body (test_vita_hascoll_native.py
# REPLAYED; the native TU's header comment)
UNIT = {
    "sub_0033af30": "guest_0092", "sub_002fe5e0": "guest_0080",
    "sub_002ff070": "guest_0080", "sub_0048b570": "guest_0140",
    "sub_00535fa0": "guest_0161", "sub_0019fce0": "guest_0044",
    "sub_0019ff20": "guest_0044", "sub_00144990": "guest_0034",
    "sub_000206b0": "guest_0004", "sub_0039cb10": "guest_0106",
    "sub_0039cb80": "guest_0106",
}

FAKE_SETJMP = """#ifndef FAKE_SETJMP_H
#define FAKE_SETJMP_H
/* newlib arm: _JBLEN 23, _JBTYPE long long */
typedef long long jmp_buf[23];
int setjmp(jmp_buf);
void longjmp(jmp_buf, int) __attribute__((noreturn));
#endif
"""
FAKE_STRING = """#ifndef FAKE_STRING_H
#define FAKE_STRING_H
#include <stddef.h>
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strchr(const char *, int);
#endif
"""
SUPPORT = r"""
#include <stddef.h>
#include "guest.h"
const unsigned k_cpu_layout[] = {
    offsetof(CPU, eax), offsetof(CPU, ecx), offsetof(CPU, edx),
    offsetof(CPU, ebx), offsetof(CPU, esp), offsetof(CPU, ebp),
    offsetof(CPU, esi), offsetof(CPU, edi), offsetof(CPU, stack_owner),
    offsetof(CPU, stack_floor), offsetof(CPU, stack_ceiling), sizeof(CPU)
};
volatile unsigned g_stub_calls;
void guest_call(CPU *__restrict c, uint32_t a) { (void)c; (void)a; ++g_stub_calls; }
void guest_int3(CPU *__restrict c, uint32_t a) { (void)c; (void)a; ++g_stub_calls; }
void oracle_hc_other_unit(CPU *__restrict c) { (void)c; ++g_stub_calls; }
void sub_0038d4c0(CPU *__restrict c) { (void)c; ++g_stub_calls; }
void sub_0055e330(CPU *__restrict c) { (void)c; ++g_stub_calls; }
unsigned __aeabi_uidiv(unsigned a, unsigned b) { (void)a; (void)b; ++g_stub_calls; return 0; }
unsigned long long __aeabi_uidivmod(unsigned a, unsigned b) { (void)a; (void)b; ++g_stub_calls; return 0; }
uint64_t sceKernelGetProcessTimeWide(void) { static uint64_t t; return ++t; }
void *memset(void *d, int v, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)v; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s;
    if (p < q) { while (n--) *p++ = *q++; } else { while (n--) p[n] = q[n]; } return d; }
void __aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy8(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr8(void *d, size_t n) { memset(d, 0, n); }
"""


def llvm_tool(name: str) -> str:
    found = shutil.which(name)
    if found:
        return found
    candidate = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / \
        "LLVM" / "bin" / (name + (".exe" if os.name == "nt" else ""))
    T.require(candidate.is_file(), f"{name} not found (LLVM)")
    return str(candidate)


def build_image(work: Path, bodies: dict[str, str], tag: str) -> Path:
    """tag: plain (G FUEL seams linked, as the measurement builds), profile
    (plus the G FUEL profiling seams), nogfuel (no G FUEL seams: the
    translated body runs the translated G FUEL bodies)."""
    profile = tag == "profile"
    clang, lld = llvm_tool("clang"), llvm_tool("ld.lld")
    env = os.environ.copy()
    fake = work / "fake"
    fake.mkdir(exist_ok=True)
    (fake / "setjmp.h").write_text(FAKE_SETJMP)
    (fake / "string.h").write_text(FAKE_STRING)
    base = ["--target=thumbv7a-none-eabi", "-mcpu=cortex-a9", "-mfpu=neon",
            "-mfloat-abi=softfp", "-mthumb", "-std=gnu11", "-O2",
            "-fno-strict-aliasing", "-ffunction-sections", "-fdata-sections",
            "-isystem", str(fake), f"-I{RUNTIME}",
            "-DGUEST_IMAGE_BASE=0x98000000u", "-DGUEST_STACK_REQUIRED=1"]
    body_defs = ["-DGUEST_GENERATED_STACK_GUARD=0", "-DGUEST_FLAGS_LOCAL=1",
                 "-DGUEST_COVERAGE_HOOKS=0", "-DISAAC_VITA_IMPORT_ID_DISPATCH=1",
                 "-DISAAC_VITA_GUEST_LOOKUP_CACHE=1", "-DGUEST_GPR_LOCAL=1"]
    header = T.bodies_source({n: "" for n in T.BODIES}).rstrip("\n")
    units: dict[str, list[str]] = collections.defaultdict(list)
    for name in T.BODIES:
        text = bodies[name]
        if name in T.OTHER_UNIT_CALLERS:
            text = text.replace("sub_0033af30(c)", "oracle_hc_other_unit(c)")
        units[UNIT.get(name, "x_" + name)].append(text)
    objs = []
    for unit, texts in units.items():
        src = work / f"{tag}-{unit}.c"
        src.write_text(header + "\n" + "\n".join(texts), encoding="utf-8")
        extra = []
        if unit == "guest_0092" and tag != "nogfuel":
            # cross-unit edges that --wrap rewrites on the device
            extra += ["-Dsub_0039cb10=__wrap_sub_0039cb10",
                      "-Dsub_0039cb80=__wrap_sub_0039cb80"]
        if unit == "guest_0092" and profile:
            extra += ["-Dsub_002fe5e0=__wrap_sub_002fe5e0",
                      "-Dsub_0038d4c0=__wrap_sub_0038d4c0",
                      "-Dsub_0039ba90=__wrap_sub_0039ba90"]
        if profile and unit in ("guest_0034", "guest_0106"):
            extra += ["-Dsub_000206b0=__wrap_sub_000206b0"]
        obj = src.with_suffix(".o")
        T.run([clang, *base, *body_defs, *extra, "-w", "-c", str(src), "-o", str(obj)], env)
        objs.append(obj)
    native = work / f"{tag}-native.o"
    T.run([clang, *base, "-Wall", "-Wextra", "-Werror",
           "-DISAAC_VITA_HASCOLLECTIBLE_NATIVE=1", "-DGUEST_FLAGS_LOCAL=1",
           "-D__real_sub_0033af30=sub_0033af30",
           "-D__wrap_sub_0033af30=hc_wrap_unused",
           "-c", str(RUNTIME / "host_vita_hascoll_native.c"), "-o", str(native)], env)
    gf = ["-D__real_sub_0039cb10=sub_0039cb10", "-D__real_sub_0039cb80=sub_0039cb80"]
    if profile:
        gf += ["-DISAAC_VITA_GFUEL_SEED_NATIVE_PROFILE=1",
               "-D__real_sub_000206b0=sub_000206b0",
               "-D__real_sub_0033af30=sub_0033af30",
               "-D__real_sub_002fe5e0=sub_002fe5e0",
               "-D__real_sub_0038d4c0=sub_0038d4c0",
               "-D__real_sub_0039ba90=sub_0039ba90",
               "-D__wrap_sub_0033af30=gfuel_has_wrap_unused"]
    gfuel = work / f"{tag}-gfuel.o"
    T.run([clang, *base, "-Wall", "-Wextra", *gf, "-c",
           str(RUNTIME / "host_vita_gfuel_seed_native.c"), "-o", str(gfuel)], env)
    support_c = work / "support.c"
    support_c.write_text(SUPPORT)
    support = work / f"{tag}-support.o"
    T.run([clang, *base, "-fno-builtin", "-c", str(support_c), "-o", str(support)], env)
    elf = work / f"{tag}.elf"
    T.run([lld, "-static", "-e", "isaac_vita_hascoll_native_call",
           "-Ttext=0x10000", *map(str, objs), str(native), str(gfuel),
           str(support), "-o", str(elf)], env)
    return elf


# ---- the emulator -------------------------------------------------------------

IMAGE = 0x98000000
HEAP, HEAP_SIZE = 0x20000000, 0x01000000
GSTACK, GSTACK_SIZE = 0x30000000, 0x10000
NSTACK, NSTACK_SIZE = 0x40000000, 0x10000
TRAP = 0x50000000
TEXT_LO, TEXT_HI = 0x10000, 0x40000
N_ITEMS = 0x2B0
ESP = GSTACK + GSTACK_SIZE - 0x400
SNAP_LO = ESP - 0x200          # deepest replay write is e-0xbc (the oracle)
SCENARIOS = [("lean", 0, 0, 0), ("typical", 3, 6, 3), ("heavy", 8, 30, 12)]
DECLINED_IDS = {0x147, 0x148, 0x139, 9, 0x3B}


class Emu:
    def __init__(self, elf: Path, blob: bytes):
        from elftools.elf.elffile import ELFFile
        from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE,
                             UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE)
        from unicorn import arm_const as A
        self.A = A
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        # VFP/NEON on (the translated xmm copies): CPACR cp10/cp11, FPEXC.EN
        uc.reg_write(A.UC_ARM_REG_C1_C0_2, uc.reg_read(A.UC_ARM_REG_C1_C0_2) | (0xF << 20))
        uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
        self.syms = {}
        with open(elf, "rb") as f:
            e = ELFFile(f)
            uc.mem_map(TEXT_LO, TEXT_HI - TEXT_LO)
            for seg in e.iter_segments():
                if seg["p_type"] == "PT_LOAD" and seg["p_filesz"]:
                    uc.mem_write(seg["p_vaddr"], seg.data())
            for s in e.get_section_by_name(".symtab").iter_symbols():
                self.syms[s.name] = (s["st_value"], s["st_size"])
        for b, n in ((HEAP, HEAP_SIZE), (GSTACK, GSTACK_SIZE), (NSTACK, NSTACK_SIZE),
                     (TRAP, 0x1000), (IMAGE + 0x2F0000, 0x10000),
                     (IMAGE + 0x390000, 0x10000), (IMAGE + 0x600000, 0x10000),
                     (IMAGE + 0x740000, 0x10000), (IMAGE + 0x7F0000, 0x10000)):
            uc.mem_map(b, n)
        self.layout = struct.unpack("<12I", uc.mem_read(self.syms["k_cpu_layout"][0], 48))
        for i in (0, 4):   # 0x400000-based VAs in the PE, rebased like the corpus
            self.w32(IMAGE + 0x2FF0BC + i,
                     struct.unpack("<I", blob[i:i + 4])[0] - 0x400000 + IMAGE)
        uc.mem_write(IMAGE + 0x2FF0C4, blob[8:208])
        # the rest of the frozen blob as the oracle's install_frozen lays it
        # out (host_vita_hascoll_native_oracle.c): sub_0039cb80's jump table
        # (rebased), then the 0x608ad0/0x608b00/0x608b54/0x74d554 bytes
        for i in range(8):
            self.w32(IMAGE + 0x39CC68 + 4 * i,
                     struct.unpack("<I", blob[208 + 4 * i:212 + 4 * i])[0]
                     - 0x400000 + IMAGE)
        for rva, lo, n in ((0x608AD0, 240, 12), (0x608B00, 252, 12),
                           (0x608B54, 264, 12), (0x74D554, 276, 24)):
            uc.mem_write(IMAGE + rva, blob[lo:lo + n])
        T.require(len(blob) == 300, "frozen blob layout changed")
        self.funcs = sorted((v & ~1, n) for n, (v, sz) in self.syms.items()
                            if sz and TEXT_LO <= v < TEXT_HI and not n.startswith("$"))
        self.addrs = [a for a, _ in self.funcs]
        self.st = collections.Counter()
        self.per_func = collections.Counter()
        self.cpu_range = (0, 0)
        uc.hook_add(UC_HOOK_CODE, self._code, begin=TEXT_LO, end=TEXT_HI)
        uc.hook_add(UC_HOOK_MEM_READ, self._read)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._write)

    def _code(self, uc, addr, size, data):
        self.st["insn"] += 1
        self.per_func[self.funcs[bisect.bisect_right(self.addrs, addr) - 1][1]] += 1

    def _read(self, uc, access, addr, size, value, data):
        self.st["rd"] += 1

    def _write(self, uc, access, addr, size, value, data):
        self.st["wr"] += 1
        if not (GSTACK <= addr < GSTACK + GSTACK_SIZE or
                NSTACK <= addr < NSTACK + NSTACK_SIZE or
                self.cpu_range[0] <= addr < self.cpu_range[1] or
                TEXT_LO <= addr < TEXT_HI):
            self.st["foreign"] += 1

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))

    def w8(self, a, v):
        self.uc.mem_write(a, bytes([v & 0xFF]))

    def call(self, entry: str, cpu: int) -> tuple[dict, tuple, int]:
        A, uc = self.A, self.uc
        stubs = self.syms["g_stub_calls"][0]
        self.w32(stubs, 0)
        uc.reg_write(A.UC_ARM_REG_SP, NSTACK + NSTACK_SIZE)
        uc.reg_write(A.UC_ARM_REG_LR, TRAP | 1)
        uc.reg_write(A.UC_ARM_REG_R0, cpu)
        before = dict(self.st)
        try:
            uc.emu_start(self.syms[entry][0] | 1, TRAP, count=500000)
        except Exception:
            pc = uc.reg_read(A.UC_ARM_REG_PC)
            fn = self.funcs[bisect.bisect_right(self.addrs, pc) - 1]
            print(f"fault in {entry} at {pc:#x} ({fn[1]}+{pc - fn[0]:#x})")
            raise
        cost = {k: self.st[k] - before.get(k, 0) for k in ("insn", "rd", "wr", "foreign")}
        size = self.layout[11]
        state = (bytes(uc.mem_read(cpu, size)), bytes(uc.mem_read(SNAP_LO, 0x20C)))
        return cost, state, struct.unpack("<I", uc.mem_read(stubs, 4))[0]


class World:
    """One synthetic player state, written identically into an emulator."""

    def __init__(self, rng: random.Random, seeds: int, map_nodes: int, effects: int):
        self.writes: list[tuple[int, bytes]] = []
        self.at = HEAP
        game = self.take(0x20000)
        config = self.take(0x2A000)
        self.player = self.take(0x2000)
        p = self.player
        self.w32(IMAGE + 0x7FD65C, game)
        self.w32(IMAGE + 0x7FD680, config)
        vec = self.take(4 * N_ITEMS)
        for i in range(N_ITEMS):
            it = self.take(0xC0)
            self.w32(it, (1, 3, 4)[rng.randrange(3)] if rng.random() < 0.9 else 5)
            self.w32(it + 4, i)
            self.w32(it + 0xB8, rng.getrandbits(32) & ~0x40008000)
            self.w32(it + 0xBC, rng.getrandbits(32))
            self.w32(vec + 4 * i, it)
        self.w32(config + 0x29E84, vec)
        self.w32(config + 0x29E88, vec + 4 * N_ITEMS)
        # the G FUEL seed set without SEED_G_FUEL (0x4f), as in a normal run
        self.tree(game + 0x1BB58, [k for k in rng.sample(range(0x60), seeds)
                                   if k != 0x4F], rng)
        gv = self.take(0x100)
        self.w32(p + 0x1ECC, gv)
        self.w32(p + 0x1ED0, gv + 0x100)
        self.tree(p + 0x1E7C, rng.sample(range(1, N_ITEMS), map_nodes), rng)
        cfgs = []
        for _ in range(8):
            e = self.take(0x10)
            self.w32(e, (1, 3, 4)[rng.randrange(3)])
            self.w32(e + 4, rng.randrange(1, N_ITEMS))
            cfgs.append(e)
        lst = self.take(12 * max(effects, 1))
        for i in range(effects):
            self.w32(lst + 12 * i, cfgs[rng.randrange(8)])
        self.w32(p + 0x1450 + 4, lst)
        self.w32(p + 0x1450 + 8, lst + 12 * effects)
        bv = self.take(N_ITEMS)
        self.writes.append((bv, bytes(rng.random() < 0.02 for _ in range(N_ITEMS))))
        self.w32(p + 0x1EC0 + 0xC, bv)
        self.w32(p + 0x1EC0 + 0x10, bv + N_ITEMS)
        counts = self.take(4 * N_ITEMS)
        self.writes.append((counts, b"".join(struct.pack("<i", rng.random() < 0.05)
                                             for _ in range(N_ITEMS))))
        self.w32(p + 0x15F0, counts)
        self.w32(p + 0x12E0, rng.randrange(1, N_ITEMS))
        self.w32(p + 0x12D8, rng.randrange(1, N_ITEMS))
        self.cpu = self.take(0x1000)   # CPU is smaller (k_cpu_layout)

    def take(self, n):
        self.at = (self.at + 15) & ~15
        a, self.at = self.at, self.at + n
        return a

    def w32(self, a, v):
        self.writes.append((a, struct.pack("<I", v & 0xFFFFFFFF)))

    def tree(self, holder, keys, rng):
        """MSVC _Tree as sub_000206b0 reads it: {left +0, parent +4,
        right +8, isnil +0xd, key +0x10, value +0x14}; head isnil = 1."""
        head = self.take(0x18)
        self.writes.append((head + 0xD, b"\1"))
        keys = sorted(set(keys))
        nodes = {}

        def build(lo, hi, parent):
            if lo > hi:
                return head
            mid = (lo + hi) // 2
            n = self.take(0x18)
            nodes[keys[mid]] = n
            self.w32(n + 4, parent)
            self.w32(n + 0x10, keys[mid])
            self.w32(n + 0x14, rng.randrange(-3, 4))
            self.w32(n, build(lo, mid - 1, n))
            self.w32(n + 8, build(mid + 1, hi, n))
            return n

        self.w32(head + 4, build(0, len(keys) - 1, head))
        self.w32(head, nodes[keys[0]] if keys else head)
        self.w32(head + 8, nodes[keys[-1]] if keys else head)
        self.w32(holder, head)
        self.w32(holder + 4, len(keys))

    def install(self, emu: Emu):
        emu.uc.mem_write(HEAP, bytes(self.at - HEAP + 0x1000))
        for a, data in self.writes:
            emu.uc.mem_write(a, data)
        emu.cpu_range = (self.cpu, self.cpu + emu.layout[11])

    def start(self, emu: Emu, ident: int, regs: list[int]):
        (o_eax, o_ecx, o_edx, o_ebx, o_esp, o_ebp, o_esi, o_edi, o_owner,
         o_floor, o_ceil, size) = emu.layout
        c = self.cpu
        emu.uc.mem_write(c, bytes(size))
        emu.w32(c + o_esp, ESP)
        emu.w32(c + o_ecx, self.player)
        for off, v in zip((o_eax, o_edx, o_ebx, o_ebp, o_esi, o_edi), regs):
            emu.w32(c + off, v)
        emu.w32(c + o_owner, c)
        emu.w32(c + o_floor, GSTACK)
        emu.w32(c + o_ceil, GSTACK + GSTACK_SIZE)
        emu.uc.mem_write(SNAP_LO, bytes(0x200))
        emu.w32(ESP, 0x00401234)
        emu.w32(ESP + 4, ident)
        emu.w32(ESP + 8, 0)


def measure(elf: Path, blob: bytes, calls: int, label: str) -> list[str]:
    emu = Emu(elf, blob)
    rng = random.Random(20261024)
    lines, bad = [], 0
    for name, seeds, map_nodes, effects in SCENARIOS:
        world = World(rng, seeds, map_nodes, effects)
        world.install(emu)
        acc = {s: collections.Counter() for s in ("translated", "native")}
        n = mismatch = stubbed = foreign = 0
        for _ in range(calls):
            ident = rng.randrange(1, N_ITEMS)
            if ident in DECLINED_IDS:
                continue
            regs = [rng.getrandbits(32) for _ in range(6)]
            out = {}
            for side, entry in (("translated", "sub_0033af30"),
                                ("native", "isaac_vita_hascoll_native_call")):
                world.start(emu, ident, regs)
                out[side] = emu.call(entry, world.cpu)
            if out["native"][2] or out["translated"][2]:
                stubbed += 1
                continue
            if out["native"][1] != out["translated"][1]:
                mismatch += 1
                continue
            foreign += out["native"][0]["foreign"] + out["translated"][0]["foreign"]
            n += 1
            for side in acc:
                acc[side].update(out[side][0])
        t, v = acc["translated"], acc["native"]
        bad += mismatch + stubbed + foreign + (n == 0)
        lines.append(
            f"[{label}] {name} (seeds={seeds} map={map_nodes} effects={effects}) "
            f"calls={n} mismatches={mismatch} stubs={stubbed} foreign={foreign}: "
            f"translated {t['insn'] / n:.0f} insn {(t['rd'] + t['wr']) / n:.0f} "
            f"mem, native {v['insn'] / n:.0f} insn {(v['rd'] + v['wr']) / n:.0f} "
            f"mem, ratio insn {v['insn'] / t['insn']:.3f} mem "
            f"{(v['rd'] + v['wr']) / (t['rd'] + t['wr']):.3f}")
    T.require(bad == 0, f"{label}: mismatches, stub entries or foreign writes")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--pe", type=Path, required=True)
    parser.add_argument("--generated-dir", type=Path, required=True)
    parser.add_argument("--calls", type=int, default=300)
    parser.add_argument("--profile", action="store_true",
                        help="also the G FUEL profiling seams (the ph120.gu build)")
    parser.add_argument("--nogfuel", action="store_true",
                        help="also without ISAAC_VITA_GFUEL_SEED_NATIVE's seams")
    args = parser.parse_args()
    blob = T.verify_frozen(args.pe)
    bodies, _ = T.extract(args.generated_dir)
    with tempfile.TemporaryDirectory(prefix="hascoll-arm-") as tmp:
        work = Path(tmp)
        tags = ["plain"] + ["profile"] * args.profile + ["nogfuel"] * args.nogfuel
        out = []
        for tag in tags:
            out += measure(build_image(work, bodies, tag), blob, args.calls, tag)
    for line in out:
        print(line)
    print("HasCollectible ARM cost probe: PASS (clang Thumb-2, unicorn; counts, not time)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
