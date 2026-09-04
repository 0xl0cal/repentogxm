#!/usr/bin/env python3
"""Inlined tiny leaves versus the out-of-line call: identical CPU state, guest
stack and memory, fault records.

recomp/leaf_inline.py copies a proven tiny leaf's statements into its direct
callers behind `#if GUEST_LEAF_INLINE`, keeping the ordinary call under
`#else`.  Because the site carries both spellings, the proof is a relink: the
same rendered C is compiled four times -- GUEST_LEAF_INLINE=0/1 with the
registers in memory and in locals (GUEST_GPR_LOCAL/GUEST_FLAGS_LOCAL) -- and
every binary must print byte-identical results over the same randomised
register file, stack and 16 MiB guest memory (registers, x87 top, xmm0/xmm1,
fault/int3/guest_call records and a hash of the whole memory).

Synthetic cases (hand-encoded x86-32) cover the accepted shapes -- register
getter, absolute global getter with add, [esp+N] argument reader with `ret 8`,
flag-producing leaf whose CF is consumed inside the leaf, partial-register
writer, push/pop frame, SSE scalar leaf, `inc [mem]`, cmp+cmov, xchg, and the
`cmp/test ; jcc ; ret` guard (MSVC __security_check_cookie shape) on both its
paths and with the ret block as branch target or fall-through -- and the
refusals: a [esp] reader, `lea r,[esp+..]`, `mov r,esp`, nine instructions, a
callee that calls, x87, a string operation and a caller that consumes the
callee's flags.  The rendered text is checked for exactly the expected set of
inlined sites, so a silently weakened predicate fails before anything runs.
  The flag-consuming caller is rendered and checked but not driven: reading a
  flag right after a call is undefined in the emitter's model (a call defines
  the flags), and the memory-mode and locals-mode binaries already disagree on
  it today, with or without inlining.

With `--pe` and the generation census (`--census leaf_inline_census.json`,
written by gen_all under GUEST_LEAF_INLINE=1) the 20 most-called eligible
leaves plus 100 more chosen by a seeded shuffle are driven from the frozen
binary through an overlay caller (`[mov ecx,[cmp operand]] ; call leaf ; ret`);
the image range is mapped and filled with seeded noise so absolute globals
read the same bytes in every binary.  A seed whose random pointers leave the
mapping is caught by a SIGSEGV handler and reported as such (identically), and
the memory hash of that leaf is skipped.

Host requirements: a GCC-compatible compiler on Linux x86-64 (guest address ==
host address; the buffer is mapped at 0x40000000 and the image at its base).
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import random
import re
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, os.fspath(HERE))

# capstone-dependent modules are imported lazily: the box that compiles and
# runs the harness (--prerendered) has no capstone.

TEXT = 0x1000
BUF_FIXED = 0x40000000
G_COOKIE = BUF_FIXED + 0x1000          # absolute globals inside the buffer
G_FAIL = BUF_FIXED + 0x1004
G_COUNTER = BUF_FIXED + 0x1008
CASE_STRIDE = 0x100

SEEDS_SYNTHETIC = 48
SEEDS_PE = 48
PE_TOP = 20
PE_RANDOM = 100
PE_OVERLAY_RVA = 0x00F00000            # decoded only, never executed


def le32(value):
    return (value & 0xFFFFFFFF).to_bytes(4, "little").hex()


# --- synthetic callees ------------------------------------------------------
# name -> (bytes hex, expected verdict: "leaf" | "guard" | None)
CALLEES = {
    "getter": ("8b4118"            # mov eax, [ecx+0x18]
               "c3", "leaf"),
    "global_add": ("a1" + le32(G_COUNTER) +  # mov eax, [G_COUNTER]
                   "05842e0200"         # add eax, 0x29e84
                   "c3", "leaf"),
    "args_ret8": ("8b442404"      # mov eax, [esp+4]
                  "0faf442408"    # imul eax, [esp+8]
                  "c20800", "leaf"),     # ret 8
    "flags_cf": ("3bca"           # cmp ecx, edx
                 "b800000000"     # mov eax, 0
                 "83d000"         # adc eax, 0   (reads CF from the cmp)
                 "c3", "leaf"),
    "partial": ("8a01"            # mov al, [ecx]
                "b403"            # mov ah, 3
                "0fb65101"        # movzx edx, byte [ecx+1]
                "c3", "leaf"),
    "push_pop": ("56"             # push esi
                 "8b31"           # mov esi, [ecx]
                 "8b4604"         # mov eax, [esi+4]
                 "5e"             # pop esi
                 "c3", "leaf"),
    "sse": ("f30f1001"            # movss xmm0, [ecx]
            "f30f584104"          # addss xmm0, [ecx+4]
            "f30f1102"            # movss [edx], xmm0
            "c3", "leaf"),
    "inc_mem": ("ff01"            # inc dword [ecx]
                "c3", "leaf"),
    "cmp_cmov": ("8b02"           # mov eax, [edx]
                 "3b01"           # cmp eax, [ecx]
                 "0f4cca"         # cmovl ecx, edx
                 "8bc1"           # mov eax, ecx
                 "c3", "leaf"),
    "xchg": ("91"                 # xchg eax, ecx
             "c3", "leaf"),
    "guard_cookie": ("3b0d" + le32(G_COOKIE) +  # cmp ecx, [G_COOKIE]
                     "7501"       # jne fail
                     "c3" +       # ret
                     "c705" + le32(G_FAIL) + "adde0000"  # fail: mov [G_FAIL], 0xdead
                     "b8efbe0000"  # mov eax, 0xbeef
                     "c3", "guard"),
    "guard_target": ("85c9"       # test ecx, ecx
                     "7405"       # jz done
                     "8b01"       # mov eax, [ecx]
                     "8902"       # mov [edx], eax
                     "c3"         # ret
                     "c3", "guard"),   # done: ret
    "reads_esp0": ("8b0424"       # mov eax, [esp]
                   "c3", None),
    "lea_esp": ("8d442404"        # lea eax, [esp+4]
                "c3", None),
    "mov_esp": ("8bc4"            # mov eax, esp
                "c3", None),
    "nine": ("40" * 8 + "c3", None),         # 8 x inc eax + ret
    "calls": ("e8{getter}"        # call getter
              "c3", None),
    "x87": ("d901"                # fld dword [ecx]
            "d91a"                # fstp dword [edx]
            "c3", None),
    "string": ("f3a5"             # rep movsd
               "c3", None),
}

# Callers: name -> (bytes hex with {callee} placeholders, expected inlined
# sites as a list of callee names, register overrides for the harness).
# Every caller keeps flags dead after each call unless the case says so.
# RENDER_ONLY cases are compiled and text-checked but never executed.
RENDER_ONLY = frozenset(("flags_cf_consumed_after",))
CALLERS = [
    ("getter_twice",
     "8bce"                  # mov ecx, esi
     "e8{getter}"            # call getter
     "894640"                # mov [esi+0x40], eax
     "8d4e10"                # lea ecx, [esi+0x10]
     "e8{getter}"            # call getter
     "894644"                # mov [esi+0x44], eax
     "c3", ["getter", "getter"], {}),
    ("global_add",
     "e8{global_add}"
     "894648"                # mov [esi+0x48], eax
     "c3", ["global_add"], {}),
    ("args_ret8",
     "6a07"                  # push 7
     "51"                    # push ecx
     "e8{args_ret8}"         # call args_ret8 (ret 8 cleans)
     "89464c"                # mov [esi+0x4c], eax
     "c3", ["args_ret8"], {}),
    ("flags_cf_dead_after",
     "e8{flags_cf}"
     "894650"                # mov [esi+0x50], eax
     "8bca"                  # mov ecx, edx
     "e8{flags_cf}"
     "894654"                # mov [esi+0x54], eax
     "c3", ["flags_cf", "flags_cf"], {}),
    ("flags_cf_consumed_after",
     "e8{flags_cf}"
     "7303"                  # jae +3   (reads CF right after the call)
     "894658"                # mov [esi+0x58], eax
     "c3", [], {}),
    ("partial",
     "e8{partial}"
     "89465c"                # mov [esi+0x5c], eax
     "895660"                # mov [esi+0x60], edx
     "c3", ["partial"], {}),
    ("push_pop",
     "e8{push_pop}"
     "894664"                # mov [esi+0x64], eax
     "c3", ["push_pop"], {}),
    ("sse",
     "8d5668"                # lea edx, [esi+0x68]
     "e8{sse}"
     "c3", ["sse"], {}),
    ("inc_mem",
     "8d4e6c"                # lea ecx, [esi+0x6c]
     "e8{inc_mem}"
     "e8{inc_mem}"
     "c3", ["inc_mem", "inc_mem"], {}),
    ("cmp_cmov",
     "e8{cmp_cmov}"
     "894670"                # mov [esi+0x70], eax
     "894e74"                # mov [esi+0x74], ecx
     "c3", ["cmp_cmov"], {}),
    ("xchg",
     "e8{xchg}"
     "894678"                # mov [esi+0x78], eax
     "894e7c"                # mov [esi+0x7c], ecx
     "c3", ["xchg"], {}),
    ("guard_cookie_both_paths",
     "8b0d" + le32(G_COOKIE) +  # mov ecx, [G_COOKIE]
     "e8{guard_cookie}"      # fast path
     "8946" + "80"           # mov [esi-0x80], eax
     "f7d1"                  # not ecx
     "e8{guard_cookie}"      # slow path (writes G_FAIL, eax = 0xbeef)
     "8946" + "84"           # mov [esi-0x7c], eax
     "c3", ["guard_cookie", "guard_cookie"], {}),
    ("guard_target_both_paths",
     "33c9"                  # xor ecx, ecx
     "e8{guard_target}"      # ret path (target)
     "8bce"                  # mov ecx, esi
     "8d5688"                # lea edx, [esi-0x78]
     "e8{guard_target}"      # body path
     "c3", ["guard_target", "guard_target"], {}),
    ("refused_shapes",
     "e8{reads_esp0}"
     "8946" + "8c"           # mov [esi-0x74], eax
     "e8{lea_esp}"
     "8946" + "90"           # mov [esi-0x70], eax
     "e8{mov_esp}"
     "8946" + "94"           # mov [esi-0x6c], eax
     "e8{nine}"
     "8946" + "98"           # mov [esi-0x68], eax
     "e8{calls}"
     "8946" + "9c"           # mov [esi-0x64], eax
     "8d56a0"                # lea edx, [esi-0x60]
     "e8{x87}"
     "c3", [], {}),
    ("refused_string",
     "fc"                    # cld
     "b904000000"            # mov ecx, 4
     "8d7e40"                # lea edi, [esi+0x40]
     "8d7680"                # lea esi, [esi-0x80]
     "e8{string}"
     "c3", [], {}),
]


def _layout():
    """Callee RVAs (each on its own 0x100 slot after the callers)."""
    layout = {}
    rva = TEXT + CASE_STRIDE * (len(CALLERS) + 1)
    for name in CALLEES:
        layout[name] = rva
        rva += CASE_STRIDE
    return layout


def _encode(hex_text, start, layout):
    out = bytearray()
    i = 0
    clean = hex_text.replace(" ", "")
    while i < len(clean):
        if clean[i] == "{":
            end = clean.index("}", i)
            target = layout[clean[i + 1:end]]
            site_end = start + len(out) + 4
            out += ((target - site_end) & 0xFFFFFFFF).to_bytes(4, "little")
            i = end + 1
        else:
            out += bytes.fromhex(clean[i:i + 2])
            i += 2
    return bytes(out)


def _emitter():
    import build_one as B                                    # noqa: E402
    import leaf_inline                                       # noqa: E402
    import trans as T                                        # noqa: E402
    return B, leaf_inline, T


def render_body(ctx, start, name, known):
    B, _leaf_inline, _T = _emitter()
    em, _insns, order, body, unsup, _indirect, _members = B.translate(
        ctx, start, name, known=known)
    if unsup:
        raise SystemExit("%s: unsupported: %s" % (name, unsup[0]))
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


def build_synthetic():
    B, leaf_inline, T = _emitter()
    layout = _layout()
    text = bytearray(CASE_STRIDE * (len(CALLERS) + 1 + len(CALLEES)))
    callers = {}
    for index, (name, hex_text, _expected, _overrides) in enumerate(CALLERS):
        rva = TEXT + CASE_STRIDE * (index + 1)
        callers[name] = rva
        code = _encode(hex_text, rva, layout)
        text[rva - TEXT:rva - TEXT + len(code)] = code
    for name, (hex_text, _verdict) in CALLEES.items():
        rva = layout[name]
        code = _encode(hex_text, rva, layout)
        text[rva - TEXT:rva - TEXT + len(code)] = code
    ctx = T.Ctx("synthetic", 0x400000, TEXT, TEXT + len(text), bytes(text))
    known = frozenset(layout.values()) | frozenset(callers.values())

    # The predicate, exactly as gen_all applies it (no seam step: the
    # synthetic bodies have none by construction).
    specs, reasons = {}, {}
    for name, rva in layout.items():
        spec, reason = leaf_inline.analyse(ctx, rva, known)
        expected = CALLEES[name][1]
        got = spec["kind"] if spec else None
        if got != expected:
            raise SystemExit("callee %s: predicate says %r (%s), expected %r"
                             % (name, got, reason, expected))
        if spec:
            specs[rva] = spec
        else:
            reasons[rva] = reason
    leaf_inline.TABLE = {"specs": specs, "reasons": reasons, "max_insns": 8}

    pieces = []
    for name, rva in layout.items():
        body, _em = render_body(ctx, rva, "sub_%08x" % rva, known)
        pieces.append(body)
    runs = []
    for name, _hex, expected, overrides in CALLERS:
        rva = callers[name]
        fn_name = "case_%s" % name
        body, em = render_body(ctx, rva, fn_name, known)
        inlined = [next(n for n, r in layout.items() if r == callee)
                   for _site, callee, _kind in getattr(em, "leaf_inlined", ())]
        if inlined != expected:
            raise SystemExit("caller %s: inlined %r, expected %r"
                             % (name, inlined, expected))
        if body.count(leaf_inline.PP_IF) != len(expected):
            raise SystemExit("caller %s: %d inline blocks rendered, expected %d"
                             % (name, body.count(leaf_inline.PP_IF),
                                len(expected)))
        pieces.append(body)
        if name not in RENDER_ONLY:
            runs.append((fn_name, overrides))
    leaf_inline.TABLE = None
    return "".join(pieces), runs, reasons


# --- harness ----------------------------------------------------------------

HARNESS_HEAD = r'''
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "guest.h"

#define BUF_SIZE (16u << 20)
static uint32_t BUF;
static uint32_t TIB;
static unsigned g_faults, g_int3, g_calls;
static uint32_t g_fault_addr, g_call_target;
static const char *g_fault_what;
static sigjmp_buf g_escape;
static volatile int g_armed;

void guest_fault(CPU *__restrict c, uint32_t addr, const char *what)
{
    g_faults++; g_fault_addr = addr; g_fault_what = what;
    st32(BUF + 0x100, c->eax); st32(BUF + 0x104, c->ecx);
    st32(BUF + 0x108, c->edx); st32(BUF + 0x10c, c->esp);
}
void guest_int3(CPU *__restrict c, uint32_t addr)
{
    g_int3++;
    c->eax ^= 0x11110000u + addr;
}
void guest_cpuid(CPU *__restrict c)
{
    c->eax = 1u; c->ebx = 0x756e6547u; c->ecx ^= 0x5a5au; c->edx = 0x49656e69u;
}
void guest_xgetbv(CPU *__restrict c) { c->eax = 7u + c->ecx; c->edx = 0u; }
void guest_call(CPU *__restrict c, uint32_t addr)
{
    uint32_t ret = ld32(c->esp);
    g_calls++; g_call_target = addr;
    c->esp += 4u;
    c->eax = addr ^ ret;
    c->ecx = ret;
    c->edx = c->esi + 0x40u;
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

static void on_segv(int sig, siginfo_t *info, void *context)
{
    (void)sig; (void)info; (void)context;
    if (g_armed) { g_armed = 0; siglongjmp(g_escape, 1); }
    _exit(3);
}

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

static void fill(uint32_t base, uint32_t size, uint32_t seed)
{
    unsigned char *p = (unsigned char *)(uintptr_t)base;
    uint32_t i;
    rng_state = seed;
    for (i = 0; i < size; i += 4) {
        uint32_t v = rnd();
        memcpy(p + i, &v, 4);
    }
}

static void randomise(CPU *cpu, uint32_t seed, uint32_t pointer_lo,
                      uint32_t pointer_span)
{
    unsigned i;
    rng_state = seed * 2654435761u + 12345u;
    memset(cpu, 0, sizeof *cpu);
    cpu->eax = BUF + pointer_lo + (rnd() % pointer_span & ~3u);
    cpu->ecx = BUF + pointer_lo + (rnd() % pointer_span & ~3u);
    cpu->edx = BUF + pointer_lo + (rnd() % pointer_span & ~3u);
    cpu->ebx = BUF + pointer_lo + (rnd() % pointer_span & ~3u);
    cpu->esi = BUF + pointer_lo + (rnd() % pointer_span & ~3u);
    cpu->edi = BUF + pointer_lo + (rnd() % pointer_span & ~3u);
    cpu->ebp = BUF + 0x800000u - (rnd() & 0xff0u);
    cpu->esp = BUF + 0x800000u - (rnd() & 0xff0u) - 0x100u;
    for (i = 0; i < 4; i++) {
        cpu->x[0].u32[i] = rnd(); cpu->x[1].u32[i] = rnd();
    }
    cpu->esp -= 4u;
    st32(cpu->esp, 0xDEADBEEFu);              /* the caller's return word */
}

static int run_one(const char *label, unsigned seed,
                   void (*fn)(CPU *__restrict), CPU *cpu)
{
    g_faults = g_int3 = g_calls = 0;
    g_fault_addr = g_call_target = 0; g_fault_what = NULL;
    if (sigsetjmp(g_escape, 1)) {
        printf("%s seed %u: SIGSEGV\n", label, seed);
        return 1;
    }
    g_armed = 1;
    fn(cpu);
    g_armed = 0;
    printf("%s seed %u: eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x "
           "esi=%08x edi=%08x x0=%08x.%08x.%08x.%08x x1=%08x.%08x.%08x.%08x "
           "st=%d faults=%u(%08x %s) int3=%u calls=%u(%08x)\n",
           label, seed, cpu->eax, cpu->ecx, cpu->edx, cpu->ebx, cpu->esp - BUF,
           cpu->ebp - BUF, cpu->esi, cpu->edi,
           cpu->x[0].u32[0], cpu->x[0].u32[1], cpu->x[0].u32[2], cpu->x[0].u32[3],
           cpu->x[1].u32[0], cpu->x[1].u32[1], cpu->x[1].u32[2], cpu->x[1].u32[3],
           cpu->st_top, g_faults, g_fault_addr, g_fault_what ? g_fault_what : "-",
           g_int3, g_calls, g_call_target);
    return 0;
}

static void drive(const char *label, void (*fn)(CPU *__restrict),
                  unsigned seeds, uint32_t pointer_lo, uint32_t pointer_span,
                  void (*setup)(CPU *))
{
    unsigned seed, segv = 0;
    CPU cpu;
    fill(BUF, BUF_SIZE, 0x9e3779b9u);
    st32(TIB, 0xFFFFFFFFu);
    for (seed = 0; seed < seeds; seed++) {
        randomise(&cpu, seed, pointer_lo, pointer_span);
        if (setup) setup(&cpu);
        segv += run_one(label, seed, fn, &cpu);
    }
    if (segv)
        printf("%s mem=skipped (%u seeds left the mapping)\n", label, segv);
    else
        printf("%s mem=%016llx\n", label,
               (unsigned long long)fnv((const unsigned char *)(uintptr_t)BUF,
                                       BUF_SIZE));
}
'''

MAIN_HEAD = r'''
int main(void)
{
    struct sigaction action;
    void *map = mmap((void *)(uintptr_t)%#xu, BUF_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (map == MAP_FAILED || (uintptr_t)map != %#xu) {
        fputs("cannot map the guest buffer at its fixed address\n", stderr);
        return 2;
    }
    BUF = (uint32_t)(uintptr_t)map;
    TIB = BUF + 0x10000u;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = on_segv;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
%s
''' % (BUF_FIXED, BUF_FIXED, "%s")


def synthetic_harness(rendered, runs):
    parts = [HARNESS_HEAD, rendered]
    setups, calls = [], []
    for index, (fn_name, overrides) in enumerate(runs):
        body = "".join("    c->%s = (uint32_t)(%s);\n" % (reg, expr)
                       for reg, expr in sorted(overrides.items()))
        setups.append("static void setup_%d(CPU *c) { (void)c;\n%s}\n"
                      % (index, body))
        calls.append('    drive("%s", %s, %du, 0x200000u, 0x400000u, setup_%d);\n'
                     % (fn_name, fn_name, SEEDS_SYNTHETIC, index))
    parts.append("".join(setups))
    parts.append(MAIN_HEAD % "".join(calls))
    parts.append('    puts("leaf inline oracle: done");\n    return 0;\n}\n')
    return "".join(parts)


# --- PE leaves ------------------------------------------------------------

class _Overlay:
    """The frozen image plus a synthetic caller region that is decoded (never
    executed): `call leaf ; ret`, optionally preceded by a load of the guard's
    compare operand into ecx so the fast path is the one exercised."""

    def __init__(self, img, blobs):
        self._img = img
        self._blobs = blobs                 # rva -> bytes

    def __getattr__(self, name):
        return getattr(self._img, name)

    def _blob(self, rva):
        for start, blob in self._blobs.items():
            if start <= rva < start + len(blob):
                return start, blob
        return None

    def is_code(self, rva):
        return self._blob(rva) is not None or self._img.is_code(rva)

    def code_at(self, rva, n):
        hit = self._blob(rva)
        if hit is None:
            return self._img.code_at(rva, n)
        start, blob = hit
        chunk = blob[rva - start:rva - start + n]
        return chunk + b"\xcc" * (n - len(chunk))


def _guard_operand_va(img, rva):
    """Absolute VA of the guard compare's memory operand, or None."""
    from capstone import x86 as cx
    _B, _leaf_inline, T = _emitter()
    insns, order, _indirect = T.decode(img, rva)
    producer = insns[order[0]]
    for op in producer.operands:
        if (op.type == cx.X86_OP_MEM and
                op.mem.base in (0, cx.X86_REG_INVALID) and
                op.mem.index in (0, cx.X86_REG_INVALID) and
                op.mem.segment in (0, cx.X86_REG_INVALID)):
            return op.mem.disp & 0xFFFFFFFF
    return None


def build_pe(pe_path, census_path, seed):
    from image import Image
    _B, leaf_inline, _T = _emitter()
    census = json.loads(pathlib.Path(census_path).read_text(encoding="utf-8"))
    eligible = census["eligible"]
    by_sites = sorted(eligible.items(), key=lambda item: (-item[1]["sites"],
                                                          item[0]))
    chosen = [rva for rva, _info in by_sites[:PE_TOP]]
    rest = [rva for rva, _info in by_sites[PE_TOP:]]
    random.Random(seed).shuffle(rest)
    chosen.extend(rest[:PE_RANDOM])
    img = Image(os.fspath(pe_path), 0x98000000)

    blobs = {}
    callers = []
    overlay_rva = PE_OVERLAY_RVA
    for hex_rva in chosen:
        rva = int(hex_rva, 16)
        kind = eligible[hex_rva]["kind"]
        prefix = b""
        if kind == leaf_inline.KIND_GUARD:
            operand = _guard_operand_va(img, rva)
            if operand is None:
                continue
            prefix = b"\x8b\x0d" + operand.to_bytes(4, "little")
        site_end = overlay_rva + len(prefix) + 5
        code = prefix + b"\xe8" + ((rva - site_end) & 0xFFFFFFFF).to_bytes(
            4, "little") + b"\xc3"
        blobs[overlay_rva] = code
        callers.append((overlay_rva, rva, kind))
        overlay_rva += 0x40
    ctx = _Overlay(img, blobs)
    known = frozenset(rva for _o, rva, _k in callers) | \
        frozenset(o for o, _r, _k in callers)

    specs = {}
    for _o, rva, kind in callers:
        spec, reason = leaf_inline.analyse(ctx, rva, None)
        if spec is None or spec["kind"] != kind:
            raise SystemExit("census leaf %08x no longer eligible: %s"
                             % (rva, reason))
        specs[rva] = spec
    leaf_inline.TABLE = {"specs": specs, "reasons": {}, "max_insns":
                         census["max_insns"]}
    pieces, runs = [], []
    rendered = frozenset(rva for _o, rva, _k in callers)
    stubs = set()
    for overlay, rva, kind in callers:
        body, callee_em = render_body(ctx, rva, "sub_%08x" % rva, None)
        pieces.append(body)
        # A guard leaf's out-of-line body calls its slow path (the cookie
        # failure handler); stand-ins below record the call and pop the
        # return word, identically in every binary.
        stubs.update(set(callee_em.calls) - rendered)
        caller_name = "call_%08x" % rva
        body, em = render_body(ctx, overlay, caller_name, known)
        if len(getattr(em, "leaf_inlined", ())) != 1:
            raise SystemExit("overlay caller of %08x was not inlined" % rva)
        pieces.append(body)
        runs.append((caller_name, kind))
    leaf_inline.TABLE = None
    prototypes = "".join("void sub_%08x(CPU *__restrict c);\n" % target
                         for target in sorted(stubs))
    stand_ins = "".join(
        "void sub_%08x(CPU *__restrict c)\n{\n    g_calls++; "
        "g_call_target = 0x%xu; c->esp += 4u;\n}\n" % (target, target)
        for target in sorted(stubs))
    image_size = (len(img.mem) + 0xFFF) & ~0xFFF
    return prototypes + "".join(pieces) + stand_ins, runs, image_size, img.base


def pe_harness(rendered, runs, image_size, image_base):
    parts = [HARNESS_HEAD, rendered]
    calls = [r'''
    {
        void *image = mmap((void *)(uintptr_t)%#xu, %#xu, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (image == MAP_FAILED || (uintptr_t)image != %#xu) {
            fputs("cannot map the image range\n", stderr);
            return 2;
        }
        fill(%#xu, %#xu, 0x2545f491u);
    }
''' % (image_base, image_size, image_base, image_base, image_size)]
    for fn_name, _kind in runs:
        calls.append('    drive("%s", %s, %du, 0x400000u, 0x400000u, NULL);\n'
                     % (fn_name, fn_name, SEEDS_PE))
    parts.append(MAIN_HEAD % "".join(calls))
    parts.append('    puts("leaf inline oracle: done");\n    return 0;\n}\n')
    return "".join(parts)


# --- driver -----------------------------------------------------------------

MODES = (
    ("call/memory", 0, 0),
    ("inline/memory", 1, 0),
    ("call/locals", 0, 1),
    ("inline/locals", 1, 1),
)


def compile_and_run(cc, source, work, tag):
    source_path = work / ("leaf_oracle_%s.c" % tag)
    source_path.write_text(source, encoding="ascii", newline="\n")
    outputs = {}
    for label, inline, locals_mode in MODES:
        binary = work / ("leaf_oracle_%s_%d_%d" % (tag, inline, locals_mode))
        command = [
            cc, "-std=gnu11", "-O2", "-Wall", "-Wextra",
            "-Wno-unused-variable", "-Wno-unused-but-set-variable",
            "-Wno-unused-label", "-Wno-unused-parameter",
            "-DGUEST_GENERATED_STACK_GUARD=0", "-DGUEST_STACK_REQUIRED=0",
            "-DGUEST_LEAF_INLINE=%d" % inline,
            "-DGUEST_GPR_LOCAL=%d" % locals_mode,
            "-DGUEST_FLAGS_LOCAL=%d" % locals_mode,
            "-I", os.fspath(HERE / "runtime"), os.fspath(source_path),
            "-o", os.fspath(binary), "-lm",
        ]
        built = subprocess.run(command, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True,
                               check=False)
        if built.stdout:
            print(built.stdout, end="")
        if built.returncode:
            raise SystemExit("leaf oracle compile failed (%s)" % label)
        ran = subprocess.run([os.fspath(binary)], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True,
                             check=False, timeout=1800)
        if ran.returncode:
            print(ran.stdout[-4000:], end="")
            raise SystemExit("leaf oracle run failed (%s)" % label)
        outputs[label] = ran.stdout
    reference = outputs[MODES[0][0]]
    for label, _inline, _locals_mode in MODES[1:]:
        if outputs[label] != reference:
            import difflib
            sys.stdout.writelines(difflib.unified_diff(
                reference.splitlines(True), outputs[label].splitlines(True),
                MODES[0][0], label, n=1))
            raise SystemExit("leaf inline oracle: %s differs from %s"
                             % (label, MODES[0][0]))
    return reference


def summarise(output, label):
    seeds = len(re.findall(r" seed \d+: eax=", output))
    segv = len(re.findall(r" seed \d+: SIGSEGV", output))
    hashes = len(re.findall(r" mem=[0-9a-f]{16}", output))
    skipped = len(re.findall(r" mem=skipped", output))
    print("leaf inline oracle (%s): %d seeds compared, %d left the mapping, "
          "%d memory hashes compared, %d skipped; identical in %d binaries"
          % (label, seeds, segv, hashes, skipped, len(MODES)))
    if hashes == 0:
        raise SystemExit("leaf inline oracle: no memory hash was compared")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", help="GCC-compatible compiler (Linux x86-64); "
                        "omit to only render")
    parser.add_argument("--pe", type=pathlib.Path,
                        help="frozen isaac-ng.exe.unpacked.exe")
    parser.add_argument("--census", type=pathlib.Path,
                        help="leaf_inline_census.json from a GUEST_LEAF_INLINE=1 "
                        "generation (with --pe)")
    parser.add_argument("--seed", type=int, default=20260903)
    parser.add_argument("--emit", type=pathlib.Path,
                        help="directory to write the harness sources into")
    parser.add_argument("--prerendered", type=pathlib.Path,
                        help="compile and run harness sources written earlier "
                        "by --emit (no capstone needed)")
    arguments = parser.parse_args()

    if arguments.prerendered is not None:
        if arguments.cc is None:
            raise SystemExit("--prerendered needs --cc")
        sources = []
        for tag in ("synthetic", "pe"):
            path = arguments.prerendered / ("leaf_oracle_%s.c" % tag)
            if path.exists():
                sources.append((tag, path.read_text(encoding="ascii")))
        if not sources:
            raise SystemExit("no harness sources under %s" % arguments.prerendered)
        with tempfile.TemporaryDirectory(prefix="isaac-leaf-oracle-") as work_raw:
            work = pathlib.Path(work_raw)
            for tag, source in sources:
                summarise(compile_and_run(arguments.cc, source, work, tag), tag)
        print("leaf inline oracle: PASS")
        return 0

    rendered, runs, reasons = build_synthetic()
    sources = [("synthetic", synthetic_harness(rendered, runs))]
    refused = sorted(set(reasons.values()))
    print("leaf inline oracle: %d synthetic callers, %d callees (%d refused: %s)"
          % (len(CALLERS), len(CALLEES), len(reasons), "; ".join(refused)))
    if arguments.pe is not None:
        if arguments.census is None:
            raise SystemExit("--pe needs --census")
        pe_rendered, pe_runs, image_size, image_base = build_pe(
            arguments.pe, arguments.census, arguments.seed)
        sources.append(("pe", pe_harness(pe_rendered, pe_runs, image_size,
                                         image_base)))
        kinds = {}
        for _name, kind in pe_runs:
            kinds[kind] = kinds.get(kind, 0) + 1
        print("leaf inline oracle: %d PE leaves rendered (%s)"
              % (len(pe_runs), ", ".join("%d %s" % (n, k)
                                         for k, n in sorted(kinds.items()))))
    if arguments.emit is not None:
        arguments.emit.mkdir(parents=True, exist_ok=True)
        for tag, source in sources:
            (arguments.emit / ("leaf_oracle_%s.c" % tag)).write_text(
                source, encoding="ascii", newline="\n")
        print("harness sources written under %s" % arguments.emit)
    if arguments.cc is None:
        print("leaf inline oracle: rendered only (no --cc, not run)")
        return 0
    with tempfile.TemporaryDirectory(prefix="isaac-leaf-oracle-") as work_raw:
        work = pathlib.Path(work_raw)
        for tag, source in sources:
            output = compile_and_run(arguments.cc, source, work, tag)
            summarise(output, tag)
    print("leaf inline oracle: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
