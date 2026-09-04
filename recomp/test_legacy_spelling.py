#!/usr/bin/env python3
"""gpr_locals.legacy_text / legacy_statements over every emitter spelling at
once: the GPR tokens, the dual-spelled inlined leaf site (leaf_inline.py), the
IAT import tokens (GUEST_IMPORT_CALL/JMP) and the SSE lowering tokens
(sse_lower.py), nested the way the corpus nests them (an inlined leaf copy
that itself carries SSE and GPR tokens; SSE tokens whose address arguments
carry GR()/GSTACK_ADDR(); a paired comis consumer inside a jcc).

The normaliser is what gen_all's seam pins, the guest-stack lowering census
and the unit packing measure (unit_packing_size) see, so its output must be
the exact pre-token text -- byte for byte, line structure included -- with no
switch-dependent residue.  No corpus, no compiler: pure text.
"""
from __future__ import annotations

import difflib
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import gpr_locals as G                                       # noqa: E402
import sse_lower                                             # noqa: E402

BODY = "\n".join([
    "    /* 00401000  call 0x00402000 */",
    "    GPUSH(0x401005U); GUEST_STACK_CALLSITE_BARRIER();",
    "    #if GUEST_LEAF_INLINE",
    "        { /* GUEST_LEAF_INLINE: sub_00002000, 5 instructions */",
    "          /* 00402000  xorps xmm0, xmm1 */",
    "          GUEST_XMM_LANEOP0_R(u32, 4, ^, 0, 1);",
    "          /* 00402003  movaps xmm2, [eax+16] */",
    "          GUEST_XMM_LDX(2, GR(eax) + 16U);",
    "          /* 00402007  comiss xmm0, xmm1 */",
    "          GUEST_XMM_COMIS_PAIRED(f, 0, c->x[1].f[0]);",
    "          /* 0040200a  seta al */",
    "          GR(eax) = (GR(eax) & ~0xFFU) | (GUEST_XMM_FCC(a, c->x[0].f[0], c->x[1].f[0]) ? 1U : 0U);",
    "          /* 0040200d  ret */",
    "          (void)GPOP();",
    "          GUEST_STACK_CALLSITE_BARRIER();",
    "        }",
    "    #else",
    "        GUEST_GPR_FLUSH(c); sub_00002000(c); GUEST_GPR_RELOAD(c);",
    "    #endif",
    "    /* 00401005  call dword ptr [0x98c0a000] */",
    "    { uint32_t _target = ld32(0x98c0a000U);",
    "      GPUSH(0x40100bU);",
    "      GUEST_STACK_CALLSITE_BARRIER();",
    "      GUEST_GPR_FLUSH(c); GUEST_IMPORT_CALL(_target, 0x0080a000U, 17U); GUEST_GPR_RELOAD(c);",
    "    }",
    "    /* 0040100b  movups xmm3, [esp+8] */",
    "    GUEST_XMM_LDX(3, GSTACK_ADDR(8U, 16U));",
    "    /* 00401010  pxor xmm3, xmm4 */",
    "    GUEST_XMM_LANEOP_R(u32, 4, ^, 3, 4);",
    "    /* 00401014  comiss xmm3, [ecx] */",
    "    GUEST_XMM_COMIS_PAIRED(f, 3, ldf(GR(ecx)));",
    "    /* 00401017  jbe 0x00401030 */",
    "    if (GUEST_XMM_FCC(be, c->x[3].f[0], ldf(GR(ecx)))) goto L_00401030;",
    "    /* 00401019  cvtdq2ps xmm5, [esi] */",
    "    GUEST_XMM_CVTDQ2PS0_M(5, GR(esi));",
    "    /* 0040101c  movups [esp], xmm5 */",
    "    GUEST_XMM_STX(GSTACK_ADDR(0U, 16U), 5);",
    "    /* 00401020  jmp dword ptr [0x98c0a004] */",
    "    GUEST_GPR_FLUSH(c); GUEST_IMPORT_JMP(ld32(0x98c0a004U), 0x0080a004U, 18U); return;",
    "L_00401030:",
    "    /* 00401030  ret */",
    "    (void)GPOP();",
    "    GUEST_STACK_CALLSITE_BARRIER();",
    "    GUEST_GPR_FLUSH(c); return;",
    "",
])

EXPECTED = "\n".join([
    "    /* 00401000  call 0x00402000 */",
    "    gpush_generated(c, 0x401005U); GUEST_STACK_CALLSITE_BARRIER(); sub_00002000(c);",
    "    /* 00401005  call dword ptr [0x98c0a000] */",
    "    { uint32_t _target = ld32(0x98c0a000U);",
    "      gpush_generated(c, 0x40100bU);",
    "      GUEST_STACK_CALLSITE_BARRIER();",
    "      guest_call(c, _target);",
    "    }",
    "    /* 0040100b  movups xmm3, [esp+8] */",
    "    c->x[3] = ldx(guest_stack_address_generated(c, 8U, 16U));",
    "    /* 00401010  pxor xmm3, xmm4 */",
    "    { xmm_t _s = c->x[4]; int _i;",
    "      for (_i = 0; _i < 4; _i++) c->x[3].u32[_i] ^= _s.u32[_i]; }",
    "    /* 00401014  comiss xmm3, [ecx] */",
    "    { double _x = (double)c->x[3].f[0], _y = (double)(ldf(c->ecx));",
    "      GUEST_FL->f_op = FLAG_PARTIAL; GUEST_FL->f_sz = 4;",
    "      GUEST_FL->f_sf = 0; GUEST_FL->f_of = 0;",
    "      if (_x != _x || _y != _y) {",
    "        GUEST_FL->f_cf = 1; GUEST_FL->f_zf = 1; GUEST_FL->f_pf = 1;",
    "      } else {",
    "        GUEST_FL->f_cf = (uint8_t)(_x < _y);",
    "        GUEST_FL->f_zf = (uint8_t)(_x == _y); GUEST_FL->f_pf = 0;",
    "      } }",
    "    /* 00401017  jbe 0x00401030 */",
    "    if (cc_be(GUEST_FL)) goto L_00401030;",
    "    /* 00401019  cvtdq2ps xmm5, [esi] */",
    "    { xmm_t _s = ldx(c->esi); int _i;",
    "      for (_i = 0; _i < 4; _i++) c->x[5].f[_i] = (float)_s.i32[_i]; }",
    "    /* 0040101c  movups [esp], xmm5 */",
    "    stx(guest_stack_address_generated(c, 0U, 16U), c->x[5]);",
    "    /* 00401020  jmp dword ptr [0x98c0a004] */",
    "    GUEST_FLAGS_FLUSH(c); guest_call(c, ld32(0x98c0a004U)); return;",
    "L_00401030:",
    "    /* 00401030  ret */",
    "    (void)gpop_generated(c);",
    "    GUEST_STACK_CALLSITE_BARRIER();",
    "    return;",
    "",
])

STATEMENTS = (
    ("GUEST_XMM_LANEOP0_R(u32, 4, ^, 0, 1);",
     "{ xmm_t _s = c->x[1]; int _i;\n"
     "  for (_i = 0; _i < 4; _i++) c->x[0].u32[_i] ^= _s.u32[_i]; }"),
    ("GUEST_GPR_FLUSH(c); GUEST_IMPORT_CALL(_target, 0x0080a000U, 17U); GUEST_GPR_RELOAD(c);",
     "guest_call(c, _target);"),
    ("GUEST_GPR_FLUSH(c); GUEST_IMPORT_JMP(ld32(0x98c0a004U), 0x0080a004U, 18U); return;",
     "GUEST_FLAGS_FLUSH(c); guest_call(c, ld32(0x98c0a004U)); return;"),
    ("if (GUEST_XMM_FCC(e, c->x[0].f[0], ldf(GR(esi) + 4U))) goto L_00401030;",
     "if (cc_e(GUEST_FL)) goto L_00401030;"),
    ("GUEST_XMM_STX(GSTACK_ADDR(0U, 16U), 5);",
     "stx(guest_stack_address_generated(c, 0U, 16U), c->x[5]);"),
    ("GUEST_XMM_MOV0(2, 7);", "c->x[2] = c->x[7];"),
    ("GPUSH(0x401005U); GUEST_STACK_CALLSITE_BARRIER();\n"
     "#if GUEST_LEAF_INLINE\n"
     "    { /* GUEST_LEAF_INLINE: sub_00002000, 1 instructions */\n"
     "      GUEST_XMM_MOV(0, 1);\n"
     "      (void)GPOP();\n"
     "      GUEST_STACK_CALLSITE_BARRIER();\n"
     "    }\n"
     "#else\n"
     "    GUEST_GPR_FLUSH(c); sub_00002000(c); GUEST_GPR_RELOAD(c);\n"
     "#endif",
     "gpush_generated(c, 0x401005U); GUEST_STACK_CALLSITE_BARRIER(); sub_00002000(c);"),
)


def main() -> int:
    got = G.legacy_text(BODY)
    if got != EXPECTED:
        sys.stdout.writelines(difflib.unified_diff(
            EXPECTED.splitlines(True), got.splitlines(True), "expected", "got"))
        raise SystemExit("legacy_text: synthetic body mismatch")
    for residue in ("GUEST_XMM_", "GUEST_IMPORT_", "GUEST_LEAF_INLINE",
                    "GUEST_GPR_", "GR(", "GPUSH(", "GPOP(", "GSTACK_ADDR("):
        if residue in got:
            raise SystemExit("legacy_text left %r behind" % residue)
    mapped = G.legacy_statements(tuple(s for s, _e in STATEMENTS))
    for (statement, expected), actual in zip(STATEMENTS, mapped):
        if actual != expected:
            raise SystemExit("legacy_statements(%r)\n  = %r\n != %r"
                             % (statement, actual, expected))
    # idempotent on legacy text (seam-owned text never changes)
    if G.legacy_text(EXPECTED) != EXPECTED:
        raise SystemExit("legacy_text is not the identity on legacy text")
    # every SSE token name is known to the seam pass as GPR-pure, and every
    # statement-level token has a legacy renderer
    missing = [name for name in sse_lower.TOKEN_NAMES if name not in G.PURE_CALLS]
    if missing:
        raise SystemExit("SSE tokens unknown to gpr_locals.PURE_CALLS: %s" % missing)
    without = [name for name in sse_lower.TOKEN_NAMES
               if name != "GUEST_XMM_FCC" and name not in sse_lower._LEGACY]
    if without:
        raise SystemExit("SSE tokens without a legacy renderer: %s" % without)
    # the seam classifier sees only token/pure lines here, never a memory-mode
    # ("hard") line that would draw a FLUSH/RELOAD bracket around emitter text
    _codes, kinds, _pp = G.classify(BODY.split("\n"), "synthetic")
    if "hard" in kinds:
        raise SystemExit("classify() reports a hard line in emitter output")
    print("legacy spelling normalisers: PASS (%d -> %d bytes, %d statements, "
          "%d SSE token names)" % (len(BODY), len(got), len(STATEMENTS),
                                  len(sse_lower.TOKEN_NAMES)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
