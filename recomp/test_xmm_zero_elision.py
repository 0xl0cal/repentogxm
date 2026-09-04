#!/usr/bin/env python3
"""Unit gate for the xmm upper-lane taint analysis (emit.xmm_zero_fill_elidable).

Each case is a hand-encoded x86-32 sequence decoded through the real
translator front end (trans.decode / trans.blocks); the analysis must elide
the zero fill of exactly the loads whose upper lanes nothing can observe, and
pin every load that reaches a whole-register store, a shuffle, a 64-bit read
of a single-width value, another block that observes it, or xmm0 at return.
"""

from __future__ import annotations

import sys

import emit as E
import trans as T

TEXT = 0x1000

# name -> (bytes, {load offset: expected "elide"|"pin"})
CASES = {
    "scalar chain, scalar store": (
        "f30f1009"          # movss xmm1, [ecx]
        "f30f584904"        # addss xmm1, [ecx+4]
        "f30f110a"          # movss [edx], xmm1
        "c3",               # ret
        {0: "elide"}),
    "whole-register store observes": (
        "f30f1009"          # movss xmm1, [ecx]
        "0f110a"            # movups [edx], xmm1
        "c3",
        {0: "pin"}),
    "copy then whole-register store": (
        "f30f1009"          # movss xmm1, [ecx]
        "0f28d1"            # movaps xmm2, xmm1
        "0f1112"            # movups [edx], xmm2
        "c3",
        {0: "pin"}),
    "copy, lane-wise op, scalar store": (
        "f30f1009"          # movss xmm1, [ecx]
        "0f28d1"            # movaps xmm2, xmm1
        "0f58d2"            # addps xmm2, xmm2
        "f30f1112"          # movss [edx], xmm2
        "c3",
        {0: "elide"}),
    "call ends the taint": (
        "f30f1009"          # movss xmm1, [ecx]
        "e8fb0f0000"        # call +0x1000 (not followed)
        "0f110a"            # movups [edx], xmm1
        "c3",
        {0: "elide"}),
    "MSVC double round-trip on an sd load": (
        "f20f1009"          # movsd xmm1, [ecx]
        "660f5ac9"          # cvtpd2ps xmm1, xmm1
        "f30f110a"          # movss [edx], xmm1
        "c3",
        {0: "elide"}),
    "cvtpd2ps of an ss load reads f[1]": (
        "f30f1009"          # movss xmm1, [ecx]
        "660f5ac9"          # cvtpd2ps xmm1, xmm1
        "f30f110a"          # movss [edx], xmm1
        "c3",
        {0: "pin"}),
    "shuffle observes": (
        "f30f1009"          # movss xmm1, [ecx]
        "0fc6c900"          # shufps xmm1, xmm1, 0
        "f30f110a"          # movss [edx], xmm1
        "c3",
        {0: "pin"}),
    "64-bit store of an ss load": (
        "f30f1009"          # movss xmm1, [ecx]
        "660fd60a"          # movq [edx], xmm1
        "c3",
        {0: "pin"}),
    "64-bit store of an sd load": (
        "f20f1009"          # movsd xmm1, [ecx]
        "660fd60a"          # movq [edx], xmm1
        "c3",
        {0: "elide"}),
    "xmm0 at return is pinned": (
        "f30f1001"          # movss xmm0, [ecx]
        "c3",
        {0: "pin"}),
    "xmm1 at return is free": (
        "f30f1009"          # movss xmm1, [ecx]
        "c3",
        {0: "elide"}),
    "taint observed in a successor block": (
        "f30f1009"          # +0  movss xmm1, [ecx]
        "85c0"              # +4  test eax, eax
        "7404"              # +6  je +4 -> +12
        "0f110a"            # +8  movups [edx], xmm1
        "c3"                # +11 ret
        "f30f110a"          # +12 movss [edx], xmm1
        "c3",               # +16 ret
        {0: "pin"}),
    "taint cleared on one path only": (
        "f30f1009"          # +0  movss xmm1, [ecx]
        "85c0"              # +4  test eax, eax
        "7403"              # +6  je +3 -> +11
        "0f57c9"            # +8  xorps xmm1, xmm1
        "0f110a"            # +11 movups [edx], xmm1
        "c3",               # +14 ret
        {0: "pin"}),
    "taint cleared on both paths": (
        "f30f1009"          # +0  movss xmm1, [ecx]
        "f30f110a"          # +4  movss [edx], xmm1
        "85c0"              # +8  test eax, eax
        "7405"              # +10 je +5 -> +17
        "0f57c9"            # +12 xorps xmm1, xmm1
        "eb03"              # +15 jmp +3 -> +20
        "0f57c9"            # +17 xorps xmm1, xmm1
        "0f110a"            # +20 movups [edx], xmm1
        "c3",               # +23 ret
        {0: "elide"}),
    "two loads, one observed": (
        "f30f1009"          # +0  movss xmm1, [ecx]
        "f30f110a"          # +4  movss [edx], xmm1
        "f30f1011"          # +8  movss xmm2, [ecx]
        "0f1112"            # +12 movups [edx], xmm2
        "c3",
        {0: "elide", 8: "pin"}),
    "zeroing idiom clears before store": (
        "f30f1009"          # movss xmm1, [ecx]
        "f30f110a"          # movss [edx], xmm1
        "0f57c9"            # xorps xmm1, xmm1
        "0f110a"            # movups [edx], xmm1
        "c3",
        {0: "elide"}),
    "unpcklps carries, movups then observes": (
        "f30f1009"          # movss xmm1, [ecx]
        "f30f1011"          # movss xmm2, [ecx]
        "0f14ca"            # unpcklps xmm1, xmm2
        "0f110a"            # movups [edx], xmm1
        "c3",
        {0: "pin", 4: "pin"}),
    "float widened to a double argument (cvtps2pd + movsd store)": (
        "f30f1009"          # movss xmm1, [ecx]
        "0f5ac9"            # cvtps2pd xmm1, xmm1
        "f20f110a"          # movsd [edx], xmm1
        "c3",
        {0: "elide"}),
    "float widened then stored whole": (
        "f30f1009"          # movss xmm1, [ecx]
        "0f5ac9"            # cvtps2pd xmm1, xmm1
        "0f110a"            # movups [edx], xmm1
        "c3",
        {0: "pin"}),
    "movsd reg-reg rewrites d[0] exactly": (
        "f30f1009"          # movss xmm1, [ecx]
        "f20f10ca"          # movsd xmm1, xmm2
        "f20f110a"          # movsd [edx], xmm1
        "c3",
        {0: "elide"}),
    "cvtdq2pd carries into a scalar-only use": (
        "660f6e01"          # movd xmm0, [ecx]
        "f30f1009"          # movss xmm1, [ecx]
        "f30fe6c9"          # cvtdq2pd xmm1, xmm1
        "f20f110a"          # movsd [edx], xmm1
        "c3",
        {4: "elide"}),
}


# SSE lowering lane-0 verdicts (emit.xmm_lane0_sites): the same taint walk
# with the packed op / register copy as its own source.  A site may be spelled
# lane-0-only when nothing observes lanes 1..3 of its result and the result
# is not live at a call, a return or a tail transfer; `cvtdq2ps x, x` after
# `movd x, r/m32` or the zeroing idiom is exact in every lane.
# name -> (bytes, {site offset: "lane0"|"full"})
LANE0_CASES = {
    "xorps then scalar store, register killed": (
        "0f57d1"            # +0 xorps xmm2, xmm1
        "f30f1112"          # +3 movss [edx], xmm2
        "0f57d2"            # +7 xorps xmm2, xmm2 (idiom: exact zero, not a site)
        "c3",
        {0: "lane0"}),
    "xorps live at ret keeps the full width": (
        "0f57d1"            # +0 xorps xmm2, xmm1
        "f30f1112"          # +3 movss [edx], xmm2
        "c3",
        {0: "full"}),
    "xorps live across a call keeps the full width": (
        "0f57d1"            # +0 xorps xmm2, xmm1
        "e8fb0f0000"        # +3 call +0x1000
        "f30f1112"          # +8 movss [edx], xmm2
        "0f57d2"            # +12 xorps xmm2, xmm2
        "c3",
        {0: "full"}),
    "xorps whole-register store observes": (
        "0f57d1"            # +0 xorps xmm2, xmm1
        "0f1112"            # +3 movups [edx], xmm2
        "0f57d2"            # +6 xorps xmm2, xmm2
        "c3",
        {0: "full"}),
    "xorps result copied then stored whole": (
        "0f57d1"            # +0 xorps xmm2, xmm1
        "0f28da"            # +3 movaps xmm3, xmm2
        "0f111a"            # +6 movups [edx], xmm3
        "0f57d2"            # +9 xorps xmm2, xmm2
        "0f57db"            # +12 xorps xmm3, xmm3
        "c3",
        {0: "full", 3: "full"}),
    "copy then scalar use, both killed": (
        "0f28da"            # +0 movaps xmm3, xmm2
        "f30f111a"          # +3 movss [edx], xmm3
        "0f57db"            # +7 xorps xmm3, xmm3
        "c3",
        {0: "lane0"}),
    "copy then unpcklps observes lane 1": (
        "0f28da"            # +0 movaps xmm3, xmm2
        "0f14d9"            # +3 unpcklps xmm3, xmm1
        "0f111a"            # +6 movups [edx], xmm3
        "0f57db"            # +9 xorps xmm3, xmm3
        "c3",
        {0: "full"}),
    "copy then 64-bit store observes lane 1": (
        "0f28da"            # +0 movaps xmm3, xmm2
        "660fd61a"          # +3 movq [edx], xmm3
        "0f57db"            # +7 xorps xmm3, xmm3
        "c3",
        {0: "full"}),
    "copy overwritten by a full load is exact": (
        "0f28da"            # +0 movaps xmm3, xmm2
        "f30f111a"          # +3 movss [edx], xmm3
        "0f281a"            # +7 movaps xmm3, [edx]
        "0f111a"            # +10 movups [edx], xmm3
        "c3",
        {0: "lane0"}),
    "xorpd is not a lane-0 mnemonic (never a candidate, never a site)": (
        "660f57d1"          # +0 xorpd xmm2, xmm1
        "f30f1112"          # +4 movss [edx], xmm2
        "0f57d2"            # +8 xorps xmm2, xmm2
        "c3",
        {}),
    "xorps zeroing idiom is never a site": (
        "0f57d2"            # +0 xorps xmm2, xmm2
        "f30f1112"          # +3 movss [edx], xmm2
        "c3",
        {}),
    "cvtdq2ps then scalar use, killed": (
        "0f5bd1"            # +0 cvtdq2ps xmm2, xmm1
        "f30f1112"          # +3 movss [edx], xmm2
        "0f57d2"            # +7 xorps xmm2, xmm2
        "c3",
        {0: "lane0"}),
    "cvtdq2ps live at ret": (
        "0f5bd1"            # +0 cvtdq2ps xmm2, xmm1
        "f30f1112"          # +3 movss [edx], xmm2
        "c3",
        {0: "full"}),
    "movd then cvtdq2ps is exact even when stored whole": (
        "660f6ec8"          # +0 movd xmm1, eax
        "0f5bc9"            # +4 cvtdq2ps xmm1, xmm1
        "0f110a"            # +7 movups [edx], xmm1
        "c3",
        {4: "lane0"}),
    "movd from memory then cvtdq2ps is exact": (
        "660f6e09"          # +0 movd xmm1, [ecx]
        "0f5bc9"            # +4 cvtdq2ps xmm1, xmm1
        "0f110a"            # +7 movups [edx], xmm1
        "c3",
        {4: "lane0"}),
    "zeroing idiom then cvtdq2ps is exact": (
        "0f57c9"            # +0 xorps xmm1, xmm1
        "0f5bc9"            # +3 cvtdq2ps xmm1, xmm1
        "0f110a"            # +6 movups [edx], xmm1
        "c3",
        {3: "lane0"}),
    "movd then a scalar load breaks the zero-upper fact": (
        "660f6ec8"          # +0 movd xmm1, eax
        "f30f1009"          # +4 movss xmm1, [ecx]
        "0f5bc9"            # +8 cvtdq2ps xmm1, xmm1
        "0f110a"            # +11 movups [edx], xmm1
        "c3",
        {8: "full"}),
    "movd then cvtdq2ps into another register stored whole": (
        "660f6ec8"          # +0 movd xmm1, eax
        "0f5bd1"            # +4 cvtdq2ps xmm2, xmm1
        "0f1112"            # +7 movups [edx], xmm2
        "c3",
        {4: "full"}),
    "movd, call, cvtdq2ps: the fact does not cross a call": (
        "660f6ec8"          # +0 movd xmm1, eax
        "e8fb0f0000"        # +4 call +0x1000
        "0f5bc9"            # +9 cvtdq2ps xmm1, xmm1
        "0f110a"            # +12 movups [edx], xmm1
        "c3",
        {9: "full"}),
    "xmm0 lane-0 result at return is pinned": (
        "0f57c1"            # +0 xorps xmm0, xmm1
        "f30f1102"          # +3 movss [edx], xmm0
        "c3",
        {0: "full"}),
    "lane-0 result observed in a successor block": (
        "0f57d1"            # +0  xorps xmm2, xmm1
        "85c0"              # +3  test eax, eax
        "7407"              # +5  je +7 -> +14
        "0f1112"            # +7  movups [edx], xmm2
        "0f57d2"            # +10 xorps xmm2, xmm2
        "c3"                # +13 ret
        "f30f1112"          # +14 movss [edx], xmm2
        "0f57d2"            # +18 xorps xmm2, xmm2
        "c3",               # +21 ret
        {0: "full"}),
    "lane-0 result killed on both paths": (
        "0f57d1"            # +0  xorps xmm2, xmm1
        "f30f1112"          # +3  movss [edx], xmm2
        "85c0"              # +7  test eax, eax
        "7405"              # +9  je +5 -> +16
        "0f57d2"            # +11 xorps xmm2, xmm2
        "eb03"              # +14 jmp +3 -> +19
        "0f57d2"            # +16 xorps xmm2, xmm2
        "0f1112"            # +19 movups [edx], xmm2
        "c3",               # +22 ret
        {0: "lane0"}),
    "lane-0 sites chain: second observes the first only through lane 0": (
        "0f57d1"            # +0  xorps xmm2, xmm1
        "0f54d3"            # +3  andps xmm2, xmm3
        "f30f1112"          # +6  movss [edx], xmm2
        "0f57d2"            # +10 xorps xmm2, xmm2
        "c3",
        {0: "lane0", 3: "lane0"}),
    # guest_0010.c @000574f7: a lane-0 op, a scalar load into the same
    # register whose zero fill is elided (nothing observes lanes 1..3), a
    # call.  The site's stale lanes 1..3 are physically live at the call
    # although the load rewrote the register's taint: the site stays pinned.
    "lane-0 op, elided scalar load into the same register, call": (
        "0f5701"            # +0  xorps xmm0, [ecx]
        "f30f10442420"      # +3  movss xmm0, [esp+0x20]   (fill elided)
        "f30f58c1"          # +9  addss xmm0, xmm1
        "f30f11442444"      # +13 movss [esp+0x44], xmm0
        "e8fb0f0000"        # +19 call +0x1000
        "0f57c0"            # +24 xorps xmm0, xmm0
        "c3",
        {0: "full"}),
    "lane-0 op, elided movsd load into the same register, call": (
        "0f5701"            # +0  xorps xmm0, [ecx]
        "f20f1001"          # +3  movsd xmm0, [ecx]        (fill elided)
        "f20f1102"          # +7  movsd [edx], xmm0
        "e8fb0f0000"        # +11 call +0x1000
        "0f57c0"            # +16 xorps xmm0, xmm0
        "c3",
        {0: "full"}),
    "lane-0 op, scalar load, register killed before the call": (
        "0f5701"            # +0  xorps xmm0, [ecx]
        "f30f1001"          # +3  movss xmm0, [ecx]
        "f30f1102"          # +7  movss [edx], xmm0
        "0f57c0"            # +11 xorps xmm0, xmm0
        "e8fb0f0000"        # +14 call +0x1000
        "c3",
        {0: "lane0"}),
    "lane-0 op, scalar load, full load before the call": (
        "0f5701"            # +0  xorps xmm0, [ecx]
        "f30f1001"          # +3  movss xmm0, [ecx]
        "f30f1102"          # +7  movss [edx], xmm0
        "0f2801"            # +11 movaps xmm0, [ecx]
        "e8fb0f0000"        # +14 call +0x1000
        "c3",
        {0: "lane0"}),
    "lane-0 op, scalar load, live at ret": (
        "0f5711"            # +0  xorps xmm2, [ecx]
        "f30f1011"          # +3  movss xmm2, [ecx]
        "f30f1112"          # +7  movss [edx], xmm2
        "c3",
        {0: "full"}),
    "lane-0 copy and its source reach the call through a scalar load": (
        "0f57d1"            # +0  xorps xmm2, xmm1
        "0f28c2"            # +3  movaps xmm0, xmm2       (lane-0 copy site)
        "f30f1001"          # +6  movss xmm0, [ecx]
        "f30f1102"          # +10 movss [edx], xmm0
        "0f57d2"            # +14 xorps xmm2, xmm2
        "e8fb0f0000"        # +17 call +0x1000
        "0f57c0"            # +22 xorps xmm0, xmm0
        "c3",
        {0: "full", 3: "full"}),
    # Conservative by construction: the load's fill is kept (the whole store
    # observes it), which zeroes the site's stale lanes, but the walk keeps
    # the site's entry past the load and the store pins it too.
    "lane-0 op, scalar load whose fill is kept, whole store (conservative)": (
        "0f5701"            # +0  xorps xmm0, [ecx]
        "f30f1102"          # +3  movss [edx], xmm0
        "f30f1001"          # +7  movss xmm0, [ecx]        (fill kept)
        "0f1102"            # +11 movups [edx], xmm0
        "c3",
        {0: "full"}),
}


def analyse(raw: bytes):
    ctx = T.Ctx("synthetic", 0x400000, TEXT, TEXT + len(raw), raw)
    insns, order, indirect = T.decode(ctx, TEXT)
    _leaders, block_of, members, succs = T.blocks(insns, order, TEXT)
    return E.xmm_zero_fill_elidable(insns, block_of, members, succs), insns


def analyse_lane0(raw: bytes):
    ctx = T.Ctx("synthetic", 0x400000, TEXT, TEXT + len(raw), raw)
    insns, order, indirect = T.decode(ctx, TEXT)
    _leaders, block_of, members, succs = T.blocks(insns, order, TEXT)
    candidates = E.xmm_lane0_candidates(insns)
    return E.xmm_lane0_sites(insns, block_of, members, succs), candidates, insns


def main() -> int:
    failures = 0
    for name, (hex_bytes, expected) in CASES.items():
        raw = bytes.fromhex(hex_bytes)
        elidable, insns = analyse(raw)
        loads = sorted(rva for rva, ins in insns.items()
                       if ins.mnemonic in ("movss", "movsd") and
                       ins.op_str.startswith("xmm") and "ptr" in ins.op_str)
        got = {rva - TEXT: ("elide" if rva in elidable else "pin")
               for rva in loads}
        if got != expected:
            failures += 1
            print(f"FAIL {name}: expected {expected}, got {got}",
                  file=sys.stderr)
            for rva in sorted(insns):
                ins = insns[rva]
                print(f"      {rva - TEXT:+3d} {ins.mnemonic} {ins.op_str}",
                      file=sys.stderr)
    print(f"xmm zero-fill elision unit gate: {len(CASES) - failures}/"
          f"{len(CASES)} cases")
    lane0_failures = 0
    for name, (hex_bytes, expected) in LANE0_CASES.items():
        raw = bytes.fromhex(hex_bytes)
        sites, candidates, insns = analyse_lane0(raw)
        interesting = set()
        for rva, ins in insns.items():
            if (ins.mnemonic not in E.sse_lower.LANE0_MNEMONICS or
                    not ins.op_str.startswith("xmm")):
                continue
            operands = ins.op_str.split(", ")
            if len(operands) != 2 or operands[0].startswith("xmmword"):
                continue                        # 16-byte store: not a site
            if (ins.mnemonic in ("xorps", "xorpd", "pxor") and
                    operands[0] == operands[1]):
                continue                        # zeroing idiom: never a site
            if (ins.mnemonic in E.sse_lower.LANE0_MOVES and
                    operands[1].startswith("xmmword")):
                continue                        # 16-byte load: not a site
            interesting.add(rva)
        got = {rva - TEXT: ("lane0" if rva in sites else "full")
               for rva in sorted(interesting)}
        stray = sites - candidates - E.xmm_cvt_zero_upper_sites(
            insns, T.blocks(insns, sorted(insns), TEXT)[2])
        if got != expected or stray:
            lane0_failures += 1
            print(f"FAIL lane0 {name}: expected {expected}, got {got}"
                  f"{' stray ' + str(stray) if stray else ''}", file=sys.stderr)
            for rva in sorted(insns):
                ins = insns[rva]
                print(f"      {rva - TEXT:+3d} {ins.mnemonic} {ins.op_str}",
                      file=sys.stderr)
    print(f"xmm lane-0 lowering unit gate: {len(LANE0_CASES) - lane0_failures}/"
          f"{len(LANE0_CASES)} cases")
    return 1 if (failures or lane0_failures) else 0


if __name__ == "__main__":
    raise SystemExit(main())
