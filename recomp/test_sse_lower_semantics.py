#!/usr/bin/env python3
"""Differential oracle for the SSE lowering (recomp/sse_lower.py, guest.h
GUEST_XMM_* tokens, comis/jcc pairing).

Every shape the emitter may spell through a token is rendered TWICE through
the real decoder, block builder and emitter -- once with the generation switch
GUEST_SSE_LOWER=0 (the legacy lane loops / union copies / lazy comis producer)
and once with GUEST_SSE_LOWER=1 (the tokens) -- into one C harness that also
carries the x86 truth: the same instruction executed natively through inline
assembly on the x86-64 host.  The harness is compiled twice, with the header
knob GUEST_SSE_LOWER=0 (tokens expand to the legacy statements) and =1 (GCC
vector spelling, NEON on the target), and each binary runs every case on the
same seeded operands:

  * every lane of every xmm register, the 16 bytes of a stored operand and
    EAX after legacy, token and truth must agree byte for byte -- for the
    *0 (lane-0) tokens lane 0 of the destination and every other register
    must agree (lanes 1..3 of the destination are proven unobserved by the
    taint analysis, gated separately by test_xmm_zero_elision.py);
  * operands: random bit patterns and edge lanes (+-0, +-inf, quiet and
    signalling NaNs with several payloads, min/max denormals, INT_MIN/MAX,
    all-ones, 1.0, 2^24, 2^31 as float; the double equivalents for comisd),
    dst == src, and every memory alignment 0..15 for the memory forms;
  * immediates: all 256 for pshufd/shufps/pshuflw/pshufhw, 1..15 for
    psrldq, every in-range count for the lane shifts;
  * comiss/ucomiss/comisd/ucomisd paired with each of the eight CF/ZF/PF
    conditions as jcc, setcc and cmovcc, plus the refused shapes (SF/OF
    conditions, an operand rewritten between producer and consumer, a
    second consumer that keeps the flags live) which must still be correct
    through the lazy producer;
  * cvtdq2ps exhaustively over all 2^32 lane values against the native
    instruction (--no-exhaustive-cvt skips it);
  * scalar float/double memory forms (movss/movsd load and store, addss..
    sqrtss/cvtss2sd/cvttss2si/addsd/cvtsd2ss with a memory operand): the
    guest.h ldf/stf/ldd/std_ accessors at every alignment, compiled once
    more with GUEST_F32_VFP_ACCESS=1 (aligned VFP access + cold misaligned
    fallback) -- the four knob combinations must print identical output.

The two binaries must print identical output.  Host requirements: a GCC-
compatible compiler on Linux x86-64 (guest address == host address, so guest
memory is an mmap(MAP_32BIT) buffer), exactly like test_gpr_locals_semantics.
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
import sse_lower                                             # noqa: E402
import trans as T                                            # noqa: E402

TEXT = 0x1000

# ----------------------------------------------------------------- encode --
# Hand encoders for the shapes under test (x86-32, xmm register indices).

def _modrm_rr(d, s):
    return bytes([0xC0 | (d << 3) | s])


def _modrm_mem_ecx(reg):
    return bytes([0x00 | (reg << 3) | 0x01])            # [ecx]


def _modrm_mem_edx(reg):
    return bytes([0x00 | (reg << 3) | 0x02])            # [edx]


OPCODES = {
    # mnemonic: (prefix bytes, opcode bytes)
    "xorps": (b"", b"\x0f\x57"), "andps": (b"", b"\x0f\x54"),
    "orps": (b"", b"\x0f\x56"), "andnps": (b"", b"\x0f\x55"),
    "xorpd": (b"\x66", b"\x0f\x57"), "andpd": (b"\x66", b"\x0f\x54"),
    "orpd": (b"\x66", b"\x0f\x56"), "andnpd": (b"\x66", b"\x0f\x55"),
    "pxor": (b"\x66", b"\x0f\xef"), "pand": (b"\x66", b"\x0f\xdb"),
    "por": (b"\x66", b"\x0f\xeb"), "pandn": (b"\x66", b"\x0f\xdf"),
    "paddd": (b"\x66", b"\x0f\xfe"), "psubd": (b"\x66", b"\x0f\xfa"),
    "paddw": (b"\x66", b"\x0f\xfd"), "psubw": (b"\x66", b"\x0f\xf9"),
    "paddb": (b"\x66", b"\x0f\xfc"), "psubb": (b"\x66", b"\x0f\xf8"),
    "paddq": (b"\x66", b"\x0f\xd4"),
    "movaps": (b"", b"\x0f\x28"), "movups": (b"", b"\x0f\x10"),
    "movdqa": (b"\x66", b"\x0f\x6f"), "movdqu": (b"\xf3", b"\x0f\x6f"),
    "cvtdq2ps": (b"", b"\x0f\x5b"),
    "pshufd": (b"\x66", b"\x0f\x70"), "shufps": (b"", b"\x0f\xc6"),
    "pshuflw": (b"\xf2", b"\x0f\x70"), "pshufhw": (b"\xf3", b"\x0f\x70"),
    "unpcklps": (b"", b"\x0f\x14"), "unpckhps": (b"", b"\x0f\x15"),
    "unpcklpd": (b"\x66", b"\x0f\x14"), "unpckhpd": (b"\x66", b"\x0f\x15"),
    "punpcklbw": (b"\x66", b"\x0f\x60"), "punpcklwd": (b"\x66", b"\x0f\x61"),
    "punpckldq": (b"\x66", b"\x0f\x62"), "punpckhwd": (b"\x66", b"\x0f\x69"),
    "punpckhdq": (b"\x66", b"\x0f\x6a"),
    "comiss": (b"", b"\x0f\x2f"), "ucomiss": (b"", b"\x0f\x2e"),
    "comisd": (b"\x66", b"\x0f\x2f"), "ucomisd": (b"\x66", b"\x0f\x2e"),
    # scalar float/double forms: the memory operand goes through guest.h
    # ldf/ldd (loads) and stf/std_ (stores), the GUEST_F32_VFP_ACCESS knob
    "movss": (b"\xf3", b"\x0f\x10"), "movsd": (b"\xf2", b"\x0f\x10"),
    "addss": (b"\xf3", b"\x0f\x58"), "subss": (b"\xf3", b"\x0f\x5c"),
    "mulss": (b"\xf3", b"\x0f\x59"), "divss": (b"\xf3", b"\x0f\x5e"),
    "sqrtss": (b"\xf3", b"\x0f\x51"), "cvtss2sd": (b"\xf3", b"\x0f\x5a"),
    "addsd": (b"\xf2", b"\x0f\x58"), "cvtsd2ss": (b"\xf2", b"\x0f\x5a"),
    "cvttss2si": (b"\xf3", b"\x0f\x2c"),
}
# addss/mulss are left out: with two NaN operands x86 keeps the destination's
# payload while C's commutative `+`/`*` lets GCC pick either operand order (a
# property of the emitter's scalar spelling under every knob), so those shapes
# cannot be compared bit for bit against native code.  subss/divss/sqrtss/
# cvtss2sd fix the operand order and cover the same ldf() load.
SCALAR_F32_MEM = ("subss", "divss", "sqrtss", "cvtss2sd")
# Group 12/13/14 immediate shifts: (prefix, opcode, /digit)
SHIFT_IMM = {
    "psrlw": (b"\x66", b"\x0f\x71", 2), "psraw": (b"\x66", b"\x0f\x71", 4),
    "psllw": (b"\x66", b"\x0f\x71", 6),
    "psrld": (b"\x66", b"\x0f\x72", 2), "psrad": (b"\x66", b"\x0f\x72", 4),
    "pslld": (b"\x66", b"\x0f\x72", 6),
    "psrldq": (b"\x66", b"\x0f\x73", 3),
}
STORE_MOVUPS_EDX = {reg: b"\x0f\x11" + _modrm_mem_edx(reg) for reg in range(8)}
SCALAR_STORE_EDX = {reg: b"\xf3\x0f\x11" + _modrm_mem_edx(reg) for reg in range(8)}
KILL = {reg: b"\x0f\x57" + _modrm_rr(reg, reg) for reg in range(8)}   # xorps r, r
RET = b"\xc3"
JCC = {"a": 0x77, "ae": 0x73, "b": 0x72, "be": 0x76, "e": 0x74, "ne": 0x75,
       "p": 0x7a, "np": 0x7b, "g": 0x7f, "ge": 0x7d, "l": 0x7c, "le": 0x7e,
       "s": 0x78, "ns": 0x79, "o": 0x70, "no": 0x71}
SETCC = {cc: bytes([0x0f, 0x90 | (op & 0x0f)]) for cc, op in JCC.items()}
CMOVCC = {cc: bytes([0x0f, 0x40 | (op & 0x0f)]) for cc, op in JCC.items()}


def enc_rr(m, d, s, imm=None):
    pre, op = OPCODES[m]
    out = pre + op + _modrm_rr(d, s)
    if imm is not None:
        out += bytes([imm])
    return out


def enc_rm(m, d, imm=None):
    """`m xmm_d, [ecx]`"""
    pre, op = OPCODES[m]
    out = pre + op + _modrm_mem_ecx(d)
    if imm is not None:
        out += bytes([imm])
    return out


def enc_store(m, s):
    """`m [ecx], xmm_s` for the 16-byte moves."""
    pre, op = OPCODES[m]
    store_op = bytes([op[0], 0x7f if m in ("movdqa", "movdqu") else op[1] + 1])
    return pre + store_op + _modrm_mem_ecx(s)


def enc_shift_imm(m, d, count):
    pre, op, digit = SHIFT_IMM[m]
    return pre + op + bytes([0xC0 | (digit << 3) | d, count])


SHIFT_REG = {"psrlw": 0xd1, "psraw": 0xe1, "psllw": 0xf1,
             "psrld": 0xd2, "psrad": 0xe2, "pslld": 0xf2}


def enc_shift_reg(m, d, s):
    """`m xmm_d, xmm_s` (the count is the low qword of xmm_s)."""
    return b"\x66\x0f" + bytes([SHIFT_REG[m]]) + _modrm_rr(d, s)


AND_EAX_63 = b"\x83\xe0\x3f"                     # and eax, 63
AND_EAX_31 = b"\x83\xe0\x1f"                     # and eax, 31
MOVD_XMM_EAX = {reg: b"\x66\x0f\x6e" + bytes([0xC0 | (reg << 3)])
                for reg in range(8)}               # movd xmm_reg, eax


# ------------------------------------------------------------------ truth --
# Intel-syntax inline assembly executing the instruction natively.  Registers
# xmm0..xmm7 are loaded from c->x[], the instruction runs, all eight are
# stored back.  Memory forms use the same [ecx]/[edx] addresses (guest ==
# host address; `movups` for the memory transfer so alignment never traps).

TRUTH_HEAD = r'''
static void truth_load(CPU *c, uint8_t *m)
{
    memcpy(m, c->x, 128);
}
'''


def truth_body(asm_lines):
    lines = [
        "    uint8_t regs[128];",
        "    truth_load(c, regs);",
        "    __asm__ volatile (",
        '        ".intel_syntax noprefix\\n\\t"',
        '        "movups xmm0, [%[r] + 0]\\n\\t"',
        '        "movups xmm1, [%[r] + 16]\\n\\t"',
        '        "movups xmm2, [%[r] + 32]\\n\\t"',
        '        "movups xmm3, [%[r] + 48]\\n\\t"',
        '        "movups xmm4, [%[r] + 64]\\n\\t"',
        '        "movups xmm5, [%[r] + 80]\\n\\t"',
        '        "movups xmm6, [%[r] + 96]\\n\\t"',
        '        "movups xmm7, [%[r] + 112]\\n\\t"',
    ]
    for line in asm_lines:
        lines.append('        "%s\\n\\t"' % line)
    lines += [
        '        "movups [%[r] + 0], xmm0\\n\\t"',
        '        "movups [%[r] + 16], xmm1\\n\\t"',
        '        "movups [%[r] + 32], xmm2\\n\\t"',
        '        "movups [%[r] + 48], xmm3\\n\\t"',
        '        "movups [%[r] + 64], xmm4\\n\\t"',
        '        "movups [%[r] + 80], xmm5\\n\\t"',
        '        "movups [%[r] + 96], xmm6\\n\\t"',
        '        "movups [%[r] + 112], xmm7\\n\\t"',
        '        ".att_syntax prefix\\n\\t"',
        '        : [eax] "+a"(c->eax), [edx] "+d"(c->edx)',
        '        : [r] "r"(regs), [ecx] "c"(c->ecx)',
        '        : "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3",',
        '          "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "rbx");',
        "    memcpy(c->x, regs, 128);",
    ]
    return lines


# ------------------------------------------------------------------ cases --
# Each case: name, x86 bytes, truth asm lines (Intel syntax), compare mode,
# expected emitter facts (token names that must / must not appear, pairing).
#   compare: "full"  -> all eight registers, memory at [ecx] 32 bytes, eax
#            ("lane0", d) -> lane 0 of x[d] and every other register whole
# The token text of a case is inspected by the Python side so the analysis
# verdicts (lane-0 or full, paired or lazy) are pinned, not just executed.

LANEOP_MNEMONICS = ("xorps", "andps", "orps", "xorpd", "andpd", "orpd",
                    "pxor", "pand", "por", "paddd", "psubd", "paddw", "psubw",
                    "paddb", "psubb", "paddq")
PANDN_MNEMONICS = ("pandn",)          # andnps/andnpd: emitter path checked below
MOVE_MNEMONICS = ("movaps", "movups", "movdqa", "movdqu")
UNPCK_MNEMONICS = ("unpcklps", "unpckhps", "unpcklpd", "unpckhpd",
                   "punpcklbw", "punpcklwd", "punpckhwd", "punpckhdq")
SHIFT_BITS = {"psrlw": 16, "psraw": 16, "psllw": 16,
              "psrld": 32, "psrad": 32, "pslld": 32}
COMIS_MNEMONICS = ("comiss", "ucomiss", "comisd", "ucomisd")
FLOAT_CCS = ("a", "ae", "b", "be", "e", "ne", "p", "np")
REFUSED_CCS = ("g", "ge", "l", "le", "s", "ns", "o", "no")


def asm_rr(m, d, s, imm=None):
    text = "%s xmm%d, xmm%d" % (m, d, s)
    if imm is not None:
        text += ", %d" % imm
    return [text]


def asm_rm(m, d, imm=None):
    """Memory source: load the 16 bytes through movups into a scratch
    register outside the guest's eight (xmm8), then the register form.
    The 16-byte moves themselves take the memory operand directly."""
    if m in MOVE_MNEMONICS:
        # movups for the transfer: the harness deliberately runs every
        # alignment 0..15 and the emitter's ldx/stx never fault on alignment.
        return ["movups xmm%d, [%%[ecx]]" % d]
    lines = ["movups xmm8, [%[ecx]]"]                # xmm8: not guest state
    text = "%s xmm%d, xmm8" % (m, d)
    if imm is not None:
        text += ", %d" % imm
    lines.append(text)
    return lines


def build_cases():
    cases = []

    def add(name, code, asm, compare, expect):
        cases.append((name, code, asm, compare, expect))

    store = lambda d: STORE_MOVUPS_EDX[d]          # noqa: E731
    store_asm = lambda d: ["movups [%%[edx]], xmm%d" % d]     # noqa: E731
    scalar_store_asm = lambda d: ["movss [%%[edx]], xmm%d" % d]   # noqa: E731
    kill_asm = lambda d: ["xorps xmm%d, xmm%d" % (d, d)]     # noqa: E731

    # -- lane ops (bitwise / integer add-sub): full, memory, dst==src, lane-0
    for m in LANEOP_MNEMONICS:
        add("%s_rr" % m, enc_rr(m, 3, 1) + store(3) + RET,
            asm_rr(m, 3, 1) + store_asm(3), "full",
            {"tokens": ["GUEST_XMM_LANEOP_R("], "not": ["LANEOP0"]})
        add("%s_rm" % m, enc_rm(m, 3) + store(3) + RET,
            asm_rm(m, 3) + store_asm(3), "full",
            {"tokens": ["GUEST_XMM_LANEOP_M("]})
        add("%s_same" % m, enc_rr(m, 3, 3) + store(3) + RET,
            asm_rr(m, 3, 3) + store_asm(3), "full",
            {"tokens": ([] if m in ("xorps", "xorpd", "pxor")
                        else ["GUEST_XMM_LANEOP_R("])})
        if m in sse_lower.LANE0_LANEOPS:
            # lane-0 shapes: lane 0 consumed by a scalar store, then the
            # register killed (zeroing idiom) so no lane-0 result is live at
            # ret -- the analysis pins live lane-0 results at call/ret/tail.
            add("%s_lane0" % m, enc_rr(m, 2, 1) + SCALAR_STORE_EDX[2] + KILL[2] + RET,
                asm_rr(m, 2, 1) + scalar_store_asm(2) + kill_asm(2), "full",
                {"tokens": ["GUEST_XMM_LANEOP0_R("]})
            add("%s_lane0_m" % m, enc_rm(m, 2) + SCALAR_STORE_EDX[2] + KILL[2] + RET,
                asm_rm(m, 2) + scalar_store_asm(2) + kill_asm(2), "full",
                {"tokens": ["GUEST_XMM_LANEOP0_M("]})
            add("%s_live_at_ret" % m, enc_rr(m, 2, 1) + SCALAR_STORE_EDX[2] + RET,
                asm_rr(m, 2, 1) + scalar_store_asm(2), "full",
                {"tokens": ["GUEST_XMM_LANEOP_R("], "not": ["LANEOP0"]})
    # -- pandn (Intel operand order: (~dst) & src)
    add("pandn_rr", enc_rr("pandn", 3, 1) + store(3) + RET,
        asm_rr("pandn", 3, 1) + store_asm(3), "full",
        {"tokens": ["GUEST_XMM_PANDN_R("]})
    add("pandn_rm", enc_rm("pandn", 3) + store(3) + RET,
        asm_rm("pandn", 3) + store_asm(3), "full",
        {"tokens": ["GUEST_XMM_PANDN_M("]})
    add("pandn_same", enc_rr("pandn", 3, 3) + store(3) + RET,
        asm_rr("pandn", 3, 3) + store_asm(3), "full",
        {"tokens": ["GUEST_XMM_PANDN_R("]})
    # -- 16-byte moves
    for m in MOVE_MNEMONICS:
        add("%s_rr" % m, enc_rr(m, 3, 1) + store(3) + RET,
            asm_rr(m, 3, 1) + store_asm(3), "full",
            {"tokens": ["GUEST_XMM_MOV(3, 1);"]})
        add("%s_rr_lane0" % m, enc_rr(m, 3, 1) + SCALAR_STORE_EDX[3] + KILL[3] + RET,
            asm_rr(m, 3, 1) + scalar_store_asm(3) + kill_asm(3), "full",
            {"tokens": ["GUEST_XMM_MOV0(3, 1);"]})
        add("%s_rr_live_at_ret" % m, enc_rr(m, 3, 1) + SCALAR_STORE_EDX[3] + RET,
            asm_rr(m, 3, 1) + scalar_store_asm(3), "full",
            {"tokens": ["GUEST_XMM_MOV(3, 1);"], "not": ["MOV0"]})
        add("%s_load" % m, enc_rm(m, 3) + store(3) + RET,
            asm_rm(m, 3) + store_asm(3), "full",
            {"tokens": ["GUEST_XMM_LDX(3,"]})
        add("%s_store" % m, enc_store(m, 1) + RET,
            ["movups [%[ecx]], xmm1"], "full",
            {"tokens": ["GUEST_XMM_STX("]})
    # -- cvtdq2ps
    add("cvtdq2ps_rr", enc_rr("cvtdq2ps", 3, 1) + store(3) + RET,
        asm_rr("cvtdq2ps", 3, 1) + store_asm(3), "full",
        {"tokens": ["GUEST_XMM_CVTDQ2PS_R(3, 1);"]})
    add("cvtdq2ps_rm", enc_rm("cvtdq2ps", 3) + store(3) + RET,
        asm_rm("cvtdq2ps", 3) + store_asm(3), "full",
        {"tokens": ["GUEST_XMM_CVTDQ2PS_M(3,"]})
    add("cvtdq2ps_same", enc_rr("cvtdq2ps", 3, 3) + store(3) + RET,
        asm_rr("cvtdq2ps", 3, 3) + store_asm(3), "full",
        {"tokens": ["GUEST_XMM_CVTDQ2PS_R(3, 3);"]})
    add("cvtdq2ps_lane0", enc_rr("cvtdq2ps", 2, 1) + SCALAR_STORE_EDX[2] + KILL[2] + RET,
        asm_rr("cvtdq2ps", 2, 1) + scalar_store_asm(2) + kill_asm(2), "full",
        {"tokens": ["GUEST_XMM_CVTDQ2PS0_R(2, 1);"]})
    add("cvtdq2ps_lane0_m", enc_rm("cvtdq2ps", 2) + SCALAR_STORE_EDX[2] + KILL[2] + RET,
        asm_rm("cvtdq2ps", 2) + scalar_store_asm(2) + kill_asm(2), "full",
        {"tokens": ["GUEST_XMM_CVTDQ2PS0_M(2,"]})
    add("cvtdq2ps_live_at_ret", enc_rr("cvtdq2ps", 2, 1) + SCALAR_STORE_EDX[2] + RET,
        asm_rr("cvtdq2ps", 2, 1) + scalar_store_asm(2), "full",
        {"tokens": ["GUEST_XMM_CVTDQ2PS_R(2, 1);"], "not": ["CVTDQ2PS0"]})
    # movd + cvtdq2ps idiom: the source's upper lanes are architecturally
    # zero, so the lane-0 conversion is exact in every lane even when the
    # whole register is stored afterwards and stays live at ret.
    add("movd_cvtdq2ps_scalar",
        b"\x66\x0f\x6e\xc8"                    # movd xmm1, eax
        + enc_rr("cvtdq2ps", 1, 1)
        + b"\xf3\x0f\x11\x0a"                  # movss [edx], xmm1
        + RET,
        ["movd xmm1, eax", "cvtdq2ps xmm1, xmm1", "movss [%[edx]], xmm1"],
        "full", {"tokens": ["GUEST_XMM_CVTDQ2PS0_R(1, 1);"]})
    add("movd_cvtdq2ps_whole",
        b"\x66\x0f\x6e\xc8" + enc_rr("cvtdq2ps", 1, 1) + store(1) + RET,
        ["movd xmm1, eax", "cvtdq2ps xmm1, xmm1"] + store_asm(1), "full",
        {"tokens": ["GUEST_XMM_CVTDQ2PS0_R(1, 1);"]})
    add("movd_mem_cvtdq2ps_whole",
        b"\x66\x0f\x6e\x09" + enc_rr("cvtdq2ps", 1, 1) + store(1) + RET,
        ["movd xmm1, [%[ecx]]", "cvtdq2ps xmm1, xmm1"] + store_asm(1), "full",
        {"tokens": ["GUEST_XMM_CVTDQ2PS0_R(1, 1);"]})
    add("xorps_zero_cvtdq2ps_whole",
        KILL[1] + enc_rr("cvtdq2ps", 1, 1) + store(1) + RET,
        kill_asm(1) + ["cvtdq2ps xmm1, xmm1"] + store_asm(1), "full",
        {"tokens": ["GUEST_XMM_CVTDQ2PS0_R(1, 1);"]})
    # the zero-upper fact does not survive another write or a different source
    add("movd_movss_cvtdq2ps_whole",
        b"\x66\x0f\x6e\xc8" + b"\xf3\x0f\x10\x09" + enc_rr("cvtdq2ps", 1, 1)
        + store(1) + RET,
        ["movd xmm1, eax", "movss xmm1, [%[ecx]]", "cvtdq2ps xmm1, xmm1"]
        + store_asm(1), "full",
        {"tokens": ["GUEST_XMM_CVTDQ2PS_R(1, 1);"], "not": ["CVTDQ2PS0"]})
    add("movd_cvtdq2ps_other_dst_whole",
        b"\x66\x0f\x6e\xc8" + enc_rr("cvtdq2ps", 2, 1) + store(2) + RET,
        ["movd xmm1, eax", "cvtdq2ps xmm2, xmm1"] + store_asm(2), "full",
        {"tokens": ["GUEST_XMM_CVTDQ2PS_R(2, 1);"], "not": ["CVTDQ2PS0"]})
    # -- shuffles with every immediate
    for imm in range(256):
        add("pshufd_%02x" % imm, enc_rr("pshufd", 3, 1, imm) + store(3) + RET,
            asm_rr("pshufd", 3, 1, imm) + store_asm(3), "full",
            {"tokens": ["GUEST_XMM_PSHUFD_R(3, 1, %d);" % imm]})
        add("shufps_%02x" % imm, enc_rr("shufps", 3, 1, imm) + store(3) + RET,
            asm_rr("shufps", 3, 1, imm) + store_asm(3), "full",
            {"tokens": ["GUEST_XMM_SHUFPS_R(3, 1, %d);" % imm]})
        add("pshuflw_%02x" % imm, enc_rr("pshuflw", 3, 1, imm) + store(3) + RET,
            asm_rr("pshuflw", 3, 1, imm) + store_asm(3), "full",
            {"tokens": ["GUEST_XMM_PSHUFW_R(0, 3, 1, %d);" % imm]})
        add("pshufhw_%02x" % imm, enc_rr("pshufhw", 3, 1, imm) + store(3) + RET,
            asm_rr("pshufhw", 3, 1, imm) + store_asm(3), "full",
            {"tokens": ["GUEST_XMM_PSHUFW_R(1, 3, 1, %d);" % imm]})
        if imm % 51 == 0:                       # a few dst==src / memory forms
            add("pshufd_same_%02x" % imm,
                enc_rr("pshufd", 3, 3, imm) + store(3) + RET,
                asm_rr("pshufd", 3, 3, imm) + store_asm(3), "full",
                {"tokens": ["GUEST_XMM_PSHUFD_R(3, 3, %d);" % imm]})
            add("shufps_same_%02x" % imm,
                enc_rr("shufps", 3, 3, imm) + store(3) + RET,
                asm_rr("shufps", 3, 3, imm) + store_asm(3), "full",
                {"tokens": ["GUEST_XMM_SHUFPS_R(3, 3, %d);" % imm]})
            add("pshuflw_same_%02x" % imm,
                enc_rr("pshuflw", 3, 3, imm) + store(3) + RET,
                asm_rr("pshuflw", 3, 3, imm) + store_asm(3), "full",
                {"tokens": ["GUEST_XMM_PSHUFW_R(0, 3, 3, %d);" % imm]})
            add("pshufhw_same_%02x" % imm,
                enc_rr("pshufhw", 3, 3, imm) + store(3) + RET,
                asm_rr("pshufhw", 3, 3, imm) + store_asm(3), "full",
                {"tokens": ["GUEST_XMM_PSHUFW_R(1, 3, 3, %d);" % imm]})
            add("pshufd_m_%02x" % imm, enc_rm("pshufd", 3, imm) + store(3) + RET,
                asm_rm("pshufd", 3, imm) + store_asm(3), "full",
                {"tokens": ["GUEST_XMM_PSHUFD_M(3,"]})
            add("shufps_m_%02x" % imm, enc_rm("shufps", 3, imm) + store(3) + RET,
                asm_rm("shufps", 3, imm) + store_asm(3), "full",
                {"tokens": ["GUEST_XMM_SHUFPS_M(3,"]})
            add("pshuflw_m_%02x" % imm, enc_rm("pshuflw", 3, imm) + store(3) + RET,
                asm_rm("pshuflw", 3, imm) + store_asm(3), "full",
                {"tokens": ["GUEST_XMM_PSHUFW_M(0, 3,"]})
            add("pshufhw_m_%02x" % imm, enc_rm("pshufhw", 3, imm) + store(3) + RET,
                asm_rm("pshufhw", 3, imm) + store_asm(3), "full",
                {"tokens": ["GUEST_XMM_PSHUFW_M(1, 3,"]})
    # -- unpacks
    for m in UNPCK_MNEMONICS:
        add("%s_rr" % m, enc_rr(m, 3, 1) + store(3) + RET,
            asm_rr(m, 3, 1) + store_asm(3), "full",
            {"tokens": ["GUEST_XMM_UNPCK_R("]})
        add("%s_rm" % m, enc_rm(m, 3) + store(3) + RET,
            asm_rm(m, 3) + store_asm(3), "full",
            {"tokens": ["GUEST_XMM_UNPCK_M("]})
        add("%s_same" % m, enc_rr(m, 3, 3) + store(3) + RET,
            asm_rr(m, 3, 3) + store_asm(3), "full",
            {"tokens": ["GUEST_XMM_UNPCK_R("]})
    add("punpckldq_rr", enc_rr("punpckldq", 3, 1) + store(3) + RET,
        asm_rr("punpckldq", 3, 1) + store_asm(3), "full",
        {"tokens": ["GUEST_XMM_PUNPCKLDQ_R(3, 1);"]})
    add("punpckldq_rm", enc_rm("punpckldq", 3) + store(3) + RET,
        asm_rm("punpckldq", 3) + store_asm(3), "full",
        {"tokens": ["GUEST_XMM_PUNPCKLDQ_M(3,"]})
    add("punpckldq_same", enc_rr("punpckldq", 3, 3) + store(3) + RET,
        asm_rr("punpckldq", 3, 3) + store_asm(3), "full",
        {"tokens": ["GUEST_XMM_PUNPCKLDQ_R(3, 3);"]})
    # -- byte shift and lane shifts: every count incl. the out-of-range ones
    for count in range(0, 20):
        expect = ({"tokens": ["GUEST_XMM_PSRLDQ(3, %d);" % count]}
                  if 1 <= count <= 15 else {"not": ["GUEST_XMM_PSRLDQ"]})
        add("psrldq_%d" % count, enc_shift_imm("psrldq", 3, count) + store(3) + RET,
            ["psrldq xmm3, %d" % count] + store_asm(3), "full", expect)
    for m, bits in SHIFT_BITS.items():
        arith = m in ("psraw", "psrad")
        token = "GUEST_XMM_PSRA_IMM(" if arith else "GUEST_XMM_PSHL_IMM("
        for count in list(range(0, bits + 2)) + [255]:
            if arith:
                # every count lowers; a count >= width is spelled as the
                # shift by width - 1 (sign fill), so the shape normalizer
                # cannot give the switch-off text back for those and the
                # switch-off `>>= width` is undefined for 32-bit lanes
                expect = {"tokens": ["%s%d, 3, %d);" % (token, bits,
                                                        min(count, bits - 1))]}
                if count >= bits:
                    expect["text_identity"] = False
                    expect["legacy_bad"] = bits == 32
            else:
                expect = ({"tokens": [token]} if count < bits
                          else {"not": ["GUEST_XMM_PSRA_IMM",
                                        "GUEST_XMM_PSHL_IMM"]})
            add("%s_%d" % (m, count), enc_shift_imm(m, 3, count) + store(3) + RET,
                ["%s xmm3, %d" % (m, count)] + store_asm(3), "full", expect)
        # register counts: 0..63 (in range and saturating), a random qword
        # (saturating), a count whose high dword is set (saturating on x86;
        # the switch-off text reads the low dword only), and dst == count
        rtoken = "GUEST_XMM_PSRA_REG(" if arith else "GUEST_XMM_PSHL_REG("
        add("%s_reg_small" % m,
            AND_EAX_63 + MOVD_XMM_EAX[4] + enc_shift_reg(m, 0, 4) + store(0) + RET,
            ["and eax, 63", "movd xmm4, eax", "%s xmm0, xmm4" % m] + store_asm(0),
            "full", {"tokens": [rtoken], "legacy_bad": m == "psrad"})
        # (the primed count qword often has a zero low dword with a set high
        # dword: x86 saturates, the switch-off text shifts by the low dword)
        add("%s_reg_rand" % m, enc_shift_reg(m, 0, 4) + store(0) + RET,
            ["%s xmm0, xmm4" % m] + store_asm(0), "full",
            {"tokens": [rtoken], "legacy_bad": True})
        add("%s_reg_hi" % m,
            AND_EAX_31 + MOVD_XMM_EAX[4] + enc_rr("punpckldq", 4, 4)
            + enc_shift_reg(m, 0, 4) + store(0) + RET,
            ["and eax, 31", "movd xmm4, eax", "punpckldq xmm4, xmm4",
             "%s xmm0, xmm4" % m] + store_asm(0),
            "full", {"tokens": [rtoken], "legacy_bad": True})
        add("%s_reg_same" % m,
            AND_EAX_63 + MOVD_XMM_EAX[0] + enc_shift_reg(m, 0, 0) + store(0) + RET,
            ["and eax, 63", "movd xmm0, eax", "%s xmm0, xmm0" % m] + store_asm(0),
            "full", {"tokens": [rtoken], "legacy_bad": m == "psrad"})
    # -- comis paired with jcc / setcc / cmovcc; refused shapes stay lazy
    for m in COMIS_MNEMONICS:
        for cc in FLOAT_CCS:
            # comis xmm0, xmm1; jcc L; mov eax, 0; ret; L: mov eax, 1; ret
            code = (enc_rr(m, 0, 1) + bytes([JCC[cc], 6])
                    + b"\xb8\x00\x00\x00\x00" + RET
                    + b"\xb8\x01\x00\x00\x00" + RET)
            asm = ["%s xmm0, xmm1" % m, "set%s al" % cc, "movzx eax, al"]
            add("%s_j%s" % (m, cc), code, asm, "full",
                {"tokens": ["GUEST_XMM_COMIS_PAIRED(", "GUEST_XMM_FCC(%s," % cc],
                 "not": ["cc_%s(GUEST_FL)" % cc]})
            # memory operand form of the producer
            code = (enc_rm(m, 0) + bytes([JCC[cc], 6])
                    + b"\xb8\x00\x00\x00\x00" + RET
                    + b"\xb8\x01\x00\x00\x00" + RET)
            asm = (["movups xmm8, [%[ecx]]", "%s xmm0, xmm8" % m,
                    "set%s al" % cc, "movzx eax, al"])
            add("%s_m_j%s" % (m, cc), code, asm, "full",
                {"tokens": ["GUEST_XMM_COMIS_PAIRED(", "GUEST_XMM_FCC(%s," % cc]})
            # setcc: xor eax, eax; comis xmm0, xmm1; setcc al; ret
            code = b"\x31\xc0" + enc_rr(m, 0, 1) + SETCC[cc] + b"\xc0" + RET
            asm = ["xor eax, eax", "%s xmm0, xmm1" % m, "set%s al" % cc]
            add("%s_set%s" % (m, cc), code, asm, "full",
                {"tokens": ["GUEST_XMM_COMIS_PAIRED(", "GUEST_XMM_FCC(%s," % cc]})
            # cmovcc: mov eax,0; mov edx,1; comis; cmovcc eax, edx; ret
            code = (b"\xb8\x00\x00\x00\x00" + b"\xba\x01\x00\x00\x00"
                    + enc_rr(m, 0, 1) + CMOVCC[cc] + b"\xc2" + RET)
            asm = ["mov eax, 0", "mov edx, 1", "%s xmm0, xmm1" % m,
                   "cmov%s eax, edx" % cc]
            add("%s_cmov%s" % (m, cc), code, asm, "full",
                {"tokens": ["GUEST_XMM_COMIS_PAIRED(", "GUEST_XMM_FCC(%s," % cc]})
        # register-safe distance: an unrelated mov between producer and jcc
        code = (enc_rr(m, 0, 1) + b"\xba\x05\x00\x00\x00" + bytes([JCC["a"], 6])
                + b"\xb8\x00\x00\x00\x00" + RET
                + b"\xb8\x01\x00\x00\x00" + RET)
        add("%s_ja_after_mov" % m, code,
            ["%s xmm0, xmm1" % m, "mov edx, 5", "seta al", "movzx eax, al"],
            "full", {"tokens": ["GUEST_XMM_COMIS_PAIRED(", "GUEST_XMM_FCC(a,"]})
        # operand rewritten in between: must NOT pair (lazy producer kept)
        code = (enc_rr(m, 0, 1) + enc_rr("movaps", 0, 2) + bytes([JCC["a"], 6])
                + b"\xb8\x00\x00\x00\x00" + RET
                + b"\xb8\x01\x00\x00\x00" + RET)
        add("%s_ja_operand_rewritten" % m, code,
            ["%s xmm0, xmm1" % m, "movaps xmm0, xmm2", "seta al",
             "movzx eax, al"],
            "full", {"tokens": ["cc_a(GUEST_FL)"],
                     "not": ["GUEST_XMM_COMIS_PAIRED(", "GUEST_XMM_FCC("]})
        # SF/OF conditions are never paired with comis
        for cc in ("g", "l", "s"):
            code = (enc_rr(m, 0, 1) + bytes([JCC[cc], 6])
                    + b"\xb8\x00\x00\x00\x00" + RET
                    + b"\xb8\x01\x00\x00\x00" + RET)
            add("%s_j%s_refused" % (m, cc), code,
                ["%s xmm0, xmm1" % m, "set%s al" % cc, "movzx eax, al"],
                "full", {"tokens": ["cc_%s(GUEST_FL)" % cc],
                         "not": ["GUEST_XMM_COMIS_PAIRED(", "GUEST_XMM_FCC("]})
        # two consumers of one producer in different blocks: the pairing is
        # block-local, so the first jcc is answered directly and the second
        # reads the lazy producer (flags live out of the first block)
        code = (enc_rr(m, 0, 1) + bytes([JCC["a"], 8])
                + bytes([JCC["e"], 12])
                + b"\xb8\x00\x00\x00\x00" + RET
                + b"\xb8\x01\x00\x00\x00" + RET
                + b"\xb8\x02\x00\x00\x00" + RET)
        add("%s_ja_je_chain" % m, code,
            ["%s xmm0, xmm1" % m, "mov eax, 0", "mov edx, 1", "cmova eax, edx",
             "mov edx, 2", "cmove eax, edx"],
            "full", {"tokens": ["GUEST_XMM_FCC(a,", "cc_e(GUEST_FL)", "FLAG_PARTIAL"],
                     "not": ["GUEST_XMM_COMIS_PAIRED(", "GUEST_XMM_FCC(e,"]})
        # a second consumer the pairing cannot answer keeps the flags live:
        # comis; ja L; lahf; and eax, 0xc500; ret; L: mov eax, 0xffffffff; ret
        code = (enc_rr(m, 0, 1) + bytes([JCC["a"], 7])
                + b"\x9f" + b"\x25\x00\xc5\x00\x00" + RET
                + b"\xb8\xff\xff\xff\xff" + RET)
        add("%s_ja_then_lahf" % m, code,
            ["%s xmm0, xmm1" % m, "seta bl", "lahf", "and eax, 0xc500",
             "test bl, bl", "jz 1f", "mov eax, 0xffffffff", "1:"],
            "full", {"tokens": ["GUEST_XMM_FCC(a,"],
                     "not": ["GUEST_XMM_COMIS_PAIRED("]})
    # -- scalar float / double memory operands: guest.h ldf/stf/ldd/std_.
    # ecx runs every alignment 0..15, so three quarters of the iterations
    # take the misaligned path of GUEST_F32_VFP_ACCESS=1 and one quarter the
    # aligned vldr/vstr path; both must equal the memcpy pun and native x86.
    add("movss_load", enc_rm("movss", 3) + store(3) + RET,
        ["movss xmm3, [%[ecx]]"] + store_asm(3), "full",
        {"tokens": ["ldf("], "not": ["stf("]})
    add("movss_load_lane0", enc_rm("movss", 2) + SCALAR_STORE_EDX[2] + KILL[2] + RET,
        ["movss xmm2, [%[ecx]]", "movss [%[edx]], xmm2", "xorps xmm2, xmm2"],
        "full", {"tokens": ["ldf(", "stf("]})
    add("movss_store", b"\xf3\x0f\x11" + _modrm_mem_ecx(1) + RET,
        ["movss [%[ecx]], xmm1"], "full", {"tokens": ["stf("], "not": ["ldf("]})
    add("movss_load_store", enc_rm("movss", 3) + b"\xf3\x0f\x11" + _modrm_mem_ecx(3)
        + b"\x83\xc1\x04" + b"\xf3\x0f\x11" + _modrm_mem_ecx(3) + RET,
        ["movss xmm3, [%[ecx]]", "movss [%[ecx]], xmm3", "add ecx, 4",
         "movss [%[ecx]], xmm3"], ("lane0", 3), {"tokens": ["ldf(", "stf("]})
    # (lane-0 compare: nothing observes xmm3's upper lanes, so the emitter's
    # zero-fill elision legitimately leaves them unwritten)
    for m in SCALAR_F32_MEM:
        add("%s_m" % m, enc_rm(m, 3) + store(3) + RET,
            ["%s xmm3, [%%[ecx]]" % m] + store_asm(3), "full",
            {"tokens": ["ldf("]})
        add("%s_m_store" % m, enc_rm(m, 3) + b"\xf3\x0f\x11" + _modrm_mem_ecx(3) + RET,
            ["%s xmm3, [%%[ecx]]" % m, "movss [%[ecx]], xmm3"], "full",
            {"tokens": ["ldf(", "stf("]})
    add("cvttss2si_m", enc_rm("cvttss2si", 0) + RET,
        ["cvttss2si eax, [%[ecx]]"], "full", {"tokens": ["ldf("]})
    add("movsd_load", enc_rm("movsd", 3) + store(3) + RET,
        ["movsd xmm3, [%[ecx]]"] + store_asm(3), "full", {"tokens": ["ldd("]})
    add("movsd_store", b"\xf2\x0f\x11" + _modrm_mem_ecx(1) + RET,
        ["movsd [%[ecx]], xmm1"], "full", {"tokens": ["std_("]})
    add("addsd_m_store", enc_rm("addsd", 3) + b"\xf2\x0f\x11" + _modrm_mem_ecx(3) + RET,
        ["addsd xmm3, [%[ecx]]", "movsd [%[ecx]], xmm3"], "full",
        {"tokens": ["ldd(", "std_("]})
    add("cvtsd2ss_m_store", enc_rm("cvtsd2ss", 3) + b"\xf3\x0f\x11" + _modrm_mem_ecx(3) + RET,
        ["cvtsd2ss xmm3, [%[ecx]]", "movss [%[ecx]], xmm3"], "full",
        {"tokens": ["ldd(", "stf("]})
    # -- hostile shapes for the GUEST_F32_VFP_ACCESS spelling.  A float store
    # observed by an integer load of the same bytes and the reverse (the
    # may_alias view must not let GCC reorder or forward across the type
    # change), overlapping stores and loads whose alignment classes differ
    # (one side takes the aligned vldr/vstr path while the other takes the
    # out-of-line memcpy fallback, at every ecx alignment), a misaligned
    # store followed by an aligned reload of the same bytes.  (fld/fstp dword
    # go through the same ldf/stf but cannot be compared bit for bit here:
    # the x87 quiets a signalling NaN on fld while the emitter's
    # (float)(double)x round trip folds to a copy -- a property of the x87
    # model under every knob, seen with GUEST_F32_VFP_ACCESS=0 too.)
    add("movss_store_int_load", b"\xf3\x0f\x11\x09" + b"\x8b\x01" + RET,
        ["movss [%[ecx]], xmm1", "mov eax, [%[ecx]]"], "full",
        {"tokens": ["stf("], "not": ["ldf("]})
    add("int_store_movss_load", b"\x89\x01" + enc_rm("movss", 3) + store(3) + RET,
        ["mov [%[ecx]], eax", "movss xmm3, [%[ecx]]"] + store_asm(3), "full",
        {"tokens": ["ldf("]})
    add("movss_overlap_store_load",
        enc_rm("movss", 2) + b"\xf3\x0f\x11\x51\x02" + KILL[2]
        + b"\xf3\x0f\x10\x59\x01" + store(3) + RET,
        ["movss xmm2, [%[ecx]]", "movss [%[ecx] + 2], xmm2", "xorps xmm2, xmm2",
         "movss xmm3, [%[ecx] + 1]"] + store_asm(3), "full",
        {"tokens": ["ldf(", "stf("]})
    add("movss_misaligned_store_aligned_load",
        b"\xf3\x0f\x11\x49\x01" + enc_rm("movss", 3) + store(3) + RET,
        ["movss [%[ecx] + 1], xmm1", "movss xmm3, [%[ecx]]"] + store_asm(3), "full",
        {"tokens": ["ldf(", "stf("]})
    return cases


# --------------------------------------------------------------- render ----

def render(code, name, switch):
    """Render one synthetic function through the real pipeline with the
    generation switch set to `switch` ("0"/"1")."""
    saved = os.environ.get(sse_lower.ENV)
    os.environ[sse_lower.ENV] = switch
    try:
        text = bytearray(0x400)
        text[:len(code)] = code
        ctx = T.Ctx("synthetic", 0x400000, TEXT, TEXT + len(text), bytes(text))
        em, insns, order, body, unsup, _indirect, _members = B.translate(
            ctx, TEXT, name, known={TEXT})
    finally:
        if saved is None:
            del os.environ[sse_lower.ENV]
        else:
            os.environ[sse_lower.ENV] = saved
    if unsup:
        raise SystemExit("%s: unsupported: %s" % (name, unsup[0]))
    lines = ["static void %s(CPU *__restrict c)" % name, "{"]
    lines.extend("    %s" % line for line in B.flag_state_prologue(em))
    gate = B.entry_gate(TEXT, order)
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


HARNESS_HEAD = r'''
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "guest.h"

#define BUF_SIZE (1u << 20)
static uint32_t BUF;
static unsigned g_faults, g_calls;
void guest_fault(CPU *__restrict c, uint32_t addr, const char *what)
{ (void)c; (void)addr; (void)what; g_faults++; }
void guest_int3(CPU *__restrict c, uint32_t addr) { (void)c; (void)addr; }
void guest_cpuid(CPU *__restrict c) { (void)c; }
void guest_xgetbv(CPU *__restrict c) { (void)c; }
void guest_call(CPU *__restrict c, uint32_t addr) { (void)c; (void)addr; g_calls++; }
uint32_t g_guest_fs_base_cached;
uint32_t guest_fs_base(CPU *__restrict c) { (void)c; return BUF; }
unsigned char *g_guest_coverage_functions;
unsigned char *g_guest_coverage_cases;
uint64_t guest_atomic_cmpxchg64(uint32_t addr, uint64_t expected, uint64_t desired)
{ uint64_t o = ld64(addr); if (o == expected) st64(addr, desired); return o; }

static uint32_t rng_state;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5; return rng_state;
}
static const uint32_t EDGE32[] = {
    0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u,   /* +-0, +-inf */
    0x7fc00000u, 0xffc00000u, 0x7fc00001u, 0x7fffffffu,   /* quiet NaNs */
    0x7f800001u, 0xff800001u, 0x7fbfffffu,                /* signalling NaNs */
    0x00000001u, 0x007fffffu, 0x80000001u, 0x807fffffu,   /* denormals */
    0x00800000u, 0x80800000u, 0x7f7fffffu, 0xff7fffffu,   /* min/max normal */
    0x3f800000u, 0xbf800000u, 0x4b800000u, 0xcb800000u,   /* 1, 2^24 */
    0x4f000000u, 0x4effffffu, 0x3f000000u, 0x40490fdbu,
    0x7fffffffu, 0x80000000u, 0x80000001u, 0xffffffffu,   /* INT_MAX/MIN, -1 */
    0x00000002u, 0x0000fffeu, 0x00010000u, 0x01000000u, 0x00ffffffu,
};
static const uint64_t EDGE64[] = {
    0x0000000000000000ull, 0x8000000000000000ull,
    0x7ff0000000000000ull, 0xfff0000000000000ull,            /* +-inf */
    0x7ff8000000000000ull, 0xfff8000000000000ull, 0x7ff8000000000001ull,
    0x7fffffffffffffffull,                                    /* quiet NaNs */
    0x7ff0000000000001ull, 0xfff0000000000001ull, 0x7ff7ffffffffffffull,
    0x0000000000000001ull, 0x000fffffffffffffull,             /* denormals */
    0x8000000000000001ull, 0x800fffffffffffffull,
    0x0010000000000000ull, 0x7fefffffffffffffull, 0xffefffffffffffffull,
    0x3ff0000000000000ull, 0xbff0000000000000ull, 0x4000000000000000ull,
    0x3fe0000000000000ull, 0x400921fb54442d18ull,
};
static uint32_t lane32(void)
{
    uint32_t r = rnd();
    if (r & 1u) return EDGE32[(r >> 1) % (sizeof EDGE32 / sizeof EDGE32[0])];
    if (r & 2u) return rnd() & 0xffu;             /* small integers */
    return rnd();
}
static uint64_t lane64(void)
{
    uint32_t r = rnd();
    if (r & 1u) return EDGE64[(r >> 1) % (sizeof EDGE64 / sizeof EDGE64[0])];
    return ((uint64_t)rnd() << 32) | rnd();
}
static void prime(CPU *c, int doubles, unsigned align, unsigned align2)
{
    int i;
    memset(c, 0, sizeof *c);
    for (i = 0; i < 8; i++) {
        if (doubles) { c->x[i].u64[0] = lane64(); c->x[i].u64[1] = lane64(); }
        else { c->x[i].u32[0] = lane32(); c->x[i].u32[1] = lane32();
               c->x[i].u32[2] = lane32(); c->x[i].u32[3] = lane32(); }
    }
    /* equal operands are the interesting comis class: force them often */
    if (rnd() % 4u == 0u) c->x[1] = c->x[0];
    if (rnd() % 8u == 0u) { c->x[1].u32[0] = c->x[0].u32[0] ^ 0x80000000u; }
    c->eax = rnd(); c->ebx = rnd(); c->esi = rnd(); c->edi = rnd(); c->ebp = rnd();
    c->ecx = BUF + 0x1000u + align;
    c->edx = BUF + 0x2000u + align2;
    c->esp = BUF + 0x8000u;
    for (i = 0; i < 32; i += 4) { st32(BUF + 0x1000u + i, lane32());
                                   st32(BUF + 0x2000u + i, lane32()); }
    st32(BUF + 0x8000u, 0x00401000u);
}
static unsigned g_mismatch, g_checked, g_legacy_bad_checked, g_legacy_bad_diverged;
static uint64_t g_hash = 1469598103934665603ull;
static void feed(const void *p, size_t n)
{
    const unsigned char *b = p; size_t i;
    for (i = 0; i < n; i++) { g_hash ^= b[i]; g_hash *= 1099511628211ull; }
}
static void dump(const char *tag, const CPU *c)
{
    int i;
    printf("    %s eax=%08x edx=%08x\n", tag, c->eax, c->edx);
    for (i = 0; i < 8; i++)
        printf("      x%d %08x %08x %08x %08x\n", i, c->x[i].u32[0],
               c->x[i].u32[1], c->x[i].u32[2], c->x[i].u32[3]);
}
static int same_state(const CPU *a, const CPU *b, int lane0_reg)
{
    int i;
    if (a->eax != b->eax) return 0;
    for (i = 0; i < 8; i++) {
        if (i == lane0_reg) { if (a->x[i].u32[0] != b->x[i].u32[0]) return 0; }
        else if (memcmp(&a->x[i], &b->x[i], 16) != 0) return 0;
    }
    return 1;
}
static void check(const char *name, CPU *leg, CPU *tok, CPU *tru,
                  const unsigned char *mem_leg, const unsigned char *mem_tok,
                  const unsigned char *mem_tru, int lane0_reg, int legacy_bad)
{
    int leg_ok = same_state(leg, tru, lane0_reg) && memcmp(mem_leg, mem_tru, 64) == 0;
    int ok = same_state(tok, tru, lane0_reg) && memcmp(mem_tok, mem_tru, 64) == 0;
    if (legacy_bad) {
        /* the switch-off spelling is known not to be exact here (shift count
         * >= the lane width on 32-bit lanes is undefined in C; register
         * counts read the low dword): record, do not judge */
        g_legacy_bad_checked++;
        g_legacy_bad_diverged += !leg_ok;
    } else {
        ok = ok && leg_ok;
    }
    g_checked++;
    feed(tru->x, sizeof tru->x); feed(&tru->eax, 4); feed(mem_tru, 64);
    if (!ok && g_mismatch++ < 40) {
        printf("MISMATCH %s (lane0_reg %d)\n", name, lane0_reg);
        dump("legacy", leg); dump("token ", tok); dump("truth ", tru);
    }
}
'''

HARNESS_CASE = r'''
static void run_%(name)s(unsigned iterations)
{
    static CPU leg, tok, tru;
    unsigned char mem_leg[64], mem_tok[64], mem_tru[64];
    unsigned it;
    for (it = 0; it < iterations; it++) {
        unsigned align = it %% 16u, align2 = (it / 16u) %% 16u;
        uint32_t seed = rng_state;
        prime(&leg, %(doubles)d, align, align2);
        memcpy(mem_leg, (void *)(uintptr_t)(BUF + 0x1000u), 64);
        rng_state = seed; prime(&tok, %(doubles)d, align, align2);
        memcpy(mem_tok, (void *)(uintptr_t)(BUF + 0x1000u), 64);
        rng_state = seed; prime(&tru, %(doubles)d, align, align2);
        memcpy(mem_tru, (void *)(uintptr_t)(BUF + 0x1000u), 64);
        memcpy((void *)(uintptr_t)(BUF + 0x1000u), mem_leg, 64);
        legacy_%(name)s(&leg);
        memcpy(mem_leg, (void *)(uintptr_t)(BUF + 0x1000u), 64);
        memcpy((void *)(uintptr_t)(BUF + 0x1000u), mem_tok, 64);
        token_%(name)s(&tok);
        memcpy(mem_tok, (void *)(uintptr_t)(BUF + 0x1000u), 64);
        memcpy((void *)(uintptr_t)(BUF + 0x1000u), mem_tru, 64);
        truth_%(name)s(&tru);
        memcpy(mem_tru, (void *)(uintptr_t)(BUF + 0x1000u), 64);
        check("%(name)s", &leg, &tok, &tru, mem_leg, mem_tok, mem_tru, %(lane0)d, %(legacy_bad)d);
    }
}
'''

HARNESS_CVT_EXHAUSTIVE = r'''
static void run_cvt_exhaustive(void)
{
    static CPU leg, tok, tru;
    uint64_t block; unsigned bad = 0;
    for (block = 0; block < (1ull << 30); block++) {
        uint32_t base = (uint32_t)(block << 2);
        int i;
        memset(&leg, 0, sizeof leg); leg.esp = BUF + 0x8000u; leg.edx = BUF + 0x2000u;
        for (i = 0; i < 4; i++) leg.x[1].u32[i] = base + (uint32_t)i;
        tok = leg; tru = leg;
        legacy_cvtdq2ps_rr(&leg);
        token_cvtdq2ps_rr(&tok);
        truth_cvtdq2ps_rr(&tru);
        if (memcmp(&leg.x[3], &tru.x[3], 16) != 0 || memcmp(&tok.x[3], &tru.x[3], 16) != 0) {
            if (bad++ < 10)
                printf("MISMATCH cvtdq2ps exhaustive at %08x: legacy %08x token %08x truth %08x\n",
                       base, leg.x[3].u32[0], tok.x[3].u32[0], tru.x[3].u32[0]);
        }
    }
    g_mismatch += bad;
    printf("cvtdq2ps exhaustive 2^32 lanes: %u mismatches\n", bad);
}
'''

HARNESS_MAIN = r'''
int main(int argc, char **argv)
{
    unsigned iterations = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 4000u;
    int exhaustive = argc > 2 ? atoi(argv[2]) : 1;
    void *map = mmap(NULL, BUF_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (map == MAP_FAILED) { perror("mmap"); return 2; }
    BUF = (uint32_t)(uintptr_t)map;
    rng_state = 0x9e3779b9u;
%(calls)s
    if (exhaustive) run_cvt_exhaustive();
    printf("sse lowering oracle: %%u cases, %%u checks, %%u mismatches, hash %%016llx, faults %%u calls %%u\n",
           %(count)uu, g_checked, g_mismatch, (unsigned long long)g_hash, g_faults, g_calls);
    printf("switch-off spelling known-inexact shift checks: %%u (diverged from x86 in %%u; tokens judged alone there)\n",
           g_legacy_bad_checked, g_legacy_bad_diverged);
    return g_mismatch ? 1 : 0;
}
'''


def build_source(cases):
    pieces = [HARNESS_HEAD, TRUTH_HEAD]
    calls = []
    facts = {"paired": 0, "lazy": 0, "lane0": 0, "tokens": 0}
    for name, code, asm, compare, expect in cases:
        legacy_text, em_legacy = render(code, "legacy_" + name, "0")
        token_text, em_token = render(code, "token_" + name, "1")
        if "GUEST_XMM_" in legacy_text:
            raise SystemExit("%s: switch-off rendering carries a token" % name)
        if em_legacy.direct_pairs and "GUEST_XMM" not in token_text:
            pass
        # legacy_sse_tokens on the token rendering must give the legacy one
        import gpr_locals
        mapped = gpr_locals.legacy_text(token_text).replace("token_", "legacy_")
        if (expect.get("text_identity", True) and
                mapped != gpr_locals.legacy_text(legacy_text)):
            import difflib
            sys.stdout.writelines(difflib.unified_diff(
                gpr_locals.legacy_text(legacy_text).splitlines(True),
                mapped.splitlines(True), "legacy", "legacy_sse_tokens(token)"))
            raise SystemExit("%s: legacy_sse_tokens does not reproduce the "
                             "switch-off text" % name)
        for needle in expect.get("tokens", ()):
            if needle not in token_text:
                print(token_text)
                raise SystemExit("%s: expected %r in the token rendering"
                                 % (name, needle))
        for needle in expect.get("not", ()):
            if needle in token_text:
                print(token_text)
                raise SystemExit("%s: unexpected %r in the token rendering"
                                 % (name, needle))
        facts["tokens"] += token_text.count("GUEST_XMM_")
        facts["paired"] += token_text.count("GUEST_XMM_FCC(")
        facts["lazy"] += token_text.count("FLAG_PARTIAL")
        facts["lane0"] += (token_text.count("LANEOP0_") + token_text.count("MOV0(")
                           + token_text.count("CVTDQ2PS0_"))
        pieces.append(legacy_text)
        pieces.append(token_text)
        pieces.append("static void truth_%s(CPU *__restrict c)\n{\n%s\n}\n"
                      % (name, "\n".join(truth_body(asm))))
        doubles = 1 if name.startswith(("comisd", "ucomisd", "movsd", "addsd",
                                        "cvtsd2ss")) else 0
        lane0 = compare[1] if isinstance(compare, tuple) else -1
        pieces.append(HARNESS_CASE % {"name": name, "doubles": doubles,
                                      "lane0": lane0,
                                      "legacy_bad": int(bool(expect.get("legacy_bad")))})
        calls.append("    run_%s(iterations);" % name)
    pieces.append(HARNESS_CVT_EXHAUSTIVE)
    pieces.append(HARNESS_MAIN % {"calls": "\n".join(calls),
                                  "count": len(cases)})
    return "".join(pieces), facts


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", help="host C compiler (gcc); without it the "
                        "harness is only rendered")
    parser.add_argument("--emit", type=pathlib.Path,
                        help="write the harness source here")
    parser.add_argument("--iterations", type=int, default=4000)
    parser.add_argument("--no-exhaustive-cvt", action="store_true")
    parser.add_argument("--keep", action="store_true",
                        help="keep the temporary build directory")
    arguments = parser.parse_args()

    cases = build_cases()
    source, facts = build_source(cases)
    print("sse lowering oracle: %d cases rendered; token statements %d, "
          "paired comis consumers %d, lazy comis producers %d, lane-0 forms %d"
          % (len(cases), facts["tokens"], facts["paired"], facts["lazy"],
             facts["lane0"]))
    if arguments.emit:
        arguments.emit.write_text(source, encoding="ascii", newline="\n")
        print("harness written: %s" % arguments.emit)
    if arguments.cc is None:
        print("sse lowering oracle: rendering checks PASS (no --cc, not run)")
        return 0
    work_raw = tempfile.mkdtemp(prefix="isaac-sse-oracle-")
    work = pathlib.Path(work_raw)
    source_path = work / "sse_oracle.c"
    source_path.write_text(source, encoding="ascii", newline="\n")
    outputs = {}
    # Two header knobs: GUEST_SSE_LOWER (tokens -> NEON) and
    # GUEST_F32_VFP_ACCESS (ldf/stf -> aligned VFP access + cold misaligned
    # fallback).  All four binaries must print the same output.
    for knob in (0, 1):
        for f32 in (0, 1):
            binary = work / ("sse_oracle_%d_%d" % (knob, f32))
            command = [
                arguments.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra",
                "-Wno-unused-variable", "-Wno-unused-but-set-variable",
                "-Wno-unused-label", "-Wno-unused-parameter", "-Wno-unused-function",
                "-DGUEST_GENERATED_STACK_GUARD=0", "-DGUEST_STACK_REQUIRED=0",
                "-DGUEST_GPR_LOCAL=0", "-DGUEST_FLAGS_LOCAL=1",
                "-DGUEST_SSE_LOWER=%d" % knob,
                "-DGUEST_F32_VFP_ACCESS=%d" % f32,
                "-I", os.fspath(HERE / "runtime"), os.fspath(source_path),
                "-o", os.fspath(binary), "-lm",
            ]
            built = subprocess.run(command, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, check=False)
            if built.stdout:
                print(built.stdout, end="")
            if built.returncode:
                raise SystemExit("sse oracle compile failed with GUEST_SSE_LOWER=%d "
                                 "GUEST_F32_VFP_ACCESS=%d" % (knob, f32))
            ran = subprocess.run([os.fspath(binary), str(arguments.iterations),
                                  "0" if arguments.no_exhaustive_cvt else "1"],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, check=False, timeout=3600)
            print("--- GUEST_SSE_LOWER=%d GUEST_F32_VFP_ACCESS=%d ---" % (knob, f32))
            print(ran.stdout, end="")
            if ran.returncode:
                raise SystemExit("sse oracle run failed with GUEST_SSE_LOWER=%d "
                                 "GUEST_F32_VFP_ACCESS=%d" % (knob, f32))
            outputs[(knob, f32)] = ran.stdout
    if len(set(outputs.values())) != 1:
        raise SystemExit("sse lowering oracle: the four knob combinations "
                         "(GUEST_SSE_LOWER x GUEST_F32_VFP_ACCESS) print "
                         "different outputs")
    print("sse lowering oracle: GUEST_F32_VFP_ACCESS=0 and =1 outputs identical "
          "under both GUEST_SSE_LOWER values (scalar float loads/stores at every "
          "alignment 0..15)")
    if not arguments.keep:
        import shutil
        shutil.rmtree(work, ignore_errors=True)
    print("sse lowering oracle: %d cases identical under all header knobs and "
          "equal to native x86: PASS" % len(cases))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
