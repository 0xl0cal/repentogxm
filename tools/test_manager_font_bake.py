#!/usr/bin/env python3
"""Tests for tools/manager_font_bake.py and the committed IMF1 files.

* Every committed recomp/vita/assets/manager/*.imf parses with an independent
  reader written here (header, CRC-32 of table + atlas, sorted unique code
  points, every glyph box inside the atlas, '?' present, flags A8) and holds
  exactly the baker's 183-glyph list.
* The manifest records the committed files' sizes and SHA-256.
* The DejaVu licence ships next to the fonts
  (recomp/vita/third_party/dejavu/LICENSE, mapped to licenses/dejavu.txt in
  the VPK) and is the Bitstream Vera / DejaVu text.
* Determinism: the baker re-bakes the three faces in memory and the bytes
  equal the committed files.  This needs Pillow, fontTools and the DejaVu
  TTFs the manifest names (matplotlib's bundled copy); when the local TTF or
  FreeType differs from the recorded one the test FAILS with the difference
  spelled out (a different rasteriser gives different bytes; that is a
  re-bake decision, not a pass).
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import struct
import sys
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
ASSETS = REPO / "recomp" / "vita" / "assets" / "manager"
LICENSE = REPO / "recomp" / "vita" / "third_party" / "dejavu" / "LICENSE"

_spec = importlib.util.spec_from_file_location("manager_font_bake", HERE / "manager_font_bake.py")
assert _spec is not None and _spec.loader is not None
bake = importlib.util.module_from_spec(_spec)
sys.modules["manager_font_bake"] = bake
_spec.loader.exec_module(bake)


def read_imf(data: bytes) -> dict[str, object]:
    """Independent IMF1 reader (does not reuse the baker's structs)."""
    if len(data) < 28 or data[:4] != b"IMF1":
        raise ValueError("bad magic or short header")
    px, line_h, ascent, atlas_w, atlas_h, count, flags, reserved = struct.unpack_from("<8H", data, 4)
    atlas_bytes, crc = struct.unpack_from("<II", data, 20)
    table_end = 28 + 16 * count
    if flags != 0 or reserved != 0:
        raise ValueError(f"flags={flags} reserved={reserved}")
    if atlas_bytes != atlas_w * atlas_h or len(data) != table_end + atlas_bytes:
        raise ValueError("size fields disagree with the file length")
    if zlib.crc32(data[28:]) & 0xFFFFFFFF != crc:
        raise ValueError("crc mismatch")
    glyphs = []
    previous = -1
    for index in range(count):
        cp, x, y, w, h, xoff, yoff, advance = struct.unpack_from("<IHHBBbbB", data, 28 + 16 * index)
        if data[28 + 16 * index + 13:28 + 16 * index + 16] != b"\0\0\0":
            raise ValueError("glyph reserved bytes not zero")
        if cp <= previous:
            raise ValueError("table not sorted/unique")
        previous = cp
        if w and h and (x + w > atlas_w or y + h > atlas_h):
            raise ValueError(f"U+{cp:04X} box outside the atlas")
        glyphs.append((cp, x, y, w, h, xoff, yoff, advance))
    return {"px": px, "line_h": line_h, "ascent": ascent, "atlas_w": atlas_w,
            "atlas_h": atlas_h, "glyphs": glyphs}


class CommittedFiles(unittest.TestCase):
    def test_each_face_parses_and_holds_the_glyph_list(self) -> None:
        expected = bake.glyph_list()
        for name, unused_ttf, px in bake.FACES:
            with self.subTest(face=name):
                info = read_imf((ASSETS / name).read_bytes())
                self.assertEqual(info["px"], px)
                self.assertEqual([g[0] for g in info["glyphs"]], expected)
                self.assertIn(ord("?"), [g[0] for g in info["glyphs"]])
                self.assertEqual(info["atlas_w"], 256)
                self.assertGreater(info["line_h"], info["ascent"])

    def test_manifest_matches_files(self) -> None:
        manifest = json.loads((ASSETS / bake.MANIFEST_NAME).read_text(encoding="utf-8"))
        self.assertEqual(manifest["glyph_count"], 183)
        self.assertEqual(sorted(manifest["files"]), sorted(name for name, _t, _p in bake.FACES))
        for name, record in manifest["files"].items():
            data = (ASSETS / name).read_bytes()
            self.assertEqual(record["size"], len(data), name)
            self.assertEqual(record["sha256"], hashlib.sha256(data).hexdigest(), name)
        self.assertEqual(manifest["toolchain"]["license_sha256"],
                         hashlib.sha256(LICENSE.read_bytes()).hexdigest())

    def test_licence_present(self) -> None:
        text = LICENSE.read_text(encoding="utf-8")
        self.assertTrue(text.startswith("Fonts are (c) Bitstream (see below). DejaVu changes are in public domain."))
        self.assertIn("Bitstream Vera Fonts Copyright", text)
        self.assertNotIn("\r", text)

    def test_reader_rejects_corruption(self) -> None:
        data = bytearray((ASSETS / "font-small-12.imf").read_bytes())
        data[-1] ^= 0x55
        with self.assertRaises(ValueError):
            read_imf(bytes(data))
        with self.assertRaises(ValueError):
            read_imf(bytes((ASSETS / "font-small-12.imf").read_bytes())[:-1])


class Determinism(unittest.TestCase):
    def test_rebake_reproduces_committed_bytes(self) -> None:
        try:
            ttf_dir = bake.default_ttf_dir()
            import fontTools  # noqa: F401,PLC0415
            import PIL  # noqa: F401,PLC0415
        except ImportError as exc:
            self.fail(f"determinism check needs Pillow, fontTools and matplotlib: {exc}")
        manifest = json.loads((ASSETS / bake.MANIFEST_NAME).read_text(encoding="utf-8"))
        local = bake.toolchain_identity(ttf_dir)
        recorded = manifest["toolchain"]
        if local != recorded:
            self.fail("local font toolchain differs from the one that baked the committed files; "
                      f"recorded={recorded} local={local} (re-bake deliberately or run on the baking machine)")
        outputs = bake.bake_all(ttf_dir)
        for name, data in outputs.items():
            with self.subTest(face=name):
                self.assertEqual(hashlib.sha256(data).hexdigest(),
                                 hashlib.sha256((ASSETS / name).read_bytes()).hexdigest())
        self.assertEqual(bake.manifest_text(ttf_dir, outputs),
                         (ASSETS / bake.MANIFEST_NAME).read_text(encoding="utf-8"))


if __name__ == "__main__":
    result = unittest.main(exit=False, verbosity=1).result
    if result.wasSuccessful():
        print(f"manager font bake tests: PASS ({result.testsRun} tests)")
    raise SystemExit(0 if result.wasSuccessful() else 1)
