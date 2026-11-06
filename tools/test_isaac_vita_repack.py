#!/usr/bin/env python3
"""Tests for tools/isaac_vita_repack.py.

Synthetic archives are built with the tool's own stock-shaped writer (dynamic
Huffman per 0x400 piece, stored latch for incompressible chunks, mixed
entries), repacked, and checked against the generated ground truth with three
independent decoders: zlib fed with the reader's cadence, the pure-Python
engine model (0x400 wrapping ring, persistent state per entry), and -- when a
host C compiler is available -- the Vita runtime's own tinfl port
(recomp/runtime/host_vita_archive_miniz_native.c) driven exactly like
sub_0059c270 drives it.  A real-archive smoke test runs when a packed
directory is found (ISAAC_VITA_PACKED_DIR / ISAAC_VITA_REPACK_SMOKE_ARCHIVE or
the usual Steam locations) and is skipped otherwise.

Set ISAAC_VITA_REPACK_TEST_TMP to keep scratch files inside a chosen directory.
"""

from __future__ import annotations

import atexit
import ctypes
import importlib.util
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from typing import TYPE_CHECKING


HERE = Path(__file__).resolve().parent
RUNTIME = HERE.parent / "recomp" / "runtime"
if TYPE_CHECKING:
    import isaac_vita_repack as rp
else:
    SPEC = importlib.util.spec_from_file_location("isaac_vita_repack", HERE / "isaac_vita_repack.py")
    assert SPEC is not None and SPEC.loader is not None
    rp = importlib.util.module_from_spec(SPEC)
    sys.modules[SPEC.name] = rp
    SPEC.loader.exec_module(rp)

TEST_TMP = os.environ.get("ISAAC_VITA_REPACK_TEST_TMP") or None
if TEST_TMP:
    Path(TEST_TMP).mkdir(parents=True, exist_ok=True)

# The native tinfl library is built once per process and stays loaded until
# exit; on Windows a loaded DLL cannot be unlinked, so it lives in its own
# directory whose cleanup tolerates that.
NATIVE_TMP = tempfile.TemporaryDirectory(dir=TEST_TMP, ignore_cleanup_errors=True)
atexit.register(NATIVE_TMP.cleanup)

WORDS = (
    b"gfx/items/collectibles/collectibles_", b"<layer ", b"anm2", b"frame ",
    b"Delay=\"", b"Visible=\"true\" ", b"XPivot=\"", b"YPivot=\"", b"Width=\"32\" ",
    b"Height=\"32\" ", b"RedTint=\"255\" ", b"Interpolated=\"false\"/>\n", b"    ",
    b"function ", b"local ", b"return ", b"end\n", b"Isaac.GetPlayer(0)",
)


def compressible(rng: random.Random, size: int) -> bytes:
    out = bytearray()
    while len(out) < size:
        out += rng.choice(WORDS)
        if rng.random() < 0.3:
            out += b"%d" % rng.randrange(1000)
    return bytes(out[:size])


def key_of(name: str) -> tuple[int, int]:
    hash_a = 5381
    hash_b = 0x5BB2220E
    for byte in name.lower().encode("ascii"):
        hash_a = (hash_a * 33 + byte) & 0xFFFFFFFF
        hash_b = ((hash_b ^ byte) * 0x1000193) & 0xFFFFFFFF
    return hash_a, hash_b


def build_stock_like_archive(path: Path, items: list[tuple[str, bytes]], aliases: dict[str, str] | None = None) -> None:
    """items in index order; ``aliases`` maps an extra name to an existing name
    so two index rows share one payload (offset)."""
    records: list[tuple[rp.Entry, bytes]] = []
    offsets: dict[str, int] = {}
    for index, (name, data) in enumerate(items):
        hash_a, hash_b = key_of(name)
        pieces = rp.encode_stock_like_entry(data, hash_b)
        payload = rp.pack_pieces(pieces)
        dummy_offset = 14 + index
        offsets[name] = dummy_offset
        records.append((rp.Entry(index, hash_a, hash_b, dummy_offset, len(data), rp.mount_checksum(data)), payload))
    for alias, target in (aliases or {}).items():
        hash_a, hash_b = key_of(alias)
        original = next(r for r in records if r[0].offset == offsets[target])
        entry = rp.Entry(len(records), hash_a, hash_b, offsets[target], original[0].size, original[0].checksum)
        records.append((entry, original[1]))
    rp.write_archive(path, rp.MODE_MINIZ, records)


def compiler() -> str | None:
    for candidate in (os.environ.get("CC"), "clang", "gcc", "cc"):
        if candidate and shutil.which(candidate):
            return candidate
    if os.name == "nt":
        installed = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM" / "bin" / "clang.exe"
        if installed.is_file():
            return str(installed)
    return None


class NativeTinfl:
    """The Vita runtime's tinfl port, called like sub_0059c270 calls it."""

    STATE_BYTES = 0x2AF0
    DONE = 0
    NEEDS_MORE_INPUT = 1
    HAS_MORE_INPUT = 2

    def __init__(self, library: Path) -> None:
        self.library = ctypes.CDLL(str(library))
        self.call = self.library.isaac_vita_archive_miniz_native
        self.call.argtypes = (
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_uint32,
        )
        self.call.restype = ctypes.c_int

    def decode_prefix(self, pieces, expected: int, terminal: bool) -> bytes:
        """One zeroed state per entry (0x59c216), one persistent 0x400 ring
        (+0x81c) refilled from slot 0, flags = (!final) * 2, whole piece as
        input.  Returns the bytes the reader would see."""
        state = ctypes.create_string_buffer(self.STATE_BYTES)
        ring = (ctypes.c_uint8 * rp.BLOCK)()
        out = bytearray()
        remaining = expected
        for number, (payload, final) in enumerate(pieces):
            data = (ctypes.c_uint8 * len(payload)).from_buffer_copy(payload)
            in_size = ctypes.c_uint32(len(payload))
            out_size = ctypes.c_uint32(rp.BLOCK)
            status = self.call(state, data, ctypes.byref(in_size), ring, ring, ctypes.byref(out_size),
                               0 if final else self.HAS_MORE_INPUT)
            want = remaining if final else rp.BLOCK
            if in_size.value != len(payload):
                raise AssertionError("piece %d: tinfl consumed %d of %d bytes" % (number, in_size.value, len(payload)))
            if out_size.value != want:
                raise AssertionError("piece %d: tinfl produced %d bytes, expected %d" % (number, out_size.value, want))
            expected_status = self.DONE if final else self.NEEDS_MORE_INPUT
            if status != expected_status:
                raise AssertionError("piece %d: tinfl status %d, expected %d" % (number, status, expected_status))
            out += bytes(ring[:want])
            remaining -= want
        if remaining != 0 or (terminal and not pieces[-1][1]):
            raise AssertionError("pieces did not cover the entry")
        return bytes(out)


_NATIVE: NativeTinfl | None | bool = False


def native_tinfl() -> NativeTinfl | None:
    global _NATIVE
    if _NATIVE is not False:
        return _NATIVE  # type: ignore[return-value]
    cc = compiler()
    source = RUNTIME / "host_vita_archive_miniz_native.c"
    if cc is None or not source.is_file():
        _NATIVE = None
        return None
    library = Path(NATIVE_TMP.name) / ("archive_miniz_native.dll" if os.name == "nt" else "archive_miniz_native.so")
    command = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-shared"]
    if os.name != "nt":
        command.append("-fPIC")
    command += ["-DISAAC_VITA_ARCHIVE_MINIZ_TEST_EXPORT=1", str(source), "-o", str(library)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise AssertionError("native tinfl build failed:\n%s\n%s" % (result.stdout, result.stderr))
    _NATIVE = NativeTinfl(library)
    return _NATIVE


def native_check_archive(native: NativeTinfl, original: Path, repacked: Path) -> int:
    """Every deflate prefix of the repacked archive through the runtime's tinfl
    must equal the original's zlib decode.  Returns the number of prefixes."""
    checked = 0
    with rp.Archive(original) as a, rp.Archive(repacked) as b:
        la, lb = a.payload_limits(), b.payload_limits()
        for x, y in zip(a.entries, b.entries):
            sa = rp.shape_of(rp.entry_pieces(a, x, la[x.offset]))
            sb = rp.shape_of(rp.entry_pieces(b, y, lb[y.offset]))
            if not sb.deflate:
                continue
            expected = x.size if sa.terminal else rp.BLOCK * len(sa.deflate)
            truth = rp.decode_deflate_pieces(sa.deflate, expected, sa.terminal).data
            got = native.decode_prefix(sb.deflate, expected, sb.terminal)
            if got != truth:
                raise AssertionError("entry %d: runtime tinfl decode differs" % x.index)
            checked += 1
    return checked


def find_real_archives() -> list[Path]:
    single = os.environ.get("ISAAC_VITA_REPACK_SMOKE_ARCHIVE")
    if single and Path(single).is_file():
        return [Path(single)]
    candidates = []
    packed = os.environ.get("ISAAC_VITA_PACKED_DIR")
    if packed:
        candidates.append(Path(packed))
    for root in (
        os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)") + r"\Steam\steamapps\common",
        r"D:\SteamLibrary\steamapps\common",
        os.path.expanduser("~/.steam/steam/steamapps/common"),
        os.path.expanduser("~/Library/Application Support/Steam/steamapps/common"),
    ):
        candidates.append(Path(root) / "The Binding of Isaac Rebirth" / "resources" / "packed")
    found: list[Path] = []
    for directory in candidates:
        if directory.is_dir():
            found.extend(sorted(p for p in directory.glob("*.a") if p.is_file()))
    return found


class RepackTests(unittest.TestCase):
    def setUp(self) -> None:
        self.rng = random.Random(20260910)
        self._tmp = tempfile.TemporaryDirectory(dir=TEST_TMP)
        self.tmp = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    # -- synthetic corpus -----------------------------------------------------
    def corpus(self) -> list[tuple[str, bytes]]:
        rng = self.rng
        random_bytes = rng.randbytes
        return [
            ("resources/gfx/a.anm2", compressible(rng, 5000)),                       # deflate, 5 pieces
            ("resources/sfx/noise.wav", random_bytes(3000)),                           # stored from piece 0
            ("resources/mixed.bin", compressible(rng, 2048) + random_bytes(4096)),    # deflate prefix + stored
            ("resources/tiny.txt", b"0123456789"),                                     # one final piece
            ("resources/exact.txt", compressible(rng, 1024)),                          # exactly one block
            ("resources/plusone.txt", compressible(rng, 1025)),                        # two pieces, tiny tail
            ("resources/empty.txt", b""),                                              # no pieces
            ("resources/big.xml", compressible(rng, 70000)),                           # 69 pieces, long stream
            ("resources/mixed2.bin", compressible(rng, 1024) + random_bytes(1024) + compressible(rng, 3000)),
        ]

    def test_stock_like_writer_shapes(self) -> None:
        items = dict(self.corpus())
        shapes = {name: rp.shape_of(rp.encode_stock_like_entry(data, key_of(name)[1])).kind
                  for name, data in items.items()}
        self.assertEqual(shapes["resources/gfx/a.anm2"], "deflate")
        self.assertEqual(shapes["resources/sfx/noise.wav"], "stored")
        self.assertEqual(shapes["resources/mixed.bin"], "mixed")
        self.assertEqual(shapes["resources/mixed2.bin"], "mixed")
        self.assertEqual(shapes["resources/empty.txt"], "empty")
        self.assertEqual(shapes["resources/tiny.txt"], "deflate")
        mixed = rp.shape_of(rp.encode_stock_like_entry(items["resources/mixed.bin"], key_of("resources/mixed.bin")[1]))
        self.assertEqual(len(mixed.deflate), 2)
        self.assertEqual(len(mixed.stored), 4)
        self.assertTrue(all(len(p) == rp.BLOCK for p, final in mixed.stored if not final))

    def test_repack_roundtrip_against_ground_truth(self) -> None:
        items = self.corpus()
        source = self.tmp / "synthetic.a"
        output = self.tmp / "synthetic.repacked.a"
        build_stock_like_archive(source, items, aliases={"resources/gfx/alias.anm2": "resources/gfx/a.anm2"})
        stats = rp.repack_archive(source, output, verify="full", verify_stored=True)
        self.assertEqual(stats.passthrough_entries, 0)
        self.assertEqual(stats.entries, len(items) + 1)
        self.assertEqual(stats.deflate_entries, 5)
        self.assertEqual(stats.mixed_entries, 2)
        self.assertEqual(stats.stored_entries, 1)
        self.assertEqual(stats.empty_entries, 1)
        assert stats.repacked_census is not None and stats.original_census is not None
        self.assertEqual(stats.repacked_census["dynamic"], 0)
        self.assertEqual(stats.dynamic_to_fixed, stats.original_census["dynamic"])
        self.assertGreater(stats.dynamic_to_fixed, 70)
        self.assertEqual(stats.stored_verified_by_decode, 1)
        # ground truth: every row decodes to the generated bytes, keys/sizes/checksums unchanged
        truth = dict(items)
        truth["resources/gfx/alias.anm2"] = truth["resources/gfx/a.anm2"]
        with rp.Archive(source) as a, rp.Archive(output) as b:
            self.assertEqual(b.mode, rp.MODE_MINIZ)
            self.assertEqual(b.index_offset + 20 * b.count, b.size)
            self.assertEqual(b.entries[0].offset, rp.HEADER.size)
            limits = b.payload_limits()
            names = [name for name, _ in items] + ["resources/gfx/alias.anm2"]
            for x, y, name in zip(a.entries, b.entries, names):
                self.assertEqual((x.hash_a, x.hash_b, x.size, x.checksum), (y.hash_a, y.hash_b, y.size, y.checksum))
                self.assertEqual((y.hash_a, y.hash_b), key_of(name))
                shape = rp.shape_of(rp.entry_pieces(b, y, limits[y.offset]))
                data, _ = rp.decode_entry(shape, y, stored_too=True)
                self.assertEqual(data, truth[name], name)
                assert data is not None
                self.assertEqual(rp.mount_checksum(data), y.checksum)
                for payload, final in shape.deflate:
                    self.assertNotEqual((len(payload), final), (rp.BLOCK, False))
                    self.assertLessEqual(len(payload), rp.PIECE_MAX)
                    if not final:
                        self.assertEqual(payload[-4:], rp.SYNC_MARKER)
            # the alias shares its payload with the original entry; every
            # other payload is written in index order
            self.assertEqual(b.entries[-1].offset, b.entries[0].offset)
            offsets = [e.offset for e in b.entries[:-1]]
            self.assertEqual(offsets, sorted(offsets))
            # the empty entry owns no bytes: it sits at the next entry's offset
            # in both files and must not turn that entry into its alias
            self.assertEqual(a.entries[6].offset, a.entries[7].offset)
            self.assertEqual(b.entries[6].offset, b.entries[7].offset)
            self.assertEqual(rp.shape_of(rp.entry_pieces(b, b.entries[7], limits[b.entries[7].offset])).kind, "deflate")

    def test_repacked_pieces_are_static_and_smaller_or_equal_window(self) -> None:
        data = compressible(self.rng, 20000)
        encoded = rp.encode_deflate_pieces(data)
        self.assertEqual(len(encoded.pieces), 20)
        self.assertEqual(encoded.static_pieces, 20)
        decoded, census = rp.model_decode_pieces(encoded.pieces, len(data), True)
        self.assertEqual(decoded, data)
        self.assertEqual(census.dynamic, 0)
        self.assertEqual(census.static, 20)
        self.assertEqual(census.stored, 19)   # one sync-flush marker per non-final piece
        # the same data through zlib with the reader's cadence
        self.assertEqual(rp.decode_deflate_pieces(encoded.pieces, len(data), True).data, data)

    def test_prefix_encoding_without_final_block(self) -> None:
        data = compressible(self.rng, 3 * rp.BLOCK)
        encoded = rp.encode_deflate_pieces(data, terminal=False)
        self.assertEqual([final for _, final in encoded.pieces], [False, False, False])
        decoded, _ = rp.model_decode_pieces(encoded.pieces, len(data), False)
        self.assertEqual(decoded, data)
        self.assertEqual(rp.decode_deflate_pieces(encoded.pieces, len(data), False).data, data)
        with self.assertRaises(rp.RepackError):
            rp.encode_deflate_pieces(data[:-1], terminal=False)
        with self.assertRaises(rp.RepackError):
            rp.model_decode_pieces(encoded.pieces, len(data), True)

    def test_per_piece_mode_and_budget(self) -> None:
        data = compressible(self.rng, 8 * rp.BLOCK)
        streamed = rp.encode_deflate_pieces(data, stream=True)
        per_piece = rp.encode_deflate_pieces(data, stream=False)
        self.assertLessEqual(sum(len(p) for p, _ in streamed.pieces), sum(len(p) for p, _ in per_piece.pieces))
        for encoded in (streamed, per_piece):
            self.assertEqual(rp.model_decode_pieces(encoded.pieces, len(data), True)[0], data)
        budget = rp.encode_deflate_pieces(data, static_budget=-1)   # dynamic whenever it is smaller
        self.assertEqual(rp.model_decode_pieces(budget.pieces, len(data), True)[0], data)
        self.assertEqual(budget.static_pieces + budget.dynamic_pieces + budget.fallback_pieces, 8)

    def test_forbidden_piece_length_takes_a_fallback(self) -> None:
        data = compressible(self.rng, 4 * rp.BLOCK + 100)

        class Forbidden:
            """First flush returns a 0x400-byte piece; the real compressor still sees every chunk."""
            calls = 0

            def __init__(self, level: int, strategy: int) -> None:
                self.real = zlib.compressobj(level, zlib.DEFLATED, -rp.WINDOW_BITS, 9, strategy)

            def compress(self, chunk: bytes) -> bytes:
                return self.real.compress(chunk)

            def flush(self, mode: int) -> bytes:
                out = self.real.flush(mode)
                Forbidden.calls += 1
                if Forbidden.calls == 2:
                    return b"\x00" * rp.BLOCK
                return out

        encoded = rp.encode_deflate_pieces(data, compressor_factory=Forbidden)
        self.assertEqual(encoded.fallback_pieces, 1)
        self.assertEqual(encoded.static_pieces, 4)
        self.assertTrue(all(rp.piece_ok(p, f) for p, f in encoded.pieces))
        self.assertEqual(rp.model_decode_pieces(encoded.pieces, len(data), True)[0], data)
        self.assertEqual(rp.decode_deflate_pieces(encoded.pieces, len(data), True).data, data)

    # -- engine model ---------------------------------------------------------
    def test_model_matches_dynamic_stock_pieces(self) -> None:
        data = compressible(self.rng, 6000)
        pieces = rp.encode_stock_like_entry(data, 0x1234)
        decoded, census = rp.model_decode_pieces(pieces, len(data), True)
        self.assertEqual(decoded, data)
        self.assertEqual(census.dynamic, 6)

    def test_model_rejects_distance_beyond_the_ring(self) -> None:
        first = self.rng.randbytes(rp.BLOCK)
        second = bytearray(self.rng.randbytes(rp.BLOCK))
        second[300:500] = first[0:200]             # a match 1324 bytes back
        data = first + bytes(second)
        c = zlib.compressobj(9, zlib.DEFLATED, -15, 9, zlib.Z_FIXED)
        wide = [(c.compress(first) + c.flush(zlib.Z_SYNC_FLUSH), False),
                (c.compress(bytes(second)) + c.flush(zlib.Z_FINISH), True)]
        self.assertEqual(rp.decode_deflate_pieces(wide, len(data), True).data, data)   # zlib is happy
        with self.assertRaisesRegex(rp.RepackError, "exceeds the 0x400 ring"):
            rp.model_decode_pieces(wide, len(data), True)
        narrow = rp.encode_deflate_pieces(data)                                        # the tool's window
        self.assertEqual(rp.model_decode_pieces(narrow.pieces, len(data), True)[0], data)

    def test_model_accepts_matches_into_the_previous_full_piece(self) -> None:
        first = self.rng.randbytes(rp.BLOCK)
        second = first[:600] + self.rng.randbytes(rp.BLOCK - 600)   # distance exactly 0x400 at pos 0
        data = first + second
        encoded = rp.encode_deflate_pieces(data)
        decoded, _ = rp.model_decode_pieces(encoded.pieces, len(data), True)
        self.assertEqual(decoded, data)

    def test_model_rejects_unaligned_piece_boundary(self) -> None:
        data = compressible(self.rng, 3000)
        pieces = rp.encode_deflate_pieces(data).pieces
        moved = [(pieces[0][0][:-1], False), (pieces[0][0][-1:] + pieces[1][0], False), pieces[2]]
        with self.assertRaises(rp.RepackError):
            rp.model_decode_pieces(moved, len(data), True)
        with self.assertRaises(rp.RepackError):
            rp.decode_deflate_pieces(moved, len(data), True)

    def test_model_huffman_rules(self) -> None:
        with self.assertRaises(rp.RepackError):
            rp.HuffmanTable([1, 1, 1])                       # over-subscribed
        with self.assertRaises(rp.RepackError):
            rp.HuffmanTable([2, 2, 2])                       # incomplete with > 1 symbol
        rp.HuffmanTable([1, 0, 0])                           # single symbol: tinfl accepts
        literal, distance = rp.fixed_tables()
        self.assertEqual(literal.fast[0] >> 9, 7)            # code 0000000 -> symbol 256
        self.assertEqual(literal.fast[0] & 511, 256)
        self.assertEqual(distance.fast[0], (5 << 9) | 0)

    # -- checksum and keystream ---------------------------------------------
    def test_mount_checksum_buffer_quirk(self) -> None:
        data = bytes(range(256)) * 2 + b"\x11\x22\x33"      # 0x203 bytes: second chunk is 3 bytes
        # independent emulation of the 0x200 stack buffer that keeps the previous chunk's bytes
        buffer = bytearray(0x200)
        h = 0xABABEB98
        for start in range(0, len(data), 0x200):
            chunk = data[start:start + 0x200]
            buffer[:len(chunk)] = chunk
            for i in range((len(chunk) + 3) // 4):
                word, = struct.unpack_from("<I", buffer, i * 4)
                h = (((h >> 1) | ((h & 1) << 31)) + word) & 0xFFFFFFFF
        self.assertEqual(rp.mount_checksum(data), h)
        # zero padding would give a different value: the quirk is real
        padded = data + b"\x00"
        zero = 0xABABEB98
        for i in range(len(padded) // 4):
            word, = struct.unpack_from("<I", padded, i * 4)
            zero = (((zero >> 1) | ((zero & 1) << 31)) + word) & 0xFFFFFFFF
        self.assertNotEqual(rp.mount_checksum(data), zero)
        self.assertEqual(rp.mount_checksum(b""), 0xABABEB98)

    def test_keystream_word_boundary_and_roundtrip(self) -> None:
        data = self.rng.randbytes(5 * rp.BLOCK + 17)
        pieces = rp.encode_stored_pieces(data, 0xDEADBEEF)
        self.assertEqual(len(pieces), 6)
        self.assertTrue(all(len(p) == rp.BLOCK for p, final in pieces if not final))
        self.assertEqual(rp.decode_stored_pieces(pieces, 0xDEADBEEF)[:len(data)], data)
        one = rp.IsaacKeystream(7).apply(bytes(1028))
        two = rp.IsaacKeystream(7)
        split = two.apply(bytes(1024)) + two.apply(bytes(4))
        self.assertEqual(one, split)                      # word 257 comes from the regenerated block
        self.assertNotEqual(one[:1024], one[4:1028])
        with self.assertRaises(rp.RepackError):
            rp.encode_stored_pieces(b"x" * 100, 1)

    # -- container guards -----------------------------------------------------
    def test_refusals(self) -> None:
        source = self.tmp / "afterbirth.a"
        build_stock_like_archive(source, [("resources/x.txt", compressible(self.rng, 3000))])
        with self.assertRaisesRegex(rp.RepackError, "receipt"):
            rp.repack_archive(source, self.tmp / "out.a")
        rp.repack_archive(source, self.tmp / "out.a", allow_receipt_archive=True)
        with self.assertRaisesRegex(rp.RepackError, "in place"):
            rp.repack_archive(source, source, allow_receipt_archive=True)
        other = self.tmp / "other.a"
        other.write_bytes(rp.HEADER.pack(rp.MAGIC, 0, 14, 0))
        with self.assertRaisesRegex(rp.RepackError, "mode-0"):
            rp.repack_archive(other, self.tmp / "other.out.a")
        broken = self.tmp / "broken.a"
        broken.write_bytes(b"ARCH000\x02" + struct.pack("<IH", 99, 1))
        with self.assertRaises(rp.RepackError):
            rp.Archive(broken)

    def test_repack_dir_copies_non_miniz(self) -> None:
        source_dir = self.tmp / "packed"
        source_dir.mkdir()
        build_stock_like_archive(source_dir / "repentance.a", [("resources/x.txt", compressible(self.rng, 3000))])
        (source_dir / "animations.a").write_bytes(rp.HEADER.pack(rp.MAGIC, 1, 14, 0))
        (source_dir / "notes.txt").write_bytes(b"keep")
        code = rp.main(["repack-dir", str(source_dir), str(self.tmp / "out"), "--verify", "zlib"])
        self.assertEqual(code, 0)
        self.assertEqual((self.tmp / "out" / "animations.a").read_bytes(), (source_dir / "animations.a").read_bytes())
        self.assertEqual((self.tmp / "out" / "notes.txt").read_bytes(), b"keep")
        with rp.Archive(self.tmp / "out" / "repentance.a") as b:
            self.assertEqual(b.count, 1)

    def test_repack_dir_copies_only_sidecars_that_still_match(self) -> None:
        # isaac_vita_halve_sfx.py pins an archive's bytes in "<archive>.receipt";
        # the sidecar of an archive this run changed is stale and must not follow
        # it, the sidecar of an archive copied unchanged still matches.
        source_dir = self.tmp / "packed"
        source_dir.mkdir()
        build_stock_like_archive(source_dir / "repentance.a", [("resources/x.txt", compressible(self.rng, 3000))])
        (source_dir / "repentance.a.receipt").write_bytes(b"R" * 256)
        (source_dir / "animations.a").write_bytes(rp.HEADER.pack(rp.MAGIC, 1, 14, 0))
        (source_dir / "animations.a.receipt").write_bytes(b"A" * 256)
        (source_dir / "notes.receipt.txt").write_bytes(b"not a sidecar")
        out = self.tmp / "out"
        code = rp.main(["repack-dir", str(source_dir), str(out), "--verify", "zlib"])
        self.assertEqual(code, 0)
        self.assertNotEqual((out / "repentance.a").read_bytes(), (source_dir / "repentance.a").read_bytes())
        self.assertFalse((out / "repentance.a.receipt").exists())
        self.assertEqual((out / "animations.a.receipt").read_bytes(), b"A" * 256)
        self.assertEqual((out / "notes.receipt.txt").read_bytes(), b"not a sidecar")
        # repacking the repack reproduces its bytes, so a sidecar written over
        # them (what "repack first, then halve" produces) is still valid: kept
        (out / "repentance.a.receipt").write_bytes(b"S" * 256)
        again = self.tmp / "again"
        code = rp.main(["repack-dir", str(out), str(again), "--verify", "zlib"])
        self.assertEqual(code, 0)
        self.assertEqual((again / "repentance.a").read_bytes(), (out / "repentance.a").read_bytes())
        self.assertEqual((again / "repentance.a.receipt").read_bytes(), b"S" * 256)

    def test_verify_alone_reports_the_sizes_it_compared(self) -> None:
        source = self.tmp / "s.a"
        output = self.tmp / "o.a"
        build_stock_like_archive(source, [("resources/x.txt", compressible(self.rng, 5000))])
        rp.repack_archive(source, output, verify="off")
        stats = rp.verify_repack(source, output, level="zlib")
        self.assertEqual(stats.original_size, source.stat().st_size)
        self.assertEqual(stats.repacked_size, output.stat().st_size)
        self.assertIn("size %d -> %d bytes" % (source.stat().st_size, output.stat().st_size), stats.summary())

    def test_verify_detects_a_corrupted_piece(self) -> None:
        source = self.tmp / "s.a"
        output = self.tmp / "o.a"
        build_stock_like_archive(source, [("resources/x.txt", compressible(self.rng, 5000))])
        rp.repack_archive(source, output, verify="model")
        raw = bytearray(output.read_bytes())
        raw[14 + 4 + 5] ^= 0x40                          # inside the first deflate piece
        output.write_bytes(raw)
        with self.assertRaises(rp.RepackError):
            rp.verify_repack(source, output, level="model")

    # -- the runtime's own decoder -------------------------------------------
    def test_native_tinfl_decodes_the_repack(self) -> None:
        native = native_tinfl()
        if native is None:
            self.skipTest("no host C compiler for recomp/runtime/host_vita_archive_miniz_native.c")
        source = self.tmp / "n.a"
        output = self.tmp / "n.repacked.a"
        build_stock_like_archive(source, self.corpus())
        rp.repack_archive(source, output, verify="model")
        self.assertEqual(native_check_archive(native, source, output), 7)
        # the wide-window stream the model rejects also breaks the real decoder's ring
        first = self.rng.randbytes(rp.BLOCK)
        second = bytearray(self.rng.randbytes(rp.BLOCK))
        second[300:500] = first[0:200]
        c = zlib.compressobj(9, zlib.DEFLATED, -15, 9, zlib.Z_FIXED)
        wide = [(c.compress(first) + c.flush(zlib.Z_SYNC_FLUSH), False),
                (c.compress(bytes(second)) + c.flush(zlib.Z_FINISH), True)]
        got = native.decode_prefix(wide, 2 * rp.BLOCK, True)
        self.assertNotEqual(got, first + bytes(second))

    # -- real archives --------------------------------------------------------
    def test_real_archive_smoke(self) -> None:
        archives = [p for p in find_real_archives() if p.name.lower() not in rp.RECEIPT_ARCHIVES]
        candidates = []
        for path in archives:
            try:
                with rp.Archive(path) as archive:
                    if archive.mode == rp.MODE_MINIZ and archive.count:
                        candidates.append((path.stat().st_size, path))
            except rp.RepackError:
                continue
        if not candidates:
            self.skipTest("no mode-2 game archive found (set ISAAC_VITA_PACKED_DIR)")
        candidates.sort()
        limit = int(os.environ.get("ISAAC_VITA_REPACK_SMOKE_MAX_BYTES", str(64 * 1024 * 1024)))
        chosen = [p for size, p in candidates if size <= limit] or [candidates[0][1]]
        path = chosen[-1]
        output = self.tmp / (path.stem + ".repacked.a")
        stats = rp.repack_archive(path, output, verify="full", verify_stored=True)
        self.assertEqual(stats.passthrough_entries, 0, stats.warnings)
        assert stats.repacked_census is not None and stats.original_census is not None
        self.assertEqual(stats.repacked_census["dynamic"], 0)
        self.assertEqual(stats.dynamic_to_fixed, stats.original_census["dynamic"])
        self.assertEqual(stats.checksums_verified, stats.deflate_entries + stats.mixed_entries + stats.stored_entries)
        native = native_tinfl()
        if native is not None:
            self.assertEqual(native_check_archive(native, path, output), stats.deflate_entries + stats.mixed_entries)
        print("\nreal-archive smoke:\n" + stats.summary())


if __name__ == "__main__":
    unittest.main()
