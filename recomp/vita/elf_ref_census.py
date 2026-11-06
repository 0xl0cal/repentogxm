#!/usr/bin/env python3
"""Reference census of one symbol in a linked Vita ELF (vitasdk links with
-Wl,-q, so every input relocation survives in .rel.* sections).

Counts every relocation whose symbol is one of the named symbols, by
relocation type (calls and tail calls, ABS32 literal-pool / data words,
MOVW/MOVT pairs, ...) and by the function that contains the site, then
cross-checks the call-type relocations against a disassembly of the
executable sections: every bl/blx/b.w/b whose target is the symbol, a
linker veneer for it (`*_veneer`) or its interworking stub
(`__<symbol>_from_thumb`).  Linker-made veneers carry no relocation, so the
disassembly view is the one that sees a call through them.

Used by the S1 gates (the GL recorder admission cache and its
sceKernelFreeMemBlock wrap) of an ARM build on the Linux build host:

    elf_ref_census.py --readelf arm-vita-eabi-readelf --objdump arm-vita-eabi-objdump \\
        ELF sceKernelFreeMemBlock __wrap_sceKernelFreeMemBlock

prints one `census` line per symbol and per (type, function), and a final
`census-check` line; exit status 1 if the two views disagree.  A symbol
absent from the symbol table prints `ABSENT` (expected for the wrap in a
build without one).  A site in a data section names the data object that
holds it (`in=@table`).  --imports adds every imported stub symbol, one line
each.  It prints everything it counts: no filter.

--gate turns the census into a verdict on the FIRST symbol S (the real
import) and prints one `census-gate` line (exit status 1 on FAIL):

  --gate wrapped    a link with `--wrap=S`: __wrap_S is defined exactly once,
                    no relocation (call or address) and no disassembled
                    branch reaches S from outside the --allow functions
                    (default: __wrap_S itself, whose __real_S call is the
                    one legitimate reference), and __wrap_S has at least
                    --min-sites relocation sites (the callers --wrap moved).
  --gate unwrapped  the positive control, a link without the wrap: __wrap_S
                    is not defined and S has at least --min-sites relocation
                    sites, so the census demonstrably sees the callers.

ld.lld emits a --wrap'd reference's relocation against __wrap_S under -q
(measured by test_gl_record_free_wrap.py's fixture).  For GNU ld the
unwrapped/wrapped pair of ARM links is the check: had the relocations kept
the name S, `wrapped` would FAIL, never pass.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import re
import subprocess
import sys

CALL_TYPES = {
    "R_ARM_THM_CALL", "R_ARM_THM_JUMP24", "R_ARM_THM_JUMP19", "R_ARM_CALL",
    "R_ARM_JUMP24", "R_ARM_PC24", "R_ARM_THM_JUMP11", "R_ARM_THM_JUMP8",
}


def run(command: list[str]) -> str:
    done = subprocess.run(command, check=False, capture_output=True, text=True,
                          errors="replace")
    if done.returncode != 0:
        raise SystemExit(f"census: command failed ({done.returncode}): {command!r}\n"
                         f"{done.stderr[-2000:]}")
    return done.stdout


def symbols(readelf: str, elf: str):
    """name -> list of (value, size, type, bind, ndx); sorted FUNC starts;
    sorted OBJECT starts (data sites are attributed to the object)."""
    table: dict[str, list[tuple[int, int, str, str, str]]] = collections.defaultdict(list)
    funcs: list[tuple[int, int, str]] = []
    objects: list[tuple[int, int, str]] = []
    row = re.compile(r"^\s*\d+:\s+([0-9a-fA-F]+)\s+(\d+|0x[0-9a-fA-F]+)\s+(\S+)\s+(\S+)\s+\S+\s+(\S+)\s+(\S+)\s*$")
    for line in run([readelf, "-s", "-W", elf]).splitlines():
        m = row.match(line)
        if not m:
            continue
        value = int(m.group(1), 16)
        size = int(m.group(2), 0)
        kind, bind, ndx, name = m.group(3), m.group(4), m.group(5), m.group(6)
        table[name].append((value, size, kind, bind, ndx))
        if kind == "FUNC" and ndx not in ("UND", "ABS"):
            funcs.append((value & ~1, size, name))
        elif kind == "OBJECT" and ndx not in ("UND", "ABS") and size:
            objects.append((value, size, name))
    funcs.sort()
    objects.sort()
    return table, funcs, objects


def section_names(readelf: str, elf: str) -> dict[str, str]:
    """Section index (as readelf -s prints Ndx) -> section name."""
    out = {}
    row = re.compile(r"^\s*\[\s*(\d+)\]\s+(\S+)")
    for line in run([readelf, "-S", "-W", elf]).splitlines():
        m = row.match(line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def containing(funcs, starts, address: int) -> str:
    i = bisect.bisect_right(starts, address) - 1
    while i >= 0:
        start, size, name = funcs[i]
        if start <= address < start + max(size, 1):
            return name
        if size and address >= start + size:
            break
        i -= 1
    return "?"


def relocations(readelf: str, elf: str, wanted: set[str]):
    """(section, offset, type, symbol) for every relocation naming a wanted symbol."""
    out = []
    section = None
    head = re.compile(r"^Relocation section '([^']+)'")
    row = re.compile(r"^\s*([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+(R_ARM_\S+)\s+[0-9a-fA-F]+\s+(\S+)")
    for line in run([readelf, "-r", "-W", elf]).splitlines():
        m = head.match(line)
        if m:
            section = m.group(1)
            continue
        m = row.match(line)
        if m and m.group(3) in wanted:
            out.append((section, int(m.group(1), 16), m.group(2), m.group(3)))
    return out


def disassembly_calls(objdump: str, elf: str, targets: set[str]):
    """(address, mnemonic, target label) for every branch to a wanted label."""
    out = []
    # GNU objdump: "8100abcd:\tbl\t824a1e90 <sym>"; llvm-objdump:
    # "8100abcd:      \tblx\t0x824a1e90 <sym> @ imm = #...".
    row = re.compile(r"^\s*([0-9a-f]+):\s+(b[a-z.]*)\s+(?:0x)?[0-9a-f]+\s+<([^>+]+)(\+0x[0-9a-f]+)?>")
    needles = tuple(f"<{t}>" for t in targets)
    # Streamed: the whole image disassembles to millions of lines.  The
    # substring test is the filter; the regex then reads only candidate lines.
    with subprocess.Popen([objdump, "-d", "--no-show-raw-insn", elf], stdout=subprocess.PIPE,
                          text=True, errors="replace") as proc:
        assert proc.stdout is not None
        for line in proc.stdout:
            if "<" not in line or not any(n in line for n in needles):
                continue
            m = row.match(line)
            if m and m.group(3) in targets and not m.group(4):
                out.append((int(m.group(1), 16), m.group(2), m.group(3)))
    if proc.returncode != 0:
        raise SystemExit(f"census: {objdump} failed ({proc.returncode})")
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--readelf", default="arm-vita-eabi-readelf")
    parser.add_argument("--objdump", default="arm-vita-eabi-objdump")
    parser.add_argument("--imports", action="store_true",
                        help="also print every imported stub symbol (.vitalink.*), unfiltered")
    parser.add_argument("--gate", choices=("wrapped", "unwrapped"),
                        help="verdict on the first symbol (see the module doc)")
    parser.add_argument("--allow", action="append", default=[],
                        help="--gate wrapped: a function allowed to reference the real symbol "
                             "(repeatable; default __wrap_<symbol>)")
    parser.add_argument("--min-sites", type=int, default=1,
                        help="--gate: minimum relocation sites of the wrap (wrapped) or of the "
                             "symbol (unwrapped)")
    parser.add_argument("elf")
    parser.add_argument("symbol", nargs="+")
    args = parser.parse_args()

    table, funcs, objects = symbols(args.readelf, args.elf)
    starts = [f[0] for f in funcs]
    object_starts = [o[0] for o in objects]
    status = 0
    if args.gate:
        if args.symbol[0].startswith("__wrap_"):
            raise SystemExit("census: --gate takes the real symbol first, not its wrap")
        if f"__wrap_{args.symbol[0]}" not in args.symbol:
            args.symbol.append(f"__wrap_{args.symbol[0]}")
    for name in args.symbol:
        entries = table.get(name, [])
        defined = [e for e in entries if e[4] != "UND"]
        print(f"census symbol {name}: defined={len(defined)} "
              f"{' '.join(f'0x{e[0]:08x}/{e[2]}/{e[3]}/ndx{e[4]}' for e in entries) or 'ABSENT'}")
    wanted = set(args.symbol)
    relocs = relocations(args.readelf, args.elf, wanted)
    by_type = collections.Counter((r[3], r[2]) for r in relocs)
    def site_owner(section: str | None, offset: int) -> str:
        if section and section.startswith(".rel.text"):
            return containing(funcs, starts, offset)
        name = containing(objects, object_starts, offset)
        return "@" + name if name != "?" else containing(funcs, starts, offset)

    by_site = collections.Counter((r[3], r[2], r[0], site_owner(r[0], r[1])) for r in relocs)
    for (sym, kind), n in sorted(by_type.items()):
        print(f"census reloc {sym} {kind} n={n}")
    for (sym, kind, section, func), n in sorted(by_site.items()):
        print(f"census site {sym} {kind} {section} in={func} n={n}")
    for sym in args.symbol:
        total = sum(n for (s, _), n in by_type.items() if s == sym)
        calls = sum(n for (s, k), n in by_type.items() if s == sym and k in CALL_TYPES)
        print(f"census total {sym}: relocs={total} call_relocs={calls} "
              f"address_relocs={total - calls}")

    labels = set()
    for sym in args.symbol:
        labels |= {sym, f"__{sym}_from_thumb", f"__{sym}_veneer", f"{sym}_veneer",
                   f"__{sym}_from_arm"}
    calls = disassembly_calls(args.objdump, args.elf, labels)
    # A veneer/interworking stub is itself a branch to the symbol: its sites
    # are counted under the stub's label and the stub's own branch is excluded.
    stub_names = {f for f in labels if f not in wanted}
    sites = [c for c in calls if containing(funcs, starts, c[0]) not in stub_names]
    stub_branches = len(calls) - len(sites)
    by_label = collections.Counter(c[2] for c in sites)
    for label, n in sorted(by_label.items()):
        print(f"census disasm branch-to {label} n={n}")
    reloc_calls = sum(1 for r in relocs if r[2] in CALL_TYPES and r[0] and
                      r[0].startswith(".rel.text"))
    ok = reloc_calls == len(sites)
    print(f"census-check: call relocs in .rel.text={reloc_calls} disassembly branch sites="
          f"{len(sites)} (+{stub_branches} inside veneer/interworking stubs) "
          f"{'AGREE' if ok else 'DISAGREE'}")
    if not ok:
        status = 1
    if args.gate:
        real = args.symbol[0]
        wrap = f"__wrap_{real}"
        wrap_defined = len([e for e in table.get(wrap, []) if e[4] != "UND"])
        real_sites = sum(n for (sym, _, _, _), n in by_site.items() if sym == real)
        wrap_sites = sum(n for (sym, _, _, _), n in by_site.items() if sym == wrap)
        if args.gate == "wrapped":
            allow = set(args.allow) or {wrap}
            outside_relocs = sum(n for (sym, _, _, func), n in by_site.items()
                                 if sym == real and func not in allow)
            # The disassembly view does not depend on the relocation's symbol
            # name: a branch that still lands on the real stub outside the
            # wrap is caught here even if the relocations were renamed.
            real_labels = {real, f"__{real}_from_thumb", f"__{real}_veneer",
                           f"{real}_veneer", f"__{real}_from_arm"}
            outside_branches = sum(1 for c in sites if c[2] in real_labels and
                                   containing(funcs, starts, c[0]) not in allow)
            verdict = (ok and wrap_defined == 1 and outside_relocs == 0 and
                       outside_branches == 0 and wrap_sites >= args.min_sites)
            print(f"census-gate wrapped {real}: wrap_defined={wrap_defined} "
                  f"real_sites={real_sites} outside_allow(relocs,branches)="
                  f"{outside_relocs},{outside_branches} allow={','.join(sorted(allow))} "
                  f"wrap_sites={wrap_sites} min={args.min_sites} "
                  f"{'PASS' if verdict else 'FAIL'}")
        else:
            verdict = ok and wrap_defined == 0 and real_sites >= args.min_sites
            print(f"census-gate unwrapped {real}: wrap_defined={wrap_defined} "
                  f"real_sites={real_sites} min={args.min_sites} "
                  f"{'PASS' if verdict else 'FAIL'}")
        if not verdict:
            status = 1
    if args.imports:
        sections = section_names(args.readelf, args.elf)
        stubs = sorted((sections.get(e[4], "?"), name)
                       for name, entries in table.items() for e in entries
                       if sections.get(e[4], "").startswith(".vitalink."))
        for section, name in stubs:
            print(f"census import {section} {name}")
        print(f"census imports: {len(stubs)} stub symbols in "
              f"{len({s for s, _ in stubs})} .vitalink.* sections (unfiltered)")
    return status


if __name__ == "__main__":
    sys.exit(main())
