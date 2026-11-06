#!/usr/bin/env python3
"""Bake the manager UI2 fonts into IMF1 files.

The LiveArea manager (isaac-manager.bin, knob ISAAC_VITA_MANAGER_UI2) draws
proportional text from three pre-baked alpha atlases shipped in the VPK as
app0:/manager/*.imf.  This script is the only producer of those files; it runs
on a PC with Pillow (FreeType) and fontTools, and its outputs are committed
under recomp/vita/assets/manager/ so neither the build host nor the VPK needs
Pillow or the TTF.

Faces (the UI2 design's chosen set, at the sizes of its mock-up renderer:
24 px bold, 16 px, 12 px):

    font-title-24.imf  DejaVu Sans Bold 24 px
    font-body-16.imf   DejaVu Sans      16 px
    font-small-12.imf  DejaVu Sans      12 px

Glyph set: the 183 code points of the mock-up renderer's glyph list (ASCII
0x20-0x7E, U+0410-044F, Ё ё and 22 symbols), copied verbatim.

IMF1 layout (little-endian; the UI2 design lists the fields as "24 B", which
does not add up: the named fields are 26 bytes, plus 2 reserved bytes so the
glyph table starts 4-byte aligned):

    header (28 B): "IMF1", u16 px, u16 line_h, u16 ascent, u16 atlas_w,
                   u16 atlas_h, u16 glyph_count, u16 flags (bit0 = A4; always
                   0 here: A8), u16 reserved (0), u32 atlas_bytes,
                   u32 crc32(table + atlas)
    glyphs (16 B each, sorted by code point): u32 codepoint, u16 x, u16 y,
                   u8 w, u8 h, i8 xoff, i8 yoff, u8 advance, u8 reserved[3]
    atlas  (atlas_bytes): row-major 8-bit alpha, atlas_w wide

xoff/yoff place the ink box relative to the pen position on the *ascender
line* (Pillow's "la" anchor, the anchor every text call of the mock renderer
uses), so a string drawn at (x, y) lands where the mock put it.  Advances are
Pillow's per-glyph getlength(), which is integral for these hinted faces (the
script refuses a fractional one); no kerning.

Packing: height-sorted shelf packer, 256 px wide, 1 px gap (the mock-up
renderer's packer, extended to record positions); empty glyphs
(space) occupy no atlas cell.  Deterministic: fixed list, stable sort.

Usage:
    python tools/manager_font_bake.py            # write the three files
    python tools/manager_font_bake.py --check    # re-bake, compare, exit 1 on drift
Options: --ttf-dir (default: matplotlib's bundled DejaVu), --out.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_OUT = REPO / "recomp" / "vita" / "assets" / "manager"
LICENSE_TARGET = REPO / "recomp" / "vita" / "third_party" / "dejavu" / "LICENSE"
MANIFEST_NAME = "fonts.manifest.json"

# Copied verbatim from the mock-up renderer's glyph list.
GLYPHS = ([chr(c) for c in range(0x20, 0x7F)] + [chr(c) for c in range(0x410, 0x450)] + ["Ё", "ё"]
          + list("←↑→↓•…×○△□—·◂▸›✓☑☐●«»№"))

# (output name, TTF file, pixel size)
FACES = (
    ("font-title-24.imf", "DejaVuSans-Bold.ttf", 24),
    ("font-body-16.imf", "DejaVuSans.ttf", 16),
    ("font-small-12.imf", "DejaVuSans.ttf", 12),
)
ATLAS_WIDTH = 256
HEADER = struct.Struct("<4sHHHHHHHHII")
GLYPH = struct.Struct("<IHHBBbbB3s")
assert HEADER.size == 28 and GLYPH.size == 16


def default_ttf_dir() -> Path:
    import matplotlib  # noqa: PLC0415 - optional, PC-only dependency

    return Path(matplotlib.__file__).resolve().parent / "mpl-data" / "fonts" / "ttf"


def glyph_list() -> list[int]:
    codepoints = sorted({ord(ch) for ch in GLYPHS})
    if len(codepoints) != len(GLYPHS) or len(codepoints) != 183:
        raise SystemExit(f"glyph list must hold 183 unique code points, has {len(codepoints)}")
    if ord("?") not in codepoints:
        raise SystemExit("glyph list lost '?' (the missing-glyph substitute)")
    return codepoints


def check_coverage(ttf: Path, codepoints: list[int]) -> None:
    from fontTools.ttLib import TTFont  # noqa: PLC0415

    cmap = TTFont(str(ttf)).getBestCmap()
    missing = [f"U+{cp:04X}" for cp in codepoints if cp not in cmap]
    if missing:
        raise SystemExit(f"{ttf.name} lacks glyphs: {', '.join(missing)}")


def rasterize(font, ch: str):
    """Tight ink box of one glyph drawn with Pillow's 'la' anchor."""
    from PIL import Image, ImageDraw  # noqa: PLC0415

    ascent, descent = font.getmetrics()
    pad = 8
    size = (int(font.size) * 3 + 2 * pad, ascent + descent + 2 * pad)
    image = Image.new("L", size, 0)
    ImageDraw.Draw(image).text((pad, pad), ch, font=font, fill=255, anchor="la")
    box = image.getbbox()
    advance = font.getlength(ch)
    if abs(advance - round(advance)) > 1e-6:
        raise SystemExit(f"fractional advance {advance} for U+{ord(ch):04X}")
    if box is None:
        return None, 0, 0, int(round(advance))
    crop = image.crop(box)
    return crop, box[0] - pad, box[1] - pad, int(round(advance))


def bake_face(ttf: Path, px: int, codepoints: list[int]) -> bytes:
    from PIL import ImageFont  # noqa: PLC0415

    font = ImageFont.truetype(str(ttf), px)
    ascent, descent = font.getmetrics()
    glyphs = []
    for cp in codepoints:
        crop, xoff, yoff, advance = rasterize(font, chr(cp))
        w, h = (0, 0) if crop is None else crop.size
        for value, name in ((w, "w"), (h, "h"), (advance, "advance")):
            if not 0 <= value <= 255:
                raise SystemExit(f"U+{cp:04X} {name}={value} does not fit u8")
        for value, name in ((xoff, "xoff"), (yoff, "yoff")):
            if not -128 <= value <= 127:
                raise SystemExit(f"U+{cp:04X} {name}={value} does not fit i8")
        glyphs.append({"cp": cp, "crop": crop, "w": w, "h": h, "xoff": xoff,
                       "yoff": yoff, "advance": advance, "x": 0, "y": 0})
    # Height-sorted shelf packer (as in the mock-up renderer): sort by
    # descending height, stable on code point order; 1 px gap.
    order = sorted((g for g in glyphs if g["w"] and g["h"]), key=lambda g: -g["h"])
    x = y = shelf_h = 0
    for g in order:
        if x + g["w"] + 1 > ATLAS_WIDTH:
            x, y, shelf_h = 0, y + shelf_h + 1, 0
        g["x"], g["y"] = x, y
        x += g["w"] + 1
        shelf_h = max(shelf_h, g["h"])
    atlas_h = y + shelf_h
    atlas = bytearray(ATLAS_WIDTH * atlas_h)
    for g in order:
        data = g["crop"].tobytes()
        for row in range(g["h"]):
            start = (g["y"] + row) * ATLAS_WIDTH + g["x"]
            atlas[start:start + g["w"]] = data[row * g["w"]:(row + 1) * g["w"]]
    table = b"".join(
        GLYPH.pack(g["cp"], g["x"], g["y"], g["w"], g["h"], g["xoff"], g["yoff"],
                   g["advance"], b"\0\0\0")
        for g in glyphs
    )
    crc = zlib.crc32(table + bytes(atlas)) & 0xFFFFFFFF
    header = HEADER.pack(b"IMF1", px, ascent + descent, ascent, ATLAS_WIDTH, atlas_h,
                         len(glyphs), 0, 0, len(atlas), crc)
    return header + table + bytes(atlas)


def toolchain_identity(ttf_dir: Path) -> dict[str, object]:
    import PIL  # noqa: PLC0415
    from PIL import ImageFont  # noqa: PLC0415

    return {
        "pillow": PIL.__version__,
        "freetype": ImageFont.core.freetype2_version,
        "ttf_sha256": {
            name: hashlib.sha256((ttf_dir / name).read_bytes()).hexdigest()
            for name in sorted({ttf for unused, ttf, unused_px in FACES})
        },
        "license_sha256": hashlib.sha256((ttf_dir / "LICENSE_DEJAVU").read_bytes()).hexdigest(),
    }


def bake_all(ttf_dir: Path) -> dict[str, bytes]:
    codepoints = glyph_list()
    outputs: dict[str, bytes] = {}
    for name, ttf, px in FACES:
        path = ttf_dir / ttf
        if not path.is_file():
            raise SystemExit(f"missing TTF: {path}")
        check_coverage(path, codepoints)
        outputs[name] = bake_face(path, px, codepoints)
    return outputs


def manifest_text(ttf_dir: Path, outputs: dict[str, bytes]) -> str:
    record = {
        "generator": "tools/manager_font_bake.py",
        "glyph_count": len(glyph_list()),
        "toolchain": toolchain_identity(ttf_dir),
        "files": {
            name: {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            for name, data in sorted(outputs.items())
        },
    }
    return json.dumps(record, indent=2, sort_keys=True) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    parser.add_argument("--ttf-dir", type=Path, default=None)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--check", action="store_true",
                        help="re-bake and compare with the files in --out")
    args = parser.parse_args()
    ttf_dir = args.ttf_dir or default_ttf_dir()
    outputs = bake_all(ttf_dir)
    manifest = manifest_text(ttf_dir, outputs)
    license_bytes = (ttf_dir / "LICENSE_DEJAVU").read_bytes()
    if args.check:
        drift = []
        for name, data in outputs.items():
            path = args.out / name
            if not path.is_file() or path.read_bytes() != data:
                drift.append(name)
        if not (args.out / MANIFEST_NAME).is_file() or \
                (args.out / MANIFEST_NAME).read_text(encoding="utf-8") != manifest:
            drift.append(MANIFEST_NAME)
        if not LICENSE_TARGET.is_file() or LICENSE_TARGET.read_bytes() != license_bytes:
            drift.append(str(LICENSE_TARGET.relative_to(REPO)))
        if drift:
            print(f"manager font bake: DRIFT in {drift}", file=sys.stderr)
            return 1
        print("manager font bake: committed files reproduce "
              + ", ".join(f"{name} {len(data)} B" for name, data in sorted(outputs.items())))
        return 0
    args.out.mkdir(parents=True, exist_ok=True)
    for name, data in outputs.items():
        (args.out / name).write_bytes(data)
        print(f"wrote {args.out / name} ({len(data)} B)")
    (args.out / MANIFEST_NAME).write_text(manifest, encoding="utf-8", newline="\n")
    LICENSE_TARGET.parent.mkdir(parents=True, exist_ok=True)
    LICENSE_TARGET.write_bytes(license_bytes)
    print(f"wrote {args.out / MANIFEST_NAME} and {LICENSE_TARGET}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
