#!/usr/bin/env python3
"""Tests for isaac_vita_halve_sfx: synthetic tones through the WAV, archive
and tree drivers, header correctness, pass/skip decisions, and the ARCH000
codec round trip.  Set ISAAC_VITA_SFX_ARCHIVE=<path to a real packed .a> to
also verify the codec against every entry of a shipped archive."""

from __future__ import annotations

import hashlib
import io
import os
import random
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import numpy as np  # noqa: E402

import isaac_vita_halve_sfx as H  # noqa: E402


def tone(freq: float, rate: int = 44100, seconds: float = 0.5, amp: float = 12000.0, phase: float = 0.0):
    t = np.arange(int(rate * seconds)) / rate
    return np.rint(amp * np.sin(2 * np.pi * freq * t + phase)).astype("<i2")


def wav_bytes(channels, rate: int, bits: int = 16, fmt_tag: int = 1, fmt_extra: bytes | None = None,
              before: bytes = b"", after: bytes = b"", block_align: int | None = None,
              byte_rate: int | None = None) -> bytes:
    """A RIFF/WAVE around int16 channel arrays (tuple of 1 or 2), with optional
    foreign chunks before fmt and after data.  fmt_extra=b'' gives fmt len 18."""
    frames = len(channels[0])
    pcm = np.stack(channels, axis=1).astype("<i2").tobytes()
    align = block_align if block_align is not None else len(channels) * bits // 8
    brate = byte_rate if byte_rate is not None else rate * align
    fmt = struct.pack("<HHIIHH", fmt_tag, len(channels), rate, brate, align, bits)
    if fmt_extra is not None:
        fmt += struct.pack("<H", len(fmt_extra)) + fmt_extra
    body = before + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(pcm)) + pcm + after
    return b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body


def read_bytes(path: str) -> bytes:
    with open(path, "rb") as fh:
        return fh.read()


def write_bytes(path: str, data: bytes) -> None:
    with open(path, "wb") as fh:
        fh.write(data)


def chunk(cid: bytes, payload: bytes) -> bytes:
    return cid + struct.pack("<I", len(payload)) + payload + (b"\0" if len(payload) & 1 else b"")


class Args:
    def __init__(self, **kw):
        self.threshold = H.DEFAULT_THRESHOLD
        self.min_rate = H.DEFAULT_MIN_RATE
        self.dry_run = False
        self.measure = False
        self.report = None
        self.verbose = False
        self.encode = "stored"
        self.prefix = None
        self.catalogue = None
        self.copy_unchanged = False
        self.include_afterbirth = False
        self.sidecar = True
        self.__dict__.update(kw)


class FilterTest(unittest.TestCase):
    def test_kaiser_lowpass_shape(self):
        h = H.kaiser_lowpass()
        self.assertEqual(len(h), 127)
        self.assertAlmostEqual(float(h.sum()), 1.0, places=9)
        # symmetric (linear phase)
        self.assertTrue(np.allclose(h, h[::-1]))
        # response: flat below 0.4 of the old Nyquist, > 60 dB down above 0.55
        w, resp = np.linspace(0, 1, 2001), None
        n = np.arange(127) - 63
        resp = np.array([abs(np.sum(h * np.exp(-1j * np.pi * f * n))) for f in w])
        self.assertGreater(resp[w <= 0.40].min(), 10 ** (-0.5 / 20))
        self.assertLess(resp[w >= 0.55].max(), 10 ** (-60 / 20))


class WavDecisionTest(unittest.TestCase):
    def test_low_tone_is_halved_and_header_is_right(self):
        before = chunk(b"JUNK", b"\0" * 28)
        after = chunk(b"LIST", b"INFOISFT" + struct.pack("<I", 4) + b"abc\0")
        src = wav_bytes((tone(440.0),), 44100, fmt_extra=b"", before=before, after=after)
        d = H.decide(src)
        self.assertEqual(d.action, "halved", d.reason)
        assert d.share is not None
        self.assertLess(d.share, 0.001)
        out = d.output
        assert out is not None
        info = H.parse_wav(out)
        self.assertEqual(info.rate, 22050)
        self.assertEqual(info.byte_rate, 44100)
        self.assertEqual(info.block_align, 2)
        self.assertEqual(info.channels, 1)
        self.assertEqual(info.bits, 16)
        self.assertEqual(info.fmt_len, 18)
        self.assertEqual(info.frames, (22050 + 1) // 2)
        self.assertEqual(info.data_len, info.frames * 2)
        self.assertEqual(info.riff_size, len(out) - 8)
        # chunks before fmt and after data are byte-identical, order kept
        self.assertEqual(out[12:12 + len(before)], before)
        self.assertEqual(out[-len(after):], after)
        self.assertEqual([c[0] for c in info.chunks], [b"JUNK", b"fmt ", b"data", b"LIST"])
        # the tone survives: same length in seconds, same level
        y = H.pcm_frames(out, info)[:, 0].astype(np.float64)
        self.assertAlmostEqual(float(np.abs(y).max()) / 12000.0, 1.0, delta=0.02)
        # and it is still a 440 Hz tone
        spec = np.abs(np.fft.rfft(y * np.hanning(len(y))))
        f = np.fft.rfftfreq(len(y), 1.0 / 22050)
        self.assertAlmostEqual(f[spec.argmax()], 440.0, delta=5.0)

    def test_bright_tone_is_left_alone(self):
        src = wav_bytes((tone(15000.0),), 44100)
        d = H.decide(src)
        self.assertEqual(d.action, "skipped")
        assert d.share is not None
        self.assertGreater(d.share, 0.9)
        self.assertIsNone(d.output)
        self.assertEqual(d.before, d.after)

    def test_threshold_boundary(self):
        # a 440 Hz tone with a small 15 kHz component: 1 % of the energy
        low = tone(440.0, amp=10000.0).astype(np.float64)
        high = tone(15000.0, amp=1000.0).astype(np.float64)
        mix = np.rint(low + high).astype("<i2")
        src = wav_bytes((mix,), 44100)
        share = H.hf_share(mix.reshape(-1, 1), 44100)
        self.assertAlmostEqual(share, 0.01, delta=0.002)
        self.assertEqual(H.decide(src, threshold=0.005).action, "skipped")
        self.assertEqual(H.decide(src, threshold=0.02).action, "halved")

    def test_stereo(self):
        src = wav_bytes((tone(440.0), tone(1000.0, phase=1.0)), 44100)
        d = H.decide(src)
        self.assertEqual(d.action, "halved", d.reason)
        assert d.output is not None
        info = H.parse_wav(d.output)
        self.assertEqual((info.channels, info.rate, info.block_align, info.byte_rate), (2, 22050, 4, 88200))
        y = H.pcm_frames(d.output, info)
        self.assertEqual(y.shape, ((22050 + 1) // 2, 2))
        for c, freq in ((0, 440.0), (1, 1000.0)):
            spec = np.abs(np.fft.rfft(y[:, c].astype(np.float64) * np.hanning(len(y))))
            f = np.fft.rfftfreq(len(y), 1.0 / 22050)
            self.assertAlmostEqual(f[spec.argmax()], freq, delta=5.0)
        # one bright channel is enough to keep the pair
        src = wav_bytes((tone(440.0), tone(15000.0)), 44100)
        self.assertEqual(H.decide(src).action, "skipped")

    def test_odd_frame_count_and_48k(self):
        src = wav_bytes((tone(440.0, rate=48000, seconds=0.31)[:14401],), 48000)
        d = H.decide(src)
        self.assertEqual(d.action, "halved", d.reason)
        assert d.output is not None
        info = H.parse_wav(d.output)
        self.assertEqual(info.rate, 24000)
        self.assertEqual(info.frames, 7201)

    def test_non_candidates_are_skipped(self):
        low = tone(440.0)
        cases = {
            "8-bit": wav_bytes((low,), 44100, bits=8, block_align=1, byte_rate=44100),
            "float": wav_bytes((low,), 44100, fmt_tag=3),
            "extensible": wav_bytes((low,), 44100, fmt_tag=0xFFFE, fmt_extra=b"\0" * 22),
            "22050 already": wav_bytes((tone(440.0, rate=22050),), 22050),
            "too short": wav_bytes((low[:100],), 44100),
            "bad byte rate": wav_bytes((low,), 44100, byte_rate=1),
            "bad block align": wav_bytes((low,), 44100, block_align=4),
        }
        for name, src in cases.items():
            d = H.decide(src)
            self.assertEqual(d.action, "skipped", name)
            self.assertIsNone(d.output, name)
        for junk in (b"", b"RIFF\0\0\0\0WAVX", b"OggS" + b"\0" * 60):
            self.assertEqual(H.decide(junk).action, "skipped")

    def test_engine_walk_disagreement_is_skipped(self):
        # an odd-length chunk before fmt without its pad byte: a standards
        # walker and sub_005bf4e0 land on different bytes -> hands off
        odd = b"JUNK" + struct.pack("<I", 3) + b"\0\0\0"
        src = wav_bytes((tone(440.0),), 44100, before=odd)
        d = H.decide(src)
        self.assertEqual(d.action, "skipped")
        # padded, the engine still lands on the pad byte: hands off as well
        src = wav_bytes((tone(440.0),), 44100, before=chunk(b"JUNK", b"\0\0\0"))
        self.assertEqual(H.decide(src).action, "skipped")
        # a foreign chunk AFTER data is skipped by the engine's second walk
        # (its length is added to the cursor) and is carried over byte for byte
        tail = chunk(b"cue ", b"\0" * 5)
        src = wav_bytes((tone(440.0),), 44100, after=tail)
        d = H.decide(src)
        self.assertEqual(d.action, "halved")
        assert d.output is not None
        self.assertEqual(d.output[-len(tail):], tail)
        assert d.info is not None
        self.assertEqual([c[0] for c in d.info.chunks], [b"fmt ", b"data", b"cue "])
        # fmt length 18 with a non-zero cbSize is walked by cbSize by the engine
        src = wav_bytes((tone(440.0),), 44100, fmt_extra=b"\0\0")
        self.assertEqual(H.decide(src).action, "halved")

    def test_second_fmt_or_data_chunk_is_refused(self):
        # the engine's second walk (0x5bf5d9) continues after data: a later
        # fmt chunk would be copied over the halved header (original rate ->
        # pitch doubled), a later data chunk read again into the same block
        good = wav_bytes((tone(440.0),), 44100)
        fmt16 = struct.pack("<HHIIHH", 1, 1, 44100, 88200, 2, 16)
        src = wav_bytes((tone(440.0),), 44100, after=chunk(b"fmt ", fmt16))
        d = H.decide(src)
        self.assertEqual(d.action, "skipped")
        self.assertIn("2 fmt chunks", d.reason)
        src = wav_bytes((tone(440.0),), 44100, after=chunk(b"data", b"\0" * 8))
        d = H.decide(src)
        self.assertEqual(d.action, "skipped")
        self.assertIn("2 data chunks", d.reason)
        # a second fmt BEFORE data is refused as well (the walk copies both)
        src = wav_bytes((tone(440.0),), 44100, before=chunk(b"fmt ", fmt16))
        self.assertEqual(H.decide(src).action, "skipped")
        self.assertEqual(H.decide(good).action, "halved")
        self.assertEqual(H._engine_walk(src)[3:], (2, 1))
        self.assertEqual(H._engine_walk(good)[3:], (1, 1))

    def test_measure_reports_small_error(self):
        src = wav_bytes((tone(440.0),), 44100)
        d = H.decide(src, want_measure=True)
        assert d.metrics is not None
        self.assertLess(d.metrics["band_error"], 0.01)
        self.assertLess(d.metrics["peak_change"], 0.02)
        self.assertLess(d.metrics["band_error_linear"], 0.02)

    def test_dry_run_predicts_size(self):
        src = wav_bytes((tone(440.0),), 44100)
        d = H.decide(src, dry_run=True)
        self.assertEqual(d.action, "halved")
        self.assertIsNone(d.output)
        self.assertEqual(d.after, ((22050 + 1) // 2) * 2)


class CodecTest(unittest.TestCase):
    def test_hashes_fold(self):
        self.assertEqual(H.key_of("resources/SFX/A\\b.wav"), H.key_of("resources/sfx/a/b.wav"))
        self.assertNotEqual(H.key_of("resources/x"), H.key_of("resources-dlc3/x"))

    def test_mount_checksum_chunk_tail_quirk(self):
        # the tail of a final partial dword keeps the previous chunk's bytes
        a = bytes(range(256)) * 2 + b"\x01"
        b = bytes(range(256)) * 2 + b"\x01\x00\x00\x00"
        self.assertNotEqual(H.mount_checksum(a), H.mount_checksum(b))
        self.assertEqual(H.mount_checksum(b""), H.CHECKSUM_INIT)

    def test_raw_round_trip(self):
        rnd = random.Random(7)
        for n in (0, 1, 3, 4, 5, 1023, 1024, 4097):
            data = bytes(rnd.getrandbits(8) for _ in range(n))
            h2 = rnd.getrandbits(32)
            enc = H.raw_encode(data, h2)
            self.assertEqual(len(enc) % 4, 0)
            self.assertEqual(H.raw_decode(enc, h2, n), data)

    def test_keystream_is_deterministic_involution(self):
        data = os.urandom(3000)
        a = H.IsaacKeystream(0x12345678).apply(data)
        b = H.IsaacKeystream(0x12345678).apply(a)
        self.assertEqual(b, data)
        self.assertNotEqual(a, data)
        self.assertNotEqual(H.IsaacKeystream(1).apply(data), H.IsaacKeystream(2).apply(data))
        # piecewise application equals one shot
        ks = H.IsaacKeystream(0x12345678)
        self.assertEqual(ks.apply(data[:1024]) + ks.apply(data[1024:2048]) + ks.apply(data[2048:]), a)

    def test_miniz_modes_round_trip(self):
        rnd = random.Random(3)
        for n in (1, 1023, 1024, 1025, 5000, 70000):
            data = bytes(rnd.getrandbits(8) for _ in range(n)) if n < 2000 else (b"abc" * (n // 3 + 1))[:n]
            h2 = rnd.getrandbits(32)
            for mode in ("stored", "deflate", "auto"):
                enc = H.miniz_encode(data, h2, mode)
                dec, raw_len = H.miniz_decode(enc, 0, n, h2)
                self.assertEqual(dec, data, (n, mode))
                self.assertEqual(raw_len, len(enc), (n, mode))
                # piece rules the reader relies on
                for payload, fin, _end in H.iter_pieces(enc, 0, n):
                    self.assertLessEqual(len(payload), H.PIECE_MAX)
                if mode == "stored" and n > H.BLOCK:
                    first, fin, _ = next(H.iter_pieces(enc, 0, n))
                    self.assertEqual((len(first), fin), (H.BLOCK, 0))
                if mode == "deflate":
                    for payload, fin, _end in H.iter_pieces(enc, 0, n):
                        self.assertTrue(fin or len(payload) != H.BLOCK)

    def test_write_and_read_archive_both_versions(self):
        items = [
            {"h1": 1, "h2": 2, "data": b"hello"},
            {"h1": 3, "h2": 4, "data": os.urandom(3000)},
            {"h1": 5, "h2": 6, "data": b""},
        ]
        with tempfile.TemporaryDirectory() as td:
            for version in (0, 2):
                path = os.path.join(td, "v%d.a" % version)
                r = H.write_archive(path, version, items)
                self.assertEqual(r["entries"], 3)
                a = H.Archive(path)
                self.assertEqual(a.version, version)
                self.assertEqual(os.path.getsize(path), r["size"])
                for it, e in zip(items, a.entries):
                    self.assertEqual((e.h1, e.h2, e.size), (it["h1"], it["h2"], len(it["data"])))
                    self.assertEqual(a.decode(e), it["data"])
                    self.assertTrue(a.verify(e))
                    self.assertEqual(a.decode_prefix(e, 4), it["data"][:4])


def sounds_xml(paths) -> bytes:
    body = "".join('\t<sound id="%d">\n\t\t<sample weight="1" path="%s" />\n\t</sound>\n' % (i + 1, p)
                   for i, p in enumerate(paths))
    return ('<sounds root="sfx/">\n<!-- <sample path="ghost.wav" /> -->\n%s</sounds>\n' % body).encode()


def make_archive(path: str, prefix: str, version: int = 2) -> dict:
    """An archive with a sounds.xml, a quiet tone, a bright tone, a non-catalogue
    WAV and a non-WAV blob.  Returns the decoded bytes by key path."""
    quiet = wav_bytes((tone(440.0, seconds=0.2),), 44100)
    bright = wav_bytes((tone(15000.0, seconds=0.2),), 44100)
    stray = wav_bytes((tone(440.0, seconds=0.2),), 44100, after=chunk(b"id3 ", b"x" * 10))
    blob = bytes(range(256)) * 9
    files = {
        prefix + "sounds.xml": sounds_xml(["Quiet/tone.wav", "Bright/tone.wav", "Also/QUIET\\tone.wav"]),
        prefix + "sfx/Quiet/tone.wav": quiet,
        prefix + "sfx/Bright/tone.wav": bright,
        prefix + "sfx/also/quiet/tone.wav": quiet,
        prefix + "sfx/stray.wav": stray,
        prefix + "gfx/blob.bin": blob,
    }
    items = []
    for p, data in files.items():
        h1, h2 = H.key_of(p)
        items.append({"h1": h1, "h2": h2, "data": data})
    H.write_archive(path, version, items, "auto")
    return files


class ArchiveDriverTest(unittest.TestCase):
    def check_rewrite(self: unittest.TestCase, src_path, out_path, files, prefix):
        # Shared with TreeAndWavDriverTest, hence typed against the base class.
        a = H.Archive(src_path)
        b = H.Archive(out_path)
        self.assertEqual(a.version, b.version)
        self.assertEqual([e.key for e in a.entries], [e.key for e in b.entries])
        for ea, eb in zip(a.entries, b.entries):
            self.assertTrue(b.verify(eb), eb.tag)
        halved_keys = {H.key_of(prefix + "sfx/Quiet/tone.wav"), H.key_of(prefix + "sfx/also/quiet/tone.wav")}
        for ea, eb in zip(a.entries, b.entries):
            if ea.key in halved_keys:
                self.assertLess(eb.size, ea.size)
                info = H.parse_wav(b.decode(eb))
                self.assertEqual(info.rate, 22050)
                self.assertEqual(b.decode(eb), H.decide(a.decode(ea)).output)
                first, fin, _ = next(H.iter_pieces(b.buf, eb.offset, eb.size))
                self.assertEqual((len(first), fin), (H.BLOCK, 0))   # stored mode
            else:
                self.assertEqual(a.raw(ea), b.raw(eb), "untouched entry must be raw-identical")
                self.assertEqual((ea.size, ea.x), (eb.size, eb.x))

    def test_archive_command(self):
        with tempfile.TemporaryDirectory() as td:
            src = os.path.join(td, "in.a")
            out = os.path.join(td, "out.a")
            report = os.path.join(td, "r.json")
            files = make_archive(src, "resources/")
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = H.main(["archive", src, out, "--report", report, "--measure"])
            self.assertEqual(rc, 0)
            text = buf.getvalue()
            self.assertIn("2 halved, 1 skipped", text)
            self.assertIn("1 WAV entries are not in any sounds.xml", text)
            self.check_rewrite(src, out, files, "resources/")
            import json
            with open(report, encoding="utf-8") as fh:
                rep = json.load(fh)
            self.assertEqual(rep["summary"]["halved"], 2)
            names = sorted(f["file"] for f in rep["files"])
            self.assertEqual(names, ["sfx/Also/QUIET/tone.wav", "sfx/Bright/tone.wav", "sfx/Quiet/tone.wav"])
            # never in place
            with self.assertRaises(SystemExit):
                H.main(["archive", src, src])

    def test_game_log_subset(self):
        # sounds.xml of make_archive: id 1 Quiet, id 2 Bright, id 3 Also/QUIET
        with tempfile.TemporaryDirectory() as td:
            src = os.path.join(td, "in.a")
            out = os.path.join(td, "out.a")
            log = os.path.join(td, "log.txt")
            report = os.path.join(td, "r.json")
            make_archive(src, "resources/")
            write_bytes(log, b"[INFO] - Preload sound 1.\n[INFO] - [warn] Sound 2 was not preloaded.\n"
                             b"[INFO] - Preload sound 1.\n[INFO] - Preload sound 99.\n[INFO] - Preload sound 10.\n")
            self.assertEqual(H.game_log_sound_ids(log), [1, 99, 10, 2])
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(H.main(["archive", src, out, "--game-log", log, "--report", report, "--dry-run"]), 0)
            self.assertIn("log.txt: 4 sound ids -> 2 files (1 halved, 1 kept, 0 not in these archives, "
                          "2 ids not in sounds.xml)", buf.getvalue())
            import json
            with open(report, encoding="utf-8") as fh:
                g = json.load(fh)["summary"]["game_logs"][0]
            quiet = H.parse_wav(wav_bytes((tone(440.0, seconds=0.2),), 44100))
            self.assertEqual((g["files"], g["halved"], g["kept"], g["unknown_ids"]), (2, 1, 1, 2))
            self.assertEqual(g["pcm_before"], 2 * quiet.data_len)
            self.assertEqual(g["pcm_after"], quiet.data_len + ((quiet.frames + 1) // 2) * 2)
            # the DLC catalogue overrides an older one for the same id
            ids = {}
            H.merge_catalogue_index(ids, sounds_xml(["old.wav"]), False)
            H.merge_catalogue_index(ids, sounds_xml(["new.wav"]), True)
            H.merge_catalogue_index(ids, sounds_xml(["older.wav"]), False)
            self.assertEqual(ids, {1: ["sfx/new.wav"]})

    def test_dlc3_prefix_and_external_catalogue(self):
        with tempfile.TemporaryDirectory() as td:
            src = os.path.join(td, "repentance.a")
            out = os.path.join(td, "out.a")
            files = make_archive(src, "resources-dlc3/")
            with redirect_stdout(io.StringIO()):
                self.assertEqual(H.main(["archive", src, out]), 0)
            self.check_rewrite(src, out, files, "resources-dlc3/")
            # an archive without its own catalogue takes --catalogue
            src2 = os.path.join(td, "nocat.a")
            quiet = wav_bytes((tone(440.0, seconds=0.2),), 44100)
            h1, h2 = H.key_of("resources/sfx/Quiet/tone.wav")
            H.write_archive(src2, 2, [{"h1": h1, "h2": h2, "data": quiet}])
            with redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit):
                    H.main(["archive", src2, os.path.join(td, "o2.a")])
                self.assertEqual(H.main(["archive", src2, os.path.join(td, "o2.a"), "--catalogue", src]), 0)
            b = H.Archive(os.path.join(td, "o2.a"))
            self.assertEqual(H.parse_wav(b.decode(b.entries[0])).rate, 22050)

    def test_v0_archive_is_rewritten_raw(self):
        with tempfile.TemporaryDirectory() as td:
            src = os.path.join(td, "in.a")
            out = os.path.join(td, "out.a")
            make_archive(src, "resources/", version=0)
            with redirect_stdout(io.StringIO()):
                self.assertEqual(H.main(["archive", src, out]), 0)
            b = H.Archive(out)
            self.assertEqual(b.version, 0)
            for e in b.entries:
                self.assertTrue(b.verify(e))
            e = b.by_key[H.key_of("resources/sfx/Quiet/tone.wav")]
            self.assertEqual(H.parse_wav(b.decode(e)).rate, 22050)

    def test_dry_run_writes_nothing(self):
        with tempfile.TemporaryDirectory() as td:
            src = os.path.join(td, "in.a")
            out = os.path.join(td, "out.a")
            make_archive(src, "resources/")
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(H.main(["archive", src, out, "--dry-run"]), 0)
            self.assertFalse(os.path.exists(out))
            self.assertIn("DRY RUN: 2 halved", buf.getvalue())


class TreeAndWavDriverTest(unittest.TestCase):
    def test_tree_command(self):
        with tempfile.TemporaryDirectory() as td:
            res = os.path.join(td, "resources")
            packed = os.path.join(res, "packed")
            os.makedirs(packed)
            files = make_archive(os.path.join(packed, "afterbirth.a"), "resources/")
            make_archive(os.path.join(packed, "repentance.a"), "resources-dlc3/")
            # an archive without WAVs, version 0
            H.write_archive(os.path.join(packed, "graphics.a"), 0,
                            [{"h1": 9, "h2": 9, "data": b"\x89PNG" + b"\0" * 100}])
            # a version-1 (LZW) archive must pass through untouched
            with open(os.path.join(packed, "old.a"), "wb") as fh:
                fh.write(H.ARCHIVE_MAGIC + bytes([1]) + struct.pack("<IH", H.HEADER_SIZE, 0))
            with open(os.path.join(res, "loose.txt"), "wb") as fh:
                fh.write(b"loose")
            out = os.path.join(td, "out")
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(H.main(["tree", res, out, "--include-afterbirth"]), 0)
            text = buf.getvalue()
            self.assertIn("catalogue: 3 unique sample paths", text)
            self.assertIn("4 halved, 2 skipped", text)
            self.assertIn("old.a: archive version 1 (LZW)", text)
            self.assertIn("and their .receipt sidecars", text)
            self.assertEqual(sorted(os.listdir(os.path.join(out, "packed"))),
                             ["afterbirth.a", "afterbirth.a.receipt", "repentance.a", "repentance.a.receipt"])
            ArchiveDriverTest.check_rewrite(self, os.path.join(packed, "afterbirth.a"),
                                            os.path.join(out, "packed", "afterbirth.a"), files, "resources/")
            for n in ("afterbirth.a", "repentance.a"):
                self.assertIsNone(H.sidecar_check(read_bytes(os.path.join(out, "packed", n)),
                                                  read_bytes(os.path.join(out, "packed", n + ".receipt")), n))
            # the parent of resources/ is accepted too, and --copy-unchanged completes the tree
            out2 = os.path.join(td, "out2")
            with redirect_stdout(io.StringIO()):
                self.assertEqual(H.main(["tree", td, out2, "--copy-unchanged", "--include-afterbirth",
                                         "--no-sidecar"]), 0)
            self.assertEqual(sorted(os.listdir(os.path.join(out2, "packed"))),
                             ["afterbirth.a", "graphics.a", "old.a", "repentance.a"])
            self.assertEqual(read_bytes(os.path.join(out2, "loose.txt")), b"loose")
            self.assertEqual(read_bytes(os.path.join(out2, "packed", "graphics.a")),
                             read_bytes(os.path.join(packed, "graphics.a")))
            # in place refused
            with self.assertRaises(SystemExit):
                H.main(["tree", res, os.path.join(res, "x")])

    def test_afterbirth_is_excluded_by_default(self):
        with tempfile.TemporaryDirectory() as td:
            res = os.path.join(td, "resources")
            packed = os.path.join(res, "packed")
            os.makedirs(packed)
            make_archive(os.path.join(packed, "afterbirth.a"), "resources/")
            make_archive(os.path.join(packed, "sfx.a"), "resources/", version=0)
            out = os.path.join(td, "out")
            report = os.path.join(td, "r.json")
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(H.main(["tree", res, out, "--report", report]), 0)
            text = buf.getvalue()
            self.assertIn("afterbirth.a: left untouched", text)
            self.assertIn("63-66 s", text)
            self.assertIn("2 halved, 1 skipped", text)          # sfx.a only
            self.assertEqual(sorted(os.listdir(os.path.join(out, "packed"))), ["sfx.a", "sfx.a.receipt"])
            import json
            with open(report, encoding="utf-8") as fh:
                s = json.load(fh)["summary"]
            self.assertEqual(s["excluded"], ["afterbirth.a"])
            self.assertEqual([os.path.basename(p) for p in s["sidecars"]], ["sfx.a.receipt"])
            # --copy-unchanged still carries the untouched afterbirth.a over
            out2 = os.path.join(td, "out2")
            with redirect_stdout(io.StringIO()):
                self.assertEqual(H.main(["tree", res, out2, "--copy-unchanged"]), 0)
            self.assertEqual(read_bytes(os.path.join(out2, "packed", "afterbirth.a")),
                             read_bytes(os.path.join(packed, "afterbirth.a")))
            # the single-archive driver refuses afterbirth.a without the flag
            with redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit) as cm:
                    H.main(["archive", os.path.join(packed, "afterbirth.a"), os.path.join(td, "ab.a")])
                self.assertIn("--include-afterbirth", str(cm.exception))
                self.assertEqual(H.main(["archive", os.path.join(packed, "afterbirth.a"), os.path.join(td, "ab.a"),
                                         "--include-afterbirth"]), 0)
            self.assertTrue(os.path.exists(os.path.join(td, "ab.a.receipt")))


class SidecarTest(unittest.TestCase):
    def make(self, td, name="sfx.a", version=2, pad=0):
        path = os.path.join(td, name)
        items = [{"h1": 1, "h2": 2, "data": b"RIFF" + bytes(range(256)) * 3},
                 {"h1": 3, "h2": 4, "data": b"x" * (5000 + pad)},
                 {"h1": 5, "h2": 6, "data": b"y"}]
        H.write_archive(path, version, items)
        return path

    def test_layout_and_check(self):
        with tempfile.TemporaryDirectory() as td:
            path = self.make(td)
            buf = read_bytes(path)
            rec = H.sidecar_bytes(buf, "sfx.a")
            self.assertEqual(len(rec), H.SIDECAR_SIZE)
            self.assertEqual(rec[:16], H.SIDECAR_MAGIC)
            version, size, asize, index_off, entries, mode, count, sample = struct.unpack_from("<IIQIIIII", rec, 16)
            self.assertEqual((version, size, asize, entries, mode, count, sample),
                             (1, 256, len(buf), 3, 2, 64, 4096))
            self.assertEqual(index_off, struct.unpack_from("<I", buf, 8)[0])
            self.assertEqual(rec[52:64], bytes(12))
            self.assertEqual(rec[64:96], hashlib.sha256(buf[index_off:index_off + 60]).digest())
            self.assertEqual(rec[128:168], b"sfx.a".ljust(40, b"\0"))
            self.assertEqual(rec[168:224], bytes(56))
            self.assertEqual(rec[224:256], hashlib.sha256(rec[:224]).digest())
            # the small-archive window scheme covers the whole entry region consecutively
            self.assertEqual(list(H.sidecar_windows(10000)), [(0, 4096), (4096, 4096), (8192, 1808)])
            big = list(H.sidecar_windows(1_000_000))
            self.assertEqual(len(big), 64)
            self.assertEqual(big[0], (0, 4096))
            self.assertEqual(big[-1], (1_000_000 - 4096, 4096))
            self.assertEqual(big[1][0], (1_000_000 - 4096) * 1 // 63)
            self.assertIsNone(H.sidecar_check(buf, rec, "sfx.a"))

    def test_mutations_are_refused(self):
        with tempfile.TemporaryDirectory() as td:
            path = self.make(td)
            buf = read_bytes(path)
            rec = H.sidecar_bytes(buf, "sfx.a")

            def rehash(b):
                b = bytearray(b)
                b[224:256] = hashlib.sha256(bytes(b[:224])).digest()
                return bytes(b)

            self.assertEqual(H.sidecar_check(buf, rec[:-1], "sfx.a"), "sidecar-identity: 255 bytes")
            bad = bytearray(rec)
            bad[230] ^= 1
            self.assertEqual(H.sidecar_check(buf, bytes(bad), "sfx.a"), "sidecar-decode: digest")
            bad = bytearray(rec)
            bad[0] ^= 1
            self.assertEqual(H.sidecar_check(buf, rehash(bad), "sfx.a"), "sidecar-decode: magic/version")
            bad = bytearray(rec)
            bad[200] = 7
            self.assertEqual(H.sidecar_check(buf, rehash(bad), "sfx.a"), "sidecar-decode: reserved bytes")
            bad = bytearray(rec)
            struct.pack_into("<I", bad, 44, 32)
            self.assertEqual(H.sidecar_check(buf, rehash(bad), "sfx.a"), "sidecar-decode: sample scheme")
            bad = bytearray(rec)
            struct.pack_into("<I", bad, 36, 4)
            self.assertEqual(H.sidecar_check(buf, rehash(bad), "sfx.a"), "sidecar-decode: geometry")
            self.assertEqual(H.sidecar_check(buf, rec, "afterbirth.a"), "sidecar-name")
            identity = H.sidecar_check(buf + b"\0", rec, "sfx.a")
            assert identity is not None
            self.assertTrue(identity.startswith("archive-identity"))
            hdr = bytearray(buf)
            hdr[7] = 0
            self.assertEqual(H.sidecar_check(bytes(hdr), rec, "sfx.a"), "archive-header")
            idx = bytearray(buf)
            idx[struct.unpack_from("<I", buf, 8)[0] + 16] ^= 1      # an entry's x field
            self.assertEqual(H.sidecar_check(bytes(idx), rec, "sfx.a"), "archive-index-hash")
            body = bytearray(buf)
            body[H.HEADER_SIZE + 100] ^= 1
            self.assertEqual(H.sidecar_check(bytes(body), rec, "sfx.a"), "archive-sample-hash")
            # a sidecar written for another archive of the same name is refused by its hashes
            other = self.make(td, name="sfx.a", pad=4)
            self.assertIsNotNone(H.sidecar_check(read_bytes(other), rec, "sfx.a"))
            # names that do not fit
            for name in ("", "a/b.a", ".hidden", "x" * 40):
                with self.assertRaises(H.SidecarError):
                    H.sidecar_bytes(buf, name)

    def test_write_sidecar_verifies_entries(self):
        with tempfile.TemporaryDirectory() as td:
            path = self.make(td)
            s = H.write_sidecar(path)
            self.assertEqual(s["path"], path + ".receipt")
            self.assertEqual((s["entries"], s["verified"]), (3, True))
            self.assertIsNone(H.sidecar_check(read_bytes(path), read_bytes(path + ".receipt"), "sfx.a"))
            # a corrupt entry blocks the sidecar
            buf = bytearray(read_bytes(path))
            buf[H.HEADER_SIZE + 40] ^= 0x55
            write_bytes(path, bytes(buf))
            with self.assertRaises(H.SidecarError):
                H.write_sidecar(path)
            # the receipt subcommand
            good = self.make(td, name="afterbirthp.a")
            buf2 = io.StringIO()
            with redirect_stdout(buf2):
                self.assertEqual(H.main(["receipt", good]), 0)
            self.assertIn("afterbirthp.a: verified 3 entries", buf2.getvalue())
            self.assertIsNone(H.sidecar_check(read_bytes(good), read_bytes(good + ".receipt"), "afterbirthp.a"))
            with self.assertRaises(SystemExit):
                H.main(["receipt", good, good])
            # a version-0 archive works too
            raw = self.make(td, name="sfx0.a", version=0)
            H.write_sidecar(raw)
            self.assertIsNone(H.sidecar_check(read_bytes(raw), read_bytes(raw + ".receipt"), "sfx0.a"))

    def test_wav_command_file_and_directory(self):
        with tempfile.TemporaryDirectory() as td:
            src = os.path.join(td, "in")
            os.makedirs(os.path.join(src, "sub"))
            quiet = wav_bytes((tone(440.0, seconds=0.2),), 44100)
            bright = wav_bytes((tone(15000.0, seconds=0.2),), 44100)
            write_bytes(os.path.join(src, "quiet.wav"), quiet)
            write_bytes(os.path.join(src, "sub", "bright.WAV"), bright)
            write_bytes(os.path.join(src, "readme.txt"), b"x")
            out = os.path.join(td, "out")
            with redirect_stdout(io.StringIO()):
                self.assertEqual(H.main(["wav", src, out]), 0)
            self.assertEqual(H.parse_wav(read_bytes(os.path.join(out, "quiet.wav"))).rate, 22050)
            self.assertEqual(read_bytes(os.path.join(out, "sub", "bright.WAV")), bright)
            self.assertFalse(os.path.exists(os.path.join(out, "readme.txt")))
            single = os.path.join(td, "single.wav")
            with redirect_stdout(io.StringIO()):
                self.assertEqual(H.main(["wav", os.path.join(src, "quiet.wav"), single]), 0)
            self.assertEqual(read_bytes(single), H.decide(quiet).output)
            with self.assertRaises(SystemExit):
                H.main(["wav", src, src])


@unittest.skipUnless(os.environ.get("ISAAC_VITA_SFX_ARCHIVE"), "set ISAAC_VITA_SFX_ARCHIVE to a shipped packed .a")
class ShippedArchiveTest(unittest.TestCase):
    def test_every_entry_checksums(self):
        a = H.Archive(os.environ["ISAAC_VITA_SFX_ARCHIVE"])
        self.assertIn(a.version, (0, 2))
        bad = [e.tag for e in a.entries if not a.verify(e)]
        self.assertEqual(bad, [])
        # stored-mode entries re-encode to the identical raw bytes
        for e in a.entries:
            if a.version != 2:
                break
            first, fin, _ = next(H.iter_pieces(a.buf, e.offset, e.size))
            if fin == 0 and len(first) == H.BLOCK:
                self.assertEqual(H.miniz_encode(a.decode(e), e.h2, "stored"), a.raw(e), e.tag)
                break


if __name__ == "__main__":
    unittest.main()
