"""Separate real functions from data that padding heuristics mistook for code.

The whole-binary sweep reported `aas`, `aam` and `das` among the instructions
blocking translation. Those are BCD instructions: no compiler born after about
1990 emits them, and MSVC certainly does not. Their presence is therefore not a
statement about the emitter's coverage -- it is a statement that some candidate
"functions" are jump tables, constant pools or string data being disassembled
as code.

That matters for priority. Counting those candidates as translation failures
overstates the remaining work, and implementing `aam` would be writing an
emitter rule for data.

This script makes the claim falsifiable rather than merely plausible. Each
candidate is labelled with the evidence that produced it:

    CALLED   a direct `call rel32` lands on it -- a function start beyond
             argument, whatever else is true
    VTABLE   its address is stored in `.rdata`/`.data` -- a virtual method or
             a table entry
    JUMPED   a direct `jmp rel32` lands on it (tail call, or an interior label)
    ORPHAN   nothing points at it; it exists only because the preceding bytes
             were `CC` padding

The prediction: candidates containing BCD instructions are overwhelmingly
ORPHAN. If they turn out to be CALLED, the hypothesis is wrong and they are
real functions that need real emitter work.

Writes `build/recompiler/bounds.json` by default.
"""
import collections
import json
import os
import sys

import capstone
import pefile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import build_contract as BC                                  # noqa: E402
import funcs as F                                            # noqa: E402

_DEFAULT_WORKDIR = os.path.abspath(os.environ.get(
    "REPENTOGXM_WORKDIR",
    os.path.join(os.path.dirname(__file__), "..", "build", "recompiler"),
))
EXE = os.path.abspath(os.environ.get(
    "REPENTOGXM_PE",
    os.path.join(_DEFAULT_WORKDIR, "isaac-ng.exe.unpacked.exe"),
))
FUNCS = os.path.join(_DEFAULT_WORKDIR, "functions.json")
OUT = os.path.join(_DEFAULT_WORKDIR, "bounds.json")
MANIFEST = os.path.join(_DEFAULT_WORKDIR, "bounds.manifest.json")

# Instructions that a 32-bit MSVC build of a 2021 game cannot contain. BCD
# arithmetic, the far pointer loads, and the 8086 leftovers. Seeing one of
# these means the bytes are not code.
IMPOSSIBLE = {
    "aaa", "aas", "aam", "aad", "daa", "das",       # packed/unpacked BCD
    "into", "bound", "salc", "xlatb", "xlat",       # 8086 leftovers
    "lds", "les",                                   # far pointer loads
    "arpl", "sysenter", "sysexit", "hlt",           # privileged / protected
    "in", "out", "insb", "insd", "outsb", "outsd",  # port IO
    "lgdt", "lidt", "lldt", "ltr", "clts", "invd",  # system
    "loopne", "loope",                              # loop forms MSVC never emits
    "iret", "iretd", "retf", "sysret",
}


def contract_recipe(functions_output_set_id):
    """Content identity of the inputs that define bounds.json."""
    return BC.recipe(
        "bounds",
        {
            "input/isaac-ng.exe.unpacked.exe": EXE,
            "upstream/functions.output_set": BC.digest(functions_output_set_id),
            "source/recomp/bounds.py": __file__,
            "source/recomp/build_contract.py": BC.__file__,
        },
        {"output_schema": 1},
        BC.tool_ids(),
    )


def _main():
    functions_recipe_id, _ = F.contract_recipe()
    functions_manifest = BC.require_manifest(
        F.MANIFEST,
        "functions",
        functions_recipe_id,
        {"functions.json": FUNCS},
    )
    recipe_id, recipe_data = contract_recipe(
        functions_manifest["output_set_id"]
    )
    if os.path.exists(MANIFEST):
        os.remove(MANIFEST)       # a failed run must not bless the old JSON

    pe = pefile.PE(EXE, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
    tlo = text.VirtualAddress
    thi = tlo + text.Misc_VirtualSize
    tdata = text.get_data()

    with open(FUNCS, encoding="utf-8") as f:
        meta = json.load(f)
    funcs = meta["functions"]
    by_rva = {f["rva"]: f for f in funcs}

    # ---- rebuild the evidence, which functions.json does not keep ---------
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    called, jumped = set(), set()
    off, n = 0, len(tdata)
    while off < n:
        consumed = 0
        for ins in md.disasm(tdata[off:], base + tlo + off):
            consumed += ins.size
            if len(ins.operands) == 1 and ins.operands[0].type == capstone.x86.X86_OP_IMM:
                t = ins.operands[0].imm - base
                if tlo <= t < thi:
                    if ins.mnemonic == "call":
                        called.add(t)
                    elif ins.mnemonic == "jmp":
                        jumped.add(t)
        if consumed == 0:
            off += 1
        else:
            off += consumed

    ptrs = set()
    for s in pe.sections:
        nm = s.Name.rstrip(b"\x00").decode("latin1")
        if nm not in (".rdata", ".data"):
            continue
        d = s.get_data()
        for i in range(0, len(d) - 3, 4):
            v = int.from_bytes(d[i:i + 4], "little")
            if base + tlo <= v < base + thi:
                ptrs.add(v - base)

    print("evidence: %d call targets, %d jmp targets, %d data pointers"
          % (len(called), len(jumped), len(ptrs)))

    def label(rva):
        if rva in called:
            return "CALLED"
        if rva in ptrs:
            return "VTABLE"
        if rva in jumped:
            return "JUMPED"
        return "ORPHAN"

    # ---- scan each candidate's bytes for the impossible -------------------
    ev = collections.Counter()
    ev_bytes = collections.Counter()
    bad = []
    for fn in funcs:
        rva, end = fn["rva"], fn["end"]
        lab = label(rva)
        ev[lab] += 1
        ev_bytes[lab] += fn["size"]
        if fn["size"] <= 0:
            continue
        found = None
        for ins in md.disasm(tdata[rva - tlo:end - tlo], base + rva):
            if ins.mnemonic in IMPOSSIBLE:
                found = ins.mnemonic
                break
        if found:
            bad.append({"rva": rva, "size": fn["size"],
                        "label": lab, "insn": found})

    print("\ncandidates by evidence:")
    for k in ("CALLED", "VTABLE", "JUMPED", "ORPHAN"):
        print("   %-8s %6d fn  %9d B  (%.1f%% of count, %.1f%% of bytes)"
              % (k, ev[k], ev_bytes[k],
                 100.0 * ev[k] / len(funcs),
                 100.0 * ev_bytes[k] / max(1, sum(ev_bytes.values()))))

    print("\ncandidates containing an instruction MSVC cannot emit: %d"
          % len(bad))
    cross = collections.Counter(b["label"] for b in bad)
    cross_bytes = collections.Counter()
    for b in bad:
        cross_bytes[b["label"]] += b["size"]
    for k in ("CALLED", "VTABLE", "JUMPED", "ORPHAN"):
        share = 100.0 * cross[k] / max(1, len(bad))
        print("   %-8s %5d  (%.1f%% of them)  %d B" % (k, cross[k], share,
                                                       cross_bytes[k]))

    print("\n   -> PREDICTION was: these are ORPHAN, i.e. data, not functions.")
    orphan_share = 100.0 * cross["ORPHAN"] / max(1, len(bad))
    called_share = 100.0 * cross["CALLED"] / max(1, len(bad))
    if orphan_share >= 90.0:
        print("   -> HOLDS (%.1f%% orphan, %.1f%% called). Drop them from the"
              % (orphan_share, called_share))
        print("      denominator; they are not emitter work.")
    elif called_share >= 25.0:
        print("   -> FAILS (%.1f%% are called directly). They are real code."
              % called_share)
    else:
        print("   -> PARTIAL (%.1f%% orphan, %.1f%% called). Mixed; inspect."
              % (orphan_share, called_share))

    top = collections.Counter(b["insn"] for b in bad)
    print("\n   first impossible instruction seen:")
    for m, c in top.most_common(12):
        print("      %-8s %5d" % (m, c))

    print("\n   largest such candidates:")
    for b in sorted(bad, key=lambda x: -x["size"])[:10]:
        print("      %08x  %8d B  %-7s %s"
              % (b["rva"], b["size"], b["label"], b["insn"]))

    keep = [f for f in funcs
            if f["rva"] not in {b["rva"] for b in bad if b["label"] == "ORPHAN"}]
    print("\ncandidates after dropping orphaned data: %d (was %d)"
          % (len(keep), len(funcs)))

    next_out = OUT + ".next"
    with open(next_out, "w", encoding="utf-8") as f:
        json.dump({"called": sorted(called), "jumped": sorted(jumped),
                   "ptrs": sorted(ptrs), "data_like": bad,
                   "evidence": {k: ev[k] for k in ev}}, f)
    final_functions_recipe_id, _ = F.contract_recipe()
    final_functions_manifest = BC.require_manifest(
        F.MANIFEST,
        "functions",
        final_functions_recipe_id,
        {"functions.json": FUNCS},
    )
    final_recipe_id, _ = contract_recipe(
        final_functions_manifest["output_set_id"]
    )
    if final_recipe_id != recipe_id:
        os.remove(next_out)
        raise BC.RecipeMismatchError(
            "bounds inputs changed while analysis was running"
        )
    os.replace(next_out, OUT)
    output_dir = os.path.dirname(OUT)
    outputs = BC.snapshot(output_dir, [os.path.basename(OUT)])
    BC.write_manifest_atomic(
        MANIFEST,
        BC.make_manifest("bounds", recipe_id, recipe_data, outputs),
    )
    print("written: %s" % OUT)
    print("contract: bounds:%s  outputs:%s"
          % (recipe_id[:16], BC.set_id(outputs)[:16]))


def main():
    with BC.exclusive_lock(os.path.join(os.path.dirname(OUT), "pipeline.lock")):
        return _main()


if __name__ == "__main__":
    sys.exit(main())
