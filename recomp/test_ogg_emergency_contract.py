#!/usr/bin/env python3
"""Frozen-PE contract for the exact OGG Open emergency allocation."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import struct


EXPECTED_PE_BYTES = 8_650_240
EXPECTED_PE_SHA256 = (
    "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
)
OPEN_PUSH_RVA = 0x005A2324
OPEN_CALL_RVA = 0x005A2330
OPEN_OWNER_RVA = 0x005A2335
QUEUE_PUSH_RVA = 0x005A23EA
QUEUE_CALL_RVA = 0x005A23F6
QUEUE_OWNER_RVA = 0x005A23FB
QUEUE_DECODER_RETURN_RVA = 0x005A2413
THIRD_PUSH_RVA = 0x005A36EA
THIRD_OWNER_RVA = 0x005A36FC
BACKING_BYTES = 0x0004B000
OPERATOR_NEW_RVA = 0x005EB09C
ALLOCATOR_RVA = 0x005EACF8
MALLOC_ARGUMENT_PUSH_RVA = 0x005EAD0A
MALLOC_WRAPPER_RETURN_RVA = 0x005EAD12
FREE_WRAPPER_RVA = 0x005EACE5
FREE_IAT_RVA = 0x006064FC
MALLOC_IAT_RVA = 0x00606504


def fail(message: str) -> None:
    raise AssertionError(message)


class PE32Image:
    """Small fail-closed PE32 reader for this frozen contract only."""

    def __init__(self, raw: bytes) -> None:
        self.raw = raw
        if self._bytes(0, 2, "DOS signature") != b"MZ":
            fail("missing DOS signature")
        pe_offset = self._u32(0x3C, "PE header offset")
        if self._bytes(pe_offset, 4, "PE signature") != b"PE\0\0":
            fail("missing PE signature")

        coff = pe_offset + 4
        machine = self._u16(coff, "COFF machine")
        if machine != 0x014C:
            fail(f"unexpected COFF machine: 0x{machine:04x}")
        section_count = self._u16(coff + 2, "COFF section count")
        optional_size = self._u16(coff + 16, "optional-header size")
        optional = coff + 20
        if optional_size < 104:
            fail(f"PE32 optional header is too small: {optional_size}")
        self._bytes(optional, optional_size, "PE32 optional header")
        if self._u16(optional, "optional-header magic") != 0x010B:
            fail("image is not PE32")

        self.image_base = self._u32(optional + 28, "PE32 image base")
        self.size_of_headers = self._u32(
            optional + 60, "PE32 SizeOfHeaders"
        )
        directory_count = self._u32(
            optional + 92, "PE32 data-directory count"
        )
        if directory_count < 2 or optional_size < 96 + 2 * 8:
            fail("PE32 import data directory is absent")
        self.import_rva = self._u32(
            optional + 96 + 8, "import-directory RVA"
        )
        self.import_size = self._u32(
            optional + 96 + 12, "import-directory size"
        )

        section_table = optional + optional_size
        self.sections: list[dict[str, int | str]] = []
        names: set[str] = set()
        for index in range(section_count):
            offset = section_table + index * 40
            record = self._bytes(offset, 40, f"section header {index}")
            try:
                name = record[:8].split(b"\0", 1)[0].decode("ascii")
            except UnicodeDecodeError as error:
                fail(f"section {index} has a non-ASCII name: {error}")
            if not name or name in names:
                fail(f"invalid or duplicate section name: {name!r}")
            names.add(name)
            virtual_size, virtual_address, raw_size, raw_offset = (
                struct.unpack_from("<IIII", record, 8)
            )
            if raw_size:
                self._bytes(raw_offset, raw_size, f"section {name} data")
            span = max(virtual_size, raw_size)
            if virtual_address + span > 0x1_0000_0000:
                fail(f"section {name} wraps the PE32 address space")
            self.sections.append(
                {
                    "name": name,
                    "virtual_size": virtual_size,
                    "virtual_address": virtual_address,
                    "raw_size": raw_size,
                    "raw_offset": raw_offset,
                }
            )

    def _bytes(self, offset: int, size: int, subject: str) -> bytes:
        if offset < 0 or size < 0 or offset > len(self.raw) - size:
            fail(
                f"{subject} is outside the file: offset={offset} size={size}"
            )
        return self.raw[offset:offset + size]

    def _u16(self, offset: int, subject: str) -> int:
        return struct.unpack("<H", self._bytes(offset, 2, subject))[0]

    def _u32(self, offset: int, subject: str) -> int:
        return struct.unpack("<I", self._bytes(offset, 4, subject))[0]

    def section(self, name: str) -> dict[str, int | str]:
        matches = [entry for entry in self.sections if entry["name"] == name]
        if len(matches) != 1:
            fail(f"expected one {name} section, found {len(matches)}")
        return matches[0]

    def section_data(self, section: dict[str, int | str]) -> bytes:
        return self._bytes(
            int(section["raw_offset"]),
            int(section["raw_size"]),
            f"section {section['name']} data",
        )

    def rva_to_offset(self, rva: int, size: int, subject: str) -> int:
        if rva < 0 or rva > 0xFFFFFFFF or size < 0:
            fail(f"invalid RVA range for {subject}: {rva}+{size}")
        if rva < self.size_of_headers:
            if (
                size <= self.size_of_headers - rva
                and size <= len(self.raw) - rva
            ):
                return rva
            fail(f"{subject} crosses the mapped PE headers")

        matches: list[dict[str, int | str]] = []
        for section in self.sections:
            start = int(section["virtual_address"])
            span = max(
                int(section["virtual_size"]), int(section["raw_size"])
            )
            if start <= rva < start + span:
                matches.append(section)
        if len(matches) != 1:
            fail(
                f"{subject} RVA 0x{rva:08x} maps to {len(matches)} sections"
            )
        section = matches[0]
        delta = rva - int(section["virtual_address"])
        raw_size = int(section["raw_size"])
        if delta > raw_size or size > raw_size - delta:
            fail(f"{subject} lies in an unbacked virtual section tail")
        return int(section["raw_offset"]) + delta

    def data_at(self, rva: int, size: int, subject: str) -> bytes:
        return self._bytes(
            self.rva_to_offset(rva, size, subject), size, subject
        )

    def c_string(self, rva: int, subject: str) -> bytes:
        offset = self.rva_to_offset(rva, 1, subject)
        limit = min(len(self.raw), offset + 4096)
        end = self.raw.find(b"\0", offset, limit)
        if end < 0:
            fail(f"{subject} has no bounded NUL terminator")
        return self.raw[offset:end]

    def imports(self) -> dict[int, str]:
        if not self.import_rva or self.import_size < 20:
            fail("PE32 import directory is empty or truncated")
        directory_end = self.import_rva + self.import_size
        if directory_end > 0x1_0000_0000:
            fail("PE32 import directory wraps the RVA space")

        result: dict[int, str] = {}
        cursor = self.import_rva
        terminated = False
        descriptor_index = 0
        while cursor + 20 <= directory_end:
            descriptor = self.data_at(
                cursor, 20, f"import descriptor {descriptor_index}"
            )
            (
                original_first_thunk,
                timestamp,
                forwarder_chain,
                name_rva,
                first_thunk,
            ) = struct.unpack("<IIIII", descriptor)
            if not any(
                (
                    original_first_thunk,
                    timestamp,
                    forwarder_chain,
                    name_rva,
                    first_thunk,
                )
            ):
                terminated = True
                break
            if not name_rva or not first_thunk:
                fail(f"import descriptor {descriptor_index} is incomplete")
            try:
                library = self.c_string(
                    name_rva, f"import library {descriptor_index}"
                ).decode("ascii").lower()
            except UnicodeDecodeError as error:
                fail(f"import library {descriptor_index} is non-ASCII: {error}")

            lookup = original_first_thunk or first_thunk
            for thunk_index in range(65536):
                thunk_rva = lookup + thunk_index * 4
                thunk = struct.unpack(
                    "<I",
                    self.data_at(
                        thunk_rva,
                        4,
                        f"import thunk {descriptor_index}:{thunk_index}",
                    ),
                )[0]
                if not thunk:
                    break
                if thunk & 0x80000000:
                    symbol = f"#{thunk & 0xFFFF}"
                else:
                    try:
                        symbol = self.c_string(
                            thunk + 2,
                            f"import name {descriptor_index}:{thunk_index}",
                        ).decode("ascii")
                    except UnicodeDecodeError as error:
                        fail(
                            "import name "
                            f"{descriptor_index}:{thunk_index} is non-ASCII: "
                            f"{error}"
                        )
                iat_rva = first_thunk + thunk_index * 4
                if iat_rva in result:
                    fail(f"duplicate import IAT RVA: 0x{iat_rva:08x}")
                result[iat_rva] = f"{library}!{symbol}"
            else:
                fail(f"import descriptor {descriptor_index} has no terminator")
            cursor += 20
            descriptor_index += 1
        if not terminated:
            fail("PE32 import descriptors have no in-directory terminator")
        return result


def expect_bytes(
    image: PE32Image, rva: int, expected: bytes, description: str
) -> None:
    actual = image.data_at(rva, len(expected), description)
    if actual != expected:
        fail(
            f"{rva:08x}: {description} bytes {actual.hex()} != "
            f"{expected.hex()}"
        )


def expect_rel32(
    image: PE32Image, rva: int, opcode: int, target_rva: int,
    description: str,
) -> None:
    encoded = image.data_at(rva, 5, description)
    if encoded[0] != opcode:
        fail(
            f"{rva:08x}: {description} opcode 0x{encoded[0]:02x} != "
            f"0x{opcode:02x}"
        )
    displacement = struct.unpack("<i", encoded[1:])[0]
    actual_target = (rva + 5 + displacement) & 0xFFFFFFFF
    if actual_target != target_rva:
        fail(
            f"{rva:08x}: {description} target 0x{actual_target:08x} != "
            f"0x{target_rva:08x}"
        )


def expect_call(
    image: PE32Image, rva: int, target_rva: int, description: str
) -> None:
    expect_rel32(image, rva, 0xE8, target_rva, description)


def expect_jump(
    image: PE32Image, rva: int, target_rva: int, description: str
) -> None:
    expect_rel32(image, rva, 0xE9, target_rva, description)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=pathlib.Path)
    parser.add_argument(
        "--header",
        type=pathlib.Path,
        default=pathlib.Path(__file__).with_name("runtime")
        / "host_vita_ogg_emergency.h",
    )
    arguments = parser.parse_args()

    raw = arguments.pe.read_bytes()
    if len(raw) != EXPECTED_PE_BYTES:
        fail(f"unexpected PE size: {len(raw)}")
    digest = hashlib.sha256(raw).hexdigest()
    if digest != EXPECTED_PE_SHA256:
        fail(f"unexpected PE SHA-256: {digest}")

    header = arguments.header.read_text(encoding="utf-8")
    expected_defines = {
        "ISAAC_VITA_OGG_OPEN_BACKING_BYTES": BACKING_BYTES,
        "ISAAC_VITA_OGG_OPEN_ALLOCATION_CALL_RVA": OPEN_CALL_RVA,
        "ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA": OPEN_OWNER_RVA,
        "ISAAC_VITA_OGG_QUEUE_ALLOCATION_RETURN_RVA": QUEUE_OWNER_RVA,
        "ISAAC_VITA_OGG_QUEUE_DECODER_RETURN_RVA":
            QUEUE_DECODER_RETURN_RVA,
        "ISAAC_VITA_OGG_THIRD_SIZE_OWNER_RVA": THIRD_OWNER_RVA,
    }
    for name, value in expected_defines.items():
        match = re.search(
            rf"^#define\s+{re.escape(name)}\s+(0x[0-9a-fA-F]+)U\s*$",
            header,
            flags=re.MULTILINE,
        )
        if not match or int(match.group(1), 16) != value:
            fail(f"header fact drifted: {name}")

    pe = PE32Image(raw)
    if pe.image_base != 0x00400000:
        fail("unexpected PE image base")
    text = pe.section(".text")
    push_pattern = b"\x68" + struct.pack("<I", BACKING_BYTES)
    text_bytes = pe.section_data(text)
    occurrences: list[int] = []
    offset = 0
    while True:
        offset = text_bytes.find(push_pattern, offset)
        if offset < 0:
            break
        occurrences.append(int(text["virtual_address"]) + offset)
        offset += 1
    if occurrences != [OPEN_PUSH_RVA, QUEUE_PUSH_RVA, THIRD_PUSH_RVA]:
        fail(f"0x4b000 push census drifted: {occurrences}")

    expect_bytes(pe, OPEN_PUSH_RVA, push_pattern, "Open push 0x4b000")
    expect_bytes(
        pe,
        0x005A2329,
        b"\xc7\x47\x04" + struct.pack("<I", BACKING_BYTES),
        "Open stores the backing size",
    )
    expect_call(
        pe, OPEN_CALL_RVA, OPERATOR_NEW_RVA, "Open common operator-new call"
    )
    expect_bytes(
        pe, OPEN_OWNER_RVA, b"\x8b\x4d\xfc", "Open owner instruction"
    )
    expect_bytes(pe, 0x005A2338, b"\x89\x07", "Open backing store")
    expect_call(pe, 0x005A2346, 0x005BD740, "Open decoder call")
    expect_bytes(
        pe,
        0x005A234B,
        b"\x89\x86" + struct.pack("<I", 0x90),
        "Open decoder-result store",
    )

    expect_bytes(pe, QUEUE_PUSH_RVA, push_pattern, "Queue push 0x4b000")
    expect_call(
        pe, QUEUE_CALL_RVA, OPERATOR_NEW_RVA, "Queue common operator-new call"
    )
    expect_bytes(pe, QUEUE_OWNER_RVA, b"\x89\x07", "Queue owner instruction")
    expect_call(pe, 0x005A240E, 0x005BD740, "Queue decoder call")
    expect_bytes(
        pe,
        QUEUE_DECODER_RETURN_RVA,
        b"\x89\x03",
        "Queue decoder-result store",
    )
    expect_bytes(pe, THIRD_PUSH_RVA, push_pattern, "third-site push 0x4b000")
    expect_call(
        pe, 0x005A36F7, OPERATOR_NEW_RVA, "third-site operator-new call"
    )
    expect_bytes(
        pe,
        THIRD_OWNER_RVA,
        b"\x8b\x4c\x24\x24",
        "third-site owner instruction",
    )

    # Active backing ownership: constructor zeroes this+0x94; Open stores it;
    # destructor, ActivateNextQueuedData, and Close all reach the same original
    # free wrapper.  Queue backing is transferred into the active field before
    # its 16-byte node is deleted without freeing that transferred backing.
    expect_bytes(
        pe,
        0x0056F3CE,
        b"\xc7\x86" + struct.pack("<I", 0x94) + struct.pack("<I", 0),
        "constructor clears active OGG backing",
    )
    for call_rva in (
        0x005A1FFD,
        0x005A2038,
        0x005A2224,
        0x005A253A,
        0x005A2581,
    ):
        expect_call(
            pe, call_rva, FREE_WRAPPER_RVA, "OGG backing free-wrapper call"
        )
    expect_bytes(
        pe,
        0x005A2285,
        b"\x89\x86" + struct.pack("<I", 0x94),
        "queued backing becomes active",
    )
    expect_call(pe, 0x005A22B2, 0x005EB08E, "queue-node delete call")

    # Freeze the operator-new malloc return used by the import-boundary owner
    # check.  The wrapper leaves the caller frame unchanged, the allocator
    # pushes [ebp+8], and its malloc call therefore exposes the exact caller
    # return at [ESP+12].  Also freeze every backing free edge to the IAT.
    expect_bytes(
        pe,
        OPERATOR_NEW_RVA,
        b"\x55\x8b\xec\x5d",
        "operator-new stack-neutral wrapper",
    )
    expect_jump(pe, 0x005EB0A0, ALLOCATOR_RVA, "operator-new allocator jump")
    expect_bytes(
        pe, ALLOCATOR_RVA, b"\x55\x8b\xec", "allocator frame prologue"
    )
    expect_bytes(
        pe,
        MALLOC_ARGUMENT_PUSH_RVA,
        b"\xff\x75\x08",
        "allocator pushes its size argument",
    )
    expect_call(pe, 0x005EAD0D, 0x005EC17C, "allocator malloc-IAT thunk call")
    expect_bytes(
        pe,
        MALLOC_WRAPPER_RETURN_RVA,
        b"\x59",
        "operator-new malloc wrapper return",
    )
    expect_bytes(
        pe,
        0x005EC17C,
        b"\xff\x25" + struct.pack("<I", pe.image_base + MALLOC_IAT_RVA),
        "malloc IAT jump",
    )
    expect_jump(pe, FREE_WRAPPER_RVA, 0x005EBD94, "free-wrapper jump")
    expect_jump(pe, 0x005EBD94, 0x005EC176, "free-IAT thunk jump")
    expect_bytes(
        pe,
        0x005EC176,
        b"\xff\x25" + struct.pack("<I", pe.image_base + FREE_IAT_RVA),
        "free IAT jump",
    )

    imports = pe.imports()
    if imports.get(FREE_IAT_RVA) != (
        "api-ms-win-crt-heap-l1-1-0.dll!free"
    ):
        fail("free IAT mapping drifted")
    if imports.get(MALLOC_IAT_RVA) != (
        "api-ms-win-crt-heap-l1-1-0.dll!malloc"
    ):
        fail("malloc IAT mapping drifted")

    print(
        "Vita OGG emergency frozen contract: PASS; "
        "Open=005a2335/0x4b000; Queue=005a23fb; "
        "decoder-return=005a2413; third-size-owner=005a36fc"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
