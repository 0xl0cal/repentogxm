#!/usr/bin/env python3
"""Prove the stock-reference patch chain cannot change texture-name allocation."""

from __future__ import annotations

import hashlib
import re
from pathlib import Path


SOURCE_COMMIT = "73dd57a8857f89f2353881c6de5891959c5c1983"
BUILD_SCRIPT_SHA256 = "bfde967f23f254e40d22fdb5c45a1f4690b89e44ee0875d8615437285a1f2c63"
EXPECTED_PATHS = {
    "0001-deterministic-build-and-init-oob.patch": (
        "Makefile",
        "source/vgl.c",
    ),
    "0002-exact-gpu-draw-optimizations.patch": (
        "source/custom_shaders.c",
        "source/draw.c",
        "source/ffp.c",
        "source/framebuffers.c",
        "source/gxm.c",
        "source/isaac_gxm_state_shadow.c",
        "source/isaac_gxm_state_shadow.h",
        "source/misc.c",
        "source/shared.h",
        "source/vgl.c",
        "source/vitaGL.h",
    ),
    "0003-exact-coloroffset-gpu-optimizations.patch": (
        "source/custom_shaders.c",
        "source/vitaGL.h",
    ),
    "0004-hardened-custom-shader-cache.patch": (
        "source/custom_shaders.c",
        "source/vitaGL.h",
    ),
}
EXPECTED_PATCH_VARIABLES = (
    ("contract_patch", "0001-deterministic-build-and-init-oob.patch"),
    ("gpu_draw_patch", "0002-exact-gpu-draw-optimizations.patch"),
    ("coloroffset_patch", "0003-exact-coloroffset-gpu-optimizations.patch"),
    ("shader_cache_patch", "0004-hardened-custom-shader-cache.patch"),
)
FORBIDDEN_ALLOCATOR_TOKENS = (
    "glGenTextures",
    "glDeleteTextures",
    "texture_slots",
    "TEXTURES_NUM",
    "TEX_UNUSED",
    "TEX_UNINITIALIZED",
    "gpu_free_texture",
)


def patch_changed_paths_and_lines(path: Path) -> tuple[tuple[str, ...], list[str]]:
    changed_paths: list[str] = []
    changed_lines: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("diff --git "):
            match = re.fullmatch(r"diff --git a/(\S+) b/(\S+)", line)
            if match is None or match.group(1) != match.group(2):
                raise AssertionError(f"unmodelled patch path header: {line!r}")
            changed_paths.append(match.group(1))
        elif line.startswith(("+++ ", "--- ")):
            continue
        elif line.startswith(("+", "-")):
            changed_lines.append(line[1:])
    if len(changed_paths) != len(set(changed_paths)):
        raise AssertionError(f"duplicate patch path in {path.name}")
    return tuple(changed_paths), changed_lines


def main() -> int:
    stock = Path(__file__).resolve().parent / "vitagl-stock-reference"
    build_script_path = stock / "build.sh"
    build_script_bytes = build_script_path.read_bytes()
    if hashlib.sha256(build_script_bytes).hexdigest() != BUILD_SCRIPT_SHA256:
        raise AssertionError("stock vitaGL build script drifted")
    build_script = build_script_bytes.decode("utf-8")
    match = re.search(r"(?m)^source_commit=([0-9a-f]{40})$", build_script)
    if match is None or match.group(1) != SOURCE_COMMIT:
        raise AssertionError("stock vitaGL source commit drifted")

    actual_patch_names = {path.name for path in stock.glob("*.patch")}
    if actual_patch_names != set(EXPECTED_PATHS):
        raise AssertionError(
            "stock vitaGL patch-file census drifted: "
            f"{sorted(actual_patch_names)!r} != {sorted(EXPECTED_PATHS)!r}"
        )
    patch_variables = tuple(re.findall(
        r'(?m)^([a-z_]+_patch)="\$script_dir/([^"/]+\.patch)"$',
        build_script,
    ))
    if patch_variables != EXPECTED_PATCH_VARIABLES:
        raise AssertionError(
            f"stock vitaGL patch-variable census drifted: {patch_variables!r}"
        )
    patch_applications = tuple(re.findall(
        r'(?m)^\s+apply_source_patch "\$([a-z_]+_patch)"$',
        build_script,
    ))
    expected_applications = tuple(name for name, unused in EXPECTED_PATCH_VARIABLES)
    if patch_applications != expected_applications:
        raise AssertionError(
            "stock vitaGL patch-application census drifted: "
            f"{patch_applications!r} != {expected_applications!r}"
        )
    if "textures.c" in build_script:
        raise AssertionError("stock vitaGL build script acquired a textures.c mutation")

    checked_lines = 0
    for name, expected_paths in EXPECTED_PATHS.items():
        paths, changed_lines = patch_changed_paths_and_lines(stock / name)
        if paths != expected_paths:
            raise AssertionError(
                f"{name} path census drifted: {paths!r} != {expected_paths!r}"
            )
        if "source/textures.c" in paths:
            raise AssertionError(f"{name} acquired the texture allocator owner")
        for line in changed_lines:
            for token in FORBIDDEN_ALLOCATOR_TOKENS:
                if token in line:
                    raise AssertionError(
                        f"{name} changed allocator token {token!r}: {line!r}"
                    )
        checked_lines += len(changed_lines)

    print(
        "Stock vitaGL texture allocator patch isolation: PASS; "
        f"commit={SOURCE_COMMIT[:7]} patches={len(EXPECTED_PATHS)} "
        f"changed_lines={checked_lines}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
