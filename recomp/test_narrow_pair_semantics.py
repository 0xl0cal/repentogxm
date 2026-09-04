#!/usr/bin/env python3
"""Prove the byte/word direct flag pairs against the lazy path and x86 truth.

Since the pair representation carries its operand width, `cmp`/`test` on 8- and
16-bit operands are answered with a direct C comparison exactly like the dword
forms.  A signed relation must sign-extend from the narrow width; an unsigned
one must zero-extend; the dword forms must be unchanged.  This gate renders the
producer through the real emitter, then compiles a harness that evaluates, for
every DIRECT condition:

  * the direct expression the paired consumer would use,
  * the lazy path (the SET_FLAGS statement the unpaired producer emits, then
    the cc_* switch), and
  * an independent model of the x86 flags,

exhaustively over all 65,536 byte operand pairs (with garbage in the upper
register bits), over 8M word pairs and 16M dword pairs, for register/register
in both operand orders and for the immediate forms.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import tempfile
import textwrap

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone import x86 as cx

import emit as emitter_module
from trans import DIRECT

HERE = pathlib.Path(__file__).resolve().parent

# (label, bytes, operand width)
PRODUCERS = (
    ("cmp cl, dl", "38d1", 1),
    ("cmp dl, cl", "38ca", 1),
    ("cmp cl, 0x80", "80f980", 1),
    ("cmp cl, 0xff", "80f9ff", 1),
    ("cmp cl, 0", "80f900", 1),
    ("test cl, dl", "84d1", 1),
    ("test cl, cl", "84c9", 1),
    ("test cl, 0x80", "f6c180", 1),
    ("cmp cx, dx", "6639d1", 2),
    ("cmp dx, cx", "6639ca", 2),
    ("cmp cx, 0x8000", "6681f90080", 2),
    ("test cx, dx", "6685d1", 2),
    ("cmp ecx, edx", "39d1", 4),
    ("cmp edx, ecx", "39ca", 4),
    ("test ecx, edx", "85d1", 4),
    ("cmp ecx, 0x80000000", "81f900000080", 4),
)

CONDITIONS = ("b", "ae", "be", "a", "l", "ge", "le", "g", "e", "ne")
assert set(CONDITIONS) == set(DIRECT)


def fail(message: str) -> None:
    raise SystemExit(message)


def decode(raw: bytes):
    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    decoder.detail = True
    instructions = list(decoder.disasm(raw, 0x1000))
    if len(instructions) != 1 or instructions[0].size != len(raw):
        fail(f"test instruction did not decode exactly: {raw.hex()}")
    return instructions[0]


def truth_expression(mnemonic: str, cc: str) -> str:
    """Independent model of the x86 condition, in terms of the harness's
    masked/extended operands ua, ub (unsigned) and sa, sb (signed)."""
    if mnemonic == "cmp":
        return {
            "b": "(ua < ub)", "ae": "(ua >= ub)", "be": "(ua <= ub)",
            "a": "(ua > ub)", "l": "(sa < sb)", "ge": "(sa >= sb)",
            "le": "(sa <= sb)", "g": "(sa > sb)", "e": "(ua == ub)",
            "ne": "(ua != ub)",
        }[cc]
    # test: CF = OF = 0, ZF/SF from the masked AND.
    return {
        "b": "0", "ae": "1", "be": "tz", "a": "(!tz)", "l": "ts",
        "ge": "(!ts)", "le": "(tz || ts)", "g": "(!tz && !ts)", "e": "tz",
        "ne": "(!tz)",
    }[cc]


def build_case(index: int, label: str, raw_hex: str, width: int) -> str:
    ins = decode(bytes.fromhex(raw_hex))
    em = emitter_module.Emitter(None, "narrow_pair_oracle", {}, {}, {})
    pair = em.producer_operands(ins)
    if pair is None or len(pair) != 3 or pair[2] != width:
        fail(f"{label}: producer_operands did not return a width-tagged pair: "
             f"{pair!r}")
    lazy = "\n".join(em.emit(ins, True, None))
    if "SET_FLAGS(GUEST_FL" not in lazy:
        fail(f"{label}: lazy producer did not record flags: {lazy}")
    direct = {cc: em.condition(cc, pair) for cc in CONDITIONS}
    switch = {cc: em.condition(cc, None) for cc in CONDITIONS}
    for cc in CONDITIONS:
        if "GUEST_FL" in direct[cc] or "cc_" in direct[cc]:
            fail(f"{label}: {cc} was not answered directly: {direct[cc]}")
        if switch[cc] != "%s(GUEST_FL)" % emitter_module.CC[cc][0]:
            fail(f"{label}: unexpected switch consumer text: {switch[cc]}")

    imm = None
    for op in ins.operands:
        if op.type == cx.X86_OP_IMM:
            imm = op.imm
    mask = (1 << (8 * width)) - 1
    if width == 1:
        loop = ("for (a = 0; a <= 0xffU; a++) for (b = 0; b <= 0xffU; b++)")
    elif width == 2:
        loop = ("for (a = 0; a <= 0xffffU; a++) "
                "for (bi = 0; bi < sizeof probes16 / sizeof probes16[0]; bi++)"
                " if ((b = probes16[bi] ^ (a * 0x9e37U)) || 1)")
    else:
        loop = ("for (ai = 0; ai < 4096U; ai++) for (bi = 0; bi < 4096U; bi++)"
                " if ((a = probe32(ai)) || 1) if ((b = probe32(bi)) || 1)")
    # The second operand of an immediate form is the immediate itself.
    b_source = ("(uint32_t)0x%xU" % (imm & mask)) if imm is not None else "b"
    garbage_a = {1: "0xa5a5a500U", 2: "0x5a5a0000U", 4: "0U"}[width]
    garbage_b = {1: "0x3c3c3c00U", 2: "0xc3c30000U", 4: "0U"}[width]
    # Which register holds `a` in the instruction text: the first register
    # operand.  For `cmp dl, cl` that is edx.
    first_reg = ins.reg_name(ins.operands[0].reg)
    a_reg, b_reg = (("edx", "ecx") if first_reg in ("dl", "dx", "edx")
                    else ("ecx", "edx"))
    checks = []
    for cc in CONDITIONS:
        checks.append(textwrap.dedent(f"""
            {{
                int direct = ({direct[cc]}) ? 1 : 0;
                int truth = ({truth_expression(ins.mnemonic, cc)}) ? 1 : 0;
                if (direct != truth || lazy_{cc} != truth) {{
                    fprintf(stderr, "{label}: cc={cc} a=0x%x b=0x%x "
                            "direct=%d lazy=%d truth=%d\\n",
                            (unsigned)ua, (unsigned)ub, direct, lazy_{cc},
                            truth);
                    return 1;
                }}
            }}""").strip("\n"))
    lazy_reads = "\n".join(
        f"        int lazy_{cc} = ({switch[cc]}) ? 1 : 0;" for cc in CONDITIONS)
    same_reg = (ins.operands[1].type == cx.X86_OP_REG and
                ins.operands[0].reg == ins.operands[1].reg)
    b_assign = "" if imm is not None else (
        f"        c->{b_reg} = (b & {mask:#x}U) | {garbage_b};\n")
    if same_reg:
        # test cl, cl: both operands are the same register; model b as a.
        b_source = "a"
    return textwrap.dedent(f"""
    /* {label}  ({raw_hex}) width {width} */
    static int case_{index}(void)
    {{
        CPU state;
        CPU *c = &state;
        uint32_t a = 0, b = 0, ai = 0, bi = 0;
        unsigned long long count = 0;
        {loop} {{
            uint32_t ua, ub;
            int32_t sa, sb;
            uint32_t tr;
            int tz, ts;
            memset(c, 0, sizeof *c);
            c->{a_reg} = (a & {mask:#x}U) | {garbage_a};
    {b_assign.rstrip()}
            ua = a & {mask:#x}U;
            ub = ({b_source}) & {mask:#x}U;
            sa = sign_extend(ua, {width});
            sb = sign_extend(ub, {width});
            tr = ua & ub;
            tz = tr == 0U;
            ts = (tr >> ({8 * width} - 1)) & 1U;
            (void)sa; (void)sb; (void)tz; (void)ts;
    {textwrap.indent(lazy, "        ")}
    {lazy_reads}
    {textwrap.indent(chr(10).join(checks), "        ")}
            count++;
        }}
        printf("{label}: %llu operand pairs x {len(CONDITIONS)} conditions OK\\n", count);
        return 0;
    }}
    """)


def build_source() -> str:
    cases = [build_case(i, *spec) for i, spec in enumerate(PRODUCERS)]
    calls = "\n".join(f"    if (case_{i}()) return 1;" for i in range(len(PRODUCERS)))
    return f"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "guest.h"

static int32_t sign_extend(uint32_t v, int width)
{{
    if (width == 1) return (int32_t)(int8_t)(uint8_t)v;
    if (width == 2) return (int32_t)(int16_t)(uint16_t)v;
    return (int32_t)v;
}}

static const uint32_t probes16[] = {{
    0U, 1U, 2U, 0x7fU, 0x80U, 0xffU, 0x100U, 0x7ffeU, 0x7fffU, 0x8000U,
    0x8001U, 0xfffeU, 0xffffU, 0x1234U, 0xedcbU, 0x4000U, 0xc000U, 0x0101U,
    0x8080U, 0x7f7fU, 0x00ffU, 0xff00U, 0x5555U, 0xaaaaU,
}};

/* 4096 dword probes: the boundary values plus an LCG spray. */
static uint32_t probe32(uint32_t i)
{{
    static const uint32_t edges[] = {{
        0U, 1U, 2U, 0x7fU, 0x80U, 0xffU, 0x7fffU, 0x8000U, 0xffffU,
        0x7ffffffeU, 0x7fffffffU, 0x80000000U, 0x80000001U, 0xfffffffeU,
        0xffffffffU, 0x12345678U,
    }};
    if (i < sizeof edges / sizeof edges[0])
        return edges[i];
    return i * 2654435761U + 0x9e3779b9U;
}}
{"".join(cases)}
int main(void)
{{
{calls}
    puts("narrow pair oracle: PASS");
    return 0;
}}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", help="GCC-compatible host C compiler; omit to "
                        "only render and syntax-check the harness in Python")
    parser.add_argument("--emit", type=pathlib.Path,
                        help="write the harness source here instead of compiling")
    arguments = parser.parse_args()
    source = build_source()
    if arguments.emit is not None:
        arguments.emit.write_text(source, encoding="ascii", newline="\n")
        print(f"harness written: {arguments.emit}")
        return 0
    if arguments.cc is None:
        print("narrow pair oracle: rendered %d producer cases (no --cc, not run)"
              % len(PRODUCERS))
        return 0
    with tempfile.TemporaryDirectory(prefix="isaac-narrow-pair-") as work_raw:
        work = pathlib.Path(work_raw)
        source_path = work / "narrow_pair.c"
        binary_path = work / "narrow_pair"
        source_path.write_text(source, encoding="ascii", newline="\n")
        command = [
            arguments.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-variable", "-Wno-unused-but-set-variable",
            "-Wno-type-limits",
            "-fsanitize=undefined", "-fno-sanitize-recover=all",
            "-I", os.fspath(HERE / "runtime"), os.fspath(source_path),
            "-o", os.fspath(binary_path),
        ]
        built = subprocess.run(command, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, check=False)
        if built.stdout:
            print(built.stdout, end="")
        if built.returncode:
            fail(f"narrow pair oracle compile failed: {built.returncode}")
        ran = subprocess.run([os.fspath(binary_path)], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, check=False,
                             timeout=600)
        if ran.stdout:
            print(ran.stdout, end="")
        if ran.returncode:
            fail(f"narrow pair oracle execution failed: {ran.returncode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
