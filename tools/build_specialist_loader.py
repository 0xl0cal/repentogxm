#!/usr/bin/env python3
"""Build the tiny Specialist Dance data used by the Vita loading screen.

The converted stream is tracked as recomp/runtime/kage_vita_loading_specialist.inc;
the original Workshop files are not copied. This converter verifies the exact credited source, samples the 449-frame ANM2
animation deterministically, pre-composites it over the loading panel, and
emits a compact transparent/literal RGB565 stream for the CPU display callback.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw


ANM2_SHA256 = "b3b0e9a333f198acd0cdbe430d1e0bc72be997fe8a6b17e7dc946e195525029a"
SHEET_SHA256 = "f9b7273595f73fde723a374448b0f34d698b99f56d668b78d4ccaa7a56f422f4"
FRAME_COUNT = 32
SOURCE_FRAME_COUNT = 449
FRAME_WIDTH = 56
FRAME_HEIGHT = 47
PANEL_RGB = (0x23, 0x17, 0x1E)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def checked_source(root: Path) -> tuple[Path, Path, list[ET.Element]]:
    anm2 = root / "specialist_isaac.anm2"
    sheet = root / "sheets" / "isaac.png"
    if sha256(anm2) != ANM2_SHA256:
        raise SystemExit(f"unexpected specialist_isaac.anm2: {anm2}")
    if sha256(sheet) != SHEET_SHA256:
        raise SystemExit(f"unexpected specialist sprite sheet: {sheet}")

    document = ET.parse(anm2).getroot()
    info = document.find("Info")
    animation = document.find("./Animations/Animation[@Name='HeadDown']")
    layer = None if animation is None else animation.find(
        "./LayerAnimations/LayerAnimation[@LayerId='0']"
    )
    if info is None or info.get("CreatedBy") != "devector-jiftoo":
        raise SystemExit("ANM2 creator attribution does not match the credited source")
    if animation is None or animation.get("FrameNum") != str(SOURCE_FRAME_COUNT):
        raise SystemExit("HeadDown is not the expected 449-frame animation")
    if layer is None:
        raise SystemExit("HeadDown layer 0 is missing")
    frames = layer.findall("Frame")
    if len(frames) != SOURCE_FRAME_COUNT:
        raise SystemExit(f"expected {SOURCE_FRAME_COUNT} layer frames, got {len(frames)}")
    return anm2, sheet, frames


def rgb565(red: int, green: int, blue: int) -> int:
    return (
        ((red * 31 + 127) // 255) << 11
        | ((green * 63 + 127) // 255) << 5
        | ((blue * 31 + 127) // 255)
    )


def sampled_frames(sheet_path: Path, frame_nodes: list[ET.Element]) -> list[Image.Image]:
    sheet = Image.open(sheet_path).convert("RGBA")
    output: list[Image.Image] = []
    for output_index in range(FRAME_COUNT):
        source_index = output_index * SOURCE_FRAME_COUNT // FRAME_COUNT
        node = frame_nodes[source_index]
        width = int(node.get("Width", "0"))
        height = int(node.get("Height", "0"))
        if (width, height) != (FRAME_WIDTH, FRAME_HEIGHT):
            raise SystemExit(f"frame {source_index} has unexpected extent {width}x{height}")
        x = int(node.get("XCrop", "-1"))
        y = int(node.get("YCrop", "-1"))
        if x < 0 or y < 0 or x + width > sheet.width or y + height > sheet.height:
            raise SystemExit(f"frame {source_index} crop is outside the sprite sheet")
        output.append(sheet.crop((x, y, x + width, y + height)))
    return output


def encode_frame(frame: Image.Image) -> list[int]:
    values: list[int | None] = []
    panel_red, panel_green, panel_blue = PANEL_RGB
    for red, green, blue, alpha in frame.getdata():
        if alpha == 0:
            values.append(None)
            continue
        red = (red * alpha + panel_red * (255 - alpha) + 127) // 255
        green = (green * alpha + panel_green * (255 - alpha) + 127) // 255
        blue = (blue * alpha + panel_blue * (255 - alpha) + 127) // 255
        values.append(rgb565(red, green, blue))

    stream: list[int] = []
    position = 0
    while position < len(values):
        transparent = values[position] is None
        end = position + 1
        while (
            end < len(values)
            and (values[end] is None) == transparent
            and end - position < 0x7FFF
        ):
            end += 1
        count = end - position
        if transparent:
            stream.append(0x8000 | count)
        else:
            stream.append(count)
            stream.extend(int(value) for value in values[position:end])
        position = end
    return stream


def format_values(values: list[int], digits: int, per_line: int) -> str:
    lines = []
    for start in range(0, len(values), per_line):
        chunk = values[start : start + per_line]
        lines.append("    " + ", ".join(f"0x{value:0{digits}x}u" for value in chunk) + ",")
    return "\n".join(lines)


def write_include(output: Path, encoded: list[list[int]]) -> None:
    offsets = [0]
    words: list[int] = []
    for frame in encoded:
        words.extend(frame)
        offsets.append(len(words))
    text = f"""/* Generated by tools/build_specialist_loader.py.
 * Source: Specialist Dance, Steam Workshop item 2575911103.
 * Programming: Jiftoo. Graphics/trailer: Devector.
 * Animation rip credited by the Workshop page: SetoKeino.
 * Input hashes: ANM2 {ANM2_SHA256}; PNG {SHEET_SHA256}.
 * Do not edit this file by hand. */
#ifndef KAGE_VITA_LOADING_SPECIALIST_INC
#define KAGE_VITA_LOADING_SPECIALIST_INC

#define KAGE_LOADING_DANCE_FRAME_COUNT {FRAME_COUNT}u
#define KAGE_LOADING_DANCE_WIDTH {FRAME_WIDTH}u
#define KAGE_LOADING_DANCE_HEIGHT {FRAME_HEIGHT}u
#define KAGE_LOADING_DANCE_WORD_COUNT {len(words)}u

static const uint32_t s_loading_dance_offsets[{len(offsets)}] = {{
{format_values(offsets, 8, 6)}
}};

/* A word with bit 15 set skips transparent pixels.  Otherwise the word is a
 * literal count followed by that many panel-precomposited RGB565 pixels. */
static const uint16_t s_loading_dance_words[{len(words)}] = {{
{format_values(words, 4, 12)}
}};

#endif
"""
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="ascii", newline="\n")


def write_preview(output: Path, frames: list[Image.Image]) -> None:
    scale = 4
    columns = 8
    cell_width = FRAME_WIDTH * scale
    cell_height = FRAME_HEIGHT * scale
    rows = (len(frames) + columns - 1) // columns
    preview = Image.new("RGB", (columns * cell_width, rows * cell_height), PANEL_RGB)
    for index, frame in enumerate(frames):
        tile = Image.new("RGBA", frame.size, PANEL_RGB + (255,))
        tile.alpha_composite(frame)
        tile = tile.convert("RGB").resize((cell_width, cell_height), Image.Resampling.NEAREST)
        preview.paste(tile, ((index % columns) * cell_width, (index // columns) * cell_height))
        ImageDraw.Draw(preview).text(
            ((index % columns) * cell_width + 3, (index // columns) * cell_height + 3),
            str(index),
            fill=(255, 255, 255),
        )
    output.parent.mkdir(parents=True, exist_ok=True)
    preview.save(output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source-root",
        type=Path,
        required=True,
        help="Workshop resources/the directory containing specialist_isaac.anm2",
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--preview", type=Path)
    arguments = parser.parse_args()

    _anm2, sheet, frame_nodes = checked_source(arguments.source_root)
    frames = sampled_frames(sheet, frame_nodes)
    encoded = [encode_frame(frame) for frame in frames]
    write_include(arguments.output, encoded)
    if arguments.preview:
        write_preview(arguments.preview, frames)
    print(
        f"Specialist loader: {len(frames)} frames, "
        f"{sum(len(frame) for frame in encoded)} uint16 words"
    )


if __name__ == "__main__":
    main()
