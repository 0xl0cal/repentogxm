#!/usr/bin/env python3
"""Reproduce the frozen Repentance RoomConfig-entry target-hybrid receipt.

This is an opt-in test: it reads a legally installed, proprietary
``repentance.a`` supplied on the command line.  No game bytes are stored here.

The ARCH000 name hashes, MiniZ block framing, per-entry ISAAC seed, and ISAAC
implementation are an independent Python port of Rick Gibbed's zlib-licensed
Gibbed.Rebirth sources at commit 8454c449cc60d680d57e3edb27ea9e56009c024a:

* projects/Gibbed.Rebirth.FileFormats/ArchiveFile.cs
* projects/Gibbed.Rebirth.FileFormats/ArchiveEntry.cs
* projects/Gibbed.Rebirth.FileFormats/ISAAC.cs

https://github.com/gibbed/Gibbed.Rebirth/tree/8454c449cc60d680d57e3edb27ea9e56009c024a

The STB1 field layouts follow Basement Renovator's ``src/roomconvert.py`` at
commit c2ae4956764096238f47615face6d67325489cbc:

https://github.com/Basement-Renovator/Basement-Renovator/blob/c2ae4956764096238f47615face6d67325489cbc/src/roomconvert.py
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterable


ARCHIVE_SIZE = 385_003_320
ARCHIVE_SHA256 = "b58a2c74f49f106be54021961b5bb11a8428861e125d7430efa24ded652187ea"
ARCHIVE_SIGNATURE = b"ARCH000"
ARCHIVE_MINIZ_MODE = 2
ARCHIVE_HEADER = struct.Struct("<7sBIH")
ARCHIVE_INDEX_ENTRY = struct.Struct("<IIIII")

STB_HEADER = struct.Struct("<4sI")
STB_ROOM_BEGIN = struct.Struct("<IIIBH")
STB_ROOM_END = struct.Struct("<fBBBBH")
STB_DOOR = struct.Struct("<hh?")
STB_STACK = struct.Struct("<hhB")
STB_ENTITY = struct.Struct("<HHHf")

SLOTS_PER_PAGE = 4_032
# Design target only; this receipt does not assert the compiled runtime tier.
TARGET_HYBRID_PAGES = 192

# Canonical DLC3 room paths from the ResourceExtractor file list.  These are
# archive names only; the test resolves and reads their bytes from the user's
# hard-pinned archive.
ROOM_PATHS = (
    "resources-dlc3/rooms/00.special rooms.stb",
    "resources-dlc3/rooms/01.basement.stb",
    "resources-dlc3/rooms/02.cellar.stb",
    "resources-dlc3/rooms/03.burning basement.stb",
    "resources-dlc3/rooms/04.caves.stb",
    "resources-dlc3/rooms/05.catacombs.stb",
    "resources-dlc3/rooms/06.flooded caves.stb",
    "resources-dlc3/rooms/07.depths.stb",
    "resources-dlc3/rooms/08.necropolis.stb",
    "resources-dlc3/rooms/09.dank depths.stb",
    "resources-dlc3/rooms/10.womb.stb",
    "resources-dlc3/rooms/11.utero.stb",
    "resources-dlc3/rooms/12.scarred womb.stb",
    "resources-dlc3/rooms/13.blue womb.stb",
    "resources-dlc3/rooms/14.sheol.stb",
    "resources-dlc3/rooms/15.cathedral.stb",
    "resources-dlc3/rooms/16.dark room.stb",
    "resources-dlc3/rooms/17.chest.stb",
    "resources-dlc3/rooms/26.the void.stb",
    "resources-dlc3/rooms/27.downpour.stb",
    "resources-dlc3/rooms/28.dross.stb",
    "resources-dlc3/rooms/29.mines.stb",
    "resources-dlc3/rooms/30.ashpit.stb",
    "resources-dlc3/rooms/31.mausoleum.stb",
    "resources-dlc3/rooms/32.gehenna.stb",
    "resources-dlc3/rooms/33.corpse.stb",
    "resources-dlc3/rooms/34.mortis.stb",
    "resources-dlc3/rooms/35.home.stb",
    "resources-dlc3/rooms/36.backwards.stb",
    "resources-dlc3/rooms/greed/00.special rooms.stb",
    "resources-dlc3/rooms/greed/01.basement.stb",
    "resources-dlc3/rooms/greed/02.cellar.stb",
    "resources-dlc3/rooms/greed/03.burning basement.stb",
    "resources-dlc3/rooms/greed/04.caves.stb",
    "resources-dlc3/rooms/greed/05.catacombs.stb",
    "resources-dlc3/rooms/greed/06.flooded caves.stb",
    "resources-dlc3/rooms/greed/07.depths.stb",
    "resources-dlc3/rooms/greed/08.necropolis.stb",
    "resources-dlc3/rooms/greed/09.dank depths.stb",
    "resources-dlc3/rooms/greed/10.womb.stb",
    "resources-dlc3/rooms/greed/11.utero.stb",
    "resources-dlc3/rooms/greed/12.scarred womb.stb",
    "resources-dlc3/rooms/greed/14.sheol.stb",
    "resources-dlc3/rooms/greed/24.the shop.stb",
    "resources-dlc3/rooms/greed/25.ultra greed.stb",
    "resources-dlc3/rooms/greed/27.downpour.stb",
    "resources-dlc3/rooms/greed/28.dross.stb",
    "resources-dlc3/rooms/greed/29.mines.stb",
    "resources-dlc3/rooms/greed/30.ashpit.stb",
    "resources-dlc3/rooms/greed/31.mausoleum.stb",
    "resources-dlc3/rooms/greed/32.gehenna.stb",
    "resources-dlc3/rooms/greed/33.corpse.stb",
    "resources-dlc3/rooms/greed/34.mortis.stb",
)

EXPECTED = {
    "archive_entries": 4_180,
    "archive_index_offset": 0x16F168A8,
    "target_hybrid_margin": 29_387,
    "target_hybrid_pages": 192,
    "target_hybrid_slots": 774_144,
    "entities": 753_721,
    "files": 53,
    "max_stack_entities": 18,
    "required_pages": 185,
    "rooms": 28_464,
    "singleton_stacks": 744_757,
    "stacks": 748_541,
    "uncompressed_bytes": 12_946_161,
}

MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF


class ReceiptError(RuntimeError):
    """A pinned-input, archive, STB, or receipt invariant failed."""


@dataclass(frozen=True)
class ArchiveEntry:
    hash_a: int
    hash_b: int
    offset: int
    length: int
    checksum: int


@dataclass
class StbCounts:
    rooms: int = 0
    stacks: int = 0
    singleton_stacks: int = 0
    entities: int = 0
    max_stack_entities: int = 0

    def add(self, other: "StbCounts") -> None:
        self.rooms += other.rooms
        self.stacks += other.stacks
        self.singleton_stacks += other.singleton_stacks
        self.entities += other.entities
        self.max_stack_entities = max(
            self.max_stack_entities, other.max_stack_entities
        )


def fail(message: str) -> "ReceiptError":
    return ReceiptError(message)


def read_exact(stream: BinaryIO, size: int, context: str) -> bytes:
    data = stream.read(size)
    if len(data) != size:
        raise fail(f"short read for {context}: wanted {size}, got {len(data)}")
    return data


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def name_hash(path: str) -> tuple[int, int]:
    lowered = path.lower()
    try:
        encoded = lowered.encode("ascii")
    except UnicodeEncodeError as exc:
        raise fail(f"non-ASCII archive path: {path!r}") from exc

    hash_a = 5_381
    hash_b = 0x5BB2220E
    for byte in encoded:
        hash_a = (hash_a * 33 + byte) & MASK32
        hash_b = ((hash_b ^ byte) * 0x1000193) & MASK32
    return hash_a, hash_b


def mix(values: list[int]) -> list[int]:
    a, b, c, d, e, f, g, h = values
    a = (a ^ ((b << 11) & MASK32)) & MASK32
    d = (d + a) & MASK32
    b = (b + c) & MASK32
    b = (b ^ (c >> 2)) & MASK32
    e = (e + b) & MASK32
    c = (c + d) & MASK32
    c = (c ^ ((d << 8) & MASK32)) & MASK32
    f = (f + c) & MASK32
    d = (d + e) & MASK32
    d = (d ^ (e >> 16)) & MASK32
    g = (g + d) & MASK32
    e = (e + f) & MASK32
    e = (e ^ ((f << 10) & MASK32)) & MASK32
    h = (h + e) & MASK32
    f = (f + g) & MASK32
    f = (f ^ (g >> 4)) & MASK32
    a = (a + f) & MASK32
    g = (g + h) & MASK32
    g = (g ^ ((h << 8) & MASK32)) & MASK32
    b = (b + g) & MASK32
    h = (h + a) & MASK32
    h = (h ^ (a >> 9)) & MASK32
    c = (c + h) & MASK32
    a = (a + b) & MASK32
    return [a, b, c, d, e, f, g, h]


class Isaac:
    SIZE = 256
    MASK = (SIZE - 1) << 2

    def __init__(self, seed_words: Iterable[int]) -> None:
        seed = [word & MASK32 for word in seed_words]
        if len(seed) != self.SIZE:
            raise fail(f"ISAAC seed has {len(seed)} words, expected {self.SIZE}")
        self.results = seed
        self.state = [0] * self.SIZE
        self.index = 0
        self.a = 0
        self.b = 0
        self.c = 0
        self._initialize()

    def _initialize(self) -> None:
        values = [0x9E3779B9] * 8
        for _ in range(4):
            values = mix(values)
        for base in range(0, self.SIZE, 8):
            values = [
                (values[index] + self.results[base + index]) & MASK32
                for index in range(8)
            ]
            values = mix(values)
            self.state[base : base + 8] = values
        for base in range(0, self.SIZE, 8):
            values = [
                (values[index] + self.state[base + index]) & MASK32
                for index in range(8)
            ]
            values = mix(values)
            self.state[base : base + 8] = values
        self._generate()

    def _step(self, index: int, other: int, shift: int, right: bool) -> None:
        x = self.state[index]
        shifted = self.a >> shift if right else (self.a << shift) & MASK32
        self.a = (self.a ^ shifted) & MASK32
        self.a = (self.a + self.state[other]) & MASK32
        y = (self.state[(x & self.MASK) >> 2] + self.a + self.b) & MASK32
        self.state[index] = y
        self.b = (self.state[((y >> 8) & self.MASK) >> 2] + x) & MASK32
        self.results[index] = self.b

    def _generate(self) -> None:
        self.c = (self.c + 1) & MASK32
        self.b = (self.b + self.c) & MASK32
        shifts = ((13, False), (6, True), (2, False), (16, True))
        for index in range(self.SIZE):
            if index < self.SIZE // 2:
                other = index + self.SIZE // 2
            else:
                other = index - self.SIZE // 2
            shift, right = shifts[index & 3]
            self._step(index, other, shift, right)

    def value(self) -> int:
        result = self.results[self.index]
        self.index += 1
        if self.index == self.SIZE:
            self._generate()
            self.index = 0
        return result


def entry_isaac(hash_b: int) -> Isaac:
    seed = hash_b & MASK64
    low = (seed ^ (((seed ^ ((seed << 15) & MASK64)) << 8) & MASK64) ^ (seed >> 9))
    seed = (((seed << 32) & MASK64) | (low & MASK32)) & MASK64
    words: list[int] = []
    for _ in range(256):
        part = ((seed >> 27) ^ (seed >> 45)) & MASK32
        shift = seed >> 59
        word = ((part >> shift) | ((part << ((-shift) & 31)) & MASK32)) & MASK32
        words.append(word)
        seed = (seed * 6_364_136_223_846_793_005 + 127) & MASK64
    return Isaac(words)


def decrypt_raw_block(data: bytes, isaac: Isaac) -> bytes:
    output = bytearray(data)
    word = 0
    for offset in range(len(output)):
        byte_index = offset & 3
        if byte_index == 0:
            word = isaac.value()
        output[offset] ^= (word >> (byte_index * 8)) & 0xFF
    return bytes(output)


def load_index(stream: BinaryIO, archive_size: int) -> tuple[int, list[ArchiveEntry]]:
    stream.seek(0)
    signature, mode, index_offset, entry_count = ARCHIVE_HEADER.unpack(
        read_exact(stream, ARCHIVE_HEADER.size, "archive header")
    )
    if signature != ARCHIVE_SIGNATURE:
        raise fail(f"bad archive signature: {signature!r}")
    if mode != ARCHIVE_MINIZ_MODE:
        raise fail(f"archive compression mode is {mode}, expected MiniZ mode 2")
    index_end = index_offset + entry_count * ARCHIVE_INDEX_ENTRY.size
    if index_end != archive_size:
        raise fail(
            f"archive index does not close at EOF: 0x{index_end:x} != 0x{archive_size:x}"
        )
    stream.seek(index_offset)
    entries = [
        ArchiveEntry(*ARCHIVE_INDEX_ENTRY.unpack(read_exact(
            stream, ARCHIVE_INDEX_ENTRY.size, f"index entry {index}"
        )))
        for index in range(entry_count)
    ]
    return index_offset, entries


def extract_entry(stream: BinaryIO, entry: ArchiveEntry, index_offset: int) -> bytes:
    if entry.offset < ARCHIVE_HEADER.size or entry.offset >= index_offset:
        raise fail(f"entry offset 0x{entry.offset:x} is outside the data area")
    stream.seek(entry.offset)
    output = bytearray()
    raw_mode = False
    isaac: Isaac | None = None
    block_index = 0
    while True:
        if stream.tell() + 4 > index_offset:
            raise fail("entry block header crosses the archive index")
        flags = struct.unpack("<I", read_exact(
            stream, 4, f"entry block {block_index} flags"
        ))[0]
        block_length = flags & 0x7FFFFFFF
        is_last = bool(flags & 0x80000000)
        if block_length > 0x800:
            raise fail(f"entry block {block_index} is too large: {block_length}")
        if stream.tell() + block_length > index_offset:
            raise fail(f"entry block {block_index} crosses the archive index")
        payload = read_exact(
            stream, block_length, f"entry block {block_index} payload"
        )
        remaining = entry.length - len(output)
        if remaining < 0:
            raise fail("entry output exceeded its declared length")

        if raw_mode or (not is_last and block_length == 1024):
            raw_mode = True
            if block_length > remaining:
                raise fail("raw entry block exceeds the declared output length")
            if isaac is None:
                isaac = entry_isaac(entry.hash_b)
            output.extend(decrypt_raw_block(payload, isaac))
        else:
            inflater = zlib.decompressobj(-15)
            try:
                decoded = inflater.decompress(payload) + inflater.flush()
            except zlib.error as exc:
                raise fail(f"raw-deflate failed in entry block {block_index}: {exc}") from exc
            # Gibbed's reader asks the raw inflater for one output window; an
            # archive block may deliberately end before a DEFLATE end marker.
            # It must still consume the whole framed payload without trailing
            # or deferred input.
            if inflater.unused_data or inflater.unconsumed_tail:
                raise fail(f"entry block {block_index} did not consume its exact payload")
            if len(decoded) > min(remaining, 1024):
                raise fail(f"entry block {block_index} inflated past its output window")
            output.extend(decoded)

        block_index += 1
        if is_last:
            break
        if len(output) >= entry.length:
            raise fail("entry reached its declared length before the final block")
    if len(output) != entry.length:
        raise fail(f"entry length mismatch: decoded {len(output)}, expected {entry.length}")
    return bytes(output)


def ensure_range(data: bytes, offset: int, size: int, context: str) -> None:
    if offset < 0 or size < 0 or offset + size > len(data):
        raise fail(
            f"{context} crosses STB EOF: offset {offset}, size {size}, file {len(data)}"
        )


def parse_stb(data: bytes, path: str) -> StbCounts:
    ensure_range(data, 0, STB_HEADER.size, f"{path} header")
    signature, room_count = STB_HEADER.unpack_from(data, 0)
    if signature != b"STB1":
        raise fail(f"{path}: bad STB signature {signature!r}")
    counts = StbCounts(rooms=room_count)
    offset = STB_HEADER.size
    for room_index in range(room_count):
        context = f"{path}: room {room_index}"
        ensure_range(data, offset, STB_ROOM_BEGIN.size, f"{context} prefix")
        _, _, _, _, name_length = STB_ROOM_BEGIN.unpack_from(data, offset)
        offset += STB_ROOM_BEGIN.size
        ensure_range(data, offset, name_length, f"{context} name")
        offset += name_length

        ensure_range(data, offset, STB_ROOM_END.size, f"{context} table")
        _, _, _, _, door_count, stack_count = STB_ROOM_END.unpack_from(data, offset)
        offset += STB_ROOM_END.size
        door_bytes = door_count * STB_DOOR.size
        ensure_range(data, offset, door_bytes, f"{context} doors")
        offset += door_bytes

        counts.stacks += stack_count
        for stack_index in range(stack_count):
            ensure_range(data, offset, STB_STACK.size, f"{context} stack {stack_index}")
            _, _, entity_count = STB_STACK.unpack_from(data, offset)
            offset += STB_STACK.size
            counts.entities += entity_count
            counts.singleton_stacks += entity_count == 1
            counts.max_stack_entities = max(counts.max_stack_entities, entity_count)
            entity_bytes = entity_count * STB_ENTITY.size
            ensure_range(data, offset, entity_bytes, f"{context} stack {stack_index} entities")
            offset += entity_bytes
    if offset != len(data):
        raise fail(f"{path}: parser stopped at {offset}, file length is {len(data)}")
    return counts


def build_receipt(path: Path) -> dict[str, int | str]:
    size = path.stat().st_size
    if size != ARCHIVE_SIZE:
        raise fail(f"archive size is {size}, expected pinned size {ARCHIVE_SIZE}")
    digest = sha256_file(path)
    if digest != ARCHIVE_SHA256:
        raise fail(f"archive SHA-256 is {digest}, expected {ARCHIVE_SHA256}")
    if len(ROOM_PATHS) != 53 or tuple(sorted(set(ROOM_PATHS))) != ROOM_PATHS:
        raise fail("embedded room path closure is not 53 unique sorted names")

    with path.open("rb") as stream:
        index_offset, entries = load_index(stream, size)
        by_hash: dict[tuple[int, int], list[ArchiveEntry]] = {}
        for entry in entries:
            by_hash.setdefault((entry.hash_a, entry.hash_b), []).append(entry)

        target_hashes = [name_hash(room_path) for room_path in ROOM_PATHS]
        if len(set(target_hashes)) != len(target_hashes):
            raise fail("embedded room paths collide under the archive name hash")

        total = StbCounts()
        uncompressed_bytes = 0
        for room_path, target_hash in zip(ROOM_PATHS, target_hashes):
            matches = by_hash.get(target_hash, [])
            if len(matches) != 1:
                raise fail(
                    f"{room_path}: expected one archive entry, found {len(matches)}"
                )
            entry = matches[0]
            data = extract_entry(stream, entry, index_offset)
            uncompressed_bytes += len(data)
            total.add(parse_stb(data, room_path))

    required_pages = (total.singleton_stacks + SLOTS_PER_PAGE - 1) // SLOTS_PER_PAGE
    target_hybrid_slots = TARGET_HYBRID_PAGES * SLOTS_PER_PAGE
    receipt: dict[str, int | str] = {
        "archive_entries": len(entries),
        "archive_index_offset": index_offset,
        "archive_sha256": digest,
        "archive_size": size,
        "target_hybrid_margin": target_hybrid_slots - total.singleton_stacks,
        "target_hybrid_pages": TARGET_HYBRID_PAGES,
        "target_hybrid_slots": target_hybrid_slots,
        "entities": total.entities,
        "files": len(ROOM_PATHS),
        "max_stack_entities": total.max_stack_entities,
        "required_pages": required_pages,
        "rooms": total.rooms,
        "singleton_stacks": total.singleton_stacks,
        "stacks": total.stacks,
        "uncompressed_bytes": uncompressed_bytes,
    }
    for key, expected in EXPECTED.items():
        actual = receipt[key]
        if actual != expected:
            raise fail(f"receipt field {key} is {actual}, expected {expected}")
    return receipt


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="verify the pinned repentance.a RoomConfig singleton corpus"
    )
    parser.add_argument("archive", type=Path, help="path to the installed repentance.a")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        archive = args.archive.resolve(strict=True)
        if not archive.is_file():
            raise fail(f"not a regular file: {archive}")
        receipt = build_receipt(archive)
    except (OSError, ReceiptError) as exc:
        print(f"room-entry corpus receipt: FAIL: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(receipt, sort_keys=True, separators=(",", ":")))
    print("Room-entry corpus receipt: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
