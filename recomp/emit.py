"""Instruction emitter: one x86-32 instruction -> C statements.

Operands are read out of capstone's operand structures. The printed operand
string is never parsed: doing that is what produced a badly wrong number in
the indirect-target probe, and in a code generator the same mistake yields
code that compiles and reads the wrong memory.

Everything not in the tables below raises Unsupported. The translator does not
guess at a width, a sign or an addressing mode -- a function containing one
unknown instruction is reported as untranslated rather than emitted
half-right.
"""
from capstone import x86 as cx

import leaf_inline
from trans import (CC, DIRECT, R8H, R8L, R16, R32, ST, XMMR, Unsupported,
                   _reg_root,
                   norm_cc)
import sse_lower

CTY = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}
STY = {1: "int8_t", 2: "int16_t", 4: "int32_t", 8: "int64_t"}

# DIRECT conditions whose C expression compares as int32_t.
SIGNED_DIRECT = frozenset(("l", "ge", "le", "g"))

# --- xmm upper-lane taint analysis -------------------------------------------
# A `movss`/`movsd` load from memory architecturally zeroes the lanes above
# the scalar one.  Emitting that zero fill costs three stores per load on the
# Cortex-A9 (61,073 sites in the corpus); the zeros are observable only where
# an upper lane can reach lane 0, memory, a general register or the flags.
# `xmm_zero_fill_elidable` decides per load, by following the value: a load
# taints its register's upper lanes; lane-wise packed operations, register
# copies and the MSVC scalar-conversion idioms (cvtdq2pd / cvtps2pd /
# cvtpd2ps) carry the taint into their destination without observing it;
# shuffles, whole-register stores, 64-bit reads of a single-width load and
# every unlisted instruction observe it and pin the tainted loads.  A call
# ends every taint: MSVC x86 treats all xmm registers as volatile across
# calls, so nothing outside the function can observe an upper lane.  The
# analysis is exact inside a basic block and a monotone fixpoint over the
# block graph; anything it does not know about is an observer.

# Read only lane 0 (32 bits) of every xmm source; write lane 0 of the
# destination and preserve its upper lanes.
_XMM_SCALAR_SS = frozenset((
    "addss", "subss", "mulss", "divss", "sqrtss", "minss", "maxss", "comiss",
    "ucomiss", "cvtsi2ss", "cvttss2si", "cvtss2si", "cvtss2sd", "vcvttss2usi",
    "rcpss", "rsqrtss", "roundss",
))
# Read the low 64 bits (d[0]) of every xmm source -- lane f[1] of a register
# loaded single-width -- and write d[0] of the destination, preserving d[1].
_XMM_SCALAR_SD = frozenset((
    "addsd", "subsd", "mulsd", "divsd", "sqrtsd", "minsd", "maxsd", "comisd",
    "ucomisd", "cvtsi2sd", "cvttsd2si", "cvtsd2si", "cvtsd2ss", "vcvttsd2usi",
    "roundsd",
))
# Lane-wise at 32-bit or narrower lanes, read-modify-write: lane 0 of the
# result depends only on lane 0 of the inputs.  Propagators.
_XMM_LANEWISE = frozenset((
    "addps", "subps", "mulps", "divps", "maxps", "minps", "sqrtps", "rcpps",
    "rsqrtps", "andps", "andnps", "orps", "xorps", "pand", "pandn", "por",
    "pxor", "paddb", "paddw", "paddd", "psubb", "psubw", "psubd", "pmulld",
    "pmullw", "pmulhw", "pcmpgtb", "pcmpgtw", "pcmpgtd", "pcmpeqb", "pcmpeqw",
    "pcmpeqd", "pabsb", "pabsw", "pabsd", "cmpps", "cvtdq2ps", "cvtps2dq",
    "cvttps2dq", "pminsb", "pminsw", "pminsd", "pmaxsb", "pmaxsw", "pmaxsd",
    "pminub", "pminuw", "pminud", "pmaxub", "pmaxuw", "pmaxud", "pavgb",
    "pavgw",
))
_XMM_LANEWISE_IMM_SHIFTS = frozenset((
    "psllw", "pslld", "psrlw", "psrld", "psraw", "psrad",
))
# Whole-register copies / loads (destination fully written).
_XMM_FULL_MOVES = frozenset((
    "movaps", "movups", "movapd", "movupd", "movdqa", "movdqu",
))
_XMM_LOADS = frozenset(("movss", "movsd"))


def _xmm_regs(ops):
    return [XMMR[o.reg] if (o.type == cx.X86_OP_REG and o.reg in XMMR)
            else None for o in ops]


def xmm_zero_fill_elidable(insns, block_of, members, succs):
    """RVAs of the `movss`/`movsd` register loads whose upper-lane zero fill
    no instruction of the function can observe (see the section comment)."""
    return xmm_upper_lane_taint(insns, block_of, members, succs)


def xmm_lane0_candidates(insns):
    """RVAs of the packed operations and register copies the SSE lowering may
    spell lane-0-only if their result's upper lanes are unobserved: 32-bit
    bitwise/integer lane ops and cvtdq2ps with an xmm destination, and
    xmm-to-xmm whole-register copies.  The `xorps x, x` zeroing idiom is not
    a candidate (it is emitted as an exact zero)."""
    out = set()
    for rva, ins in insns.items():
        m = ins.mnemonic
        ops = ins.operands
        if (m not in sse_lower.LANE0_MNEMONICS or len(ops) != 2 or
                ops[0].type != cx.X86_OP_REG or ops[0].reg not in XMMR):
            continue
        if m in sse_lower.LANE0_MOVES:
            if m not in _XMM_FULL_MOVES:
                continue
            if ops[1].type != cx.X86_OP_REG or ops[1].reg not in XMMR:
                continue
        else:
            # Only the propagators the taint analysis models lane-wise can
            # carry the site's taint; anything else (xorpd/andpd/orpd fall
            # through to "observes every lane, result exact") keeps the
            # full-width spelling.
            if m not in _XMM_LANEWISE:
                continue
            if (ops[1].type == cx.X86_OP_REG and ops[1].reg == ops[0].reg and
                    m in ("xorps", "xorpd", "pxor")):
                continue
        out.add(rva)
    return frozenset(out)


def xmm_cvt_zero_upper_sites(insns, members):
    """`cvtdq2ps x, x` sites whose source's lanes 1..3 are architecturally
    zero: within the block, x was last written by `movd x, r/m32` (zero
    extends) or the `xorps/pxor x, x` idiom and nothing wrote x since.  The
    lane-0 conversion is then bit-exact in every lane ((float)0 is +0.0f, all
    bits clear), so the site needs no taint at all."""
    out = set()
    for rvas in members.values():
        zero_upper = set()
        for rva in rvas:
            ins = insns.get(rva)
            if ins is None:
                zero_upper.clear()
                continue
            m = ins.mnemonic
            ops = ins.operands
            regs = _xmm_regs(ops)
            if m in ("call", "int3", "syscall", "sysenter"):
                zero_upper.clear()
                continue
            if m == "cvtdq2ps" and len(ops) == 2 and regs[0] is not None:
                if (regs[1] is not None and regs[1] == regs[0] and
                        regs[0] in zero_upper):
                    out.add(rva)                       # exact, stays zero-upper
                    continue
                zero_upper.discard(regs[0])
                continue
            if (m in ("movd", "vmovd") and regs[0] is not None and
                    len(ops) == 2 and opsize(ins, ops[1]) == 4):
                zero_upper.add(regs[0])
                continue
            if (m in ("xorps", "xorpd", "pxor") and len(ops) == 2 and
                    regs[0] is not None and regs[1] == regs[0]):
                zero_upper.add(regs[0])
                continue
            # Any other instruction naming an xmm register as its first
            # operand may write it: forget it.
            if regs and regs[0] is not None:
                zero_upper.discard(regs[0])
    return frozenset(out)


def xmm_lane0_sites(insns, block_of, members, succs):
    """The lane-0 candidates whose upper lanes nothing observes (the packed
    op's site is treated as a taint source: its result's lanes 1..3 hold
    stale bits under the lane-0 spelling, exactly like a movss load's)."""
    candidates = xmm_lane0_candidates(insns)
    proven = xmm_upper_lane_taint(insns, block_of, members, succs,
                                  candidates) & candidates
    return proven | xmm_cvt_zero_upper_sites(insns, members)


def xmm_upper_lane_taint(insns, block_of, members, succs, sources=frozenset()):
    """RVAs among the `movss`/`movsd` register loads and `sources` (packed-op
    or register-copy sites, see xmm_lane0_candidates) whose upper lanes no
    instruction of the function can observe.

    State per register: {source rva: level}, level 1 = lane f[1] may hold
    garbage from that source (a `movss`, a lane-0 packed op), level 2 = only
    lanes f[2..3] / d[1] may (a `movsd`, or a value whose low 64 bits were
    rewritten exactly).  A 64-bit read (d[0]) observes level 1; a
    whole-register read observes both.  Entries are independent: adding a
    source never changes another source's verdict (a movss/movsd load's
    verdict is the same with or without `sources`; a lane-0 site's entry
    outlives a scalar load into its register, see the _XMM_LOADS step).
    """
    loads = {}                       # rva -> reg
    for rva, ins in insns.items():
        if (ins.mnemonic in _XMM_LOADS and len(ins.operands) == 2 and
                ins.operands[0].type == cx.X86_OP_REG and
                ins.operands[0].reg in XMMR and
                ins.operands[1].type == cx.X86_OP_MEM):
            loads[rva] = XMMR[ins.operands[0].reg]
    sources = frozenset(sources)
    if not (loads or sources) or not members:
        return frozenset()

    pinned = set()
    L1, L2 = 1, 2

    def merge(*taints):
        out = {}
        for t in taints:
            for rva, level in t.items():
                out[rva] = min(level, out.get(rva, L2))
        return out

    def lowered(t):
        """After the low 64 bits were rewritten exactly: lane-1 garbage is
        gone, lanes 2..3 keep whatever they had."""
        return {rva: L2 for rva in t}

    def carried_to_upper(*taints):
        """The inputs' lane 1 lands in d[1] of the result; their lanes 2..3
        are not copied: only level-1 entries survive, as level 2."""
        return {rva: L2 for t in taints for rva, level in t.items()
                if level == L1}

    def observe_all(t):
        pinned.update(t)

    def observe_d0(t):
        pinned.update(rva for rva, level in t.items() if level == L1)

    recorded = set()

    def observe_sources(state):
        """Every live lane-0 site: another function may read the register
        whole (register arguments and preserved registers under MSVC's
        link-time custom conventions), so a lane-0 result must not be live
        across a call, a return or a tail transfer.  The movss/movsd loads
        keep their original rule (the switch-off corpus is unchanged)."""
        for t in state.values():
            pinned.update(rva for rva in t if rva in sources)

    def step(state, ins):
        m = ins.mnemonic
        ops = ins.operands
        regs = _xmm_regs(ops)
        if m in ("call", "int3", "syscall", "sysenter"):
            observe_sources(state)
            state.clear()
            return
        if m in ("ret", "retn", "retf", "iret", "iretd") or (
                m == "jmp" and not (ops and ops[0].type == cx.X86_OP_IMM and
                                    ops[0].imm in insns)):
            # Leaving the function: xmm0 may carry a scalar return value to
            # a caller.  Only lane 0 is defined by the ABI, but pin whatever
            # taints it so the caller sees exactly the architectural zeros.
            observe_all(state.get(0, {}))
            observe_sources(state)
            return
        if not any(r is not None for r in regs):
            return
        dst = regs[0]
        dst_is_reg = dst is not None
        srcs = [r for r in regs[1:] if r is not None]
        taint_of = lambda r: state.get(r, {})                # noqa: E731
        src_taint = merge(*(taint_of(r) for r in srcs))

        if m in _XMM_LOADS:
            if dst_is_reg and ops[1].type == cx.X86_OP_MEM:
                # The load is its own source (its zero fill may be elided).
                # The lane-0 sites still tainting the destination stay: a
                # lane-0 result's stale lanes 1..3 physically survive a
                # scalar load whose fill is elided, so the sites must remain
                # observable (and pinned at a call/ret/tail) past it.  When
                # the load's fill is kept they are zeroed there; the walk does
                # not know the load's verdict, so it keeps them regardless
                # (conservative).  Without sources this is the old rule.
                kept = {s: lvl for s, lvl in taint_of(dst).items()
                        if s in sources}
                if m == "movss":
                    state[dst] = merge({ins.address: L1}, kept)
                else:
                    state[dst] = merge({ins.address: L2}, lowered(kept))
            elif dst_is_reg:
                # reg <- reg: movss copies lane 0 (upper kept); movsd copies
                # d[0] (reads the source's f[1], rewrites the destination's).
                if m == "movsd":
                    observe_d0(src_taint)
                    state[dst] = lowered(taint_of(dst))
            else:
                if m == "movsd":
                    observe_d0(src_taint)           # 64-bit store
            return
        if m in _XMM_SCALAR_SS:
            if m == "cvtss2sd" and dst_is_reg:
                state[dst] = lowered(taint_of(dst))  # writes d[0] exactly
            return                                   # lane 0 only, upper kept
        if m in _XMM_SCALAR_SD:
            observe_d0(src_taint)
            if dst_is_reg:
                if m not in ("cvtsi2sd", "cvtsd2ss"):
                    observe_d0(taint_of(dst))        # RMW reads its own d[0]
                if m != "cvtsd2ss":                  # cvtsd2ss writes f[0]
                    state[dst] = lowered(taint_of(dst))
            return
        if m in ("movd", "vmovd"):
            if dst_is_reg:
                state[dst] = {}                      # zeroes the upper lanes
            return                                   # r32 <- xmm reads lane 0
        if m == "movq":
            if dst_is_reg and ops[1].type == cx.X86_OP_MEM:
                state[dst] = {}                      # 64-bit load, upper zero
            elif dst_is_reg:
                observe_d0(src_taint)                # copies d[0], zero upper
                state[dst] = {}
            else:
                observe_d0(src_taint)                # 64-bit store
            return
        if m in ("movlpd", "movlps"):
            if dst_is_reg:
                state[dst] = lowered(taint_of(dst))  # writes d[0], keeps d[1]
            else:
                observe_d0(src_taint)                # stores d[0]
            return
        if m in _XMM_FULL_MOVES:
            if dst_is_reg and ops[1].type == cx.X86_OP_MEM:
                state[dst] = {}                      # exact 16-byte load
            elif dst_is_reg:
                state[dst] = dict(src_taint)         # register copy
                if ins.address in sources:
                    state[dst][ins.address] = L1     # lane-0 copy: 1..3 stale
                    recorded.add(ins.address)
            else:
                observe_all(src_taint)               # 16-byte store
            return
        if (m in ("xorps", "xorpd", "pxor") and len(ops) == 2 and
                ops[1].type == cx.X86_OP_REG and ops[1].reg == ops[0].reg):
            state[dst] = {}                          # zeroing idiom
            return
        lanewise = (m in _XMM_LANEWISE or
                    (m in _XMM_LANEWISE_IMM_SHIFTS and len(ops) == 2 and
                     ops[1].type == cx.X86_OP_IMM))
        if lanewise and dst_is_reg:
            state[dst] = merge(taint_of(dst), src_taint)
            if ins.address in sources:
                state[dst][ins.address] = L1         # lane-0 op: 1..3 stale
                recorded.add(ins.address)
            return
        if m in ("cvtdq2pd", "cvtps2pd"):
            # d[0] exact from lane 0; d[1] from lane 1 of the source.
            if dst_is_reg:
                state[dst] = carried_to_upper(src_taint)
            return
        if m == "cvtpd2ps":
            # f[0] from src d[0] (64-bit read), f[1] from src d[1], f[2..3]
            # zero: the destination's lane 1 is dirty if src d[1] was.
            observe_d0(src_taint)
            if dst_is_reg:
                state[dst] = {rva: L1 for rva in src_taint}
            return
        if m == "unpcklps" and dst_is_reg:
            # [d0, s0, d1, s1]: lane 1 exact, lanes 2..3 carry both lane 1s.
            state[dst] = carried_to_upper(taint_of(dst), src_taint)
            return
        # Everything else observes every lane of every xmm operand it names
        # (shuffles, unpckh*, 64-bit-lane ops, horizontal ops, other stores,
        # unknown instructions).  Their tainted inputs are now pinned to exact
        # zero fills, so the result is exact.
        for r in regs:
            if r is not None:
                observe_all(taint_of(r))
        if dst_is_reg:
            state[dst] = {}
        if ins.address in sources:
            pinned.add(ins.address)                  # not modelled: full width

    # Block-level fixpoint over entry states (union of predecessors' exits).
    blocks = list(members)
    preds = {b: [] for b in blocks}
    for b, ss in succs.items():
        for t in ss:
            if t in preds:
                preds[t].append(b)
    entry = {b: {} for b in blocks}
    exit_ = {b: {} for b in blocks}

    def run_block(b, state):
        for rva in members[b]:
            ins = insns.get(rva)
            if ins is not None:
                step(state, ins)
        return {r: t for r, t in state.items() if t}

    def copy_state(state):
        return {r: dict(t) for r, t in state.items()}

    changed = True
    while changed:
        changed = False
        for b in blocks:
            merged = {}
            for p_ in preds[b]:
                for r, t in exit_[p_].items():
                    merged[r] = merge(merged.get(r, {}), t)
            if merged != entry[b]:
                entry[b] = merged
                changed = True
            out = run_block(b, copy_state(merged))
            if out != exit_[b]:
                exit_[b] = out
                changed = True
    pinned.clear()
    recorded.clear()
    for b in blocks:
        run_block(b, copy_state(entry[b]))
    # A source the walk never recorded as a taint (its instruction took a
    # branch that does not model it, or it was never reached) fails closed.
    return frozenset(rva for rva in loads.keys() | sources
                     if rva not in pinned and (rva in loads or rva in recorded))


LD = {1: "ld8", 2: "ld16", 4: "ld32", 8: "ld64"}
STO = {1: "st8", 2: "st16", 4: "st32", 8: "st64"}

# Deterministic flat-ring-3 selector values for the virtual Win32 CPU.  Guest
# addressing already models CS/DS/ES/SS as base zero and FS through guest_fs;
# these numeric selectors are observable only in exception-context records.
SEGMENT_VALUE = {
    cx.X86_REG_CS: "0x1bU",
    cx.X86_REG_SS: "0x23U",
    cx.X86_REG_DS: "0x23U",
    cx.X86_REG_ES: "0x23U",
    cx.X86_REG_FS: "0x3bU",
    cx.X86_REG_GS: "0U",
}


def u32(v):
    return "0x%xU" % (v & 0xFFFFFFFF)


def raw_addr_expr(ins, op):
    """Memory operand -> its unchecked uint32_t effective-address expression.

    Only LEA may consume this directly.  Every actual memory operation goes
    through addr_expr(), which adds the synthetic-stack range check when ESP
    participates in the decoded address.  Keeping the distinction here makes
    it impossible for a new memory emitter to accidentally copy LEA's special
    no-dereference behaviour.
    """
    m = op.mem
    terms = []
    if m.segment not in (0, cx.X86_REG_INVALID):
        # Win32 is a flat model: CS, DS, ES and SS all have base 0, so an
        # explicit prefix naming one of them changes nothing. capstone reports
        # them anyway (`mov dword ptr ds:[esp + 4], ...`), and refusing them
        # blocked 49 functions over a prefix with no effect.
        # FS carries the TEB and is real. GS is unused by 32-bit Windows, so
        # if one ever turns up it is a decode error and must be loud.
        if m.segment == cx.X86_REG_FS:
            terms.append("guest_fs_base(c)")
        elif m.segment == cx.X86_REG_GS:
            raise Unsupported(ins, "GS override in a 32-bit Windows binary")
        elif m.segment not in (cx.X86_REG_CS, cx.X86_REG_DS,
                               cx.X86_REG_ES, cx.X86_REG_SS):
            raise Unsupported(ins, "segment override %d" % m.segment)
    if m.base not in (0, cx.X86_REG_INVALID):
        if m.base not in R32:
            raise Unsupported(ins, "non-32-bit base register")
        terms.append("GR(%s)" % R32[m.base])
    if m.index not in (0, cx.X86_REG_INVALID):
        if m.index not in R32:
            raise Unsupported(ins, "non-32-bit index register")
        if m.scale == 1:
            terms.append("GR(%s)" % R32[m.index])
        else:
            terms.append("GR(%s) * %d" % (R32[m.index], m.scale))
    if m.disp or not terms:
        # Displacements are signed. Formatting a negative one as hex produced
        # the literal `0x-4U` on ladder rung 2; `[ebp - 4]` is the commonest
        # operand in the binary, so this path is not an edge case.
        d = m.disp
        if d < 0:
            terms.append("(uint32_t)(int32_t)(%d)" % d)
        else:
            terms.append(u32(d))
    return "(uint32_t)(%s)" % " + ".join(terms)


def addr_expr(ins, op, size=None):
    """Memory operand -> a checked uint32_t effective address."""
    address = raw_addr_expr(ins, op)
    m = op.mem
    if m.base == cx.X86_REG_ESP or m.index == cx.X86_REG_ESP:
        n = size or op.size
        if not n:
            raise Unsupported(ins, "ESP-derived memory access has no width")
        return "GSTACK_ADDR(%s, %dU)" % (address, n)
    return address


# General registers are spelled through the GR(reg) token and the synthetic
# stack through GPUSH/GPOP/GESP_SET/GESP_ADJ/GSTACK_ADDR (guest.h).  In the
# default build they expand to exactly the `c->reg` field and `_generated`
# helper call the emitter always produced; a unit compiled with
# GUEST_GPR_LOCAL=1 keeps the eight registers in locals instead, and every
# statement below that lets code outside the body see the registers (calls,
# guest_call, faults, int3, cpuid, return) publishes them with
# GUEST_GPR_FLUSH(c) first and, where control returns, takes them back with
# GUEST_GPR_RELOAD(c).  gpr_locals.py holds the exact spelling map.


def rd(ins, op, size=None):
    """Operand -> C expression producing its value (unsigned, natural width)."""
    if op.type == cx.X86_OP_REG:
        r = op.reg
        if r in R32:
            return "GR(%s)" % R32[r]
        if r in R16:
            return "(uint16_t)GR(%s)" % R16[r]
        if r in R8L:
            return "(uint8_t)GR(%s)" % R8L[r]
        if r in R8H:
            return "(uint8_t)(GR(%s) >> 8)" % R8H[r]
        if r in XMMR:
            return "c->x[%d]" % XMMR[r]
        if r in SEGMENT_VALUE:
            return SEGMENT_VALUE[r]
        raise Unsupported(ins, "read of register %d" % r)
    if op.type == cx.X86_OP_IMM:
        return u32(op.imm)
    if op.type == cx.X86_OP_MEM:
        n = size or op.size
        if n not in LD:
            raise Unsupported(ins, "memory read width %d" % n)
        return "%s(%s)" % (LD[n], addr_expr(ins, op, n))
    raise Unsupported(ins, "operand type %d" % op.type)


def wr(ins, op, value, size=None):
    """Operand + value expression -> a C statement writing it."""
    if op.type == cx.X86_OP_REG:
        r = op.reg
        if r in R32:
            if r == cx.X86_REG_ESP:
                return "if (!GESP_SET((uint32_t)(%s))) return;" % value
            return "GR(%s) = (uint32_t)(%s);" % (R32[r], value)
        if r in R16:
            n = R16[r]
            if n == "esp":
                return ("if (!GESP_SET((GR(esp) & 0xFFFF0000U) | "
                        "((uint32_t)(%s) & 0xFFFFU))) return;" % value)
            return ("GR(%s) = (GR(%s) & 0xFFFF0000U) | ((uint32_t)(%s) & 0xFFFFU);"
                    % (n, n, value))
        if r in R8L:
            n = R8L[r]
            return ("GR(%s) = (GR(%s) & 0xFFFFFF00U) | ((uint32_t)(%s) & 0xFFU);"
                    % (n, n, value))
        if r in R8H:
            n = R8H[r]
            return ("GR(%s) = (GR(%s) & 0xFFFF00FFU) | (((uint32_t)(%s) & 0xFFU) << 8);"
                    % (n, n, value))
        if r in XMMR:
            return "c->x[%d] = (%s);" % (XMMR[r], value)
        raise Unsupported(ins, "write to register %d" % r)
    if op.type == cx.X86_OP_MEM:
        n = size or op.size
        if n not in STO:
            raise Unsupported(ins, "memory write width %d" % n)
        return "%s(%s, (%s)(%s));" % (
            STO[n], addr_expr(ins, op, n), CTY[n], value)
    raise Unsupported(ins, "write to operand type %d" % op.type)


def opsize(ins, op):
    if op.type == cx.X86_OP_REG:
        r = op.reg
        if r in R32:
            return 4
        if r in R16:
            return 2
        if r in R8L or r in R8H:
            return 1
        if r in XMMR:
            return 16
    return op.size


def xmm_of(ins, op):
    if op.type != cx.X86_OP_REG or op.reg not in XMMR:
        raise Unsupported(ins, "expected an xmm register")
    return "c->x[%d]" % XMMR[op.reg]


# ------------------------------------------------------------ arithmetic ---

BINOP = {
    "add": ("+", "FLAG_ADD"), "sub": ("-", "FLAG_SUB"),
    "and": ("&", "FLAG_LOGIC"), "or": ("|", "FLAG_LOGIC"),
    "xor": ("^", "FLAG_LOGIC"),
}


def iat_slot_operand(ctx, import_slots, operand):
    """(slot_rva, import_id) when `operand` is `dword ptr [abs]` and `abs` is
    the relocated VA of a PE import slot named in `import_slots`; else None.

    Segment, base and index must be absent: `[fs:abs]` or `[reg+abs]` is not
    the slot even when the displacement matches.  The displacement is already
    the relocated VA (Image applies the PE relocations), so the slot RVA is
    its distance from the image base."""
    if not import_slots or operand.type != cx.X86_OP_MEM or operand.size != 4:
        return None
    mem = operand.mem
    if (mem.segment not in (0, cx.X86_REG_INVALID) or
            mem.base not in (0, cx.X86_REG_INVALID) or
            mem.index not in (0, cx.X86_REG_INVALID)):
        return None
    rva = ((mem.disp & 0xFFFFFFFF) - ctx.base) & 0xFFFFFFFF
    import_id = import_slots.get(rva)
    return None if import_id is None else (rva, import_id)


class Emitter:
    """Translates one function. `flags_live` says whether a producer's flag
    state must be recorded; the block-local pass fills `direct` with the
    consumers it could answer without touching the context at all."""

    def __init__(self, ctx, fname, live_out, block_of, insns):
        self.ctx = ctx
        self.fname = fname
        self.live_out = live_out
        self.block_of = block_of
        self.insns = insns
        self.labels = set()
        self.calls = set()
        self.indirect = 0
        # Proved relocation-backed switch targets, keyed by the RVA of the
        # indirect JMP.  gen_all seeds those labels into this same body; the
        # computed target is still read at run time and unmatched values keep
        # the generic dispatch path.
        self.indirect_edges = {}
        self.direct_pairs = 0
        self.flag_stores = 0
        # Set of function RVAs that will actually be emitted. A direct call or
        # tail call to anything else must NOT become a C call: the first full
        # link failed on `_sub_bdd015d5` and `_sub_45d39813`, addresses far
        # outside the image, reached because recursive descent walked into
        # data. The compiler cannot see this; only the linker can, and only if
        # we do not silently invent the name. When None, no checking is done
        # (single-function builds, where the caller supplies its own stubs).
        self.known = None
        self.bad_targets = 0
        # Set when the function's entry block consumes flags it did not
        # produce (only reachable through an external tail transfer).
        self.entry_flags_live = False
        # Load RVAs whose zero fill may be skipped; set by the driver from
        # xmm_zero_fill_elidable() once the block graph is known.
        self.xmm_zero_elide = frozenset()
        self.xmm_zero_elided = 0
        # IAT slot RVA -> dense import ID (the slot-RVA-sorted PE import
        # table), supplied by gen_all through build_one.translate.  Empty
        # (single-function builds, host oracles): every import site keeps
        # the plain guest_call spelling.  With it, `call/jmp dword ptr [slot]`
        # and `call reg` with reg loaded from a slot in the same block are
        # spelled GUEST_IMPORT_CALL/GUEST_IMPORT_JMP (guest.h): the runtime
        # compares the value read from the slot with the validated token of
        # that ID and otherwise runs the very same guest_call.
        self.import_slots = {}
        self.iat_call_sites = 0       # call dword ptr [slot]
        self.iat_jmp_sites = 0        # jmp dword ptr [slot]
        self.iat_reg_sites = 0        # call reg, reg = [slot] in this block
        self._iat_regs = {}           # register name -> (slot_rva, import_id)
        self._iat_block = None
        # SSE lowering (sse_lower.py): when the generation switch is on the
        # packed ops, 16-byte moves and paired comis are spelled through the
        # GUEST_XMM_* tokens; `xmm_lane0` holds the sites whose result upper
        # lanes the driver proved unobserved (xmm_lane0_sites).
        self.sse_lower = sse_lower.enabled()
        self.xmm_lane0 = frozenset()

    # -- SSE lowering helpers ---------------------------------------------
    @staticmethod
    def _xmm_index(ins, op):
        xmm_of(ins, op)
        return XMMR[op.reg]

    @staticmethod
    def _xmm_source(ins, op):
        """Keyword operand for a token's second operand: an xmm index or the
        checked address expression of a 16-byte memory operand."""
        if op.type == cx.X86_OP_REG:
            return {"src_reg": Emitter._xmm_index(ins, op)}
        if op.type == cx.X86_OP_MEM:
            return {"src_addr": addr_expr(ins, op)}
        raise Unsupported(ins, "expected an xmm register or memory operand")

    # -- flag helpers ----------------------------------------------------
    def producer_operands(self, ins):
        """For a non-mutating pairable producer, return its C operands.

        These expressions are evaluated at the later flag consumer.  CMP and
        TEST do not change what they read; SUB does, so pairing SUB with a Jcc
        would compare the already-subtracted destination against the source.
        """
        m = ins.mnemonic
        if m == "cmp":
            a, b = ins.operands
            n = opsize(ins, a)
            # The pair carries its operand width: condition() sign-extends
            # both sides from that width before a signed relation, so a
            # byte/word CMP pairs as exactly as a dword one.  The corpus has
            # 40,797 byte-wide producers (cmp/test byte ptr against 0, al/al)
            # that previously fell back to five flag stores and a switch.
            return (self.cast(rd(ins, a), n), self.cast(rd(ins, b, n), n), n)
        if m == "test":
            a, b = ins.operands
            n = opsize(ins, a)
            return ("((%s) & (%s))" % (self.cast(rd(ins, a), n),
                                       self.cast(rd(ins, b, n), n)), "0U", n)
        if m in sse_lower.COMIS and self.sse_lower:
            # comis reads lane 0 of its operands and nothing else; the pair's
            # third slot names the lane type (sse_lower.FLOAT_PAIR_KINDS) so
            # condition() picks the float relation table.
            fld = "f" if "ss" in m else "d"
            d, s = ins.operands
            rhs = ("%s.%s[0]" % (xmm_of(ins, s), fld) if s.type == cx.X86_OP_REG
                   else "%s(%s)" % ("ldf" if fld == "f" else "ldd",
                                    addr_expr(ins, s)))
            return ("%s.%s[0]" % (xmm_of(ins, d), fld), rhs, fld)
        return None

    @staticmethod
    def cast(expr, n):
        return expr if n == 4 else "(%s)(%s)" % (CTY[n], expr)

    def set_flags(self, op, a, b, r, n):
        self.flag_stores += 1
        return "SET_FLAGS(GUEST_FL, %s, %s, %s, %s, %d);" % (op, a, b, r, n)

    # -- the instruction table -------------------------------------------
    def emit(self, ins, live_after, pair):
        """`pair` is a (a_expr, b_expr) from a paired producer when this
        instruction is a consumer that could be answered directly."""
        block = self.block_of.get(ins.address)
        if block != self._iat_block:
            # A block leader may be reached from anywhere: nothing known
            # about IAT-loaded registers survives it.
            self._iat_block = block
            self._iat_regs = {}
        out = self._emit_insn(ins, live_after, pair)
        self._track_iat_registers(ins)
        return out

    def _track_iat_registers(self, ins):
        """Block-local knowledge for `call reg`: which registers hold the word
        of an IAT slot (`mov reg, dword ptr [slot]`).  Any write to the
        register forgets it; a call forgets the caller-saved registers.  The
        knowledge only selects the guard constants of GUEST_IMPORT_CALL -- a
        wrong guess costs the guarded fallback, never semantics."""
        if not self.import_slots:
            return
        try:
            _read, written = ins.regs_access()
        except Exception:                                    # noqa: BLE001
            self._iat_regs = {}
            return
        for register in written:
            self._iat_regs.pop(_reg_root(register), None)
        m = ins.mnemonic.split()[-1]
        if m == "call":
            for register in ("eax", "ecx", "edx"):
                self._iat_regs.pop(register, None)
            return
        ops = ins.operands
        if (m == "mov" and len(ops) == 2 and ops[0].type == cx.X86_OP_REG and
                ops[0].size == 4 and ops[0].reg in R32):
            iat = iat_slot_operand(self.ctx, self.import_slots, ops[1])
            if iat is not None:
                self._iat_regs[R32[ops[0].reg]] = iat

    def _emit_insn(self, ins, live_after, pair):
        m = ins.mnemonic
        ops = ins.operands
        out = []

        # capstone folds prefixes into the mnemonic string: `rep movsd`,
        # `lock cmpxchg`, `notrack call`. Dispatching on the raw string means
        # a rule written for `cmpxchg` never fires -- which is exactly what
        # happened: `rep movsd` and `lock cmpxchg` kept appearing as blockers
        # in the sweep *after* both were implemented, and two of my own
        # numbers disagreeing is what exposed it. Strip the prefix into a
        # flag and dispatch on the instruction.
        #   lock    -- retained for instructions whose atomicity is observable
        #   notrack -- CET hint, no semantics at all
        #   rep/repe/repne -- real, and consumed by the string-op rule
        rep_pfx = None
        lock_pfx = False
        for p in ("notrack ", "lock ", "rep ", "repe ", "repne ", "repz ",
                  "repnz ", "bnd "):
            if m.startswith(p):
                if p.startswith("rep"):
                    rep_pfx = p.strip()
                elif p == "lock ":
                    lock_pfx = True
                m = m[len(p):]
                break

        # ---- data movement --------------------------------------------
        if m == "mov":
            return [wr(ins, ops[0], rd(ins, ops[1], opsize(ins, ops[0])))]
        if m == "movzx":
            n = opsize(ins, ops[1])
            return [wr(ins, ops[0], "(uint32_t)%s" % rd(ins, ops[1], n))]
        if m == "movsx":
            n = opsize(ins, ops[1])
            return [wr(ins, ops[0],
                       "(uint32_t)(int32_t)(%s)(%s)" % (STY[n], rd(ins, ops[1], n)))]
        if m == "lea":
            return [wr(ins, ops[0], raw_addr_expr(ins, ops[1]))]
        if m == "xchg":
            t = "uint32_t _t = %s;" % rd(ins, ops[0])
            return ["{ %s %s %s }" % (t,
                                      wr(ins, ops[0], rd(ins, ops[1])),
                                      wr(ins, ops[1], "_t"))]
        if m == "push":
            return ["GPUSH(%s); GUEST_STACK_CALLSITE_BARRIER();"
                    % rd(ins, ops[0], 4)]
        if m == "pop":
            return [wr(ins, ops[0], "GPOP()")]
        if m == "leave":
            return ["if (!GESP_SET(GR(ebp))) return;",
                    "GR(ebp) = GPOP();"]
        if m == "cdq":
            return ["GR(edx) = (uint32_t)((int32_t)GR(eax) >> 31);"]
        if m == "cwde":
            return ["GR(eax) = (uint32_t)(int32_t)(int16_t)GR(eax);"]
        if m == "nop":
            return []

        # ---- integer arithmetic ---------------------------------------
        if m in BINOP:
            cop, fop = BINOP[m]
            n = opsize(ins, ops[0])
            a = self.cast(rd(ins, ops[0]), n)
            b = self.cast(rd(ins, ops[1], n), n)
            out.append("{ uint32_t _a = %s, _b = %s, _r = (uint32_t)(_a %s _b);"
                       % (a, b, cop))
            out.append("  " + wr(ins, ops[0], "_r", n))
            if live_after:
                out.append("  " + self.set_flags(fop, "_a", "_b", "_r", n))
            out.append("}")
            return out
        if m == "cmp":
            n = opsize(ins, ops[0])
            if not live_after:
                return []
            return [self.set_flags("FLAG_SUB",
                                   self.cast(rd(ins, ops[0]), n),
                                   self.cast(rd(ins, ops[1], n), n),
                                   "(uint32_t)(%s - %s)"
                                   % (self.cast(rd(ins, ops[0]), n),
                                      self.cast(rd(ins, ops[1], n), n)), n)]
        if m == "test":
            n = opsize(ins, ops[0])
            if not live_after:
                return []
            return [self.set_flags("FLAG_LOGIC", "0U", "0U",
                                   "(uint32_t)(%s & %s)"
                                   % (self.cast(rd(ins, ops[0]), n),
                                      self.cast(rd(ins, ops[1], n), n)), n)]
        if m in ("inc", "dec"):
            n = opsize(ins, ops[0])
            d = "+ 1U" if m == "inc" else "- 1U"
            preserve = ", _cf = (uint32_t)fl_cf(GUEST_FL)" if live_after else ""
            out.append("{ uint32_t _a = %s, _r = (uint32_t)(_a %s)%s;"
                       % (self.cast(rd(ins, ops[0]), n), d, preserve))
            out.append("  " + wr(ins, ops[0], "_r", n))
            if live_after:
                out.append("  " + self.set_flags(
                    "FLAG_INC" if m == "inc" else "FLAG_DEC",
                    "_a", "1U", "_r", n))
                out.append("  GUEST_FL->f_cf = (uint8_t)_cf;")
            out.append("}")
            return out
        if m == "neg":
            n = opsize(ins, ops[0])
            out.append("{ uint32_t _a = %s, _r = (uint32_t)(0U - _a);"
                       % self.cast(rd(ins, ops[0]), n))
            out.append("  " + wr(ins, ops[0], "_r", n))
            if live_after:
                out.append("  " + self.set_flags("FLAG_NEG", "_a", "0U", "_r", n))
            out.append("}")
            return out
        if m == "not":
            n = opsize(ins, ops[0])
            return [wr(ins, ops[0], "~(%s)" % self.cast(rd(ins, ops[0]), n), n)]
        if m in ("adc", "sbb"):
            n = opsize(ins, ops[0])
            sign = "+" if m == "adc" else "-"
            out.append("{ uint32_t _a = %s, _b = %s, _c = (uint32_t)fl_cf(GUEST_FL);"
                       % (self.cast(rd(ins, ops[0]), n),
                          self.cast(rd(ins, ops[1], n), n)))
            out.append("  uint32_t _r = (uint32_t)(_a %s _b %s _c);" % (sign, sign))
            out.append("  " + wr(ins, ops[0], "_r", n))
            if live_after:
                out.append("  " + self.set_flags(
                    "FLAG_ADD" if m == "adc" else "FLAG_SUB", "_a", "_b", "_r", n))
            out.append("}")
            return out
        if m in ("shl", "sal", "shr", "sar"):
            n = opsize(ins, ops[0])
            cnt = rd(ins, ops[1], 1) if len(ops) > 1 else "1U"
            body = {
                "shl": "(uint32_t)(_a << _n)", "sal": "(uint32_t)(_a << _n)",
                "shr": "(uint32_t)(_a >> _n)",
                "sar": "(uint32_t)((int32_t)(%s)_a >> _n)" % STY[n],
            }[m]
            out.append("{ uint32_t _a = %s, _n = (%s) & 31U;"
                       % (self.cast(rd(ins, ops[0]), n), cnt))
            out.append("  uint32_t _r = %s;" % body)
            out.append("  if (_n) { " + wr(ins, ops[0], "_r", n))
            if live_after:
                flag_op = {"shl": "FLAG_SHL", "sal": "FLAG_SHL",
                           "shr": "FLAG_SHR", "sar": "FLAG_SAR"}[m]
                out.append("    " + self.set_flags(flag_op, "_a", "_n", "_r", n))
            out.append("  } }")
            return out
        if m == "imul":
            if len(ops) == 1:
                n = opsize(ins, ops[0])
                if n != 4:
                    raise Unsupported(ins, "one-operand imul width %d" % n)
                out.append("{ int64_t _p = (int64_t)(int32_t)GR(eax) * "
                           "(int64_t)(int32_t)%s;" % rd(ins, ops[0], 4))
                out.append("  GR(eax) = (uint32_t)_p; "
                           "GR(edx) = (uint32_t)((uint64_t)_p >> 32);")
                if live_after:
                    out.append(
                        "  GUEST_FL->f_cf = GUEST_FL->f_of = (uint8_t)"
                        "(_p != (int64_t)(int32_t)(uint32_t)_p);")
                    out.append("  " + self.set_flags("FLAG_IMUL", "0U", "0U",
                                                     "(uint32_t)_p", 4))
                out.append("}")
                return out
            a = ops[0]
            b = ops[1]
            src = ops[2] if len(ops) == 3 else ops[0]
            n = opsize(ins, a)
            out.append(
                "{ int64_t _p = (int64_t)(%s)(%s) * (int64_t)(%s)(%s);"
                % (STY[n], rd(ins, b, n), STY[n], rd(ins, src, n)))
            out.append("  uint32_t _r = (uint32_t)_p;")
            out.append("  " + wr(ins, a, "_r", n))
            if live_after:
                out.append(
                    "  GUEST_FL->f_cf = GUEST_FL->f_of = (uint8_t)"
                    "(_p != (int64_t)(%s)_r);" % STY[n])
                out.append("  " + self.set_flags("FLAG_IMUL", "0U", "0U", "_r", n))
            out.append("}")
            return out
        if m == "mul":
            n = opsize(ins, ops[0])
            if n != 4:
                raise Unsupported(ins, "mul width %d" % n)
            out.append("{ uint64_t _p = (uint64_t)GR(eax) * (uint64_t)%s;"
                       % rd(ins, ops[0], 4))
            out.append("  GR(eax) = (uint32_t)_p; GR(edx) = (uint32_t)(_p >> 32);")
            if live_after:
                out.append(
                    "  GUEST_FL->f_cf = GUEST_FL->f_of = "
                    "(uint8_t)((_p >> 32) != 0U);")
                out.append("  " + self.set_flags("FLAG_MUL", "0U", "0U",
                                                 "(uint32_t)_p", 4))
            out.append("}")
            return out
        if m in ("div", "idiv"):
            n = opsize(ins, ops[0])
            if n != 4:
                raise Unsupported(ins, "%s width %d" % (m, n))
            if m == "div":
                return ["{ uint64_t _d = ((uint64_t)GR(edx) << 32) | GR(eax);",
                        "  uint32_t _v = %s;" % rd(ins, ops[0], 4),
                        "  if (!_v) { GUEST_GPR_FLUSH(c); guest_fault(c, 0x%xU,"
                        " \"divide by zero\"); return; }" % ins.address,
                        "  GR(eax) = (uint32_t)(_d / _v); GR(edx) = (uint32_t)(_d % _v); }"]
            return ["{ int64_t _d = (int64_t)(((uint64_t)GR(edx) << 32) | GR(eax));",
                    "  int32_t _v = (int32_t)%s;" % rd(ins, ops[0], 4),
                    "  if (!_v) { GUEST_GPR_FLUSH(c); guest_fault(c, 0x%xU,"
                    " \"divide by zero\"); return; }" % ins.address,
                    "  GR(eax) = (uint32_t)(_d / _v); GR(edx) = (uint32_t)(_d % _v); }"]

        # ---- control flow ---------------------------------------------
        if m == "jmp":
            o = ops[0]
            if o.type == cx.X86_OP_IMM:
                # A `jmp` whose target this function did not decode is not a
                # branch, it is a TAIL CALL -- MSVC ends a function that way
                # constantly, and it also jumps into shared epilogues. The
                # first full build failed 14 of 145 units on exactly this:
                # `goto` to a label that was never emitted, because recursive
                # descent correctly stopped at the function boundary.
                # No return address is pushed: the caller's is already there,
                # which is the whole point of a tail call.
                if o.imm in self.insns:
                    self.labels.add(o.imm)
                    return ["goto L_%08x;" % o.imm]
                if self.known is not None and o.imm not in self.known:
                    self.bad_targets += 1
                    return ["GUEST_GPR_FLUSH(c); guest_fault(c, 0x%xU,"
                            " \"tail call to %08x, not a known function\");"
                            " return;" % (ins.address, o.imm)]
                self.calls.add(o.imm)
                # A tail transfer hands the callee the registers: publish
                # them; nothing runs here afterwards, so no reload.
                return ["GUEST_GPR_FLUSH(c); sub_%08x(c); return;" % o.imm]
            self.indirect += 1
            # Several switch indices commonly share one case label.  Keep the
            # PE slot count in the resolver's report, but C permits only one
            # `case` for each loaded target value.
            targets = tuple(dict.fromkeys(
                self.indirect_edges.get(ins.address, ())))
            if targets:
                for target in targets:
                    self.labels.add(target)
                lines = ["{ uint32_t _target = %s;" % rd(ins, o, 4),
                         "  switch (_target) {"]
                for target in targets:
                    lines.append("  case 0x%08xU: goto L_%08x;"
                                 % (self.ctx.va(target), target))
                lines.extend(["  default: GUEST_FLAGS_FLUSH(c);"
                              " GUEST_GPR_FLUSH(c);"
                              " guest_call(c, _target); return;",
                              "  }",
                              "}"])
                return lines
            iat = iat_slot_operand(self.ctx, self.import_slots, o)
            if iat is not None:
                # `jmp dword ptr [IAT slot]`: the destination is a host
                # import.  Host shims never read the x86 flags (the assumption
                # every `call [IAT]` site already makes), flag_liveness saw no
                # consumer here, and nothing is published: LOAD followed by
                # FLUSH was an identity copy of c->fl in every such thunk.
                # The token guards the exact slot value (guest.h) and keeps
                # guest_call for every other value.
                self.iat_jmp_sites += 1
                return ["GUEST_GPR_FLUSH(c); GUEST_IMPORT_JMP(%s, 0x%08xU, %dU);"
                        " return;" % (rd(ins, o, 4), iat[0], iat[1])]
            # An indirect JMP hands the destination the arithmetic flags
            # (flag_liveness keeps them live here); a unit that holds the
            # flag state in locals must publish it first.
            return ["GUEST_FLAGS_FLUSH(c); GUEST_GPR_FLUSH(c);"
                    " guest_call(c, %s); return;" % rd(ins, o, 4)]
        if m.startswith("j"):
            cc = norm_cc(m, "j")
            if cc not in CC:
                raise Unsupported(ins, "unknown condition %r" % cc)
            t = ops[0].imm
            cond = self.condition(cc, pair)
            if t not in self.insns:             # conditional tail call
                if self.known is not None and t not in self.known:
                    self.bad_targets += 1
                    return ["if (%s) { GUEST_GPR_FLUSH(c); guest_fault(c, 0x%xU,"
                            " \"tail call to %08x, not a known function\");"
                            " return; }" % (cond, ins.address, t)]
                self.calls.add(t)
                return ["if (%s) { GUEST_GPR_FLUSH(c); sub_%08x(c); return; }"
                        % (cond, t)]
            self.labels.add(t)
            return ["if (%s) goto L_%08x;" % (cond, t)]
        if m.startswith("set"):
            cc = norm_cc(m, "set")
            if cc not in CC:
                raise Unsupported(ins, "unknown condition %r" % cc)
            return [wr(ins, ops[0], "(%s) ? 1U : 0U" % self.condition(cc, pair), 1)]
        if m.startswith("cmov"):
            cc = norm_cc(m, "cmov")
            if cc not in CC:
                raise Unsupported(ins, "unknown condition %r" % cc)
            n = opsize(ins, ops[0])
            return ["if (%s) { %s }" % (self.condition(cc, pair),
                                        wr(ins, ops[0], rd(ins, ops[1], n), n))]
        if m == "call":
            o = ops[0]
            if o.type == cx.X86_OP_IMM:
                if self.known is not None and o.imm not in self.known:
                    self.bad_targets += 1
                    return ["GUEST_GPR_FLUSH(c); guest_fault(c, 0x%xU,"
                            " \"call to %08x, not a known function\");"
                            " return;" % (ins.address, o.imm)]
                self.calls.add(o.imm)
                # A proven tiny leaf is copied in place behind the
                # GUEST_LEAF_INLINE knob (leaf_inline.py); one statement.
                inlined = leaf_inline.site_statement(self, ins, o.imm)
                if inlined is not None:
                    return [inlined]
                # The callee reads and writes the CPU's registers: publish the
                # locals before the call and take back what it left behind.
                return ["GPUSH(0x%xU); GUEST_STACK_CALLSITE_BARRIER();"
                        " GUEST_GPR_FLUSH(c); sub_%08x(c); GUEST_GPR_RELOAD(c);"
                        % (ins.address + ins.size, o.imm)]
            self.indirect += 1
            # x86 resolves an indirect CALL target before pushing the return
            # address.  Reading it after gpush is observably wrong for an
            # ESP-relative operand (and for any register-held address which
            # aliases the word overwritten by the push).  Materialise every
            # indirect target first; limiting this to syntactic [esp+...] forms
            # would still miss the aliasing case.
            # `call dword ptr [IAT slot]` and `call reg` with reg loaded
            # from a slot in this block dispatch through GUEST_IMPORT_CALL:
            # the runtime compares _target with the validated token of the
            # named import and otherwise runs this very guest_call.
            iat = None
            if o.type == cx.X86_OP_MEM:
                iat = iat_slot_operand(self.ctx, self.import_slots, o)
                if iat is not None:
                    self.iat_call_sites += 1
            elif o.type == cx.X86_OP_REG and o.size == 4:
                iat = self._iat_regs.get(R32.get(o.reg))
                if iat is not None:
                    self.iat_reg_sites += 1
            dispatch = ("guest_call(c, _target);" if iat is None else
                        "GUEST_IMPORT_CALL(_target, 0x%08xU, %dU);" % iat)
            return ["{ uint32_t _target = %s;" % rd(ins, o, 4),
                    "  GPUSH(0x%xU);" % (ins.address + ins.size),
                    "  GUEST_STACK_CALLSITE_BARRIER();",
                    "  GUEST_GPR_FLUSH(c); %s GUEST_GPR_RELOAD(c);" % dispatch,
                    "}"]
        if m == "retf":
            # A far return pops both EIP and CS (plus an optional byte count).
            # Treating it as a near RET silently corrupts the guest stack.  The
            # two decoded instances currently sit in data/noreturn fallthrough,
            # so keep their containing functions and stop loudly only if that
            # path is ever proved reachable.
            return ["GUEST_GPR_FLUSH(c); guest_fault(c, 0x%xU,"
                    " \"unsupported far return\"); return;" % ins.address]
        if m == "ret":
            # The return address is popped because the guest pushed it; the
            # immediate form additionally removes the callee-cleaned arguments.
            # `ret 0x10` is how the Windows build revealed that PickOutcome
            # takes its RNG BY VALUE -- so this operand is load-bearing, not
            # bookkeeping.
            if ops:
                return ["(void)GPOP();",
                        "if (!GESP_ADJ(%dU)) return;" % ops[0].imm,
                        "GUEST_STACK_CALLSITE_BARRIER();",
                        "GUEST_GPR_FLUSH(c); return;"]
            return ["(void)GPOP();",
                    "GUEST_STACK_CALLSITE_BARRIER();",
                    "GUEST_GPR_FLUSH(c); return;"]
        if m == "int3":
            return ["GUEST_GPR_FLUSH(c); guest_int3(c, 0x%xU);"
                    " GUEST_GPR_RELOAD(c);" % ins.address]
        if m == "int":
            vector = ops[0].imm if ops and ops[0].type == cx.X86_OP_IMM else 0
            # A software interrupt is a terminal exceptional edge.  Keeping it
            # loud locally is crucial: MSVC's security-cookie helper contains
            # `int 29h` only in its failure branch, and stubbing the WHOLE
            # helper made the normal cookie-match path impossible to run.
            return ["GUEST_GPR_FLUSH(c); guest_fault(c, 0x%xU,"
                    " \"software interrupt 0x%x\"); return;"
                    % (ins.address, vector)]

        # ---- target-owned CPU capability probes -----------------------
        # These cannot be emitted as host instructions in portable C.  The
        # PC backend reports the real x86 host; Vita can provide a deliberate
        # conservative profile so the CRT selects code paths our translator
        # and target actually support.
        if m == "cpuid":
            return ["GUEST_GPR_FLUSH(c); guest_cpuid(c); GUEST_GPR_RELOAD(c);"]
        if m == "xgetbv":
            return ["GUEST_GPR_FLUSH(c); guest_xgetbv(c); GUEST_GPR_RELOAD(c);"]

        # ---- flags as a value -----------------------------------------
        if m == "pushfd":
            return ["GPUSH((uint32_t)(0x2U |"
                    " (fl_cf(GUEST_FL) ? 0x1U : 0U) |"
                    " (fl_pf(GUEST_FL) ? 0x4U : 0U) |"
                    " (fl_zf(GUEST_FL) ? 0x40U : 0U) |"
                    " (fl_sf(GUEST_FL) ? 0x80U : 0U) |"
                    " (c->df ? 0x400U : 0U) |"
                    " (fl_of(GUEST_FL) ? 0x800U : 0U))); "
                    "GUEST_STACK_CALLSITE_BARRIER();"]
        if m == "lahf":
            # AH := SF ZF 0 AF 0 PF 1 CF. AF is not modelled; MSVC emits lahf
            # only in float-comparison and 64-bit helper sequences, none of
            # which read AF, so it is emitted as 0 and recorded as such.
            return ["GR(eax) = (GR(eax) & 0xFFFF00FFU) | ((uint32_t)("
                    "(fl_sf(GUEST_FL) ? 0x80U : 0U) | (fl_zf(GUEST_FL) ? 0x40U : 0U) |"
                    " (fl_pf(GUEST_FL) ? 0x04U : 0U) | 0x02U |"
                    " (fl_cf(GUEST_FL) ? 0x01U : 0U)) << 8);"]
        if m == "sahf":
            return ["{ uint32_t _f = (GR(eax) >> 8) & 0xFFU;",
                    "  GUEST_FL->f_op = FLAG_EXPLICIT; GUEST_FL->f_sz = 4;",
                    "  GUEST_FL->f_cf = (_f & 1U) != 0; GUEST_FL->f_of = 0;",
                    "  GUEST_FL->f_r = (_f & 0x40U) ? 0U : 1U; }"]
        if m in ("clc", "stc", "cmc"):
            cf = {"clc": "0U", "stc": "1U", "cmc": "!fl_cf(GUEST_FL)"}[m]
            return ["{ uint32_t _cf = (uint32_t)(%s), _of = (uint32_t)fl_of(GUEST_FL);"
                    % cf,
                    "  uint32_t _zf = (uint32_t)fl_zf(GUEST_FL),"
                    " _sf = (uint32_t)fl_sf(GUEST_FL), _pf = (uint32_t)fl_pf(GUEST_FL);",
                    "  GUEST_FL->f_op = FLAG_PARTIAL; GUEST_FL->f_sz = 4;",
                    "  GUEST_FL->f_cf = (uint8_t)_cf; GUEST_FL->f_of = (uint8_t)_of;",
                    "  GUEST_FL->f_zf = (uint8_t)_zf; GUEST_FL->f_sf = (uint8_t)_sf;"
                    " GUEST_FL->f_pf = (uint8_t)_pf; }"]

        # ---- bit test -------------------------------------------------
        if m in ("bt", "bts", "btr", "btc"):
            n = opsize(ins, ops[0])
            if ops[0].type != cx.X86_OP_REG:
                raise Unsupported(ins, "%s with a memory bit base" % m)
            bits = n * 8
            out.append("{ uint32_t _v = %s, _i = (%s) & %uU;"
                       % (self.cast(rd(ins, ops[0]), n),
                          rd(ins, ops[1], n), bits - 1))
            out.append("  GUEST_FL->f_op = FLAG_EXPLICIT; GUEST_FL->f_sz = %d; GUEST_FL->f_of = 0;" % n)
            out.append("  GUEST_FL->f_cf = (_v >> _i) & 1U; GUEST_FL->f_r = 1U;")
            if m != "bt":
                op = {"bts": "|=", "btr": "&= ~", "btc": "^="}[m]
                out.append("  _v %s (1U << _i);" % op)
                out.append("  " + wr(ins, ops[0], "_v", n))
            out.append("}")
            return out

        # ---- rotates --------------------------------------------------
        if m in ("rol", "ror"):
            n = opsize(ins, ops[0])
            bits = n * 8
            cnt = rd(ins, ops[1], 1) if len(ops) > 1 else "1U"
            expr = ("((_v << _n) | (_v >> (%d - _n)))" % bits if m == "rol"
                    else "((_v >> _n) | (_v << (%d - _n)))" % bits)
            out.append("{ uint32_t _v = %s, _n = (%s) & %uU;"
                       % (self.cast(rd(ins, ops[0]), n), cnt, bits - 1))
            out.append("  if (_n) { uint32_t _r = (uint32_t)%s;" % expr)
            out.append("    " + wr(ins, ops[0], "_r", n))
            out.append("  } }")
            return out
        if m in ("rcl", "rcr"):
            n = opsize(ins, ops[0])
            if n not in (1, 2, 4):
                raise Unsupported(ins, "%s width %d" % (m, n))
            bits = n * 8
            cnt = rd(ins, ops[1], 1) if len(ops) > 1 else "1U"
            out.append("{ uint32_t _v = %s, _raw = (%s) & 31U;"
                       % (self.cast(rd(ins, ops[0]), n), cnt))
            out.append("  uint32_t _n = %s%s;"
                       % ("_raw" if bits == 32 else "_raw %% %dU" % (bits + 1),
                          ", _mask = %s" % u32((1 << bits) - 1)
                          if m == "rcl" else ""))
            out.append("  if (_n) { uint32_t _cf = (uint32_t)fl_cf(GUEST_FL), _i;")
            out.append("    uint32_t _zf = (uint32_t)fl_zf(GUEST_FL),"
                       " _sf = (uint32_t)fl_sf(GUEST_FL), _pf = (uint32_t)fl_pf(GUEST_FL),"
                       " _of = (uint32_t)fl_of(GUEST_FL);")
            if m == "rcr":
                out.append("    for (_i = 0; _i < _n; ++_i) {"
                           " uint32_t _next = _v & 1U;"
                           " _v = (_v >> 1) | (_cf << %d); _cf = _next; }"
                           % (bits - 1))
                of_expr = "((_v >> %d) ^ (_v >> %d)) & 1U" % (bits - 1, bits - 2)
            else:
                out.append("    for (_i = 0; _i < _n; ++_i) {"
                           " uint32_t _next = (_v >> %d) & 1U;"
                           " _v = ((_v << 1) & _mask) | _cf; _cf = _next; }"
                           % (bits - 1))
                of_expr = "((_v >> %d) ^ _cf) & 1U" % (bits - 1)
            out.append("    " + wr(ins, ops[0], "_v", n))
            out.append("    GUEST_FL->f_op = FLAG_PARTIAL; GUEST_FL->f_sz = %d;" % n)
            out.append("    GUEST_FL->f_cf = (uint8_t)_cf;"
                       " GUEST_FL->f_of = (uint8_t)(_n == 1U ? %s : _of);"
                       % of_expr)
            out.append("    GUEST_FL->f_zf = (uint8_t)_zf; GUEST_FL->f_sf = (uint8_t)_sf;"
                       " GUEST_FL->f_pf = (uint8_t)_pf;")
            out.append("  } }")
            return out
        if m in ("shld", "shrd"):
            n = opsize(ins, ops[0])
            if n != 4:
                raise Unsupported(ins, "%s width %d" % (m, n))
            cnt = rd(ins, ops[2], 1)
            expr = ("((_a << _n) | (_b >> (32 - _n)))" if m == "shld"
                    else "((_a >> _n) | (_b << (32 - _n)))")
            out.append("{ uint32_t _a = %s, _b = %s, _n = (%s) & 31U;"
                       % (rd(ins, ops[0], 4), rd(ins, ops[1], 4), cnt))
            out.append("  if (_n) { uint32_t _r = (uint32_t)%s;" % expr)
            out.append("    " + wr(ins, ops[0], "_r", 4))
            if live_after:
                out.append("    " + self.set_flags("FLAG_LOGIC", "_a", "_n",
                                                   "_r", 4))
            out.append("  } }")
            return out

        # ---- SSE ------------------------------------------------------
        # `movsd` and `movq` are each TWO instructions sharing one mnemonic:
        # the string move / MMX move, and the SSE scalar-double / quadword
        # move. Dispatching on the name alone sent `rep movsd` into the SSE
        # handler, which then refused it for having no xmm operand -- 48
        # functions blocked by a name collision after the string rule was
        # already written. The discriminator is the operands, not the string.
        _sse_ambiguous = m in ("movsd", "movq") and not any(
            o.type == cx.X86_OP_REG and o.reg in XMMR for o in ops)
        if not _sse_ambiguous:
            r = self.sse(ins, m, ops, live_after)
            if r is not None:
                return r

        # ---- direction flag -------------------------------------------
        if m == "cld":
            return ["c->df = 0;"]
        if m == "std":
            return ["c->df = 1;"]
        if m in ("cli", "sti"):
            # Ring 3. These fault on a real Windows process; MSVC never emits
            # them, so reaching one means the bytes are not code. Keep it as a
            # translated no-op rather than a hard stop, but say so.
            return ["/* %s: ring-0 only, unreachable in user mode */" % m]

        # ---- bit scan --------------------------------------------------
        if m in ("bsf", "bsr"):
            n = opsize(ins, ops[0])
            if n != 4:
                raise Unsupported(ins, "%s width %d" % (m, n))
            out.append("{ uint32_t _v = %s;" % rd(ins, ops[1], 4))
            out.append("  GUEST_FL->f_op = FLAG_EXPLICIT; GUEST_FL->f_sz = 4;")
            out.append("  GUEST_FL->f_cf = 0; GUEST_FL->f_of = 0; GUEST_FL->f_r = _v ? 1U : 0U;")
            # When the source is zero the destination is ARCHITECTURALLY
            # UNDEFINED and ZF is set. Leaving the destination alone is what
            # real hardware does and what the following `jz` depends on.
            out.append("  if (_v) { uint32_t _i;")
            if m == "bsf":
                out.append("    for (_i = 0; !((_v >> _i) & 1U); _i++) {}")
            else:
                out.append("    for (_i = 31; !((_v >> _i) & 1U); _i--) {}")
            out.append("    " + wr(ins, ops[0], "_i", 4))
            out.append("  } }")
            return out

        # ---- string operations -----------------------------------------
        # `rep movsd` and `rep stosd` are 69 functions between them: MSVC's
        # inline memcpy and memset. The direction flag is honoured rather than
        # assumed forward -- `std` exists in this binary.
        S_OPS = {"movsb": 1, "movsw": 2, "movsd": 4,
                 "stosb": 1, "stosw": 2, "stosd": 4,
                 "lodsb": 1, "lodsw": 2, "lodsd": 4,
                 "scasb": 1, "scasw": 2, "scasd": 4,
                 "cmpsb": 1, "cmpsw": 2, "cmpsd": 4}
        if m in S_OPS and (ins.operands is None or not [
                o for o in ops if o.type == cx.X86_OP_REG and o.reg in XMMR]):
            n = S_OPS[m]
            rep = rep_pfx is not None
            # `repe`/`repne` differ only for scas/cmps; for movs/stos/lods any
            # rep form is the same plain count-down loop.
            repne = rep_pfx in ("repne", "repnz")
            kind = m[:-1]
            ld, st = LD[n], STO[n]
            step = "%d" % n
            out.append("{ int32_t _d = c->df ? -%s : %s;" % (step, step))
            if rep:
                out.append("  while (GR(ecx)) {")
                ind = "    "
            else:
                out.append("  {")
                ind = "    "
            if kind == "movs":
                out.append(ind + "%s(GR(edi), %s(GR(esi)));" % (st, ld))
                out.append(ind + "GR(esi) += (uint32_t)_d; GR(edi) += (uint32_t)_d;")
            elif kind == "stos":
                out.append(ind + "%s(GR(edi), (uint%d_t)GR(eax));" % (st, n * 8))
                out.append(ind + "GR(edi) += (uint32_t)_d;")
            elif kind == "lods":
                out.append(ind + "GR(eax) = (GR(eax) & ~%s) | %s(GR(esi));"
                           % (u32((1 << (n * 8)) - 1), ld))
                out.append(ind + "GR(esi) += (uint32_t)_d;")
            else:                       # scas / cmps set flags and may stop
                a = "(uint32_t)GR(eax)" if kind == "scas" else "%s(GR(esi))" % ld
                out.append(ind + "uint32_t _a = %s, _b = %s(GR(edi));" % (a, ld))
                out.append(ind + "GUEST_FL->f_op = FLAG_SUB; GUEST_FL->f_sz = %d;" % n)
                out.append(ind + "GUEST_FL->f_a = _a; GUEST_FL->f_b = _b; GUEST_FL->f_r = _a - _b;")
                if kind == "cmps":
                    out.append(ind + "GR(esi) += (uint32_t)_d;")
                out.append(ind + "GR(edi) += (uint32_t)_d;")
            if rep:
                out.append(ind + "GR(ecx)--;")
                if kind in ("scas", "cmps"):
                    # repe runs while equal, so it stops when the compare
                    # differs; repne is the mirror. Getting this backwards
                    # turns a string search into its own negation.
                    test = "if (%s(GUEST_FL->f_r == 0)) break;" % (
                        "!" if not repne else "")
                    out.append(ind + test)
            out.append("  } }")
            return out

        # ---- atomics ----------------------------------------------------
        # CMPXCHG8B is the primitive under mimalloc's 64-bit CAS loops.  A
        # split ld64/st64 implementation would lose both atomicity and the
        # failure value returned in EDX:EAX, so keep the operation behind one
        # host primitive.  Only the two measured locked memory forms are
        # accepted; a new shape must be audited rather than silently widened.
        if m == "cmpxchg8b":
            if (not lock_pfx or len(ops) != 1 or
                    ops[0].type != cx.X86_OP_MEM or
                    opsize(ins, ops[0]) != 8):
                raise Unsupported(ins, "expected lock cmpxchg8b m64")
            out.append("{ uint32_t _addr = %s;" % addr_expr(ins, ops[0]))
            if live_after:
                # CMPXCHG8B changes only ZF.  Materialise every other modelled
                # flag before replacing the lazy producer, then store the new
                # ZF beside that preserved snapshot.
                out.append("  uint32_t _cf = (uint32_t)fl_cf(GUEST_FL),"
                           " _of = (uint32_t)fl_of(GUEST_FL);")
                out.append("  uint32_t _sf = (uint32_t)fl_sf(GUEST_FL),"
                           " _pf = (uint32_t)fl_pf(GUEST_FL);")
            out.append("  uint64_t _expected = ((uint64_t)GR(edx) << 32) |"
                       " (uint64_t)GR(eax);")
            out.append("  uint64_t _desired = ((uint64_t)GR(ecx) << 32) |"
                       " (uint64_t)GR(ebx);")
            out.append("  uint64_t _observed = guest_atomic_cmpxchg64("
                       "_addr, _expected, _desired);")
            out.append("  uint32_t _equal = (uint32_t)(_observed == _expected);")
            if live_after:
                out.append("  GUEST_FL->f_op = FLAG_PARTIAL; GUEST_FL->f_sz = 4;")
                out.append("  GUEST_FL->f_cf = (uint8_t)_cf; GUEST_FL->f_of = (uint8_t)_of;")
                out.append("  GUEST_FL->f_zf = (uint8_t)_equal;"
                           " GUEST_FL->f_sf = (uint8_t)_sf; GUEST_FL->f_pf = (uint8_t)_pf;")
            out.append("  if (!_equal) { GR(eax) = (uint32_t)_observed;"
                       " GR(edx) = (uint32_t)(_observed >> 32); }")
            out.append("}")
            return out

        # The remaining currently translated lock-prefixed operations were
        # admitted under the single-guest-thread milestone and are emitted
        # sequentially.  Do not copy that compromise into a new atomic form.
        if m == "cmpxchg":
            n = opsize(ins, ops[0])
            acc = {1: "(uint8_t)GR(eax)", 2: "(uint16_t)GR(eax)",
                   4: "GR(eax)"}[n]
            out.append("{ uint32_t _dst = %s, _acc = %s;"
                       % (self.cast(rd(ins, ops[0]), n), acc))
            out.append("  GUEST_FL->f_op = FLAG_SUB; GUEST_FL->f_sz = %d;" % n)
            out.append("  GUEST_FL->f_a = _acc; GUEST_FL->f_b = _dst; GUEST_FL->f_r = _acc - _dst;")
            out.append("  if (_acc == _dst) {")
            out.append("    " + wr(ins, ops[0], rd(ins, ops[1], n), n))
            out.append("  } else {")
            out.append("    GR(eax) = (GR(eax) & ~%s) | _dst;"
                       % u32((1 << (n * 8)) - 1) if n < 4 else "    GR(eax) = _dst;")
            out.append("  } }")
            return out
        if m == "xadd":
            n = opsize(ins, ops[0])
            out.append("{ uint32_t _a = %s, _b = %s, _r = _a + _b;"
                       % (self.cast(rd(ins, ops[0]), n),
                          self.cast(rd(ins, ops[1]), n)))
            out.append("  " + wr(ins, ops[1], "_a", n))
            out.append("  " + wr(ins, ops[0], "_r", n))
            if live_after:
                out.append("  " + self.set_flags("FLAG_ADD", "_a", "_b",
                                                 "_r", n))
            out.append("}")
            return out

        # ---- x87 ------------------------------------------------------
        r = self.x87(ins, m, ops)
        if r is not None:
            return r

        raise Unsupported(ins, "no rule")

    # -- conditions -------------------------------------------------------
    def condition(self, cc, pair):
        if pair is not None and pair[2] in sse_lower.FLOAT_PAIR_KINDS:
            # A comis producer: the driver pairs only the conditions of the
            # float relation table (trans.direct_flag_consumer).
            if cc not in sse_lower.FLOAT_DIRECT:
                raise RuntimeError("comis pair reached condition %r" % cc)
            self.direct_pairs += 1
            return sse_lower.fcc(cc, pair[0], pair[1])
        if pair is not None and cc in DIRECT:
            self.direct_pairs += 1
            a, b, n = pair
            if n != 4 and cc in SIGNED_DIRECT:
                # DIRECT's signed relations cast to int32_t; a narrow operand
                # is already `(uintN_t)(...)`, so go through the signed narrow
                # type first to sign-extend instead of zero-extend.
                a = "(%s)%s" % (STY[n], a)
                b = "(%s)%s" % (STY[n], b)
            return DIRECT[cc].format(a=a, b=b)
        return "%s(GUEST_FL)" % CC[cc][0]

    # -- SSE --------------------------------------------------------------
    def sse(self, ins, m, ops, live_after):
        SC = {"ss": ("f", 0, "float"), "sd": ("d", 0, "double")}

        if m in ("movss", "movsd"):
            fld, _, ty = SC[m[-2:]]
            d, s = ops
            if d.type == cx.X86_OP_REG and s.type == cx.X86_OP_REG:
                return ["%s.%s[0] = %s.%s[0];" % (xmm_of(ins, d), fld,
                                                  xmm_of(ins, s), fld)]
            if d.type == cx.X86_OP_REG:
                # a load from memory zeroes the rest of the register -- unless
                # nothing in this function can observe those lanes (see
                # xmm_zero_fill_elidable)
                if ins.address in self.xmm_zero_elide:
                    self.xmm_zero_elided += 1
                    return ["%s.%s[0] = %s(%s);"
                            % (xmm_of(ins, d), fld,
                               "ldf" if ty == "float" else "ldd",
                               addr_expr(ins, s))]
                return ["%s = (xmm_t){0}; %s.%s[0] = %s(%s);"
                        % (xmm_of(ins, d), xmm_of(ins, d), fld,
                           "ldf" if ty == "float" else "ldd", addr_expr(ins, s))]
            return ["%s(%s, %s.%s[0]);"
                    % ("stf" if ty == "float" else "std_",
                       addr_expr(ins, d), xmm_of(ins, s), fld)]

        if m in ("movaps", "movups", "movdqa", "movdqu"):
            d, s = ops
            if d.type == cx.X86_OP_REG and s.type == cx.X86_OP_REG:
                if self.sse_lower:
                    return [sse_lower.token_mov(
                        self._xmm_index(ins, d), self._xmm_index(ins, s),
                        lane0=ins.address in self.xmm_lane0)]
                return ["%s = %s;" % (xmm_of(ins, d), xmm_of(ins, s))]
            if d.type == cx.X86_OP_REG:
                if self.sse_lower:
                    return [sse_lower.token_ldx(self._xmm_index(ins, d),
                                                addr_expr(ins, s))]
                return ["%s = ldx(%s);" % (xmm_of(ins, d), addr_expr(ins, s))]
            if self.sse_lower:
                return [sse_lower.token_stx(addr_expr(ins, d),
                                            self._xmm_index(ins, s))]
            return ["stx(%s, %s);" % (addr_expr(ins, d), xmm_of(ins, s))]

        if m in ("movd", "vmovd"):
            d, s = ops
            if d.type == cx.X86_OP_REG and d.reg in XMMR:
                return ["%s = (xmm_t){0}; %s.u32[0] = %s;"
                        % (xmm_of(ins, d), xmm_of(ins, d), rd(ins, s, 4))]
            return [wr(ins, d, "%s.u32[0]" % xmm_of(ins, s), 4)]

        if m == "vpinsrd":
            d, vector, scalar, index = ops
            if (d.type != cx.X86_OP_REG or d.reg not in XMMR or
                    vector.type != cx.X86_OP_REG or vector.reg not in XMMR or
                    index.type != cx.X86_OP_IMM):
                raise Unsupported(ins, "unsupported vpinsrd operand form")
            lane = index.imm & 3
            return ["{ xmm_t _v = %s;" % xmm_of(ins, vector),
                    "  _v.u32[%d] = %s; %s = _v; }"
                    % (lane, rd(ins, scalar, 4), xmm_of(ins, d))]

        if m in ("vcvtqq2pd", "vcvtuqq2pd"):
            d, s = ops
            if d.type != cx.X86_OP_REG or d.reg not in XMMR:
                raise Unsupported(ins, "expected an xmm destination")
            src = (xmm_of(ins, s) if s.type == cx.X86_OP_REG
                   else "ldx(%s)" % addr_expr(ins, s))
            fld = "i64" if m == "vcvtqq2pd" else "u64"
            return ["{ xmm_t _s = %s, _r = (xmm_t){0};" % src,
                    "  _r.d[0] = (double)_s.%s[0];"
                    " _r.d[1] = (double)_s.%s[1]; %s = _r; }"
                    % (fld, fld, xmm_of(ins, d))]

        if m == "movq":
            d, s = ops
            if d.type == cx.X86_OP_REG and d.reg in XMMR:
                if s.type == cx.X86_OP_MEM:
                    return ["%s = (xmm_t){0}; %s.u64[0] = ld64(%s);"
                            % (xmm_of(ins, d), xmm_of(ins, d), addr_expr(ins, s))]
                return ["%s = (xmm_t){0}; %s.u64[0] = %s.u64[0];"
                        % (xmm_of(ins, d), xmm_of(ins, d), xmm_of(ins, s))]
            if d.type == cx.X86_OP_MEM:
                return ["st64(%s, %s.u64[0]);" % (addr_expr(ins, d), xmm_of(ins, s))]

        # scalar arithmetic
        for base, cop in (("add", "+"), ("sub", "-"), ("mul", "*"), ("div", "/")):
            for sfx, fld, ldi in (("ss", "f", "ldf"), ("sd", "d", "ldd")):
                if m == base + sfx:
                    d, s = ops
                    rhs = ("%s.%s[0]" % (xmm_of(ins, s), fld)
                           if s.type == cx.X86_OP_REG
                           else "%s(%s)" % (ldi, addr_expr(ins, s)))
                    return ["%s.%s[0] = (%s)(%s.%s[0] %s %s);"
                            % (xmm_of(ins, d), fld,
                               "float" if fld == "f" else "double",
                               xmm_of(ins, d), fld, cop, rhs)]

        if m in ("sqrtss", "sqrtsd"):
            fld = "f" if m.endswith("ss") else "d"
            d, s = ops
            rhs = ("%s.%s[0]" % (xmm_of(ins, s), fld) if s.type == cx.X86_OP_REG
                   else "%s(%s)" % ("ldf" if fld == "f" else "ldd", addr_expr(ins, s)))
            fn = "sqrtf" if fld == "f" else "sqrt"
            return ["%s.%s[0] = %s(%s);" % (xmm_of(ins, d), fld, fn, rhs)]

        if m in ("comiss", "ucomiss", "comisd", "ucomisd"):
            fld = "f" if "ss" in m else "d"
            d, s = ops
            rhs = ("%s.%s[0]" % (xmm_of(ins, s), fld) if s.type == cx.X86_OP_REG
                   else "%s(%s)" % ("ldf" if fld == "f" else "ldd", addr_expr(ins, s)))
            # These instructions define CF/ZF/PF independently.  In
            # particular, equal means ZF=1 but PF=0; encoding equality only
            # through f_r makes the generic parity derivation report PF=1.
            # That used to turn MSVC's UCOMISS/LAHF/TEST AH,44h equality test
            # in SortCharRenders into an endless equal-key swap loop.
            if self.sse_lower and not live_after:
                # Every consumer was answered directly (sse_lower.FLOAT_DIRECT
                # via condition()) or the flags are dead: the producer token
                # keeps the lazy producer under the header knob 0 and
                # disappears under knob 1.
                return [sse_lower.token_comis_paired(
                    fld, self._xmm_index(ins, d), rhs)]
            return ["{ double _x = (double)%s.%s[0], _y = (double)(%s);"
                    % (xmm_of(ins, d), fld, rhs),
                    "  GUEST_FL->f_op = FLAG_PARTIAL; GUEST_FL->f_sz = 4;",
                    "  GUEST_FL->f_sf = 0; GUEST_FL->f_of = 0;",
                    "  if (_x != _x || _y != _y) {",
                    "    GUEST_FL->f_cf = 1; GUEST_FL->f_zf = 1; GUEST_FL->f_pf = 1;",
                    "  } else {",
                    "    GUEST_FL->f_cf = (uint8_t)(_x < _y);",
                    "    GUEST_FL->f_zf = (uint8_t)(_x == _y); GUEST_FL->f_pf = 0;",
                    "  } }"]

        if m in ("cvtsi2ss", "cvtsi2sd"):
            fld = "f" if m.endswith("ss") else "d"
            d, s = ops
            return ["%s.%s[0] = (%s)(int32_t)%s;"
                    % (xmm_of(ins, d), fld, "float" if fld == "f" else "double",
                       rd(ins, s, 4))]
        if m in ("cvttss2si", "cvttsd2si"):
            fld = "f" if "ss" in m else "d"
            d, s = ops
            src = ("%s.%s[0]" % (xmm_of(ins, s), fld) if s.type == cx.X86_OP_REG
                   else "%s(%s)" % ("ldf" if fld == "f" else "ldd", addr_expr(ins, s)))
            return [wr(ins, d, "(uint32_t)(int32_t)(%s)" % src, 4)]
        if m in ("vcvttss2usi", "vcvttsd2usi"):
            fld = "f" if "ss" in m else "d"
            d, s = ops
            if opsize(ins, d) != 4:
                raise Unsupported(ins, "64-bit unsigned scalar conversion")
            src = ("%s.%s[0]" % (xmm_of(ins, s), fld)
                   if s.type == cx.X86_OP_REG
                   else "%s(%s)" % ("ldf" if fld == "f" else "ldd",
                                     addr_expr(ins, s)))
            # CVTT accepts values whose truncation lies in uint32_t. Avoid a
            # C cast outside that domain (undefined behaviour) and return the
            # architectural indefinite integer for invalid/NaN input.
            return ["{ double _v = (double)(%s);" % src,
                    "  uint32_t _r = (_v == _v && _v > -1.0 &&"
                    " _v < 4294967296.0) ? (uint32_t)_v : 0xffffffffU;",
                    "  " + wr(ins, d, "_r", 4) + " }"]
        if m == "cvtss2sd":
            d, s = ops
            src = ("%s.f[0]" % xmm_of(ins, s) if s.type == cx.X86_OP_REG
                   else "ldf(%s)" % addr_expr(ins, s))
            return ["%s.d[0] = (double)(%s);" % (xmm_of(ins, d), src)]
        if m == "cvtsd2ss":
            d, s = ops
            src = ("%s.d[0]" % xmm_of(ins, s) if s.type == cx.X86_OP_REG
                   else "ldd(%s)" % addr_expr(ins, s))
            return ["%s.f[0] = (float)(%s);" % (xmm_of(ins, d), src)]
        if m == "cvtdq2pd":
            d, s = ops
            src = xmm_of(ins, s) if s.type == cx.X86_OP_REG else None
            if src is None:
                return ["{ xmm_t _t = ldx(%s);" % addr_expr(ins, s),
                        "  %s.d[0] = (double)_t.i32[0]; %s.d[1] = (double)_t.i32[1]; }"
                        % (xmm_of(ins, d), xmm_of(ins, d))]
            # reads the low two dwords as SIGNED; temporaries are mandatory
            # because source and destination are often the same register.
            return ["{ int32_t _a = %s.i32[0], _b = %s.i32[1];" % (src, src),
                    "  %s.d[0] = (double)_a; %s.d[1] = (double)_b; }"
                    % (xmm_of(ins, d), xmm_of(ins, d))]
        if m == "cvtpd2ps":
            d, s = ops
            src = xmm_of(ins, s)
            return ["{ double _a = %s.d[0], _b = %s.d[1];" % (src, src),
                    "  %s = (xmm_t){0}; %s.f[0] = (float)_a; %s.f[1] = (float)_b; }"
                    % (xmm_of(ins, d), xmm_of(ins, d), xmm_of(ins, d))]

        if m in ("cvtdq2ps", "cvtps2dq", "cvttps2dq"):
            d, s = ops
            src = (xmm_of(ins, s) if s.type == cx.X86_OP_REG
                   else "ldx(%s)" % addr_expr(ins, s))
            if m == "cvtdq2ps" and self.sse_lower:
                return [sse_lower.token_cvtdq2ps(
                    self._xmm_index(ins, d),
                    lane0=ins.address in self.xmm_lane0,
                    **self._xmm_source(ins, s))]
            if m == "cvtdq2ps":
                return ["{ xmm_t _s = %s; int _i;" % src,
                        "  for (_i = 0; _i < 4; _i++) %s.f[_i] = (float)_s.i32[_i]; }"
                        % xmm_of(ins, d)]
            return ["{ xmm_t _s = %s; int _i;" % src,
                    "  for (_i = 0; _i < 4; _i++) %s.i32[_i] = (int32_t)_s.f[_i]; }"
                    % xmm_of(ins, d)]
        if m == "cvtps2pd":
            d, s = ops
            src = (xmm_of(ins, s) if s.type == cx.X86_OP_REG
                   else "ldx(%s)" % addr_expr(ins, s))
            return ["{ xmm_t _s = %s;" % src,
                    "  %s.d[0] = (double)_s.f[0]; %s.d[1] = (double)_s.f[1]; }"
                    % (xmm_of(ins, d), xmm_of(ins, d))]

        for base, fn in (("max", ">"), ("min", "<")):
            for sfx, fld in (("ss", "f"), ("sd", "d")):
                if m == base + sfx:
                    d, s = ops
                    rhs = ("%s.%s[0]" % (xmm_of(ins, s), fld)
                           if s.type == cx.X86_OP_REG
                           else "%s(%s)" % ("ldf" if fld == "f" else "ldd",
                                            addr_expr(ins, s)))
                    # x86 MAXSS/MINSS return the SECOND operand when either is
                    # NaN or both are zero -- not a C ternary on `>`.
                    return ["{ double _b = (double)(%s);" % rhs,
                            "  %s.%s[0] = (%s)((double)%s.%s[0] %s _b ? "
                            "(double)%s.%s[0] : _b); }"
                            % (xmm_of(ins, d), fld,
                               "float" if fld == "f" else "double",
                               xmm_of(ins, d), fld, fn, xmm_of(ins, d), fld)]

        if m in ("movlps", "movlpd"):
            d, s = ops
            if d.type == cx.X86_OP_REG:
                return ["%s.u64[0] = ld64(%s);" % (xmm_of(ins, d), addr_expr(ins, s))]
            return ["st64(%s, %s.u64[0]);" % (addr_expr(ins, d), xmm_of(ins, s))]
        if m in ("movhps", "movhpd"):
            d, s = ops
            if d.type == cx.X86_OP_REG:
                return ["%s.u64[1] = ld64(%s);" % (xmm_of(ins, d), addr_expr(ins, s))]
            return ["st64(%s, %s.u64[1]);" % (addr_expr(ins, d), xmm_of(ins, s))]

        if m in ("unpcklps", "unpckhps", "unpcklpd", "unpckhpd",
                 "punpcklbw", "punpcklwd", "punpckhwd", "punpckhdq"):
            d, s = ops
            src = (xmm_of(ins, s) if s.type == cx.X86_OP_REG
                   else "ldx(%s)" % addr_expr(ins, s))
            hi = m.startswith("unpckh") or m.startswith("punpckh")
            unit = {"unpcklps": 4, "unpckhps": 4, "unpcklpd": 8, "unpckhpd": 8,
                    "punpcklbw": 1, "punpcklwd": 2, "punpckhwd": 2,
                    "punpckhdq": 4}[m]
            if self.sse_lower:
                return [sse_lower.token_unpck(unit, int(hi),
                                              self._xmm_index(ins, d),
                                              **self._xmm_source(ins, s))]
            n = 16 // unit
            half = n // 2
            out = ["{ xmm_t _d = %s, _s = %s, _r; int _i;"
                   % (xmm_of(ins, d), src),
                   "  for (_i = 0; _i < %d; _i++) {" % half,
                   "    int _k = _i + %d;" % (half if hi else 0)]
            if unit == 1:
                out += ["    _r.u8[_i*2] = _d.u8[_k]; _r.u8[_i*2+1] = _s.u8[_k];"]
            elif unit == 2:
                out += ["    _r.u16[_i*2] = _d.u16[_k]; _r.u16[_i*2+1] = _s.u16[_k];"]
            elif unit == 4:
                out += ["    _r.u32[_i*2] = _d.u32[_k]; _r.u32[_i*2+1] = _s.u32[_k];"]
            else:
                out += ["    _r.u64[_i*2] = _d.u64[_k]; _r.u64[_i*2+1] = _s.u64[_k];"]
            out += ["  } %s = _r; }" % xmm_of(ins, d)]
            return out

        if m in ("shufps", "pshufd"):
            d, s, i = ops
            src = (xmm_of(ins, s) if s.type == cx.X86_OP_REG
                   else "ldx(%s)" % addr_expr(ins, s))
            if self.sse_lower and i.type == cx.X86_OP_IMM:
                token = (sse_lower.token_pshufd if m == "pshufd"
                         else sse_lower.token_shufps)
                return [token(self._xmm_index(ins, d), i.imm,
                              **self._xmm_source(ins, s))]
            if m == "pshufd":
                return ["{ xmm_t _s = %s, _r; unsigned _c = %uU; int _i;"
                        % (src, i.imm),
                        "  for (_i = 0; _i < 4; _i++) "
                        "_r.u32[_i] = _s.u32[(_c >> (_i*2)) & 3];",
                        "  %s = _r; }" % xmm_of(ins, d)]
            return ["{ xmm_t _d = %s, _s = %s, _r; unsigned _c = %uU;"
                    % (xmm_of(ins, d), src, i.imm),
                    "  _r.u32[0] = _d.u32[_c & 3]; _r.u32[1] = _d.u32[(_c>>2)&3];",
                    "  _r.u32[2] = _s.u32[(_c>>4)&3]; _r.u32[3] = _s.u32[(_c>>6)&3];",
                    "  %s = _r; }" % xmm_of(ins, d)]

        if m in ("pshuflw", "pshufhw"):
            if len(ops) != 3:
                raise Unsupported(ins, "%s requires xmm, xmm/m128, imm8" % m)
            d, s, i = ops
            if (d.type != cx.X86_OP_REG or d.reg not in XMMR or
                    opsize(ins, d) != 16 or opsize(ins, s) != 16 or
                    s.type not in (cx.X86_OP_REG, cx.X86_OP_MEM) or
                    i.type != cx.X86_OP_IMM or opsize(ins, i) != 1):
                raise Unsupported(ins, "%s requires xmm, xmm/m128, imm8" % m)
            src = (xmm_of(ins, s) if s.type == cx.X86_OP_REG
                   else "ldx(%s)" % addr_expr(ins, s))
            # The untouched half comes from the source, not the old
            # destination.  Snapshot before writing because every shipped
            # site aliases source and destination and imm8 D8 swaps lanes 1/2.
            if self.sse_lower:
                return [sse_lower.token_pshufw(int(m == "pshufhw"),
                                               self._xmm_index(ins, d),
                                               i.imm & 0xff,
                                               **self._xmm_source(ins, s))]
            if m == "pshuflw":
                lane = "_i"
                selected = "((_c >> (_i * 2)) & 3U)"
            else:
                lane = "4 + _i"
                selected = "4 + ((_c >> (_i * 2)) & 3U)"
            return ["{ xmm_t _s = %s, _r = _s; unsigned _c = %uU; int _i;"
                    % (src, i.imm & 0xff),
                    "  for (_i = 0; _i < 4; _i++) "
                    "_r.u16[%s] = _s.u16[%s];" % (lane, selected),
                    "  %s = _r; }" % xmm_of(ins, d)]

        if m == "packuswb":
            if len(ops) != 2:
                raise Unsupported(ins, "packuswb requires xmm, xmm/m128")
            d, s = ops
            if (d.type != cx.X86_OP_REG or d.reg not in XMMR or
                    opsize(ins, d) != 16 or opsize(ins, s) != 16 or
                    s.type not in (cx.X86_OP_REG, cx.X86_OP_MEM)):
                raise Unsupported(ins, "packuswb requires xmm, xmm/m128")
            src = (xmm_of(ins, s) if s.type == cx.X86_OP_REG
                   else "ldx(%s)" % addr_expr(ins, s))
            # Signed words from the old destination form the low half; signed
            # source words form the high half.  Saturation is unsigned, and
            # snapshots are mandatory for the shipped dest==source forms.
            return ["{ xmm_t _d = %s, _s = %s, _r; int _i;"
                    % (xmm_of(ins, d), src),
                    "  for (_i = 0; _i < 8; _i++) {",
                    "    int32_t _a = (int32_t)_d.i16[_i];",
                    "    int32_t _b = (int32_t)_s.i16[_i];",
                    "    _r.u8[_i] = (uint8_t)(_a < 0 ? 0 : "
                    "(_a > 255 ? 255 : _a));",
                    "    _r.u8[8 + _i] = (uint8_t)(_b < 0 ? 0 : "
                    "(_b > 255 ? 255 : _b));",
                    "  } %s = _r; }" % xmm_of(ins, d)]

        PACK = {"paddw": ("u16", 8, "+"), "psubw": ("u16", 8, "-"),
                "paddb": ("u8", 16, "+"), "psubb": ("u8", 16, "-"),
                "psubd": ("u32", 4, "-"), "paddq": ("u64", 2, "+")}
        if m in PACK:
            fld, n, cop = PACK[m]
            d, s = ops
            src = (xmm_of(ins, s) if s.type == cx.X86_OP_REG
                   else "ldx(%s)" % addr_expr(ins, s))
            if self.sse_lower:
                return [sse_lower.token_laneop(
                    fld, n, cop, self._xmm_index(ins, d),
                    lane0=ins.address in self.xmm_lane0,
                    **self._xmm_source(ins, s))]
            return ["{ xmm_t _s = %s; int _i;" % src,
                    "  for (_i = 0; _i < %d; _i++) %s.%s[_i] %s= _s.%s[_i]; }"
                    % (n, xmm_of(ins, d), fld, cop, fld)]

        if m in ("psraw", "psrad", "psrlw", "psrld", "psllw", "pslld"):
            fld = {"w": "16", "d": "32"}[m[-1]]
            signed = m[2] == "r" and m[3] == "a"
            n = 8 if fld == "16" else 4
            d, s = ops
            cnt = ("%uU" % s.imm if s.type == cx.X86_OP_IMM
                   else "%s.u32[0]" % xmm_of(ins, s))
            if self.sse_lower:
                bits = int(fld)
                sop = ">>" if m[2] == "r" else "<<"
                di = self._xmm_index(ins, d)
                if s.type == cx.X86_OP_IMM:
                    imm = s.imm & 0xff
                    if signed:
                        # A count >= the lane width fills every lane with
                        # its sign bit: exactly the shift by width - 1, which
                        # keeps the vector shift defined.
                        return [sse_lower.token_psra_imm(
                            bits, di, min(imm, bits - 1))]
                    if imm < bits:
                        return [sse_lower.token_pshl_imm(bits, sop, di, imm)]
                    # logical count >= width: the legacy statement zeroes the
                    # register outright (already well defined)
                elif s.type == cx.X86_OP_REG and s.reg in XMMR:
                    # Register counts: the token reads the low qword and
                    # saturates (x86 semantics) instead of the low dword.
                    si = self._xmm_index(ins, s)
                    if signed:
                        return [sse_lower.token_psra_reg(bits, di, si)]
                    return [sse_lower.token_pshl_reg(bits, sop, di, si)]
            if signed:
                ty = "i" + fld
                return ["{ unsigned _n = %s; int _i;" % cnt,
                        "  if (_n > %s) _n = %s;" % (fld, fld),
                        "  for (_i = 0; _i < %d; _i++) %s.%s[_i] >>= _n; }"
                        % (n, xmm_of(ins, d), ty)]
            op = ">>" if m[2] == "r" else "<<"
            return ["{ unsigned _n = %s; int _i;" % cnt,
                    "  if (_n >= %s) { %s = (xmm_t){0}; } else"
                    % (fld, xmm_of(ins, d)),
                    "  for (_i = 0; _i < %d; _i++) %s.u%s[_i] %s= _n; }"
                    % (n, xmm_of(ins, d), fld, op)]

        if m in ("addps", "subps", "mulps", "divps"):
            cop = {"addps": "+", "subps": "-", "mulps": "*", "divps": "/"}[m]
            d, s = ops
            src = (xmm_of(ins, s) if s.type == cx.X86_OP_REG
                   else "ldx(%s)" % addr_expr(ins, s))
            return ["{ xmm_t _s = %s; int _i;" % src,
                    "  for (_i = 0; _i < 4; _i++) %s.f[_i] %s= _s.f[_i]; }"
                    % (xmm_of(ins, d), cop)]

        if m in ("xorps", "xorpd", "pxor", "andps", "andpd", "pand",
                 "orps", "orpd", "por"):
            cop = "^" if "xor" in m else ("&" if ("and" in m) else "|")
            d, s = ops
            if s.type == cx.X86_OP_REG and s.reg == d.reg and cop == "^":
                return ["%s = (xmm_t){0};" % xmm_of(ins, d)]
            src = (xmm_of(ins, s) if s.type == cx.X86_OP_REG
                   else "ldx(%s)" % addr_expr(ins, s))
            if self.sse_lower:
                return [sse_lower.token_laneop(
                    "u32", 4, cop, self._xmm_index(ins, d),
                    lane0=ins.address in self.xmm_lane0,
                    **self._xmm_source(ins, s))]
            return ["{ xmm_t _s = %s; int _i;" % src,
                    "  for (_i = 0; _i < 4; _i++) %s.u32[_i] %s= _s.u32[_i]; }"
                    % (xmm_of(ins, d), cop)]

        if m == "pandn":
            if len(ops) != 2:
                raise Unsupported(ins, "pandn requires xmm, xmm/m128")
            d, s = ops
            if (d.type != cx.X86_OP_REG or d.reg not in XMMR or
                    opsize(ins, d) != 16 or opsize(ins, s) != 16 or
                    s.type not in (cx.X86_OP_REG, cx.X86_OP_MEM)):
                raise Unsupported(ins, "pandn requires xmm, xmm/m128")
            src = (xmm_of(ins, s) if s.type == cx.X86_OP_REG
                   else "ldx(%s)" % addr_expr(ins, s))
            # Intel PANDN reverses the tempting spelling: (~old_dest) & src.
            if self.sse_lower:
                return [sse_lower.token_pandn(self._xmm_index(ins, d),
                                              **self._xmm_source(ins, s))]
            return ["{ xmm_t _d = %s, _s = %s; int _i;"
                    % (xmm_of(ins, d), src),
                    "  for (_i = 0; _i < 4; _i++) %s.u32[_i] = "
                    "(~_d.u32[_i]) & _s.u32[_i]; }" % xmm_of(ins, d)]

        if m == "paddd":
            d, s = ops
            src = (xmm_of(ins, s) if s.type == cx.X86_OP_REG
                   else "ldx(%s)" % addr_expr(ins, s))
            if self.sse_lower:
                return [sse_lower.token_laneop(
                    "u32", 4, "+", self._xmm_index(ins, d),
                    lane0=ins.address in self.xmm_lane0,
                    **self._xmm_source(ins, s))]
            return ["{ xmm_t _s = %s; int _i;" % src,
                    "  for (_i = 0; _i < 4; _i++) %s.u32[_i] += _s.u32[_i]; }"
                    % xmm_of(ins, d)]
        if m in ("pmulld", "pcmpgtd"):
            d, s = ops
            if (d.type != cx.X86_OP_REG or d.reg not in XMMR or
                    opsize(ins, d) != 16 or opsize(ins, s) != 16 or
                    s.type not in (cx.X86_OP_REG, cx.X86_OP_MEM)):
                raise Unsupported(ins, "%s requires xmm, xmm/m128" % m)
            src = (xmm_of(ins, s) if s.type == cx.X86_OP_REG
                   else "ldx(%s)" % addr_expr(ins, s))
            if m == "pmulld":
                # PMULLD keeps each 64-bit product's low 32 bits.  Multiply
                # unsigned snapshots in uint64_t: using int32_t here would
                # make the common overflow cases undefined C behaviour.
                return ["{ xmm_t _d = %s, _s = %s; int _i;"
                        % (xmm_of(ins, d), src),
                        "  for (_i = 0; _i < 4; _i++) %s.u32[_i] = "
                        "(uint32_t)((uint64_t)_d.u32[_i] * "
                        "(uint64_t)_s.u32[_i]); }" % xmm_of(ins, d)]
            # PCMPGTD is a signed per-lane comparison and writes an all-one
            # mask, not the C boolean 1.  Snapshot both operands so d == s is
            # also exact.
            return ["{ xmm_t _d = %s, _s = %s; int _i;"
                    % (xmm_of(ins, d), src),
                    "  for (_i = 0; _i < 4; _i++) %s.u32[_i] = "
                    "(_d.i32[_i] > _s.i32[_i]) ? 0xffffffffU : 0U; }"
                    % xmm_of(ins, d)]
        if m == "punpckldq":
            d, s = ops
            src = (xmm_of(ins, s) if s.type == cx.X86_OP_REG
                   else "ldx(%s)" % addr_expr(ins, s))
            if self.sse_lower:
                return [sse_lower.token_punpckldq(self._xmm_index(ins, d),
                                                  **self._xmm_source(ins, s))]
            return ["{ xmm_t _d = %s, _s = %s, _r;" % (xmm_of(ins, d), src),
                    "  _r.u32[0] = _d.u32[0]; _r.u32[1] = _s.u32[0];",
                    "  _r.u32[2] = _d.u32[1]; _r.u32[3] = _s.u32[1];",
                    "  %s = _r; }" % xmm_of(ins, d)]
        if m == "psrldq":
            d, s = ops
            if (self.sse_lower and s.type == cx.X86_OP_IMM and
                    1 <= s.imm <= 15):
                return [sse_lower.token_psrldq(self._xmm_index(ins, d), s.imm)]
            return ["{ xmm_t _r = (xmm_t){0}; unsigned _n = %sU, _i;" % s.imm,
                    "  for (_i = 0; _i + _n < 16; _i++) _r.u8[_i] = %s.u8[_i + _n];"
                    % xmm_of(ins, d),
                    "  %s = _r; }" % xmm_of(ins, d)]
        return None

    # -- x87 ---------------------------------------------------------------
    def x87(self, ins, m, ops):
        if m == "fld":
            o = ops[0]
            if o.type == cx.X86_OP_MEM:
                if o.size == 4:
                    return ["fpush(c, (double)ldf(%s));" % addr_expr(ins, o)]
                if o.size == 8:
                    return ["fpush(c, ldd(%s));" % addr_expr(ins, o)]
                raise Unsupported(ins, "fld width %d" % o.size)
            if o.reg in ST:
                return ["fpush(c, *fst(c, %d));" % ST[o.reg]]
        if m == "fild":
            o = ops[0]
            if o.size == 4:
                return ["fpush(c, (double)(int32_t)ld32(%s));" % addr_expr(ins, o)]
            if o.size == 8:
                return ["fpush(c, (double)(int64_t)ld64(%s));" % addr_expr(ins, o)]
        if m in ("fstp", "fst"):
            o = ops[0]
            pop = "fpop(c)" if m == "fstp" else "(*fst(c, 0))"
            if o.type == cx.X86_OP_MEM:
                if o.size == 4:
                    return ["stf(%s, (float)%s);" % (addr_expr(ins, o), pop)]
                if o.size == 8:
                    return ["std_(%s, %s);" % (addr_expr(ins, o), pop)]
                if o.size == 10 and m == "fstp":
                    # MSVC long double is binary64, not the x87 ten-byte
                    # memory format. Snapshot both operands, complete the
                    # checked store, and only then pop: a memory fault must
                    # leave the architectural x87 stack untouched.
                    return ["{ uint32_t _a = %s;" % addr_expr(ins, o),
                            "  double _v = *fst(c, 0);",
                            "  st80d(_a, _v);",
                            "  (void)fpop(c); }"]
            if o.reg in ST:
                i = ST[o.reg]
                if m == "fst":
                    return ["*fst(c, %d) = *fst(c, 0);" % i]
                if i == 0:
                    # FSTP ST(0) has no store effect; it only pops.  Writing
                    # `*fst(c,0) = fpop(c)` both reads and changes st_top in
                    # one unsequenced expression and may overwrite new ST(0).
                    return ["(void)fpop(c);"]
                # ST(i) is named relative to the OLD top.  Snapshot both the
                # physical destination and value before fpop changes st_top,
                # then perform the store and pop as separate operations.
                return ["{ int _di = (c->st_top + %d) & 7;" % i,
                        "  double _v = *fst(c, 0);",
                        "  c->st[_di] = _v;",
                        "  (void)fpop(c); }"]
        if m == "frndint":
            # The current x87 model deliberately fixes the control word to
            # round-to-nearest/even; nearbyint is already the corresponding
            # operation used by FIST/FISTP and preserves signed zero/NaN.
            return ["*fst(c, 0) = nearbyint(*fst(c, 0));"]
        if m in ("fadd", "fsub", "fmul", "fdiv"):
            cop = {"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/"}[m]
            o = ops[0]
            if o.type == cx.X86_OP_MEM:
                ld = "ldf" if o.size == 4 else "ldd"
                return ["*fst(c, 0) %s= (double)%s(%s);"
                        % (cop, ld, addr_expr(ins, o))]
        if m == "fchs":
            return ["*fst(c, 0) = -*fst(c, 0);"]
        if m == "fabs":
            return ["*fst(c, 0) = fabs(*fst(c, 0));"]
        if m == "fsqrt":
            return ["*fst(c, 0) = sqrt(*fst(c, 0));"]

        # ---- constant loads ---------------------------------------------
        K = {"fldz": "0.0", "fld1": "1.0",
             "fldpi": "3.14159265358979323846",
             "fldl2e": "1.44269504088896340736",
             "fldl2t": "3.32192809488736234787",
             "fldlg2": "0.30102999566398119521",
             "fldln2": "0.69314718055994530942"}
        if m in K:
            return ["fpush(c, %s);" % K[m]]

        # ---- store integer ------------------------------------------------
        if m in ("fistp", "fist", "fisttp"):
            o = ops[0]
            val = "fpop(c)" if m in ("fistp", "fisttp") else "(*fst(c, 0))"
            # `fist`/`fistp` round to the current mode (nearest-even by
            # default); only `fisttp` truncates. Using a C cast for both would
            # truncate always, which is a silent off-by-one on .5 boundaries.
            conv = "(int64_t)(%s)" % val if m == "fisttp" \
                else "(int64_t)nearbyint(%s)" % val
            if o.size == 2:
                return ["st16(%s, (uint16_t)(int16_t)%s);"
                        % (addr_expr(ins, o), conv)]
            if o.size == 4:
                return ["st32(%s, (uint32_t)(int32_t)%s);"
                        % (addr_expr(ins, o), conv)]
            if o.size == 8:
                return ["st64(%s, (uint64_t)%s);" % (addr_expr(ins, o), conv)]
            raise Unsupported(ins, "%s width %d" % (m, o.size))

        # ---- exchange, and the status word --------------------------------
        if m == "fxch":
            i = ST.get(ops[0].reg, 1) if ops else 1
            return ["{ double _t = *fst(c, 0);",
                    "  *fst(c, 0) = *fst(c, %d); *fst(c, %d) = _t; }" % (i, i)]
        if m in ("fnstsw", "fstsw"):
            o = ops[0]
            if o.type == cx.X86_OP_REG:
                return ["GR(eax) = (GR(eax) & 0xFFFF0000U) | c->fsw;"]
            return ["st16(%s, c->fsw);" % addr_expr(ins, o)]
        if m in ("fnstcw", "fstcw"):
            # The control word is read and restored around float-to-int
            # conversions. Nothing here changes rounding, so a fixed value is
            # honest as long as nobody acts on it -- 0x027F is the Win32
            # default (round to nearest, double-extended precision).
            return ["st16(%s, 0x027FU);" % addr_expr(ins, ops[0])]
        if m in ("fldcw", "fnclex", "fclex", "fwait", "wait", "fninit"):
            return ["/* %s: rounding mode is fixed in this model */" % m]

        # ---- compares -----------------------------------------------------
        if m in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp",
                 "ficom", "ficomp"):
            if ops and ops[0].type == cx.X86_OP_MEM:
                o = ops[0]
                if m.startswith("fi"):
                    src = ("(double)(int%d_t)ld%d(%s)"
                           % (o.size * 8, o.size * 8, addr_expr(ins, o)))
                else:
                    src = "(double)%s(%s)" % ("ldf" if o.size == 4 else "ldd",
                                              addr_expr(ins, o))
            elif ops:
                src = "*fst(c, %d)" % ST.get(ops[0].reg, 1)
            else:
                src = "*fst(c, 1)"
            out = ["fcmp_set(c, *fst(c, 0), %s);" % src]
            if m.endswith("pp"):
                out.append("fpop(c); fpop(c);")
            elif m.endswith("p"):
                out.append("fpop(c);")
            return out
        if m in ("fcomi", "fcomip", "fucomi", "fucomip"):
            # These write EFLAGS directly: ZF PF CF, with OF/SF/AF cleared.
            i = ST.get(ops[-1].reg, 1) if ops else 1
            out = ["{ double _a = *fst(c, 0), _b = *fst(c, %d);" % i,
                   "  GUEST_FL->f_op = FLAG_EXPLICIT; GUEST_FL->f_sz = 4; GUEST_FL->f_of = 0;",
                   "  if (_a != _a || _b != _b) { GUEST_FL->f_cf = 1; GUEST_FL->f_r = 0U; }",
                   "  else { GUEST_FL->f_cf = (_a < _b); GUEST_FL->f_r = (_a == _b) ? 0U : 1U; }",
                   "}"]
            if m.endswith("p"):
                out.append("fpop(c);")
            return out

        # ---- arithmetic, including the reversed and popping forms ----------
        BASE = {"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/",
                "fsubr": "-", "fdivr": "/",
                "faddp": "+", "fsubp": "-", "fmulp": "*", "fdivp": "/",
                "fsubrp": "-", "fdivrp": "/",
                "fiadd": "+", "fisub": "-", "fimul": "*", "fidiv": "/",
                "fisubr": "-", "fidivr": "/"}
        if m in BASE:
            cop = BASE[m]
            rev = "r" in m[1:].replace("p", "")[-1:] or m in (
                "fsubr", "fdivr", "fsubrp", "fdivrp", "fisubr", "fidivr")
            pop = m.endswith("p")
            o = ops[0] if ops else None
            if o is not None and o.type == cx.X86_OP_MEM:
                if m.startswith("fi"):
                    src = ("(double)(int%d_t)ld%d(%s)"
                           % (o.size * 8, o.size * 8, addr_expr(ins, o)))
                else:
                    src = "(double)%s(%s)" % ("ldf" if o.size == 4 else "ldd",
                                              addr_expr(ins, o))
                lhs = "*fst(c, 0)"
                expr = ("%s %s %s" % (src, cop, lhs) if rev
                        else "%s %s %s" % (lhs, cop, src))
                return ["*fst(c, 0) = %s;" % expr]
            # register forms: dest is operand 0, source operand 1
            di = ST.get(ops[0].reg, 0) if ops else 1
            si = ST.get(ops[1].reg, 0) if len(ops) > 1 else 0
            if not ops:
                di, si = 1, 0
            d, s = "*fst(c, %d)" % di, "*fst(c, %d)" % si
            expr = "%s %s %s" % (s, cop, d) if rev else "%s %s %s" % (d, cop, s)
            out = ["*fst(c, %d) = %s;" % (di, expr)]
            if pop:
                out.append("fpop(c);")
            return out
        return None
