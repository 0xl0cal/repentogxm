"""SSE lowering (generation switch GUEST_SSE_LOWER): token spelling, the exact
legacy text of every token, the comis/jcc float-relation table.

Today the emitter spells every packed SSE operation as a 4-iteration C loop
over the lanes of `xmm_t` and every 16-byte move as a union copy through
`ldx`/`stx`.  GCC 10 -O2 on the Cortex-A9 neither unrolls nor vectorises
those loops (an `xorps` is a 16-byte copy through the native stack plus
4x{ldr, ldr, eors, str, cmp, bne}) and copies the union through a stack
temporary.  A comiss/ucomiss followed by a jcc goes through the FLAG_PARTIAL
producer and the cc_* consumer: three vcmp + three serialising vmrs per branch.

With the generation switch on (environment GUEST_SSE_LOWER unset or "1"; "0"
reproduces the old corpus byte for byte) the emitter spells those shapes
through the GUEST_XMM_* tokens of guest.h:

  * header knob GUEST_SSE_LOWER=0 (default): every token expands to exactly
    the statement the emitter always produced, so the object code is
    unchanged and a device A/B bisects by relink;
  * header knob GUEST_SSE_LOWER=1 (generated units only, GCC): xmm_t gains
    GCC vector views and the tokens become one NEON operation each (veor,
    vand, vorr, vbic, vadd, vsub, vshl, vshr, vext, vzip, vtbl,
    vcvt.f32.s32 q) or one unaligned vld1/vst1 for the 16-byte moves.  Only
    bitwise, integer, shuffle and int->float lane operations are lowered:
    NEON float arithmetic flushes denormals and saturates float->int, so
    cvtps2dq/cvttps2dq and every float arithmetic loop keep their scalar C.

Lane-0 forms: the emitter's upper-lane taint analysis (emit.py,
xmm_upper_lane_taint) proves for a packed op or a register copy that no
instruction of the function observes lanes 1..3 of its result (the same
analysis that already elides the movss zero fill).  Those sites are spelled
through the *0 tokens: knob 0 expands the full legacy loop, knob 1 only lane 0
(`c->x[d].u32[0] ^= c->x[s].u32[0]`, `c->x[d].f[0] = (float)c->x[s].i32[0]`),
four core/VFP instructions and no NEON at all.  Lane 0 is bit-exact either way;
lanes 1..3 differ only where nothing can read them.

comis pairing: comiss/ucomiss/comisd/ucomisd are paired with an adjacent (or
register-safe) jcc/setcc/cmovcc exactly like the narrow cmp/jcc pairing.  The
producer is spelled GUEST_XMM_COMIS_PAIRED(fld, d, rhs) (knob 0: the FLAG_PARTIAL
producer; knob 1: nothing) and the consumer's condition GUEST_XMM_FCC(cc, x, y)
(knob 0: cc_<cc>(GUEST_FL); knob 1: one C99 relation whose unordered behaviour
matches the x86 flag encoding -- comis: unordered => ZF=PF=CF=1, greater => 000,
less => CF, equal => ZF):

    cc   flags read       meaning                 relation (knob 1)
    a    CF=0 & ZF=0      greater                 isgreater(x, y)
    ae   CF=0             greater or equal        isgreaterequal(x, y)
    b    CF=1             less or unordered       !isgreaterequal(x, y)
    be   CF=1 | ZF=1      not greater             !isgreater(x, y)
    e    ZF=1             equal or unordered      !islessgreater(x, y)
    ne   ZF=0             less or greater         islessgreater(x, y)
    p    PF=1             unordered               isunordered(x, y)
    np   PF=0             ordered                 !isunordered(x, y)

g/ge/l/le/s/ns/o/no read SF/OF (always 0 after comis) and are never paired.
The relations evaluate each operand once (GCC builtins), so a `comiss xmm, m32`
memory operand is loaded once, as in the lazy producer.  ss compares in float
(float->double widening is exact, so `(double)a < (double)b` == `a < b`).

`legacy_sse_tokens()` maps rendered text (or one statement) back to the exact
pre-token spelling, line structure included, so gen_all's pins and the unit
packing measure (gen_all.unit_packing_size via gpr_locals.legacy_text) see what
they always saw.
"""
from __future__ import annotations

import os
import re

ENV = "GUEST_SSE_LOWER"


def enabled() -> bool:
    """Generation-time switch.  Unset or "1": tokens; "0": the old spelling."""
    value = os.environ.get(ENV, "1")
    if value not in ("0", "1"):
        raise ValueError("%s must be 0 or 1, not %r" % (ENV, value))
    return value == "1"


# comis family producers and the conditions they can answer directly.
COMIS = frozenset(("comiss", "ucomiss", "comisd", "ucomisd"))
FLOAT_DIRECT = {
    "a":  "isgreater({a}, {b})",
    "ae": "isgreaterequal({a}, {b})",
    "b":  "(!isgreaterequal({a}, {b}))",
    "be": "(!isgreater({a}, {b}))",
    "e":  "(!islessgreater({a}, {b}))",
    "ne": "islessgreater({a}, {b})",
    "p":  "isunordered({a}, {b})",
    "np": "(!isunordered({a}, {b}))",
}
# Values of the pair's third slot for a comis producer (cmp/test carry the
# operand width 1/2/4 there).
FLOAT_PAIR_KINDS = frozenset(("f", "d"))

# Bitwise / integer lane operations spelled through GUEST_XMM_LANEOP_*:
# mnemonic -> (union field, lane count, C operator).
LANEOPS = {
    "xorps": ("u32", 4, "^"), "xorpd": ("u32", 4, "^"), "pxor": ("u32", 4, "^"),
    "andps": ("u32", 4, "&"), "andpd": ("u32", 4, "&"), "pand": ("u32", 4, "&"),
    "orps": ("u32", 4, "|"), "orpd": ("u32", 4, "|"), "por": ("u32", 4, "|"),
    "paddd": ("u32", 4, "+"), "psubd": ("u32", 4, "-"),
    "paddw": ("u16", 8, "+"), "psubw": ("u16", 8, "-"),
    "paddb": ("u8", 16, "+"), "psubb": ("u8", 16, "-"),
    "paddq": ("u64", 2, "+"),
}
# Sites whose result may be emitted lane-0-only when the analysis proves the
# upper lanes unobserved: 32-bit lane operations, register copies, cvtdq2ps.
# The pd bitwise forms are not modelled lane-wise by emit.xmm_upper_lane_taint
# (they fall through to "observes every lane"), so they never get a lane-0
# form; emit.xmm_lane0_candidates re-checks against _XMM_LANEWISE.
LANE0_LANEOPS = frozenset(m for m, (fld, _n, _op) in LANEOPS.items()
                          if fld == "u32") - frozenset(("xorpd", "andpd", "orpd"))
LANE0_MOVES = frozenset(("movaps", "movups", "movdqa", "movdqu"))
LANE0_MNEMONICS = LANE0_LANEOPS | LANE0_MOVES | frozenset(("cvtdq2ps",))

# The call-like names the tokens introduce into emitted text.  gpr_locals'
# seam pass and the corpus contract must know them: none reads or writes a
# general register (the FCC relations read xmm lanes and guest memory only).
TOKEN_NAMES = (
    "GUEST_XMM_MOV", "GUEST_XMM_MOV0", "GUEST_XMM_LDX", "GUEST_XMM_STX",
    "GUEST_XMM_LANEOP_R", "GUEST_XMM_LANEOP_M",
    "GUEST_XMM_LANEOP0_R", "GUEST_XMM_LANEOP0_M",
    "GUEST_XMM_PANDN_R", "GUEST_XMM_PANDN_M",
    "GUEST_XMM_CVTDQ2PS_R", "GUEST_XMM_CVTDQ2PS_M",
    "GUEST_XMM_CVTDQ2PS0_R", "GUEST_XMM_CVTDQ2PS0_M",
    "GUEST_XMM_PSRLDQ", "GUEST_XMM_PSRA_IMM", "GUEST_XMM_PSHL_IMM",
    "GUEST_XMM_PSRA_REG", "GUEST_XMM_PSHL_REG",
    "GUEST_XMM_PSHUFD_R", "GUEST_XMM_PSHUFD_M",
    "GUEST_XMM_SHUFPS_R", "GUEST_XMM_SHUFPS_M",
    "GUEST_XMM_UNPCK_R", "GUEST_XMM_UNPCK_M",
    "GUEST_XMM_PSHUFW_R", "GUEST_XMM_PSHUFW_M",
    "GUEST_XMM_PUNPCKLDQ_R", "GUEST_XMM_PUNPCKLDQ_M",
    "GUEST_XMM_COMIS_PAIRED", "GUEST_XMM_FCC",
)
PURE_NAMES = TOKEN_NAMES


# --------------------------------------------------------------- spelling ---
# Each statement token is one emitted statement `NAME(args);`; GUEST_XMM_FCC is
# an expression inside the consumer's condition.

def _x(i):
    return "c->x[%d]" % i


def token_mov(d, s, lane0=False):
    return "GUEST_XMM_MOV%s(%d, %d);" % ("0" if lane0 else "", d, s)


def token_ldx(d, addr):
    return "GUEST_XMM_LDX(%d, %s);" % (d, addr)


def token_stx(addr, s):
    return "GUEST_XMM_STX(%s, %d);" % (addr, s)


def token_laneop(fld, n, op, d, src_reg=None, src_addr=None, lane0=False):
    name = "GUEST_XMM_LANEOP%s" % ("0" if lane0 else "")
    if lane0 and fld != "u32":
        raise ValueError("lane-0 form is only defined for 32-bit lanes")
    if src_reg is not None:
        return "%s_R(%s, %d, %s, %d, %d);" % (name, fld, n, op, d, src_reg)
    return "%s_M(%s, %d, %s, %d, %s);" % (name, fld, n, op, d, src_addr)


def token_pandn(d, src_reg=None, src_addr=None):
    if src_reg is not None:
        return "GUEST_XMM_PANDN_R(%d, %d);" % (d, src_reg)
    return "GUEST_XMM_PANDN_M(%d, %s);" % (d, src_addr)


def token_cvtdq2ps(d, src_reg=None, src_addr=None, lane0=False):
    name = "GUEST_XMM_CVTDQ2PS%s" % ("0" if lane0 else "")
    if src_reg is not None:
        return "%s_R(%d, %d);" % (name, d, src_reg)
    return "%s_M(%d, %s);" % (name, d, src_addr)


def token_psrldq(d, n):
    return "GUEST_XMM_PSRLDQ(%d, %d);" % (d, n)


def token_psra_imm(bits, d, n):
    return "GUEST_XMM_PSRA_IMM(%d, %d, %d);" % (bits, d, n)


def token_pshl_imm(bits, op, d, n):
    return "GUEST_XMM_PSHL_IMM(%d, %s, %d, %d);" % (bits, op, d, n)


def token_psra_reg(bits, d, s):
    return "GUEST_XMM_PSRA_REG(%d, %d, %d);" % (bits, d, s)


def token_pshl_reg(bits, op, d, s):
    return "GUEST_XMM_PSHL_REG(%d, %s, %d, %d);" % (bits, op, d, s)


def token_pshufd(d, imm, src_reg=None, src_addr=None):
    if src_reg is not None:
        return "GUEST_XMM_PSHUFD_R(%d, %d, %d);" % (d, src_reg, imm)
    return "GUEST_XMM_PSHUFD_M(%d, %s, %d);" % (d, src_addr, imm)


def token_shufps(d, imm, src_reg=None, src_addr=None):
    if src_reg is not None:
        return "GUEST_XMM_SHUFPS_R(%d, %d, %d);" % (d, src_reg, imm)
    return "GUEST_XMM_SHUFPS_M(%d, %s, %d);" % (d, src_addr, imm)


def token_unpck(unit, hi, d, src_reg=None, src_addr=None):
    if src_reg is not None:
        return "GUEST_XMM_UNPCK_R(%d, %d, %d, %d);" % (unit, hi, d, src_reg)
    return "GUEST_XMM_UNPCK_M(%d, %d, %d, %s);" % (unit, hi, d, src_addr)


def token_pshufw(hi, d, imm, src_reg=None, src_addr=None):
    if src_reg is not None:
        return "GUEST_XMM_PSHUFW_R(%d, %d, %d, %d);" % (hi, d, src_reg, imm)
    return "GUEST_XMM_PSHUFW_M(%d, %d, %s, %d);" % (hi, d, src_addr, imm)


def token_punpckldq(d, src_reg=None, src_addr=None):
    if src_reg is not None:
        return "GUEST_XMM_PUNPCKLDQ_R(%d, %d);" % (d, src_reg)
    return "GUEST_XMM_PUNPCKLDQ_M(%d, %s);" % (d, src_addr)


def token_comis_paired(fld, d, rhs):
    """The dropped producer: `fld` f/d, `d` the first xmm operand, `rhs` the
    exact right-hand expression of the legacy producer (`c->x[s].f[0]` or
    `ldf(addr)`)."""
    return "GUEST_XMM_COMIS_PAIRED(%s, %d, %s);" % (fld, d, rhs)


def fcc(cc, a, b):
    """The paired consumer's condition expression."""
    if cc not in FLOAT_DIRECT:
        raise ValueError("condition %r has no float relation" % cc)
    return "GUEST_XMM_FCC(%s, %s, %s)" % (cc, a, b)


# ---------------------------------------------------------- legacy text -----
# The exact statements emit.py produced before the tokens, as lists of lines
# (the renderer prefixes each with the body indentation).

def legacy_mov(d, s):
    return ["%s = %s;" % (_x(d), _x(s))]


def legacy_ldx(d, addr):
    return ["%s = ldx(%s);" % (_x(d), addr)]


def legacy_stx(addr, s):
    return ["stx(%s, %s);" % (addr, _x(s))]


def _src(src_reg, src_addr):
    return _x(src_reg) if src_reg is not None else "ldx(%s)" % src_addr


def legacy_laneop(fld, n, op, d, src_reg=None, src_addr=None):
    return ["{ xmm_t _s = %s; int _i;" % _src(src_reg, src_addr),
            "  for (_i = 0; _i < %d; _i++) %s.%s[_i] %s= _s.%s[_i]; }"
            % (n, _x(d), fld, op, fld)]


def legacy_pandn(d, src_reg=None, src_addr=None):
    return ["{ xmm_t _d = %s, _s = %s; int _i;" % (_x(d), _src(src_reg, src_addr)),
            "  for (_i = 0; _i < 4; _i++) %s.u32[_i] = "
            "(~_d.u32[_i]) & _s.u32[_i]; }" % _x(d)]


def legacy_cvtdq2ps(d, src_reg=None, src_addr=None):
    return ["{ xmm_t _s = %s; int _i;" % _src(src_reg, src_addr),
            "  for (_i = 0; _i < 4; _i++) %s.f[_i] = (float)_s.i32[_i]; }"
            % _x(d)]


def legacy_psrldq(d, n):
    return ["{ xmm_t _r = (xmm_t){0}; unsigned _n = %dU, _i;" % n,
            "  for (_i = 0; _i + _n < 16; _i++) _r.u8[_i] = %s.u8[_i + _n];"
            % _x(d),
            "  %s = _r; }" % _x(d)]


def legacy_psra_imm(bits, d, n):
    lanes = 128 // bits
    return ["{ unsigned _n = %uU; int _i;" % n,
            "  if (_n > %d) _n = %d;" % (bits, bits),
            "  for (_i = 0; _i < %d; _i++) %s.i%d[_i] >>= _n; }"
            % (lanes, _x(d), bits)]


def legacy_pshl_imm(bits, op, d, n):
    lanes = 128 // bits
    return ["{ unsigned _n = %uU; int _i;" % n,
            "  if (_n >= %d) { %s = (xmm_t){0}; } else" % (bits, _x(d)),
            "  for (_i = 0; _i < %d; _i++) %s.u%d[_i] %s= _n; }"
            % (lanes, _x(d), bits, op)]


def legacy_psra_reg(bits, d, s):
    """The switch-off spelling of a register-count arithmetic shift (for the
    shape normalizer only: it reads the low dword and shifts by a count that
    may equal the lane width; the tokens use the low qword and saturate)."""
    lanes = 128 // bits
    return ["{ unsigned _n = %s.u32[0]; int _i;" % _x(s),
            "  if (_n > %d) _n = %d;" % (bits, bits),
            "  for (_i = 0; _i < %d; _i++) %s.i%d[_i] >>= _n; }"
            % (lanes, _x(d), bits)]


def legacy_pshl_reg(bits, op, d, s):
    lanes = 128 // bits
    return ["{ unsigned _n = %s.u32[0]; int _i;" % _x(s),
            "  if (_n >= %d) { %s = (xmm_t){0}; } else" % (bits, _x(d)),
            "  for (_i = 0; _i < %d; _i++) %s.u%d[_i] %s= _n; }"
            % (lanes, _x(d), bits, op)]


def legacy_pshufd(d, imm, src_reg=None, src_addr=None):
    return ["{ xmm_t _s = %s, _r; unsigned _c = %uU; int _i;"
            % (_src(src_reg, src_addr), imm),
            "  for (_i = 0; _i < 4; _i++) "
            "_r.u32[_i] = _s.u32[(_c >> (_i*2)) & 3];",
            "  %s = _r; }" % _x(d)]


def legacy_shufps(d, imm, src_reg=None, src_addr=None):
    return ["{ xmm_t _d = %s, _s = %s, _r; unsigned _c = %uU;"
            % (_x(d), _src(src_reg, src_addr), imm),
            "  _r.u32[0] = _d.u32[_c & 3]; _r.u32[1] = _d.u32[(_c>>2)&3];",
            "  _r.u32[2] = _s.u32[(_c>>4)&3]; _r.u32[3] = _s.u32[(_c>>6)&3];",
            "  %s = _r; }" % _x(d)]


_UNPCK_LANE = {
    1: "    _r.u8[_i*2] = _d.u8[_k]; _r.u8[_i*2+1] = _s.u8[_k];",
    2: "    _r.u16[_i*2] = _d.u16[_k]; _r.u16[_i*2+1] = _s.u16[_k];",
    4: "    _r.u32[_i*2] = _d.u32[_k]; _r.u32[_i*2+1] = _s.u32[_k];",
    8: "    _r.u64[_i*2] = _d.u64[_k]; _r.u64[_i*2+1] = _s.u64[_k];",
}


def legacy_unpck(unit, hi, d, src_reg=None, src_addr=None):
    half = (16 // unit) // 2
    return ["{ xmm_t _d = %s, _s = %s, _r; int _i;"
            % (_x(d), _src(src_reg, src_addr)),
            "  for (_i = 0; _i < %d; _i++) {" % half,
            "    int _k = _i + %d;" % (half if hi else 0),
            _UNPCK_LANE[unit],
            "  } %s = _r; }" % _x(d)]


def legacy_pshufw(hi, d, imm, src_reg=None, src_addr=None):
    lane = "4 + _i" if hi else "_i"
    selected = ("4 + ((_c >> (_i * 2)) & 3U)" if hi
                else "((_c >> (_i * 2)) & 3U)")
    return ["{ xmm_t _s = %s, _r = _s; unsigned _c = %uU; int _i;"
            % (_src(src_reg, src_addr), imm),
            "  for (_i = 0; _i < 4; _i++) "
            "_r.u16[%s] = _s.u16[%s];" % (lane, selected),
            "  %s = _r; }" % _x(d)]


def legacy_punpckldq(d, src_reg=None, src_addr=None):
    return ["{ xmm_t _d = %s, _s = %s, _r;" % (_x(d), _src(src_reg, src_addr)),
            "  _r.u32[0] = _d.u32[0]; _r.u32[1] = _s.u32[0];",
            "  _r.u32[2] = _d.u32[1]; _r.u32[3] = _s.u32[1];",
            "  %s = _r; }" % _x(d)]


def legacy_comis(fld, d, rhs):
    return ["{ double _x = (double)%s.%s[0], _y = (double)(%s);"
            % (_x(d), fld, rhs),
            "  GUEST_FL->f_op = FLAG_PARTIAL; GUEST_FL->f_sz = 4;",
            "  GUEST_FL->f_sf = 0; GUEST_FL->f_of = 0;",
            "  if (_x != _x || _y != _y) {",
            "    GUEST_FL->f_cf = 1; GUEST_FL->f_zf = 1; GUEST_FL->f_pf = 1;",
            "  } else {",
            "    GUEST_FL->f_cf = (uint8_t)(_x < _y);",
            "    GUEST_FL->f_zf = (uint8_t)(_x == _y); GUEST_FL->f_pf = 0;",
            "  } }"]


def legacy_fcc(cc):
    return "cc_%s(GUEST_FL)" % cc


_LEGACY = {
    "GUEST_XMM_MOV": (legacy_mov, ("int", "int")),
    "GUEST_XMM_MOV0": (legacy_mov, ("int", "int")),
    "GUEST_XMM_LDX": (legacy_ldx, ("int", "text")),
    "GUEST_XMM_STX": (legacy_stx, ("text", "int")),
    "GUEST_XMM_LANEOP_R": (
        lambda fld, n, op, d, s: legacy_laneop(fld, n, op, d, src_reg=s),
        ("text", "int", "text", "int", "int")),
    "GUEST_XMM_LANEOP_M": (
        lambda fld, n, op, d, a: legacy_laneop(fld, n, op, d, src_addr=a),
        ("text", "int", "text", "int", "text")),
    "GUEST_XMM_LANEOP0_R": (
        lambda fld, n, op, d, s: legacy_laneop(fld, n, op, d, src_reg=s),
        ("text", "int", "text", "int", "int")),
    "GUEST_XMM_LANEOP0_M": (
        lambda fld, n, op, d, a: legacy_laneop(fld, n, op, d, src_addr=a),
        ("text", "int", "text", "int", "text")),
    "GUEST_XMM_PANDN_R": (lambda d, s: legacy_pandn(d, src_reg=s),
                          ("int", "int")),
    "GUEST_XMM_PANDN_M": (lambda d, a: legacy_pandn(d, src_addr=a),
                          ("int", "text")),
    "GUEST_XMM_CVTDQ2PS_R": (lambda d, s: legacy_cvtdq2ps(d, src_reg=s),
                             ("int", "int")),
    "GUEST_XMM_CVTDQ2PS_M": (lambda d, a: legacy_cvtdq2ps(d, src_addr=a),
                             ("int", "text")),
    "GUEST_XMM_CVTDQ2PS0_R": (lambda d, s: legacy_cvtdq2ps(d, src_reg=s),
                              ("int", "int")),
    "GUEST_XMM_CVTDQ2PS0_M": (lambda d, a: legacy_cvtdq2ps(d, src_addr=a),
                              ("int", "text")),
    "GUEST_XMM_PSRLDQ": (legacy_psrldq, ("int", "int")),
    "GUEST_XMM_PSRA_IMM": (legacy_psra_imm, ("int", "int", "int")),
    "GUEST_XMM_PSHL_IMM": (legacy_pshl_imm, ("int", "text", "int", "int")),
    "GUEST_XMM_PSRA_REG": (legacy_psra_reg, ("int", "int", "int")),
    "GUEST_XMM_PSHL_REG": (legacy_pshl_reg, ("int", "text", "int", "int")),
    "GUEST_XMM_PSHUFD_R": (lambda d, s, imm: legacy_pshufd(d, imm, src_reg=s),
                           ("int", "int", "int")),
    "GUEST_XMM_PSHUFD_M": (lambda d, a, imm: legacy_pshufd(d, imm, src_addr=a),
                           ("int", "text", "int")),
    "GUEST_XMM_SHUFPS_R": (lambda d, s, imm: legacy_shufps(d, imm, src_reg=s),
                           ("int", "int", "int")),
    "GUEST_XMM_SHUFPS_M": (lambda d, a, imm: legacy_shufps(d, imm, src_addr=a),
                           ("int", "text", "int")),
    "GUEST_XMM_UNPCK_R": (
        lambda unit, hi, d, s: legacy_unpck(unit, hi, d, src_reg=s),
        ("int", "int", "int", "int")),
    "GUEST_XMM_UNPCK_M": (
        lambda unit, hi, d, a: legacy_unpck(unit, hi, d, src_addr=a),
        ("int", "int", "int", "text")),
    "GUEST_XMM_PSHUFW_R": (
        lambda hi, d, s, imm: legacy_pshufw(hi, d, imm, src_reg=s),
        ("int", "int", "int", "int")),
    "GUEST_XMM_PSHUFW_M": (
        lambda hi, d, a, imm: legacy_pshufw(hi, d, imm, src_addr=a),
        ("int", "int", "text", "int")),
    "GUEST_XMM_PUNPCKLDQ_R": (lambda d, s: legacy_punpckldq(d, src_reg=s),
                              ("int", "int")),
    "GUEST_XMM_PUNPCKLDQ_M": (lambda d, a: legacy_punpckldq(d, src_addr=a),
                              ("int", "text")),
    "GUEST_XMM_COMIS_PAIRED": (legacy_comis, ("text", "int", "text")),
}

_STATEMENT_TOKEN_RE = re.compile(r"\bGUEST_XMM_(?!FCC\()[A-Z0-9_]+\(")
_FCC_RE = re.compile(r"\bGUEST_XMM_FCC\(")


def _split_arguments(text: str, start: int):
    """`text[start:]` follows `NAME(`: the top-level comma-separated arguments
    and the index just past the closing parenthesis."""
    depth = 0
    cursor = start
    arguments = []
    for index in range(start, len(text)):
        char = text[index]
        if char == "(":
            depth += 1
        elif char == ")":
            if depth == 0:
                arguments.append(text[cursor:index].strip())
                return arguments, index + 1
            depth -= 1
        elif char == "," and depth == 0:
            arguments.append(text[cursor:index].strip())
            cursor = index + 1
    raise ValueError("unterminated GUEST_XMM token: %r" % text[start - 1:])


def _legacy_fcc_expressions(text: str) -> str:
    pieces = []
    position = 0
    for match in _FCC_RE.finditer(text):
        if match.start() < position:
            continue
        arguments, end = _split_arguments(text, match.end())
        if len(arguments) != 3 or arguments[0] not in FLOAT_DIRECT:
            raise ValueError("malformed GUEST_XMM_FCC: %r"
                             % text[match.start():end])
        pieces.append(text[position:match.start()])
        pieces.append(legacy_fcc(arguments[0]))
        position = end
    pieces.append(text[position:])
    return "".join(pieces)


def legacy_sse_tokens(text: str) -> str:
    """Rendered text or a single statement -> the pre-token spelling.

    A statement token `NAME(args);` becomes the legacy statement(s); the
    continuation lines of a multi-line legacy statement are indented like the
    renderer indents them (the line's own indentation), so a whole rendered
    body maps to exactly the text gen_all rendered before the tokens.  The
    FCC expression token becomes the cc_* consumer.  Text without tokens is
    returned unchanged."""
    if "GUEST_XMM_" not in text:
        return text
    pieces = []
    position = 0
    for match in _STATEMENT_TOKEN_RE.finditer(text):
        if match.start() < position:
            continue
        name = match.group(0)[:-1]
        spec = _LEGACY.get(name)
        if spec is None:
            raise ValueError("unknown SSE lowering token %s" % name)
        render, kinds = spec
        arguments, end = _split_arguments(text, match.end())
        if len(arguments) != len(kinds):
            raise ValueError("%s takes %d arguments, got %r"
                             % (name, len(kinds), arguments))
        if end >= len(text) or text[end] != ";":
            raise ValueError("SSE lowering token must end a statement: %r"
                             % text[match.start():end + 1])
        values = [int(a, 0) if kind == "int" else a
                  for a, kind in zip(arguments, kinds)]
        statements = render(*values)
        line_start = text.rfind("\n", 0, match.start()) + 1
        indent = text[line_start:match.start()]
        if indent.strip():
            raise ValueError("SSE lowering token must start its line: %r"
                             % text[line_start:end + 1])
        pieces.append(text[position:match.start()])
        pieces.append(("\n" + indent).join(statements))
        position = end + 1
    pieces.append(text[position:])
    return _legacy_fcc_expressions("".join(pieces))
