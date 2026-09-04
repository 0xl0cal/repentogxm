"""Function discovery for the recompiler.

Builds the authoritative function table: one entry per function in the
unpacked Windows Repentance build, with a start RVA, an end RVA and -- where a
name can be transferred -- a symbol from the aarch64 Switch build.

Evidence is combined rather than trusted from one source, because each source
alone lies in a different direction:

  * `CC` padding gives starts but misses functions that follow a `ret`+align
    with no padding, and invents starts inside jump tables stored in `.text`;
  * direct `call rel32` targets are certainly function starts, but only for
    functions that somebody calls directly;
  * relocation-backed ctor/dtor callbacks passed to the exact, content-pinned
    MSVC EH vector-constructor iterator are address-taken function starts;
  * addresses stored in `.rdata` (vtable slots) are certainly function starts
    for the virtual ones;
  * the Switch build's symbol table has 14,011 names with sizes, but it is a
    different build, so it corroborates the *count*, not the addresses.

The spike measured 13,998 starts from padding alone against 14,011 Switch
symbols. That agreement is what makes this tractable; this script's job is to
turn it into a table with ends, so a translator can be pointed at one function
and know where to stop.

The MSVC CRT initializer arrays are one deliberate exception to the generic
data-pointer rule below.  They are invoked as functions by `_initterm_e` and
`_initterm`, so every non-null entry is a dispatch root even when it has no CC
padding and no direct CALL.  Their ranges are copied from the real entry-point
pushes (0x5eb703 and 0x5eb729) and are checked against the exact expected
non-null counts before use.

Writes `build/recompiler/functions.json` by default.
"""
import collections
import hashlib
import json
import os
import sys

import capstone
import pefile

import build_contract as BC
import msvc_rtti as RTTI

_DEFAULT_WORKDIR = os.path.abspath(os.environ.get(
    "REPENTOGXM_WORKDIR",
    os.path.join(os.path.dirname(__file__), "..", "build", "recompiler"),
))
EXE = os.path.abspath(os.environ.get(
    "REPENTOGXM_PE",
    os.path.join(_DEFAULT_WORKDIR, "isaac-ng.exe.unpacked.exe"),
))
OUTDIR = _DEFAULT_WORKDIR
OUT = os.path.join(OUTDIR, "functions.json")
MANIFEST = os.path.join(OUTDIR, "functions.manifest.json")
PE_LOGICAL_NAME = "input/isaac-ng.exe.unpacked.exe"

CRT_INITIALIZER_RANGES = (
    (0x0060689C, 0x006068B0, "_initterm_e", 4),
    (0x006066E0, 0x00606890, "_initterm", 107),
)

EXPECTED_RTTI_ONLY_ROOTS = 70
EXPECTED_RTTI_ONLY_SHA256 = \
    "b2df806b84170e9838290e3e11e37a12f90c7c30d850dfee9ca0993fd70f6da6"

# The x86 MSVC EH vector-constructor iterator calls the two callbacks supplied
# in its fourth and fifth stack arguments.  We accept callback roots only at
# direct calls to this exact, content-pinned helper, and only when the callback
# immediate itself has a HIGHLOW relocation.  This deliberately does not turn
# arbitrary immediates that happen to land in .text into function evidence.
EH_VECTOR_CTOR_ITERATOR_RVA = 0x005EB0C8
EH_VECTOR_CTOR_ITERATOR_SIZE = 0x60
EXPECTED_EH_VECTOR_CTOR_ITERATOR_SHA256 = \
    "5787dc0814da6fcd22587264df2c7e74280160b4b0d7f32399b874581ba6e949"
EXPECTED_EH_VECTOR_CTOR_CALLS = 95
EXPECTED_EH_CALLBACK_ROOTS = 104
EXPECTED_EH_CALLBACK_RECORDS_SHA256 = \
    "b9e1f3163cd7497433e483ea926659e971e00ce00b87ef68de063c2a093406fb"
EXPECTED_EH_CALLBACK_ONLY_ROOTS = 8
EXPECTED_EH_CALLBACK_ONLY_SHA256 = \
    "16b527e5e1ecd5fec018ce33a97f576cfa700a2125e765ec5ceffdf690fecab3"
EXPECTED_EH_CALLBACK_PROBE_RVA = 0x002C4600
EXPECTED_EH_CALLBACK_PROBE_SITES = (
    (0x002C67FE, "ctor", 0x002C67E7, 0x002C67E8),
    (0x0051FE9F, "ctor", 0x0051FE88, 0x0051FE89),
)
EXPECTED_EH_CALLBACK_PROBE_EXTENTS = {
    0x002C45E0: 0x002C4600,
    0x002C4600: 0x002C469B,
}


def contract_recipe():
    """Content identity of the inputs that define functions.json."""
    return BC.recipe(
        "functions",
        {
            PE_LOGICAL_NAME: EXE,
            "source/recomp/funcs.py": __file__,
            "source/recomp/msvc_rtti.py": RTTI.__file__,
            "source/recomp/build_contract.py": BC.__file__,
        },
        {"output_schema": 1},
        BC.tool_ids(),
    )


def functions_document(base, size, entry_rva, text_rva, text_end,
                       sections, functions):
    """Return portable metadata for one discovered function corpus."""
    return {
        # A physical PE path is only an input locator.  It must not enter an
        # output whose content hash feeds every downstream recipe.
        "exe": PE_LOGICAL_NAME,
        "image_base": base,
        "image_size": size,
        "entry_rva": entry_rva,
        "text_rva": text_rva,
        "text_end": text_end,
        "sections": [
            {key: value for key, value in section.items() if key != "data"}
            for section in sections
        ],
        "functions": functions,
    }


def hash_eh_callback_records(records):
    raw = "".join(
        "%08x %s %08x %08x %08x\n" % record
        for record in sorted(records)
    ).encode("ascii")
    return hashlib.sha256(raw).hexdigest()


def load():
    pe = pefile.PE(EXE, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    size = pe.OPTIONAL_HEADER.SizeOfImage
    secs = []
    for s in pe.sections:
        nm = s.Name.rstrip(b"\x00").decode("latin1")
        secs.append({
            "name": nm,
            "rva": s.VirtualAddress,
            "vsize": s.Misc_VirtualSize,
            "data": s.get_data(),
            "exec": bool(s.Characteristics & 0x20000000),
        })
    return pe, base, size, secs


def _main():
    os.makedirs(OUTDIR, exist_ok=True)
    recipe_id, recipe_data = contract_recipe()
    if os.path.exists(MANIFEST):
        os.remove(MANIFEST)       # a failed run must not bless the old JSON
    pe, base, size, secs = load()
    text = next(s for s in secs if s["name"] == ".text")
    tlo, thi = text["rva"], text["rva"] + text["vsize"]
    tdata = text["data"]

    def code(rva):
        return tlo <= rva < thi

    def byte(rva):
        return tdata[rva - tlo]

    print("image base 0x%x  size 0x%x" % (base, size))
    print(".text rva 0x%x .. 0x%x  (%d bytes)" % (tlo, thi, thi - tlo))

    iterator = tdata[
        EH_VECTOR_CTOR_ITERATOR_RVA - tlo:
        EH_VECTOR_CTOR_ITERATOR_RVA - tlo + EH_VECTOR_CTOR_ITERATOR_SIZE
    ]
    iterator_hash = hashlib.sha256(iterator).hexdigest()
    if (len(iterator) != EH_VECTOR_CTOR_ITERATOR_SIZE or
            iterator_hash != EXPECTED_EH_VECTOR_CTOR_ITERATOR_SHA256):
        raise RuntimeError(
            "MSVC EH vector ctor iterator changed: %d bytes, sha256 %s"
            % (len(iterator), iterator_hash))

    reloc_directory = \
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]
    pe.parse_data_directories(directories=[reloc_directory])
    if not hasattr(pe, "DIRECTORY_ENTRY_BASERELOC"):
        raise RuntimeError("PE has no base-relocation directory")
    highlow_relocations = set()
    relocation_types = set()
    for block in pe.DIRECTORY_ENTRY_BASERELOC:
        for entry in block.entries:
            relocation_types.add(entry.type)
            if entry.type == pefile.RELOCATION_TYPE["IMAGE_REL_BASED_HIGHLOW"]:
                highlow_relocations.add(entry.rva)
    expected_relocation_types = {
        pefile.RELOCATION_TYPE["IMAGE_REL_BASED_ABSOLUTE"],
        pefile.RELOCATION_TYPE["IMAGE_REL_BASED_HIGHLOW"],
    }
    if relocation_types - expected_relocation_types:
        raise RuntimeError("unexpected x86 relocation types: %s"
                           % sorted(relocation_types - expected_relocation_types))
    if not highlow_relocations:
        raise RuntimeError("PE has no HIGHLOW relocations")

    # ---- evidence 1: CC padding ------------------------------------------
    # MSVC pads between functions with int3. A run of >= 1 CC ending at a
    # non-CC byte marks a start. Require >= 2 to avoid catching a lone int3
    # emitted as a trap inside a function (RNG::Next has exactly one).
    pad_starts = set()
    run = 0
    for i in range(len(tdata)):
        if tdata[i] == 0xCC:
            run += 1
        else:
            if run >= 2:
                pad_starts.add(tlo + i)
            run = 0
    print("starts from CC padding      : %d" % len(pad_starts))

    # ---- evidence 2: direct call targets ---------------------------------
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    call_targets = set()
    jmp_targets = set()
    eh_callback_roots = set()
    eh_callback_records = []
    eh_iterator_calls = 0
    insn_count = 0
    bad_bytes = 0
    # capstone's disasm generator STOPS at the first byte it cannot decode and
    # does not resume. Iterating it once decoded 38,921 instructions of an
    # expected 1.76M -- a contradiction with the spike's own measurement, which
    # is what exposed the defect. Restart past each bad byte.
    off = 0
    n = len(tdata)
    while off < n:
        consumed = 0
        history = []
        for ins in md.disasm(tdata[off:], base + tlo + off):
            insn_count += 1
            consumed += ins.size
            if ins.mnemonic == "call" and len(ins.operands) == 1:
                o = ins.operands[0]
                if o.type == capstone.x86.X86_OP_IMM:
                    t = o.imm - base
                    if code(t):
                        call_targets.add(t)
                    if t == EH_VECTOR_CTOR_ITERATOR_RVA:
                        eh_iterator_calls += 1
                        pushes = [previous for previous in reversed(history)
                                  if previous.mnemonic == "push"][:5]
                        callsite = ins.address - base
                        if len(pushes) != 5:
                            raise RuntimeError(
                                "EH iterator call %08x has only %d visible args"
                                % (callsite, len(pushes)))
                        earliest_push = pushes[-1]
                        window_start = next(
                            index for index, previous in enumerate(history)
                            if previous.address == earliest_push.address)
                        for previous in history[window_start:]:
                            _read, written = previous.regs_access()
                            if (capstone.x86.X86_REG_ESP in written and
                                    previous.mnemonic != "push"):
                                raise RuntimeError(
                                    "EH iterator args at %08x cross ESP writer "
                                    "%08x: %s %s"
                                    % (callsite, previous.address - base,
                                       previous.mnemonic, previous.op_str))
                        # Nearest first: ptr, size, count, ctor, dtor.
                        for role, push in zip(("ctor", "dtor"), pushes[3:5]):
                            if (len(push.operands) != 1 or
                                    push.operands[0].type !=
                                    capstone.x86.X86_OP_IMM or
                                    push.imm_offset <= 0 or push.imm_size != 4):
                                raise RuntimeError(
                                    "EH iterator %s arg at %08x is not imm32"
                                    % (role, push.address - base))
                            field_rva = (push.address - base) + push.imm_offset
                            if field_rva not in highlow_relocations:
                                raise RuntimeError(
                                    "EH iterator %s arg at %08x lacks HIGHLOW"
                                    % (role, field_rva))
                            target = (push.operands[0].imm & 0xFFFFFFFF) - base
                            if not code(target):
                                raise RuntimeError(
                                    "EH iterator %s arg at %08x leaves .text: %08x"
                                    % (role, field_rva,
                                       push.operands[0].imm & 0xFFFFFFFF))
                            eh_callback_roots.add(target)
                            eh_callback_records.append((
                                callsite, role, push.address - base, field_rva,
                                target))
            elif ins.mnemonic == "jmp" and len(ins.operands) == 1:
                o = ins.operands[0]
                if o.type == capstone.x86.X86_OP_IMM:
                    t = o.imm - base
                    if code(t):
                        jmp_targets.add(t)
            mnemonic = ins.mnemonic
            if (mnemonic == "call" or mnemonic.startswith("j") or
                    mnemonic.startswith("loop") or mnemonic.startswith("ret") or
                    mnemonic.startswith("iret") or mnemonic == "int3"):
                history = []
            else:
                history.append(ins)
                if len(history) > 80:
                    del history[:-80]
        if consumed == 0:
            bad_bytes += 1
            off += 1
        else:
            off += consumed
    print("linear sweep instructions   : %d  (%d bytes refused)"
          % (insn_count, bad_bytes))
    print("direct call targets         : %d" % len(call_targets))
    print("direct jmp targets          : %d" % len(jmp_targets))
    if eh_iterator_calls != EXPECTED_EH_VECTOR_CTOR_CALLS:
        raise RuntimeError("EH vector ctor iterator calls changed: %d != %d"
                           % (eh_iterator_calls,
                              EXPECTED_EH_VECTOR_CTOR_CALLS))
    if len(eh_callback_roots) != EXPECTED_EH_CALLBACK_ROOTS:
        raise RuntimeError("EH callback root count changed: %d != %d"
                           % (len(eh_callback_roots),
                              EXPECTED_EH_CALLBACK_ROOTS))
    eh_callback_records_hash = hash_eh_callback_records(eh_callback_records)
    if eh_callback_records_hash != EXPECTED_EH_CALLBACK_RECORDS_SHA256:
        raise RuntimeError("EH callback records hash changed: %s != %s"
                           % (eh_callback_records_hash,
                              EXPECTED_EH_CALLBACK_RECORDS_SHA256))
    probe_sites = tuple(sorted(
        record[:4] for record in eh_callback_records
        if record[4] == EXPECTED_EH_CALLBACK_PROBE_RVA))
    if probe_sites != EXPECTED_EH_CALLBACK_PROBE_SITES:
        raise RuntimeError("EH callback probe sites changed: %r != %r"
                           % (probe_sites, EXPECTED_EH_CALLBACK_PROBE_SITES))
    print("EH vector ctor calls        : %d" % eh_iterator_calls)
    print("EH ctor/dtor slots/roots    : %d / %d"
          % (len(eh_callback_records), len(eh_callback_roots)))

    # ---- evidence 3: pointers stored in data -----------------------------
    data_ptrs = set()
    for s in secs:
        if s["exec"] or s["name"] not in (".rdata", ".data"):
            continue
        d = s["data"]
        for i in range(0, len(d) - 3, 4):
            v = int.from_bytes(d[i:i + 4], "little")
            if base + tlo <= v < base + thi:
                data_ptrs.add(v - base)
    print("distinct .text ptrs in data : %d" % len(data_ptrs))

    # ---- evidence 4: relocation-proved MSVC vftables --------------------
    # A value merely landing in .text is not enough: three UTF-16 strings
    # immediately after real vftables contain five such dwords in this PE.
    # msvc_rtti walks TypeDescriptor -> COL -> CHD -> BCD and requires a
    # HIGHLOW relocation on every absolute pointer and every accepted slot.
    rtti = RTTI.discover(pe, secs)
    vtable_roots = set(rtti.roots)
    print("MSVC RTTI TypeDescriptors  : %d" % rtti.type_descriptors)
    print("MSVC RTTI COL/vftables     : %d" %
          rtti.complete_object_locators)
    print("MSVC RTTI slots/roots      : %d / %d" %
          (rtti.slot_count, len(vtable_roots)))

    # ---- evidence 5: CRT initializer arrays ------------------------------
    crt_starts = set()
    for begin, end, name, expected in CRT_INITIALIZER_RANGES:
        found = []
        if begin & 3 or end & 3 or begin > end:
            raise RuntimeError("invalid %s range %x..%x" % (name, begin, end))
        for rva in range(begin, end, 4):
            raw = pe.get_data(rva, 4)
            if len(raw) != 4:
                raise RuntimeError("%s range leaves the PE at %x" % (name, rva))
            value = int.from_bytes(raw, "little")
            if not value:
                continue
            target = value - base
            if not code(target):
                raise RuntimeError("%s entry %x is not a .text pointer: %08x"
                                   % (name, rva, value))
            found.append(target)
            crt_starts.add(target)
        if len(found) != expected:
            raise RuntimeError("%s has %d non-null roots, expected %d"
                               % (name, len(found), expected))
        print("%-28s: %d roots" % (name + " roots", len(found)))

    # ---- combine ----------------------------------------------------------
    # A call target is a function start beyond argument. Padding starts are
    # strong but not certain. A data pointer may be a vtable slot (a start) or
    # a jump-table entry (an interior label), so it only counts when another
    # source agrees, or when the byte before it is padding.
    starts = set(call_targets)
    starts |= pad_starts
    for p in data_ptrs:
        if p in starts:
            continue
        if p - 1 >= tlo and byte(p - 1) == 0xCC:
            starts.add(p)
    rtti_only = vtable_roots - starts
    if len(rtti_only) != EXPECTED_RTTI_ONLY_ROOTS:
        raise RuntimeError("RTTI-only root count changed: %d != %d"
                           % (len(rtti_only), EXPECTED_RTTI_ONLY_ROOTS))
    rtti_only_hash = RTTI.hash_rvas(rtti_only)
    if rtti_only_hash != EXPECTED_RTTI_ONLY_SHA256:
        raise RuntimeError("RTTI-only root hash changed: %s != %s"
                           % (rtti_only_hash, EXPECTED_RTTI_ONLY_SHA256))
    starts |= vtable_roots
    print("new RTTI-only starts        : %d" % len(rtti_only))
    starts.add(pe.OPTIONAL_HEADER.AddressOfEntryPoint)
    starts = {s for s in starts if code(s)}
    print("generic starts before CRT   : %d" % len(starts))
    print("new CRT-only starts         : %d" % len(crt_starts - starts))
    starts |= crt_starts
    eh_callback_only = eh_callback_roots - starts
    if len(eh_callback_only) != EXPECTED_EH_CALLBACK_ONLY_ROOTS:
        raise RuntimeError("EH-only root count changed: %d != %d"
                           % (len(eh_callback_only),
                              EXPECTED_EH_CALLBACK_ONLY_ROOTS))
    eh_callback_only_hash = RTTI.hash_rvas(eh_callback_only)
    if eh_callback_only_hash != EXPECTED_EH_CALLBACK_ONLY_SHA256:
        raise RuntimeError("EH-only root hash changed: %s != %s"
                           % (eh_callback_only_hash,
                              EXPECTED_EH_CALLBACK_ONLY_SHA256))
    if EXPECTED_EH_CALLBACK_PROBE_RVA not in eh_callback_only:
        raise RuntimeError("EH callback probe is not a new canonical root")
    starts |= eh_callback_roots
    print("new EH-callback-only starts : %d" % len(eh_callback_only))
    print("combined function starts    : %d" % len(starts))

    ordered = sorted(starts)

    # ---- ends -------------------------------------------------------------
    # A function ends at the next start, minus any trailing CC padding.
    funcs = []
    for i, st in enumerate(ordered):
        nxt = ordered[i + 1] if i + 1 < len(ordered) else thi
        e = nxt
        while e > st and tdata[e - 1 - tlo] == 0xCC:
            e -= 1
        funcs.append({"rva": st, "end": e, "size": e - st})

    funcs_by_rva = {f["rva"]: f for f in funcs}
    for rva, expected_end in EXPECTED_EH_CALLBACK_PROBE_EXTENTS.items():
        actual = funcs_by_rva.get(rva)
        if actual is None or actual["end"] != expected_end:
            raise RuntimeError(
                "EH callback probe extent changed at %08x: %r != %08x"
                % (rva, actual, expected_end))

    sizes = [f["size"] for f in funcs]
    sizes.sort()
    total = sum(sizes)
    print("total covered bytes         : %d of %d (%.1f%%)"
          % (total, thi - tlo, 100.0 * total / (thi - tlo)))
    print("size  min/median/max        : %d / %d / %d"
          % (sizes[0], sizes[len(sizes) // 2], sizes[-1]))
    zero = sum(1 for s in sizes if s == 0)
    if zero:
        print("ZERO-SIZE functions         : %d" % zero)

    buckets = collections.Counter()
    for s in sizes:
        if s < 64:
            buckets["<64"] += 1
        elif s < 512:
            buckets["64-512"] += 1
        elif s < 4096:
            buckets["512-4K"] += 1
        else:
            buckets[">4K"] += 1
    for k in ("<64", "64-512", "512-4K", ">4K"):
        n = buckets[k]
        b = sum(s for s in sizes
                if (k == "<64" and s < 64) or (k == "64-512" and 64 <= s < 512)
                or (k == "512-4K" and 512 <= s < 4096) or (k == ">4K" and s >= 4096))
        print("  %-8s %6d fn  %5.1f%% of count  %5.1f%% of bytes"
              % (k, n, 100.0 * n / len(sizes), 100.0 * b / total))

    # ---- cross-check against the Switch symbol table ----------------------
    nxi = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "data", "nx_index.txt")
    if os.path.exists(nxi):
        n = 0
        with open(nxi, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("  0x"):
                    n += 1
        print("Switch indexed functions    : %d  (ours %d, delta %+.2f%%)"
              % (n, len(funcs), 100.0 * (len(funcs) - n) / n if n else 0))

    next_out = OUT + ".next"
    with open(next_out, "w", encoding="utf-8") as f:
        json.dump(functions_document(
            base,
            size,
            pe.OPTIONAL_HEADER.AddressOfEntryPoint,
            tlo,
            thi,
            secs,
            funcs,
        ), f)
    final_recipe_id, _ = contract_recipe()
    if final_recipe_id != recipe_id:
        os.remove(next_out)
        raise BC.RecipeMismatchError(
            "functions inputs changed while discovery was running"
        )
    os.replace(next_out, OUT)
    outputs = BC.snapshot(OUTDIR, [os.path.basename(OUT)])
    BC.write_manifest_atomic(
        MANIFEST,
        BC.make_manifest("functions", recipe_id, recipe_data, outputs),
    )
    print("written                     : %s" % OUT)
    print("contract                    : functions:%s  outputs:%s"
          % (recipe_id[:16], BC.set_id(outputs)[:16]))


def main():
    with BC.exclusive_lock(os.path.join(OUTDIR, "pipeline.lock")):
        return _main()


if __name__ == "__main__":
    sys.exit(main())
