"""Strict MSVC x86 RTTI/vftable discovery for the exact Repentance PE.

An arbitrary dword whose value happens to land in ``.text`` is not evidence of
a function start: jump tables and even UTF-16 text produce those by accident.
This module accepts a vftable only through the complete MSVC RTTI chain and
requires an IMAGE_REL_BASED_HIGHLOW relocation on every absolute pointer,
including every vftable slot.

The input PE is content-pinned by the caller's build contract.  The census and
set hashes below make an incomplete or over-broad parser change fail loudly.
"""
from __future__ import annotations

import collections
import dataclasses
import hashlib
import struct

import pefile


class RTTIError(RuntimeError):
    pass


@dataclasses.dataclass(frozen=True)
class RTTICensus:
    type_descriptors: int
    complete_object_locators: int
    class_hierarchies: int
    base_class_descriptors: int
    base_class_references: int
    table_headers: tuple[int, ...]
    slot_count: int
    roots: frozenset[int]
    nonrelocated_executable_tails: tuple[tuple[int, tuple[int, ...]], ...]


@dataclasses.dataclass(frozen=True)
class _Section:
    name: str
    rva: int
    vsize: int
    data: bytes
    executable: bool

    @property
    def raw_end(self):
        return self.rva + min(self.vsize, len(self.data))

    @property
    def virtual_end(self):
        return self.rva + self.vsize

    def contains_raw(self, rva, size=1):
        return self.rva <= rva and rva + size <= self.raw_end


@dataclasses.dataclass(frozen=True)
class _TypeDescriptor:
    rva: int
    type_info_vftable: int
    name: str


@dataclasses.dataclass(frozen=True)
class _Hierarchy:
    rva: int
    attributes: int
    base_count: int
    base_array: int
    bases: tuple[int, ...]


@dataclasses.dataclass(frozen=True)
class _Locator:
    rva: int
    offset: int
    cd_offset: int
    type_descriptor: int
    hierarchy: int


EXPECTED = {
    "type_descriptors": 225,
    "complete_object_locators": 216,
    "used_type_descriptors": 214,
    "class_hierarchies": 214,
    "base_class_descriptors": 229,
    "base_class_references": 439,
    "table_headers": 216,
    "slots": 2388,
    "roots": 929,
    "type_info_vftable": 0x006080C4,
    "headers_sha256":
        "f5f5f252ad325ff4233537d7f3bf05750b7022157551f7a9a445ad763ce6130a",
    "roots_sha256":
        "bc57027bb163ad9adc22046a661b4c179a05727dc63c2367d2d22953b185a5ea",
}

# These are UTF-16 string dwords immediately after three real vftables.  Their
# numeric values happen to land in .text.  A value-only run has 2,393 slots;
# the relocation-backed run has the correct 2,388.
EXPECTED_FALSE_TAILS = (
    (0x006080C0, (0x00300061,)),
    (0x00748690, (0x00396C66,)),
    (0x007682A8, (0x00330064, 0x00050074, 0x0024006E)),
)


def hash_rvas(rvas):
    raw = "".join("%08x\n" % rva for rva in sorted(rvas)).encode("ascii")
    return hashlib.sha256(raw).hexdigest()


def _expect(name, actual, expected):
    if actual != expected:
        raise RTTIError("MSVC RTTI %s changed: %r != %r"
                        % (name, actual, expected))


def _signed(value):
    return struct.unpack("<i", struct.pack("<I", value))[0]


def discover(pe, sections):
    """Return every relocation-proved vftable function root.

    ``sections`` is the list produced by ``funcs.load()``.  RVAs, rather than
    rebased runtime addresses, are returned to the function-discovery pass.
    """
    base = pe.OPTIONAL_HEADER.ImageBase
    image_size = pe.OPTIONAL_HEADER.SizeOfImage
    sec_list = tuple(_Section(
        s["name"], s["rva"], s["vsize"], s["data"], s["exec"])
        for s in sections)
    by_name = {s.name: s for s in sec_list}
    try:
        rdata = by_name[".rdata"]
        data = by_name[".data"]
    except KeyError as ex:
        raise RTTIError("MSVC RTTI requires .rdata and .data") from ex
    executable = tuple(s for s in sec_list if s.executable)
    if not executable:
        raise RTTIError("MSVC RTTI found no executable section")

    directory = pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]
    pe.parse_data_directories(directories=[directory])
    if not hasattr(pe, "DIRECTORY_ENTRY_BASERELOC"):
        raise RTTIError("PE has no base-relocation directory")
    relocation_types = collections.Counter()
    relocations = set()
    for block in pe.DIRECTORY_ENTRY_BASERELOC:
        for entry in block.entries:
            relocation_types[entry.type] += 1
            if entry.type == pefile.RELOCATION_TYPE["IMAGE_REL_BASED_HIGHLOW"]:
                relocations.add(entry.rva)
    unexpected_types = set(relocation_types) - {
        pefile.RELOCATION_TYPE["IMAGE_REL_BASED_ABSOLUTE"],
        pefile.RELOCATION_TYPE["IMAGE_REL_BASED_HIGHLOW"],
    }
    if unexpected_types:
        raise RTTIError("unexpected x86 relocation types: %s"
                        % sorted(unexpected_types))
    if not relocations:
        raise RTTIError("PE has no HIGHLOW relocations")

    def u32(section, rva):
        if not section.contains_raw(rva, 4):
            raise RTTIError("read outside %s at %08x" % (section.name, rva))
        return struct.unpack_from("<I", section.data, rva - section.rva)[0]

    def words(section, rva, count):
        if not section.contains_raw(rva, 4 * count):
            raise RTTIError("read outside %s at %08x" % (section.name, rva))
        return struct.unpack_from("<%dI" % count, section.data,
                                  rva - section.rva)

    def pointer_rva(value):
        if not (base <= value < base + image_size):
            raise RTTIError("absolute pointer leaves image: %08x" % value)
        return value - base

    def maybe_pointer_rva(value):
        if base <= value < base + image_size:
            return value - base
        return None

    def executable_pointer(value):
        rva = maybe_pointer_rva(value)
        if rva is None:
            return None
        for section in executable:
            if section.rva <= rva < section.virtual_end:
                return rva
        return None

    # A TypeDescriptor is anchored by a relocated type_info vfptr, a zero
    # spare field, and a printable MSVC class/struct decorated name.
    type_candidates = {}
    max_name = 4096
    for rva in range(data.rva, data.raw_end - 12 + 1, 4):
        if rva not in relocations or u32(data, rva + 4) != 0:
            continue
        offset = rva - data.rva + 8
        if data.data[offset:offset + 4] not in (b".?AV", b".?AU"):
            continue
        end = data.data.find(b"\x00", offset,
                             min(len(data.data), offset + max_name))
        if end < 0:
            raise RTTIError("unterminated TypeDescriptor at %08x" % rva)
        encoded = data.data[offset:end]
        if not encoded or any(ch < 0x20 or ch >= 0x7F for ch in encoded):
            raise RTTIError("invalid TypeDescriptor name at %08x" % rva)
        vfptr = pointer_rva(u32(data, rva))
        if not rdata.contains_raw(vfptr, 4):
            raise RTTIError("TypeDescriptor vfptr is not in .rdata: %08x"
                            % rva)
        type_candidates[rva] = _TypeDescriptor(
            rva, vfptr, encoded.decode("ascii"))

    type_info = [td for td in type_candidates.values()
                 if td.name == ".?AVtype_info@@"]
    if len(type_info) != 1:
        raise RTTIError("expected one type_info TypeDescriptor, found %d"
                        % len(type_info))
    common_vftable = type_info[0].type_info_vftable
    foreign = [td for td in type_candidates.values()
               if td.type_info_vftable != common_vftable]
    if foreign:
        raise RTTIError("%d TypeDescriptors use a foreign type_info vfptr"
                        % len(foreign))
    type_descriptors = type_candidates

    hierarchy_cache = {}

    def shallow_hierarchy(rva):
        if not rdata.contains_raw(rva, 16):
            raise RTTIError("ClassHierarchyDescriptor leaves .rdata: %08x"
                            % rva)
        signature, attributes, base_count, array_va = words(rdata, rva, 4)
        if signature != 0 or attributes & ~7:
            raise RTTIError("invalid ClassHierarchyDescriptor at %08x" % rva)
        if not 1 <= base_count <= 512:
            raise RTTIError("invalid base count at %08x: %d"
                            % (rva, base_count))
        if rva + 12 not in relocations:
            raise RTTIError("unrelocated base array at %08x" % rva)
        array_rva = pointer_rva(array_va)
        if not rdata.contains_raw(array_rva, 4 * base_count):
            raise RTTIError("base array leaves .rdata at %08x" % rva)
        return attributes, base_count, array_rva

    def validate_hierarchy(rva, expected_type):
        key = (rva, expected_type)
        if key in hierarchy_cache:
            return hierarchy_cache[key]
        attributes, base_count, array_rva = shallow_hierarchy(rva)
        bases = []
        for index in range(base_count):
            slot = array_rva + 4 * index
            if slot not in relocations:
                raise RTTIError("unrelocated base-array slot %08x" % slot)
            base_rva = pointer_rva(u32(rdata, slot))
            if not rdata.contains_raw(base_rva, 28):
                raise RTTIError("BaseClassDescriptor leaves .rdata: %08x"
                                % base_rva)
            values = words(rdata, base_rva, 7)
            if base_rva not in relocations:
                raise RTTIError("unrelocated base type at %08x" % base_rva)
            base_type = pointer_rva(values[0])
            if base_type not in type_descriptors:
                raise RTTIError("unknown base TypeDescriptor at %08x"
                                % base_rva)
            if values[1] >= base_count:
                raise RTTIError("invalid contained-base count at %08x"
                                % base_rva)
            mdisp, pdisp, vdisp = map(_signed, values[2:5])
            if not (0 <= mdisp <= 0x100000 and mdisp % 4 == 0):
                raise RTTIError("invalid PMD.mdisp at %08x" % base_rva)
            if not (pdisp == -1 or
                    0 <= pdisp <= 0x100000 and pdisp % 4 == 0):
                raise RTTIError("invalid PMD.pdisp at %08x" % base_rva)
            if not (0 <= vdisp <= 0x100000 and vdisp % 4 == 0):
                raise RTTIError("invalid PMD.vdisp at %08x" % base_rva)
            if values[5] & ~0x1FF:
                raise RTTIError("unknown base attributes at %08x" % base_rva)
            if not values[6] or base_rva + 24 not in relocations:
                raise RTTIError("missing relocated base hierarchy at %08x"
                                % base_rva)
            base_hierarchy = pointer_rva(values[6])
            shallow_hierarchy(base_hierarchy)
            bases.append(base_rva)
        if not bases or pointer_rva(u32(rdata, bases[0])) != expected_type:
            raise RTTIError("COL type is not first hierarchy base at %08x"
                            % rva)
        result = _Hierarchy(rva, attributes, base_count, array_rva,
                            tuple(bases))
        hierarchy_cache[key] = result
        return result

    # COL layout for 32-bit absolute RTTI:
    # signature, offset, cdOffset, TypeDescriptor*, ClassHierarchyDescriptor*.
    locators = {}
    for rva in range(rdata.rva, rdata.raw_end - 20 + 1, 4):
        signature, offset, cd_offset, type_va, hierarchy_va = \
            words(rdata, rva, 5)
        if signature != 0:
            continue
        type_rva = maybe_pointer_rva(type_va)
        if type_rva not in type_descriptors:
            continue
        if rva + 12 not in relocations or rva + 16 not in relocations:
            # An overlapping 20-byte window in a BCD can have signature zero
            # and its fourth word can be another valid TypeDescriptor.  It is
            # not a COL candidate until both pointer fields are relocations.
            continue
        if (offset > 0x100000 or offset % 4 or
                cd_offset > 0x100000 or cd_offset % 4):
            raise RTTIError("invalid COL offsets at %08x" % rva)
        hierarchy_rva = pointer_rva(hierarchy_va)
        validate_hierarchy(hierarchy_rva, type_rva)
        locators[rva] = _Locator(
            rva, offset, cd_offset, type_rva, hierarchy_rva)

    # A relocated pointer to a proved COL is the word at vftable[-1].  Every
    # following slot must itself be relocated and point into executable memory.
    references = collections.defaultdict(list)
    for rva in range(rdata.rva, rdata.raw_end - 4 + 1, 4):
        if rva not in relocations:
            continue
        target = maybe_pointer_rva(u32(rdata, rva))
        if target in locators:
            references[target].append(rva)
    bad_references = {rva: slots for rva, slots in references.items()
                      if len(slots) != 1}
    missing_references = set(locators) - set(references)
    if bad_references or missing_references:
        raise RTTIError("COL reference cardinality changed: bad=%r missing=%r"
                        % (bad_references, sorted(missing_references)))

    tables = []
    roots = set()
    false_tails = []
    for locator_rva in sorted(locators):
        header = references[locator_rva][0]
        slot = header + 4
        entries = []
        while rdata.contains_raw(slot, 4) and slot in relocations:
            target = executable_pointer(u32(rdata, slot))
            if target is None:
                break
            entries.append(target)
            slot += 4
        if not entries:
            raise RTTIError("vftable at %08x has no executable slots" % header)
        if len(entries) > 512:
            raise RTTIError("implausibly long vftable at %08x" % header)
        tables.append((header, tuple(entries), slot))
        roots.update(entries)

        # Measure exactly the value-only mistake this relocation rule avoids.
        tail = []
        probe = slot
        while rdata.contains_raw(probe, 4):
            target = executable_pointer(u32(rdata, probe))
            if target is None:
                break
            tail.append(target)
            probe += 4
        if tail:
            false_tails.append((header, tuple(tail)))

    intervals = sorted((header + 4, end, header)
                       for header, _entries, end in tables)
    for previous, current in zip(intervals, intervals[1:]):
        if current[0] < previous[1]:
            raise RTTIError("overlapping vftables at %08x and %08x"
                            % (previous[2], current[2]))
    headers = {header for header, _entries, _end in tables}
    for begin, end, owner in intervals:
        intruders = [header for header in headers
                     if header != owner and begin <= header < end]
        if intruders:
            raise RTTIError("RTTI header inside vftable %08x: %r"
                            % (owner, intruders))

    used_types = {locator.type_descriptor for locator in locators.values()}
    used_hierarchies = {locator.hierarchy for locator in locators.values()}
    base_descriptors = {base
                        for hierarchy in hierarchy_cache.values()
                        for base in hierarchy.bases}
    base_references = sum(
        validate_hierarchy(locator.hierarchy,
                           locator.type_descriptor).base_count
        for locator in locators.values())
    table_headers = tuple(sorted(headers))
    slot_count = sum(len(entries) for _header, entries, _end in tables)
    false_tails_tuple = tuple(sorted(false_tails))

    _expect("TypeDescriptor count", len(type_descriptors),
            EXPECTED["type_descriptors"])
    _expect("type_info vftable", common_vftable,
            EXPECTED["type_info_vftable"])
    _expect("COL count", len(locators),
            EXPECTED["complete_object_locators"])
    _expect("used TypeDescriptor count", len(used_types),
            EXPECTED["used_type_descriptors"])
    _expect("ClassHierarchyDescriptor count", len(used_hierarchies),
            EXPECTED["class_hierarchies"])
    _expect("BaseClassDescriptor count", len(base_descriptors),
            EXPECTED["base_class_descriptors"])
    _expect("base-class reference count", base_references,
            EXPECTED["base_class_references"])
    _expect("vftable header count", len(table_headers),
            EXPECTED["table_headers"])
    _expect("vftable slot count", slot_count, EXPECTED["slots"])
    _expect("unique vftable root count", len(roots), EXPECTED["roots"])
    _expect("vftable header hash", hash_rvas(table_headers),
            EXPECTED["headers_sha256"])
    _expect("vftable root hash", hash_rvas(roots),
            EXPECTED["roots_sha256"])
    _expect("nonrelocated executable tails", false_tails_tuple,
            EXPECTED_FALSE_TAILS)

    return RTTICensus(
        type_descriptors=len(type_descriptors),
        complete_object_locators=len(locators),
        class_hierarchies=len(used_hierarchies),
        base_class_descriptors=len(base_descriptors),
        base_class_references=base_references,
        table_headers=table_headers,
        slot_count=slot_count,
        roots=frozenset(roots),
        nonrelocated_executable_tails=false_tails_tuple,
    )
