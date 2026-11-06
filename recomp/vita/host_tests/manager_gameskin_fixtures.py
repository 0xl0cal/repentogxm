#!/usr/bin/env python3
"""Fixtures for the manager game-skin host tests.

Writes into OUTPUT_DIR (created if needed):

  * ARCH000 archives built at test time with the repository's own stdlib-only
    encoder, tools/isaac_vita_repack.py (the helper pattern of
    tools/test_isaac_vita_repack.py build_stock_like_archive): pure deflate,
    streamed deflate with matches into the previous piece, pure stored, mixed,
    a deflate prefix followed by stored pieces, alias rows, size-0 rows, both
    path prefixes; rows the reader must refuse (piece > 0x7ff, stored piece
    > 0x400, a piece crossing the entry limit, reserved deflate symbols,
    garbage, wrong sizes, wrong checksum); whole archives it must refuse
    (short, bad magic, index not closing at EOF, offsets outside the data
    area); a mode-0 archive; a small synthetic font (BMF + page PNG) for the
    archive -> PNG -> BMFont chain;
  * PNG fixtures written with stdlib zlib: all five filters, 1x1, odd widths,
    multi-IDAT (with an empty IDAT), stored/fixed-Huffman zlib, ancillary
    chunks, and one file per refusal the decoder promises;
  * expected outputs: decoded entry bytes, RGBA pixels, keystream words,
    checksum and name-hash vectors, the rendered chain canvas;
  * manifest.txt, one record per line (see write_manifest()), ending with
    "end <record count>" so a truncated manifest fails the C tests;
  * skin/<variant>/{repentance,afterbirthp,afterbirth}.a: a synthetic game
    install for manager_skin_game_host_test (build_skin()): sheets of the
    sizes the skin expects, two BMFont faces with Cyrillic, and one broken
    variant per failure path the skin must turn into the flat fallback.

Every expected value is computed here independently of the C code under
test: entry bytes are the generator's own input and are re-decoded with
isaac_vita_repack's zlib path (and its engine-ring model for deflate-only
entries); PNG pixels are the raw pixels before filtering; the chain canvas
is rendered by render_text() below from the blend rule in manager_bmfont.h.
Nothing here is game data.

Usage: manager_gameskin_fixtures.py OUTPUT_DIR
"""

from __future__ import annotations

import importlib.util
import random
import struct
import sys
import zlib
from pathlib import Path
from typing import TYPE_CHECKING

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
if TYPE_CHECKING:
    import isaac_vita_repack as rp
else:
    SPEC = importlib.util.spec_from_file_location(
        "isaac_vita_repack", REPO / "tools" / "isaac_vita_repack.py")
    assert SPEC is not None and SPEC.loader is not None
    rp = importlib.util.module_from_spec(SPEC)
    sys.modules[SPEC.name] = rp
    SPEC.loader.exec_module(rp)

MASK32 = 0xFFFFFFFF
BLOCK = rp.BLOCK

# Words of tools/test_isaac_vita_repack.py WORDS: text that compresses.
WORDS = (
    b"gfx/items/collectibles/collectibles_", b"<layer ", b"anm2", b"frame ",
    b"Delay=\"", b"Visible=\"true\" ", b"XPivot=\"", b"YPivot=\"", b"Width=\"32\" ",
    b"Height=\"32\" ", b"RedTint=\"255\" ", b"Interpolated=\"false\"/>\n", b"    ",
    b"function ", b"local ", b"return ", b"end\n", b"Isaac.GetPlayer(0)",
)


class FixtureError(RuntimeError):
    pass


def compressible(rng: random.Random, size: int) -> bytes:
    """tools/test_isaac_vita_repack.py compressible()."""
    out = bytearray()
    while len(out) < size:
        out += rng.choice(WORDS)
        if rng.random() < 0.3:
            out += b"%d" % rng.randrange(1000)
    return bytes(out[:size])


def noise(rng: random.Random, size: int) -> bytes:
    return bytes(rng.randrange(256) for _ in range(size))


def key_of(name: str) -> tuple[int, int]:
    """tools/test_isaac_vita_repack.py key_of() (= name_hash())."""
    hash_a = 5381
    hash_b = 0x5BB2220E
    for byte in name.lower().encode("ascii"):
        hash_a = (hash_a * 33 + byte) & MASK32
        hash_b = ((hash_b ^ byte) * 0x1000193) & MASK32
    return hash_a, hash_b


def hexname(text: str | bytes) -> str:
    data = text.encode("utf-8") if isinstance(text, str) else text
    return data.hex() if data else "-"


class Manifest:
    def __init__(self) -> None:
        self.lines: list[str] = []

    def add(self, *fields: object) -> None:
        text = " ".join(str(f) for f in fields)
        if "\n" in text:
            raise FixtureError("newline in a manifest record")
        self.lines.append(text)

    def write(self, path: Path) -> None:
        body = "\n".join(self.lines + ["end %d" % len(self.lines)]) + "\n"
        path.write_text(body, encoding="ascii", newline="\n")


# ---------------------------------------------------------------------------
# raw deflate bit writer (reserved-symbol streams the encoders never emit)
# ---------------------------------------------------------------------------

class BitWriter:
    """Deflate bit order: header fields and extra bits LSB first, Huffman
    codes MSB first (RFC 1951 3.1.1)."""

    def __init__(self) -> None:
        self.out = bytearray()
        self.acc = 0
        self.count = 0

    def _bit(self, bit: int) -> None:
        self.acc |= bit << self.count
        self.count += 1
        if self.count == 8:
            self.out.append(self.acc)
            self.acc = 0
            self.count = 0

    def bits(self, value: int, count: int) -> None:
        for i in range(count):
            self._bit((value >> i) & 1)

    def code(self, code: int, length: int) -> None:
        for i in range(length - 1, -1, -1):
            self._bit((code >> i) & 1)

    def finish(self) -> bytes:
        if self.count:
            self.out.append(self.acc)
            self.acc = 0
            self.count = 0
        return bytes(self.out)


def fixed_litlen(writer: BitWriter, symbol: int) -> None:
    """RFC 1951 3.2.6 fixed literal/length codes."""
    if symbol < 144:
        writer.code(0x30 + symbol, 8)
    elif symbol < 256:
        writer.code(0x190 + symbol - 144, 9)
    elif symbol < 280:
        writer.code(symbol - 256, 7)
    else:
        writer.code(0xC0 + symbol - 280, 8)


def reserved_stream(literals: bytes, length_symbol: int, dist_symbol: int) -> bytes:
    """One final fixed-Huffman block: literals, one match with the given
    length/distance symbols (286/287 and 30/31 are the reserved ones; none
    of them carries extra bits in tinfl v1.15's tables), end of block."""
    writer = BitWriter()
    writer.bits(1, 1)  # BFINAL
    writer.bits(1, 2)  # BTYPE 01 fixed
    for byte in literals:
        fixed_litlen(writer, byte)
    fixed_litlen(writer, length_symbol)
    writer.code(dist_symbol, 5)
    fixed_litlen(writer, 256)
    return writer.finish()


# ---------------------------------------------------------------------------
# archives
# ---------------------------------------------------------------------------

class ArchiveBuilder:
    """Records for rp.write_archive with unique placeholder offsets (the
    writer deduplicates payloads by (offset, size, checksum))."""

    def __init__(self, name: str) -> None:
        self.name = name
        self.records: list[tuple[rp.Entry, bytes]] = []
        # per row: (path, expected bytes or None, status, stock data)
        self.rows: list[tuple[str, bytes | None, str]] = []
        self._offsets: dict[str, int] = {}

    def add_payload(self, path: str, payload: bytes, size: int, checksum: int,
                    expected: bytes | None, status: str) -> None:
        hash_a, hash_b = key_of(path)
        offset = 14 + len(self.records)
        self._offsets[path] = offset
        self.records.append((rp.Entry(len(self.records), hash_a, hash_b, offset,
                                      size, checksum), payload))
        self.rows.append((path, expected, status))

    def add_pieces(self, path: str, pieces: list[tuple[bytes, bool]],
                   data: bytes, status: str = "ok", size: int | None = None,
                   checksum: int | None = None) -> None:
        self.add_payload(path, rp.pack_pieces(pieces),
                         len(data) if size is None else size,
                         rp.mount_checksum(data) if checksum is None else checksum,
                         data, status)

    def add_stock(self, path: str, data: bytes, status: str = "ok") -> None:
        hash_b = key_of(path)[1]
        self.add_pieces(path, rp.encode_stock_like_entry(data, hash_b), data, status)

    def add_alias(self, path: str, target: str) -> None:
        hash_a, hash_b = key_of(path)
        original = next(r for r in self.records if r[0].offset == self._offsets[target])
        entry = rp.Entry(len(self.records), hash_a, hash_b, original[0].offset,
                         original[0].size, original[0].checksum)
        self.records.append((entry, original[1]))
        expected = next(row[1] for row in self.rows if row[0] == target)
        self.rows.append((path, expected, "ok"))

    def write(self, out: Path, mode: int = rp.MODE_MINIZ) -> Path:
        path = out / self.name
        rp.write_archive(path, mode, self.records)
        return path


def check_archive_with_python(path: Path, rows: list[tuple[str, bytes | None, str]]
                              ) -> list[tuple[int, int]]:
    """Re-decode every "ok"/"checksum" row with isaac_vita_repack (zlib path,
    plus the engine-ring model for deflate-only entries); returns the
    (deflate, stored) piece counts per row, (-1, -1) for refused rows."""
    counts: list[tuple[int, int]] = []
    with rp.Archive(path) as archive:
        limits = archive.payload_limits()
        if len(archive.entries) != len(rows):
            raise FixtureError("%s: %d rows, expected %d" % (path.name, len(archive.entries), len(rows)))
        for entry, (name, expected, status) in zip(archive.entries, rows):
            if status not in ("ok", "checksum"):
                counts.append((-1, -1))
                continue
            assert expected is not None
            if (entry.hash_a, entry.hash_b) != key_of(name):
                raise FixtureError("%s row %d: hash mismatch" % (path.name, entry.index))
            pieces = rp.entry_pieces(archive, entry, limits[entry.offset])
            shape = rp.shape_of(pieces)
            data, _ = rp.decode_entry(shape, entry, True)
            if data is None and entry.size == 0:
                data = b""
            if data != expected:
                raise FixtureError("%s row %d (%s): repack decode differs" % (path.name, entry.index, name))
            if shape.deflate and shape.terminal:
                model, _ = rp.model_decode_pieces(shape.deflate, entry.size, True)
                if model != expected:
                    raise FixtureError("%s row %d: ring model differs" % (path.name, entry.index))
            good_sum = rp.mount_checksum(expected) == entry.checksum
            if good_sum != (status == "ok"):
                raise FixtureError("%s row %d: checksum expectation wrong" % (path.name, entry.index))
            counts.append((len(shape.deflate), len(shape.stored)))
    return counts


def streamed_pieces_need_history(pieces: list[tuple[bytes, bool]]) -> bool:
    """True when some piece after the first does not inflate on its own,
    i.e. the stream really matches into the previous piece."""
    for payload, _final in pieces[1:]:
        try:
            alone = zlib.decompressobj(-15).decompress(payload)
        except zlib.error:
            return True
        if len(alone) != BLOCK and not _final:
            return True
    return False


def build_good_archive(out: Path, manifest: Manifest, rng: random.Random) -> None:
    b = ArchiveBuilder("good.a")
    tiny = compressible(rng, 300)
    b.add_stock("resources/gs/tiny.txt", tiny)
    b.add_stock("resources-dlc3/gs/tiny.txt", compressible(rng, 280))
    deflate = compressible(rng, 5000)
    b.add_stock("resources/gs/deflate.bin", deflate)
    b.add_stock("resources/gs/exact.bin", compressible(rng, BLOCK))
    b.add_stock("resources/gs/two-blocks.bin", compressible(rng, 2 * BLOCK))
    b.add_stock("resources/gs/stored.bin", noise(rng, 3000))
    b.add_stock("resources/gs/stored-odd.bin", noise(rng, 2 * BLOCK + 3))
    mixed = compressible(rng, 3 * BLOCK) + noise(rng, 2 * BLOCK + 0x155)
    b.add_stock("resources/gs/mixed.bin", mixed)
    # Static-Huffman stream, one inflater per entry, 10-bit window: matches
    # reach into the previous piece (the repack tool's output shape).
    streamed = compressible(rng, 4 * BLOCK + 77)
    streamed_pieces = rp.encode_deflate_pieces(streamed, stream=True).pieces
    if not streamed_pieces_need_history(streamed_pieces):
        raise FixtureError("streamed fixture has no cross-piece match")
    b.add_pieces("resources/gs/streamed.bin", streamed_pieces, streamed)
    # Repacked mixed shape: streamed deflate prefix (non-final pieces), then
    # stored pieces keyed from the latch.
    prefix = compressible(rng, 2 * BLOCK)
    tail = noise(rng, BLOCK + 100)
    hash_b = key_of("resources/gs/prefix-stored.bin")[1]
    pieces = (rp.encode_deflate_pieces(prefix, terminal=False, stream=True).pieces
              + rp.encode_stored_pieces(tail, hash_b))
    b.add_pieces("resources/gs/prefix-stored.bin", pieces, prefix + tail)
    b.add_stock("resources/gs/empty.bin", b"")
    b.add_alias("resources/gs/alias.bin", "resources/gs/deflate.bin")
    b.add_stock("resources/GS/Upper Case.BIN", compressible(rng, 900))
    # Refusals inside an otherwise valid archive.
    bad = compressible(rng, 700)
    b.add_pieces("resources/gs/badsum.bin",
                 rp.encode_stock_like_entry(bad, key_of("resources/gs/badsum.bin")[1]),
                 bad, status="checksum", checksum=rp.mount_checksum(bad) ^ 1)
    short = noise(rng, 2100)
    b.add_pieces("resources/gs/stored-short.bin",
                 rp.encode_stock_like_entry(short, key_of("resources/gs/stored-short.bin")[1]),
                 short, status="size", size=3100)
    small = compressible(rng, 300)
    b.add_pieces("resources/gs/deflate-long.bin",
                 rp.encode_stock_like_entry(small, key_of("resources/gs/deflate-long.bin")[1]),
                 small, status="inflate", size=310)
    b.add_pieces("resources/gs/deflate-short.bin",
                 rp.encode_stock_like_entry(small, key_of("resources/gs/deflate-short.bin")[1]),
                 small, status="inflate", size=200)
    b.add_pieces("resources/gs/deflate-huge.bin",
                 rp.encode_stock_like_entry(small, key_of("resources/gs/deflate-huge.bin")[1]),
                 small, status="size", size=2000)
    garbage = bytearray(rp.encode_stock_like_entry(small, 0)[0][0])
    garbage[0] = 0x07  # BFINAL 1, BTYPE 11 (reserved)
    b.add_pieces("resources/gs/garbage.bin", [(bytes(garbage), True)], small,
                 status="inflate")
    b.add_payload("resources/gs/reserved-length.bin",
                  rp.pack_pieces([(reserved_stream(b"abcd", 286, 0), True)]),
                  4, 0, None, "inflate")
    b.add_payload("resources/gs/reserved-distance.bin",
                  rp.pack_pieces([(reserved_stream(b"abcd", 257, 30), True)]),
                  7, 0, None, "inflate")
    b.add_payload("resources/gs/piece-big.bin",
                  struct.pack("<I", 0x800 | rp.PIECE_FINAL) + bytes(0x800),
                  0x800, 0, None, "piece")
    latch = noise(rng, BLOCK)
    b.add_payload("resources/gs/stored-big.bin",
                  struct.pack("<I", BLOCK) + latch
                  + struct.pack("<I", 0x500 | rp.PIECE_FINAL) + bytes(0x500),
                  BLOCK + 0x500, 0, None, "piece")
    b.add_payload("resources/gs/crosses.bin",
                  struct.pack("<I", 0x300 | rp.PIECE_FINAL) + bytes(0x10),
                  0x300, 0, None, "piece")
    b.add_payload("resources/gs/header-cut.bin", b"\x01\x00", 5, 0, None, "piece")
    early = rp.encode_stock_like_entry(small, 0)[0][0]  # a complete stream
    b.add_payload("resources/gs/early-end.bin",
                  rp.pack_pieces([(early, False), (b"\x03\x00", True)]),
                  BLOCK + 2, 0, None, "inflate")
    path = b.write(out)
    counts = check_archive_with_python(path, b.rows)

    manifest.add("archive", path.name, "ok", "ok", rp.MODE_MINIZ, len(b.rows))
    for row, ((name, expected, status), (deflate_pieces, stored_pieces)) in enumerate(zip(b.rows, counts)):
        expected_file = "-"
        if expected is not None and status in ("ok", "checksum"):
            expected_file = "good.%d.bin" % row
            (out / expected_file).write_bytes(expected)
        manifest.add("entry", path.name, row, status, expected_file,
                     deflate_pieces, stored_pieces)
    def row_of(name: str) -> int:
        return next(i for i, row in enumerate(b.rows) if row[0] == name)

    for prefix, rel, row in (
        ("resources/", "gs/tiny.txt", row_of("resources/gs/tiny.txt")),
        ("resources-dlc3/", "gs/tiny.txt", row_of("resources-dlc3/gs/tiny.txt")),
        ("resources/", "gs/alias.bin", row_of("resources/gs/alias.bin")),
        ("resources/", "gs/upper case.bin", row_of("resources/GS/Upper Case.BIN")),
        ("resources/", "GS/UPPER CASE.bin", row_of("resources/GS/Upper Case.BIN")),
        ("-", "resources/gs/deflate.bin", row_of("resources/gs/deflate.bin")),
        ("resources/", "gs/missing.bin", -1),
        ("resources-dlc3/", "gs/deflate.bin", -1),
    ):
        manifest.add("find", path.name, prefix, hexname(rel), row)


def build_bad_archives(out: Path, manifest: Manifest) -> None:
    header = rp.HEADER
    row = rp.INDEX_ENTRY
    good = (out / "good.a").read_bytes()
    cases: list[tuple[str, bytes, str, str]] = [
        ("short.a", b"ARCH000\x02\x0e\x00", "header", "-"),
        ("magic.a", header.pack(b"ARCH001", 2, 14, 0), "header", "-"),
        ("index-cut.a", good[:-7], "index", "-"),
        ("index-extra.a", good + b"\x00\x00\x00", "index", "-"),
        ("index-low.a", header.pack(b"ARCH000", 2, 0, 1) + bytes(6), "index", "-"),
        ("row-low.a", header.pack(b"ARCH000", 2, 14, 1) + row.pack(1, 2, 5, 0, 0),
         "ok", "index"),
        ("row-high.a", header.pack(b"ARCH000", 2, 18, 1) + bytes(4) + row.pack(1, 2, 19, 0, 0),
         "ok", "index"),
        ("empty.a", header.pack(b"ARCH000", 2, 14, 0), "ok", "ok"),
    ]
    for name, data, open_status, index_status in cases:
        (out / name).write_bytes(data)
        # The Python reader must agree on the refusals.
        try:
            rp.Archive(out / name).close()
            python_ok = True
        except rp.RepackError:
            python_ok = False
        if python_ok != (open_status == "ok" and index_status == "ok"):
            raise FixtureError("%s: isaac_vita_repack disagrees" % name)
        count = 0 if open_status != "ok" else struct.unpack_from("<H", data, 12)[0]
        manifest.add("archive", name, open_status, index_status, data[7] if len(data) > 7 else 0, count)


def build_set_archives(out: Path, manifest: Manifest, rng: random.Random) -> None:
    raw = ArchiveBuilder("raw.a")
    raw.add_payload("resources-dlc3/gs/shared.txt", b"raw mode payload", 16, 0, None, "mode")
    raw_path = raw.write(out, rp.MODE_RAW_XOR)
    manifest.add("archive", raw_path.name, "ok", "ok", rp.MODE_RAW_XOR, 1)
    manifest.add("entry", raw_path.name, 0, "mode", "-", -1, -1)

    a = ArchiveBuilder("set-a.a")
    shared_a = compressible(rng, 600)
    a.add_stock("resources/gs/shared.txt", shared_a)
    a_path = a.write(out)
    check_archive_with_python(a_path, a.rows)
    manifest.add("archive", a_path.name, "ok", "ok", rp.MODE_MINIZ, 1)

    b = ArchiveBuilder("set-b.a")
    b.add_stock("resources-dlc3/gs/shared.txt", compressible(rng, 610))
    b.add_stock("resources/gs/only-b.txt", compressible(rng, 620))
    b_path = b.write(out)
    check_archive_with_python(b_path, b.rows)
    manifest.add("archive", b_path.name, "ok", "ok", rp.MODE_MINIZ, 2)

    # Archive order beats prefix order; mode-0 archives are skipped; inside
    # one archive resources-dlc3/ is tried before resources/.
    manifest.add("set", "order", raw_path.name, a_path.name, b_path.name)
    manifest.add("setfind", "order", hexname("gs/shared.txt"), 1, 0)
    manifest.add("setfind", "order", hexname("gs/only-b.txt"), 2, 1)
    manifest.add("setfind", "order", hexname("gs/none.txt"), -1, -1)
    manifest.add("set", "prefix", "good.a")
    manifest.add("setfind", "prefix", hexname("gs/tiny.txt"), 0, 1)
    manifest.add("setfind", "prefix", hexname("gs/deflate.bin"), 0, 2)


# ---------------------------------------------------------------------------
# keystream / checksum / hash vectors
# ---------------------------------------------------------------------------

def build_vectors(out: Path, manifest: Manifest, rng: random.Random) -> None:
    for name in ("resources/gfx/ui/main menu/titlemenu.png", "Resources/FONT/TeamMeatFont16.fnt",
                 "a", "", "resources-dlc3/gfx/ui/main menu/gamemenu.png"):
        hash_a, hash_b = key_of(name)
        manifest.add("hash", hexname(name), hash_a, hash_b)
    for seed in (0, 1, 0x5BB2220E, 0xDEADBEEF, 0xFFFFFFFF):
        keystream = rp.IsaacKeystream(seed)
        words = bytearray()
        for _ in range(600):  # crosses two regenerations
            data = keystream.apply(b"\x00\x00\x00\x00")
            words += data
        file = "keystream.%08x.bin" % seed
        (out / file).write_bytes(bytes(words))
        manifest.add("keystream", seed, 600, file)
    lengths = [1, 3, 4, 5, 1023, 1024, 7, 1022, 2, 1024, 1024, 9]
    for seed in (0x12345678, 0x9E3779B9):
        plain = noise(rng, sum(lengths))
        keystream = rp.IsaacKeystream(seed)
        cipher = bytearray()
        pos = 0
        for length in lengths:
            cipher += keystream.apply(plain[pos:pos + length])
            pos += length
        (out / ("xor.%08x.in" % seed)).write_bytes(plain)
        (out / ("xor.%08x.out" % seed)).write_bytes(bytes(cipher))
        manifest.add("xor", seed, "xor.%08x.in" % seed, "xor.%08x.out" % seed,
                     ",".join(str(n) for n in lengths))
    for size in (0, 1, 2, 3, 4, 5, 0x1FF, 0x200, 0x201, 0x203, 0x3FF, 0x400, 0x401, 5000):
        data = noise(rng, size)
        file = "checksum.%d.bin" % size
        (out / file).write_bytes(data)
        manifest.add("checksum", file, rp.mount_checksum(data))


# ---------------------------------------------------------------------------
# PNG
# ---------------------------------------------------------------------------

SIGNATURE = b"\x89PNG\r\n\x1a\n"


def chunk(kind: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + kind + data
            + struct.pack(">I", zlib.crc32(kind + data) & MASK32))


def ihdr(width: int, height: int, depth: int = 8, color: int = 6,
         compression: int = 0, filter_method: int = 0, interlace: int = 0) -> bytes:
    return chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, color,
                                      compression, filter_method, interlace))


def paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def filter_row(kind: int, row: bytes, prev: bytes, bpp: int) -> bytes:
    """PNG spec filter types 0-4 (encoder side)."""
    out = bytearray(len(row))
    for i, value in enumerate(row):
        left = row[i - bpp] if i >= bpp else 0
        up = prev[i]
        upleft = prev[i - bpp] if i >= bpp else 0
        if kind == 0:
            pred = 0
        elif kind == 1:
            pred = left
        elif kind == 2:
            pred = up
        elif kind == 3:
            pred = (left + up) >> 1
        elif kind == 4:
            pred = paeth(left, up, upleft)
        else:
            raise FixtureError("filter %d" % kind)
        out[i] = (value - pred) & 0xFF
    return bytes(out)


def image_pixels(rng: random.Random, width: int, height: int, channels: int) -> list[bytes]:
    """Rows mixing gradients and noise so every predictor wraps."""
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            for c in range(channels):
                if rng.random() < 0.5:
                    row.append((x * 37 + y * 11 + c * 90) & 0xFF)
                else:
                    row.append(rng.randrange(256))
        rows.append(bytes(row))
    return rows


def filtered_stream(rows: list[bytes], channels: int, filters) -> bytes:
    prev = bytes(len(rows[0])) if rows else b""
    raw = bytearray()
    for y, row in enumerate(rows):
        kind = filters(y)
        raw.append(kind)
        raw += filter_row(kind, row, prev, channels)
        prev = row
    return bytes(raw)


def rgba_of(rows: list[bytes], channels: int) -> bytes:
    out = bytearray()
    for row in rows:
        if channels == 4:
            out += row
        else:
            for x in range(0, len(row), 3):
                out += row[x:x + 3] + b"\xff"
    return bytes(out)


def png_file(width: int, height: int, color: int, idat_payloads: list[bytes],
             before: list[bytes] = (), between: list[bytes] = (), after: list[bytes] = (),
             header: bytes | None = None, end: bool = True) -> bytes:
    data = SIGNATURE + (header if header is not None else ihdr(width, height, 8, color))
    data += b"".join(before)
    for i, payload in enumerate(idat_payloads):
        data += chunk(b"IDAT", payload)
        if i == 0:
            data += b"".join(between)
    data += b"".join(after)
    if end:
        data += chunk(b"IEND", b"")
    return data


def build_pngs(out: Path, manifest: Manifest, rng: random.Random) -> None:
    count = 0

    def good(name: str, width: int, height: int, color: int, filters,
             level: int = 9, strategy: int = zlib.Z_DEFAULT_STRATEGY,
             split: list[int] | None = None, **extra) -> tuple[bytes, list[bytes]]:
        nonlocal count
        channels = 4 if color == 6 else 3
        rows = image_pixels(rng, width, height, channels)
        compressor = zlib.compressobj(level, zlib.DEFLATED, 15, 9, strategy)
        stream = compressor.compress(filtered_stream(rows, channels, filters)) + compressor.flush()
        stream += extra.pop("trailing", b"")
        payloads = [stream]
        if split:
            payloads = []
            pos = 0
            for size in split:
                payloads.append(stream[pos:pos + size])
                pos += size
            payloads.append(stream[pos:])
        data = png_file(width, height, color, payloads, **extra)
        file = "png.%s.png" % name
        (out / file).write_bytes(data)
        (out / (file + ".rgba")).write_bytes(rgba_of(rows, channels))
        manifest.add("png", file, "ok", "ok", width, height, file + ".rgba")
        count += 1
        return stream, rows

    good("rgba-1x1", 1, 1, 6, lambda y: 0)
    good("rgb-1x1", 1, 1, 2, lambda y: 0)
    for color in (6, 2):
        for kind in range(5):
            good("%s-f%d" % ("rgba" if color == 6 else "rgb", kind), 13, 7, color,
                 lambda y, k=kind: k)
    good("rgba-mixed-17x11", 17, 11, 6, lambda y: y % 5)
    good("rgb-mixed-17x11", 17, 11, 2, lambda y: (y * 3) % 5)
    good("rgb-3x5", 3, 5, 2, lambda y: 4 - y % 5)
    good("rgba-7x3", 7, 3, 6, lambda y: (y + 3) % 5)
    good("rgba-wide", 257, 3, 6, lambda y: (y + 1) % 5)
    good("rgb-tall", 1, 40, 2, lambda y: y % 5)
    good("rgba-multi-idat", 31, 9, 6, lambda y: y % 5, split=[1, 0, 5, 17])
    good("rgb-level0", 20, 20, 2, lambda y: y % 5, level=0)
    good("rgba-fixed", 20, 20, 6, lambda y: y % 5, strategy=zlib.Z_FIXED)
    good("rgb-ancillary", 9, 6, 2, lambda y: y % 5,
         before=[chunk(b"gAMA", struct.pack(">I", 45455)), chunk(b"PLTE", bytes(range(12))),
                 chunk(b"zzZz", b"private ancillary")],
         after=[chunk(b"tEXt", b"Comment\x00synthetic")])
    good("rgba-trailing", 5, 4, 6, lambda y: y % 5, trailing=b"\x00junk")
    good("rgba-big", 300, 200, 6, lambda y: y % 5)
    good("rgba-after-iend", 4, 4, 6, lambda y: 2)
    # (the after-iend file gets bytes appended after IEND below)
    path = out / "png.rgba-after-iend.png"
    path.write_bytes(path.read_bytes() + b"trailing bytes after IEND")

    def bad(name: str, data: bytes, probe: str, decode: str, width: int = 0, height: int = 0) -> None:
        nonlocal count
        file = "png.bad-%s.png" % name
        (out / file).write_bytes(data)
        manifest.add("png", file, probe, decode, width, height, "-")
        count += 1

    rows = image_pixels(rng, 6, 3, 4)
    raw = filtered_stream(rows, 4, lambda y: y % 5)
    stream = zlib.compress(raw, 9)
    base = png_file(6, 3, 6, [stream])
    bad("signature", b"\x88" + base[1:], "signature", "signature")
    bad("empty", b"", "signature", "signature")
    bad("truncated", base[:len(base) // 2], "chunk", "chunk")
    bad("ihdr-not-first", SIGNATURE + chunk(b"gAMA", bytes(4)) + base[8:], "chunk", "chunk")
    bad("ihdr-12", SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBB", 6, 3, 8, 6, 0, 0))
        + chunk(b"IDAT", stream) + chunk(b"IEND", b""), "chunk", "chunk")
    bad("ihdr-twice", png_file(6, 3, 6, [stream], before=[ihdr(6, 3)]), "chunk", "chunk", 6, 3)
    bad("no-idat", png_file(6, 3, 6, []), "chunk", "chunk", 6, 3)
    bad("split-idat", png_file(6, 3, 6, [stream[:10], stream[10:]],
                               between=[chunk(b"tEXt", b"a\x00b")]), "chunk", "chunk", 6, 3)
    bad("no-iend", png_file(6, 3, 6, [stream], end=False), "chunk", "chunk", 6, 3)
    bad("huge-chunk", SIGNATURE + ihdr(6, 3) + struct.pack(">I", 0xFFFFFFF0) + b"IDAT" + stream,
        "chunk", "chunk", 6, 3)
    bad("depth16", png_file(6, 3, 6, [stream], header=ihdr(6, 3, 16, 6)), "unsupported", "unsupported", 6, 3)
    bad("palette", png_file(6, 3, 3, [stream], header=ihdr(6, 3, 8, 3),
                            before=[chunk(b"PLTE", bytes(6))]), "unsupported", "unsupported", 6, 3)
    bad("gray", png_file(6, 3, 0, [stream], header=ihdr(6, 3, 8, 0)), "unsupported", "unsupported", 6, 3)
    bad("gray-alpha", png_file(6, 3, 4, [stream], header=ihdr(6, 3, 8, 4)), "unsupported", "unsupported", 6, 3)
    bad("interlaced", png_file(6, 3, 6, [stream], header=ihdr(6, 3, 8, 6, interlace=1)),
        "unsupported", "unsupported", 6, 3)
    bad("compression1", png_file(6, 3, 6, [stream], header=ihdr(6, 3, 8, 6, compression=1)),
        "unsupported", "unsupported", 6, 3)
    bad("filter1", png_file(6, 3, 6, [stream], header=ihdr(6, 3, 8, 6, filter_method=1)),
        "unsupported", "unsupported", 6, 3)
    bad("trns", png_file(6, 3, 2, [zlib.compress(filtered_stream(
        image_pixels(rng, 6, 3, 3), 3, lambda y: 0))], header=ihdr(6, 3, 8, 2),
        before=[chunk(b"tRNS", bytes(6))]), "unsupported", "unsupported", 6, 3)
    bad("critical-unknown", png_file(6, 3, 6, [stream], before=[chunk(b"ABCD", b"x")]),
        "unsupported", "unsupported", 6, 3)
    bad("width0", png_file(0, 3, 6, [stream], header=ihdr(0, 3)), "dimensions", "dimensions", 0, 3)
    bad("height4097", png_file(6, 4097, 6, [stream], header=ihdr(6, 4097)), "dimensions", "dimensions", 6, 4097)
    bad("zlib-header", png_file(6, 3, 6, [bytes([stream[0], stream[1] ^ 1]) + stream[2:]]),
        "ok", "zlib", 6, 3)
    bad("adler32", png_file(6, 3, 6, [stream[:-1] + bytes([stream[-1] ^ 0x55])]), "ok", "zlib", 6, 3)
    bad("zlib-truncated", png_file(6, 3, 6, [stream[:-6]]), "ok", "zlib", 6, 3)
    # A well-formed zlib stream (valid adler32 of the 4 bytes it yields)
    # whose only defect is the reserved length symbol: without the reserved
    # check this would read as "data" (4 of 75 bytes), with it it is "zlib".
    bad("zlib-reserved", png_file(6, 3, 6, [b"\x78\x01" + reserved_stream(b"\x00abc", 286, 0)
                                            + struct.pack(">I", zlib.adler32(b"\x00abc"))]),
        "ok", "zlib", 6, 3)
    bad("rows-extra", png_file(6, 3, 6, [zlib.compress(raw + raw[:25])]), "ok", "data", 6, 3)
    bad("rows-short", png_file(6, 3, 6, [zlib.compress(raw[:-25])]), "ok", "data", 6, 3)
    broken = bytearray(raw)
    broken[2 * 25] = 5  # filter byte of row 2 (stride 6 * 4 + 1)
    bad("filter5", png_file(6, 3, 6, [zlib.compress(bytes(broken))]), "ok", "filter", 6, 3)
    broken[2 * 25] = 255
    bad("filter255", png_file(6, 3, 6, [zlib.compress(bytes(broken))]), "ok", "filter", 6, 3)
    manifest.add("pngcount", count)


# ---------------------------------------------------------------------------
# archive -> PNG -> BMFont chain
# ---------------------------------------------------------------------------

CHAIN_GLYPHS = (
    # id, x, y, w, h, xoffset, yoffset, xadvance (page 0, channel 15)
    (0x41, 0, 0, 5, 7, 0, 1, 6),
    (0x42, 5, 0, 5, 7, 1, 1, 7),
    (0x3F, 10, 0, 4, 7, 0, 1, 5),
    (0x416, 14, 0, 7, 7, -1, 1, 7),
    (0x20, 0, 0, 0, 0, 0, 0, 3),
    (0x67, 21, 0, 4, 9, 0, 3, 5),
)
CHAIN_PAGE = (48, 16)


def bmf_bytes(glyphs, page_name: bytes, scale_w: int, scale_h: int,
              line_height: int = 12, base: int = 9) -> bytes:
    """Binary BMFont v3 (block layout of manager_bmfont.h, measured on the
    game's files): info, common, pages, chars, empty kerning."""
    def block(kind: int, payload: bytes) -> bytes:
        return struct.pack("<BI", kind, len(payload)) + payload
    info = struct.pack("<h", 10) + bytes(12) + b"synthetic\x00"
    common = struct.pack("<HHHHH", line_height, base, scale_w, scale_h, 1) + bytes(5)
    chars = b"".join(struct.pack("<IHHHHhhhBB", *g, 0, 15) for g in reversed(glyphs))
    return (b"BMF\x03" + block(1, info) + block(2, common) + block(3, page_name + b"\x00")
            + block(4, chars) + block(5, b""))


def render_text(canvas: list[int], width: int, height: int, alpha_page: bytes,
                page_w: int, glyphs, text: bytes, x: int, y: int, color: int,
                scale: int) -> tuple[int, int]:
    """Independent renderer of the manager_bmfont.h rules (Python's UTF-8
    decoder with errors='replace' agrees with the C decoder for the lone
    invalid bytes used here).  Returns (pen after the text, measure)."""
    table = {g[0]: g for g in glyphs}
    fallback = table.get(0x3F)
    pen = x
    ca = color >> 24
    for char in text.decode("utf-8", errors="replace"):
        g = table.get(ord(char), fallback)
        if g is None:
            continue
        gid, gx, gy, gw, gh, xo, yo, adv = g
        ox, oy = pen + xo * scale, y + yo * scale
        for cy in range(max(oy, 0), min(oy + gh * scale, height)):
            for cx in range(max(ox, 0), min(ox + gw * scale, width)):
                alpha = alpha_page[(gy + (cy - oy) // scale) * page_w + gx + (cx - ox) // scale]
                if alpha == 0:
                    continue
                alpha = (alpha * ca + 127) // 255
                if alpha == 0:
                    continue
                dst = canvas[cy * width + cx]
                if alpha >= 255:
                    canvas[cy * width + cx] = color | 0xFF000000
                    continue
                inv = 255 - alpha
                out = 0
                for shift in (0, 8, 16):
                    s = (color >> shift) & 0xFF
                    d = (dst >> shift) & 0xFF
                    out |= ((s * alpha + d * inv + 127) // 255) << shift
                out |= ((255 * alpha + (dst >> 24) * inv + 127) // 255) << 24
                canvas[cy * width + cx] = out
        pen += adv * scale
    return pen, pen - x


def build_chain(out: Path, manifest: Manifest, rng: random.Random) -> None:
    page_w, page_h = CHAIN_PAGE
    alpha = bytearray(page_w * page_h)
    rgba = bytearray()
    for i in range(page_w * page_h):
        value = rng.choice((0, 0, 1, 64, 127, 128, 200, 254, 255, 255))
        alpha[i] = value
        # transparent texels carry colour noise (A8 drops it, and the
        # non-white counter must ignore it); opaque ones are white
        rgba += (b"\xff\xff\xff" if value else bytes((rng.randrange(256), 0, 9))) + bytes((value,))
    rows = [bytes(rgba[y * page_w * 4:(y + 1) * page_w * 4]) for y in range(page_h)]
    stream = zlib.compress(filtered_stream(rows, 4, lambda y: y % 5), 9)
    page_png = png_file(page_w, page_h, 6, [stream])
    fnt = bmf_bytes(CHAIN_GLYPHS, b"chainfont_0.png", page_w, page_h)

    builder = ArchiveBuilder("chain.a")
    builder.add_stock("resources/font/chainfont.fnt", fnt)
    builder.add_stock("resources/font/chainfont_0.png", page_png)
    path = builder.write(out)
    check_archive_with_python(path, builder.rows)
    manifest.add("archive", path.name, "ok", "ok", rp.MODE_MINIZ, 2)
    (out / "chain.page.a8").write_bytes(bytes(alpha))

    text = "AB gЖ€".encode("utf-8") + b"\xff" + b"?\x80A"
    width, height, background = 64, 40, 0xFF302010
    for name, x, y, color, scale in (("s1", 2, 3, 0xE01080F0, 1),
                                     ("s2", -3, 14, 0xFFFFFFFF, 2),
                                     ("edge", 40, 34, 0x80FF00FF, 2)):
        canvas = [background] * (width * height)
        pen, measure = render_text(canvas, width, height, bytes(alpha), page_w,
                                   CHAIN_GLYPHS, text, x, y, color, scale)
        file = "chain.%s.canvas" % name
        (out / file).write_bytes(struct.pack("<%dI" % len(canvas), *canvas))
        manifest.add("chain", path.name, hexname("font/chainfont.fnt"), hexname(text),
                     x, y, scale, color, background, width, height, file, measure, pen,
                     len(CHAIN_GLYPHS), "chain.page.a8")


# ---------------------------------------------------------------------------
# synthetic game-skin install (manager_skin_game_host_test)
# ---------------------------------------------------------------------------
#
# Stand-ins for the files manager_skin_game.c reads, with the sizes the
# skin expects (measured on the game's files) and content drawn here from
# simple shapes: torn-edged "paper" boxes at the skin's crop rectangles, an
# ink cursor, button blobs at the Buttons.anm2 frame boxes, and two
# BMFont faces whose glyphs are box patterns derived from the code point,
# with made-up metrics.  The faces cover ASCII, U+00B7, U+2026 and the
# Russian alphabet (U+0401, U+0410-044F, U+0451), so Russian strings run
# through the BMFont path and anything else (U+00D7, U+2014, U+25B8, ...)
# through the DejaVu fallback.  Nothing here is game data.

SKIN_SHEET_SIZE = (512, 256)     # gfx/ui/main menu/KeyConfigMenu.png
SKIN_BACKDROP_SIZE = (480, 270)  # gfx/ui/main menu/EmptyScreen.png (RGB)
SKIN_POPUP_SIZE = (290, 192)     # gfx/ui/GenericPopup.png
SKIN_BUTTONS_SIZE = (320, 256)   # gfx/ui/Buttons.png
# The crop rectangles manager_skin_game.c uses (its constants).
SKIN_SHEET_BOX = (5, 2, 404, 190)
SKIN_STRIP_BOX = (4, 201, 225, 52)
SKIN_POPUP_BOX = (40, 41, 231, 129)
SKIN_CURSOR_BOX = (2, 41, 12, 15)
SKIN_BUTTON_BOXES = (
    (136, 2, 15, 11), (136, 18, 15, 11), (136, 34, 15, 11), (136, 50, 15, 11),
    (167, 97, 19, 13), (170, 36, 13, 9), (170, 52, 13, 9),
)
SKIN_DPAD_BOXES = (  # (box, arm) with the white arm of frames 4..7
    ((137, 65, 14, 14), "down"), ((137, 81, 14, 14), "up"),
    ((137, 97, 14, 14), "right"), ((137, 113, 14, 14), "left"),
)
SKIN_FACES = (
    # stem, line height, base, glyph height, page size
    ("TeamMeatFont12", 18, 14, 13, (256, 256)),
    ("TeamMeatFont16", 24, 20, 17, (256, 128)),
)


def skin_png(width: int, height: int, pixels: list[list[tuple[int, ...]]], color: int = 6) -> bytes:
    channels = 4 if color == 6 else 3
    rows = [bytes(v for px in row for v in px[:channels]) for row in pixels]
    stream = zlib.compress(filtered_stream(rows, channels, lambda y: y % 5), 9)
    return png_file(width, height, color, [stream])


def skin_canvas(width: int, height: int, fill: tuple[int, ...]) -> list[list[tuple[int, ...]]]:
    return [[fill] * width for _ in range(height)]


def skin_paper(canvas, box, tint, rng: random.Random) -> None:
    """A torn-edged paper with an ink outline and a shadow inside box."""
    x0, y0, w, h = box
    edge_top = [rng.randrange(3) for _ in range(w)]
    edge_bottom = [rng.randrange(3) for _ in range(w)]
    edge_left = [rng.randrange(3) for _ in range(h)]
    edge_right = [rng.randrange(3) for _ in range(h)]
    for y in range(h):
        for x in range(w):
            inside = (y >= edge_top[x] and y < h - 3 - edge_bottom[x]
                      and x >= edge_left[y] and x < w - 3 - edge_right[y])
            shadow = (y >= edge_top[x] + 3 and y < h - edge_bottom[x]
                      and x >= edge_left[y] + 3 and x < w - edge_right[y])
            if inside:
                border = (y == edge_top[x] or y == h - 4 - edge_bottom[x]
                          or x == edge_left[y] or x == w - 4 - edge_right[y])
                if border:
                    canvas[y0 + y][x0 + x] = (80, 66, 70, 255)
                else:
                    speck = 6 if (x * 7 + y * 13) % 41 == 0 else 0
                    canvas[y0 + y][x0 + x] = (tint[0] - speck, tint[1] - speck, tint[2] - speck, 255)
            elif shadow:
                canvas[y0 + y][x0 + x] = (110, 100, 104, 160)


def skin_backdrop() -> bytes:
    width, height = SKIN_BACKDROP_SIZE
    pixels = []
    for y in range(height):
        row = []
        for x in range(width):
            base = 196 + ((x // 6 + y // 9) % 3)
            ring = ((x - 120) ** 2 + (y - 140) ** 2) // 40 in (90, 91, 140)
            ring = ring or ((x - 380) ** 2 + (y - 60) ** 2) // 30 in (40, 41)
            value = base - 40 if ring else base
            row.append((value, value - 8, value - 10))
        pixels.append(row)
    return skin_png(width, height, pixels, color=2)


def skin_sheet(rng: random.Random) -> bytes:
    width, height = SKIN_SHEET_SIZE
    canvas = skin_canvas(width, height, (255, 255, 255, 0))
    skin_paper(canvas, SKIN_SHEET_BOX, (232, 220, 224), rng)
    skin_paper(canvas, SKIN_STRIP_BOX, (226, 216, 220), rng)
    for x in range(300, 400):  # unrelated art the crops must not take
        canvas[230][x] = (40, 30, 30, 255)
    return skin_png(width, height, canvas)


def skin_popup(rng: random.Random, tint: tuple[int, int, int]) -> bytes:
    width, height = SKIN_POPUP_SIZE
    canvas = skin_canvas(width, height, (255, 255, 255, 0))
    skin_paper(canvas, SKIN_POPUP_BOX, tint, rng)
    x0, y0, w, h = SKIN_CURSOR_BOX
    for y in range(h):  # an ink triangle pointing right
        reach = (h // 2 - abs(y - h // 2)) * w // (h // 2)
        for x in range(min(w, reach + 1)):
            canvas[y0 + y][x0 + x] = (47, 33, 33, 255)
    return skin_png(width, height, canvas)


def skin_buttons() -> bytes:
    width, height = SKIN_BUTTONS_SIZE
    canvas = skin_canvas(width, height, (0, 0, 0, 0))
    for index, (x0, y0, w, h) in enumerate(SKIN_BUTTON_BOXES):
        cx, cy = (w - 1) / 2, (h - 1) / 2
        for y in range(h):
            for x in range(w):
                d = ((x - cx) / (w / 2)) ** 2 + ((y - cy) / (h / 2)) ** 2
                if d <= 1.0:
                    edge = d > 0.6
                    canvas[y0 + y][x0 + x] = ((30, 30, 30, 255) if edge else
                                              (60 + 25 * index, 90, 140 - 10 * index, 255))
    for (x0, y0, w, h), arm in SKIN_DPAD_BOXES:
        for y in range(h):
            for x in range(w):
                if not (4 <= x < 10 or 4 <= y < 10):
                    continue
                white = {"up": y < 4, "down": y >= 10, "left": x < 4, "right": x >= 10}[arm]
                canvas[y0 + y][x0 + x] = (255, 255, 255, 255) if white else (70, 70, 70, 255)
    return skin_png(width, height, canvas)


def skin_face(stem: str, line_height: int, base: int, glyph_h: int,
              page: tuple[int, int]) -> tuple[bytes, bytes]:
    """A BMFont face and its white page; glyph shapes are box patterns."""
    codepoints = (list(range(0x20, 0x7F)) + [0xB7, 0x2026, 0x401]
                  + list(range(0x410, 0x450)) + [0x451])
    page_w, page_h = page
    alpha = bytearray(page_w * page_h)
    glyphs = []
    x = y = 0
    for cp in codepoints:
        if cp == 0x20:
            glyphs.append((cp, 0, 0, 0, 0, 0, 0, glyph_h // 3))
            continue
        w = 3 + (cp * 7) % 5 + (2 if cp >= 0x400 else 0)
        h = glyph_h if cp >= 0x41 else glyph_h - 2
        if x + w + 1 > page_w:
            x, y = 0, y + glyph_h + 1
        if y + h > page_h:
            raise FixtureError("%s: page too small" % stem)
        for gy in range(h):
            for gx in range(w):
                edge = gx in (0, w - 1) or gy in (0, h - 1)
                bit = (cp >> ((gx + gy) % 11)) & 1
                if edge or bit:
                    alpha[(y + gy) * page_w + x + gx] = 255 if edge else 160
        yoffset = base - h + (2 if cp in (0x67, 0x70, 0x71, 0x79, 0x443) else 0)
        glyphs.append((cp, x, y, w, h, 0, yoffset, w + 1))
        x += w + 1
    rgba = []
    for row in range(page_h):
        rgba.append([(255, 255, 255, alpha[row * page_w + col]) for col in range(page_w)])
    fnt = bmf_bytes(glyphs, (stem + "_0.png").encode("ascii"), page_w, page_h,
                    line_height=line_height, base=base)
    return fnt, skin_png(page_w, page_h, rgba)


def build_skin(out: Path, rng: random.Random) -> None:
    """skin/<variant>/{repentance,afterbirthp,afterbirth}.a for the skin test."""
    root = out / "skin"
    root.mkdir(exist_ok=True)
    for variant in root.iterdir():
        if variant.is_dir():
            for stale in variant.iterdir():
                stale.unlink()
    backdrop = skin_backdrop()
    sheet = skin_sheet(rng)
    popup = skin_popup(rng, (226, 214, 218))
    popup_dlc3 = skin_popup(rng, (236, 226, 214))
    buttons = skin_buttons()
    faces = {stem: skin_face(stem, lh, base, gh, page) for stem, lh, base, gh, page in SKIN_FACES}

    def install(variant: str, *, repentance=True, afterbirthp=True, afterbirth=True,
                replace: dict[str, bytes] | None = None, drop: tuple[str, ...] = (),
                damage_repentance_index=False) -> None:
        files = {
            "afterbirth.a": {
                "resources/gfx/ui/main menu/EmptyScreen.png": backdrop,
                "resources/gfx/ui/main menu/KeyConfigMenu.png": sheet,
                "resources/gfx/ui/GenericPopup.png": popup,
            },
            "afterbirthp.a": {
                "resources/gfx/ui/Buttons.png": buttons,
            },
            # A newer copy under the dlc3 prefix: repentance.a is searched
            # first, so this paper wins (manager_arch_default_order).
            "repentance.a": {
                "resources-dlc3/gfx/ui/GenericPopup.png": popup_dlc3,
                "resources/unrelated/readme.txt": b"synthetic unrelated row\n",
            },
        }
        for stem, (fnt, page) in faces.items():
            files["afterbirthp.a"]["resources/font/%s.fnt" % stem] = fnt
            files["afterbirthp.a"]["resources/font/%s_0.png" % stem] = page
        for name, data in (replace or {}).items():
            for rows in files.values():
                if name in rows:
                    rows[name] = data
        for name in drop:
            for rows in files.values():
                rows.pop(name, None)
        target = root / variant
        target.mkdir(exist_ok=True)
        wanted = {"repentance.a": repentance, "afterbirthp.a": afterbirthp,
                  "afterbirth.a": afterbirth}
        for archive, rows in files.items():
            if not wanted[archive]:
                continue
            builder = ArchiveBuilder(archive)
            for name, data in rows.items():
                builder.add_stock(name, data)
            path = builder.write(target)
            check_archive_with_python(path, builder.rows)
            if archive == "repentance.a" and damage_repentance_index:
                data = bytearray(path.read_bytes())
                # Top byte of the last row's offset: far outside the data
                # area, so manager_arch_load_index() refuses the index.
                data[-9] ^= 0xFF
                path.write_bytes(bytes(data))

    install("good")
    install("damaged-index", damage_repentance_index=True)
    install("empty", repentance=False, afterbirthp=False, afterbirth=False)
    install("no-afterbirthp", afterbirthp=False)
    install("no-popup", drop=("resources/gfx/ui/GenericPopup.png",
                              "resources-dlc3/gfx/ui/GenericPopup.png"))
    corrupt = bytearray(sheet)
    corrupt[len(corrupt) // 2] ^= 0x5A
    install("bad-png", replace={"resources/gfx/ui/main menu/KeyConfigMenu.png": bytes(corrupt)})
    small = skin_png(240, 135, [[(200, 190, 190)] * 240 for _ in range(135)], color=2)
    install("bad-size", replace={"resources/gfx/ui/main menu/EmptyScreen.png": small})
    fnt12 = faces["TeamMeatFont12"][0]
    install("bad-font", replace={"resources/font/TeamMeatFont12.fnt": fnt12[:len(fnt12) - 7]})
    install("bad-page", replace={"resources/font/TeamMeatFont16_0.png": faces["TeamMeatFont12"][1]})


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    out = Path(argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for stale in out.iterdir():
        if stale.is_file():
            stale.unlink()
    rng = random.Random(0x6A3E5C1B)
    manifest = Manifest()
    build_good_archive(out, manifest, rng)
    build_bad_archives(out, manifest)
    build_set_archives(out, manifest, rng)
    build_vectors(out, manifest, rng)
    build_pngs(out, manifest, rng)
    build_chain(out, manifest, rng)
    manifest.write(out / "manifest.txt")
    build_skin(out, random.Random(0x5C1E6A3B))
    print("manager game-skin fixtures: %d records, %d files in %s"
          % (len(manifest.lines), sum(1 for _ in out.iterdir()), out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
