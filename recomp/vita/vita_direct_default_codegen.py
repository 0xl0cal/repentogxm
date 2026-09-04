#!/usr/bin/env python3
"""Derive the one Vita direct-default guest translation unit.

Two derivation modes share the frozen guest_0144.c input and the same
build-owned output path:

* ``direct-default``: Game::Render bypasses all three outer targets (Render
  Surface, HQX surface, Color Correction); the world is rasterized straight
  into the default framebuffer at the 960x540 logical size.
* ``render-surface-native``: only the HQX and Color Correction targets are
  bypassed.  The game keeps its own Render Surface (480x270 in rooms, sized by
  Game::SetViewport from MaxRenderScale) and composites it to the default
  framebuffer with its plain non-HQX textured-quad blit (0x4b0ea8, shader
  override [0x987c7a44] = 0).  Measured rationale: with direct-default every
  world layer is rasterized at 960x540, four times the native fill; the
  seven full-screen-class blended layers of a heavy room cost ~9 ms at
  720x408 and the display raster does not remove that.  Menus and cutscenes
  are untouched: there the game sets Render Width == framebuffer width and
  skips the Render Surface itself (0x4b06ec).
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import sys
from typing import Sequence


SOURCE_NAME = "guest_0144.c"
SOURCE_SIZE = 1_115_751
SOURCE_SHA256 = "2c15e0305d7dbe6f5bdbb122b47b10d5952646942da503961b8da50b8ab09513"
OUTPUT_SIZE = 1_116_115
OUTPUT_SHA256 = "8e6c45d6a502d7fe4dae75e4bda3f661f748ae8952a3ec5cb8a85605968dad6f"
RENDER_SURFACE_NATIVE_OUTPUT_SIZE = 1_116_121
RENDER_SURFACE_NATIVE_OUTPUT_SHA256 = (
    "b53d55930fbf7adba71008843591a654e0ae782a06ce057310fb2de1547511a1"
)
OVERRIDE_PARENT = ("isaac-generated-overrides", "direct-default")
MARKER = b"ISAAC_VITA_DIRECT_DEFAULT:"
MODE_DIRECT_DEFAULT = "direct-default"
MODE_RENDER_SURFACE_NATIVE = "render-surface-native"
MODES = (MODE_DIRECT_DEFAULT, MODE_RENDER_SURFACE_NATIVE)

REPLACEMENTS = (
    (
        b"    /* 004b06f4  mov byte ptr [esp + 0xb], 1 */\n",
        b"    /* ISAAC_VITA_DIRECT_DEFAULT: skip Render Surface activation. */\n"
        b"    goto L_004b0712;\n"
        b"    /* 004b06f4  mov byte ptr [esp + 0xb], 1 */\n",
    ),
    (
        b"L_004b0759:\n"
        b"    /* 004b0759  mov byte ptr [esp + 0xa], 1 */\n",
        b"L_004b0759:\n"
        b"    /* ISAAC_VITA_DIRECT_DEFAULT: skip HQX Surface activation. */\n"
        b"    goto L_004b0777;\n"
        b"    /* 004b0759  mov byte ptr [esp + 0xa], 1 */\n",
    ),
    (
        b"    /* 004b078a  call 0x25b570 */\n",
        b"    /* ISAAC_VITA_DIRECT_DEFAULT: skip Color Correction activation. */\n"
        b"    goto L_004b07a1;\n"
        b"    /* 004b078a  call 0x25b570 */\n",
    ),
    (
        b"    /* 004b0af7  mov ecx, 0x987c7a30 */\n",
        b"    /* ISAAC_VITA_DIRECT_DEFAULT: skip the absent Color target unwind. */\n"
        b"    goto L_004b0c34;\n"
        b"    /* 004b0af7  mov ecx, 0x987c7a30 */\n",
    ),
)


# render-surface-native keeps the Render Surface activation (seam 0 of
# REPLACEMENTS) and still bypasses the HQX and Color Correction targets.  The
# first entry only annotates the preserved seam so the derived unit stays
# self-describing and carries exactly len(REPLACEMENTS) markers.
RENDER_SURFACE_NATIVE_REPLACEMENTS = (
    (
        REPLACEMENTS[0][0],
        b"    /* ISAAC_VITA_DIRECT_DEFAULT: render-surface-native keeps the "
        b"Render Surface activation. */\n" + REPLACEMENTS[0][0],
    ),
) + REPLACEMENTS[1:]


class DirectDefaultCodegenError(RuntimeError):
    pass


def mode_replacements(mode: str) -> tuple[tuple[bytes, bytes], ...]:
    if mode == MODE_DIRECT_DEFAULT:
        return REPLACEMENTS
    if mode == MODE_RENDER_SURFACE_NATIVE:
        return RENDER_SURFACE_NATIVE_REPLACEMENTS
    raise DirectDefaultCodegenError(f"unknown derivation mode: {mode!r}")


def mode_output_identity(mode: str) -> tuple[int, str]:
    """Read the pinned output identity at call time (tests re-pin globals)."""
    if mode == MODE_DIRECT_DEFAULT:
        return OUTPUT_SIZE, OUTPUT_SHA256
    if mode == MODE_RENDER_SURFACE_NATIVE:
        return (RENDER_SURFACE_NATIVE_OUTPUT_SIZE,
                RENDER_SURFACE_NATIVE_OUTPUT_SHA256)
    raise DirectDefaultCodegenError(f"unknown derivation mode: {mode!r}")


def _identity(data: bytes) -> tuple[int, str]:
    return len(data), hashlib.sha256(data).hexdigest()


def _apply_exact_replacements(
    data: bytes, mode: str = MODE_DIRECT_DEFAULT
) -> bytes:
    replacements = mode_replacements(mode)
    if data.count(MARKER):
        raise DirectDefaultCodegenError(
            "canonical input already contains a direct-default marker"
        )
    result = data
    for old, new in replacements:
        count = result.count(old)
        if count != 1:
            raise DirectDefaultCodegenError(
                f"direct-default seam count changed: {count} != 1: "
                f"{old.splitlines()[0]!r}"
            )
        result = result.replace(old, new, 1)
    if result.count(MARKER) != len(replacements):
        raise DirectDefaultCodegenError(
            "direct-default output marker census changed"
        )
    return result


def render_exact_source(
    data: bytes, mode: str = MODE_DIRECT_DEFAULT
) -> bytes:
    expected_size, expected_sha = mode_output_identity(mode)
    if _identity(data) != (SOURCE_SIZE, SOURCE_SHA256):
        size, digest = _identity(data)
        raise DirectDefaultCodegenError(
            "canonical direct-default input identity changed: "
            f"size={size}/{SOURCE_SIZE}, sha256={digest}/{SOURCE_SHA256}"
        )
    result = _apply_exact_replacements(data, mode)
    if _identity(result) != (expected_size, expected_sha):
        size, digest = _identity(result)
        raise DirectDefaultCodegenError(
            f"derived {mode} output identity changed: "
            f"size={size}/{expected_size}, sha256={digest}/{expected_sha}"
        )
    return result


def _require_input(path: Path) -> bytes:
    if path.name != SOURCE_NAME:
        raise DirectDefaultCodegenError(
            f"canonical input must be named {SOURCE_NAME}: {path}"
        )
    if path.is_symlink() or not path.is_file():
        raise DirectDefaultCodegenError(
            f"canonical input is not a regular non-symlink file: {path}"
        )
    try:
        return path.read_bytes()
    except OSError as exc:
        raise DirectDefaultCodegenError(
            f"cannot read canonical direct-default input {path}: {exc}"
        ) from exc


def _validate_output_path(path: Path) -> None:
    if path.name != SOURCE_NAME or tuple(part.name for part in path.parents[:2]) != (
        OVERRIDE_PARENT[1], OVERRIDE_PARENT[0]
    ):
        raise DirectDefaultCodegenError(
            "derived output must end in "
            f"{'/'.join((*OVERRIDE_PARENT, SOURCE_NAME))}: {path}"
        )
    if path.exists() or path.is_symlink():
        if path.is_symlink() or not path.is_file():
            raise DirectDefaultCodegenError(
                f"derived output is not a regular non-symlink file: {path}"
            )


def _reject_symlinked_output_owners(path: Path) -> None:
    for directory in (path.parent.parent, path.parent):
        if directory.is_symlink():
            raise DirectDefaultCodegenError(
                "derived output has a symlinked build-owned directory: "
                f"{directory}"
            )


def _require_private_output_directory(path: Path) -> None:
    if not path.exists():
        return
    if path.is_symlink() or not path.is_dir():
        raise DirectDefaultCodegenError(
            f"derived output parent is not a regular directory: {path}"
        )
    entries = list(path.iterdir())
    if any(entry.name != SOURCE_NAME for entry in entries):
        raise DirectDefaultCodegenError(
            "derived output directory contains an unowned entry: "
            + ", ".join(sorted(entry.name for entry in entries))
        )


def _lexical_absolute(path: Path, description: str) -> Path:
    if not path.is_absolute():
        raise DirectDefaultCodegenError(
            f"{description} must be absolute: {path}"
        )
    # Normalize dot components without resolving the final component through a
    # symlink.  The caller must be able to reject a symlink instead of silently
    # reading or replacing its target.
    return Path(os.path.abspath(path))


def derive_file(
    source: Path, output: Path, mode: str = MODE_DIRECT_DEFAULT
) -> bool:
    source = _lexical_absolute(source, "canonical input")
    output = _lexical_absolute(output, "derived output")
    if source.resolve(strict=False) == output.resolve(strict=False):
        raise DirectDefaultCodegenError(
            "canonical input and derived output resolve to the same file"
        )
    _validate_output_path(output)
    _reject_symlinked_output_owners(output)
    _require_private_output_directory(output.parent)
    result = render_exact_source(_require_input(source), mode)

    if output.is_file() and output.read_bytes() == result:
        return False

    output.parent.mkdir(parents=True, exist_ok=True)
    _require_private_output_directory(output.parent)
    temporary = output.with_name(f".{output.name}.tmp-{os.getpid()}")
    if temporary.exists() or temporary.is_symlink():
        raise DirectDefaultCodegenError(
            f"temporary derived output already exists: {temporary}"
        )
    try:
        with temporary.open("xb") as stream:
            stream.write(result)
        os.replace(temporary, output)
    except OSError as exc:
        raise DirectDefaultCodegenError(
            f"cannot publish derived direct-default source {output}: {exc}"
        ) from exc
    finally:
        if temporary.exists() and temporary.is_file() and not temporary.is_symlink():
            temporary.unlink()

    if output.is_symlink() or not output.is_file() or output.read_bytes() != result:
        raise DirectDefaultCodegenError(
            "published direct-default source failed byte verification"
        )
    return True


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--mode", choices=MODES, default=MODE_DIRECT_DEFAULT,
        help="which outer Game::Render targets the derived unit bypasses",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = build_parser().parse_args(argv)
        changed = derive_file(args.input, args.output, args.mode)
        size, digest = mode_output_identity(args.mode)
    except DirectDefaultCodegenError as exc:
        print(f"Vita direct-default codegen: FAIL: {exc}", file=sys.stderr)
        return 1
    state = "UPDATED" if changed else "BYTES STABLE"
    print(
        f"Vita direct-default codegen: {state}; mode={args.mode} "
        f"size={size} sha256={digest} output={args.output.resolve()}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
