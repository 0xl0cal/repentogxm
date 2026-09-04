"""Translate one function through the real pipeline and write a C file.

The point of this driver is to regenerate individual ladder targets through the
same translator used by the whole-program build. `build_test.py` combines six
such targets into the current 206,480-check oracle baseline. A rewrite that
loses a proven result is a regression whether or not anything says so.

Usage: python build_one.py <rva_hex> [name]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import emit as E                                            # noqa: E402
import trans as T                                           # noqa: E402
from image import Image                                     # noqa: E402

_DEFAULT_WORKDIR = os.path.abspath(os.environ.get(
    "REPENTOGXM_WORKDIR",
    os.path.join(os.path.dirname(__file__), "..", "build", "recompiler"),
))
EXE = os.path.abspath(os.environ.get(
    "REPENTOGXM_PE",
    os.path.join(_DEFAULT_WORKDIR, "isaac-ng.exe.unpacked.exe"),
))
OUTDIR = _DEFAULT_WORKDIR


def load_ctx():
    """The Image is the context: it exposes `is_code` and `code_at` over the
    rebased bytes, so the translator disassembles what the runtime will map.
    Instruction addresses stay RVAs (function names, labels) while memory
    displacements are absolute VAs already corrected by the relocation pass."""
    return Image(EXE)


def translate(ctx, start, name, known=None, extra_entries=(),
              indirect_edges=None, data_ranges=(), import_slots=None):
    """`known` is the set of function RVAs that will exist in the link. When
    given, a direct call or tail call to anything outside it becomes a loud
    fault instead of a reference to a function nobody will define -- which is
    how the first whole-program link failed.

    `import_slots` maps IAT slot RVA -> dense import ID (gen_all supplies the
    slot-RVA-sorted PE import table).  With it, `call/jmp dword ptr [slot]`
    and IAT-loaded `call reg` sites are spelled through GUEST_IMPORT_CALL/
    GUEST_IMPORT_JMP (guest.h) and a `jmp [slot]` is not a flag consumer.
    Without it (single-function builds, host oracles) the text is unchanged."""
    extra_entries = tuple(sorted(set(extra_entries)))
    import_slots = dict(import_slots or {})
    insns, order, indirect = T.decode(
        ctx, start, extra_starts=extra_entries, data_ranges=data_ranges)
    leaders, block_of, members, succs = T.blocks(
        insns, order, start, extra_entries, indirect_edges)
    iat_jmps = frozenset(
        rva for rva, ins in insns.items()
        if ins.mnemonic.split()[-1] == "jmp" and len(ins.operands) == 1 and
        E.iat_slot_operand(ctx, import_slots, ins.operands[0]) is not None)
    live_in, live_out = T.flag_liveness(insns, members, succs, iat_jmps)

    em = E.Emitter(ctx, name, live_out, block_of, insns)
    em.known = known
    em.indirect_edges = indirect_edges or {}
    em.import_slots = import_slots
    # Flags reaching the entry block from outside can only arrive through an
    # external tail transfer; a unit holding flags in locals must load them.
    em.entry_flags_live = bool(live_in.get(block_of.get(start)))
    # Which scalar xmm loads may skip their upper-lane zero fill: decided by
    # following each loaded value through the block graph (emit.py).
    em.xmm_zero_elide = E.xmm_zero_fill_elidable(
        insns, block_of, members, succs)
    # SSE lowering: which packed ops / register copies may be spelled
    # lane-0-only because nothing observes their result's upper lanes (same
    # analysis, the op's own site as the taint source).
    if em.sse_lower:
        em.xmm_lane0 = E.xmm_lane0_sites(insns, block_of, members, succs)

    # Block-local producer/consumer pairing. Walking each block forward, the
    # nearest preceding flag writer is the producer for a consumer; if that
    # producer is one we can express directly (CMP / TEST, and comis under
    # the SSE lowering switch) the consumer is answered with a plain C
    # comparison and the producer stores nothing.
    pair_for = {}
    dead_producer = set()
    unsafe_pairs = 0
    for b, rvas in members.items():
        prod = None
        prod_used_directly = True
        for i, rva in enumerate(rvas):
            ins = insns[rva]
            rdf = T.flags_read(ins)
            if (rdf and prod is not None and
                    T.direct_flag_consumer(ins, insns[prod])):
                p = em.producer_operands(insns[prod])
                # A direct comparison substitutes the producer's operand
                # EXPRESSIONS at the consumer's position, so it is only valid
                # while nothing in between changes what they read. `cmp` is
                # nearly always adjacent to its `jcc`, but "nearly always" is
                # how a silent miscompile gets in.
                between = [insns[r] for r in rvas[rvas.index(prod) + 1:i]]
                if p is not None and T.pair_is_safe(insns[prod], between):
                    pair_for[rva] = p
                else:
                    if p is not None:
                        unsafe_pairs += 1
                    prod_used_directly = False
            elif rdf:
                prod_used_directly = False
            written = T.flags_written(ins)
            if written:
                # A partial writer needs the previous context for the flags it
                # preserves (and CMC/RCL/RCR also read CF).  Only a full writer
                # makes the previous producer genuinely dead here.
                if (prod is not None and prod_used_directly and
                        written == set("CZSOP")):
                    dead_producer.add(prod)
                prod = rva if insns[rva].mnemonic in T.FLAG_WRITERS else None
                prod_used_directly = True
        # the last producer in the block only dies if no successor needs it
        if prod is not None and prod_used_directly and not live_out[b]:
            dead_producer.add(prod)
    em.unsafe_pairs = unsafe_pairs

    body = []
    unsupported = []
    for rva in order:
        ins = insns[rva]
        if rva in em.labels or True:            # labels resolved after the pass
            pass
        try:
            stmts = em.emit(ins, rva not in dead_producer, pair_for.get(rva))
        except T.Unsupported as ex:
            unsupported.append(ex)
            stmts = ["GUEST_GPR_FLUSH(c); guest_fault(c, 0x%xU, \"%s %s\");"
                     " return;"
                     % (rva, ins.mnemonic, ins.op_str.replace('"', "'"))]
        body.append((ins, stmts))

    # Recursive descent may reach code below the requested entry RVA through
    # a backward jump or a tail thunk.  `order` is intentionally address
    # sorted so ordinary x86 fall-through remains C fall-through, but a C
    # function starts at the first emitted statement.  Without an explicit
    # gate, 2,303 current dispatch entries (including the real PE entry point
    # 0x5eb83e) start at the lowest reachable address instead of at their own
    # address.  Make the requested entry a label; both writers below emit the
    # gate returned by entry_gate().
    if order and order[0] != start:
        em.labels.add(start)
    em.labels.update(rva for rva in extra_entries if rva in insns)

    return em, insns, order, body, unsupported, indirect, members


def flag_state_prologue(em):
    """Statements that open every generated body: the flag-state declaration
    (a no-op unless the unit is compiled with GUEST_FLAGS_LOCAL), the
    general-register declaration (a no-op unless GUEST_GPR_LOCAL; otherwise
    it loads eax..edi/esp into the body's locals) and, only when the entry
    block consumes flags it did not produce, the flag load."""
    lines = ["GUEST_FLAGS_DECL;", "GUEST_GPR_DECL;"]
    if getattr(em, "entry_flags_live", False):
        lines.append("GUEST_FLAGS_LOAD(c);")
    return lines


def entry_gate(start, order):
    """C statement needed to enter an address-sorted decoded body correctly."""
    if order and order[0] != start:
        return "goto L_%08x;" % start
    return None


def write_c(path, name, em, body, order, start):
    L = []
    L.append("/* GENERATED by recomp/build_one.py -- do not edit. */")
    L.append("/* %s at RVA %08x, %d instructions. */" % (name, start, len(body)))
    L.append("#include <math.h>")
    L.append("#include \"guest.h\"")
    L.append("")
    for t in sorted(em.calls):
        L.append("void sub_%08x(CPU *__restrict c);" % t)
    L.append("")
    L.append("void %s(CPU *__restrict c)" % name)
    L.append("{")
    L.extend("    %s" % line for line in flag_state_prologue(em))
    gate = entry_gate(start, order)
    if gate:
        L.append("    %s" % gate)
    for ins, stmts in body:
        if ins.address in em.labels:
            L.append("L_%08x:" % ins.address)
        L.append("    /* %08x  %-16s %s %s */"
                 % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
        for s in stmts:
            L.append("    %s" % s)
    L.append("}")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")
    return len(L)


def main():
    rva = int(sys.argv[1], 16)
    name = sys.argv[2] if len(sys.argv) > 2 else "sub_%08x" % rva
    os.makedirs(OUTDIR, exist_ok=True)
    ctx = load_ctx()
    em, insns, order, body, unsup, indirect, members = translate(ctx, rva, name)

    out = os.path.join(OUTDIR, "%s.c" % name)
    n = write_c(out, name, em, body, order, rva)

    print("function            : %s @ RVA %08x" % (name, rva))
    print("instructions        : %d   (extent %08x..%08x)"
          % (len(order), order[0], order[-1] + insns[order[-1]].size))
    print("basic blocks        : %d" % len(members))
    mn = {}
    for r in order:
        mn[insns[r].mnemonic] = mn.get(insns[r].mnemonic, 0) + 1
    print("distinct mnemonics  : %d" % len(mn))
    print("direct calls        : %s"
          % (", ".join("%08x" % t for t in sorted(em.calls)) or "none"))
    print("indirect transfers  : %d" % em.indirect)
    print("flag stores emitted : %d" % em.flag_stores)
    print("conditions answered directly (no flag context): %d" % em.direct_pairs)
    print("pairs refused as unsafe (operand changed in between): %d"
          % getattr(em, "unsafe_pairs", 0))
    print("UNSUPPORTED         : %d" % len(unsup))
    for ex in unsup:
        print("   %s" % ex)
    print("written             : %s (%d lines)" % (out, n))
    return 1 if unsup else 0


if __name__ == "__main__":
    sys.exit(main())
