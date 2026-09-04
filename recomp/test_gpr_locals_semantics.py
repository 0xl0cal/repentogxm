#!/usr/bin/env python3
"""Execute the same translated functions with the registers in memory and in
locals, and require identical final CPU state and guest memory.

The emitter spells every general register through `GR(reg)` and the stack
through `GPUSH/GPOP/GESP_SET/GESP_ADJ/GSTACK_ADDR`.  With GUEST_GPR_LOCAL=0
that is exactly the `c->reg` code the corpus always had; with GUEST_GPR_LOCAL=1
the eight registers live in locals and are published/reloaded at every
boundary (translated call, guest_call, fault, int3, cpuid, return, host seam).
This gate renders a set of functions ONCE through the real decoder, block
builder and emitter and compiles that one text twice, into a memory-mode and a
locals-mode executable of the same harness.  The harness runs every function
on the same seeded register file and guest memory image, then prints the
final registers, the fault/int3/call records and a hash of the whole guest
memory.  The two outputs must be byte-identical.

The synthetic cases are hand-encoded x86-32 sequences chosen for the register
traffic they create at boundaries: push/pop/esp arithmetic around a translated
callee and around a C callee that clobbers callee-saved registers (a reload
must pick that up), int3 and cpuid stubs that rewrite registers, guest_call
for an indirect call and an indirect jump, string operations, div/idiv/mul,
partial-register writes, setcc/cmovcc/lahf/sahf, x87 status word into eax,
movd between eax and xmm, the fs: segment base, `leave`/`ret n`, `push esp`,
`pop [mem]`, `xchg esp`, the divide-by-zero fault edge and lock cmpxchg.
With `--pe` RNG::Next (sub_003a7e20) from the frozen binary is added and also
checked against its native reference.

Host requirements: a GCC-compatible compiler on Linux x86-64 (guest address
== host address, so guest memory is an mmap(MAP_32BIT) buffer).
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, os.fspath(HERE))

import build_one as B                                        # noqa: E402
import trans as T                                            # noqa: E402

TEXT = 0x1000                 # synthetic image: text RVA
CALLEE = 0x1200               # translated callee inside the synthetic image
STUB = 0x00700000             # C callee, known to the emitter, not in the image
RNG_NEXT = 0x003A7E20

# (name, main bytes, optional callee bytes, initial register overrides)
# Register overrides: "reg": python expression in terms of BUF (guest buffer
# base, a uint32_t) evaluated in C.
CASES = [
    ("frame_and_calls",
     "55"              # push ebp
     "8bec"            # mov ebp, esp
     "56"              # push esi
     "57"              # push edi
     "8bf1"            # mov esi, ecx
     "8b06"            # mov eax, [esi]
     "034604"          # add eax, [esi+4]
     "50"              # push eax
     "e8{callee}"      # call CALLEE (translated)
     "83c404"          # add esp, 4
     "894608"          # mov [esi+8], eax
     "51"              # push ecx
     "e8{stub}"        # call STUB (C, clobbers esi/edi/ebx)
     "83c404"          # add esp, 4
     "89460c"          # mov [esi+0xc], eax
     "897e10"          # mov [esi+0x10], edi
     "895e14"          # mov [esi+0x14], ebx
     "5f5e5d"          # pop edi; pop esi; pop ebp
     "c3",
     "8b442404"        # callee: mov eax, [esp+4]
     "8d0440"          # lea eax, [eax+eax*2]
     "b902000000"      # mov ecx, 2
     "c3",
     {"ecx": "BUF + 0x1000", "esi": "BUF + 0x2000", "edi": "BUF + 0x3000"}),
    ("int3_and_cpuid",
     "8b06"            # mov eax, [esi]
     "cc"              # int3 (stub rewrites eax/ecx)
     "8946 04"         # mov [esi+4], eax
     "894e08"          # mov [esi+8], ecx
     "0fa2"            # cpuid (stub rewrites eax..edx)
     "89460c"          # mov [esi+0xc], eax
     "895e10"          # mov [esi+0x10], ebx
     "894e14"          # mov [esi+0x14], ecx
     "895618"          # mov [esi+0x18], edx
     "0f01d0"          # xgetbv
     "89461c"          # mov [esi+0x1c], eax
     "c3", None, {"esi": "BUF + 0x2000"}),
    ("indirect_call_and_jump",
     "8b4610"          # mov eax, [esi+0x10]  (guest address held in memory)
     "ff5614"          # call [esi+0x14]      -> guest_call stub
     "894604"          # mov [esi+4], eax
     "895608"          # mov [esi+8], edx
     "ffe0",           # jmp eax               -> guest_call stub, tail
     None, {"esi": "BUF + 0x2000"}),
    ("string_ops",
     "fc"              # cld
     "8b4e00"          # mov ecx, [esi+0]  (count, masked below)
     "83e10f"          # and ecx, 15
     "83c101"          # add ecx, 1
     "8bd1"            # mov edx, ecx
     "8d7e40"          # lea edi, [esi+0x40]
     "8d7680"          # lea esi, [esi-0x80]
     "f3a5"            # rep movsd
     "8bca"            # mov ecx, edx
     "b8efbeadde"      # mov eax, 0xdeadbeef
     "f3ab"            # rep stosd
     "ad"              # lodsd
     "fd"              # std
     "8d7f0c"          # lea edi, [edi+0xc]
     "b904000000"      # mov ecx, 4
     "f2ae"            # repne scasb
     "fc"              # cld
     "c3", None, {"esi": "BUF + 0x2100"}),
    ("div_mul",
     "8b06"            # mov eax, [esi]
     "8b4e04"          # mov ecx, [esi+4]
     "83c901"          # or ecx, 1
     "33d2"            # xor edx, edx
     "f7f1"            # div ecx
     "894608"          # mov [esi+8], eax
     "89560c"          # mov [esi+0xc], edx
     "8b06"            # mov eax, [esi]
     "99"              # cdq
     "f7f9"            # idiv ecx
     "894610"          # mov [esi+0x10], eax
     "895614"          # mov [esi+0x14], edx
     "f7e1"            # mul ecx
     "894618"          # mov [esi+0x18], eax
     "89561c"          # mov [esi+0x1c], edx
     "0fafc1"          # imul eax, ecx
     "6bc00b"          # imul eax, eax, 11
     "f7e9"            # imul ecx
     "894620"          # mov [esi+0x20], eax
     "c3", None, {"esi": "BUF + 0x2200"}),
    ("partial_registers",
     "8a06"            # mov al, [esi]
     "b412"            # mov ah, 0x12
     "668bc8"          # mov cx, ax
     "0fb65601"        # movzx edx, byte [esi+1]
     "0fbf4602"        # movsx eax, word [esi+2]
     "91"              # xchg eax, ecx
     "3bc1"            # cmp eax, ecx
     "0f95c0"          # setne al
     "0f44d1"          # cmovz edx, ecx
     "3bc8"            # cmp ecx, eax
     "9f"              # lahf
     "9e"              # sahf
     "0f92c3"          # setb bl
     "98"              # cwde
     "0fbae003"        # bt eax, 3
     "0f92c7"          # setb bh
     "0fa4c204"        # shld eax, edx, 4
     "0fbcca"          # bsf ecx, edx
     "c1c005"          # rol eax, 5
     "d1ca"            # ror edx, 1
     "f6d9"            # neg cl
     "f6d1"            # not cl
     "80c119"          # add cl, 0x19
     "6681c23412"      # add dx, 0x1234
     "890e"            # mov [esi], ecx
     "895604"          # mov [esi+4], edx
     "894608"          # mov [esi+8], eax
     "895e0c"          # mov [esi+0xc], ebx
     "c3", None, {"esi": "BUF + 0x2300"}),
    ("stack_shapes",
     "55"              # push ebp
     "8bec"            # mov ebp, esp
     "83ec20"          # sub esp, 0x20
     "83e4f0"          # and esp, -16
     "54"              # push esp
     "ff742404"        # push [esp+4]
     "8f06"            # pop dword [esi]
     "58"              # pop eax
     "894604"          # mov [esi+4], eax
     "8bc4"            # mov eax, esp
     "94"              # xchg eax, esp
     "94"              # xchg eax, esp
     "9c"              # pushfd
     "58"              # pop eax
     "894608"          # mov [esi+8], eax
     "c9"              # leave
     "c20800",         # ret 8
     None, {"esi": "BUF + 0x2400"}),
    ("fs_and_x87_and_sse",
     "64a100000000"    # mov eax, fs:[0]
     "8906"            # mov [esi], eax
     "d94604"          # fld dword [esi+4]
     "d84608"          # fadd dword [esi+8]
     "d95e0c"          # fstp dword [esi+0xc]
     "d94604"          # fld dword [esi+4]
     "d9ee"            # fldz
     "ded9"            # fcompp
     "dfe0"            # fnstsw ax
     "894610"          # mov [esi+0x10], eax
     "8b4e14"          # mov ecx, [esi+0x14]
     "660f6ec1"        # movd xmm0, ecx
     "660f7ec2"        # movd edx, xmm0
     "895618"          # mov [esi+0x18], edx
     "f30f2cc0"        # cvttss2si eax, xmm0
     "89461c"          # mov [esi+0x1c], eax
     "c3", None, {"esi": "BUF + 0x2500"}),
    ("divide_by_zero_fault",
     "8b06"            # mov eax, [esi]
     "33c9"            # xor ecx, ecx
     "33d2"            # xor edx, edx
     "f7f1"            # div ecx      -> guest_fault, return
     "894604"          # mov [esi+4], eax (unreachable)
     "c3", None, {"esi": "BUF + 0x2600"}),
    ("lock_ops",
     "8b06"            # mov eax, [esi]
     "8b4e04"          # mov ecx, [esi+4]
     "f00fb10e"        # lock cmpxchg [esi], ecx
     "0f94c2"          # sete dl
     "895608"          # mov [esi+8], edx
     "f00fc146 0c"     # lock xadd [esi+0xc], eax
     "894610"          # mov [esi+0x10], eax
     "8b4614"          # mov eax, [esi+0x14]
     "8b5618"          # mov edx, [esi+0x18]
     "8b4e1c"          # mov ecx, [esi+0x1c]
     "8b5e20"          # mov ebx, [esi+0x20]
     "f00fc74e28"      # lock cmpxchg8b [esi+0x28]
     "894630"          # mov [esi+0x30], eax
     "895634"          # mov [esi+0x34], edx
     "c3", None, {"esi": "BUF + 0x2700"}),
    ("conditional_tail_and_switch_like",
     "8b06"            # mov eax, [esi]
     "85c0"            # test eax, eax
     "0f84{stubj}"     # je STUB  (conditional tail call)
     "83f805"          # cmp eax, 5
     "7f03"            # jg +3
     "40"              # inc eax
     "eb01"            # jmp +1
     "48"              # dec eax
     "8906"            # mov [esi], eax
     "c3", None, {"esi": "BUF + 0x2800"}),
]


def _rel32(target: int, site_end: int) -> str:
    return ((target - site_end) & 0xFFFFFFFF).to_bytes(4, "little").hex()


def _encode(main_hex: str) -> bytes:
    """Fill the rel32 placeholders once the layout is known."""
    clean = main_hex.replace(" ", "")
    out = bytearray()
    i = 0
    while i < len(clean):
        if clean.startswith("{callee}", i):
            site_end = TEXT + len(out) + 4
            out += bytes.fromhex(_rel32(CALLEE, site_end))
            i += len("{callee}")
        elif clean.startswith("{stub}", i):
            site_end = TEXT + len(out) + 4
            out += bytes.fromhex(_rel32(STUB, site_end))
            i += len("{stub}")
        elif clean.startswith("{stubj}", i):
            site_end = TEXT + len(out) + 4
            out += bytes.fromhex(_rel32(STUB, site_end))
            i += len("{stubj}")
        else:
            out += bytes.fromhex(clean[i:i + 2])
            i += 2
    return bytes(out)


def render_function(ctx, start, name, known, calls_out):
    em, insns, order, body, unsup, _indirect, _members = B.translate(
        ctx, start, name, known=known)
    if unsup:
        raise SystemExit("%s: unsupported: %s" % (name, unsup[0]))
    calls_out.update(em.calls)
    lines = ["void %s(CPU *__restrict c)" % name, "{"]
    lines.extend("    %s" % line for line in B.flag_state_prologue(em))
    gate = B.entry_gate(start, order)
    if gate:
        lines.append("    %s" % gate)
    for ins, stmts in body:
        if ins.address in em.labels:
            lines.append("L_%08x:" % ins.address)
        lines.append("    /* %08x  %s %s */" % (ins.address, ins.mnemonic,
                                               ins.op_str))
        lines.extend("    %s" % s for s in stmts)
    lines.append("}")
    return "\n".join(lines) + "\n", em


def build_synthetic(case_index, name, main_hex, callee_hex, overrides):
    main = _encode(main_hex)
    text = bytearray(0x400)
    text[:len(main)] = main
    if callee_hex:
        callee = bytes.fromhex(callee_hex.replace(" ", ""))
        text[CALLEE - TEXT:CALLEE - TEXT + len(callee)] = callee
    ctx = T.Ctx("synthetic_%d" % case_index, 0x400000, TEXT,
                TEXT + len(text), bytes(text))
    known = {TEXT, CALLEE, STUB}
    calls = set()
    fn_name = "case_%d_%s" % (case_index, name)
    text_c, em = render_function(ctx, TEXT, fn_name, known, calls)
    pieces = [text_c]
    if callee_hex:
        callee_c, _ = render_function(ctx, CALLEE, "sub_%08x" % CALLEE,
                                      known, calls)
        pieces.insert(0, callee_c)
    return fn_name, "".join(pieces), em


HARNESS_HEAD = r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "guest.h"

#define BUF_SIZE (16u << 20)
static uint32_t BUF;                    /* guest base of the mapped buffer */
static uint32_t TIB;
static unsigned g_faults, g_int3, g_calls, g_stub_calls, g_callee_calls;
static uint32_t g_fault_addr, g_call_target;
static const char *g_fault_what;

void guest_fault(CPU *__restrict c, uint32_t addr, const char *what)
{
    g_faults++; g_fault_addr = addr; g_fault_what = what;
    /* Diagnostics read the register file exactly as a fault handler would. */
    st32(BUF + 0x100, c->eax); st32(BUF + 0x104, c->ecx);
    st32(BUF + 0x108, c->edx); st32(BUF + 0x10c, c->esp);
}
void guest_int3(CPU *__restrict c, uint32_t addr)
{
    g_int3++;
    c->eax ^= 0x11110000u + addr;       /* a debugger-style register rewrite */
    c->ecx = c->esp + 8u;
}
void guest_cpuid(CPU *__restrict c)
{
    c->eax = 0x0001u; c->ebx = 0x756e6547u; c->ecx = c->edx ^ 0x5a5au;
    c->edx = 0x49656e69u;
}
void guest_xgetbv(CPU *__restrict c) { c->eax = 7u + c->ecx; c->edx = 0u; }
void guest_call(CPU *__restrict c, uint32_t addr)
{
    /* An indirect target: pop the return address, read a stack argument,
     * change every caller-saved register and touch guest memory. */
    uint32_t ret = ld32(c->esp);
    g_calls++; g_call_target = addr;
    c->esp += 4u;
    c->eax = addr ^ ret;
    c->ecx = ret;
    c->edx = c->esi + 0x40u;
    st32(BUF + 0x120 + 4u * (g_calls & 7u), addr);
}
uint32_t g_guest_fs_base_cached;
uint32_t guest_fs_base(CPU *__restrict c) { (void)c; return TIB; }
unsigned char *g_guest_coverage_functions;
unsigned char *g_guest_coverage_cases;
uint64_t guest_atomic_cmpxchg64(uint32_t addr, uint64_t expected,
                                uint64_t desired)
{
    uint64_t observed = ld64(addr);
    if (observed == expected) st64(addr, desired);
    return observed;
}
/* The C callee: pops its return address, uses its argument, and violates
 * the callee-saved convention on purpose so a missing reload shows. */
void sub_00700000(CPU *__restrict c)
{
    uint32_t ret = ld32(c->esp);
    uint32_t arg = ld32(c->esp + 4u);
    g_stub_calls++;
    c->esp += 4u;
    c->eax = arg * 3u + ret;
    c->ecx = ~arg;
    c->edx = arg >> 3;
    c->esi += 0x100u;
    c->edi ^= 0x00ff00ffu;
    c->ebx = g_stub_calls;
    st32(BUF + 0x140 + 4u * (g_stub_calls & 7u), arg);
}
'''

HARNESS_TAIL = r'''
static uint32_t rng_state;
static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static uint64_t fnv(const unsigned char *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static void prime(uint32_t seed)
{
    unsigned char *base = (unsigned char *)(uintptr_t)BUF;
    uint32_t i;
    rng_state = seed;
    for (i = 0; i < BUF_SIZE; i += 4) {
        uint32_t v = rnd();
        memcpy(base + i, &v, 4);
    }
    /* guest addresses held in memory for the indirect call/jump case */
    st32(BUF + 0x2010, 0x00401234u);
    st32(BUF + 0x2014, 0x00405678u);
    /* a usable TIB word */
    st32(TIB, 0xFFFFFFFFu);
    g_faults = g_int3 = g_calls = g_stub_calls = g_callee_calls = 0;
    g_fault_addr = g_call_target = 0; g_fault_what = NULL;
}

static void run(const char *label, void (*fn)(CPU *__restrict), int with_ret,
                void (*setup)(CPU *))
{
    CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.eax = 0x11111111u; cpu.ecx = 0x22222222u; cpu.edx = 0x33333333u;
    cpu.ebx = 0x44444444u; cpu.ebp = BUF + 0x800000u; cpu.esi = 0x66666666u;
    cpu.edi = 0x77777777u;
    cpu.esp = BUF + 0x800000u;
    setup(&cpu);
    if (with_ret) {
        cpu.esp -= 4u;
        st32(cpu.esp, 0xDEADBEEFu);
    }
    fn(&cpu);
    printf("%-36s eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x "
           "esi=%08x edi=%08x | faults=%u(%08x %s) int3=%u calls=%u(%08x) "
           "stub=%u st_top=%d fsw=%04x mem=%016llx\n",
           label, cpu.eax, cpu.ecx, cpu.edx, cpu.ebx, cpu.esp - BUF, cpu.ebp - BUF,
           cpu.esi, cpu.edi, g_faults, g_fault_addr,
           g_fault_what ? g_fault_what : "-", g_int3, g_calls, g_call_target,
           g_stub_calls, cpu.st_top, cpu.fsw,
           (unsigned long long)fnv((const unsigned char *)(uintptr_t)BUF, BUF_SIZE));
}
'''


def build_harness(cases, pe_functions):
    parts = [HARNESS_HEAD]
    for _fn_name, text, _em in cases:
        parts.append(text)
    for _fn_name, text in pe_functions:
        parts.append(text)
    parts.append(HARNESS_TAIL)
    setups = []
    runs = []
    for index, (fn_name, _text, _em) in enumerate(cases):
        overrides = CASES[index][3]
        body = "".join("    c->%s = (uint32_t)(%s);\n" % (reg, expr)
                       for reg, expr in sorted(overrides.items()))
        setups.append("static void setup_%d(CPU *c) { (void)c;\n%s}\n"
                      % (index, body))
        runs.append('    prime(0x1234567u + %du);\n    run("%s", %s, 1, setup_%d);\n'
                    % (index, fn_name, fn_name, index))
    pe_runs = []
    if pe_functions:
        pe_runs.append(r'''
    {
        /* RNG::Next against its native reference (ladder_test.c) */
        typedef struct { uint32_t seed, s1, s2, s3; } RNG;
        RNG ref = { 12345u, 13u, 17u, 5u }, guest = ref;
        uint32_t i, h_ref = 2166136261u, h_guest = 2166136261u;
        CPU cpu;
        prime(0x77u);
        memcpy((void *)(uintptr_t)(BUF + 0x4000), &guest, sizeof guest);
        memset(&cpu, 0, sizeof cpu);
        cpu.esp = BUF + 0x800000u;
        for (i = 0; i < 200000u; i++) {
            uint32_t s = ref.seed;
            s ^= s >> (ref.s1 & 31); s ^= s << (ref.s2 & 31);
            s ^= s >> (ref.s3 & 31); ref.seed = s;
            h_ref = (h_ref ^ s) * 16777619u;
            cpu.ecx = BUF + 0x4000;
            cpu.esp -= 4u; st32(cpu.esp, 0xDEADBEEFu);
            sub_003a7e20(&cpu);
            h_guest = (h_guest ^ cpu.eax) * 16777619u;
        }
        printf("RNG::Next x200000: ref=%08x guest=%08x esp=%08x %s\n", h_ref,
               h_guest, cpu.esp - BUF, h_ref == h_guest ? "EQUAL" : "DIFFERENT");
        if (h_ref != h_guest) return 1;
    }
''')
    main = r'''
%s
int main(void)
{
    /* A fixed guest address makes the two modes' outputs comparable byte
     * for byte (stored pointers and the memory hash include it). */
    void *map = mmap((void *)0x40000000u, BUF_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (map == MAP_FAILED)
        map = mmap(NULL, BUF_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (map == MAP_FAILED) { perror("mmap"); return 2; }
    BUF = (uint32_t)(uintptr_t)map;
    TIB = BUF + 0x10000u;
    if ((uintptr_t)map != BUF) { fputs("buffer above 4 GiB\n", stderr); return 2; }
%s%s
    puts("gpr oracle: done");
    return 0;
}
''' % ("".join(setups), "".join(runs), "".join(pe_runs))
    parts.append(main)
    return "".join(parts)


def logger_stub():
    return r'''
/* KAGE's logger, RNG::Next's only callee (see ladder_test.c): pops the
 * return address, otherwise leaves the register file alone. */
unsigned g_log_calls;
void sub_0055e330(CPU *__restrict c) { g_log_calls++; c->esp += 4u; }
'''


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", help="GCC-compatible host compiler (Linux "
                        "x86-64); omit to only render")
    parser.add_argument("--pe", type=pathlib.Path,
                        help="frozen isaac-ng.exe.unpacked.exe: adds RNG::Next")
    parser.add_argument("--emit", type=pathlib.Path,
                        help="write the harness source here")
    arguments = parser.parse_args()

    cases = []
    for index, (name, main_hex, callee_hex, overrides) in enumerate(CASES):
        cases.append(build_synthetic(index, name, main_hex, callee_hex,
                                     overrides))
    pe_functions = []
    if arguments.pe is not None:
        from image import Image
        img = Image(os.fspath(arguments.pe))
        calls = set()
        text, _em = render_function(img, RNG_NEXT, "sub_%08x" % RNG_NEXT,
                                    None, calls)
        if calls != {0x0055E330}:
            raise SystemExit("RNG::Next callee set changed: %r" % calls)
        pe_functions.append(("sub_%08x" % RNG_NEXT, logger_stub() + text))
    source = build_harness(cases, pe_functions)
    # Every rendered body carries the two declarations and no CPU spelling.
    bodies = source.count("GUEST_GPR_DECL;")
    if bodies != len(cases) + sum(1 for c in cases if c[2].calls & {CALLEE}) + len(pe_functions):
        raise SystemExit("unexpected number of rendered bodies: %d" % bodies)
    if arguments.emit is not None:
        arguments.emit.write_text(source, encoding="ascii", newline="\n")
        print("harness written: %s" % arguments.emit)
    if arguments.cc is None:
        print("gpr locals oracle: rendered %d synthetic cases%s (no --cc, not run)"
              % (len(cases), " + RNG::Next" if pe_functions else ""))
        return 0
    with tempfile.TemporaryDirectory(prefix="isaac-gpr-oracle-") as work_raw:
        work = pathlib.Path(work_raw)
        source_path = work / "gpr_oracle.c"
        source_path.write_text(source, encoding="ascii", newline="\n")
        outputs = {}
        for mode in (0, 1):
            binary = work / ("gpr_oracle_%d" % mode)
            command = [
                arguments.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra",
                "-Wno-unused-variable", "-Wno-unused-but-set-variable",
                "-Wno-unused-label", "-Wno-unused-parameter",
                "-DGUEST_GENERATED_STACK_GUARD=0", "-DGUEST_STACK_REQUIRED=0",
                "-DGUEST_GPR_LOCAL=%d" % mode, "-DGUEST_FLAGS_LOCAL=%d" % mode,
                "-I", os.fspath(HERE / "runtime"), os.fspath(source_path),
                "-o", os.fspath(binary), "-lm",
            ]
            built = subprocess.run(command, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True,
                                   check=False)
            if built.stdout:
                print(built.stdout, end="")
            if built.returncode:
                raise SystemExit("gpr oracle compile failed in mode %d" % mode)
            ran = subprocess.run([os.fspath(binary)], stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True,
                                 check=False, timeout=600)
            if ran.returncode:
                print(ran.stdout, end="")
                raise SystemExit("gpr oracle run failed in mode %d" % mode)
            outputs[mode] = ran.stdout
        print(outputs[0], end="")
        if outputs[0] != outputs[1]:
            import difflib
            sys.stdout.writelines(difflib.unified_diff(
                outputs[0].splitlines(True), outputs[1].splitlines(True),
                "memory-mode", "locals-mode"))
            raise SystemExit("gpr locals oracle: memory and locals modes differ")
        print("gpr locals oracle: %d cases%s identical in both modes: PASS"
              % (len(cases), " + RNG::Next" if pe_functions else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
