#!/usr/bin/env python3
"""Host tests for vita_text_layout.py (ISAAC_VITA_LAYOUT_HUB).

Covers the order-file reader and half arrangement, the fail-closed default
script patching, the map parser + structural check, the reach model and the
edge census (headroom) against a synthetic map/relocation pair, and the order
derivation on that pair.
"""
from __future__ import annotations

import os
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import vita_text_layout as layout  # noqa: E402

MB = 1 << 20

# The vitasdk GNU ld 2.34 default script, reduced to what the generator touches
# plus neighbours that must survive verbatim.
DEFAULT_SCRIPT = """OUTPUT_FORMAT("elf32-littlearm", "elf32-bigarm",
\t      "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)
SECTIONS
{
  PROVIDE (__executable_start = SEGMENT_START("text-segment", 0x81000000)); . = SEGMENT_START("text-segment", 0x81000000);
  .plt            : { *(.plt) }
  .iplt           : { *(.iplt) }
""" + layout.DEFAULT_TEXT_BLOCK + """  .fini           :
  {
    KEEP (*(SORT_NONE(.fini)))
  }
  . = ALIGN(0x10000);
  _end = .; PROVIDE (end = .);
}
"""

LD_VERBOSE = "GNU ld (GNU Tools for ARM Embedded Processors) 2.34\n  Supported emulations:\n   armelf\nusing internal linker script:\n==================================================\n" + DEFAULT_SCRIPT + "\n==================================================\n"


def expect(condition: bool, label: str) -> None:
    if not condition:
        raise AssertionError(label)


def must_fail(label: str, callback, needle: str) -> None:
    try:
        callback()
    except layout.LayoutError as exc:
        if needle not in str(exc):
            raise AssertionError(f"{label}: wrong failure: {exc}") from exc
    else:
        raise AssertionError(f"{label}: hostile input accepted")


def order_tests(root: Path) -> None:
    root.mkdir(parents=True, exist_ok=True)
    order = root / "order.txt"
    order.write_text("# c\n[before-hub]\nguest_0002\nguest_0000\n\n[after-hub]\nguest_0001\n",
                     encoding="ascii")
    before, after = layout.read_order(str(order))
    expect(before == ["guest_0002", "guest_0000"] and after == ["guest_0001"], "groups")

    units = ["guest_0000", "guest_0001", "guest_0002", "guest_0003", "guest_0004", "guest_0005"]
    half_a, half_b, unknown, stale = layout.arrange(units, before, after)
    expect(half_a == ["guest_0002", "guest_0000", "guest_0003", "guest_0005"], f"A: {half_a}")
    expect(half_b == ["guest_0004", "guest_0001"], f"unknown units hug the hub: {half_b}")
    expect(unknown == ["guest_0003", "guest_0004", "guest_0005"], "unknown list")
    expect(stale == [], "no stale")
    half_a, half_b, unknown, stale = layout.arrange(["guest_0000", "guest_0001"], before, after)
    expect(half_a == ["guest_0000"] and half_b == ["guest_0001"] and stale == ["guest_0002"],
           "stale entry skipped")

    order.write_text("guest_0000\n[before-hub]\n[after-hub]\n", encoding="ascii")
    must_fail("unit outside group", lambda: layout.read_order(str(order)), "outside a group")
    order.write_text("[before-hub]\nguest_0000\nguest_0000\n[after-hub]\nguest_0001\n",
                     encoding="ascii")
    must_fail("duplicate", lambda: layout.read_order(str(order)), "duplicate")
    order.write_text("[before-hub]\nmain\n[after-hub]\nguest_0001\n", encoding="ascii")
    must_fail("bad name", lambda: layout.read_order(str(order)), "not a generated unit")
    order.write_text("[before-hub]\nguest_0001\n", encoding="ascii")
    must_fail("empty half", lambda: layout.read_order(str(order)), "both")
    must_fail("duplicate build unit",
              lambda: layout.arrange(["guest_0000", "guest_0000"], before, after), "duplicate")
    expect(layout.unit_name_from_source("/b/isaac-generated-overrides/direct-default/guest_0144.c")
           == "guest_0144", "override path yields the unit name")
    expect(layout.unit_name_from_source("/g/guest_stubs.c") is None
           and layout.unit_name_from_source("/g/guest_table.c") is None,
           "generated hub sources are not cold units")
    must_fail("non-unit source", lambda: layout.unit_name_from_source("/x/guest.c"),
              "unexpected name")
    must_fail("non-C source", lambda: layout.unit_name_from_source("/x/guest_0001.h"),
              "not a C source")


def script_tests() -> None:
    default = layout.extract_default_script(LD_VERBOSE)
    expect(default.rstrip("\n") == DEFAULT_SCRIPT.rstrip("\n"),
           "fenced script extracted verbatim")
    text = layout.render_script(default, ["guest_0002", "guest_0000"], ["guest_0001"], ".c.obj")
    lines = text.splitlines()
    expect(layout.DEFAULT_TEXT_BLOCK not in text, "default .text block replaced")
    expect("    *guest_0002.c.obj(.text .text.[!s]* .text.s[!o]*)" in lines, "cold A pattern")
    expect("    *guest_0001.c.obj(.text .text.*)" in lines, "cold B pattern")
    i_a2 = lines.index("    *guest_0002.c.obj(.text .text.[!s]* .text.s[!o]*)")
    i_a0 = lines.index("    *guest_0000.c.obj(.text .text.[!s]* .text.s[!o]*)")
    i_sorted = lines.index("    *(SORT(.text.sorted.*))")
    i_hub = lines.index("    EXCLUDE_FILE (*guest_[0-9][0-9][0-9][0-9].c.obj) *(.text .stub .text.* .gnu.linkonce.t.*)")
    i_b1 = lines.index("    *guest_0001.c.obj(.text .text.*)")
    i_tail = lines.index("    *(.text .stub .text.* .gnu.linkonce.t.*)")
    i_unlikely = lines.index("    *(.text.unlikely .text.*_unlikely .text.unlikely.*)")
    expect(i_a2 < i_a0 < i_unlikely < i_sorted < i_hub < i_b1 < i_tail, "block order A, hub, B, tail")
    for marker in layout.MARKERS:
        expect(f"    {marker} = .;" in lines, f"marker {marker}")
    expect(lines.index(f"    {layout.MARK_HUB_LO} = .;") < i_unlikely, "hub_lo precedes preamble")
    expect(i_hub < lines.index(f"    {layout.MARK_HUB_HI} = .;") < i_b1, "hub_hi after catch-all")
    expect("  . = ALIGN(0x10000);" in lines and "  _end = .; PROVIDE (end = .);" in lines,
           "rest of the script preserved")
    expect(text.count("_end = .") == 1, "no marker spells _end")
    expect('OUTPUT_FORMAT("elf32-littlearm"' in text and "ENTRY(_start)" in text, "header kept")

    must_fail("foreign script", lambda: layout.render_script(
        default.replace("*(SORT(.text.sorted.*))\n", ""), ["guest_0000"], ["guest_0001"], ".c.obj"),
        "differs from the audited")
    must_fail("empty half", lambda: layout.render_script(default, [], ["guest_0001"], ".c.obj"),
              "both cold halves")
    must_fail("bad unit", lambda: layout.render_script(default, ["guest.c"], ["guest_0001"], ".c.obj"),
              "not a generated unit")
    must_fail("bad suffix", lambda: layout.render_script(default, ["guest_0000"], ["guest_0001"], ".c.obj)"),
              "unsafe object suffix")
    must_fail("no fences", lambda: layout.extract_default_script(DEFAULT_SCRIPT), "two script fences")


def make_map(path: Path, text_start: int, entries, markers) -> None:
    """entries: (name, address, size, origin); markers: name -> address."""
    total = max(a + s for _n, a, s, _o in entries) - text_start
    lines = ["Linker script and memory map", "",
             f".text           0x{text_start:016x}  0x{total:x}"]
    marker_lines = {a: n for n, a in markers.items()}
    for name, address, size, origin in sorted(entries, key=lambda e: e[1]):
        if address in marker_lines:
            lines.append(f"                0x{address:016x}                {marker_lines.pop(address)} = .")
        if len(name) > 14:
            lines.append(f" {name}")
            lines.append(f"                0x{address:016x}  0x{size:x} {origin}")
        else:
            lines.append(f" {name:<14} 0x{address:016x}  0x{size:x} {origin}")
    for address, name in sorted(marker_lines.items()):
        lines.append(f"                0x{address:016x}                {name} = .")
    lines.append(f".rodata         0x{text_start + total:016x}  0x10")
    lines.append(f"                0x{text_start + total + 16:016x}                _end = .")
    path.write_text("\n".join(lines) + "\n", encoding="ascii")


def check_tests(root: Path) -> None:
    root.mkdir(parents=True, exist_ok=True)
    base = 0x81000040
    gen = "CMakeFiles/isaac_first_arm_fault.dir/gen/{}.c.obj"
    entries = [
        (".text.sub_00000010", base, 0x100, gen.format("guest_0000")),
        (".text.cc_be", base + 0x100, 0x20, gen.format("guest_0000")),
        (".text.unlikely.guest_stack_violation", base + 0x200, 0x40, "CMakeFiles/x/guest.c.obj"),
        (".text.sorted.000000", base + 0x240, 0x80, gen.format("guest_0001")),
        (".text.guest_call", base + 0x2c0, 0x100, "CMakeFiles/x/guest.c.obj"),
        (".text.malloc", base + 0x3c0, 0x40, "/sdk/lib/libc.a(malloc.o)"),
        (".text.sub_00000020", base + 0x400, 0x100, gen.format("guest_0001")),
        (".text", base + 0x500, 0x30, "linker stubs"),
    ]
    markers = {layout.MARK_COLD_A_LO: base, layout.MARK_HUB_LO: base + 0x200,
               layout.MARK_HUB_HI: base + 0x400, layout.MARK_COLD_B_HI: base + 0x500}
    path = root / "good.map"
    make_map(path, base, entries, markers)
    text = layout.parse_map(str(path))
    expect(text.start == base and len(text.sections) == 8, f"map parsed: {len(text.sections)}")
    kinds = [s.kind for s in text.sections]
    expect(kinds == ["GEN", "GEN", "PRE", "HOT", "RT", "LIB", "GEN", "STUB"], f"kinds {kinds}")
    result = layout.check(text, ["guest_0000"], ["guest_0001"], None)
    expect(result["hub_bytes"] == 0x200 and result["cold_a_bytes"] == 0x200
           and result["cold_b_bytes"] == 0x100 and result["tail_bytes"] == 0x30
           and result["stub_bytes"] == 0x30 and result["hot_bytes"] == 0x80, f"stats {result}")
    nm_text = ("81000000 T sub_00000010\n81bea538 t __guest_call_veneer\n"
               "81bea548 t __guest_call_veneer\n82000000 t __sub_00562560_veneer\n"
               "82000010 t __sceClibMemset_from_thumb\n")
    veneers, targets = layout.count_veneers(nm_text)
    expect(veneers == 3 and targets == {"guest_call": 2, "sub_00562560": 1},
           f"veneer census {targets}")
    result = layout.check(text, ["guest_0000"], ["guest_0001"], veneers, None)
    expect(result["veneers"] == 3, "veneers reported")
    must_fail("veneer budget", lambda: layout.check(text, ["guest_0000"], ["guest_0001"], 3, 0),
              "long-branch veneers remain")
    expect(layout.check(text, ["guest_0000"], ["guest_0001"], 0, 0)["veneers"] == 0, "zero passes")
    must_fail("unit in wrong half", lambda: layout.check(text, ["guest_0001"], ["guest_0000"], None),
              "outside cold half")
    must_fail("unassigned unit", lambda: layout.check(text, ["guest_0000"], ["guest_0002"], None),
              "not assigned")

    bad = [e for e in entries]
    bad[3] = (".text.sorted.000000", base + 0x180, 0x80, gen.format("guest_0001"))
    bad[1] = (".text.cc_be", base + 0x100, 0x20, gen.format("guest_0000"))
    make_map(root / "hot_outside.map", base, bad, markers)
    must_fail("hot outside hub",
              lambda: layout.check(layout.parse_map(str(root / "hot_outside.map")),
                                   ["guest_0000"], ["guest_0001"], None), "outside the hub")
    make_map(root / "nomarkers.map", base, entries, {})
    must_fail("markers", lambda: layout.check(layout.parse_map(str(root / "nomarkers.map")),
                                              ["guest_0000"], ["guest_0001"], None),
              "lacks layout markers")

    far = [
        (".text.sub_00000010", base, 9 * MB, gen.format("guest_0000")),
        (".text.sorted.000000", base + 9 * MB, 4 * MB, gen.format("guest_0001")),
        (".text.guest_call", base + 13 * MB, MB, "CMakeFiles/x/guest.c.obj"),
        (".text.sub_00000020", base + 14 * MB, 9 * MB, gen.format("guest_0001")),
    ]
    far_markers = {layout.MARK_COLD_A_LO: base, layout.MARK_HUB_LO: base + 9 * MB,
                   layout.MARK_HUB_HI: base + 14 * MB, layout.MARK_COLD_B_HI: base + 23 * MB}
    make_map(root / "far.map", base, far, far_markers)
    result = layout.check(layout.parse_map(str(root / "far.map")), ["guest_0000"], ["guest_0001"], None)
    expect(result["forward_slack"] == layout.THM2_MAX_FWD_BRANCH_OFFSET - 14 * MB, "forward slack")
    too_far = [(n, a, (s if n != ".text.sub_00000020" else 12 * MB), o) for n, a, s, o in far]
    far_markers[layout.MARK_COLD_B_HI] = base + 26 * MB
    make_map(root / "toofar.map", base, too_far, far_markers)
    must_fail("hub out of reach",
              lambda: layout.check(layout.parse_map(str(root / "toofar.map")),
                                   ["guest_0000"], ["guest_0001"], None), "out of Thumb-2 reach")


def make_relocs(path: Path, edges) -> None:
    lines = ["Relocation section '.rel.text' at offset 0x1 contains 3 entries:",
             " Offset     Info    Type                Sym.Value  Sym. Name"]
    for site, target, name in edges:
        lines.append(f"{site:08x}  0000000a R_ARM_THM_CALL         {target | 1:08x}   {name}")
    lines.append("00000000  00000002 R_ARM_ABS32            00000000   data")
    path.write_text("\n".join(lines) + "\n", encoding="ascii")


def model_tests(root: Path) -> None:
    root.mkdir(parents=True, exist_ok=True)
    base = 0x81000040
    gen = "CMakeFiles/isaac_first_arm_fault.dir/gen/{}.c.obj"
    unit = int(5.5 * MB)
    # Natural order u0 u1 [hub 1 MB] u2 u3 (23 MB): pairs two units apart are
    # 17.5 MB apart.  Edges: u0<->u2 and u1<->u3 (far in natural order), u1->u0
    # (near), u0->hub (near), one weak-undefined target (ignored).
    entries = [
        (".text.sub_00000010", base, unit, gen.format("guest_0000")),
        (".text.sub_00000020", base + unit, unit, gen.format("guest_0001")),
        (".text.guest_call", base + 2 * unit, MB, "CMakeFiles/x/guest.c.obj"),
        (".text.sub_00000030", base + 2 * unit + MB, unit, gen.format("guest_0002")),
        (".text.sub_00000040", base + 3 * unit + MB, unit, gen.format("guest_0003")),
    ]
    make_map(root / "model.map", base, entries, {})
    u0, u1, hub, u2, u3 = (e[1] for e in entries)
    edges = [
        (u0 + MB // 10, u2 + 5 * MB, "sub_00000030"),       # u0 -> u2: 16.9 MB
        (u2 + 5 * MB, u0 + MB // 5, "sub_00000010"),        # u2 -> u0: 16.8 MB
        (u1 + MB // 10, u3 + 5 * MB + MB // 4, "sub_00000040"),  # u1 -> u3: 17.15 MB
        (u3 + 5 * MB + MB // 4, u1 + MB // 5, "sub_00000020"),   # u3 -> u1
        (u1 + MB // 5, u0 + 3 * MB // 10, "sub_00000010"),  # u1 -> u0 near
        (u0 + MB // 2, hub + MB // 2, "guest_call"),        # u0 -> hub, 11 MB
        (u0 + MB, 0, "__register_frame_info"),              # weak undefined, ignored
    ]
    make_relocs(root / "model.rel", edges)
    text = layout.parse_map(str(root / "model.map"))
    relocs = layout.read_branch_relocs(str(root / "model.rel"))
    expect(len(relocs) == 7, "branch relocs read")
    model = layout.ReachModel(text, relocs)
    expect(len(model.edges) == 6, "weak undefined edge dropped")
    natural = list(range(len(model.sections)))
    stubbed = model.stubbed(natural, base)
    expect(sorted(s for _a, _b, s in stubbed) ==
           ["sub_00000010", "sub_00000020", "sub_00000030", "sub_00000040"],
           f"four far edges in natural order: {stubbed}")

    # Edge census: the map's own addresses (census_of_link) and the model's
    # placement of the natural order must agree edge for edge.
    linked = layout.census_of_link(text, relocs)
    modelled = model.census(natural, base)
    expect(linked.summary() == modelled.summary(), f"census agree: {linked.summary()} {modelled.summary()}")
    summary = linked.summary()
    expect(summary["edges"] == 6 and summary["beyond_reach"] == 4, f"census totals {summary}")
    expect(summary["cold_cold_edges"] == 5 and summary["cold_cold_beyond"] == 4
           and summary["hub_edges"] == 1 and summary["hub_beyond"] == 0, f"census classes {summary}")
    # u1 -> u3 is the farthest cold<->cold edge: (u3 + 5.25 MB) - (u1 + 0.1 MB).
    far_offset = (u3 + 5 * MB + MB // 4) - (u1 + MB // 10)
    expect(summary["cold_cold_headroom"] == layout.THM2_MAX_FWD_BRANCH_OFFSET - far_offset
           and summary["cold_cold_headroom"] < 0, f"cold<->cold headroom {summary}")
    expect(summary["hub_headroom"] == layout.THM2_MAX_FWD_BRANCH_OFFSET - (hub - u0)
           and summary["hub_headroom"] > 0, f"hub headroom {summary}")
    lines = linked.worst_lines()
    expect(len(lines) == 2 and lines[0].startswith("worst cold<->cold edge: guest_0001 .text.sub_00000020+0x")
           and "-> sub_00000040 (guest_0003)" in lines[0] and "headroom -" in lines[0], f"worst lines {lines}")
    expect(lines[1].startswith("worst hub edge: guest_0000 .text.sub_00000010+0x")
           and "-> guest_call (RT)" in lines[1], f"hub line {lines[1]}")
    backward = layout.EdgeCensus.headroom_of(layout.THM2_MAX_BWD_BRANCH_OFFSET - 4)
    expect(backward == -4 and layout.EdgeCensus.headroom_of(layout.THM2_MAX_FWD_BRANCH_OFFSET) == 0,
           "headroom is measured against the nearer limit")
    logs: list[str] = []
    half_a, half_b, summary = layout.derive_order(text, relocs, margin=0, iterations=2000,
                                                  seed=1, log=logs.append)
    expect(summary["natural_exact"] == 4, f"natural exact {summary}")
    expect(summary["optimised_exact"] == 0 and summary["optimised_margin"] == 0
           and summary["margin"] == 0, f"optimised exact {summary}")
    expect(summary["cold_cold_headroom"] >= 0 and summary["hub_headroom"] > 0,
           f"optimised order reports its headroom {summary}")
    expect(any("worst cold<->cold edge" in line for line in logs), "worst edge logged")

    # Margin ladder: 16 MiB leaves no reach at all and 8 MiB (reach 8.8 MB) is
    # still less than the 10.4 MB an adjacent far pair needs, so the search
    # must halve twice and settle at 4 MiB with every edge in reach.
    ladder_logs: list[str] = []
    half_a2, half_b2, summary2 = layout.derive_order(text, relocs, margin=16 * MB,
                                                     iterations=2000, seed=1,
                                                     log=ladder_logs.append)
    expect(summary2["margin"] == 4 * MB and summary2["optimised_exact"] == 0
           and summary2["optimised_margin"] == 0, f"ladder settles at 4 MiB: {summary2}")
    expect(summary2["cold_cold_headroom"] >= 4 * MB, f"ladder headroom {summary2}")
    expect(sum("retrying with margin" in line for line in ladder_logs) == 2, f"two retries: {ladder_logs}")
    expect(sorted(half_a2 + half_b2) == ["guest_0000", "guest_0001", "guest_0002", "guest_0003"],
           "ladder places every unit")
    expect(sorted(half_a + half_b) == ["guest_0000", "guest_0001", "guest_0002", "guest_0003"],
           "all units placed")
    expect(len(half_a) == 2 and len(half_b) == 2, "byte-balanced split")
    expect(any("chosen sequence" in line for line in logs), "progress logged")
    text_order = layout.render_order(half_a, half_b, summary, 0)
    before, after = layout.read_order(str(_write(root / "derived.txt", text_order)))
    expect(before == half_a and after == half_b, "order file round-trips")
    expect(f"worst cold<->cold edge headroom {summary['cold_cold_headroom']} B" in text_order
           and "isaac_vita_layout_order" in text_order, "order file states its growth budget")
    must_fail("no relocs", lambda: layout.read_branch_relocs(str(_write(root / "empty.rel", "x\n"))),
              "no Thumb branch relocations")


def _write(path: Path, text: str) -> Path:
    path.write_text(text, encoding="ascii")
    return path


def cli_tests(root: Path) -> None:
    root.mkdir(parents=True, exist_ok=True)
    verbose = root / "ld-verbose.txt"
    verbose.write_text(LD_VERBOSE, encoding="ascii")
    order = root / "order.txt"
    order.write_text("[before-hub]\nguest_0001\n[after-hub]\nguest_0000\n", encoding="ascii")
    units = root / "units.txt"
    units.write_text("/g/guest_0000.c\n/g/guest_0001.c\n/g/guest_stubs.c\n/g/guest_0002.c\n"
                     "/g/guest_table.c\n", encoding="ascii")
    out = root / "layout.ld"
    halves = root / "halves.txt"
    rc = layout.main(["script", "--order", str(order), "--units-file", str(units),
                      "--ld-verbose-file", str(verbose), "--output", str(out),
                      "--write-halves", str(halves)])
    expect(rc == 0, "script cli")
    text = out.read_text(encoding="ascii")
    expect("*guest_0001.c.obj(.text .text.[!s]* .text.s[!o]*)" in text
           and "*guest_0002.c.obj(.text .text.[!s]* .text.s[!o]*)" in text
           and "*guest_0000.c.obj(.text .text.*)" in text, "cli placed the unknown unit next to the hub")
    before, after = layout.read_order(str(halves))
    expect(before == ["guest_0001", "guest_0002"] and after == ["guest_0000"], "halves file")
    rc = layout.main(["script", "--order", str(order), "--units-file", str(units),
                      "--ld-verbose-file", str(root / "missing.txt"), "--output", str(out)])
    expect(rc == 1, "missing input fails closed")
    before_mtime = out.stat().st_mtime_ns
    rc = layout.main(["script", "--order", str(order), "--units-file", str(units),
                      "--ld-verbose-file", str(verbose), "--output", str(out),
                      "--write-halves", str(halves)])
    expect(rc == 0 and out.stat().st_mtime_ns == before_mtime, "unchanged output is not rewritten")
    good_map = root / "good.map"
    base = 0x81000040
    gen = "CMakeFiles/isaac_first_arm_fault.dir/gen/{}.c.obj"
    make_map(good_map, base, [
        (".text.sub_00000010", base, 0x100, gen.format("guest_0001")),
        (".text.sub_00000011", base + 0x100, 0x100, gen.format("guest_0002")),
        (".text.sorted.000000", base + 0x200, 0x80, gen.format("guest_0000")),
        (".text.guest_call", base + 0x280, 0x100, "CMakeFiles/x/guest.c.obj"),
        (".text.sub_00000020", base + 0x380, 0x100, gen.format("guest_0000")),
    ], {layout.MARK_COLD_A_LO: base, layout.MARK_HUB_LO: base + 0x200,
        layout.MARK_HUB_HI: base + 0x380, layout.MARK_COLD_B_HI: base + 0x480})
    nm_out = root / "nm.txt"
    nm_out.write_text("81000040 T sub_00000010\n", encoding="ascii")
    rc = layout.main(["check", "--map", str(good_map), "--order", str(halves),
                      "--nm-output", str(nm_out), "--max-veneers", "0"])
    expect(rc == 0, "check cli passes on the resolved halves")
    nm_out.write_text("81000040 T sub_00000010\n81000400 t __malloc_veneer\n", encoding="ascii")
    rc = layout.main(["check", "--map", str(good_map), "--order", str(halves),
                      "--nm-output", str(nm_out), "--max-veneers", "0"])
    expect(rc == 1, "check cli fails on a veneer over budget")
    rc = layout.main(["check", "--map", str(good_map), "--order", str(halves), "--nm", "x"])
    expect(rc == 1, "--nm without --elf fails closed")
    rc = layout.main(["check", "--map", str(good_map), "--order", str(halves), "--readelf", "x"])
    expect(rc == 1, "--readelf without --elf fails closed")

    # check --relocs measures the linked edges: sub_00000010 (unit 1, A) -> sub_00000020
    # (unit 0, B) across the hub, and a hub call.
    nm_out.write_text("81000040 T sub_00000010\n", encoding="ascii")
    make_relocs(root / "good.rel", [
        (base + 0x10, base + 0x380 + 0x20, "sub_00000020"),
        (base + 0x110, base + 0x280 + 0x8, "guest_call"),
    ])
    import contextlib
    import io
    captured = io.StringIO()
    with contextlib.redirect_stdout(captured):
        rc = layout.main(["check", "--map", str(good_map), "--order", str(halves),
                          "--nm-output", str(nm_out), "--max-veneers", "0",
                          "--relocs", str(root / "good.rel"), "--order-file", "/src/order.txt"])
    out_text = captured.getvalue()
    expect(rc == 0, f"check --relocs passes: {out_text}")
    expect("cold_cold_edges=1 cold_cold_beyond=0 cold_cold_headroom=" in out_text
           and "hub_edges=1 hub_beyond=0 hub_headroom=" in out_text
           and "worst cold<->cold edge: guest_0001 .text.sub_00000010+0x10 -> sub_00000020 (guest_0000)" in out_text,
           f"check prints the headroom census: {out_text}")
    nm_out.write_text("81000040 T sub_00000010\n81000400 t __malloc_veneer\n", encoding="ascii")
    err = io.StringIO()
    with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
        rc = layout.main(["check", "--map", str(good_map), "--order", str(halves),
                          "--nm-output", str(nm_out), "--max-veneers", "0",
                          "--order-file", "/src/order.txt"])
    expect(rc == 1 and "regenerate /src/order.txt from this link" in err.getvalue()
           and "isaac_vita_layout_order" in err.getvalue(), f"recovery hint names the order file: {err.getvalue()}")

    # order cli: --relocs or --readelf/--elf, and rc 2 when no order can bring
    # every cold<->cold edge into reach.
    rc = layout.main(["order", "--map", str(good_map), "--output", str(root / "o.txt")])
    expect(rc == 1, "order without relocs fails closed")
    unit = int(5.5 * MB)
    gen = "CMakeFiles/isaac_first_arm_fault.dir/gen/{}.c.obj"
    entries = [
        (".text.sub_00000010", base, unit, gen.format("guest_0000")),
        (".text.sub_00000020", base + unit, unit, gen.format("guest_0001")),
        (".text.guest_call", base + 2 * unit, MB, "CMakeFiles/x/guest.c.obj"),
        (".text.sub_00000030", base + 2 * unit + MB, unit, gen.format("guest_0002")),
        (".text.sub_00000040", base + 3 * unit + MB, unit, gen.format("guest_0003")),
    ]
    make_map(root / "hard.map", base, entries, {})
    starts = [e[1] for e in entries if "guest_" in e[3]]
    names = ["sub_00000010", "sub_00000020", "sub_00000030", "sub_00000040"]
    edges = [(s + 4, t + unit - 4, names[j]) for i, s in enumerate(starts)
             for j, t in enumerate(starts) if i != j]
    make_relocs(root / "hard.rel", edges)
    err = io.StringIO()
    with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
        rc = layout.main(["order", "--map", str(root / "hard.map"), "--relocs", str(root / "hard.rel"),
                          "--margin", "0", "--iterations", "500", "--output", str(root / "hard.txt")])
    expect(rc == 2 and "cannot be made veneer-free" in err.getvalue(), f"order rc 2: {rc} {err.getvalue()}")
    before, after = layout.read_order(str(root / "hard.txt"))
    expect(sorted(before + after) == ["guest_0000", "guest_0001", "guest_0002", "guest_0003"],
           "best-effort order still written")


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        order_tests(root / "order")
        script_tests()
        check_tests(root / "check")
        model_tests(root / "model")
        cli_tests(root / "cli")
    print("test_vita_text_layout: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
