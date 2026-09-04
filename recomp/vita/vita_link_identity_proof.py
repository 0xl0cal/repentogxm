#!/usr/bin/env python3
"""Per-function identity proof between two -Wl,-q links of the same objects (ISAAC_VITA_LAYOUT_HUB).

Usage: vita_link_identity_proof.py <nm> <readelf> <reference.elf> <candidate.elf> [class-prefixes]

For every function symbol (nm -nS, types T/t/W) present in both ELFs, compare
the bytes of the function with every relocated field masked (the -q output
keeps one relocation per patched field), and require the relocation sequence
(type, symbol, offset-in-function) to be identical.  Any byte outside a
relocated field that differs is a codegen difference and fails the proof.
Expected, explainable differences between two links of identical objects:
linker-synthesized code (``__*_veneer`` long-branch stubs, ``__*_from_thumb``
interworking stubs) and Cortex-A8 erratum branch redirects (compare two
``--no-fix-cortex-a8`` links to exclude the latter).  Duplicate-named local
symbols (out-of-line guest.h helpers) are compared as multisets.
"""
import bisect
import collections
import struct
import subprocess
import sys

RELOC_WIDTH = {
    "R_ARM_THM_CALL": 4, "R_ARM_THM_JUMP24": 4, "R_ARM_ABS32": 4,
    "R_ARM_THM_MOVW_ABS_NC": 4, "R_ARM_THM_MOVT_ABS": 4, "R_ARM_PREL31": 4,
    "R_ARM_TARGET1": 4, "R_ARM_TARGET2": 4, "R_ARM_THM_JUMP11": 2,
    "R_ARM_REL32": 4, "R_ARM_CALL": 4, "R_ARM_JUMP24": 4, "R_ARM_THM_JUMP8": 2,
    "R_ARM_NONE": 0, "R_ARM_THM_PC8": 2, "R_ARM_THM_PC12": 4,
}


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def load_elf(path):
    data = open(path, "rb").read()
    # program headers -> address to file offset
    (e_phoff, e_shoff, _flags, _ehsize, e_phentsize, e_phnum) = struct.unpack_from("<IIIHHH", data, 28)
    segs = []
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, _p_paddr, p_filesz = struct.unpack_from("<IIIII", data, e_phoff + i * e_phentsize)
        if p_type == 1 and p_filesz:
            segs.append((p_vaddr, p_offset, p_filesz))
    segs.sort()
    return data, segs


def addr_to_off(segs, addr):
    for vaddr, off, size in segs:
        if vaddr <= addr < vaddr + size:
            return off + (addr - vaddr)
    return None


def load_symbols(nm, path):
    syms = {}
    dups = collections.defaultdict(list)
    for line in run([nm, "-nS", path]).splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[2] in ("T", "t", "W"):
            dups[parts[3]].append((int(parts[0], 16), int(parts[1], 16)))
    for name, entries in dups.items():
        if len(entries) == 1:
            syms[name] = entries[0]
    return syms, {n: e for n, e in dups.items() if len(e) > 1}


def load_relocs(readelf, path):
    relocs = []
    for line in run([readelf, "-rW", path]).splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[2].startswith("R_ARM_"):
            name = parts[4] if len(parts) >= 5 else ""
            relocs.append((int(parts[0], 16), parts[2], name))
    relocs.sort()
    return relocs


def main():
    nm, readelf, a_path, b_path = sys.argv[1:5]
    sample_prefixes = sys.argv[5] if len(sys.argv) > 5 else "sub_,guest_"
    a_data, a_segs = load_elf(a_path)
    b_data, b_segs = load_elf(b_path)
    a_syms, a_dups = load_symbols(nm, a_path)
    b_syms, b_dups = load_symbols(nm, b_path)
    a_rel = load_relocs(readelf, a_path)
    b_rel = load_relocs(readelf, b_path)
    a_rel_addr = [r[0] for r in a_rel]
    b_rel_addr = [r[0] for r in b_rel]
    common = sorted(set(a_syms) & set(b_syms))
    only_a = sorted(set(a_syms) - set(b_syms))
    only_b = sorted(set(b_syms) - set(a_syms))
    stats = collections.Counter()
    failures = []
    per_class = collections.Counter()

    def relocs_in(rel, addrs, lo, hi):
        i = bisect.bisect_left(addrs, lo)
        out = []
        while i < len(rel) and rel[i][0] < hi:
            out.append(rel[i])
            i += 1
        return out

    for name in common:
        a_addr, a_size = a_syms[name]
        b_addr, b_size = b_syms[name]
        cls = next((p for p in sample_prefixes.split(",") if name.startswith(p)), "other")
        if a_size != b_size:
            failures.append((name, "size", a_size, b_size))
            continue
        if a_size == 0:
            stats["zero_size"] += 1
            continue
        a_off = addr_to_off(a_segs, a_addr)
        b_off = addr_to_off(b_segs, b_addr)
        if a_off is None or b_off is None:
            failures.append((name, "unmapped", a_off, b_off))
            continue
        a_bytes = bytearray(a_data[a_off:a_off + a_size])
        b_bytes = bytearray(b_data[b_off:b_off + b_size])
        ra = relocs_in(a_rel, a_rel_addr, a_addr, a_addr + a_size)
        rb = relocs_in(b_rel, b_rel_addr, b_addr, b_addr + b_size)
        sig_a = [(r[0] - a_addr, r[1], r[2]) for r in ra]
        sig_b = [(r[0] - b_addr, r[1], r[2]) for r in rb]
        if sig_a != sig_b:
            failures.append((name, "reloc-sequence", len(sig_a), len(sig_b)))
            continue
        for off, typ, _sym in sig_a:
            w = RELOC_WIDTH.get(typ)
            if w is None:
                failures.append((name, "unknown-reloc", typ, 0))
                break
            for k in range(w):
                if off + k < a_size:
                    a_bytes[off + k] = 0
                    b_bytes[off + k] = 0
        else:
            if a_bytes == b_bytes:
                stats["identical"] += 1
                per_class[cls] += 1
                stats["relocated_fields"] += len(sig_a)
                stats["bytes_compared"] += a_size
            else:
                diff = next(i for i in range(a_size) if a_bytes[i] != b_bytes[i])
                failures.append((name, "bytes", diff, a_size))
    # duplicate-named local symbols (out-of-line static helpers): compare as multisets of masked bytes
    def masked_multiset(data, segs, rel, addrs, entries):
        out = []
        for addr, size in entries:
            off = addr_to_off(segs, addr)
            body = bytearray(data[off:off + size])
            for r in relocs_in(rel, addrs, addr, addr + size):
                w = RELOC_WIDTH.get(r[1], 4)
                for k in range(w):
                    if r[0] - addr + k < size:
                        body[r[0] - addr + k] = 0
            out.append((size, bytes(body)))
        return sorted(out)
    dup_names = sorted(set(a_dups) | set(b_dups))
    dup_ok = 0
    for name in dup_names:
        ea = a_dups.get(name, [a_syms[name]] if name in a_syms else [])
        eb = b_dups.get(name, [b_syms[name]] if name in b_syms else [])
        if masked_multiset(a_data, a_segs, a_rel, a_rel_addr, ea) == masked_multiset(b_data, b_segs, b_rel, b_rel_addr, eb):
            dup_ok += 1
        else:
            failures.append((name, "duplicate-multiset", len(ea), len(eb)))
    print(f"duplicate-named symbols compared as multisets: {len(dup_names)} identical={dup_ok} names={dup_names}")
    print(f"functions common={len(common)} only_in_a={len(only_a)} only_in_b={len(only_b)}")
    print(f"only_in_a sample: {only_a[:8]}")
    print(f"only_in_b sample: {only_b[:8]}")
    print(f"identical={stats['identical']} zero_size={stats['zero_size']} bytes_compared={stats['bytes_compared']} relocated_fields_masked={stats['relocated_fields']}")
    print(f"identical by class: {dict(per_class)}")
    print(f"failures={len(failures)}")
    for f in failures:
        print("  FAIL", f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
