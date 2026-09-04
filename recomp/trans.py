"""x86-32 -> C translator core.

This replaces the two ladder spikes (`spike/recomp_one.py`, `spike/recomp2.py`)
with something built to carry the whole binary rather than one function. What
is different, and why:

  * **Recursive descent, not a linear run to the first `ret`.** A function's
    extent is discovered by following its own control flow, so the translator
    never walks into the next function or into a jump table.
  * **Operands come from capstone's operand structures, never from the printed
    string.** Parsing `op_str` is what turned 4,078 genuine address-takes into
    113,327 in the indirect-target probe; the same class of mistake in a code
    generator produces code that builds and reads the wrong memory.
  * **Lazy flags with block-local resolution.** A `cmp` immediately followed by
    a `jcc` -- the overwhelmingly common shape -- emits a direct C comparison
    and stores nothing. Only producers whose flags are still live at the end of
    a block record their state.
  * **Every unknown is a hard error.** The translator never guesses at an
    instruction, an operand form or a width. A function that contains something
    unsupported is reported as untranslated, not emitted half-right.

Liveness assumption, stated because it is an assumption: a `call` is treated as
defining the flags. No x86 calling convention preserves the arithmetic flags
across a call and no compiler emits code that relies on it, so for a binary
that is entirely compiler output this is a fact about the ABI rather than a
guess. It is what lets flag stores before a call be dropped.
"""
import bisect
import collections

import capstone
from capstone import x86 as cx

import sse_lower

CS = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
CS.detail = True

R32 = {
    cx.X86_REG_EAX: "eax", cx.X86_REG_ECX: "ecx", cx.X86_REG_EDX: "edx",
    cx.X86_REG_EBX: "ebx", cx.X86_REG_ESP: "esp", cx.X86_REG_EBP: "ebp",
    cx.X86_REG_ESI: "esi", cx.X86_REG_EDI: "edi",
}
R16 = {
    cx.X86_REG_AX: "eax", cx.X86_REG_CX: "ecx", cx.X86_REG_DX: "edx",
    cx.X86_REG_BX: "ebx", cx.X86_REG_SP: "esp", cx.X86_REG_BP: "ebp",
    cx.X86_REG_SI: "esi", cx.X86_REG_DI: "edi",
}
R8L = {
    cx.X86_REG_AL: "eax", cx.X86_REG_CL: "ecx", cx.X86_REG_DL: "edx",
    cx.X86_REG_BL: "ebx",
}
R8H = {
    cx.X86_REG_AH: "eax", cx.X86_REG_CH: "ecx", cx.X86_REG_DH: "edx",
    cx.X86_REG_BH: "ebx",
}
XMMR = {getattr(cx, "X86_REG_XMM%d" % i): i for i in range(8)}
ST = {getattr(cx, "X86_REG_ST%d" % i): i for i in range(8)}

# jcc / setcc / cmovcc suffix -> the runtime predicate that implements it,
# and the flags it reads.
CC = {
    "a": ("cc_a", "CZ"), "ae": ("cc_ae", "C"), "b": ("cc_b", "C"),
    "be": ("cc_be", "CZ"), "e": ("cc_e", "Z"), "z": ("cc_e", "Z"),
    "g": ("cc_g", "ZSO"), "ge": ("cc_ge", "SO"), "l": ("cc_l", "SO"),
    "le": ("cc_le", "ZSO"), "ne": ("cc_ne", "Z"), "nz": ("cc_ne", "Z"),
    "no": ("cc_no", "O"), "np": ("cc_np", "P"), "ns": ("cc_ns", "S"),
    "o": ("cc_o", "O"), "p": ("cc_p", "P"), "pe": ("cc_p", "P"),
    "po": ("cc_np", "P"), "s": ("cc_s", "S"),
    "nae": ("cc_b", "C"), "nb": ("cc_ae", "C"), "nbe": ("cc_a", "CZ"),
    "c": ("cc_b", "C"), "nc": ("cc_ae", "C"),
    "na": ("cc_be", "CZ"), "nge": ("cc_l", "SO"), "nl": ("cc_ge", "SO"),
    "nle": ("cc_g", "ZSO"), "ng": ("cc_le", "ZSO"),
}

# Producers we can pair with a consumer inside a block, and the C expression
# that computes each condition directly from the operands. Keyed by
# (producer mnemonic family, condition).
DIRECT = {
    # unsigned
    "b":  "((uint32_t)({a}) <  (uint32_t)({b}))",
    "ae": "((uint32_t)({a}) >= (uint32_t)({b}))",
    "be": "((uint32_t)({a}) <= (uint32_t)({b}))",
    "a":  "((uint32_t)({a}) >  (uint32_t)({b}))",
    # signed
    "l":  "((int32_t)({a}) <  (int32_t)({b}))",
    "ge": "((int32_t)({a}) >= (int32_t)({b}))",
    "le": "((int32_t)({a}) <= (int32_t)({b}))",
    "g":  "((int32_t)({a}) >  (int32_t)({b}))",
    # equality
    "e":  "(({a}) == ({b}))",
    "ne": "(({a}) != ({b}))",
}


class Unsupported(Exception):
    def __init__(self, ins, why):
        super().__init__("%08x  %s %s   -- %s"
                         % (ins.address, ins.mnemonic, ins.op_str, why))
        self.ins = ins
        self.why = why


def norm_cc(m, pfx):
    """'jae' -> 'ae' given prefix 'j'."""
    return m[len(pfx):]


class Ctx:
    """Everything the emitter needs about one binary."""

    def __init__(self, exe_path, base, text_rva, text_end, data):
        self.exe = exe_path
        self.base = base
        self.text_rva = text_rva
        self.text_end = text_end
        self.data = data          # bytes of .text

    def code_at(self, rva, n):
        i = rva - self.text_rva
        return self.data[i:i + n]

    def is_code(self, rva):
        return self.text_rva <= rva < self.text_end


# Six exceptionally large MSVC functions end in a proven throwing CRT/STL
# helper, but their metadata extends beyond that call into another function.
# Recursive descent cannot infer C++ noreturn semantics from an ordinary CALL,
# and following its apparent fallthrough pulls unrelated code into the root.
# Keep this deliberately machine-specific: the exact root, site, wrapper and
# IAT identities are all part of the proof.  The binary has 1,391 superficially
# similar calls, so a name- or target-wide rule would be unsafe.
NORETURN_IMPORTS = {
    0x006062C0: "MSVCP140.dll!?_Xlength_error@std@@YAXPBD@Z",
    0x006062D0: "MSVCP140.dll!?_Xout_of_range@std@@YAXPBD@Z",
    0x00606480: "VCRUNTIME140.dll!_CxxThrowException",
    0x0060648C: "VCRUNTIME140.dll!longjmp",
    0x006065AC: ("api-ms-win-crt-runtime-l1-1-0.dll!"
                 "_invalid_parameter_noinfo_noreturn"),
}

# wrapper RVA -> (message RVA, terminal IAT slot RVA)
NORETURN_WRAPPERS = {
    0x00010B40: (0x0073F32C, 0x006062C0),
    0x000C3A90: (0x007423D0, 0x006062D0),
}

# import thunk RVA -> terminal IAT slot RVA.  The three XML entity scanners
# call this same linker thunk, but only their exact physical call sites are
# classified as terminal below.
NORETURN_THUNKS = {
    0x005EC158: 0x00606480,
}

# Internal libpng fatal-error helper.  Its complete body is base-independent
# and machine-pinned, including the direct call to a second helper.  That
# helper is separately pinned through its unconditional VCRUNTIME `longjmp`
# call; the trailing INT3 is only a defensive trap and is not treated as the
# reason this path is terminal (the recompilation runtime's INT3 can return).
NORETURN_INTERNALS = {
    0x005C3420: bytes.fromhex(
        "568bf1578bfa8b464085c074075756ffd083c4088bd78bcee813010000cc"),
}

# internal fatal helper -> (direct-call site, longjmp helper, longjmp IAT site,
#                           longjmp IAT slot)
NORETURN_INTERNAL_LONGJMPS = {
    0x005C3420: (0x005C3438, 0x005C3550, 0x005C3570, 0x0060648C),
}


def validate_noreturn_imports(imports):
    """Fail closed unless every terminal IAT slot keeps its exact identity."""
    actual = dict(imports)
    for slot_rva, expected_name in NORETURN_IMPORTS.items():
        got = actual.get(slot_rva)
        if got != expected_name:
            raise RuntimeError(
                "pinned noreturn IAT identity changed at %08x: %r != %r"
                % (slot_rva, got, expected_name))
    return True

# root RVA -> ((terminal call site, kind, exact target RVA), ...)
NORETURN_TERMINATIONS = {
    0x002B8D50: ((0x002B91AD, "iat", 0x006065AC),),
    0x0030B440: ((0x0030B99D, "wrapper", 0x000C3A90),),
    0x0030CFD0: ((0x0030DB34, "wrapper", 0x00010B40),),
    0x00372FC0: ((0x00374650, "wrapper", 0x00010B40),),
    0x00380810: ((0x00382973, "wrapper", 0x00010B40),),
    0x004E2E40: ((0x004E4E3B, "wrapper", 0x00010B40),),
    0x005B1500: (
        (0x005B18E1, "internal", 0x005C3420),
        (0x005B18ED, "internal", 0x005C3420),
        (0x005B18F9, "internal", 0x005C3420),
        (0x005B1905, "internal", 0x005C3420),
        (0x005B191C, "internal", 0x005C3420),
    ),
    # png_read_filter_row's invalid-filter tail is immediately followed by
    # its five-entry relocation table.  png_error ends in the already pinned
    # longjmp chain, so decoding its impossible fallthrough as code invents
    # INSB/INSD/OUTSB instructions from those table bytes.
    0x005C6BF0: ((0x005C6F7E, "internal", 0x005C3420),),
}

# These calls sit behind relocation-backed switch edges, so the root's first
# decode cannot reach them.  They become mandatory once case labels are seeded
# into the shared CFG; standalone case decoding still sees them through the
# exact global site map below.
SWITCH_NORETURN_TERMINATIONS = {
    0x00011790: ((0x0001196D, "thunk", 0x005EC158),),
    0x000119E0: ((0x00011BBD, "thunk", 0x005EC158),),
    0x00011D00: ((0x00011EDD, "thunk", 0x005EC158),),
}

# A terminal CALL is a property of its exact machine-code site, not of the
# decoder seed which happened to reach it.  Switch-case validation and shared
# metadata bodies deliberately decode from interior labels; keying termination
# only by the canonical function root made those paths walk through the throw
# into unrelated bytes.  Retain the owner in the value so validation still
# uses the complete root/site/kind/target pin rather than broadening this into
# a helper-name or call-target rule.
NORETURN_SITES = {}
for _termination_map in (NORETURN_TERMINATIONS,
                         SWITCH_NORETURN_TERMINATIONS):
    for _owner, _specs in _termination_map.items():
        for _site, _kind, _target in _specs:
            if _site in NORETURN_SITES:
                raise RuntimeError("duplicate pinned noreturn site %08x" % _site)
            NORETURN_SITES[_site] = (_owner, _kind, _target)


def _decode_one(ctx, rva):
    got = list(CS.disasm(ctx.code_at(rva, 16), rva, count=1))
    if not got:
        raise Unsupported(_Fake(rva), "pinned noreturn instruction undecodable")
    return got[0]


def _validate_abs_iat_call(ctx, ins, slot_rva):
    """Require the exact FF /2 absolute-memory call to one pinned IAT slot."""
    expected_va = (ctx.base + slot_rva) & 0xFFFFFFFF
    expected_raw = b"\xff\x15" + expected_va.to_bytes(4, "little")
    ops = ins.operands
    op = ops[0] if len(ops) == 1 else None
    mem = op.mem if op is not None and op.type == cx.X86_OP_MEM else None
    if (ctx.code_at(ins.address, 6) != expected_raw or ins.size != 6 or
            ins.mnemonic != "call" or op is None or op.size != 4 or
            mem is None or mem.segment not in (0, cx.X86_REG_INVALID) or
            mem.base not in (0, cx.X86_REG_INVALID) or
            mem.index not in (0, cx.X86_REG_INVALID) or
            (mem.disp & 0xFFFFFFFF) != expected_va):
        raise Unsupported(
            ins, "pinned noreturn IAT call no longer targets %08x" % slot_rva)


def validate_noreturn_wrapper(ctx, wrapper_rva):
    """Validate exact `push message; call [IAT]; int3` wrapper bytes/shape."""
    if wrapper_rva not in NORETURN_WRAPPERS:
        raise Unsupported(_Fake(wrapper_rva), "unconfigured noreturn wrapper")
    message_rva, slot_rva = NORETURN_WRAPPERS[wrapper_rva]
    message_va = (ctx.base + message_rva) & 0xFFFFFFFF
    slot_va = (ctx.base + slot_rva) & 0xFFFFFFFF
    expected = (b"\x68" + message_va.to_bytes(4, "little") +
                b"\xff\x15" + slot_va.to_bytes(4, "little") + b"\xcc")
    raw = ctx.code_at(wrapper_rva, len(expected))
    got = list(CS.disasm(raw, wrapper_rva))
    if raw != expected or len(got) != 3:
        raise Unsupported(_Fake(wrapper_rva),
                          "pinned noreturn wrapper bytes changed")
    push, call, trap = got
    push_op = push.operands[0] if len(push.operands) == 1 else None
    if (push.address != wrapper_rva or push.mnemonic != "push" or
            push.size != 5 or push_op is None or
            push_op.type != cx.X86_OP_IMM or
            (push_op.imm & 0xFFFFFFFF) != message_va or
            call.address != wrapper_rva + 5 or trap.address != wrapper_rva + 11 or
            trap.mnemonic != "int3" or trap.size != 1):
        raise Unsupported(_Fake(wrapper_rva),
                          "pinned noreturn wrapper shape changed")
    _validate_abs_iat_call(ctx, call, slot_rva)
    return True


def validate_noreturn_thunk(ctx, thunk_rva):
    """Require an exact absolute JMP through one configured terminal IAT."""
    slot_rva = NORETURN_THUNKS.get(thunk_rva)
    if slot_rva is None:
        raise Unsupported(_Fake(thunk_rva), "unconfigured noreturn thunk")
    slot_va = (ctx.base + slot_rva) & 0xFFFFFFFF
    expected = b"\xff\x25" + slot_va.to_bytes(4, "little")
    raw = ctx.code_at(thunk_rva, len(expected))
    got = list(CS.disasm(raw, thunk_rva, count=1))
    ins = got[0] if len(got) == 1 else None
    op = ins.operands[0] if ins is not None and len(ins.operands) == 1 else None
    mem = op.mem if op is not None and op.type == cx.X86_OP_MEM else None
    invalid = (0, cx.X86_REG_INVALID)
    if (raw != expected or ins is None or ins.address != thunk_rva or
            ins.size != 6 or ins.mnemonic != "jmp" or op is None or
            op.size != 4 or
            mem is None or mem.segment not in invalid or
            mem.base not in invalid or mem.index not in invalid or
            (mem.disp & 0xFFFFFFFF) != slot_va):
        raise Unsupported(_Fake(thunk_rva),
                          "pinned noreturn thunk bytes/shape changed")
    return True


def validate_noreturn_internal(ctx, target_rva):
    """Require the complete libpng fatal helper and its real longjmp exit."""
    expected = NORETURN_INTERNALS.get(target_rva)
    chain = NORETURN_INTERNAL_LONGJMPS.get(target_rva)
    if expected is None or chain is None:
        raise Unsupported(_Fake(target_rva),
                          "unconfigured internal noreturn target")
    raw = ctx.code_at(target_rva, len(expected))
    got = list(CS.disasm(raw, target_rva))
    call_site, helper_rva, longjmp_site, longjmp_slot = chain
    helper_call = next((ins for ins in got if ins.address == call_site), None)
    helper_op = (helper_call.operands[0] if helper_call is not None and
                 len(helper_call.operands) == 1 else None)
    if (raw != expected or not got or
            got[0].address != target_rva or got[0].mnemonic != "push" or
            got[-1].address != target_rva + len(expected) - 1 or
            got[-1].mnemonic != "int3" or helper_call is None or
            helper_call.mnemonic != "call" or helper_call.size != 5 or
            helper_op is None or helper_op.type != cx.X86_OP_IMM or
            helper_op.imm != helper_rva):
        raise Unsupported(_Fake(target_rva),
                          "pinned internal noreturn body changed")

    # Pin the entire straight-line second helper, constructing relocated
    # absolute operands from the current image base.  There is no branch or
    # RET before the exact longjmp IAT call.
    helper_expected = (
        b"\x56\x52\x68" +
        ((ctx.base + 0x0076936C) & 0xFFFFFFFF).to_bytes(4, "little") +
        b"\x6a\x02\x8b\xf1\xff\x15" +
        ((ctx.base + 0x00606604) & 0xFFFFFFFF).to_bytes(4, "little") +
        b"\x83\xc4\x04\x50\xe8\x16\x52\xe3\xff\x83\xc4\x0c"
        b"\x6a\x01\x56\xff\x15" +
        ((ctx.base + longjmp_slot) & 0xFFFFFFFF).to_bytes(4, "little") +
        b"\xcc")
    helper_raw = ctx.code_at(helper_rva, len(helper_expected))
    helper_insns = list(CS.disasm(helper_raw, helper_rva))
    longjmp_call = next(
        (ins for ins in helper_insns if ins.address == longjmp_site), None)
    if (helper_raw != helper_expected or not helper_insns or
            helper_insns[0].address != helper_rva or
            helper_insns[-1].address != helper_rva + len(helper_expected) - 1 or
            helper_insns[-1].mnemonic != "int3" or longjmp_call is None or
            any(ins.mnemonic.startswith("j") or
                ins.mnemonic in ("ret", "retf", "iret")
                for ins in helper_insns)):
        raise Unsupported(_Fake(helper_rva),
                          "pinned internal longjmp helper changed")
    _validate_abs_iat_call(ctx, longjmp_call, longjmp_slot)
    return True


def validate_noreturn_termination(ctx, root, site, ins=None):
    """Validate one exact root/site activation; never classify by name alone."""
    configured = {
        rva: (kind, target)
        for termination_map in (NORETURN_TERMINATIONS,
                                SWITCH_NORETURN_TERMINATIONS)
        for rva, kind, target in termination_map.get(root, ())
    }
    if site not in configured:
        raise Unsupported(_Fake(site),
                          "unconfigured noreturn site for root %08x" % root)
    ins = ins or _decode_one(ctx, site)
    if ins.address != site:
        raise Unsupported(ins, "pinned noreturn site address changed")
    kind, target = configured[site]
    if kind == "iat":
        _validate_abs_iat_call(ctx, ins, target)
    elif kind in ("wrapper", "thunk", "internal"):
        expected_raw = (b"\xe8" +
                        ((target - (site + 5)) & 0xFFFFFFFF).to_bytes(
                            4, "little"))
        op = ins.operands[0] if len(ins.operands) == 1 else None
        if (ctx.code_at(site, 5) != expected_raw or ins.mnemonic != "call" or
                ins.size != 5 or op is None or op.type != cx.X86_OP_IMM or
                op.imm != target):
            raise Unsupported(
                ins, "pinned noreturn wrapper call no longer targets %08x" % target)
        if kind == "wrapper":
            validate_noreturn_wrapper(ctx, target)
        elif kind == "thunk":
            validate_noreturn_thunk(ctx, target)
        else:
            validate_noreturn_internal(ctx, target)
    else:
        raise Unsupported(ins, "unknown pinned noreturn kind %r" % kind)
    return True


# ------------------------------------------------------------------ decode --

def _normalise_data_ranges(ctx, data_ranges):
    """Validate non-overlapping RVA ranges proved to contain embedded data."""
    rows = []
    for row in data_ranges:
        if (not isinstance(row, (tuple, list)) or len(row) != 2 or
                any(isinstance(value, bool) or not isinstance(value, int)
                    for value in row)):
            raise ValueError("decode data range must be an integer pair")
        begin, end = row
        if begin < 0 or begin >= end or end > ctx.size:
            raise ValueError("decode data range is outside the image")
        rows.append((begin, end))
    rows = sorted(set(rows))
    for previous, current in zip(rows, rows[1:]):
        if current[0] < previous[1]:
            raise ValueError("decode data ranges overlap")
    return tuple(rows)


def _containing_data_range(data_ranges, rva):
    index = bisect.bisect_right(data_ranges, (rva, 0x100000000)) - 1
    if index >= 0:
        begin, end = data_ranges[index]
        if rva < end:
            return begin, end
    return None


def _overlapping_data_range(data_ranges, begin, end):
    index = bisect.bisect_left(data_ranges, (begin, 0))
    if index and data_ranges[index - 1][1] > begin:
        return data_ranges[index - 1]
    if index < len(data_ranges) and data_ranges[index][0] < end:
        return data_ranges[index]
    return None


def decode(ctx, start, limit=0x40000, extra_starts=(), data_ranges=()):
    """Recursive descent from `start`.

    Returns (insns_by_rva, order, targets, exits) where `order` is the
    ascending list of reachable instruction addresses. Jump-table targets are
    NOT followed here -- an indirect jump ends a path and is reported, so the
    caller can decide (a switch table needs its own resolution pass).
    """
    data_ranges = _normalise_data_ranges(ctx, data_ranges)
    seeds = (start,) + tuple(extra_starts)
    for seed in seeds:
        containing = _containing_data_range(data_ranges, seed)
        if containing is not None:
            raise Unsupported(
                _Fake(seed),
                "decode seed lies in proved data range %08x-%08x" %
                containing)

    def require_code_target(source, target):
        containing = _containing_data_range(data_ranges, target)
        if containing is not None:
            raise Unsupported(
                source, "control target enters proved data range %08x-%08x" %
                containing)
    insns = {}
    # Switch case labels are real control-flow entries even though they are
    # not functions.  The whole-program generator may seed them after it has
    # proved an absolute relocation-backed jump table.  Put `start` last so
    # the ordinary function entry is decoded first; output remains sorted.
    work = [(rva, rva) for rva in reversed(tuple(extra_starts))]
    work.append((start, start))
    seen = set()
    indirect = []
    terminal_specs = {site: (kind, target) for site, kind, target in
                      NORETURN_TERMINATIONS.get(start, ())}
    if extra_starts:
        terminal_specs.update(
            {site: (kind, target) for site, kind, target in
             SWITCH_NORETURN_TERMINATIONS.get(start, ())})
    seen_terminals = set()
    occupied = {} if extra_starts else None
    while work:
        rva, seed = work.pop()
        while True:
            # Only sequential fallthrough can reach a proved data range: all
            # seeds and explicit control-flow targets are rejected above.
            if _containing_data_range(data_ranges, rva) is not None:
                break
            if rva in insns or not ctx.is_code(rva) or rva - seed > limit:
                break
            buf = ctx.code_at(rva, 16)
            got = list(CS.disasm(buf, rva, count=1))
            if not got:
                raise Unsupported(_Fake(rva), "undecodable bytes %s" % buf[:8].hex())
            ins = got[0]
            overlap = _overlapping_data_range(
                data_ranges, rva, rva + ins.size)
            if overlap is not None:
                raise Unsupported(
                    ins, "instruction overlaps proved data range %08x-%08x" %
                    overlap)
            if occupied is not None:
                overlap = {occupied[a] for a in range(rva, rva + ins.size)
                           if a in occupied and occupied[a] != rva}
                if overlap:
                    raise Unsupported(
                        ins, "instruction overlaps decoded start(s) %s"
                        % ",".join("%08x" % x for x in sorted(overlap)))
                for a in range(rva, rva + ins.size):
                    occupied[a] = rva
            insns[rva] = ins
            m = ins.mnemonic
            nxt = rva + ins.size

            # Record the CALL itself, but do not decode its impossible
            # fallthrough.  A configured site that moves, becomes unreachable,
            # or stops matching its exact wrapper/IAT proof is a hard error.
            terminal = NORETURN_SITES.get(rva)
            if terminal is not None:
                owner, _kind, _target = terminal
                validate_noreturn_termination(ctx, owner, rva, ins)
                if owner == start:
                    seen_terminals.add(rva)
                break

            if m in ("ret", "retf", "iret"):
                break
            if m == "jmp":
                o = ins.operands[0]
                if o.type == cx.X86_OP_IMM:
                    t = o.imm
                    require_code_target(ins, t)
                    if ctx.is_code(t):
                        rva = t
                        continue
                    break
                indirect.append(rva)
                break
            if m.startswith("j"):          # conditional
                o = ins.operands[0]
                if o.type != cx.X86_OP_IMM:
                    indirect.append(rva)
                    break
                t = o.imm
                require_code_target(ins, t)
                if ctx.is_code(t) and t not in insns and t not in seen:
                    seen.add(t)
                    work.append((t, seed))
                rva = nxt
                continue
            if m == "int3":
                # An int3 in the middle of a function is a deliberate trap
                # (RNG::Next has one); a run of them is padding and means the
                # path fell off the end.
                if ctx.code_at(nxt, 1) == b"\xcc":
                    break
                rva = nxt
                continue
            rva = nxt

    missing_terminals = sorted(set(terminal_specs) - seen_terminals)
    if missing_terminals:
        raise Unsupported(
            _Fake(missing_terminals[0]),
            "pinned noreturn site not reached from root %08x" % start)
    order = sorted(insns)
    return insns, order, indirect


class _Fake:
    """Stand-in so Unsupported can report an address with no decoded insn."""
    def __init__(self, rva):
        self.address = rva
        self.mnemonic = "??"
        self.op_str = ""


# ------------------------------------------------------------------ blocks --

def blocks(insns, order, start, extra_starts=(), indirect_edges=None):
    """Split into basic blocks. Returns (leaders, block_of, succs)."""
    leaders = {start}
    leaders.update(extra_starts)
    for rva, ins in insns.items():
        m = ins.mnemonic
        nxt = rva + ins.size
        if m.startswith("j") and ins.operands and ins.operands[0].type == cx.X86_OP_IMM:
            t = ins.operands[0].imm
            if t in insns:
                leaders.add(t)
            if m != "jmp" and nxt in insns:
                leaders.add(nxt)
        elif m in ("ret", "retf", "iret", "jmp"):
            pass
    leaders = sorted(l for l in leaders if l in insns)

    block_of = {}
    cur = None
    for rva in order:
        if rva in leaders:
            cur = rva
        block_of[rva] = cur

    members = collections.defaultdict(list)
    for rva in order:
        members[block_of[rva]].append(rva)

    succs = collections.defaultdict(set)
    for b, rvas in members.items():
        last = insns[rvas[-1]]
        m = last.mnemonic
        nxt = rvas[-1] + last.size
        if m in ("ret", "retf", "iret"):
            continue
        if m == "jmp":
            if last.operands[0].type == cx.X86_OP_IMM and last.operands[0].imm in insns:
                succs[b].add(block_of[last.operands[0].imm])
            elif indirect_edges:
                # A proved switch edge carries the lazy flag state into its
                # case label.  Without this edge the producer before the
                # dispatch may be deleted even when a case consumes it.
                for t in indirect_edges.get(rvas[-1], ()):
                    if t in block_of:
                        succs[b].add(block_of[t])
            continue
        if m.startswith("j") and last.operands and last.operands[0].type == cx.X86_OP_IMM:
            t = last.operands[0].imm
            if t in insns:
                succs[b].add(block_of[t])
        if nxt in insns:
            succs[b].add(block_of[nxt])
    return leaders, block_of, members, succs


# ------------------------------------------------------- flag bookkeeping ---

# Which flags an instruction writes. "*" means all arithmetic flags.
FLAG_WRITERS = {
    "add": "*", "sub": "*", "cmp": "*", "and": "*", "or": "*", "xor": "*",
    "test": "*", "neg": "*", "adc": "*", "sbb": "*", "imul": "*", "mul": "*",
    "shl": "*", "shr": "*", "sar": "*", "sal": "*", "rol": "*", "ror": "*",
    "shld": "*", "shrd": "*", "bt": "C", "bts": "C", "btr": "C",
    "rcl": "CO", "rcr": "CO",
    "clc": "C", "stc": "C", "cmc": "C",
    "inc": "ZSOP", "dec": "ZSOP",
    "comiss": "*", "ucomiss": "*", "comisd": "*", "ucomisd": "*",
    "xadd": "*", "cmpxchg": "*", "lock cmpxchg8b": "Z",
    "sahf": "*", "btc": "C",
}


def flags_written(ins):
    m = ins.mnemonic
    if m in FLAG_WRITERS:
        w = FLAG_WRITERS[m]
        return set("CZSOP") if w == "*" else set(w)
    if m == "call":
        # No x86 convention preserves the arithmetic flags across a call.
        return set("CZSOP")
    return set()


def flags_read(ins):
    m = ins.mnemonic
    if m.startswith("j") and m != "jmp" and len(m) > 1:
        cc = norm_cc(m, "j")
        if cc in CC:
            return set(CC[cc][1])
    if m.startswith("set"):
        cc = norm_cc(m, "set")
        if cc in CC:
            return set(CC[cc][1])
    if m.startswith("cmov"):
        cc = norm_cc(m, "cmov")
        if cc in CC:
            return set(CC[cc][1])
    if m in ("adc", "sbb"):
        return set("C")
    if m in ("rcl", "rcr", "cmc"):
        return set("C")
    if m == "lahf":
        return set("CZSP")
    if m == "pushfd":
        return set("CZSOP")
    return set()


def direct_flag_consumer(ins, producer=None):
    """Whether emit.py actually consumes a paired producer for `ins`.

    Only conditions are replaced by a direct C comparison.  Arithmetic flag
    consumers such as ADC/SBB/RCL/RCR/CMC still call fl_* at runtime; handing
    them a pair and deleting the producer leaves those calls reading stale
    state.  Keep this predicate beside DIRECT so adding another consumer has
    one explicit opt-in point.

    A comis producer (`producer` given, sse_lower.COMIS) answers only the
    CF/ZF/PF conditions of sse_lower.FLOAT_DIRECT; the SF/OF relations
    (g/ge/l/le/s/o and their negations) stay on the lazy path.
    """
    m = ins.mnemonic
    if m.startswith("j") and m != "jmp" and len(m) > 1:
        cc = norm_cc(m, "j")
    elif m.startswith("set"):
        cc = norm_cc(m, "set")
    elif m.startswith("cmov"):
        cc = norm_cc(m, "cmov")
    else:
        return False
    if producer is not None and producer.mnemonic in sse_lower.COMIS:
        return cc in sse_lower.FLOAT_DIRECT
    return cc in DIRECT


def _reg_root(r):
    """Capstone register id -> the 32-bit register it lives in, so that a
    write to `al` is seen to clobber a producer that read `eax`."""
    for tbl in (R32, R16, R8L, R8H):
        if r in tbl:
            return tbl[r]
    if r in XMMR:
        return "xmm%d" % XMMR[r]
    return r


def operand_footprint(ins):
    """(register roots read, whether memory is read) for a producer's operands."""
    regs, mem = set(), False
    for op in ins.operands:
        if op.type == cx.X86_OP_REG:
            regs.add(_reg_root(op.reg))
        elif op.type == cx.X86_OP_MEM:
            mem = True
            for r in (op.mem.base, op.mem.index):
                if r not in (0, cx.X86_REG_INVALID):
                    regs.add(_reg_root(r))
    return regs, mem


def pair_is_safe(producer, between):
    """May the producer's operand expressions be evaluated at the consumer?

    Only if nothing strictly between them changes what those expressions
    read. Conservative on purpose: a call clobbers everything, and any memory
    write invalidates a producer that reads memory. Refusing costs one flag
    store; accepting wrongly costs a silent miscompile, so the asymmetry is
    deliberate.
    """
    # The operand expressions are re-evaluated at the consumer.  A mutating
    # producer such as SUB is unsafe even when adjacent: its destination no
    # longer contains the input value by then.  Keep this guard here as a
    # second line of defence if producer_operands() grows later.  comis
    # reads its two operands and writes only flags, so it qualifies under
    # the same footprint rule (xmm registers are roots too, see _reg_root).
    if (producer.mnemonic not in ("cmp", "test") and
            producer.mnemonic not in sse_lower.COMIS):
        return False
    if not between:
        return True
    regs, mem = operand_footprint(producer)
    # A memory operand would be re-read at the consumer.  Capstone exposes
    # ordinary destination operands, but implicit stores such as PUSH are not
    # present in ins.operands; proving non-aliasing against the producer would
    # require more than this local pass knows.  Refuse the small set of
    # non-adjacent memory pairs instead of making that assumption.
    if mem:
        return False
    for ins in between:
        if ins.mnemonic in ("call", "int3"):
            return False
        try:
            _, written = ins.regs_access()
        except Exception:
            return False
        for w in written:
            if _reg_root(w) in regs:
                return False
    return True

def flag_liveness(insns, members, succs, flags_dead_jmps=frozenset()):
    """Backward dataflow: which blocks end with flags still needed.

    `flags_dead_jmps` names the RVAs of indirect JMPs whose destination is a
    host import (`jmp dword ptr [IAT slot]`).  A host shim never reads the
    x86 arithmetic flags -- exactly the assumption every `call [IAT]` site
    already makes -- so such a block is not an external flag edge: a producer
    whose only consumer would be that jump is dead, and a body that is one
    such jump does not load the flag state at entry."""
    use = {}
    dfn = {}
    for b, rvas in members.items():
        u, d = set(), set()
        for rva in rvas:
            ins = insns[rva]
            u |= (flags_read(ins) - d)
            d |= flags_written(ins)
        use[b], dfn[b] = u, d

    live_in = {b: set() for b in members}
    live_out = {b: set() for b in members}
    # An indirect JMP is a tail transfer, not an ABI call: the destination
    # receives the arithmetic flags unchanged.  Even a resolved switch keeps
    # a generic default edge for mutable/unexpected table values, so no local
    # successor set can prove those flags dead.
    external_flag_edge = set()
    for b, rvas in members.items():
        last = insns[rvas[-1]]
        if (last.mnemonic == "jmp" and last.operands and
                last.operands[0].type != cx.X86_OP_IMM and
                rvas[-1] not in flags_dead_jmps):
            external_flag_edge.add(b)
    changed = True
    while changed:
        changed = False
        for b in members:
            o = set("CZSOP") if b in external_flag_edge else set()
            for s in succs.get(b, ()):
                o |= live_in[s]
            i = use[b] | (o - dfn[b])
            if o != live_out[b] or i != live_in[b]:
                live_out[b], live_in[b] = o, i
                changed = True
    return live_in, live_out
