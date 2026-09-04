"""Tiny leaf callees inlined at direct call sites (GUEST_LEAF_INLINE).

A direct `call sub_X` is emitted as

    GPUSH(ret); GUEST_STACK_CALLSITE_BARRIER(); GUEST_GPR_FLUSH(c);
    sub_X(c); GUEST_GPR_RELOAD(c);

For a callee this module proves to be a tiny straight-line leaf, the site
carries the callee's own statements instead of the C call -- the return word
is still pushed by the caller and popped by the copied `ret`, so the guest
stack bytes are identical and the only things that disappear are the C call,
the callee's prologue/epilogue and, in GPR-locals mode, the FLUSH/RELOAD
boundary around the call.  The out-of-line body stays for indirect callers
and the dispatch table.

The site is dual-spelled behind a header knob so a device A/B can bisect by
relink without regenerating:

    GPUSH(ret); GUEST_STACK_CALLSITE_BARRIER();
    #if GUEST_LEAF_INLINE
        { <callee statements, (void)GPOP() kept> }
    #else
        GUEST_GPR_FLUSH(c); sub_X(c); GUEST_GPR_RELOAD(c);
    #endif

and the whole block is ONE emitter statement (with embedded newlines), so
every gen_all pin, seam renderer and normaliser that looks at
`statements[0]` or `len(statements)` keeps working; gpr_locals.legacy_spelling
collapses the block back to the exact pre-inlining statement, which keeps the
seam pins, the stack-lowering census and the unit packing measure stable.

Eligibility is decided from the decoded x86 CFG (trans.decode / blocks /
flag_liveness) and from the emitted statements of the callee, never from
rendered text.  Two shapes are accepted:

  * `leaf`: one basic block of at most MAX instructions ending in `ret`
    (`ret n` allowed), nothing in it transfers control, faults, touches
    x87/fs/gs, is a string operation, names ESP as a value, uses ESP with an
    index register, or reads/writes the return-address slot (tracked through
    the leaf's own push/pop depth); the entry block consumes no flags it did
    not produce.  Flag effects of the copied statements land in the caller's
    flag state, so a site is refused when the caller could consume a flag
    before rewriting all of them (x86 code never does; the check is cheap).

  * `guard`: the entry block is exactly `cmp/test ; jcc` with a direct
    (paired) condition and one of the two successors is a lone `ret`.
    The site tests the same condition, pops the return word on the ret path
    and calls the out-of-line body otherwise -- the body re-evaluates the
    side-effect-free compare and takes the same edge.  This is the shape of
    MSVC's __security_check_cookie (3,7k sites).

Everything else fails closed to the ordinary call, with the reason counted in
the census.  A callee whose rendered body differs from the plain emitter
output (a gen_all host seam), a --wrap'ed symbol, a manual runtime boundary,
a supplemental (address-taken) root, a switch owner and any body with an
unsupported instruction are never inlined.

The copied statements are the emitter's, so they carry whatever spelling the
generation switches select -- GPR tokens, and with GUEST_SSE_LOWER the
GUEST_XMM_* tokens of sse_lower.py -- exactly as the out-of-line body does
(test_leaf_inline_corpus_contract.py compares the two line for line).  No IAT
site is ever copied (call/jmp are denied), so GUEST_IMPORT_ stays forbidden.
"""
from __future__ import annotations

import collections
import json
import os
import re

from capstone import x86 as cx

import sse_lower
import trans as T

ENVIRONMENT_SWITCH = "GUEST_LEAF_INLINE"
ENVIRONMENT_MAX_INSNS = "GUEST_LEAF_INLINE_MAX_INSNS"
DEFAULT_MAX_INSNS = 8
# Byte extent (from the boundary analysis) above which a function is not
# even decoded: eight x86 instructions are at most 120 bytes.
EXTENT_LIMIT = 256

PP_IF = "#if GUEST_LEAF_INLINE"
PP_ELSE = "#else"
PP_ENDIF = "#endif"

KIND_LEAF = "leaf"
KIND_GUARD = "guard"

# Worker-side table: None disables inlining; otherwise
# {"specs": {rva: spec}, "reasons": {rva: reason}, "max_insns": int}.
TABLE = None

_ALL_FLAGS = frozenset("CZSOP")
_ESP_REGS = frozenset((cx.X86_REG_ESP, cx.X86_REG_SP))

# Mnemonics (prefix stripped) that end a leaf candidacy outright.  Control
# transfers, traps, capability probes, flag-as-value forms, direction flag,
# frame helpers, the division fault edge, string/IO operations and the partial
# flag writers whose CF handling reads the previous state.
DENIED_MNEMONICS = frozenset((
    "call", "jmp", "ret", "retf", "iret", "iretd", "int3", "int", "int1",
    "into", "hlt", "ud2", "cpuid", "xgetbv", "rdtsc", "rdtscp", "rdpmc",
    "syscall", "sysenter", "pushf", "pushfd", "popf", "popfd", "lahf",
    "sahf", "cld", "std", "leave", "enter", "bound", "in", "out", "xlat",
    "xlatb", "loop", "loope", "loopne", "loopz", "loopnz", "jecxz", "jcxz",
    "cmc", "clc", "stc", "div", "idiv", "emms", "wait", "fwait", "ldmxcsr",
    "stmxcsr", "pause", "lfence", "mfence", "sfence", "cli", "sti", "arpl",
    "lar", "lsl", "verr", "verw", "sgdt", "sidt", "lgdt", "lidt", "smsw",
    "lmsw", "invd", "wbinvd", "invlpg", "rsm", "rdmsr", "wrmsr", "ud0", "ud1",
    "aaa", "aad", "aam", "aas", "daa", "das", "salc", "icebp",
))
_STRING_OPS = frozenset((
    "movsb", "movsw", "movsd", "movsq", "stosb", "stosw", "stosd", "lodsb",
    "lodsw", "lodsd", "scasb", "scasw", "scasd", "cmpsb", "cmpsw", "cmpsd",
    "insb", "insw", "insd", "outsb", "outsw", "outsd",
))
_PREFIXES = ("notrack ", "lock ", "rep ", "repe ", "repne ", "repz ",
             "repnz ", "bnd ")

# Identifiers a copied statement may call.  Everything else -- a host helper,
# a fault, a translated function -- fails the candidate closed.  The SSE
# lowering tokens (guest.h, sse_lower.py) are the emitter's own spelling of
# packed ops, 16-byte moves and paired comis: the callee's statements come
# from the same emitter under the same generation switch as its out-of-line
# body, so the copy carries the very tokens the body does, and the header
# knob expands both the same way.  Lane-0 forms are safe to copy: the taint
# analysis pins every lane-0 result still live at the callee's `ret`, so a
# surviving lane-0 site is overwritten by the copied instructions themselves.
_ALLOWED_CALLS = frozenset((
    "ld8", "ld16", "ld32", "ld64", "ldf", "ldd", "ldx",
    "st8", "st16", "st32", "st64", "stf", "std_", "stx",
    "SET_FLAGS", "GR", "GPUSH", "GPOP", "GSTACK_ADDR",
    "GUEST_STACK_CALLSITE_BARRIER", "sqrt", "sqrtf", "fabs", "fabsf",
    "nearbyint", "nearbyintf", "UINT64_C", "guest_atomic_cmpxchg64",
    "__sync_lock_test_and_set", "__sync_lock_release", "sizeof",
    "if", "while", "for", "do", "else", "switch", "case",
    *sse_lower.TOKEN_NAMES,
))
_CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
_CC_FL_RE = re.compile(r"^(?:cc|fl)_[a-z]+\Z")
_FORBIDDEN_TEXT = (
    "return", "goto", "GUEST_GPR_", "GUEST_FLAGS_LOAD", "GUEST_FLAGS_FLUSH",
    "GUEST_IMPORT_", "guest_", "c->df", "c->st", "c->fsw", "c->fcw", "c->mxcsr", "c->fault",
    "c->fl", "c->eax", "c->ecx", "c->edx", "c->ebx", "c->esp", "c->ebp",
    "c->esi", "c->edi", "c->r[", "GESP_SET(", "GESP_ADJ(", "#", "L_",
)
_JCC_RE = re.compile(r"^if \((.*)\) goto L_([0-9a-f]{8});\Z")
_RET_PLAIN = ("(void)GPOP();", "GUEST_STACK_CALLSITE_BARRIER();",
              "GUEST_GPR_FLUSH(c); return;")
_RET_IMM_RE = re.compile(r"^if \(!GESP_ADJ\((\d+)U\)\) return;\Z")
_WRAP_RE = re.compile(r"--wrap=(sub_[0-9a-f]{8})\b")


def enabled_by_environment() -> bool:
    return os.environ.get(ENVIRONMENT_SWITCH, "0") == "1"


def max_insns_from_environment() -> int:
    raw = os.environ.get(ENVIRONMENT_MAX_INSNS)
    if raw is None:
        return DEFAULT_MAX_INSNS
    value = int(raw)
    if not 1 <= value <= 32:
        raise RuntimeError("%s must be between 1 and 32" % ENVIRONMENT_MAX_INSNS)
    return value


def wrapped_symbols(cmake_lists_path: str) -> frozenset:
    """RVAs of translated symbols GNU ld --wrap replaces at link time: their
    callers must keep calling the symbol (the wrapper is the host)."""
    with open(cmake_lists_path, "r", encoding="utf-8") as handle:
        text = handle.read()
    return frozenset(int(name[4:], 16) for name in _WRAP_RE.findall(text))


# ------------------------------------------------------------ predicate ---

def _split_prefix(mnemonic):
    rep = lock = False
    for prefix in _PREFIXES:
        if mnemonic.startswith(prefix):
            if prefix.startswith("rep"):
                rep = True
            elif prefix == "lock ":
                lock = True
            mnemonic = mnemonic[len(prefix):]
            break
    return rep, lock, mnemonic


def _has_xmm_operand(ins):
    return any(op.type == cx.X86_OP_REG and op.reg in T.XMMR
               for op in ins.operands)


def _operand_reason(ins, mnemonic, depth):
    """Why this instruction's operands disqualify a leaf, or None."""
    for op in ins.operands:
        if op.type == cx.X86_OP_REG:
            if op.reg in _ESP_REGS:
                return "esp named as an operand"
        elif op.type == cx.X86_OP_MEM:
            m = op.mem
            if m.segment in (cx.X86_REG_FS, cx.X86_REG_GS):
                return "fs/gs segment access"
            if m.index in _ESP_REGS:
                return "esp as an index register"
            if m.base in _ESP_REGS:
                if mnemonic == "lea":
                    return "esp used as a value (lea)"
                if mnemonic in ("push", "pop"):
                    return "esp-relative push/pop operand"
                if m.index not in (0, cx.X86_REG_INVALID):
                    return "esp-relative access with an index register"
                size = op.size or 4
                offset = m.disp - depth
                if offset < 4 and offset + size > 0:
                    return "reads or writes the return-address slot"
    return None


def _instruction_reason(ins, depth):
    """(reason or None, depth after the instruction) for a leaf body member."""
    rep, _lock, m = _split_prefix(ins.mnemonic)
    if rep:
        return "rep-prefixed string operation", depth
    if m in _STRING_OPS and not _has_xmm_operand(ins):
        return "string operation", depth
    if m.startswith("f"):
        return "x87 instruction", depth
    if m in DENIED_MNEMONICS:
        return "denied mnemonic %s" % m, depth
    if m.startswith("j"):
        return "control transfer", depth
    reason = _operand_reason(ins, m, depth)
    if reason:
        return reason, depth
    if m == "push":
        depth += 4
    elif m == "pop":
        depth -= 4
        if depth < 0:
            return "pops the return address", depth
    return None, depth


def _statement_reason(statements):
    """Textual belt-and-braces over the emitter's output for the copied
    instructions: no boundary, no host helper, no CPU-field register."""
    for statement in statements:
        for needle in _FORBIDDEN_TEXT:
            if needle in statement:
                return "statement carries %r" % needle
        for name in _CALL_RE.findall(statement):
            if name in _ALLOWED_CALLS or _CC_FL_RE.match(name):
                continue
            return "statement calls %s" % name
    return None


def _ret_inline_statements(statements):
    """The copied form of a translated `ret` / `ret n`: pop the return word
    (the caller pushed it), drop the callee-cleaned arguments, and continue
    in the caller instead of returning.  None when the shape is not the
    emitter's exact ret rendering."""
    statements = tuple(statements)
    if statements == _RET_PLAIN:
        return ("(void)GPOP();", "GUEST_STACK_CALLSITE_BARRIER();")
    if len(statements) == 4 and statements[0] == _RET_PLAIN[0] and \
            statements[2:] == _RET_PLAIN[1:]:
        match = _RET_IMM_RE.match(statements[1])
        if match:
            # A rejected adjust has recorded its fault and published the
            # registers; the out-of-line callee then returned into the
            # caller's RELOAD of the very same values, so continuing is the
            # same state.
            return ("(void)GPOP();", "(void)GESP_ADJ(%sU);" % match.group(1),
                    "GUEST_STACK_CALLSITE_BARRIER();")
    return None


def _sse_touches_flags(statements):
    """A paired comis is spelled GUEST_XMM_COMIS_PAIRED / GUEST_XMM_FCC without
    naming GUEST_FL, yet under the header knob GUEST_SSE_LOWER=0 the pair is
    the lazy FLAG_PARTIAL producer and the cc_* consumer: judged on the legacy
    text, as the switch-off corpus was, so the caller-side flag check
    (_flags_dead_after) fails closed under both knobs."""
    return any("GUEST_FL" in sse_lower.legacy_sse_tokens(statement)
               for statement in statements if "GUEST_XMM_" in statement)


def _comment(ins):
    return "/* %08x  %s %s */" % (ins.address, ins.mnemonic, ins.op_str)


def analyse(img, rva, known, max_insns=DEFAULT_MAX_INSNS, switch_owner=False,
            import_slots=None):
    """Decide from the decoded CFG and the emitted statements whether the
    function at `rva` may be copied into its direct callers.

    Returns (spec, None) or (None, reason).  `spec` is a picklable dict.

    `import_slots` is gen_all's IAT slot table (build_one.translate): the
    candidate is rendered exactly as the corpus renders it.  No IAT site is
    ever copied -- `call`/`jmp` are denied mnemonics and a `jmp [slot]`
    thunk does not end in `ret` -- so the table only matters for the guard
    kind, whose out-of-line body may carry GUEST_IMPORT_CALL/JMP sites.
    """
    import build_one as B                      # lazy: emit imports this module

    if switch_owner:
        return None, "switch owner"
    try:
        insns, order, indirect = T.decode(img, rva)
    except Exception as ex:                                  # noqa: BLE001
        return None, "decode failed (%s)" % type(ex).__name__
    if indirect:
        return None, "indirect transfer"
    if not order or order[0] != rva:
        return None, "entry is not the lowest decoded address"
    _leaders, block_of, members, succs = T.blocks(insns, order, rva)
    live_in, _live_out = T.flag_liveness(insns, members, succs)
    if live_in.get(rva):
        return None, "entry block consumes flags"
    entry = list(members[rva])
    single_block = len(members) == 1
    if single_block and len(order) > max_insns:
        return None, "more than %d instructions" % max_insns
    if not single_block:
        guard_shape = _guard_shape(insns, members, entry)
        if guard_shape is None:
            return None, "multiple blocks"
    try:
        em, _insns, _order, body, unsup, _indirect, _members = B.translate(
            img, rva, "sub_%08x" % rva, known, import_slots=import_slots)
    except Exception as ex:                                  # noqa: BLE001
        return None, "translate failed (%s)" % type(ex).__name__
    if unsup:
        return None, "unsupported instruction in body"
    if em.bad_targets:
        return None, "body references an unknown function"
    if getattr(em, "entry_flags_live", False):
        return None, "entry block consumes flags"
    if single_block:
        return _leaf_spec(rva, insns, order, body, max_insns)
    return _guard_spec(rva, insns, body, guard_shape, em)


def _leaf_spec(rva, insns, order, body, max_insns):
    if insns[order[-1]].mnemonic != "ret":
        return None, "single block does not end in ret"
    depth = 0
    for address in order[:-1]:
        reason, depth = _instruction_reason(insns[address], depth)
        if reason:
            return None, reason
    if depth != 0:
        return None, "unbalanced push/pop at ret"
    lines = []
    copied = []
    for ins, statements in body:
        lines.append(_comment(ins))
        if ins.address == order[-1]:
            replacement = _ret_inline_statements(statements)
            if replacement is None:
                return None, "ret rendering shape changed"
            lines.extend(replacement)
        else:
            copied.extend(statements)
            lines.extend(statements)
    reason = _statement_reason(copied)
    if reason:
        return None, reason
    touches_flags = any("GUEST_FL" in s for s in copied) or _sse_touches_flags(copied)
    return {
        "rva": rva, "kind": KIND_LEAF, "insns": len(order),
        "lines": tuple(lines), "touches_flags": touches_flags,
    }, None


def _guard_shape(insns, members, entry):
    """(producer, jcc, ret_rva, ret_is_target) for `cmp/test; jcc` whose
    one successor is a lone `ret`, else None."""
    if len(entry) != 2:
        return None
    producer, jcc = insns[entry[0]], insns[entry[1]]
    rep, lock, pm = _split_prefix(producer.mnemonic)
    if rep or lock or pm not in ("cmp", "test"):
        return None
    jm = jcc.mnemonic
    if not (jm.startswith("j") and jm != "jmp" and jcc.operands and
            jcc.operands[0].type == cx.X86_OP_IMM):
        return None
    if _operand_reason(producer, pm, 0) is not None:
        return None
    target = jcc.operands[0].imm
    fallthrough = jcc.address + jcc.size
    for candidate, is_target in ((target, True), (fallthrough, False)):
        block = members.get(candidate)
        if block and list(block) == [candidate] and \
                insns[candidate].mnemonic == "ret":
            return producer, jcc, candidate, is_target
    return None


def _guard_spec(rva, insns, body, shape, em):
    producer, jcc, ret_rva, ret_is_target = shape
    by_address = {ins.address: tuple(statements) for ins, statements in body}
    if by_address.get(producer.address) != ():
        return None, "guard compare not paired with its branch"
    jcc_statements = by_address.get(jcc.address, ())
    if len(jcc_statements) != 1:
        return None, "guard branch rendering shape changed"
    match = _JCC_RE.match(jcc_statements[0])
    if not match or int(match.group(2), 16) != jcc.operands[0].imm:
        return None, "guard branch is not a direct condition"
    condition = match.group(1)
    reason = _statement_reason((condition,))
    if reason:
        return None, reason
    ret_lines = _ret_inline_statements(by_address.get(ret_rva, ()))
    if ret_lines is None:
        return None, "ret rendering shape changed"
    return {
        "rva": rva, "kind": KIND_GUARD, "insns": 3,
        "condition": condition, "ret_is_target": ret_is_target,
        "ret_lines": ret_lines, "touches_flags": False,
        "comments": (_comment(producer), _comment(jcc),
                     _comment(insns[ret_rva])),
    }, None


# ------------------------------------------------------------ discovery ---

def _plain_body_text(name, rva, em, order, body, function_coverage_ids):
    """What _translate_function renders for a body no host seam touches."""
    lines = [
        "/* %s  RVA %08x  %d insns, %d switch entries */"
        % (name, rva, len(order), 0),
        "void %s(CPU *__restrict c)" % name,
        "{",
        "    GUEST_FLAGS_DECL;",
        "    GUEST_GPR_DECL;",
    ]
    if getattr(em, "entry_flags_live", False):
        lines.append("    GUEST_FLAGS_LOAD(c);")
    if function_coverage_ids is not None:
        lines.append("    guest_coverage_function(%dU);"
                     % function_coverage_ids[rva])
    for ins, statements in body:
        if ins.address in em.labels:
            lines.append("L_%08x:" % ins.address)
            if (function_coverage_ids is not None and ins.address != rva and
                    ins.address in function_coverage_ids):
                lines.append("    guest_coverage_function(%dU);"
                             % function_coverage_ids[ins.address])
        lines.append("    " + _comment(ins))
        lines.extend("    %s" % statement for statement in statements)
    lines.append("}")
    return "\n".join(lines) + "\n"


def discover(img, functions, known, switch_info, coverage, pin_img,
             translate_function, wrapped, manual_rvas, excluded_roots,
             max_insns=DEFAULT_MAX_INSNS, log=print, import_slots=None):
    """Build the worker table over the canonical roots.

    `translate_function` is gen_all._translate_function: a candidate is kept
    only when its fully rendered body is byte-identical to the plain emitter
    rendering, i.e. no host seam, hook or fence owns any part of it.  The
    plain rendering is taken with the same `import_slots` the workers use
    (gen_all's IAT table), so a GUEST_IMPORT_CALL/JMP site in a guard's
    out-of-line body compares equal instead of looking like a seam.
    """
    import build_one as B

    specs = {}
    reasons = {}
    seam_pinned = []
    function_ids = coverage["function_ids"] if coverage else None
    case_ids = coverage["case_ids"] if coverage else None
    for fn in functions:
        rva = fn["rva"]
        if rva in wrapped:
            reasons[rva] = "--wrap symbol"
            continue
        if rva in manual_rvas:
            reasons[rva] = "manual runtime boundary"
            continue
        if rva in excluded_roots:
            reasons[rva] = "address-taken supplemental root"
            continue
        if fn["end"] - rva > EXTENT_LIMIT:
            reasons[rva] = "not tiny (extent > %d bytes)" % EXTENT_LIMIT
            continue
        spec, reason = analyse(img, rva, known, max_insns,
                               switch_owner=rva in switch_info,
                               import_slots=import_slots)
        if spec is None:
            reasons[rva] = reason
            continue
        record = translate_function(img, fn, known, switch_info,
                                    function_ids, case_ids, pin_img)
        if record.get("stub") is not None or record.get("manual"):
            reasons[rva] = "stub or manual body"
            continue
        name = "sub_%08x" % rva
        em, _insns, order, body, unsup, _indirect, _members = B.translate(
            img, rva, name, known, import_slots=import_slots)
        if unsup:
            reasons[rva] = "unsupported instruction in body"
            continue
        if record["text"] != _plain_body_text(name, rva, em, order, body,
                                              function_ids):
            reasons[rva] = "seam-pinned body"
            seam_pinned.append(rva)
            continue
        specs[rva] = spec
    kinds = collections.Counter(spec["kind"] for spec in specs.values())
    log("leaf inline candidates      : %d eligible (%d leaf, %d guard), "
        "%d seam-pinned excluded, max %d instructions"
        % (len(specs), kinds[KIND_LEAF], kinds[KIND_GUARD], len(seam_pinned),
           max_insns))
    return {"specs": specs, "reasons": reasons, "max_insns": max_insns}


# ------------------------------------------------------------- emission ---

def _flags_dead_after(em, ins, touches_flags):
    """True when nothing the caller does after the call can read a flag the
    copied statements may have changed: every flag is rewritten before any
    read in the rest of the block, or dead at the block's exit."""
    if not touches_flags:
        return True
    written = set()
    block = em.block_of.get(ins.address)
    address = ins.address + ins.size
    while address in em.insns and em.block_of.get(address) == block:
        following = em.insns[address]
        if T.flags_read(following) - written:
            return False
        written |= T.flags_written(following)
        if written >= _ALL_FLAGS:
            return True
        address += following.size
    return not (set(em.live_out.get(block, ())) - written)


def render_site(return_rva, callee_rva, spec):
    """The dual-spelled direct-call statement (one string, embedded newlines)."""
    head = "GPUSH(0x%xU); GUEST_STACK_CALLSITE_BARRIER();" % return_rva
    call = ("GUEST_GPR_FLUSH(c); sub_%08x(c); GUEST_GPR_RELOAD(c);"
            % callee_rva)
    if spec["kind"] == KIND_LEAF:
        inner = ["    { /* GUEST_LEAF_INLINE: sub_%08x, %d instructions */"
                 % (callee_rva, spec["insns"])]
        inner.extend("      " + line for line in spec["lines"])
        inner.append("    }")
    else:
        fast = " ".join(spec["ret_lines"])
        inner = ["    /* GUEST_LEAF_INLINE guard: sub_%08x */" % callee_rva]
        inner.extend("    " + comment for comment in spec["comments"])
        if spec["ret_is_target"]:
            inner.append("    if (%s) { %s } else { %s }"
                         % (spec["condition"], fast, call))
        else:
            inner.append("    if (%s) { %s } else { %s }"
                         % (spec["condition"], call, fast))
    return "\n".join([head, PP_IF] + inner + [PP_ELSE, "    " + call, PP_ENDIF])


def site_statement(em, ins, callee_rva):
    """Called by the emitter for a direct `call imm` to a known function.

    Returns the dual-spelled statement, or None when the site keeps the
    ordinary call; the decision is counted on the emitter for the census.
    """
    if TABLE is None:
        return None
    inlined = getattr(em, "leaf_inlined", None)
    if inlined is None:
        inlined = em.leaf_inlined = []
        em.leaf_refused = collections.Counter()
    spec = TABLE["specs"].get(callee_rva)
    if spec is None:
        em.leaf_refused[TABLE["reasons"].get(
            callee_rva, "callee is not a canonical root")] += 1
        return None
    if not _flags_dead_after(em, ins, spec["touches_flags"]):
        em.leaf_refused["caller may consume the callee's flags"] += 1
        return None
    inlined.append((ins.address, callee_rva, spec["kind"]))
    return render_site(ins.address + ins.size, callee_rva, spec)


# --------------------------------------------------------------- census ---

def census_report(table, site_records, refused_totals, code_size_delta,
                  path=None, log=print):
    """Print (and optionally write) the generation census: eligible leaves,
    sites inlined per callee, sites left with reasons, text delta."""
    per_callee = collections.Counter()
    per_kind = collections.Counter()
    for _caller, _site, callee, kind in site_records:
        per_callee[callee] += 1
        per_kind[kind] += 1
    eligible = table["specs"] if table else {}
    unused = sorted(set(eligible) - set(per_callee))
    log("leaf inline sites           : %d inlined (%d leaf, %d guard) at "
        "%d callees; %d eligible callees without a direct site; "
        "%d sites kept as calls; text %+d bytes"
        % (len(site_records), per_kind[KIND_LEAF], per_kind[KIND_GUARD],
           len(per_callee), len(unused), sum(refused_totals.values()),
           code_size_delta))
    for reason, count in sorted(refused_totals.items(),
                                key=lambda item: (-item[1], item[0]))[:12]:
        log("   kept: %7d  %s" % (count, reason))
    for callee, count in per_callee.most_common(12):
        spec = eligible[callee]
        log("   inlined: %5d  sub_%08x (%s, %d insns)"
            % (count, callee, spec["kind"], spec["insns"]))
    if path is not None:
        report = {
            "max_insns": table["max_insns"] if table else None,
            "eligible": {
                "%08x" % rva: {"kind": spec["kind"], "insns": spec["insns"],
                               "sites": per_callee.get(rva, 0)}
                for rva, spec in sorted(eligible.items())
            },
            "sites": [
                {"caller": "%08x" % caller, "site": "%08x" % site,
                 "callee": "%08x" % callee, "kind": kind}
                for caller, site, callee, kind in site_records
            ],
            "kept_by_reason": dict(sorted(refused_totals.items())),
            "ineligible_by_reason": dict(collections.Counter(
                table["reasons"].values())) if table else {},
            "code_size_delta": code_size_delta,
        }
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=1, sort_keys=True)
            handle.write("\n")
