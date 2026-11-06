#!/usr/bin/env python3
"""Offline, opt-in repacker for the game's KAGE ``ARCH000`` v2 (MiniZ) archives.

Run this on the PC *before* copying ``resources/packed`` to the Vita.  It
re-encodes every deflate piece of a version-2 archive as static Huffman
(zlib level 9, ``Z_FIXED``) while keeping the piece boundaries, flush cadence,
keys, decoded sizes, mount checksums, entry order and the index the engine's
reader expects.  The output is always a new file; nothing is rewritten in
place.  Stored (ISAAC-XOR) pieces and every non-MiniZ archive are copied
byte-for-byte.

Format facts (frozen PE isaac-ng.exe.unpacked.exe, SHA256
31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404; all
addresses are RVAs; the translated corpus is guest_0166.c / guest_0173.c /
guest_0175.c / guest_0178.c):

  mount            sub_00563880  reads 7 signature bytes (never compared), the
                                 mode byte -> archive table [0x7c7b64+8i+4]
                                 (0x563a3a..0x563a57), u32 index offset
                                 (0x563a69, seek at 0x563a8a), u16 entry count
                                 (0x563a7b); then per entry five u32 fields
                                 into a 0x1c record: +4 hash_a, +8 hash_b,
                                 +0xc offset, +0x10 size, +0x14 checksum
                                 (0x563aa6..0x563b0e).
  baked skips      0x563b11..0x563b2e compare the PE's skip list (descriptor
                                 table RVA 0x7aab40, 18 archives) with the
                                 entry checksum AT THE SAME INDEX -> the entry
                                 order and every checksum must be preserved.
  mount checksum   0x563b5e..0x563c2e: a 0x200-byte stack buffer zeroed once
                                 (memset at 0x563b78), the decoded entry read
                                 0x200 bytes at a time through
                                 ArchivedFile::read, and ceil(got/4) dwords
                                 folded with h = ror32(h, 1) + dword from the
                                 seed 0xababeb98 (0x563b73, loop 0x563be2);
                                 mismatch -> log (string RVA 0x75d678) and the
                                 entry is dropped from the 0x8000-slot hash
                                 table.
  ArchivedFile     sub_0059bfa0  ctor: +4 archive record, +8 stream, +0xc
                                 entry record, +0x14 MiniZ decoder (0x14
                                 bytes: +0 ISAAC state 0x810, +0xc tinfl
                                 state 0x2af0, +0x10 stored latch), +0x18
                                 position, +0x1c input buffer (0x800),
                                 +0x81c output ring (0x400), +0xc1c cursor,
                                 +0xc20 bytes in block, +0xc28 eof.
  reset            sub_0059c170  mode 2: tinfl state <- 0 (0x59c216), latch <-
                                 0 (0x59c223), ISAAC re-seeded from hash_b
                                 (sub_005c2f10, called at 0x59c23f), stream
                                 seeked to the entry offset (0x59c266).
  refill_buffer    sub_0059c270  mode 2 (0x59c32a..0x59c42d): u32 piece
                                 header = length | final << 31; length >=
                                 0x800 logs "ArchivedFile::refill_buffer MiniZ
                                 compressed buffer overflow." (RVA 0x7650bc),
                                 so length <= 0x7ff; the payload is read into
                                 +0x1c; sticky stored latch when !final &&
                                 length == 0x400 (0x59c3b5..0x59c3c7); latched
                                 -> memcpy + ISAAC XOR (sub_005b06f0 at
                                 0x59c422); else tinfl_decompress
                                 (sub_005aeb00 at 0x59c3f3) with flags =
                                 (!final) * 2 = TINFL_FLAG_HAS_MORE_INPUT
                                 (0x59c3da..0x59c3dd), out_start = out_next =
                                 +0x81c, out_size = 0x400 (0x59c3a7): one
                                 persistent tinfl state per entry and a
                                 WRAPPING 0x400-byte output ring (mask 0x3ff).
                                 The bytes tinfl produced ([ebp-0x98]) become
                                 the block size (+0xc20); unconsumed input is
                                 dropped.
  read / seek      sub_0059c750 / sub_0059c690 consume +0xc20 bytes per block;
                                 a backward seek calls sub_0059c170.
  keystream        sub_005c2f10  PCG32 (mult 0x5851f42d4c957f2d, inc 0x7f,
                                 seed = hash_b) fills 256 words, then ISAAC
                                 randinit (sub_005c30b0); sub_005b06f0 XORs
                                 one u32 per 4 bytes, regenerating with
                                 sub_005c2fd0 after word 256.

Entry shapes the stock packer produces (all three occur in the shipped
archives): pure deflate (last piece carries BFINAL), pure stored (first piece
is a full non-final 0x400 block, so the latch fires immediately), and MIXED:
a deflate prefix without any BFINAL block followed by stored pieces from the
first 1 KiB chunk that did not compress below 0x400.  The latch is sticky and
tinfl is never called again for that entry, so the prefix stream simply ends.

Consequences the packer must honour (and this tool verifies):

  * A non-final deflate piece must decode to exactly 0x400 bytes and be
    consumed completely by that call, so every piece ends on a byte boundary
    with a deflate sync marker (empty stored block, ``00 00 ff ff``) -- the
    stock archives satisfy this for 100 % of their pieces.  Because each piece
    refills the ring from slot 0, slot p holds the byte 0x400 back only when
    pieces are full, and a match may never reach farther back than 0x400 (the
    ring mask); zlib with a 10-bit window emits distances <= 762.
  * A non-final deflate piece must never be exactly 0x400 bytes long (that
    flips the entry into stored mode) and no piece may exceed 0x7ff bytes.
  * The last piece of a pure deflate entry carries the BFINAL block and no
    trailing bytes; stored pieces are copied unchanged (their keystream starts
    at the entry, independent of any deflate prefix).
"""

from __future__ import annotations

import argparse
import filecmp
import hashlib
import json
import mmap
import os
import shutil
import struct
import sys
import time
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import BinaryIO, Callable, Iterable, Iterator, Protocol, Sequence


MAGIC = b"ARCH000"
HEADER = struct.Struct("<7sBIH")
INDEX_ENTRY = struct.Struct("<IIIII")
MODE_RAW_XOR = 0
MODE_LZW = 1
MODE_MINIZ = 2
BLOCK = 0x400
PIECE_MAX = 0x7FF
PIECE_FINAL = 0x80000000
PIECE_HEADER = struct.Struct("<I")
WINDOW_BITS = 10
CHECKSUM_SEED = 0xABABEB98
CHECKSUM_CHUNK = 0x200
SYNC_MARKER = b"\x00\x00\xff\xff"
MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF

# Archives whose mount validation the Vita runtime short-circuits with a
# receipt pinned to the STOCK file (size + index SHA-256, see
# recomp/runtime/host_vita_archive_receipt.c).  A repacked copy can never be
# trusted by that receipt, so every boot would decode the whole archive again.
RECEIPT_ARCHIVES = ("afterbirth.a", "animations.a")
# tools/isaac_vita_halve_sfx.py writes a "<archive>.receipt" validation sidecar
# next to every archive it rewrites (size, index SHA-256, sampled windows; the
# runtime knob ISAAC_VITA_ARCHIVE_VALIDATION_SIDECAR trusts it).  It pins the
# exact bytes, so it is only worth copying next to an archive that did not
# change.  Same literal as ISAAC_VITA_ARCHIVE_SIDECAR_SUFFIX in
# recomp/runtime/host_vita_archive_sidecar.h.
SIDECAR_SUFFIX = ".receipt"

VERIFY_LEVELS = ("off", "zlib", "model", "full")


class RepackError(RuntimeError):
    """A container, piece, encoder or verification invariant failed."""


# ---------------------------------------------------------------------------
# container
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Entry:
    index: int
    hash_a: int
    hash_b: int
    offset: int
    size: int
    checksum: int

    @property
    def tag(self) -> str:
        return "%08x-%08x" % (self.hash_a, self.hash_b)


class Archive:
    """A read-only, memory-mapped ARCH000 archive."""

    def __init__(self, path: str | os.PathLike[str]) -> None:
        self.path = str(path)
        self._file: BinaryIO = open(self.path, "rb")
        try:
            self.buf: bytes | mmap.mmap = mmap.mmap(
                self._file.fileno(), 0, access=mmap.ACCESS_READ
            )
        except (ValueError, OSError):
            self.buf = self._file.read()
        try:
            self._parse()
        except BaseException:
            self.close()
            raise

    def _parse(self) -> None:
        self.size = len(self.buf)
        if self.size < HEADER.size:
            raise RepackError("%s: shorter than the 14-byte header" % self.path)
        magic, self.mode, self.index_offset, self.count = HEADER.unpack_from(
            self.buf, 0
        )
        if magic != MAGIC:
            raise RepackError("%s: not an ARCH000 archive" % self.path)
        index_end = self.index_offset + INDEX_ENTRY.size * self.count
        if self.index_offset < HEADER.size or index_end != self.size:
            raise RepackError(
                "%s: index does not close at EOF (0x%x + %d*20 != 0x%x)"
                % (self.path, self.index_offset, self.count, self.size)
            )
        self.entries: list[Entry] = []
        for i in range(self.count):
            hash_a, hash_b, offset, size, checksum = INDEX_ENTRY.unpack_from(
                self.buf, self.index_offset + INDEX_ENTRY.size * i
            )
            if offset < HEADER.size or offset > self.index_offset:
                raise RepackError(
                    "%s: entry %d offset 0x%x outside the data area"
                    % (self.path, i, offset)
                )
            self.entries.append(Entry(i, hash_a, hash_b, offset, size, checksum))

    def close(self) -> None:
        if isinstance(self.buf, mmap.mmap):
            self.buf.close()
        self._file.close()

    def __enter__(self) -> "Archive":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def payload_limits(self) -> dict[int, int]:
        """offset -> first byte after that payload (next higher offset or the index)."""
        offsets = sorted({e.offset for e in self.entries})
        limits: dict[int, int] = {}
        for pos, offset in enumerate(offsets):
            limits[offset] = (
                offsets[pos + 1] if pos + 1 < len(offsets) else self.index_offset
            )
        return limits


Piece = tuple[bytes, bool]


def iter_pieces(buf, offset: int, size: int, limit: int) -> Iterator[tuple[bytes, bool, int]]:
    """Yield (payload, final, header_position) exactly as sub_0059c270 reads them.

    An entry of size 0 has no pieces: the reader reaches EOF (+0x18 >= size)
    before its first refill (0x59c296).  Otherwise pieces follow each other
    until one carries the final bit."""
    if size == 0:
        return
    pos = offset
    while True:
        if pos + PIECE_HEADER.size > limit:
            raise RepackError("piece header at 0x%x crosses the entry limit 0x%x" % (pos, limit))
        header, = PIECE_HEADER.unpack_from(buf, pos)
        length = header & 0x7FFFFFFF
        final = bool(header & PIECE_FINAL)
        if length > PIECE_MAX:
            raise RepackError("piece at 0x%x is %d bytes (> 0x7ff)" % (pos, length))
        start = pos + PIECE_HEADER.size
        if start + length > limit:
            raise RepackError("piece at 0x%x crosses the entry limit 0x%x" % (pos, limit))
        yield bytes(buf[start:start + length]), final, pos
        pos = start + length
        if final:
            return


def entry_pieces(archive: Archive, entry: Entry, limit: int) -> list[Piece]:
    return [(payload, final) for payload, final, _ in iter_pieces(archive.buf, entry.offset, entry.size, limit)]


def latch_index(pieces: Sequence[Piece]) -> int:
    """Index of the piece at which the sticky stored latch (0x59c3b5..0x59c3c7)
    fires: the first non-final piece of exactly 0x400 bytes.  len(pieces) when
    the entry stays deflate throughout."""
    for number, (payload, final) in enumerate(pieces):
        if not final and len(payload) == BLOCK:
            return number
    return len(pieces)


def piece_ok(piece: bytes, final: bool) -> bool:
    return len(piece) <= PIECE_MAX and (final or len(piece) != BLOCK)


def pack_pieces(pieces: Iterable[Piece]) -> bytes:
    out = bytearray()
    for payload, final in pieces:
        if len(payload) > PIECE_MAX:
            raise RepackError("piece of %d bytes exceeds 0x7ff" % len(payload))
        out += PIECE_HEADER.pack(len(payload) | (PIECE_FINAL if final else 0))
        out += payload
    return bytes(out)


def payload_key(entry: Entry) -> tuple[int, int, int] | None:
    """Identity of an entry's payload for sharing between index rows.

    Two rows share one payload only when they agree on offset, decoded size
    and checksum.  A size-0 entry owns no bytes at all (the reader never
    refills it), so it can sit at the same offset as the next entry without
    being an alias of it; such rows are never shared (None)."""
    if entry.size == 0:
        return None
    return (entry.offset, entry.size, entry.checksum)


# ---------------------------------------------------------------------------
# mount checksum (sub_00563880, 0x563b5e..0x563c2e)
# ---------------------------------------------------------------------------

def mount_checksum(data: bytes) -> int:
    """h = 0xababeb98; per little-endian dword: h = ror32(h, 1) + dword.

    The mount loop reads the decoded entry in 0x200-byte chunks into ONE stack
    buffer that is zeroed once per entry (memset 0x563b78) and folds
    ceil(got/4) dwords of that buffer, so the padding bytes of a final partial
    dword are whatever the previous chunk left there (zeros for an entry
    shorter than 0x200).  The loop stops when the entry is exhausted or a
    short read happens (0x563c05..0x563c19)."""
    h = CHECKSUM_SEED
    size = len(data)
    buffer = bytearray(CHECKSUM_CHUNK)
    pos = 0
    remaining = size
    view = memoryview(data)
    while True:
        want = remaining if remaining < CHECKSUM_CHUNK else CHECKSUM_CHUNK
        got = min(want, size - pos)
        if got:
            buffer[:got] = view[pos:pos + got]
        dwords = (got + 3) >> 2
        if dwords:
            for d in memoryview(buffer)[:dwords * 4].cast("I"):
                h = (((h >> 1) | ((h & 1) << 31)) + d) & MASK32
        remaining -= want
        pos += want
        if not (remaining > 0 and got == want):
            break
    return h


# ---------------------------------------------------------------------------
# ISAAC keystream (sub_005c2f10 / sub_005c30b0 / sub_005c2fd0 / sub_005b06f0)
# ---------------------------------------------------------------------------

class IsaacKeystream:
    """The stored-piece keystream, seeded from the entry's hash_b.

    sub_005c2f10: low = ((seed << 15 ^ seed) << 8) ^ (seed >> 9) ^ seed,
    state = seed << 32 | low; 256 PCG32 outputs (xorshift 18, shift 27,
    rotate by state >> 59; state = state * 0x5851f42d4c957f2d + 0x7f) fill
    randrsl; sub_005c30b0 is Jenkins' randinit(TRUE); sub_005c2fd0 is
    isaac(); sub_005b06f0 consumes one word per 4 output bytes (low byte
    first) and regenerates as soon as word 256 has been taken."""

    MULT = 0x5851F42D4C957F2D
    INC = 0x7F

    def __init__(self, seed: int) -> None:
        seed &= MASK32
        low = ((((seed << 15) ^ seed) << 8) ^ (seed >> 9) ^ seed) & MASK32
        state = (seed << 32) | low
        rsl = [0] * 256
        for i in range(256):
            xs = ((state ^ (state >> 18)) >> 27) & MASK32
            rot = state >> 59
            rsl[i] = ((xs >> rot) | (xs << ((32 - rot) & 31))) & MASK32
            state = (state * self.MULT + self.INC) & MASK64
        self.rsl = rsl
        self.mm = [0] * 256
        self.aa = self.bb = self.cc = 0
        self._randinit()
        self.index = 0

    @staticmethod
    def _mix(v: list[int]) -> None:
        a, b, c, d, e, f, g, h = v
        a ^= (b << 11) & MASK32; d = (d + a) & MASK32; b = (b + c) & MASK32
        b ^= c >> 2;              e = (e + b) & MASK32; c = (c + d) & MASK32
        c ^= (d << 8) & MASK32;   f = (f + c) & MASK32; d = (d + e) & MASK32
        d ^= e >> 16;             g = (g + d) & MASK32; e = (e + f) & MASK32
        e ^= (f << 10) & MASK32;  h = (h + e) & MASK32; f = (f + g) & MASK32
        f ^= g >> 4;              a = (a + f) & MASK32; g = (g + h) & MASK32
        g ^= (h << 8) & MASK32;   b = (b + g) & MASK32; h = (h + a) & MASK32
        h ^= a >> 9;              c = (c + h) & MASK32; a = (a + b) & MASK32
        v[:] = [a, b, c, d, e, f, g, h]

    def _randinit(self) -> None:
        v = [0x9E3779B9] * 8
        for _ in range(4):
            self._mix(v)
        for source in (self.rsl, self.mm):
            for i in range(0, 256, 8):
                for j in range(8):
                    v[j] = (v[j] + source[i + j]) & MASK32
                self._mix(v)
                self.mm[i:i + 8] = v
        self.generate()

    def generate(self) -> None:
        mm, rsl = self.mm, self.rsl
        aa, bb = self.aa, self.bb
        self.cc = (self.cc + 1) & MASK32
        bb = (bb + self.cc) & MASK32
        for i in range(256):
            x = mm[i]
            s = i & 3
            if s == 0:
                aa ^= (aa << 13) & MASK32
            elif s == 1:
                aa ^= aa >> 6
            elif s == 2:
                aa ^= (aa << 2) & MASK32
            else:
                aa ^= aa >> 16
            aa = (aa + mm[(i + 128) & 0xFF]) & MASK32
            y = (mm[(x >> 2) & 0xFF] + aa + bb) & MASK32
            mm[i] = y
            bb = (mm[(y >> 10) & 0xFF] + x) & MASK32
            rsl[i] = bb
        self.aa, self.bb = aa, bb

    def apply(self, data: bytes) -> bytes:
        """XOR one piece exactly as one sub_005b06f0 call does."""
        n = len(data)
        if n == 0:
            return b""
        words: list[int] = []
        need = (n + 3) >> 2
        while need:
            take = min(need, 256 - self.index)
            words.extend(self.rsl[self.index:self.index + take])
            self.index += take
            need -= take
            if self.index >= 256:
                self.generate()
                self.index = 0
        stream = struct.pack("<%dI" % len(words), *words)[:n]
        return (int.from_bytes(data, "little") ^ int.from_bytes(stream, "little")).to_bytes(n, "little")


def decode_stored_pieces(pieces: Sequence[Piece], hash_b: int) -> bytes:
    keystream = IsaacKeystream(hash_b)
    out = bytearray()
    for payload, _final in pieces:
        out += keystream.apply(payload)
    return bytes(out)


def encode_stored_pieces(data: bytes, hash_b: int) -> list[Piece]:
    """Stored mode as the stock packer writes it: full 0x400 pieces XORed with
    the keystream; the first piece must be a full non-final block for the
    reader's latch to fire, so this needs more than 0x400 bytes."""
    n = (len(data) + BLOCK - 1) // BLOCK
    if n < 2:
        raise RepackError("stored mode needs more than 0x400 bytes")
    keystream = IsaacKeystream(hash_b)
    return [
        (keystream.apply(data[i * BLOCK:(i + 1) * BLOCK]), i == n - 1)
        for i in range(n)
    ]


# ---------------------------------------------------------------------------
# entry shape
# ---------------------------------------------------------------------------

@dataclass
class EntryShape:
    """How the reader consumes one entry: ``deflate`` pieces (all non-final
    unless ``terminal``), then ``stored`` pieces from the latch on."""
    deflate: list[Piece]
    stored: list[Piece]

    @property
    def terminal(self) -> bool:
        return not self.stored

    @property
    def kind(self) -> str:
        if not self.deflate and not self.stored:
            return "empty"
        if not self.deflate:
            return "stored"
        if not self.stored:
            return "deflate"
        return "mixed"

    @property
    def deflate_bytes(self) -> int:
        """Decoded bytes the deflate prefix must produce."""
        if not self.deflate:
            return 0
        if self.terminal:
            return -1   # the entry size decides
        return BLOCK * len(self.deflate)


def shape_of(pieces: Sequence[Piece]) -> EntryShape:
    latch = latch_index(pieces)
    return EntryShape(list(pieces[:latch]), list(pieces[latch:]))


# ---------------------------------------------------------------------------
# zlib-side decode with the reader's cadence
# ---------------------------------------------------------------------------

@dataclass
class DeflateDecode:
    data: bytes
    first_block_types: list[tuple[int, int]]   # (bfinal, btype) of each piece's first block


def decode_deflate_pieces(pieces: Sequence[Piece], expected: int, terminal: bool) -> DeflateDecode:
    """Feed the pieces to ONE raw inflater the way sub_0059c270 feeds tinfl.

    Every non-final piece must yield exactly 0x400 bytes, be consumed
    completely and end on the sync marker.  With ``terminal`` the last piece
    is final and must end the stream; otherwise no piece is final (the stored
    latch follows).  ``expected`` is the total decoded size."""
    inflater = zlib.decompressobj(-15)
    out = bytearray()
    headers: list[tuple[int, int]] = []
    if not pieces:
        raise RepackError("deflate prefix without pieces")
    for number, (payload, final) in enumerate(pieces):
        if not payload:
            raise RepackError("piece %d is empty" % number)
        headers.append((payload[0] & 1, (payload[0] >> 1) & 3))
        if final != (terminal and number == len(pieces) - 1):
            raise RepackError("piece %d has an unexpected final flag" % number)
        try:
            produced = inflater.decompress(payload)
        except zlib.error as exc:
            raise RepackError("piece %d: raw deflate error: %s" % (number, exc)) from exc
        out += produced
        if inflater.unconsumed_tail:
            raise RepackError("piece %d left %d unconsumed bytes" % (number, len(inflater.unconsumed_tail)))
        if final:
            if not inflater.eof:
                raise RepackError("final piece %d does not end the deflate stream" % number)
            if inflater.unused_data:
                raise RepackError("final piece %d has %d trailing bytes" % (number, len(inflater.unused_data)))
        else:
            if len(produced) != BLOCK:
                raise RepackError("non-final piece %d produced %d bytes, not 0x400" % (number, len(produced)))
            if payload[-4:] != SYNC_MARKER:
                raise RepackError("non-final piece %d does not end on a sync marker" % number)
    if not terminal and inflater.eof:
        raise RepackError("deflate prefix ends the stream before the stored latch")
    if len(out) != expected:
        raise RepackError("deflate pieces decoded %d bytes, expected %d" % (len(out), expected))
    return DeflateDecode(bytes(out), headers)


def decode_entry(shape: EntryShape, entry: Entry, stored_too: bool) -> tuple[bytes | None, DeflateDecode | None]:
    """Decode an entry through zlib (+ the keystream for stored pieces when
    ``stored_too``).  Returns (full data or None, deflate decode or None)."""
    prefix: DeflateDecode | None = None
    data = bytearray()
    if shape.deflate:
        expected = entry.size if shape.terminal else BLOCK * len(shape.deflate)
        prefix = decode_deflate_pieces(shape.deflate, expected, shape.terminal)
        data += prefix.data
    if shape.stored:
        if not stored_too:
            return None, prefix
        data += decode_stored_pieces(shape.stored, entry.hash_b)
        if len(data) < entry.size:
            raise RepackError("entry decoded %d bytes, index says %d" % (len(data), entry.size))
        del data[entry.size:]
    return bytes(data), prefix


# ---------------------------------------------------------------------------
# engine-model inflater: tinfl with the reader's 0x400 wrapping ring
# ---------------------------------------------------------------------------

LENGTH_BASE = (3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258)
LENGTH_EXTRA = (0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0)
DIST_BASE = (1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
             257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
             12289, 16385, 24577)
DIST_EXTRA = (0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
              7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13)
CODE_LENGTH_ORDER = (16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15)
FAST_BITS = 10
FAST_SIZE = 1 << FAST_BITS


class HuffmanTable:
    """tinfl_huff_table: a 10-bit reversed-code lookup plus a map for longer codes.

    The build mirrors tinfl (0x5aeb00 / host_vita_archive_miniz_native.c):
    canonical codes from the code sizes, accepted only when the code is
    complete or has at most one used symbol (state 35 otherwise)."""

    __slots__ = ("fast", "long")

    def __init__(self, code_sizes: Sequence[int]) -> None:
        counts = [0] * 17
        for size in code_sizes:
            counts[size] += 1
        counts[0] = 0
        used = 0
        total = 0
        next_code = [0] * 17
        for bits in range(1, 16):
            used += counts[bits]
            total = (total + counts[bits]) << 1
            next_code[bits + 1] = total
        if total != 65536 and used > 1:
            raise RepackError("Huffman code is neither complete nor a single symbol")
        fast = [0] * FAST_SIZE
        long: dict[tuple[int, int], int] = {}
        for symbol, size in enumerate(code_sizes):
            if not size:
                continue
            code = next_code[size]
            next_code[size] += 1
            reversed_code = 0
            for _ in range(size):
                reversed_code = (reversed_code << 1) | (code & 1)
                code >>= 1
            if size <= FAST_BITS:
                value = (size << 9) | symbol
                for slot in range(reversed_code, FAST_SIZE, 1 << size):
                    fast[slot] = value
            else:
                fast[reversed_code & (FAST_SIZE - 1)] = -1
                long[(size, reversed_code)] = symbol
        self.fast = fast
        self.long = long


_FIXED_TABLES: tuple[HuffmanTable, HuffmanTable] | None = None


def fixed_tables() -> tuple[HuffmanTable, HuffmanTable]:
    global _FIXED_TABLES
    if _FIXED_TABLES is None:
        literal = [8] * 144 + [9] * 112 + [7] * 24 + [8] * 8
        _FIXED_TABLES = (HuffmanTable(literal), HuffmanTable([5] * 32))
    return _FIXED_TABLES


@dataclass
class BlockCensus:
    stored: int = 0
    static: int = 0
    dynamic: int = 0

    def add(self, other: "BlockCensus") -> None:
        self.stored += other.stored
        self.static += other.static
        self.dynamic += other.dynamic

    @property
    def total(self) -> int:
        return self.stored + self.static + self.dynamic


class ModelInflater:
    """An independent raw inflater that models the engine's reader per entry.

    The model keeps the 0x400-byte ring across pieces, requires every
    non-final piece to fill it exactly and end byte-aligned with all input
    consumed, rejects distances the ring cannot serve (> 0x400, or into a
    previous piece that was not full), rejects the reserved symbols tinfl
    mis-handles (lengths 286/287, distances 30/31) and counts block types.
    It shares no code with zlib or with the encoder."""

    def __init__(self) -> None:
        self.previous: bytes | None = None
        self.produced = 0
        self.finished = False
        self.census = BlockCensus()
        self._data = b""
        self._pos = 0
        self._end = 0
        self._bits = 0
        self._nbits = 0

    # -- bit reader ---------------------------------------------------------
    def _fill(self, count: int) -> None:
        while self._nbits < count and self._pos < self._end:
            self._bits |= self._data[self._pos] << self._nbits
            self._pos += 1
            self._nbits += 8

    def _bits_exact(self, count: int) -> int:
        if count == 0:
            return 0
        self._fill(count)
        if self._nbits < count:
            raise RepackError("piece ends inside a symbol (%d bits short)" % (count - self._nbits))
        value = self._bits & ((1 << count) - 1)
        self._bits >>= count
        self._nbits -= count
        return value

    def _symbol(self, table: HuffmanTable) -> int:
        if self._nbits < 15:
            self._fill(15)
        entry = table.fast[self._bits & (FAST_SIZE - 1)]
        if entry >= 0:
            size = entry >> 9
            if size == 0 or size > self._nbits:
                raise RepackError("piece ends inside a Huffman code")
            self._bits >>= size
            self._nbits -= size
            return entry & 511
        for size in range(FAST_BITS + 1, 16):
            if size > self._nbits:
                break
            symbol = table.long.get((size, self._bits & ((1 << size) - 1)))
            if symbol is not None:
                self._bits >>= size
                self._nbits -= size
                return symbol
        raise RepackError("undecodable Huffman code")

    # -- ring output --------------------------------------------------------
    def _copy(self, out: bytearray, distance: int, length: int) -> None:
        pos = len(out)
        if distance > BLOCK:
            raise RepackError("match distance %d exceeds the 0x400 ring" % distance)
        if pos + length > BLOCK:
            raise RepackError("piece produces more than 0x400 bytes")
        if distance <= pos:
            if distance >= length:
                out += out[pos - distance:pos - distance + length]
            else:
                pattern = bytes(out[pos - distance:pos])
                out += (pattern * (length // distance + 1))[:length]
            return
        previous = self.previous
        if previous is None:
            raise RepackError("match distance %d reaches before the entry start" % distance)
        if len(previous) != BLOCK:
            raise RepackError("match reaches into a previous piece that did not fill the ring")
        for i in range(length):
            source = pos + i - distance
            out.append(previous[BLOCK + source] if source < 0 else out[source])

    # -- blocks -------------------------------------------------------------
    def _stored_block(self, out: bytearray) -> None:
        self._bits >>= self._nbits & 7
        self._nbits -= self._nbits & 7
        header = bytearray()
        for _ in range(4):
            if self._nbits:
                header.append(self._bits_exact(8))
            else:
                if self._pos >= self._end:
                    raise RepackError("piece ends inside a stored block header")
                header.append(self._data[self._pos])
                self._pos += 1
        length = header[0] | (header[1] << 8)
        if length != (0xFFFF ^ (header[2] | (header[3] << 8))):
            raise RepackError("stored block LEN/NLEN mismatch")
        if len(out) + length > BLOCK:
            raise RepackError("stored block overflows the 0x400 ring")
        while length and self._nbits:
            out.append(self._bits_exact(8))
            length -= 1
        if self._pos + length > self._end:
            raise RepackError("piece ends inside stored block data")
        out += self._data[self._pos:self._pos + length]
        self._pos += length

    def _huffman_block(self, out: bytearray, literal: HuffmanTable, distance_table: HuffmanTable) -> None:
        symbol = self._symbol
        while True:
            sym = symbol(literal)
            if sym < 256:
                if len(out) >= BLOCK:
                    raise RepackError("piece produces more than 0x400 bytes")
                out.append(sym)
                continue
            if sym == 256:
                return
            if sym >= 286:
                raise RepackError("reserved length symbol %d" % sym)
            length = LENGTH_BASE[sym - 257] + self._bits_exact(LENGTH_EXTRA[sym - 257])
            dsym = symbol(distance_table)
            if dsym >= 30:
                raise RepackError("reserved distance symbol %d" % dsym)
            distance = DIST_BASE[dsym] + self._bits_exact(DIST_EXTRA[dsym])
            self._copy(out, distance, length)

    def _dynamic_tables(self) -> tuple[HuffmanTable, HuffmanTable]:
        literal_count = self._bits_exact(5) + 257
        distance_count = self._bits_exact(5) + 1
        length_count = self._bits_exact(4) + 4
        code_length_sizes = [0] * 19
        for i in range(length_count):
            code_length_sizes[CODE_LENGTH_ORDER[i]] = self._bits_exact(3)
        code_length_table = HuffmanTable(code_length_sizes)
        sizes: list[int] = []
        total = literal_count + distance_count
        while len(sizes) < total:
            sym = self._symbol(code_length_table)
            if sym < 16:
                sizes.append(sym)
            elif sym == 16:
                if not sizes:
                    raise RepackError("code length repeat without a previous length")
                sizes.extend([sizes[-1]] * (3 + self._bits_exact(2)))
            elif sym == 17:
                sizes.extend([0] * (3 + self._bits_exact(3)))
            else:
                sizes.extend([0] * (11 + self._bits_exact(7)))
        if len(sizes) != total:
            raise RepackError("code length run overshoots the alphabet")
        return HuffmanTable(sizes[:literal_count]), HuffmanTable(sizes[literal_count:])

    # -- pieces -------------------------------------------------------------
    def decode_piece(self, payload: bytes, final: bool, expected_size: int) -> bytes:
        """Decode one piece; ``expected_size`` is what the reader must see in +0xc20."""
        if self.finished:
            raise RepackError("piece after the final deflate block")
        if not payload:
            raise RepackError("empty piece")
        self._data = payload
        self._pos = 0
        self._end = len(payload)
        if self._nbits:
            raise RepackError("previous piece left %d bits in the bit buffer" % self._nbits)
        out = bytearray()
        saw_final = False
        while True:
            if self._pos >= self._end and self._nbits < 3:
                break
            bfinal = self._bits_exact(1)
            btype = self._bits_exact(2)
            if btype == 0:
                self.census.stored += 1
                self._stored_block(out)
            elif btype == 1:
                self.census.static += 1
                literal, distance = fixed_tables()
                self._huffman_block(out, literal, distance)
            elif btype == 2:
                self.census.dynamic += 1
                literal, distance = self._dynamic_tables()
                self._huffman_block(out, literal, distance)
            else:
                raise RepackError("reserved block type 3")
            if bfinal:
                saw_final = True
                break
        if self._pos != self._end:
            raise RepackError("piece has %d unread bytes" % (self._end - self._pos))
        if final:
            if not saw_final:
                raise RepackError("final piece has no BFINAL block")
            if self._nbits >= 8:
                raise RepackError("final piece has a trailing byte")
            self._bits = 0
            self._nbits = 0
            self.finished = True
        else:
            if saw_final:
                raise RepackError("non-final piece carries the BFINAL block")
            if self._nbits:
                raise RepackError("non-final piece does not end on a byte boundary")
            if len(out) != BLOCK:
                raise RepackError("non-final piece produced %d bytes, not 0x400" % len(out))
        if len(out) != expected_size:
            raise RepackError("piece produced %d bytes, expected %d" % (len(out), expected_size))
        result = bytes(out)
        self.previous = result
        self.produced += len(result)
        return result


def model_decode_pieces(pieces: Sequence[Piece], expected: int, terminal: bool) -> tuple[bytes, BlockCensus]:
    """Decode a deflate prefix (or a whole pure-deflate entry) with the model."""
    model = ModelInflater()
    out = bytearray()
    remaining = expected
    for number, (payload, final) in enumerate(pieces):
        if final != (terminal and number == len(pieces) - 1):
            raise RepackError("piece %d has an unexpected final flag" % number)
        size = remaining if final else BLOCK
        out += model.decode_piece(payload, final, size)
        remaining -= size
    if remaining != 0 or model.finished != terminal:
        raise RepackError("deflate pieces do not cover %d bytes" % expected)
    return bytes(out), model.census


# ---------------------------------------------------------------------------
# encoder
# ---------------------------------------------------------------------------

class Compressor(Protocol):
    """The zlib.compressobj surface encode_deflate_pieces drives."""

    def compress(self, data: bytes, /) -> bytes: ...

    def flush(self, mode: int, /) -> bytes: ...


def _compressor(level: int, strategy: int, wbits: int = -WINDOW_BITS) -> Compressor:
    return zlib.compressobj(level, zlib.DEFLATED, wbits, 9, strategy)


def _alternative_pieces(chunk: bytes, final: bool, level: int) -> Iterator[bytes]:
    """Byte-aligned, self-contained encodings of one chunk that may replace a
    piece whose length the format forbids.  A fresh compressor only references
    the chunk itself, so the piece stays valid inside the entry's stream."""
    flush = zlib.Z_FINISH if final else zlib.Z_SYNC_FLUSH
    # Static first (other levels change the match choice, level 0 is a plain
    # stored block of 0x400 + 10 bytes); dynamic Huffman only as a last resort.
    for strategy in (zlib.Z_FIXED, zlib.Z_DEFAULT_STRATEGY):
        for alt_level in (6, 1, 0, level):
            c = _compressor(alt_level, strategy)
            yield c.compress(chunk) + c.flush(flush)


@dataclass
class EncodeResult:
    pieces: list[Piece]
    static_pieces: int = 0
    dynamic_pieces: int = 0
    fallback_pieces: int = 0


def encode_deflate_pieces(
    data: bytes,
    terminal: bool = True,
    level: int = 9,
    stream: bool = True,
    static_budget: int | None = None,
    compressor_factory: Callable[[int, int], Compressor] | None = None,
) -> EncodeResult:
    """Re-encode ``data`` as 0x400-byte pieces.

    ``terminal``: the last piece is final and ends the stream; otherwise every
    piece is a non-final sync-flushed block (a deflate prefix before the
    stored latch; ``data`` must then be a multiple of 0x400).
    ``stream``: one compressor per entry with Z_SYNC_FLUSH at every piece
    boundary (the reader keeps one tinfl state per entry, so matches may
    reach into the previous full piece; the 10-bit window keeps them inside
    the ring).  ``stream=False`` starts a fresh compressor per piece.
    ``static_budget``: run a dynamic-Huffman compressor in lockstep and keep
    the static piece only when it is at most that many bytes larger
    (None = static everywhere)."""
    factory = compressor_factory or _compressor
    n = (len(data) + BLOCK - 1) // BLOCK
    result = EncodeResult([])
    if n == 0:
        return result
    if not terminal and len(data) % BLOCK:
        raise RepackError("a deflate prefix must be a multiple of 0x400 bytes")
    fixed = factory(level, zlib.Z_FIXED)
    dynamic = factory(level, zlib.Z_DEFAULT_STRATEGY) if static_budget is not None else None
    for i in range(n):
        chunk = data[i * BLOCK:(i + 1) * BLOCK]
        final = terminal and i == n - 1
        flush = zlib.Z_FINISH if final else zlib.Z_SYNC_FLUSH
        if not stream:
            fixed = factory(level, zlib.Z_FIXED)
            if dynamic is not None:
                dynamic = factory(level, zlib.Z_DEFAULT_STRATEGY)
        piece = fixed.compress(chunk) + fixed.flush(flush)
        kind = "static"
        if dynamic is not None:
            assert static_budget is not None  # dynamic exists only with a budget
            alternative = dynamic.compress(chunk) + dynamic.flush(flush)
            if len(piece) - len(alternative) > static_budget and piece_ok(alternative, final):
                piece = alternative
                kind = "dynamic"
        if not piece_ok(piece, final):
            # The compressor's own output for the chunk is discarded but the
            # chunk stays in its window, so the following pieces remain valid.
            for candidate in _alternative_pieces(chunk, final, level):
                if piece_ok(candidate, final):
                    piece = candidate
                    kind = "fallback"
                    break
            else:
                raise RepackError("no admissible encoding for a %d-byte chunk" % len(chunk))
        if kind == "static":
            result.static_pieces += 1
        elif kind == "dynamic":
            result.dynamic_pieces += 1
        else:
            result.fallback_pieces += 1
        result.pieces.append((piece, final))
    return result


def encode_stock_like_entry(data: bytes, hash_b: int, level: int = 9) -> list[Piece]:
    """A writer with the stock packer's shape: one dynamic-Huffman full-flushed
    piece per 0x400 chunk while the chunk compresses below 0x400, then stored
    (keystream XOR) pieces for the rest of the entry.  Used by the tests to
    build synthetic archives that look like the game's."""
    n = (len(data) + BLOCK - 1) // BLOCK
    if n == 0:
        return []
    pieces: list[Piece] = []
    keystream: IsaacKeystream | None = None
    for i in range(n):
        chunk = data[i * BLOCK:(i + 1) * BLOCK]
        final = i == n - 1
        if keystream is None:
            c = _compressor(level, zlib.Z_DEFAULT_STRATEGY, -15)
            piece = c.compress(chunk) + c.flush(zlib.Z_FINISH if final else zlib.Z_FULL_FLUSH)
            if final or len(piece) < BLOCK:
                pieces.append((piece, final))
                continue
            keystream = IsaacKeystream(hash_b)
        pieces.append((keystream.apply(chunk), final))
    return pieces


# ---------------------------------------------------------------------------
# writer
# ---------------------------------------------------------------------------

def write_archive(path: str | os.PathLike[str], mode: int, records: Sequence[tuple[Entry, bytes]]) -> int:
    """Write header, payloads (index order, deduplicated by original offset) and
    the index.  Returns the file size."""
    if len(records) > 0xFFFF:
        raise RepackError("too many entries (%d)" % len(records))
    with open(path, "wb") as stream:
        stream.write(HEADER.pack(MAGIC, mode, 0, len(records)))
        pos = HEADER.size
        offsets: dict[tuple[int, int, int], int] = {}
        rows: list[tuple[int, int, int, int, int]] = []
        for entry, payload in records:
            key = payload_key(entry)
            if key is not None and key in offsets:
                new_offset = offsets[key]
            else:
                new_offset = pos
                if key is not None:
                    offsets[key] = pos
                stream.write(payload)
                pos += len(payload)
            rows.append((entry.hash_a, entry.hash_b, new_offset, entry.size, entry.checksum))
        index_offset = pos
        for row in rows:
            stream.write(INDEX_ENTRY.pack(*row))
        stream.seek(8)
        stream.write(struct.pack("<I", index_offset))
        return index_offset + INDEX_ENTRY.size * len(rows)


# ---------------------------------------------------------------------------
# repack
# ---------------------------------------------------------------------------

@dataclass
class Stats:
    source: str = ""
    output: str = ""
    mode: int = 0
    entries: int = 0
    deflate_entries: int = 0
    mixed_entries: int = 0
    stored_entries: int = 0
    passthrough_entries: int = 0
    empty_entries: int = 0
    deflate_pieces: int = 0
    stored_pieces: int = 0
    original_first_block: dict[str, int] = field(default_factory=dict)
    original_census: dict[str, int] | None = None
    repacked_census: dict[str, int] | None = None
    static_pieces: int = 0
    dynamic_pieces_kept: int = 0
    fallback_pieces: int = 0
    dynamic_to_fixed: int = 0
    deflate_decoded_bytes: int = 0
    decoded_bytes: int = 0
    original_size: int = 0
    repacked_size: int = 0
    original_deflate_payload: int = 0
    repacked_deflate_payload: int = 0
    verify: str = ""
    verified_entries: int = 0
    model_verified_entries: int = 0
    checksums_verified: int = 0
    stored_verified_by_bytes: int = 0
    stored_verified_by_decode: int = 0
    seconds: float = 0.0
    warnings: list[str] = field(default_factory=list)

    @property
    def size_delta(self) -> int:
        return self.repacked_size - self.original_size

    def table_builds_avoided_per_mb(self) -> float:
        if not self.deflate_decoded_bytes:
            return 0.0
        return self.dynamic_to_fixed / (self.deflate_decoded_bytes / 1048576.0)

    def to_json(self) -> dict:
        d = dict(self.__dict__)
        d["size_delta"] = self.size_delta
        d["table_builds_avoided_per_mb"] = self.table_builds_avoided_per_mb()
        return d

    def summary(self) -> str:
        lines = [
            "%s -> %s" % (self.source, self.output),
            "  entries %d: %d deflate + %d mixed re-encoded, %d stored copied, %d passthrough, %d empty"
            % (self.entries, self.deflate_entries, self.mixed_entries, self.stored_entries,
               self.passthrough_entries, self.empty_entries),
            "  deflate pieces %d, stored pieces %d (first-block census of the original deflate pieces: %s)"
            % (self.deflate_pieces, self.stored_pieces,
               ", ".join("%s=%d" % kv for kv in sorted(self.original_first_block.items())) or "-"),
            "  repacked pieces: %d static, %d dynamic kept, %d fallback; dynamic->fixed %d"
            % (self.static_pieces, self.dynamic_pieces_kept, self.fallback_pieces, self.dynamic_to_fixed),
            "  size %d -> %d bytes (%+d, %+.3f%%); deflate payload %d -> %d (%+.2f%%)"
            % (self.original_size, self.repacked_size, self.size_delta,
               100.0 * self.size_delta / self.original_size if self.original_size else 0.0,
               self.original_deflate_payload, self.repacked_deflate_payload,
               100.0 * (self.repacked_deflate_payload - self.original_deflate_payload) / self.original_deflate_payload
               if self.original_deflate_payload else 0.0),
            "  deflate decoded %.2f MB; dynamic tables avoided per decoded MB: %.1f"
            % (self.deflate_decoded_bytes / 1048576.0, self.table_builds_avoided_per_mb()),
        ]
        if self.original_census is not None:
            lines.append("  original block census (model): %s" % self.original_census)
        if self.repacked_census is not None:
            lines.append("  repacked block census (model): %s" % self.repacked_census)
        lines.append(
            "  verify=%s: %d entries byte-compared, %d model-decoded, %d checksums recomputed, "
            "stored entries %d by bytes / %d by keystream decode, %.1f s"
            % (self.verify, self.verified_entries, self.model_verified_entries,
               self.checksums_verified, self.stored_verified_by_bytes,
               self.stored_verified_by_decode, self.seconds)
        )
        for warning in self.warnings:
            lines.append("  WARNING: " + warning)
        return "\n".join(lines)


def _census_key(bfinal: int, btype: int) -> str:
    return ("final-" if bfinal else "") + {0: "stored", 1: "static", 2: "dynamic", 3: "reserved"}[btype]


def repack_archive(
    source: str | os.PathLike[str],
    output: str | os.PathLike[str],
    level: int = 9,
    stream: bool = True,
    static_budget: int | None = None,
    verify: str = "model",
    verify_stored: bool = False,
    allow_receipt_archive: bool = False,
    progress: Callable[[str], None] | None = None,
) -> Stats:
    """Repack one archive; raises RepackError on any invariant failure.  The
    output file is removed again if verification fails."""
    if verify not in VERIFY_LEVELS:
        raise RepackError("unknown verify level %r" % verify)
    source_path = Path(source)
    output_path = Path(output)
    if output_path.exists() and os.path.samefile(source_path, output_path):
        raise RepackError("refusing to repack in place: %s" % source_path)
    started = time.time()
    stats = Stats(source=str(source_path), output=str(output_path))
    with Archive(source_path) as archive:
        stats.mode = archive.mode
        stats.entries = archive.count
        stats.original_size = archive.size
        if archive.mode != MODE_MINIZ:
            raise RepackError(
                "%s is a mode-%d archive (only mode 2 / MiniZ archives have deflate pieces)"
                % (source_path, archive.mode)
            )
        if source_path.name.lower() in RECEIPT_ARCHIVES and not allow_receipt_archive:
            raise RepackError(
                "%s is validated through the Vita's stock-pinned receipt; a repacked copy "
                "would be fully re-validated on every boot.  Pass --allow-receipt-archive "
                "to repack it anyway." % source_path.name
            )
        limits = archive.payload_limits()
        records: list[tuple[Entry, bytes]] = []
        done: dict[tuple[int, int, int], bytes] = {}
        first_block: dict[str, int] = {}
        for entry in archive.entries:
            key = payload_key(entry)
            if key is not None and key in done:
                records.append((entry, done[key]))
                continue
            pieces = entry_pieces(archive, entry, limits[entry.offset])
            shape = shape_of(pieces)
            stats.decoded_bytes += entry.size
            if shape.kind == "empty":
                stats.empty_entries += 1
                payload = b""
            elif shape.kind == "stored":
                stats.stored_entries += 1
                stats.stored_pieces += len(pieces)
                payload = pack_pieces(pieces)
            else:
                try:
                    _, prefix = decode_entry(shape, entry, stored_too=False)
                    assert prefix is not None
                except RepackError as exc:
                    stats.passthrough_entries += 1
                    stats.warnings.append("entry %d (%s) copied unchanged: %s" % (entry.index, entry.tag, exc))
                    payload = pack_pieces(pieces)
                else:
                    if shape.kind == "deflate":
                        stats.deflate_entries += 1
                    else:
                        stats.mixed_entries += 1
                        stats.stored_pieces += len(shape.stored)
                    stats.deflate_pieces += len(shape.deflate)
                    stats.deflate_decoded_bytes += len(prefix.data)
                    stats.original_deflate_payload += sum(len(p) for p, _ in shape.deflate)
                    for bfinal, btype in prefix.first_block_types:
                        census = _census_key(bfinal, btype)
                        first_block[census] = first_block.get(census, 0) + 1
                        if btype == 2:
                            stats.dynamic_to_fixed += 1
                    encoded = encode_deflate_pieces(prefix.data, shape.terminal, level, stream, static_budget)
                    stats.static_pieces += encoded.static_pieces
                    stats.dynamic_pieces_kept += encoded.dynamic_pieces
                    stats.fallback_pieces += encoded.fallback_pieces
                    stats.repacked_deflate_payload += sum(len(p) for p, _ in encoded.pieces)
                    payload = pack_pieces(encoded.pieces) + pack_pieces(shape.stored)
            if key is not None:
                done[key] = payload
            records.append((entry, payload))
            if progress and entry.index % 500 == 0:
                progress("  encoded %d/%d entries" % (entry.index + 1, archive.count))
        stats.original_first_block = first_block
        stats.dynamic_to_fixed -= stats.dynamic_pieces_kept
        output_path.parent.mkdir(parents=True, exist_ok=True)
        stats.repacked_size = write_archive(output_path, archive.mode, records)
    if verify != "off":
        try:
            verify_repack(source_path, output_path, stats, verify, verify_stored, progress)
        except RepackError:
            try:
                output_path.unlink()
            except OSError:
                pass
            raise
    stats.verify = verify
    stats.seconds = time.time() - started
    return stats


def verify_repack(
    source: str | os.PathLike[str],
    output: str | os.PathLike[str],
    stats: Stats | None = None,
    level: str = "model",
    verify_stored: bool = False,
    progress: Callable[[str], None] | None = None,
) -> Stats:
    """Independently decode both archives entry by entry and compare.

    zlib  : both archives through zlib with the reader's cadence checks, byte
            compare, mount checksum recomputed from the decoded bytes of every
            entry whose bytes changed (deflate and mixed entries).
    model : plus the engine-model inflater over every repacked deflate piece.
    full  : plus the engine-model inflater over the original (exact census).
    Pure stored entries are compared as raw bytes; ``verify_stored`` also
    decodes them through the keystream and checks their mount checksums."""
    if stats is None:
        stats = Stats(source=str(source), output=str(output))
    if level not in VERIFY_LEVELS or level == "off":
        raise RepackError("verify level must be zlib, model or full")
    with Archive(source) as a, Archive(output) as b:
        # repack_archive fills these before it verifies; a stand-alone `verify`
        # reports the file sizes it compared instead of "0 -> 0".
        if not stats.original_size:
            stats.original_size = a.size
        if not stats.repacked_size:
            stats.repacked_size = b.size
        if a.count != b.count or a.mode != b.mode:
            raise RepackError("entry count or mode changed (%d/%d vs %d/%d)" % (a.count, a.mode, b.count, b.mode))
        if b.index_offset + INDEX_ENTRY.size * b.count != b.size:
            raise RepackError("repacked index does not close at EOF")
        a_limits = a.payload_limits()
        b_limits = b.payload_limits()
        original_census = BlockCensus()
        repacked_census = BlockCensus()
        highest_b_offset = HEADER.size
        shared_b: dict[tuple[int, int, int], int] = {}
        for x, y in zip(a.entries, b.entries):
            if (x.hash_a, x.hash_b, x.size, x.checksum) != (y.hash_a, y.hash_b, y.size, y.checksum):
                raise RepackError("entry %d: key/size/checksum changed" % x.index)
            # Payloads are written in index order; a row may only point back
            # to a payload it shares (same original offset, size, checksum),
            # and that payload was already verified with its first row.
            key = payload_key(x)
            if key is not None and key in shared_b:
                if y.offset != shared_b[key]:
                    raise RepackError("entry %d: shared payload was not kept shared" % x.index)
                stats.verified_entries += 1
                continue
            if y.offset < highest_b_offset:
                raise RepackError("entry %d: offsets are not monotonic in the repacked index" % x.index)
            if key is not None:
                shared_b[key] = y.offset
            highest_b_offset = max(highest_b_offset, y.offset)
            pa = entry_pieces(a, x, a_limits[x.offset])
            pb = entry_pieces(b, y, b_limits[y.offset])
            sa = shape_of(pa)
            sb = shape_of(pb)
            if sa.kind != sb.kind or len(sa.deflate) != len(sb.deflate):
                raise RepackError("entry %d: shape changed (%s/%d -> %s/%d)"
                                  % (x.index, sa.kind, len(sa.deflate), sb.kind, len(sb.deflate)))
            if pack_pieces(sa.stored) != pack_pieces(sb.stored):
                raise RepackError("entry %d: stored pieces changed" % x.index)
            if sa.kind == "empty":
                stats.verified_entries += 1
                continue
            if sa.kind == "stored":
                stats.stored_verified_by_bytes += 1
                if verify_stored:
                    data = decode_stored_pieces(pb, y.hash_b)[:y.size]
                    if len(data) != y.size or mount_checksum(data) != y.checksum:
                        raise RepackError("entry %d: stored entry mount checksum mismatch" % x.index)
                    stats.stored_verified_by_decode += 1
                    stats.checksums_verified += 1
                stats.verified_entries += 1
                continue
            try:
                original, original_prefix = decode_entry(sa, x, stored_too=True)
            except RepackError:
                # Copied unchanged because it did not meet the strict cadence;
                # it must still be byte-identical.
                if pack_pieces(pa) != pack_pieces(pb):
                    raise RepackError("entry %d: passthrough entry bytes changed" % x.index)
                stats.verified_entries += 1
                continue
            assert original is not None and original_prefix is not None
            repacked, repacked_prefix = decode_entry(sb, y, stored_too=True)
            assert repacked is not None and repacked_prefix is not None
            if original != repacked:
                raise RepackError("entry %d (%s): decoded bytes differ" % (x.index, x.tag))
            if mount_checksum(repacked) != y.checksum:
                raise RepackError("entry %d (%s): mount checksum mismatch" % (x.index, x.tag))
            stats.checksums_verified += 1
            expected = len(original_prefix.data)
            if level in ("model", "full"):
                model_data, census = model_decode_pieces(sb.deflate, expected, sb.terminal)
                if model_data != original_prefix.data:
                    raise RepackError("entry %d (%s): engine-model decode differs" % (x.index, x.tag))
                repacked_census.add(census)
                stats.model_verified_entries += 1
            if level == "full":
                model_data, census = model_decode_pieces(sa.deflate, expected, sa.terminal)
                if model_data != original_prefix.data:
                    raise RepackError("entry %d (%s): engine-model decode of the original differs" % (x.index, x.tag))
                original_census.add(census)
            stats.verified_entries += 1
            if progress and x.index % 500 == 0:
                progress("  verified %d/%d entries" % (x.index + 1, a.count))
        if level in ("model", "full"):
            stats.repacked_census = repacked_census.__dict__.copy()
        if level == "full":
            stats.original_census = original_census.__dict__.copy()
    stats.verify = level
    return stats


# ---------------------------------------------------------------------------
# info
# ---------------------------------------------------------------------------

def archive_info(path: str | os.PathLike[str]) -> dict:
    with Archive(path) as archive:
        info = {
            "path": archive.path,
            "size": archive.size,
            "mode": archive.mode,
            "entries": archive.count,
            "index_offset": archive.index_offset,
            "index_sha256": hashlib.sha256(
                bytes(archive.buf[archive.index_offset:archive.size])
            ).hexdigest(),
            "decoded_bytes": sum(e.size for e in archive.entries),
        }
        if archive.mode == MODE_MINIZ:
            limits = archive.payload_limits()
            kinds: dict[str, int] = {}
            pieces = deflate_pieces = stored_pieces = deflate_payload = stored_payload = 0
            first_block: dict[str, int] = {}
            for entry in archive.entries:
                ps = entry_pieces(archive, entry, limits[entry.offset])
                shape = shape_of(ps)
                kinds[shape.kind] = kinds.get(shape.kind, 0) + 1
                pieces += len(ps)
                deflate_pieces += len(shape.deflate)
                stored_pieces += len(shape.stored)
                deflate_payload += sum(len(p) for p, _ in shape.deflate)
                stored_payload += sum(len(p) for p, _ in shape.stored)
                for p, _ in shape.deflate:
                    key = _census_key(p[0] & 1, (p[0] >> 1) & 3)
                    first_block[key] = first_block.get(key, 0) + 1
            info.update({
                "entry_kinds": kinds,
                "pieces": pieces,
                "deflate_pieces": deflate_pieces,
                "stored_pieces": stored_pieces,
                "deflate_payload": deflate_payload,
                "stored_payload": stored_payload,
                "first_block_census": first_block,
            })
        return info


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _print(message: str) -> None:
    print(message, flush=True)


def cmd_repack(args: argparse.Namespace) -> int:
    stats = repack_archive(
        args.source, args.output, level=args.level, stream=not args.per_piece,
        static_budget=args.static_budget, verify=args.verify,
        verify_stored=args.verify_stored,
        allow_receipt_archive=args.allow_receipt_archive,
        progress=_print if args.progress else None,
    )
    print(stats.summary())
    if args.stats_json:
        Path(args.stats_json).write_text(json.dumps(stats.to_json(), indent=2, sort_keys=True), encoding="utf-8")
    return 0


def cmd_repack_dir(args: argparse.Namespace) -> int:
    source_dir = Path(args.source_dir)
    output_dir = Path(args.output_dir)
    if not source_dir.is_dir():
        raise RepackError("%s is not a directory" % source_dir)
    if output_dir.exists() and os.path.samefile(source_dir, output_dir):
        raise RepackError("refusing to repack a directory onto itself")
    output_dir.mkdir(parents=True, exist_ok=True)
    all_stats: list[dict] = []
    failures = 0
    files = sorted(path for path in source_dir.iterdir() if path.is_file())
    changed: set[str] = set()          # lower-cased names of archives whose bytes differ now
    for path in files:
        if path.suffix.lower() != ".a":
            continue
        target = output_dir / path.name
        try:
            with Archive(path) as archive:
                mode = archive.mode
        except RepackError as exc:
            print("%s: %s -- copied unchanged" % (path.name, exc))
            shutil.copyfile(path, target)
            continue
        if mode != MODE_MINIZ or (path.name.lower() in RECEIPT_ARCHIVES and not args.allow_receipt_archive):
            reason = "mode %d" % mode if mode != MODE_MINIZ else "receipt-validated archive"
            print("%s: %s -- copied unchanged" % (path.name, reason))
            shutil.copyfile(path, target)
            continue
        try:
            stats = repack_archive(
                path, target, level=args.level, stream=not args.per_piece,
                static_budget=args.static_budget, verify=args.verify,
                verify_stored=args.verify_stored, allow_receipt_archive=True,
                progress=_print if args.progress else None,
            )
        except RepackError as exc:
            failures += 1
            print("%s: FAILED: %s -- copied unchanged" % (path.name, exc))
            shutil.copyfile(path, target)
            continue
        print(stats.summary())
        all_stats.append(stats.to_json())
        if not filecmp.cmp(path, target, shallow=False):
            changed.add(path.name.lower())
    # Everything else is copied, except a validation sidecar that pinned the
    # bytes of an archive this run changed: the Vita would refuse it (size and
    # index digest differ) and validate the archive the slow way regardless,
    # and a stale sidecar next to a fresh archive misleads.  Repack before
    # halving, or write a new sidecar with `isaac_vita_halve_sfx.py receipt`.
    for path in files:
        if path.suffix.lower() == ".a":
            continue
        name = path.name.lower()
        if name.endswith(SIDECAR_SUFFIX) and name[:-len(SIDECAR_SUFFIX)] in changed:
            print("%s: not copied -- validation sidecar of an archive this run changed; repack before "
                  "isaac_vita_halve_sfx.py, or write a new one with its `receipt` command" % path.name)
            continue
        shutil.copyfile(path, output_dir / path.name)
    if args.stats_json:
        Path(args.stats_json).write_text(json.dumps(all_stats, indent=2, sort_keys=True), encoding="utf-8")
    return 1 if failures else 0


def cmd_verify(args: argparse.Namespace) -> int:
    stats = verify_repack(args.source, args.output, None, args.verify, args.verify_stored,
                          _print if args.progress else None)
    print(stats.summary())
    return 0


def cmd_info(args: argparse.Namespace) -> int:
    for path in args.archive:
        print(json.dumps(archive_info(path), indent=2, sort_keys=True))
    return 0


def _add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--level", type=int, default=9, choices=range(1, 10), metavar="1..9",
                        help="zlib compression level (default 9)")
    parser.add_argument("--per-piece", action="store_true",
                        help="fresh compressor per 0x400 piece instead of one sync-flushed stream per entry")
    parser.add_argument("--static-budget", type=int, default=None, metavar="BYTES",
                        help="keep a piece dynamic when static Huffman would grow it by more than BYTES "
                             "(default: static everywhere)")
    parser.add_argument("--verify", choices=VERIFY_LEVELS, default="model",
                        help="off | zlib (both archives, cadence + checksums) | model (plus the engine-model "
                             "inflater over the repacked archive; default) | full (model over both)")
    parser.add_argument("--verify-stored", action="store_true",
                        help="also decode pure stored (ISAAC-XOR) entries and check their mount checksums")
    parser.add_argument("--allow-receipt-archive", action="store_true",
                        help="repack afterbirth.a although the Vita's receipt then never trusts it")
    parser.add_argument("--progress", action="store_true", help="print progress lines")
    parser.add_argument("--stats-json", metavar="FILE", help="write the statistics as JSON")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="isaac_vita_repack.py",
        description="Re-encode the deflate pieces of KAGE ARCH000 v2 archives as static Huffman "
                    "for faster decoding on the Vita.  Always writes a new file.",
    )
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("repack", help="repack one archive into a new file")
    p.add_argument("source")
    p.add_argument("output")
    _add_common(p)
    p.set_defaults(fn=cmd_repack)
    p = sub.add_parser("repack-dir", help="repack every mode-2 .a in a directory; everything else is copied")
    p.add_argument("source_dir")
    p.add_argument("output_dir")
    _add_common(p)
    p.set_defaults(fn=cmd_repack_dir)
    p = sub.add_parser("verify", help="compare an original and a repacked archive entry by entry")
    p.add_argument("source")
    p.add_argument("output")
    p.add_argument("--verify", choices=VERIFY_LEVELS[1:], default="model")
    p.add_argument("--verify-stored", action="store_true")
    p.add_argument("--progress", action="store_true")
    p.set_defaults(fn=cmd_verify)
    p = sub.add_parser("info", help="print container facts and the piece census of archives")
    p.add_argument("archive", nargs="+")
    p.set_defaults(fn=cmd_info)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.fn(args)
    except RepackError as exc:
        print("isaac_vita_repack: error: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
