#!/usr/bin/env python3
"""Veneer-free .text layout for ISAAC_VITA_LAYOUT_HUB (pure placement).

The linked .text (23.4 MB with GPR locals) exceeds the Thumb-2 BL/B.W reach of
+/-16 MB, so GNU ld routes every far call through a three-instruction ARM-state
PIC veneer (``ldr ip,[pc,#4]; add ip,pc,ip; bx ip``).  With the default script
the hot region (``.text.sorted.*``) sits at the very start and the libraries at
the very end, so the CRT/STL units at the far end of the corpus reach the hot
region, ``guest_call`` and the allocator only through veneers (33.7k branch
sites, 1,179 stubs in the gpr-v3 link).

This tool rewrites only the ``.text`` block of the toolchain's own default
script (``arm-vita-eabi-ld --verbose``) into

    [cold half A: generated units]
    [hub: .text.unlikely/.exit/.startup/.hot, SORT(.text.sorted.*), every
          non-generated object (runtime, crt, libraries)]
    [cold half B: generated units]
    [tail catch-all, required to hold no generated code]

so every site in the image is within reach of every hub target, and orders the
cold units from a committed order file (``translated_cpu_unit_order.txt``,
produced by ``order`` from the linked map + ``readelf -rW`` relocations) so
that cold<->cold pairs more than 16 MB apart carry no call edge.  Function
bodies are untouched: the same objects are linked, only their placement moves.

Growth budget.  The hub stays reachable from both ends of .text while
``hub_hi - text_start`` and ``text_end - hub_lo`` are under 16 MiB (``check``
prints the remaining room as ``forward_slack``/``backward_slack``; 3.2 MB on
the gpr-v3 link).  The tighter budget is the cold<->cold one: a unit in half A
calling a unit in half B (or vice versa) is separated from it by the hub, and
the committed order only guarantees that distance stays under 16 MiB minus the
``order`` margin *for the corpus the order was derived from*.  ``check`` with
``--readelf``/``--relocs`` therefore measures every branch edge of the actual
link and reports the worst cold<->cold and hub edge with its headroom to the
bfd limit (``cold_cold_headroom``, ``hub_headroom``).  After any change to the
generated corpus (emitter change, repack, new hot set) link once, regenerate
the order from that link (``ninja isaac_vita_layout_order``, or ``order --map
<map> --relocs <readelf -rW output>``), reconfigure and relink.  Until then
``ISAAC_VITA_LAYOUT_HUB_MAX_VENEERS=0`` fails closed the moment a cold<->cold
edge falls out of reach; a veneer is correct (slower) code, so a small non-zero
limit is an acceptable soft mode on emitter-iteration days.

Subcommands:
  script  configure time: emit the ld script for the unit list and order file
  check   post link: verify the structural contract from the linker map and,
          with --readelf/--relocs, the branch-distance headroom of the link
  order   derive the cold-unit order from the map + relocations of a link
"""
from __future__ import annotations

import argparse
import bisect
import collections
import os
import random
import re
import subprocess
import sys

# bfd/elf32-arm.c: Thumb-2 BL/B.W direct reach measured from the branch site.
THM2_MAX_FWD_BRANCH_OFFSET = (1 << 24) - 2 + 4
THM2_MAX_BWD_BRANCH_OFFSET = -(1 << 24) + 4
# `order` keeps every cold<->cold edge this far inside the reach so the corpus
# may drift before the MAX_VENEERS pin trips (see the module docstring).  3 MiB
# is what the gpr-v3 corpus admits (the hub's own headroom is 3.25 MB, so a
# larger cold<->cold margin buys nothing); when a corpus cannot satisfy the
# requested margin the search halves it down to ORDER_MARGIN_FLOOR.
DEFAULT_ORDER_MARGIN = 3 << 20
ORDER_MARGIN_FLOOR = 1 << 17

UNIT_RE = re.compile(r"^guest_\d{4}$")
UNIT_IN_PATH_RE = re.compile(r"(guest_\d{4})\.c\.o")
SORTED_PREFIX = ".text.sorted."
HUB_PREAMBLE_RE = re.compile(r"^\.text\.(unlikely|exit|startup|hot)(\.|$)")
BRANCH_RELOCS = frozenset({"R_ARM_THM_CALL", "R_ARM_THM_JUMP24"})

MARK_COLD_A_LO = "__isaac_layout_cold_a_lo"
MARK_HUB_LO = "__isaac_layout_hub_lo"
MARK_HUB_HI = "__isaac_layout_hub_hi"
MARK_COLD_B_HI = "__isaac_layout_cold_b_hi"
MARKERS = (MARK_COLD_A_LO, MARK_HUB_LO, MARK_HUB_HI, MARK_COLD_B_HI)

# The exact .text block of the vitasdk GNU ld 2.34 default script.  The
# generator refuses any other shape: a different toolchain script must be
# re-audited (the .vitalink/.sceModuleInfo orphans and the data-segment
# alignment hack live outside this block and are copied verbatim).
DEFAULT_TEXT_BLOCK = """  .text           :
  {
    *(.text.unlikely .text.*_unlikely .text.unlikely.*)
    *(.text.exit .text.exit.*)
    *(.text.startup .text.startup.*)
    *(.text.hot .text.hot.*)
    *(SORT(.text.sorted.*))
    *(.text .stub .text.* .gnu.linkonce.t.*)
    /* .gnu.warning sections are handled specially by elf.em.  */
    *(.gnu.warning)
    *(.glue_7t) *(.glue_7) *(.vfp11_veneer) *(.v4_bx)
  }
"""

# Cold half A precedes the hot region in script order, and GNU ld gives an
# input section to the first matching description, so its patterns must not
# claim ``.text.sorted.*``: ``[!s]*`` takes every helper section that does not
# start with 's' (.text.sub_* excluded on purpose here), ``s[!o]*`` takes
# .text.sub_* and every other 's' helper except ``.text.so*``.  Cold half B
# follows the SORT line, so a plain ``.text.*`` is safe there.
COLD_A_SECTIONS = ".text .text.[!s]* .text.s[!o]*"
COLD_B_SECTIONS = ".text .text.*"
CATCH_ALL_SECTIONS = ".text .stub .text.* .gnu.linkonce.t.*"


class LayoutError(ValueError):
    """A fail-closed layout precondition or contract failed."""


def write_if_changed(path: str, text: str) -> bool:
    """Write ``text`` only when it differs so LINK_DEPENDS does not relink needlessly."""
    try:
        with open(path, encoding="utf-8") as handle:
            if handle.read() == text:
                return False
    except OSError:
        pass
    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write(text)
    return True


# --------------------------------------------------------------------------
# order file
# --------------------------------------------------------------------------

def read_order(path: str) -> tuple[list[str], list[str]]:
    before: list[str] = []
    after: list[str] = []
    seen: set[str] = set()
    target: list[str] | None = None
    with open(path, encoding="utf-8") as handle:
        for number, raw in enumerate(handle, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line == "[before-hub]":
                target = before
                continue
            if line == "[after-hub]":
                target = after
                continue
            if target is None:
                raise LayoutError(f"{path}:{number}: unit outside a group: {line!r}")
            if not UNIT_RE.match(line):
                raise LayoutError(f"{path}:{number}: not a generated unit name: {line!r}")
            if line in seen:
                raise LayoutError(f"{path}:{number}: duplicate unit {line}")
            seen.add(line)
            target.append(line)
    if not before or not after:
        raise LayoutError(f"{path}: both [before-hub] and [after-hub] must list units")
    return before, after


HUB_SOURCE_RE = re.compile(r"^guest_[A-Za-z_][A-Za-z0-9_]*$")


def unit_name_from_source(path: str) -> str | None:
    """Return the cold unit name of a generated source, or None for the generated
    hub sources (guest_stubs.c, guest_table.c: dispatch table and import stubs,
    which the EXCLUDE_FILE catch-all keeps inside the hub)."""
    base = os.path.basename(path)
    if not base.endswith(".c"):
        raise LayoutError(f"generated unit is not a C source: {path}")
    name = base[:-2]
    if UNIT_RE.match(name):
        return name
    if HUB_SOURCE_RE.match(name):
        return None
    raise LayoutError(f"generated unit has an unexpected name: {path}")


def arrange(units: list[str], before: list[str], after: list[str]
            ) -> tuple[list[str], list[str], list[str], list[str]]:
    """Split the build's units into the two cold halves.

    Units the order file does not know are placed adjacent to the hub (end of
    A, start of B, alternating), where every other unit is within reach of
    them; units the order file lists but the build lacks are skipped.
    """
    present = set(units)
    if len(present) != len(units):
        raise LayoutError("duplicate generated unit in the build list")
    half_a = [u for u in before if u in present]
    half_b = [u for u in after if u in present]
    known = set(half_a) | set(half_b)
    unknown = sorted(u for u in units if u not in known)
    stale = sorted(u for u in before + after if u not in present)
    for index, unit in enumerate(unknown):
        if index % 2 == 0:
            half_a.append(unit)
        else:
            half_b.insert(0, unit)
    return half_a, half_b, unknown, stale


# --------------------------------------------------------------------------
# script generation
# --------------------------------------------------------------------------

def default_script_from_ld(ld: str) -> str:
    try:
        result = subprocess.run([ld, "--verbose"], check=False,
                                capture_output=True, text=True)
    except OSError as exc:
        raise LayoutError(f"cannot execute {ld}: {exc}") from exc
    if result.returncode != 0:
        raise LayoutError(f"{ld} --verbose failed: {result.stderr.strip()}")
    return extract_default_script(result.stdout)


def extract_default_script(verbose_output: str) -> str:
    """Return the script between the ===== fences of ``ld --verbose``."""
    lines = verbose_output.splitlines(keepends=True)
    fences = [i for i, line in enumerate(lines) if line.startswith("=====")]
    if len(fences) != 2:
        raise LayoutError("ld --verbose output does not contain exactly two script fences")
    body = "".join(lines[fences[0] + 1:fences[1]])
    if "SECTIONS" not in body or "OUTPUT_FORMAT" not in body:
        raise LayoutError("ld --verbose output has no default linker script")
    return body


def unit_object_pattern(unit: str, object_suffix: str) -> str:
    return f"*{unit}{object_suffix}"


def render_text_block(half_a: list[str], half_b: list[str],
                      object_suffix: str) -> str:
    lines = ["  .text           :", "  {",
             "    /* ISAAC_VITA_LAYOUT_HUB (vita_text_layout.py): cold half A. */",
             f"    {MARK_COLD_A_LO} = .;"]
    for unit in half_a:
        lines.append(f"    {unit_object_pattern(unit, object_suffix)}({COLD_A_SECTIONS})")
    lines += [
        "    /* Hub: preamble, the hot region and every non-generated object, so",
        "       each site of the image reaches each hub target without a veneer. */",
        f"    {MARK_HUB_LO} = .;",
        "    *(.text.unlikely .text.*_unlikely .text.unlikely.*)",
        "    *(.text.exit .text.exit.*)",
        "    *(.text.startup .text.startup.*)",
        "    *(.text.hot .text.hot.*)",
        "    *(SORT(.text.sorted.*))",
        f"    EXCLUDE_FILE (*guest_[0-9][0-9][0-9][0-9]{object_suffix}) *({CATCH_ALL_SECTIONS})",
        f"    {MARK_HUB_HI} = .;",
        "    /* Cold half B. */",
    ]
    for unit in half_b:
        lines.append(f"    {unit_object_pattern(unit, object_suffix)}({COLD_B_SECTIONS})")
    lines += [
        f"    {MARK_COLD_B_HI} = .;",
        "    /* Unclaimed input; the post-link check requires no generated code here. */",
        f"    *({CATCH_ALL_SECTIONS})",
        "    /* .gnu.warning sections are handled specially by elf.em.  */",
        "    *(.gnu.warning)",
        "    *(.glue_7t) *(.glue_7) *(.vfp11_veneer) *(.v4_bx)",
        "  }",
    ]
    return "\n".join(lines) + "\n"


def render_script(default_script: str, half_a: list[str], half_b: list[str],
                  object_suffix: str) -> str:
    if default_script.count(DEFAULT_TEXT_BLOCK) != 1:
        raise LayoutError(
            "default linker script .text block differs from the audited "
            "vitasdk ld 2.34 shape; re-audit before enabling ISAAC_VITA_LAYOUT_HUB")
    if not half_a or not half_b:
        raise LayoutError("both cold halves must contain at least one unit")
    for unit in half_a + half_b:
        if not UNIT_RE.match(unit):
            raise LayoutError(f"not a generated unit name: {unit!r}")
    if not re.fullmatch(r"\.[A-Za-z0-9_.]+", object_suffix):
        raise LayoutError(f"unsafe object suffix: {object_suffix!r}")
    header = ("/* GENERATED by vita_text_layout.py from the toolchain's default "
              "script; do not edit.\n   Only the .text block is rewritten. */\n")
    return header + default_script.replace(
        DEFAULT_TEXT_BLOCK, render_text_block(half_a, half_b, object_suffix))


# --------------------------------------------------------------------------
# linker map
# --------------------------------------------------------------------------

class Section:
    __slots__ = ("name", "address", "size", "origin", "kind", "unit")

    def __init__(self, name: str, address: int, size: int, origin: str):
        self.name = name
        self.address = address
        self.size = size
        self.origin = origin
        self.kind, self.unit = classify(name, origin)


def classify(name: str, origin: str) -> tuple[str, str | None]:
    if origin == "linker stubs":
        return "STUB", None
    if name.startswith(SORTED_PREFIX):
        return "HOT", None
    match = UNIT_IN_PATH_RE.search(origin)
    if match:
        return "GEN", match.group(1)
    if HUB_PREAMBLE_RE.match(name) or name.endswith("_unlikely"):
        return "PRE", None
    if ".a(" in origin:
        return "LIB", None
    return "RT", None


class TextMap:
    def __init__(self, start: int, size: int, sections: list[Section],
                 markers: dict[str, int]):
        self.start = start
        self.size = size
        self.sections = sections
        self.markers = markers

    @property
    def end(self) -> int:
        return self.start + self.size

    def locate(self, address: int) -> Section | None:
        """Return the input section holding ``address`` (sections are sorted)."""
        starts = getattr(self, "_starts", None)
        if starts is None:
            starts = self._starts = [s.address for s in self.sections]
        index = bisect.bisect_right(starts, address) - 1
        if index >= 0:
            section = self.sections[index]
            if section.address <= address < section.address + section.size:
                return section
        return None


_MAP_TEXT_HEADER_RE = re.compile(r"^\.text\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)")
_MAP_NAME_ONLY_RE = re.compile(r"^ (\.text\S*)\s*$")
_MAP_ONE_LINE_RE = re.compile(r"^ (\.text\S*)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+?)\s*$")
_MAP_CONTINUATION_RE = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+?)\s*$")
_MAP_MARKER_RE = re.compile(r"^\s+0x([0-9a-f]+)\s+(__isaac_layout_\w+) = \.\s*$")


def parse_map(path: str) -> TextMap:
    sections: list[Section] = []
    markers: dict[str, int] = {}
    start = size = None
    pending = None
    with open(path, encoding="utf-8", errors="replace") as handle:
        in_text = False
        for line in handle:
            if start is None:
                match = _MAP_TEXT_HEADER_RE.match(line)
                if match and line.startswith(".text "):
                    start = int(match.group(1), 16)
                    size = int(match.group(2), 16)
                    in_text = True
                continue
            if in_text and re.match(r"^\.[A-Za-z]", line):
                break
            match = _MAP_MARKER_RE.match(line)
            if match:
                markers[match.group(2)] = int(match.group(1), 16)
                continue
            match = _MAP_NAME_ONLY_RE.match(line)
            if match:
                pending = match.group(1)
                continue
            match = _MAP_ONE_LINE_RE.match(line)
            if match:
                sections.append(Section(match.group(1), int(match.group(2), 16),
                                        int(match.group(3), 16), match.group(4)))
                pending = None
                continue
            match = _MAP_CONTINUATION_RE.match(line)
            if match and pending is not None:
                sections.append(Section(pending, int(match.group(1), 16),
                                        int(match.group(2), 16), match.group(3)))
                pending = None
    if start is None or size is None:
        raise LayoutError(f"{path}: no .text output section in the linker map")
    sections = [s for s in sections if s.size > 0]
    sections.sort(key=lambda s: s.address)
    return TextMap(start, size, sections, markers)


_VENEER_RE = re.compile(r"^[0-9a-f]+ [tT] (__\S+_veneer)$")


def count_veneers(nm_output: str) -> tuple[int, collections.Counter]:
    """Count GNU ld long-branch stubs (``__<target>_veneer``) in an ``nm`` listing."""
    targets: collections.Counter = collections.Counter()
    for line in nm_output.splitlines():
        match = _VENEER_RE.match(line.strip())
        if match:
            name = match.group(1)[2:-len("_veneer")]
            targets[name] += 1
    return sum(targets.values()), targets


def nm_listing(nm: str, elf: str) -> str:
    try:
        result = subprocess.run([nm, elf], check=False, capture_output=True, text=True)
    except OSError as exc:
        raise LayoutError(f"cannot execute {nm}: {exc}") from exc
    if result.returncode != 0:
        raise LayoutError(f"{nm} {elf} failed: {result.stderr.strip()}")
    return result.stdout


def check(text: TextMap, half_a: list[str], half_b: list[str],
          veneers: int | None = None, max_veneers: int | None = None,
          census: "EdgeCensus | None" = None,
          order_hint: str | None = None) -> dict[str, int]:
    missing = [m for m in MARKERS if m not in text.markers]
    if missing:
        raise LayoutError(f"linker map lacks layout markers: {', '.join(missing)}")
    cold_a_lo = text.markers[MARK_COLD_A_LO]
    hub_lo = text.markers[MARK_HUB_LO]
    hub_hi = text.markers[MARK_HUB_HI]
    cold_b_hi = text.markers[MARK_COLD_B_HI]
    if not (text.start <= cold_a_lo <= hub_lo <= hub_hi <= cold_b_hi <= text.end):
        raise LayoutError("layout markers are not monotonic inside .text")
    side = {u: "A" for u in half_a}
    side.update({u: "B" for u in half_b})
    stats = collections.Counter()
    problems: list[str] = []
    for section in text.sections:
        lo, hi = section.address, section.address + section.size
        stats[section.kind] += section.size
        if section.kind == "STUB":
            continue
        in_a = cold_a_lo <= lo and hi <= hub_lo
        in_hub = hub_lo <= lo and hi <= hub_hi
        in_b = hub_hi <= lo and hi <= cold_b_hi
        if section.kind == "GEN":
            expected = side.get(section.unit)
            if expected is None:
                problems.append(f"unit {section.unit} is not assigned to a half ({section.name})")
            elif expected == "A" and not in_a:
                problems.append(f"{section.unit} {section.name} @0x{lo:x} is outside cold half A")
            elif expected == "B" and not in_b:
                problems.append(f"{section.unit} {section.name} @0x{lo:x} is outside cold half B")
        elif not in_hub:
            problems.append(f"{section.kind} {section.name} ({section.origin}) @0x{lo:x} is outside the hub")
        if len(problems) >= 20:
            break
    if problems:
        raise LayoutError("layout contract violated:\n  " + "\n  ".join(problems))
    forward = hub_hi - text.start
    backward = text.end - hub_lo
    if forward > THM2_MAX_FWD_BRANCH_OFFSET or backward > -THM2_MAX_BWD_BRANCH_OFFSET:
        raise LayoutError(
            f"hub is out of Thumb-2 reach: forward span {forward} B, backward span "
            f"{backward} B, limit {THM2_MAX_FWD_BRANCH_OFFSET} B; the image grew "
            "past what one hub can serve")
    result = {
        "text_bytes": text.size,
        "cold_a_bytes": hub_lo - cold_a_lo,
        "hub_bytes": hub_hi - hub_lo,
        "cold_b_bytes": cold_b_hi - hub_hi,
        "tail_bytes": text.end - cold_b_hi,
        "hot_bytes": stats["HOT"],
        "stub_bytes": stats["STUB"],
        "forward_slack": THM2_MAX_FWD_BRANCH_OFFSET - forward,
        "backward_slack": -THM2_MAX_BWD_BRANCH_OFFSET - backward,
    }
    if census is not None:
        result.update(census.summary())
    if veneers is not None:
        result["veneers"] = veneers
        if max_veneers is not None and veneers > max_veneers:
            regenerate = order_hint or "translated_cpu_unit_order.txt"
            raise LayoutError(
                f"{veneers} long-branch veneers remain (limit {max_veneers}); the hub "
                "is in reach, so these are cold<->cold edges of a corpus the committed "
                f"order was not derived from: regenerate {regenerate} from this link "
                "(ninja isaac_vita_layout_order, or vita_text_layout.py order --map "
                "<map> --relocs <readelf -rW of the ELF>), reconfigure and relink; "
                "or raise ISAAC_VITA_LAYOUT_HUB_MAX_VENEERS for a soft (veneered) build")
    return result


# --------------------------------------------------------------------------
# reach model and order derivation
# --------------------------------------------------------------------------

def parse_branch_relocs(lines, label: str) -> list[tuple[int, int, str]]:
    """(site, target, symbol) for every Thumb BL/B.W relocation of ``readelf -rW``."""
    edges: list[tuple[int, int, str]] = []
    for line in lines:
        parts = line.split()
        if len(parts) >= 5 and parts[2] in BRANCH_RELOCS:
            try:
                site = int(parts[0], 16)
                target = int(parts[3], 16) & ~1
            except ValueError:
                continue
            edges.append((site, target, parts[4]))
    if not edges:
        raise LayoutError(f"{label}: no Thumb branch relocations (link with -Wl,-q)")
    return edges


def read_branch_relocs(path: str) -> list[tuple[int, int, str]]:
    with open(path, encoding="utf-8", errors="replace") as handle:
        return parse_branch_relocs(handle, path)


def readelf_branch_relocs(readelf: str, elf: str) -> list[tuple[int, int, str]]:
    try:
        result = subprocess.run([readelf, "-rW", elf], check=False, capture_output=True,
                                text=True, errors="replace")
    except OSError as exc:
        raise LayoutError(f"cannot execute {readelf}: {exc}") from exc
    if result.returncode != 0:
        raise LayoutError(f"{readelf} -rW {elf} failed: {result.stderr.strip()}")
    return parse_branch_relocs(result.stdout.splitlines(), f"{readelf} -rW {elf}")


def edge_class(site: Section, target: Section | None) -> str:
    """cold_cold: generated -> generated (only the committed order keeps these in
    reach); hub: at least one end in the hub (kept in reach by the [A][hub][B]
    shape and the forward/backward span check); outside: target outside .text."""
    if target is None:
        return "outside"
    if site.kind == "GEN" and target.kind == "GEN":
        return "cold_cold"
    return "hub"


def describe_edge(site: Section, site_offset: int, target: Section | None,
                  symbol: str) -> str:
    where = "outside .text" if target is None else (target.unit or target.kind)
    return f"{site.unit or site.kind} {site.name}+0x{site_offset:x} -> {symbol} ({where})"


class EdgeCensus:
    """Worst-case Thumb-2 branch distance per edge class for one placement.

    ``headroom`` of an edge is its distance to the nearer bfd limit (negative:
    the edge already needs a veneer); the class headroom is the minimum over the
    class, i.e. how many bytes the layout may still drift before the first edge
    of that class is veneered.
    """

    def __init__(self):
        self.count: collections.Counter = collections.Counter()
        self.beyond: collections.Counter = collections.Counter()
        self.worst: dict[str, tuple[int, int, str]] = {}

    @staticmethod
    def headroom_of(offset: int) -> int:
        return min(THM2_MAX_FWD_BRANCH_OFFSET - offset,
                   offset - THM2_MAX_BWD_BRANCH_OFFSET)

    def add(self, cls: str, offset: int, description: str) -> None:
        self.count[cls] += 1
        headroom = self.headroom_of(offset)
        if headroom < 0:
            self.beyond[cls] += 1
        worst = self.worst.get(cls)
        if worst is None or headroom < worst[0]:
            self.worst[cls] = (headroom, offset, description)

    @property
    def beyond_reach(self) -> int:
        return sum(self.beyond.values())

    def headroom(self, cls: str) -> int | None:
        worst = self.worst.get(cls)
        return None if worst is None else worst[0]

    def summary(self) -> dict[str, int]:
        out = {"edges": sum(self.count.values()), "beyond_reach": self.beyond_reach}
        for cls in ("cold_cold", "hub"):
            out[f"{cls}_edges"] = self.count[cls]
            out[f"{cls}_beyond"] = self.beyond[cls]
            if cls in self.worst:
                out[f"{cls}_headroom"] = self.worst[cls][0]
        if self.count["outside"]:
            out["outside_edges"] = self.count["outside"]
            out["outside_beyond"] = self.beyond["outside"]
        return out

    def worst_lines(self) -> list[str]:
        lines = []
        for cls in ("cold_cold", "hub"):
            if cls in self.worst:
                headroom, offset, description = self.worst[cls]
                lines.append(f"worst {cls.replace('_', '<->')} edge: {description} "
                             f"offset {offset:+d} B headroom {headroom} B")
        return lines


def census_of_link(text: TextMap, relocs: list[tuple[int, int, str]]) -> EdgeCensus:
    """Measure every branch edge at the addresses of the linked map."""
    census = EdgeCensus()
    for site, target, symbol in relocs:
        if target == 0:
            continue            # weak undefined: no branch is emitted
        section = text.locate(site)
        if section is None:
            continue            # site outside .text (.init/.fini glue)
        target_section = text.locate(target)
        census.add(edge_class(section, target_section), target - site,
                   describe_edge(section, site - section.address, target_section, symbol))
    return census


class ReachModel:
    """Section-level branch reach for an arbitrary placement of the map's sections."""

    def __init__(self, text: TextMap, relocs: list[tuple[int, int, str]]):
        self.sections = [s for s in text.sections if s.kind != "STUB"]
        starts = [s.address for s in self.sections]

        def locate(address: int) -> int | None:
            index = bisect.bisect_right(starts, address) - 1
            if index >= 0:
                section = self.sections[index]
                if section.address <= address < section.address + section.size:
                    return index
            return None

        self.edges: list[tuple[int, int, int | None, int, str]] = []
        for site, target, symbol in relocs:
            si = locate(site)
            if si is None:
                continue
            ti = locate(target)
            if target == 0:
                continue            # weak undefined: no branch is emitted
            toff = (target - text.end) if ti is None else target - self.sections[ti].address
            self.edges.append((si, site - self.sections[si].address, ti, toff, symbol))

    def place(self, order: list[int], base: int) -> tuple[dict[int, int], int]:
        address: dict[int, int] = {}
        cursor = base
        for index in order:
            cursor = (cursor + 3) & ~3
            address[index] = cursor
            cursor += self.sections[index].size
        return address, cursor

    def offsets(self, order: list[int], base: int):
        """Yield (edge, branch offset) for the placement ``order``."""
        address, end = self.place(order, base)
        for edge in self.edges:
            si, soff, ti, toff, _symbol = edge
            site = address[si] + soff
            target = (end + toff) if ti is None else address[ti] + toff
            yield edge, target - site

    def stubbed(self, order: list[int], base: int) -> list[tuple[int, int | None, str]]:
        return [(si, ti, symbol) for (si, _soff, ti, _toff, symbol), offset
                in self.offsets(order, base)
                if offset > THM2_MAX_FWD_BRANCH_OFFSET or offset < THM2_MAX_BWD_BRANCH_OFFSET]

    def census(self, order: list[int], base: int) -> EdgeCensus:
        census = EdgeCensus()
        for (si, soff, ti, _toff, symbol), offset in self.offsets(order, base):
            site = self.sections[si]
            target = None if ti is None else self.sections[ti]
            census.add(edge_class(site, target), offset,
                       describe_edge(site, soff, target, symbol))
        return census


def derive_order(text: TextMap, relocs: list[tuple[int, int, str]],
                 margin: int, iterations: int, seed: int, log=print
                 ) -> tuple[list[str], list[str], dict[str, int]]:
    """Arrange the generated units around the hub.

    A seeded local search minimises the number of unit pairs with call edges
    whose distance exceeds reach - margin; when it cannot reach zero the margin
    is halved (down to ORDER_MARGIN_FLOOR) and the search repeated.  The natural
    order and every attempt are then measured exactly and the sequence with the
    fewest edges beyond reach, then the largest cold<->cold headroom, wins, so
    the result is never worse than the natural order.  ``summary['margin']`` is
    the margin the winning attempt was searched with.
    """
    model = ReachModel(text, relocs)
    sections = model.sections
    by_unit: dict[str, list[int]] = collections.OrderedDict()
    hub: list[int] = []
    for index, section in enumerate(sections):
        if section.kind == "GEN":
            by_unit.setdefault(section.unit, []).append(index)
        else:
            hub.append(index)
    units = list(by_unit)
    if len(units) < 2:
        raise LayoutError("fewer than two generated units in the map")
    size = {u: sum(sections[i].size for i in by_unit[u]) for u in units}
    hub_bytes = sum(sections[i].size for i in hub)
    weight: collections.Counter = collections.Counter()
    for si, _soff, ti, _toff, _symbol in model.edges:
        if ti is None:
            continue
        a, b = sections[si], sections[ti]
        if a.kind == "GEN" and b.kind == "GEN" and a.unit != b.unit:
            weight[(a.unit, b.unit)] += 1
    pairs = list(weight.items())

    def split(sequence: list[str]) -> tuple[list[str], list[str]]:
        total = sum(size[u] for u in sequence)
        acc = 0
        for k, unit in enumerate(sequence):
            acc += size[unit]
            if acc >= total / 2:
                return sequence[:k + 1], sequence[k + 1:]
        return sequence, []

    def approx(sequence: list[str], reach: int) -> int:
        half_a, half_b = split(sequence)
        position: dict[str, tuple[int, int]] = {}
        cursor = 0
        for unit in half_a:
            position[unit] = (cursor, cursor + size[unit])
            cursor += size[unit]
        cursor += hub_bytes
        for unit in half_b:
            position[unit] = (cursor, cursor + size[unit])
            cursor += size[unit]
        bad = 0
        for (a, b), w in pairs:
            lo_a, hi_a = position[a]
            lo_b, hi_b = position[b]
            if max(abs(hi_b - lo_a), abs(hi_a - lo_b)) > reach:
                bad += w
        return bad

    def exact(sequence: list[str]) -> EdgeCensus:
        half_a, half_b = split(sequence)
        order = [i for u in half_a for i in by_unit[u]] + hub + \
                [i for u in half_b for i in by_unit[u]]
        return model.census(order, text.start)

    def search(start: list[str], reach: int) -> tuple[list[str], int]:
        rng = random.Random(seed)
        current = start[:]
        current_value = approx(current, reach)
        best, best_value = current[:], current_value
        for _ in range(iterations):
            if best_value == 0:
                break
            i = rng.randrange(len(current))
            j = rng.randrange(len(current))
            if i == j:
                continue
            candidate = current[:]
            candidate.insert(j, candidate.pop(i))
            value = approx(candidate, reach)
            if value <= current_value:
                current, current_value = candidate, value
                if value < best_value:
                    best, best_value = candidate[:], value
        return best, best_value

    natural = list(units)
    natural_census = exact(natural)
    natural_exact = natural_census.beyond_reach
    log(f"order: {len(units)} units, {len(pairs)} unit pairs with call edges, "
        f"hub {hub_bytes} B, margin {margin} B")
    log(f"order: natural sequence -> {natural_exact} edges beyond reach (exact)")
    # (margin, sequence, edges beyond reach-margin, census); the natural order is
    # the first candidate so the result can never be worse than it.
    candidates = [(margin, natural, approx(natural, THM2_MAX_FWD_BRANCH_OFFSET - margin),
                   natural_census)]
    current_margin = margin
    while True:
        reach = THM2_MAX_FWD_BRANCH_OFFSET - current_margin
        best, best_value = search(natural, reach)
        census = exact(best)
        log(f"order: margin {current_margin} B -> {best_value} edges beyond reach-margin, "
            f"{census.beyond_reach} beyond reach (exact)")
        candidates.append((current_margin, best, best_value, census))
        if best_value == 0 or current_margin <= ORDER_MARGIN_FLOOR:
            break
        current_margin = max(current_margin // 2, ORDER_MARGIN_FLOOR)
        log(f"order: retrying with margin {current_margin} B")

    def rank(candidate):
        _margin, _sequence, beyond_margin, census = candidate
        headroom = census.headroom("cold_cold")
        return (census.beyond_reach, -(headroom if headroom is not None else 0), beyond_margin)

    chosen_margin, best, best_value, census = min(candidates, key=rank)
    best_exact = census.beyond_reach
    log(f"order: chosen sequence (margin {chosen_margin} B) -> {best_value} edges beyond "
        f"reach-margin, {best_exact} beyond reach (exact)")
    for line in census.worst_lines():
        log(f"order: {line}")
    half_a, half_b = split(best)
    summary = {"units": len(units), "natural_exact": natural_exact,
               "optimised_exact": best_exact, "optimised_margin": best_value,
               "margin": chosen_margin}
    for cls in ("cold_cold", "hub"):
        headroom = census.headroom(cls)
        if headroom is not None:
            summary[f"{cls}_headroom"] = headroom
    return half_a, half_b, summary


def render_order(half_a: list[str], half_b: list[str], summary: dict[str, int],
                 margin: int) -> str:
    lines = [
        "# Cold-unit order for ISAAC_VITA_LAYOUT_HUB (vita_text_layout.py order).",
        "# Derived from the linked map + readelf -rW branch relocations: units are",
        "# arranged around the hub so no two units more than 16 MiB - margin apart",
        f"# share a call edge (margin {margin} B; natural order left "
        f"{summary['natural_exact']} edges beyond reach, this order "
        f"{summary['optimised_exact']}).",
        "# Growth budget on the deriving link: worst cold<->cold edge headroom "
        f"{summary.get('cold_cold_headroom', 'n/a')} B, worst hub edge headroom "
        f"{summary.get('hub_headroom', 'n/a')} B; the generated code may drift by",
        "# less than that before a cold<->cold edge needs a veneer and the",
        "# ISAAC_VITA_LAYOUT_HUB_MAX_VENEERS=0 pin fails closed.  After any corpus",
        "# change: link once, `ninja isaac_vita_layout_order` (or vita_text_layout.py",
        "# order --map <map> --relocs <readelf -rW>), reconfigure, relink.",
        "# Units missing here are placed next to the hub; units missing from the",
        "# build are skipped.",
        "[before-hub]",
    ]
    lines += half_a
    lines.append("[after-hub]")
    lines += half_b
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def _units_from_args(args) -> tuple[list[str], int]:
    sources: list[str] = []
    if args.units_file:
        with open(args.units_file, encoding="utf-8") as handle:
            sources += [line.strip() for line in handle if line.strip()]
    sources += args.unit or []
    units: list[str] = []
    hub_sources = 0
    for source in sources:
        name = unit_name_from_source(source)
        if name is None:
            hub_sources += 1
        else:
            units.append(name)
    if not units:
        raise LayoutError("no generated units given (--units-file or --unit)")
    return units, hub_sources


def cmd_script(args) -> int:
    units, hub_sources = _units_from_args(args)
    before, after = read_order(args.order)
    half_a, half_b, unknown, stale = arrange(units, before, after)
    if args.ld_verbose_file:
        with open(args.ld_verbose_file, encoding="utf-8") as handle:
            default_script = extract_default_script(handle.read())
    else:
        default_script = default_script_from_ld(args.ld)
    text = render_script(default_script, half_a, half_b, args.object_suffix)
    write_if_changed(args.output, text)
    if args.write_halves:
        write_if_changed(args.write_halves,
                         "[before-hub]\n" + "\n".join(half_a) + "\n[after-hub]\n"
                         + "\n".join(half_b) + "\n")
    print(f"units={len(units)} before_hub={len(half_a)} after_hub={len(half_b)} "
          f"unknown={len(unknown)} stale={len(stale)} hub_sources={hub_sources}")
    return 0


def cmd_check(args) -> int:
    before, after = read_order(args.order)
    text = parse_map(args.map)
    veneers = None
    targets: collections.Counter = collections.Counter()
    if args.nm_output:
        with open(args.nm_output, encoding="utf-8", errors="replace") as handle:
            veneers, targets = count_veneers(handle.read())
    elif args.nm or args.elf:
        if not (args.nm and args.elf):
            raise LayoutError("--nm and --elf must be given together")
        veneers, targets = count_veneers(nm_listing(args.nm, args.elf))
    census = None
    if args.relocs:
        census = census_of_link(text, read_branch_relocs(args.relocs))
    elif args.readelf:
        if not args.elf:
            raise LayoutError("--readelf needs --elf")
        census = census_of_link(text, readelf_branch_relocs(args.readelf, args.elf))
    result = check(text, before, after, veneers, args.max_veneers, census, args.order_file)
    print("layout-hub check: " + " ".join(f"{k}={v}" for k, v in result.items()))
    if census is not None:
        for line in census.worst_lines():
            print("layout-hub " + line)
    if targets:
        print("layout-hub veneer targets: " + " ".join(
            f"{name}x{count}" for name, count in targets.most_common(20)))
    return 0


def cmd_order(args) -> int:
    text = parse_map(args.map)
    if args.relocs:
        relocs = read_branch_relocs(args.relocs)
    elif args.readelf and args.elf:
        relocs = readelf_branch_relocs(args.readelf, args.elf)
    else:
        raise LayoutError("order needs --relocs, or --readelf together with --elf")
    half_a, half_b, summary = derive_order(text, relocs, args.margin,
                                           args.iterations, args.seed,
                                           log=lambda s: print(s, file=sys.stderr))
    with open(args.output, "w", encoding="utf-8", newline="\n") as out:
        out.write(render_order(half_a, half_b, summary, summary["margin"]))
    print(" ".join(f"{k}={v}" for k, v in summary.items()))
    if summary["optimised_exact"] > 0:
        print(f"vita_text_layout: {summary['optimised_exact']} cold<->cold edges stay "
              "beyond reach in the best order found (written anyway); the corpus "
              "cannot be made veneer-free by placement alone at this margin",
              file=sys.stderr)
        return 2
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("script", help="emit the ld script (configure time)")
    p.add_argument("--order", required=True, help="translated_cpu_unit_order.txt")
    p.add_argument("--units-file", help="one generated unit source path per line")
    p.add_argument("--unit", action="append", help="generated unit source path")
    p.add_argument("--ld", help="arm-vita-eabi-ld (runs --verbose)")
    p.add_argument("--ld-verbose-file", help="saved 'ld --verbose' output instead of --ld")
    p.add_argument("--object-suffix", default=".c.obj",
                   help="object file suffix appended to the unit name")
    p.add_argument("--output", required=True)
    p.add_argument("--write-halves", help="also write the resolved halves (for check)")
    p.set_defaults(func=cmd_script)

    p = sub.add_parser("check", help="verify the linked map against the contract")
    p.add_argument("--map", required=True, help="GNU ld -Map output")
    p.add_argument("--order", required=True,
                   help="resolved halves file written by 'script --write-halves'")
    p.add_argument("--nm", help="arm-vita-eabi-nm, used with --elf to count veneers")
    p.add_argument("--elf", help="linked ELF for --nm")
    p.add_argument("--nm-output", help="saved nm listing instead of --nm/--elf")
    p.add_argument("--max-veneers", type=int, default=None,
                   help="fail when more long-branch veneers than this remain")
    p.add_argument("--relocs", help="readelf -rW output of the -q link: measure every "
                   "branch edge and report the cold<->cold / hub headroom")
    p.add_argument("--readelf", help="arm-vita-eabi-readelf, run on --elf instead of --relocs")
    p.add_argument("--order-file", help="committed order file named in the recovery hint")
    p.set_defaults(func=cmd_check)

    p = sub.add_parser("order", help="derive the cold-unit order from map + relocs")
    p.add_argument("--map", required=True)
    p.add_argument("--relocs", help="readelf -rW output of the -q link")
    p.add_argument("--readelf", help="arm-vita-eabi-readelf, run on --elf instead of --relocs")
    p.add_argument("--elf", help="linked -q ELF for --readelf")
    p.add_argument("--margin", type=int, default=DEFAULT_ORDER_MARGIN,
                   help=f"initial reach safety margin in bytes (default {DEFAULT_ORDER_MARGIN}; "
                   f"halved down to {ORDER_MARGIN_FLOOR} while no order satisfies it)")
    p.add_argument("--iterations", type=int, default=300000)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--output", required=True)
    p.set_defaults(func=cmd_order)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except (OSError, LayoutError) as exc:
        print(f"vita_text_layout: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
