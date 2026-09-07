#!/usr/bin/env python3
"""Apply every supported vitaGL patch chain to the exact pinned stock source."""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile
from pathlib import Path

SOURCE_COMMIT = "73dd57a8857f89f2353881c6de5891959c5c1983"
SOURCE_ARCHIVE_SHA256 = (
    "f484dd9d2aec707ac5f91352c6e0631239f2330fe6769e8476cfe425331c400a"
)
SOURCE_ROOT = f"vitaGL-{SOURCE_COMMIT}"
# Key: gpu_draw:coloroffset:coloroffset_fs_probe_applied (the build.sh
# shader-cache matrix).  The 0008 diagnostic probe edits custom_shaders.c only,
# so the vitaGL.h hash of 1:1:1 equals 1:1:0.
EXPECTED_HASHES = {
    "0:0:0": (
        "51377c7a9f75a27a093c86ba3106314fb3a08dff824fe6228ebbd796a106c9ed",
        "a0c2141c568048797a9431c3f3947d1975a013448d4327edb53316894d990a4e",
    ),
    "1:0:0": (
        "1a8285ee3377855359268306d65a879f9697e2235e337aa9be4f496108da14d5",
        "4ed165659b6909b4a329e974b78ff48cab1d777dcd638a3dc066128dc1260bcc",
    ),
    "1:1:0": (
        "6a9ca09632ab40e86af8d6752789bb75c6c0a10ebd03174339264e306c78f70e",
        "59e81f903cf6e6a0f5fd8f5b8785bea3b3d5ee65d5f1724f3e54d0808e6f0224",
    ),
    "1:1:1": (
        "978752ece599cf284b620991eba2ee20b0cb8c969ec15db9b00539d5d2e1a529",
        "59e81f903cf6e6a0f5fd8f5b8785bea3b3d5ee65d5f1724f3e54d0808e6f0224",
    ),
}
FS_PROBE_PATCH = "0008-isaac-coloroffset-fs-probe.patch"
STAGING_PATCH = "0012-isaac-coloroffset-staging-proof.patch"
STAGING_WITH_CACHE_SHA256 = "c27605f306f1160a27f30fc5ade7c15492fcd5da2f34a37643edd9844b20a9e2"
SINGLE_BIND_PATCH = "0014-isaac-single-final-fragment-bind.patch"
SINGLE_BIND_WITH_CACHE_SHA256 = "358f3419a559d7d00d463afeaa788e87b08fe13fd40fbc082e4d0ff18bd9ae7d"
PLAIN_PATCH = "0013-isaac-coloroffset-plain-fastpath.patch"
PLAIN_WITH_CACHE_SHA256 = "d304c32ea1b88f8511531cb2681e20ca4144236dae06d42c1507fd6da6ed4718"
PLAIN_SINGLE_BIND_WITH_CACHE_SHA256 = "8888d87edeeb488d46a2def21a3f4e1dcd3ba986677c3ec5fd5495761ff78219"
PLAIN_SINGLE_BIND_WITHOUT_CACHE_SHA256 = "d06e872acf785786becb529b73ae3f241b02101d6d30a843337909963ea6a8a7"
PLAIN_VERTEX_PAIR_PATCH = "0015-isaac-coloroffset-plain-vertex-pair.patch"
PLAIN_VERTEX_PAIR_WITH_CACHE_SHA256 = "a4ff11a5911646d6ea70278a2243b737b50693876105d181569767a452e3c0ea"
PLAIN_VERTEX_PAIR_WITHOUT_CACHE_SHA256 = "32f139efe30991389d1a715cae600585eb8625fd40362844cb948b821b023899"
LIGHT_HALO_WITH_CACHE_SHA256 = "64c3d77ec8f6ea565616bd5c3e5012ec7efd104a714faeff08747600be1f8737"
LIGHT_HALO_WITH_PAIR_SHA256 = "e887f6bcc48d6984e617028c0c17aeec89ace87df8315c55821cf89bd39aa894"
TRANSFORM_PATCH = "0018-isaac-transform-uniform-noop.patch"
TRANSFORM_WITH_CACHE_SHA256 = "71ac71f8a637b22c4de50cba110590f4348b5cfdd3869710d71f4c8d3ecd64e5"
TRANSFORM_WITHOUT_CACHE_SHA256 = "c15c96580aaf3a391f8d0a27714d0858037f6230cc931b7e4b103d3a9e4d1e76"
TRANSFORM_WITH_HALO_SHA256 = "5d8df29f49e2994f38181d061b27829273f92841262597cae832a30279d4dea8"
ATLAS_NEAREST_PATCH = "0019-isaac-laser-atlas-nearest.patch"
ATLAS_NEAREST_WITH_CACHE_SHA256 = (
    "c4b77d487337a853a87abf6b0d3be265fdf69218c88d9666c64a868fc3b67ee2",
    "5ac2bedbaf323475dd586ed85b7b392ce53436b9fddb16a5a64e89690a52e613",
)
# Source-text evidence mirrored from build.sh: present in custom_shaders.c iff
# the probe is applied, never in vitaGL.h (consumer-declared statistics).
FS_PROBE_NEEDLES = (
    b"HAVE_ISAAC_COLOROFFSET_FS_PROBE",
    b"isaac_coloroffset_fs_probe_link(p)",
    b"isaac_coloroffset_fs_probe_select(",
    b"vglGetIsaacColorOffsetFsProbeStats",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command: list[str], *, cwd: Path | None = None) -> bytes:
    return subprocess.run(
        command, cwd=cwd, check=True, stdout=subprocess.PIPE
    ).stdout


def safe_extract(bundle: tarfile.TarFile, destination: Path) -> None:
    resolved_destination = destination.resolve()
    for member in bundle.getmembers():
        target = (destination / member.name).resolve()
        if target != resolved_destination and resolved_destination not in target.parents:
            raise AssertionError(f"unsafe stock archive member: {member.name}")
        if member.name != SOURCE_ROOT and not member.name.startswith(SOURCE_ROOT + "/"):
            raise AssertionError(f"unexpected stock archive root: {member.name}")
    bundle.extractall(destination)


def stock_archive_from_repo(repository: Path) -> bytes:
    if not repository.is_dir():
        raise AssertionError(f"stock repository is not a directory: {repository}")
    resolved = run(
        [
            "git",
            "-c",
            "core.autocrlf=false",
            "-c",
            "core.eol=lf",
            "-C",
            str(repository),
            "rev-parse",
            "--verify",
            f"{SOURCE_COMMIT}^{{commit}}",
        ]
    ).decode("ascii").strip()
    if resolved != SOURCE_COMMIT:
        raise AssertionError(
            f"stock repository resolved {resolved}, expected {SOURCE_COMMIT}"
        )
    return run(
        [
            "git",
            "-c",
            "core.autocrlf=false",
            "-c",
            "core.eol=lf",
            "-C",
            str(repository),
            "archive",
            "--format=tar",
            f"--prefix={SOURCE_ROOT}/",
            SOURCE_COMMIT,
        ]
    )


def materialize_stock(
    destination: Path, stock_tar: Path | None, repository_archive: bytes | None
) -> Path:
    destination.mkdir()
    if stock_tar is not None:
        with tarfile.open(stock_tar, mode="r:*") as bundle:
            safe_extract(bundle, destination)
    else:
        assert repository_archive is not None
        with tarfile.open(fileobj=io.BytesIO(repository_archive), mode="r:") as bundle:
            safe_extract(bundle, destination)
    source = destination / SOURCE_ROOT
    for required in (
        source / ".gitattributes",
        source / "source" / "custom_shaders.c",
        source / "source" / "vitaGL.h",
    ):
        if not required.is_file():
            raise AssertionError(f"stock source is incomplete: {required}")
    return source


def verify_build_contract(recipe: Path) -> None:
    build = (recipe / "build.sh").read_text(encoding="utf-8")
    for needle in (
        'native_resource_profile=${ISAAC_NATIVE_RESOURCE_PROFILE:-0}',
        'if [ "$native_resource_profile" = 1 ] && [ "$gl_time_profile" != 1 ]; then',
        'build_flags="$build_flags HAVE_ISAAC_NATIVE_RESOURCE_PROFILE=1"',
        'extra_cflags="$extra_cflags -DHAVE_ISAAC_NATIVE_RESOURCE_PROFILE=1"',
        'cp -- "$native_resource_source" "$source_dir/source/isaac_native_resource_profile.c"',
        'local recipe_gl_time=$1 recipe_native_resource=$2 recipe_build_flags=$3',
        '"$recipe_native_resource" "$native_resource_patch_sha"',
        'recipe=$({ emit_recipe_inputs "$gl_time_profile" "$native_resource_profile" \\\n'
        '    "$build_flags";',
        "printf 'native_resource_profile=%s\\n'",
        '1:*vglIsaacNativeResourceProfileTake*) ;;',
        '0:*vglIsaacNativeResourceProfileTake*)',
    ):
        if needle not in build:
            raise AssertionError(f"native resource profile wiring lost {needle}")
    assert build.index('apply_source_patch "$native_resource_patch"') > build.index(
        'apply_source_patch "$laser_atlas_nearest_patch"')
    for needle in (
        'laser_atlas_nearest=${ISAAC_LASER_ATLAS_NEAREST:-0}',
        'if [ "$laser_atlas_nearest" = 1 ] &&\n'
        '   { [ "$shader_cache" != 1 ] || [ "$laser_atlas_p8" != 1 ] ||',
        'build_flags="$build_flags HAVE_ISAAC_LASER_ATLAS_NEAREST=1"',
        'extra_cflags="$extra_cflags -DHAVE_ISAAC_LASER_ATLAS_NEAREST=1"',
        'if [ "$laser_light_halo_clip" = 1 ] || [ "$laser_atlas_nearest" = 1 ]; then',
        'if [ "$laser_light_nearest" = 1 ] || [ "$laser_atlas_nearest" = 1 ]; then',
        'cp -- "$laser_atlas_nearest_policy" "$source_dir/source/isaac_laser_atlas_nearest.h"',
        '"$laser_atlas_nearest" "$laser_atlas_nearest_patch_sha"',
        "printf 'laser_atlas_nearest=%s\\n'",
        *ATLAS_NEAREST_WITH_CACHE_SHA256,
    ):
        if needle not in build:
            raise AssertionError(f"atlas nearest native-only wiring lost {needle}")
    if build.index('apply_source_patch "$laser_atlas_nearest_patch"') < build.index(
            'apply_source_patch "$coloroffset_transform_uniform_patch"'):
        raise AssertionError("atlas nearest must follow optional Transform")
    for needle in (
        'coloroffset_transform_uniform=${ISAAC_COLOROFFSET_TRANSFORM_UNIFORM:-0}',
        '"coloroffset_transform_uniform:$coloroffset_transform_uniform"',
        'if [ "$coloroffset_transform_uniform" = 1 ] && [ "$coloroffset_plain_vertex_pair" != 1 ]; then',
        'build_flags="$build_flags HAVE_ISAAC_COLOROFFSET_TRANSFORM_UNIFORM=1"',
        'extra_cflags="$extra_cflags -DHAVE_ISAAC_COLOROFFSET_TRANSFORM_UNIFORM=1"',
        '"$coloroffset_transform_uniform" "$coloroffset_transform_uniform_patch_sha"',
        '"$coloroffset_transform_uniform_header_sha"',
        'cp -- "$coloroffset_transform_uniform_header" "$source_dir/source/isaac_coloroffset_transform_uniform.h"',
        'stock vitaGL source unexpectedly contains Transform uniform suppression',
        "printf 'coloroffset_transform_uniform=%s\\n'",
        TRANSFORM_WITH_CACHE_SHA256, TRANSFORM_WITHOUT_CACHE_SHA256,
        TRANSFORM_WITH_HALO_SHA256,
    ):
        if needle not in build:
            raise AssertionError(f"Transform native-only wiring lost {needle}")
    if build.index('apply_source_patch "$coloroffset_transform_uniform_patch"') < build.index(
            'apply_source_patch "$laser_light_nearest_patch"'):
        raise AssertionError("Transform patch must follow the optional halo/nearest chain")
    matrix_pattern = re.compile(
        r"(?m)^    (0:0:0|1:0:0|1:1:0|1:1:1)\)\n"
        r"        shader_source_sha=([0-9a-f]{64})\n"
        r"        shader_header_sha=([0-9a-f]{64})$"
    )
    actual = {
        name: (source_hash, header_hash)
        for name, source_hash, header_hash in matrix_pattern.findall(build)
    }
    if actual != EXPECTED_HASHES:
        raise AssertionError(
            f"build.sh shader hash matrix drifted: {actual} != {EXPECTED_HASHES}"
        )
    if build.count("git -c core.autocrlf=false -c core.eol=lf") != 2:
        raise AssertionError("build.sh lost the canonical-LF git apply helper")
    for patch_name in ("contract", "gpu_draw", "coloroffset",
                       "coloroffset_fs_probe", "shader_cache",
                       "scene_timer", "scene_split", "fbo_rt_scenes",
                       "fbo_valid_region"):
        needle = f'apply_source_patch "${patch_name}_patch"'
        if needle not in build:
            raise AssertionError(f"build.sh bypasses the patch helper: {needle}")
    # 0008 is written against the 0003 tree and must precede 0004 (its hunks
    # are disjoint from 0004's; prove_combination applies them in this order).
    probe_order = [build.index(f'apply_source_patch "${name}_patch"')
                   for name in ("coloroffset", "coloroffset_fs_probe",
                                "shader_cache")]
    if probe_order != sorted(probe_order):
        raise AssertionError(
            "build.sh applies the ColorOffset FS probe out of order")
    # build.sh applies the gxm.c patches in this order; the matrix below
    # proves the same order, the standalone 0007 application and the 0009
    # legs (with and without 0007, both orders, refused without 0005/0006).
    order = [build.index(f'apply_source_patch "${name}_patch"')
             for name in ("scene_timer", "scene_split", "fbo_rt_scenes",
                          "fbo_valid_region")]
    if order != sorted(order):
        raise AssertionError("build.sh applies the gxm.c patches out of order")
    if 'if [ "$fbo_valid_region" = 1 ] && [ "$gl_time_profile" != 1 ]; then' \
            not in build:
        raise AssertionError("build.sh lost the 0009 -> GL_TIME_PROFILE constraint")
    if ('coloroffset_staging_proof=${ISAAC_COLOROFFSET_STAGING_PROOF:-0}' not in build or
            '[ "$coloroffset_staging_proof" = 1 ] && [ "$coloroffset_fs_probe" != 3 ]' not in build):
        raise AssertionError("staging proof lost its default-off / neutral-only condition")
    if build.index('apply_source_patch "$coloroffset_staging_patch"') < probe_order[-1]:
        raise AssertionError("staging proof must follow the optional shader cache")
    if f"shader_source_sha={STAGING_WITH_CACHE_SHA256}" not in build:
        raise AssertionError("staging proof shader-cache source hash drifted")
    for needle in (
        'coloroffset_single_final_bind=${ISAAC_COLOROFFSET_SINGLE_FINAL_BIND:-0}',
        '[ "$coloroffset_staging_proof" != 1 ] || [ "$coloroffset_fs_probe" != 3 ]',
        'apply_source_patch "$coloroffset_single_final_bind_patch"',
        '-DHAVE_ISAAC_COLOROFFSET_SINGLE_FINAL_BIND=1',
        f'shader_source_sha={SINGLE_BIND_WITH_CACHE_SHA256}',
    ):
        if needle not in build:
            raise AssertionError(f"single final fragment bind lost {needle}")
    if build.index('apply_source_patch "$coloroffset_single_final_bind_patch"') < build.index(
            'apply_source_patch "$coloroffset_staging_patch"'):
        raise AssertionError("single final bind must follow staging proof")
    if ('coloroffset_plain_fastpath=${ISAAC_COLOROFFSET_PLAIN_FASTPATH:-0}' not in build or
            '[ "$coloroffset_plain_fastpath" = 1 ] && [ "$coloroffset_staging_proof" != 1 ]' not in build):
        raise AssertionError("plain specialization lost default-OFF / staging dependency")
    plain_order = [build.index(f'apply_source_patch "${name}_patch"')
                   for name in ("coloroffset_staging", "coloroffset_plain",
                                "coloroffset_single_final_bind")]
    if plain_order != sorted(plain_order):
        raise AssertionError("plain/final-bind specialization must follow 0012 -> 0013 -> 0014")
    if f"shader_source_sha={PLAIN_WITH_CACHE_SHA256}" not in build:
        raise AssertionError("plain shader-cache source hash drifted")
    if f"shader_source_sha={PLAIN_SINGLE_BIND_WITH_CACHE_SHA256}" not in build:
        raise AssertionError("plain plus single final bind shader-cache source hash drifted")
    for needle in (
        'coloroffset_plain_vertex_pair=${ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR:-0}',
        '[ "$coloroffset_plain_fastpath" != 1 ] || [ "$coloroffset_single_final_bind" != 1 ]',
        'apply_source_patch "$coloroffset_plain_vertex_pair_patch"',
        '-DHAVE_ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR=1',
        f'shader_source_sha={PLAIN_VERTEX_PAIR_WITH_CACHE_SHA256}',
    ):
        if needle not in build:
            raise AssertionError(f"plain vertex pair lost {needle}")
    if build.index('apply_source_patch "$coloroffset_plain_vertex_pair_patch"') < plain_order[-1]:
        raise AssertionError("plain vertex pair must follow 0014")
    for name in ("p8_safe", "laser_p8"):
        if f'apply_source_patch "${name}_patch"' not in build:
            raise AssertionError(f"missing native P8 patch application: {name}")
    if build.index('apply_source_patch "$laser_p8_patch"') < build.index(
            'apply_source_patch "$p8_safe_patch"'):
        raise AssertionError("laser P8 patch must follow its safety prerequisite")
    for needle in (
        'laser_p8_swizzle=${ISAAC_LASER_P8_SWIZZLE:-0}',
        'if [ "$laser_p8_swizzle" = 1 ] && [ "$laser_atlas_p8" != 1 ]; then',
        'build_flags="$build_flags HAVE_ISAAC_LASER_P8_SWIZZLE=1 SUPPORT_SMALL_FMT=1"',
        'extra_cflags="$extra_cflags -DHAVE_ISAAC_LASER_P8_SWIZZLE=1 -DSUPPORT_SMALL_FMT=1"',
        '"$laser_p8_reference_sha" "$laser_p8_swizzle"',
        "printf 'laser_p8_swizzle=%s\\n'",
    ):
        if needle not in build:
            raise AssertionError(f"laser P8 swizzle lost {needle}")
    for needle in (
        'laser_light_nearest=${ISAAC_LASER_LIGHT_NEAREST:-0}',
        'if [ "$laser_light_nearest" = 1 ] && [ "$laser_light_halo_clip" != 1 ]; then',
        'build_flags="$build_flags HAVE_ISAAC_LASER_LIGHT_NEAREST=1"',
        'extra_cflags="$extra_cflags -DHAVE_ISAAC_LASER_LIGHT_NEAREST=1"',
        '"$laser_light_halo_integration_sha" "$laser_light_nearest"',
        'cp -- "$laser_light_nearest_policy" "$source_dir/source/isaac_laser_light_nearest.h"',
        "printf 'laser_light_nearest=%s\\n'",
    ):
        if needle not in build:
            raise AssertionError(f"light laser nearest lost {needle}")
    if build.index('apply_source_patch "$laser_light_nearest_patch"') < build.index(
            'apply_source_patch "$laser_light_halo_patch"'):
        raise AssertionError("light laser nearest patch must follow halo clipping")
    for needle in (
        'laser_light_halo_depth_approx=${ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX:-0}',
        '"laser_light_halo_depth_approx:$laser_light_halo_depth_approx"',
        'if [ "$laser_light_halo_depth_approx" = 1 ] && [ "$laser_light_halo_clip" != 1 ]; then',
        'build_flags="$build_flags HAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX=1"',
        'extra_cflags="$extra_cflags -DHAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX=1"',
        '"$laser_light_halo_depth_approx" \\\n',
        "printf 'laser_light_halo_depth_approx=%s\\n'",
        'verify_sha "$source_dir/source/isaac_laser_light_halo_vitagl.h" "$laser_light_halo_integration_sha"',
    ):
        if needle not in build:
            raise AssertionError(f"light halo depth approximation lost {needle}")
    halo = (recipe / "isaac_laser_light_halo_vitagl.h").read_text(encoding="utf-8")
    if ("#if !defined(HAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX) || "
            "HAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX != 1\n"
            "            depth_test_state ||\n#endif\n"
            "            stencil_test_state || !blend_state ||") not in halo:
        raise AssertionError("depth approximation changed more than its explicit default-OFF veto")
    if "depth-approx=1 depth-test=%u" not in halo:
        raise AssertionError("depth approximation lost its existing success-line engagement suffix")
    for needle in (
        'laser_red_cap_side_clip=${ISAAC_LASER_RED_CAP_SIDE_CLIP:-0}',
        '"laser_red_cap_side_clip:$laser_red_cap_side_clip"',
        'if [ "$laser_red_cap_side_clip" = 1 ] && [ "$laser_light_halo_clip" != 1 ]; then',
        'build_flags="$build_flags HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP=1"',
        'extra_cflags="$extra_cflags -DHAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP=1"',
        '"$laser_red_cap_side_clip"',
        "printf 'laser_red_cap_side_clip=%s\\n'",
        'laser_light_producer_uv=${ISAAC_LASER_LIGHT_PRODUCER_UV:-0}',
        '"laser_light_producer_uv:$laser_light_producer_uv"',
        'if [ "$laser_light_producer_uv" = 1 ] && [ "$laser_light_halo_clip" != 1 ]; then',
        'build_flags="$build_flags HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV=1"',
        'extra_cflags="$extra_cflags -DHAVE_ISAAC_LASER_LIGHT_PRODUCER_UV=1"',
        '"$laser_light_producer_uv"',
        "printf 'laser_light_producer_uv=%s\\n'",
    ):
        if needle not in build:
            raise AssertionError(f"red cap side option lost {needle}")


def check_laser_reference(recipe: Path) -> None:
    """Decode the actual compiled reference, pin full RGBA rather than a name."""
    source = (recipe / "isaac_laser_p8_reference.h").read_text(encoding="utf-8")
    palette_text = source.split("isaac_laser_p8_palette[188] = {", 1)[1].split("};", 1)[0]
    runs_text = source.split("isaac_laser_p8_runs[] = {", 1)[1].split("};", 1)[0]
    palette = [int(x, 16) for x in re.findall(r"0x([0-9a-f]+)U", palette_text)]
    runs = [int(x, 16) for x in re.findall(r"0x([0-9a-f]+)U", runs_text)]
    assert len(palette) == 188 and palette[187] == 0
    assert len(set(palette[:187])) == 187
    assert all(0 < (run >> 8) <= 255 and (run & 255) < 187 for run in runs)
    rgba = b"".join(struct.pack("<I", palette[run & 255]) * (run >> 8)
                    for run in runs)
    assert len(rgba) == 448 * 64 * 4
    assert hashlib.sha256(rgba).hexdigest() == (
        "7d9a0e2d16d2cac0dc16a8b1fa2372e0954f3b3d21d9517724a74ac7a6988063")
    # Golden outputs use the actual frozen PE lookup bytes, not an inferred
    # multiplication/gamma formula (sub_005a0e10 RVA 0x5a1480..0x5a14e5).
    for mode, expected in (
        ("gamma", "85cd457e68b2ee6cf0ee2fe3454383238125294eead17c4c5842d564f27538c6"),
        ("linear", "6e481968e9d646086ae1710baec8703483a9fbe54bb805644d0f1d55f12af0ac"),
    ):
        part = source.split(f"isaac_laser_p8_premul_{mode}[188] = {{", 1)[1].split("};", 1)[0]
        converted = [int(x, 16) for x in re.findall(r"0x([0-9a-f]+)U", part)]
        assert len(converted) == 188 and converted[187] == 0
        rgba = b"".join(struct.pack("<I", converted[run & 255]) * (run >> 8)
                        for run in runs)
        assert len(rgba) == 448 * 64 * 4 and hashlib.sha256(rgba).hexdigest() == expected
    print("Exact laser atlas reference: raw/two frozen premultiply RGBA identities PASS")


def check_laser_lifecycle(source: Path, texture_before: str) -> None:
    """Inspect freshly patched native owners, not an unrelated wrapper mock."""
    textures = (source / "source/textures.c").read_text(encoding="utf-8")
    framebuffers = (source / "source/framebuffers.c").read_text(encoding="utf-8")
    gpu = (source / "source/utils/gpu_utils.h").read_text(encoding="utf-8")

    def body(text: str, name: str) -> str:
        start = re.search(r"(?m)^.*\b" + re.escape(name) + r"\([^\n]*\) \{", text)
        assert start, name
        return text[start.end():].split("\n}\n", 1)[0]

    for name, later in (
        ("glTexImage2D", "#ifndef SKIP_ERROR_HANDLING"),
        ("glTextureImage2D", "#ifndef SKIP_ERROR_HANDLING"),
        ("_glTexImage2D_CubeIMPL", "SceGxmTextureFormat tex_format"),
        ("_glTexImage2D_FlatIMPL", "SceGxmTextureFormat tex_format"),
        ("_glTexSubImage2D", "isaac_p8_promote_rgba_subimage"),
        ("_glCompressedTexImage2D", "isaac_p8_image_level0"),
        ("glGenerateMipmap", "gpu_alloc_mipmaps"),
        ("glGenerateTextureMipmap", "gpu_alloc_mipmaps"),
    ):
        value = body(textures, name)
        assert value.index("isaacLaserP8Restore(tex)") < value.index(later), name
    for name, later in (
        ("vglGetTexDataPointer", "return tex->data"),
        ("vglOverloadTexDataPointer", "tex->data = data"),
        ("vglGetGxmTexture", "return &tex->gxm_tex"),
    ):
        value = body(textures, name)
        assert value.index("isaac_laser_p8_escape(tex)") < value.index(later), name
    for name in ("glTexImage2D", "glTextureImage2D"):
        value = body(textures, name)
        assert value.index("isaac_laser_p8_try_upload") < value.index("isaacLaserP8Restore")
    for name in ("_glTexParameterx", "_glTexParameteri"):
        value = body(textures, name)
        assert value.index("isaac_laser_p8_parameter") < value.index("switch (target)")
    for name in ("glFramebufferTexture2D", "glNamedFramebufferTexture2D"):
        value = body(framebuffers, name)
        assert value.index("isaacLaserP8Restore") < value.index("_glFramebufferTexture2D(")
    assert "isaacLaserP8Restore" in body(framebuffers, "vglTexImageDepthBuffer")
    assert "isaacLaserP8Restore" in body(textures, "glBindTexture")
    assert "isaacLaserP8Restore" in body(textures, "glBindSampler")
    value = body(gpu, "gpu_free_texture_data")
    assert value.index("isaacLaserP8ReleaseBackup") < value.index("tex->data")
    # No name-allocation algorithm is added or replaced. The common free
    # hook owns backup retirement even if native deletion becomes deferred.
    for name in ("glGenTextures", "glDeleteTextures"):
        assert body(textures, name) == body(texture_before, name), name


GXM_PATCHES = {
    "scene_timer": "0005-isaac-scene-timer.patch",
    "scene_split": "0006-isaac-scene-split.patch",
    "fbo_rt_scenes": "0007-isaac-fbo-rt-scenes.patch",
    "fbo_valid_region": "0009-isaac-fbo-valid-region.patch",
}
# Source-text evidence mirrored from build.sh: present iff the flag is 1.
GL_TIME_PROFILE_NEEDLES = (
    b"HAVE_ISAAC_GL_TIME_PROFILE", b"vglIsaacSceneTimes",
    b"ISAAC_SCENE_TIME_END_BEGIN_SCENE", b"vglIsaacSceneWaits",
    b"ISAAC_SCENE_TIME_RT_CREATED", b"ISAAC_SCENE_TIME_DEPTH_CREATED",
    b"ISAAC_SCENE_TIME_SWAP", b"isaac_scene_end_ordinal_us",
)
FBO_RT_SCENES_NEEDLES = (
    b"HAVE_ISAAC_FBO_RT_SCENES", b"gxm_fbo_rt_size",
    b"active_write_fb->height, ISAAC_FBO_RT_SCENES());",
    b"vglIsaacSetupFboRenderTargetScenes",
)
# 0009 edits gxm.c (region derivation, BeginScene argument, hooks), misc.c
# (guest glViewport record) and tests.c (tile-clipper clamp); every needle is
# present iff the flag is 1, and shared.h/vitaGL.h never see its names.
FBO_VALID_REGION_NEEDLES = (
    b"HAVE_ISAAC_FBO_VALID_REGION", b"isaac_fbo_region_for_scene",
    b"isaac_fbo_region_known", b"isaac_fbo_region_scene_reset_pending",
    b"ISAAC_FBO_REGION_RETRY_NULL", b"isaacFboRegionNoteViewport",
    b"isaacFboRegionClampClip", b"vglIsaacSetupFboValidRegion",
    b"vglIsaacFboValidRegionStats",
)
FBO_VALID_REGION_MISC_NEEDLE = b"isaacFboRegionNoteViewport(x, y, width, height);"
FBO_VALID_REGION_TESTS_NEEDLE = (
    b"ISAAC_FBO_REGION_CLAMP_CLIP(clip_x_min, clip_y_min, clip_x_max, clip_y_max);"
)
CONSUMER_DECLARED_HOOKS = (
    b"vglIsaacSceneTimes", b"vglIsaacSceneWaits",
    b"vglIsaacSetupFboRenderTargetScenes",
    b"vglIsaacSetupFboValidRegion", b"vglIsaacFboValidRegionStats",
    b"isaacFboRegion",
)


def check_gxm(
    source: Path, gl_time_profile: bool, fbo_rt_scenes: bool,
    fbo_valid_region: bool = False,
) -> bytes:
    gxm = (source / "source" / "gxm.c").read_bytes()
    misc = (source / "source" / "misc.c").read_bytes()
    tests = (source / "source" / "tests.c").read_bytes()
    for name, text in (("gxm.c", gxm), ("misc.c", misc), ("tests.c", tests)):
        if b"\r\n" in text:
            raise AssertionError(f"{name} patch chain introduced CRLF")
    for flag, needles in ((gl_time_profile, GL_TIME_PROFILE_NEEDLES),
                          (fbo_rt_scenes, FBO_RT_SCENES_NEEDLES),
                          (fbo_valid_region, FBO_VALID_REGION_NEEDLES)):
        for needle in needles:
            if (needle in gxm) != flag:
                raise AssertionError(
                    f"gxm.c evidence {needle!r}: present={needle in gxm}, "
                    f"expected {flag}"
                )
    if (FBO_VALID_REGION_MISC_NEEDLE in misc) != fbo_valid_region or \
            (b"isaacFboRegion" in misc) != fbo_valid_region:
        raise AssertionError(
            f"misc.c valid-region call site: expected {fbo_valid_region}")
    if (FBO_VALID_REGION_TESTS_NEEDLE in tests) != fbo_valid_region or \
            (b"isaacFboRegion" in tests) != fbo_valid_region:
        raise AssertionError(
            f"tests.c valid-region clamp sites: expected {fbo_valid_region}")
    if fbo_valid_region and tests.count(FBO_VALID_REGION_TESTS_NEEDLE) != 2:
        raise AssertionError(
            "tests.c must clamp both the full-framebuffer and the scissor clip")
    header = (source / "source" / "vitaGL.h").read_bytes()
    shared = (source / "source" / "shared.h").read_bytes()
    for needle in CONSUMER_DECLARED_HOOKS:
        if needle in header:
            raise AssertionError(f"consumer-declared hook leaked into vitaGL.h: {needle!r}")
        if needle in shared:
            raise AssertionError(f"consumer-declared hook leaked into shared.h: {needle!r}")
    return gxm + misc + tests


def refuse_patch(source: Path, patch: Path) -> None:
    """The patch must NOT apply on this tree (missing context)."""
    completed = subprocess.run(
        ["git", "-c", "core.autocrlf=false", "-c", "core.eol=lf", "apply",
         "--check", str(patch)],
        cwd=source, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    if completed.returncode == 0:
        raise AssertionError(f"{patch.name} unexpectedly applies on {source.name}")


def prove_gxm_matrix(
    recipe: Path,
    temporary: Path,
    stock_tar: Path | None,
    repository_archive: bytes | None,
    cc: str | None = None,
) -> None:
    """Every GL_TIME_PROFILE x FBO_RT_SCENES combination applies on the 0002
    tree in build.sh order (0005, 0006, 0007); 0007 alone applies too, and the
    two groups commute (their gxm.c hunks are disjoint).  The 0009 valid
    region applies on every GL_TIME_PROFILE=1 tree with and without 0007, in
    both orders with identical gxm.c/misc.c/tests.c, and is refused on the
    trees without the 0005/0006 BeginScene brackets it needs as context."""
    orders = {
        "gl0-fbo0": (),
        "gl1-fbo0": ("scene_timer", "scene_split"),
        "gl0-fbo1": ("fbo_rt_scenes",),
        "gl1-fbo1": ("scene_timer", "scene_split", "fbo_rt_scenes"),
        "gl1-fbo1-reverse": ("fbo_rt_scenes", "scene_timer", "scene_split"),
        "gl1-fbo0-vr1": ("scene_timer", "scene_split", "fbo_valid_region"),
        "gl1-fbo1-vr1": ("scene_timer", "scene_split", "fbo_rt_scenes",
                         "fbo_valid_region"),
        "gl1-fbo1-vr1-reverse": ("scene_timer", "scene_split",
                                 "fbo_valid_region", "fbo_rt_scenes"),
    }
    results = {}
    for name, order in orders.items():
        source = materialize_stock(
            temporary / ("gxm-" + name), stock_tar, repository_archive
        )
        apply_patch(source, recipe / "0001-deterministic-build-and-init-oob.patch")
        apply_patch(source, recipe / "0002-exact-gpu-draw-optimizations.patch")
        for policy in ("isaac_gpu_draw_policy.h", "isaac_gxm_state_policy.h"):
            shutil.copyfile(recipe / policy, source / "source" / policy)
        for patch_name in order:
            apply_patch(source, recipe / GXM_PATCHES[patch_name])
        if "fbo_valid_region" not in order:
            # Negative leg: without 0005/0006 the 0009 context is absent.
            if "scene_split" not in order:
                refuse_patch(source, recipe / GXM_PATCHES["fbo_valid_region"])
                print(f"vitaGL gxm.c patch matrix {name}: 0009 refused (no 0005/0006)")
        results[name] = check_gxm(
            source, "scene_split" in order, "fbo_rt_scenes" in order,
            "fbo_valid_region" in order,
        )
        if cc and name == "gl1-fbo1-vr1":
            check_fbo_region_observe(source, cc)
        digest = hashlib.sha256(results[name]).hexdigest()
        print(f"vitaGL gxm.c patch matrix {name}: {digest}")
        resource_patch = recipe / "0020-isaac-native-resource-profile.patch"
        assert b"isaacNativeResource" not in results[name]
        if "scene_split" in order:
            if name == "gl1-fbo1-vr1":
                apply_patch(source, recipe / "0010-isaac-p8-safe-upload.patch")
                apply_patch(source, recipe / "0011-isaac-exact-laser-p8.patch")
            apply_patch(source, resource_patch)
            patched = (source / "source/gxm.c").read_text(encoding="utf-8")
            assert patched.count("isaacNativeResourceSceneBegin(") == 1
            assert patched.count("isaacNativeResourceSceneEnd(") == 1
            assert patched.count("isaacNativeResourceGcObject();") == 1
            assert "active_write_fb ? active_write_fb->width : DISPLAY_WIDTH" in patched
            assert "isaacNativeResourceDepth(surface->depthData, w, h," in patched
            # The optional profile composes after both texture patches too.
            if name != "gl1-fbo1-vr1":
                apply_patch(source, recipe / "0010-isaac-p8-safe-upload.patch")
                apply_patch(source, recipe / "0011-isaac-exact-laser-p8.patch")
            print(f"native resource profile {name}: 0020 + safe/laser P8 apply PASS")
            if cc and name == "gl1-fbo1-vr1":
                check_native_resource_profile(recipe, source, cc)
        else:
            refuse_patch(source, resource_patch)
    if results["gl1-fbo1"] != results["gl1-fbo1-reverse"]:
        raise AssertionError("0005/0006 and 0007 do not commute on gxm.c")
    if results["gl1-fbo1-vr1"] != results["gl1-fbo1-vr1-reverse"]:
        raise AssertionError("0007 and 0009 do not commute on gxm.c/misc.c/tests.c")


def prove_fbo_float_sync(
    recipe: Path, temporary: Path, stock_tar: Path | None, cc: str | None,
    repository_archive: bytes | None = None,
) -> None:
    """Fresh current source; execute actual reset/attach bodies, not a model."""
    patch = recipe / "0028-sync-fbo-float-selector.patch"
    build = (recipe / "build.sh").read_text(encoding="utf-8")
    assert build.count('apply_source_patch "$fbo_float_sync_patch"') == 1
    assert build.index('apply_source_patch "$fbo_float_sync_patch"') > build.index(
        'apply_source_patch "$laser_halo_profile_patch"')
    prefix = build[build.index("emit_recipe_inputs()\n{"):build.index("emit_sdk_sparse_recipe_inputs()")]
    assert '"$fbo_float_sync_patch_sha"' in prefix
    assert "printf 'fbo_float_selector_sync=1\\n'" in build
    assert patch.name in (recipe.parent / "CMakeLists.txt").read_text(encoding="utf-8")

    # This is the current CONTROL/LEAN source order; proven emission changes
    # only a private header. The copied source is regenerated on every run.
    source = materialize_stock(temporary / "fbo-float-sync", stock_tar, repository_archive)
    by_number = {int(p.name[:4]): p for p in recipe.glob("[0-9][0-9][0-9][0-9]-*.patch")}
    for number in (1, 2, 3, 8, 4, 12, 13, 14, 15, 7, 10, 11, 16, 17,
                   18, 21, 24, 22, 23, 26):
        apply_patch(source, by_number[number])
    original = (source / "source/gxm.c").read_text(encoding="utf-8")
    shared = (source / "source/shared.h").read_text(encoding="utf-8")
    # Actual-built RT LEAN source retained through immutable 4013c97.
    assert sha256(source / "source/gxm.c") == "a2f9fc852a3d0591dfcba5eee0d326142e06f1936cf88d43ecf94eeb2442268b"
    assert sha256(source / "source/shared.h") == "256854bfb32957a215b03e1bfd5ed7ff6f7c68c0ae0c1c9a36c76a339526d0e8"
    before = {p.relative_to(source): sha256(p) for p in (source / "source").rglob("*") if p.is_file()}
    apply_patch(source, patch)
    fixed = (source / "source/gxm.c").read_text(encoding="utf-8")
    changed = {name for name, digest in before.items() if sha256(source / name) != digest}
    assert changed == {Path("source/gxm.c")}
    addition = (
        "\t\t\t/* Lazy attachment sync may change the color-surface register size.\n"
        "\t\t\t * Match its shader selector before depth/RT setup or any draw. */\n"
        "\t\t\tis_fbo_float = active_write_fb->is_float;\n\n")
    assert fixed.count(addition) == 1 and fixed.replace(addition, "") == original
    print("FBO float sync: fresh current gxm/shared match actual-built authority; only gxm changed")

    # Same balanced-brace extraction as the existing RT lease native fixture.
    def function(text, signature):
        start = text.index(signature)
        end = text.index("{", start) + 1
        depth = 1
        while depth:
            depth += (text[end] == "{") - (text[end] == "}")
            end += 1
        return text[start:end] + "\n"

    if cc:
        common = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(source)]
        fixture = recipe.parent / "host_tests/vitagl_fbo_float_sync_host_test.c"
        for label, gxm, expect, fast in (("baseline", original, 0, False),
                                          ("fixed", fixed, 1, False),
                                          ("fixed-fast", fixed, 1, True),
                                          ("negative-old-selector", original, 1, False)):
            native = function(shared, "static inline __attribute__((always_inline)) void _glFramebufferTexture2D(")
            native += function(gxm, "static inline __attribute__((always_inline)) void scene_end(")
            native += function(gxm, "void scene_reset(") + function(gxm, "void glFinish(")
            (source / "native_fbo_float_sync.h").write_text(native, encoding="utf-8")
            executable = source / (label + ".exe")
            flags = ["-ffast-math"] if fast else []
            run([*common, *flags, f"-DEXPECT_FIXED={expect}", str(fixture), "-o", str(executable)])
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            if label == "negative-old-selector":
                assert result.returncode != 0 and "is_fbo_float == expected_selector" in result.stderr, result
                print("FBO float sync old-selector negative: rejected at first BeginScene selector check")
            else:
                assert result.returncode == 0, result.stderr
                print(result.stdout.strip())
        # Leave the fixed native bodies beside the fixed executable artifacts.
        native = function(shared, "static inline __attribute__((always_inline)) void _glFramebufferTexture2D(")
        native += function(fixed, "static inline __attribute__((always_inline)) void scene_end(")
        native += function(fixed, "void scene_reset(") + function(fixed, "void glFinish(")
        (source / "native_fbo_float_sync.h").write_text(native, encoding="utf-8")

    # The unconditional last patch also composes with bare stock and the
    # existing diagnostic/valid-region chain. No extra host profile matrix.
    for label, numbers in (("bare", (1,)),
                           ("diag", (1, 2, 3, 8, 4, 12, 13, 14, 15, 5, 6, 7,
                                     10, 9, 11, 16, 17, 18, 20, 21, 24, 22, 23, 26, 25, 27))):
        tree = materialize_stock(temporary / ("fbo-float-" + label), stock_tar, repository_archive)
        for number in numbers:
            apply_patch(tree, by_number[number])
        prior = (tree / "source/gxm.c").read_text(encoding="utf-8")
        apply_patch(tree, patch)
        assert (tree / "source/gxm.c").read_text(encoding="utf-8").replace(addition, "") == prior
        print(f"FBO float sync {label} source composition: PASS")


def prove_fbo_scissor_resize(
    recipe: Path, temporary: Path, stock_tar: Path | None, cc: str | None,
    repository_archive: bytes | None = None,
) -> None:
    """Fresh attachment/reset/scissor functions; mocked SDK, no GPU claim."""
    from test_vitagl_rt_lease import function
    if stock_tar:
        assert sha256(stock_tar) == SOURCE_ARCHIVE_SHA256
    patch = recipe / "0029-invalidate-resized-fbo-scissor.patch"
    build = (recipe / "build.sh").read_text(encoding="utf-8")
    assert build.count('apply_source_patch "$fbo_scissor_resize_patch"') == 1
    assert build.index('apply_source_patch "$fbo_scissor_resize_patch"') > build.index(
        'apply_source_patch "$laser_halo_profile_patch"')
    prefix = build[build.index("emit_recipe_inputs()\n{"):build.index("emit_sdk_sparse_recipe_inputs()")]
    assert '"$fbo_scissor_resize_patch_sha"' in prefix
    assert "printf 'fbo_scissor_resize_invalidation=1\\n'" in build
    assert patch.name in (recipe.parent / "CMakeLists.txt").read_text(encoding="utf-8")
    source = materialize_stock(temporary / "fbo-scissor-resize", stock_tar, repository_archive)
    patches = {int(p.name[:4]): p for p in recipe.glob("[0-9][0-9][0-9][0-9]-*.patch")}
    for number in (1, 2, 3, 8, 4, 12, 13, 14, 15, 7, 10, 11, 16, 17,
                   18, 21, 24, 22, 23, 26):
        apply_patch(source, patches[number])
    path = source / "source/shared.h"
    original = path.read_text(encoding="utf-8")
    assert sha256(path) == "256854bfb32957a215b03e1bfd5ed7ff6f7c68c0ae0c1c9a36c76a339526d0e8"
    assert sha256(source / "source/gxm.c") == "a2f9fc852a3d0591dfcba5eee0d326142e06f1936cf88d43ecf94eeb2442268b"
    before = {p.relative_to(source): sha256(p) for p in (source / "source").rglob("*") if p.is_file()}
    apply_patch(source, patch)
    fixed = path.read_text(encoding="utf-8")
    assert {p for p, digest in before.items() if sha256(source / p) != digest} == {Path("source/shared.h")}
    addition = (
        "\t\t\t/* Same-object resize changes the cached, attachment-clipped box.\n"
        "\t\t\t * Rebuild after BeginScene, not inside this attachment helper. */\n"
        "\t\t\tif (fb == in_use_framebuffer && scissor_test_state)\n"
        "\t\t\t\tdirty_scissor_state = GL_TRUE;\n")
    assert fixed.count(addition) == 1 and fixed.replace(addition, "") == original
    print("FBO scissor: fresh current gxm/shared match actual-built authority; only four shared.h lines added")
    gxm = (source / "source/gxm.c").read_text(encoding="utf-8")
    tests = (source / "source/tests.c").read_text(encoding="utf-8")
    framebuffers = (source / "source/framebuffers.c").read_text(encoding="utf-8")
    # These complete functions are compiled unchanged, including native reset
    # ordering and actual coordinate/clip calculation. Only SDK/allocator edges
    # and the upload's published descriptor are mocked in the existing style.
    tail = function(gxm, "static inline __attribute__((always_inline)) void scene_end(")
    tail += function(tests, "void update_scissor_test(") + function(tests, "void glScissor(")
    tail += function(framebuffers, "void glFramebufferTexture2D(")
    tail += function(gxm, "void scene_reset(") + function(gxm, "void glFlush(") + function(gxm, "void glFinish(")
    fixture = recipe.parent / "host_tests/vitagl_fbo_scissor_resize_host_test.c"
    header = source / "native_fbo_scissor_resize.h"
    if cc:
        for label, shared, expect in (("baseline", original, 0), ("fixed", fixed, 1),
                                       ("negative-original", original, 1)):
            native = function(shared, "static inline __attribute__((always_inline)) void _glFramebufferTexture2D(") + tail
            header.write_text(native, encoding="utf-8")
            exe = source / (label + ".exe")
            # Stock scissor uses signed int coordinates versus uint32 bounds;
            # retain those exact expressions, and the unused public level arg.
            run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-parameter", "-Wno-sign-compare",
                 f"-DEXPECT_FIXED={expect}", "-I", str(source), str(fixture), "-o", str(exe)])
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            if label == "negative-original":
                assert result.returncode != 0 and "mask_draws == (unsigned)(EXPECT_FIXED && enabled ? 2 : 0)" in result.stderr, result
                print("FBO scissor old-body negative: rejected at missing enabled-resize mask rebuild")
            else:
                assert result.returncode == 0, result.stderr
                print(result.stdout.strip())
        header.write_text(function(fixed, "static inline __attribute__((always_inline)) void _glFramebufferTexture2D(") + tail,
                          encoding="utf-8")
    # Mandatory patch must also compose without optional RT reuse, and with
    # current observer/valid-region edits. This is source-only, not a matrix.
    for label, numbers in (("bare", (1,)),
                           ("diag", (1, 2, 3, 8, 4, 12, 13, 14, 15, 5, 6, 7,
                                     10, 9, 11, 16, 17, 18, 20, 21, 24, 22, 23, 26, 25, 27))):
        tree = materialize_stock(temporary / ("fbo-scissor-" + label), stock_tar, repository_archive)
        for number in numbers:
            apply_patch(tree, patches[number])
        prior = (tree / "source/shared.h").read_text(encoding="utf-8")
        apply_patch(tree, patch)
        assert (tree / "source/shared.h").read_text(encoding="utf-8").replace(addition, "") == prior
        print(f"FBO scissor {label} source composition: PASS")


def prove_fbo_scissor_replay(
    recipe: Path, temporary: Path, stock_tar: Path | None, cc: str | None,
    repository_archive: bytes | None = None,
) -> None:
    """Reuse the scissor fixture for first-update versus no-resize replay."""
    from test_vitagl_rt_lease import function
    if stock_tar:
        assert sha256(stock_tar) == SOURCE_ARCHIVE_SHA256
    patches = {int(p.name[:4]): p for p in recipe.glob("[0-9][0-9][0-9][0-9]-*.patch")}
    build = (recipe / "build.sh").read_text(encoding="utf-8")
    selection = ('if [ "$fbo_valid_region" = 1 ]; then\n'
                 '        apply_source_patch "$fbo_scissor_replay_region_patch"\n'
                 '    else\n        apply_source_patch "$fbo_scissor_replay_patch"\n    fi')
    assert selection in build and build.index(selection) > build.index(
        'apply_source_patch "$fbo_scissor_resize_patch"')
    prefix = build[build.index("emit_recipe_inputs()\n{"):build.index("emit_sdk_sparse_recipe_inputs()")]
    for variable, number in (("fbo_scissor_replay", 30), ("fbo_scissor_replay_region", 31)):
        assert f'"${variable}_patch_sha"' in prefix
        assert f"printf '{variable}_patch_sha256=%s\\n'" in build
        assert patches[number].name in (recipe.parent / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "printf 'fbo_scissor_replay_sync=1\\n'" in build
    core = (1, 2, 3, 8, 4, 12, 13, 14, 15, 7, 10, 11, 16, 17, 18, 21, 24, 22, 23, 26, 29)
    diag = (1, 2, 3, 8, 4, 12, 13, 14, 15, 5, 6, 7, 10, 9, 11, 16, 17, 18,
            20, 21, 24, 22, 23, 26, 25, 27, 29)
    fixture = recipe.parent / "host_tests/vitagl_fbo_scissor_resize_host_test.c"
    for label, numbers, number in (("bare", (1, 29), 30), ("control", core, 30), ("valid-region", diag, 31)):
        source = materialize_stock(temporary / ("scissor-replay-" + label), stock_tar, repository_archive)
        for n in numbers:
            apply_patch(source, patches[n])
        path = source / "source/gxm.c"
        original = path.read_text(encoding="utf-8")
        if label == "control":
            assert sha256(path) == "a2f9fc852a3d0591dfcba5eee0d326142e06f1936cf88d43ecf94eeb2442268b"
        before = {p.relative_to(source): sha256(p) for p in (source / "source").rglob("*") if p.is_file()}
        apply_patch(source, patches[number])
        fixed = path.read_text(encoding="utf-8")
        assert {p for p, digest in before.items() if sha256(source / p) != digest} == {Path("source/gxm.c")}
        old_reset = function(original, "void scene_reset(")
        new_reset = function(fixed, "void scene_reset(")
        assert fixed.replace(new_reset, old_reset) == original
        print(f"FBO scissor replay {label}: fresh source composition, only scene_reset changed; gxm={sha256(path)}")
        if label == "bare" or not cc:
            continue
        shared = (source / "source/shared.h").read_text(encoding="utf-8")
        tests = (source / "source/tests.c").read_text(encoding="utf-8")
        misc = (source / "source/misc.c").read_text(encoding="utf-8")
        framebuffers = (source / "source/framebuffers.c").read_text(encoding="utf-8")
        native_prefix = ""
        if number == 31:
            # Use the actual 0009 policy/ClampClip, not a modeled clamp. Its
            # complete block also retains viewport ownership and retry logic.
            start = fixed.index("#ifdef HAVE_ISAAC_FBO_VALID_REGION\n/* Isaac FBO valid region")
            native_prefix = fixed[start:fixed.index("#define MAX_SCENES_PER_FRAME", start)]
            native_prefix += function(fixed, "static GLboolean isaac_fbo_region_scene_reset_pending(void) {")
            start = tests.index("#ifdef HAVE_ISAAC_FBO_VALID_REGION")
            native_prefix += tests[start:tests.index("// Depth Test", start)]
        native_prefix += function(shared, "static inline __attribute__((always_inline)) void _glFramebufferTexture2D(")
        native_prefix += function(misc, "void glViewport(")
        for signature in ("inline __attribute__((always_inline)) void invalidate_viewport(",
                          "inline __attribute__((always_inline)) void validate_viewport(",
                          "void update_scissor_test(", "void glScissor("):
            native_prefix += function(tests, signature)
        native_prefix += function(framebuffers, "void glFramebufferTexture2D(")
        native_prefix += function(fixed, "static inline __attribute__((always_inline)) void scene_end(")
        native_tail = function(fixed, "void glFlush(") + function(fixed, "void glFinish(")
        header = source / "native_fbo_scissor_resize.h"
        for case, reset_body, expect, flags in (
            ("baseline", old_reset, 0, []),
            ("fixed", new_reset, 1, []),
            ("fixed-unflipped", new_reset, 1, ["-DHAVE_UNFLIPPED_FBOS=1"]),
            ("fixed-fast", new_reset, 1, ["-ffast-math"]),
            ("negative-original", old_reset, 1, []),
        ):
            header.write_text(native_prefix + reset_body + native_tail, encoding="utf-8")
            exe = source / (case + ".exe")
            defines = ["-DTEST_SCISSOR_REPLAY=1", "-DEXPECT_FIXED=1", f"-DEXPECT_REPLAY_FIXED={expect}"]
            if number == 31:
                defines += ["-DTEST_VALID_REGION=1", "-DHAVE_ISAAC_FBO_VALID_REGION=1"]
                if case == "negative-original":
                    defines += ["-DTEST_REPLAY_REGION_ONLY=1"]
            run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                 "-Wno-unused-function", "-Wno-sign-compare", *defines, *flags,
                 "-I", str(source), str(fixture), "-o", str(exe)])
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            if case == "negative-original":
                assert result.returncode != 0 and "!memcmp(last_clip,expected,sizeof expected)" in result.stderr, result
                print(f"FBO scissor replay {label}: original-reset negative rejected at SDK rectangle mismatch")
            else:
                assert result.returncode == 0, result.stderr
                print(f"{label}/{case}: {result.stdout.strip()}")
        header.write_text(native_prefix + new_reset + native_tail, encoding="utf-8")


def check_native_resource_profile(recipe: Path, source: Path, cc: str) -> None:
    """Execute the actual aggregate helper against SDK mocks, not a timing model."""
    target = source / "native-resource-host"
    target.mkdir()
    (target / "shared.h").write_text(r'''
#ifndef NR_MOCK_SHARED_H
#define NR_MOCK_SHARED_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define HAVE_SINGLE_THREADED_GC 1
typedef struct { int unused; } SceGxmContext;
typedef struct { int unused; } SceGxmRenderTarget;
typedef struct { uint32_t width, height; } SceGxmRenderTargetParams;
enum { VGL_MEM_RAM, VGL_MEM_VRAM };
uint64_t sceKernelGetProcessTimeWide(void);
void sceGxmFinish(SceGxmContext *context);
int sceGxmCreateRenderTarget(const SceGxmRenderTargetParams *p, SceGxmRenderTarget **t);
int sceGxmDestroyRenderTarget(SceGxmRenderTarget *t);
size_t vglMemFree(int type);
size_t vglMemTotal(int type);
#define vgl_memset memset
#include "isaac_native_resource_profile_internal.h"
#endif
''', encoding="utf-8", newline="\n")
    fixture = target / "fixture.c"
    fixture.write_text(r'''
#include "shared.h"
#include <assert.h>
#include <stdio.h>
#undef sceGxmFinish
#undef sceGxmCreateRenderTarget
#undef sceGxmDestroyRenderTarget
static uint64_t now;
static unsigned finish_calls, create_calls, destroy_calls, memory_calls;
static int create_result, destroy_result;
static SceGxmRenderTarget *next_target;
uint64_t sceKernelGetProcessTimeWide(void) { return now; }
void sceGxmFinish(SceGxmContext *context) { (void)context; ++finish_calls; now += 13; }
int sceGxmCreateRenderTarget(const SceGxmRenderTargetParams *p, SceGxmRenderTarget **t)
{ (void)p; ++create_calls; now += 7; if (!create_result) *t = next_target; return create_result; }
int sceGxmDestroyRenderTarget(SceGxmRenderTarget *t)
{ (void)t; ++destroy_calls; now += 11; return destroy_result; }
size_t vglMemFree(int type) { ++memory_calls; return type ? 200U : 100U; }
size_t vglMemTotal(int type) { ++memory_calls; return type ? 400U : 300U; }
static IsaacNativeResourceSize *find(IsaacNativeResourceStats *s, unsigned w, unsigned h)
{ for (unsigned i = 0; i < s->sizes_used; ++i)
    if (s->size[i].width == w && s->size[i].height == h) return &s->size[i];
  return NULL; }
int main(void)
{
    IsaacNativeResourceStats s;
    SceGxmRenderTargetParams p = {480, 272};
    SceGxmRenderTarget rt, *out = NULL;
    int depth;
    next_target = &rt;
    assert(isaacNativeResourceCreateRT(&p, &out) == 0 && out == &rt);
    isaacNativeResourceDepth(&depth, 480, 272, 19, 0);
    isaacNativeResourceSceneBegin(480, 272, 0);
    isaacNativeResourceFinish(NULL);
    assert(memory_calls == 0 && finish_calls == 1 && create_calls == 1);
    vglIsaacNativeResourceProfileTake(NULL);
    assert(memory_calls == 0);
    vglIsaacNativeResourceProfileTake(&s);
    assert(s.abi == 1 && memory_calls == 4);
    assert(s.ram_free == 100 && s.ram_total == 300 && s.vram_free == 200 && s.vram_total == 400);
    assert(s.metric[ISAAC_NR_FINISH].us == 13 && s.metric[ISAAC_NR_RT_CREATE].us == 7);
    assert(find(&s,480,272)->depth_create.us == 19);
    /* Take must not erase live scene/retirement identity. */
    isaacNativeResourceSceneEnd(31);
    assert(isaacNativeResourceRetireRT(&rt) == &rt);
    assert(isaacNativeResourceRetireDepth(&depth) == &depth);
    destroy_result = -9;
    assert(isaacNativeResourceDestroyRT(&rt) == -9);
    destroy_result = 0;
    assert(isaacNativeResourceDestroyRT(&rt) == 0 && destroy_calls == 2);
    vglIsaacNativeResourceProfileTake(&s);
    assert(find(&s,480,272)->end_scene.us == 31);
    assert(find(&s,480,272)->rt_destroy.count == 2);
    assert(find(&s,480,272)->rt_destroy.failed == 1);
    assert(s.rt_retired == 1 && s.depth_retired == 1 && !s.unknown_destroy);
    assert(!s.metric[ISAAC_NR_FINISH].count);
    isaacNativeResourceSceneBegin(1, 2, -1);
    isaacNativeResourceSceneEnd(3);
    assert(isaacNativeResourceRetireDepth(&depth) == &depth);
    assert(isaacNativeResourceRetireRT(NULL) == NULL);
    create_result = -5;
    assert(isaacNativeResourceCreateRT(NULL, NULL) == -5);
    isaacNativeResourceGcObject();
    isaacNativeResourceEvent(ISAAC_NR_GC, 0, 0, 42, 0);
    isaacNativeResourceRecovery(ISAAC_NR_RECOVER_GPU, 4096, 101, 1);
    isaacNativeResourceRecovery(ISAAC_NR_RECOVER_CPU, UINT32_MAX, UINT64_MAX, 0);
    isaacNativeResourceRecovery(ISAAC_NR_RECOVER_CPU, 1, 1, 0);
    vglIsaacNativeResourceProfileTake(&s);
    assert(s.scene_unknown == 1 && s.unknown_retire == 1 && s.rt_retired == 0);
    assert(s.gc_objects == 1 && s.metric[ISAAC_NR_GC].us == 42);
    assert(s.recovery_gpu_bytes == 4096 && s.metric[ISAAC_NR_RECOVER_GPU].failed == 1);
    assert(s.recovery_cpu_bytes == UINT32_MAX && s.clock_clamp == 1 && s.saturation == 2);
    assert(s.metric[ISAAC_NR_RT_CREATE].failed == 1);
    for (unsigned i = 0; i < 9; ++i) isaacNativeResourceEvent(ISAAC_NR_RT_CREATE, i, 1, 1, 0);
    for (uintptr_t i = 1; i <= 129; ++i) isaacNativeResourceDepth((void *)i, 2, 2, 1, 0);
    vglIsaacNativeResourceProfileTake(&s);
    assert(s.sizes_used == 8 && s.size_overflow == 130 && s.registry_overflow == 1);
    assert(s.metric[ISAAC_NR_DEPTH_CREATE].count == 129);
    puts("native resource actual helper: take/reset, active scene/RT identity, failures, saturation, bounds, memory endpoints PASS (SDK mocked)");
    return 0;
}
''', encoding="utf-8", newline="\n")
    executable = target / "fixture.exe"
    run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
         "-I", str(target), "-I", str(recipe), str(fixture),
         str(recipe / "isaac_native_resource_profile.c"), "-o", str(executable)])
    print(run([str(executable)]).decode().strip())


def prove_sdk_sparse(recipe: Path, temporary: Path, stock_tar: Path | None,
                     cc: str | None = None, repository_archive: bytes | None = None) -> None:
    """Fresh pinned callsites, actual canonical/nearest helpers and timer core."""
    if stock_tar:
        assert sha256(stock_tar) == SOURCE_ARCHIVE_SHA256
    common = ["0001-deterministic-build-and-init-oob.patch",
              "0002-exact-gpu-draw-optimizations.patch"]
    scene = [GXM_PATCHES[k] for k in ("scene_timer", "scene_split", "fbo_rt_scenes")]
    full = ["0003-exact-coloroffset-gpu-optimizations.patch", FS_PROBE_PATCH,
            "0004-hardened-custom-shader-cache.patch", STAGING_PATCH, PLAIN_PATCH,
            SINGLE_BIND_PATCH, PLAIN_VERTEX_PAIR_PATCH]
    tail = ["0010-isaac-p8-safe-upload.patch", GXM_PATCHES["fbo_valid_region"],
            "0011-isaac-exact-laser-p8.patch", "0016-isaac-laser-light-halo-clip.patch",
            "0017-isaac-laser-light-nearest.patch", TRANSFORM_PATCH, ATLAS_NEAREST_PATCH,
            "0020-isaac-native-resource-profile.patch", "0021-isaac-fbo-rt-reuse.patch",
            "0022-isaac-coloroffset-link-proof.patch", "0023-isaac-skip-zero-fragment-uniform.patch"]
    for atlas in (False, True):
        source = materialize_stock(temporary / f"sdk-sparse-{int(atlas)}", stock_tar, repository_archive)
        for name in common + (full if atlas else []) + scene + (tail if atlas else []):
            apply_patch(source, recipe / name)
        hashed = [source / "source" / n for n in ("custom_shaders.c", "vitaGL.h")]
        before = [sha256(p) for p in hashed]
        originals = {n: (source / "source" / n).read_text(encoding="utf-8")
                     for n in ("draw.c", "misc.c", "gxm.c")}
        apply_patch(source, recipe / "0025-isaac-sdk-sparse-profile.patch")
        assert before == [sha256(p) for p in hashed], "sparse patch touched shader inputs"
        draw, misc, gxm = [(source / "source" / n).read_text(encoding="utf-8")
                          for n in ("draw.c", "misc.c", "gxm.c")]
        assert draw.count("ISAAC_SDK_CANONICAL_BEGIN();") == 1
        assert draw.count("ISAAC_SDK_CANONICAL_END();") == 1
        assert draw.count("#define ISAAC_SDK_SPARSE_DRAW_OWNER") == 1
        if atlas:
            assert draw.index('"isaac_sdk_sparse_profile_internal.h"') < draw.index(
                '"isaac_laser_light_nearest.h"')
        for suffix in ("clear_t0", "vertex_t0", "fragment_t0", "clear_draw_t0"):
            assert misc.count(f"ISAAC_SDK_BEGIN(isaac_sdk_{suffix});") == 1
        for kind in ("CLEAR", "CLEAR_VERTEX", "CLEAR_FRAGMENT", "CLEAR_DRAW"):
            assert misc.count(f"ISAAC_SDK_END(ISAAC_SDK_{kind},") == 1
        assert gxm.count("ISAAC_SDK_BEGIN(isaac_sdk_queue_t0);") == 1
        assert gxm.count("ISAAC_SDK_END(ISAAC_SDK_QUEUE, isaac_sdk_queue_t0);") == 1
        assert gxm.count("#define sceGxmBeginScene(...)") == 1
        assert gxm.count("#define sceGxmEndScene(...)") == 1
        # No SDK invocation was removed, duplicated or changed by the patch.
        # Function-like counter aliases add only one textual self-reference.
        for n, after in (("draw.c", draw), ("misc.c", misc), ("gxm.c", gxm)):
            def calls(text: str) -> list[str]:
                return re.findall(r"\bsce(?:Gxm|Kernel|Display)\w+\s*\([^;]*?;", text)
            if n != "gxm.c":
                assert calls(originals[n]) == calls(after)
        if cc:
            _check_sdk_sparse_canonical(recipe, source, draw, originals["draw.c"], cc, atlas)
            if atlas:
                _check_sdk_sparse_edges(recipe, source, misc, gxm, cc)
        print(f"SDK-sparse fresh pinned chain atlas={int(atlas)}: source population/hash checks PASS")


def prove_metadata_once(recipe: Path, temporary: Path, stock_tar: Path | None,
                        cc: str | None = None, repository_archive: bytes | None = None) -> None:
    """Regenerate the two supported recipes; exercise the actual staging block."""
    if stock_tar:
        assert sha256(stock_tar) == SOURCE_ARCHIVE_SHA256
    order = [1, 2, 3, 8, 4, 12, 13, 14, 15, 5, 6, 7, 10, 9, 11, 16, 17, 18]
    patches = {int(p.name[:4]): p for p in recipe.glob("[0-9][0-9][0-9][0-9]-*.patch")}
    build = (recipe / "build.sh").read_text(encoding="utf-8")
    cmake = (recipe.parent / "CMakeLists.txt").read_text(encoding="utf-8")
    for needle in ('coloroffset_metadata_once=${ISAAC_COLOROFFSET_METADATA_ONCE:-0}',
                   '"coloroffset_metadata_once:$coloroffset_metadata_once"',
                   '"$coloroffset_metadata_once" "$coloroffset_metadata_once_patch_sha"',
                   'build_flags="$build_flags HAVE_ISAAC_COLOROFFSET_METADATA_ONCE=1"',
                   'extra_cflags="$extra_cflags -DHAVE_ISAAC_COLOROFFSET_METADATA_ONCE=1"',
                   'apply_source_patch "$coloroffset_metadata_once_patch"',
                   'unsupported ColorOffset metadata-once source combination'):
        assert needle in build, needle
    for needle in ('option(ISAAC_VITA_COLOROFFSET_METADATA_ONCE',
                   'NOT ISAAC_VITA_LASER_LIGHT_HALO_CLIP',
                   '"ISAAC_COLOROFFSET_METADATA_ONCE=${ISAAC_VITA_COLOROFFSET_METADATA_ONCE_MODE}"',
                   '"${ISAAC_VITA_COLOROFFSET_METADATA_ONCE_MODE_FILE}"',
                   '0026-isaac-coloroffset-metadata-once.patch'):
        assert needle in cmake, needle
    for atlas in (False, True):
        source = materialize_stock(temporary / f"metadata-once-{int(atlas)}", stock_tar, repository_archive)
        for number in order + ([19] if atlas else []) + [20, 21, 22, 23, 25]:
            apply_patch(source, patches[number])
        path = source / "source/custom_shaders.c"
        before = path.read_text(encoding="utf-8")
        before_hash = sha256(path)
        assert before_hash == (
            "7afd718fab909dc25e0798b5c91604ad9f3aaf5c8048384f2e1b59f883b48c3e" if atlas else
            "f93b8ffe1cacb8f1656b4d3451f42e05202ce336d3e0a85d38e1b3fcd7ee3d50")
        other_files = ("draw.c", "gxm.c", "misc.c", "textures.c", "framebuffers.c", "vitaGL.h")
        other_hashes = [sha256(source / "source" / name) for name in other_files]
        apply_patch(source, patches[26])
        after = path.read_text(encoding="utf-8")
        after_hash = sha256(path)
        assert other_hashes == [sha256(source / "source" / name) for name in other_files]
        print(f"metadata-once atlas={int(atlas)} {before_hash} -> {after_hash}")
        assert f"{before_hash}) shader_source_sha={after_hash} ;;" in build
        if cc:
            gxm = (source / "source/gxm.c").read_text(encoding="utf-8")
            reset = gxm[gxm.index("void scene_reset(void)"):]
            assert reset.index("is_fbo_float = in_use_framebuffer ? in_use_framebuffer->is_float : GL_FALSE;") < reset.index("_glFramebufferTexture2D(active_write_fb,")
            shared = (source / "source/shared.h").read_text(encoding="utf-8")
            attach = shared[shared.index("void _glFramebufferTexture2D("):]
            assert attach.index("fb->is_float = tex->format == SCE_GXM_TEXTURE_FORMAT_F16F16F16F16_RGBA;") < attach.index("dirty_framebuffer = GL_TRUE;")
            finish = gxm[gxm.index("void glFinish(void)"):gxm.index("void glReleaseShaderCompiler(void)")]
            assert finish.index("dirty_framebuffer = GL_TRUE;") < finish.index("scene_reset();") < finish.index("sceGxmFinish(gxm_context);")
            alloc = (source / "source/utils/gpu_utils.c").read_text(encoding="utf-8")
            recovery = alloc[alloc.index("void *gpu_alloc_mapped_aligned_unsafe_for_cpu("):alloc.index("void *gpu_alloc_mapped_aligned_unsafe_for_gpu(")]
            assert recovery.index("glFinish();") < recovery.index("garbage_collector(0, NULL);")
            # Bind the existing P8 fixture's cap reservation lifetime case
            # to real pool-overflow marking and four-bucket CPU recovery.
            vgl = (source / "source/vgl.c").read_text(encoding="utf-8")
            pool = vgl[vgl.index("uint8_t *vgl_reserve_data_pool(uint32_t size)"):]
            assert pool.index("res = (uint8_t *)gpu_alloc_mapped_for_cpu(size);") < pool.index("mark_as_dirty(res);")
            assert "#define FRAME_PURGE_FREQ 4 " in shared
            assert "int frame_purge_idx = 0;" in gxm and "static int frame_purge_clean_idx = 1;" in gxm
            gc = gxm[gxm.index("int garbage_collector("):]
            assert gc.index("vgl_free(frame_purge_list[frame_purge_clean_idx][i]);") < gc.index(
                "frame_purge_clean_idx = (frame_purge_clean_idx + 1) % FRAME_PURGE_FREQ;")
            assert "frame_purge_idx = (frame_purge_idx + 1) % FRAME_PURGE_FREQ;" in gc
            assert recovery.index("garbage_collector(0, NULL);") < recovery.index("gpu_alloc_mapped_aligned_for_cpu_inner(alignment, size);")
            assert "unsafe_allocator_counter < FRAME_PURGE_FREQ" in recovery
            _check_metadata_once(recipe, source, before, after, cc)


def prove_halo_profile(recipe: Path, temporary: Path, stock_tar: Path,
                       cc: str | None = None) -> None:
    """Regenerate current authenticated recipes plus the counter-only seam."""
    assert sha256(stock_tar) == SOURCE_ARCHIVE_SHA256
    patches = {int(p.name[:4]): p for p in recipe.glob("[0-9][0-9][0-9][0-9]-*.patch")}
    build = (recipe / "build.sh").read_text(encoding="utf-8")
    identities = []
    for atlas in (False, True):
        source = materialize_stock(temporary / f"halo-profile-{int(atlas)}", stock_tar, None)
        for number in [1, 2, 3, 8, 4, 12, 13, 14, 15, 5, 6, 7, 10, 9, 11, 16, 17, 18] + (
                [19] if atlas else []) + [20, 21, 24, 22, 23, 26, 25]:
            apply_patch(source, patches[number])
        path = source / "source/custom_shaders.c"
        before = path.read_text(encoding="utf-8")
        before_hash = sha256(path)
        assert before_hash == (
            "e4eb34d6f5b97d7f7b8c06abc5c15dd08448b6185f313149769b6eec67d3f864" if atlas else
            "1a432f2a70ae62acf2b50d00cf61170c29daaa0004543c285c310224d1cc0989")
        other = {p: sha256(p) for p in (source / "source").rglob("*") if p.is_file() and p != path}
        apply_patch(source, patches[27])
        assert all(sha256(p) == digest for p, digest in other.items())
        after_hash = sha256(path)
        identities.append(f"{before_hash}) shader_source_sha={after_hash} ;;")
        print(f"halo-profile atlas={int(atlas)} {before_hash} -> {after_hash}", flush=True)
        if cc:
            _check_metadata_once(recipe, source, before, path.read_text(encoding="utf-8"), cc, True)
        # Current mandatory stock repairs precede 0032 in the real recipe;
        # this existing full chain includes 0009, so use its replay variant.
        for number in (28, 29, 31):
            apply_patch(source, patches[number])
        other = {p: sha256(p) for p in (source / "source").rglob("*") if p.is_file() and p != path}
        before_white = path.read_text(encoding="utf-8")
        apply_patch(source, patches[32])
        assert all(sha256(p) == digest for p, digest in other.items())
        white_hash = sha256(path)
        identities.append(f"{after_hash}) shader_source_sha={white_hash} ;;")
        print(f"white-census atlas={int(atlas)} {after_hash} -> {white_hash}", flush=True)
        if cc:
            _check_metadata_once(recipe, source, before_white, path.read_text(encoding="utf-8"), cc, True, True)
    assert all(identity in build for identity in identities), identities


def _check_metadata_once(recipe: Path, source: Path, before: str, after: str, cc: str,
                         halo_profile: bool = False, white_profile: bool = False) -> None:
    # Extract the exact production predicate and packed staging block. The
    # fixture mocks allocation/halo results, not the decision under review.
    import test_vitagl_coloroffset_link_proof as link
    predicate = link.function(after, "isaac_coloroffset_neutral_metadata_is_eligible")
    assert predicate == link.function(before, "isaac_coloroffset_neutral_metadata_is_eligible")
    predicate = predicate.replace("{\n", "{\n\t++metadata_calls; note('M');\n", 1)

    def block(text):
        begin = text.index("\t\t\t{\n\t\t\t\tGLboolean halo_staged = GL_FALSE;")
        end = text.index("\n\t\t}\t\n\t} else {", begin)
        return text[begin:end]

    prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "isaac_coloroffset_gpu_policy.h"
typedef int GLboolean;
typedef int GLsizei;
typedef unsigned SceGxmIndexSource;
typedef unsigned SceGxmTextureFormat;
#define GL_FALSE 0
#define GL_TRUE 1
#define TEX_VALID 1
#define UNIFORM_SAMPLER 1
#define SCE_GXM_INDEX_SOURCE_INDEX_16BIT 0
#define SCE_GXM_ATTRIBUTE_FORMAT_F32 7
#define SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR 1
#define SCE_GXM_TEXTURE_FORMAT_U8U8U8_BGR 2
#define SCE_GXM_TEXTURE_FORMAT_P8_ABGR 3
#define ISAAC_COLOROFFSET_STAGING_UNKNOWN 0
#define ISAAC_COLOROFFSET_STAGING_NEUTRAL 1
#define ISAAC_COLOROFFSET_STAGING_PLAIN 2
#define ISAAC_NOTE_VERTEX_COPY(n,s) ((void)(n),(void)(s))
#if defined(HAVE_ISAAC_COLOROFFSET_METADATA_ONCE) && HAVE_ISAAC_COLOROFFSET_METADATA_ONCE && defined(HAVE_ISAAC_LASER_LIGHT_HALO_CLIP) && !defined(HAVE_TEX_CACHE)
#define EXPECT_REUSE 1
#else
#define EXPECT_REUSE 0
#endif
typedef struct { unsigned format; } Tex;
typedef struct { int status; void *data; Tex gxm_tex; } texture;
typedef struct { unsigned type, sampler_index; } Uniform;
typedef struct { unsigned streamIndex, offset, format, componentCount, regIndex; } SceGxmVertexAttribute;
typedef struct { unsigned stride; } SceGxmVertexStream;
typedef struct { unsigned isaac_coloroffset_link_vertex_exact, attr_num, max_frag_texunit_idx, max_vert_texunit_idx; Uniform *frag_texunits[1]; unsigned attr_map[8]; } program;
typedef struct { unsigned vertex_attrib_state; uintptr_t vertex_attrib_offsets[8]; } Vao;
typedef struct { const uint16_t *indices; GLsizei count; } Halo;
static unsigned metadata_calls, halo_mode, allocation_calls, proof_calls, copy_calls, recovery, recovery_calls, native_shadow, mutate_vertex;
static int is_fbo_float, fragment_exact;
static unsigned char input[352], output[352];
static const uint16_t indices[6] = {0,2,1,1,2,3};
static Vao vao, *cur_vao = &vao;
static program prog;
static texture tex;
static Uniform uniform;
static SceGxmVertexAttribute attrs[8];
static SceGxmVertexStream vertex_streams[8];
static char events[40];
static unsigned event_count;
static void note(char c) { assert(event_count+1 < sizeof events); events[event_count++] = c; events[event_count] = 0; }
static struct { int is_float, texture_float, lazy_attach; } framebuffer;
static void scene_reset(void) {
    /* Actual gxm.c ordering authenticated by the Python source check: the
     * global is read BEFORE lazy attachment changes the framebuffer field. */
    is_fbo_float = framebuffer.is_float;
    if (framebuffer.lazy_attach) {
        framebuffer.is_float = framebuffer.texture_float;
        framebuffer.lazy_attach = 0;
    }
}
static SceGxmTextureFormat vglGetTexFormat(const Tex *t) { return t->format; }
static void *gpu_alloc_mapped_temp(size_t size) {
    assert(size == sizeof output); ++allocation_calls; note('A');
    if (recovery) { ++recovery_calls; native_shadow = 0; scene_reset(); }
#ifdef HAVE_TEX_CACHE
    /* An adversarial metadata mutation must still be observed by the legacy
     * second call in the excluded configuration. Not a claim that the real
     * texture-cache implementation performs this particular write. */
    if (recovery == 2) tex.status = 0;
#endif
    if (mutate_vertex) input[9] ^= 0x40;
    return output;
}
static int isaac_coloroffset_copy_and_prove_neutral(void *dst, const void *src, size_t bytes, unsigned count, unsigned stride) {
    assert(dst == output && src == input && bytes == sizeof input && count == 4 && stride == 88);
    ++proof_calls; note('P'); memcpy(dst, src, bytes); return ISAAC_COLOROFFSET_STAGING_NEUTRAL;
}
static void vgl_fast_memcpy(void *dst, const void *src, size_t bytes) { ++copy_calls; note('C'); memcpy(dst, src, bytes); }
static int isaac_light_halo_try_stage(texture *t, unsigned sampler, const void *src, unsigned count, const uint16_t *idx, GLsizei n, Halo *halo, void **dst, unsigned *top) {
    assert(t == &tex && sampler == 0 && src == input && count == 4 && idx == indices && n == 6);
    note('H');
    if (halo_mode == 2) (void)gpu_alloc_mapped_temp(sizeof output); /* failed partial staging: no publication */
    if (halo_mode != 1) return 0;
    *dst = output; *top = 8; halo->count = 12; halo->indices = indices; return 1;
}
'''
    if halo_profile:
        # Reuse the actual private counter/argument definitions. The helper's
        # result is mocked here; its real guard/classifier paths are exercised
        # separately by the existing P8 fixture.
        policy = (recipe / "isaac_laser_light_halo_policy.h").read_text(encoding="utf-8")
        native = (recipe / "isaac_laser_light_halo_vitagl.h").read_text(encoding="utf-8")
        observer = policy[policy.index("#if defined(HAVE_ISAAC_LASER_HALO_PROFILE)"):
                          policy.index("/* Visual-quality")]
        observer += native[native.index("#if defined(HAVE_ISAAC_LASER_HALO_PROFILE)"):
                           native.index("/* Called only")]
        prelude = prelude.replace('#include "isaac_coloroffset_gpu_policy.h"',
                                  '#include "isaac_coloroffset_gpu_policy.h"\n' + observer)
        prelude = prelude.replace('unsigned *top) {', 'unsigned *top ISAAC_HALO_REASON_DECL) {')
        prelude = prelude.replace("    note('H');", "    note('H');\n    ISAAC_HALO_REASON(ISAAC_HALO_OUTPUT);")
        prelude = prelude.replace("    *dst = output; *top = 8;", "    ISAAC_HALO_REASON(ISAAC_HALO_LIGHT);\n    *dst = output; *top = 8;")
    wrapper = r'''
static int submit(unsigned base_idx) {
    program *p = &prog;
    texture *isaac_coloroffset_texture = &tex;
    const GLboolean isaac_coloroffset_fragment_exact = fragment_exact;
    SceGxmVertexAttribute *attributes = attrs;
    SceGxmVertexStream *streams = vertex_streams;
    const uint32_t isaac_copy_stride = 88;
    SceGxmIndexSource isaac_coloroffset_original_index_type = 0;
    int isaac_neutral_staging = ISAAC_COLOROFFSET_STAGING_UNKNOWN;
    GLsizei count = 6;
    uint32_t top_idx = 4;
    const uint16_t *idx_buf = indices;
    Halo halo = {indices, 6}, *halo_submission = &halo;
    void *ptrs[1] = {0};
    (void)halo_submission; (void)idx_buf; (void)ptrs;
'''
    tests = r'''
    assert(ptrs[0] == output);
    assert(count == (halo_mode == 1 && EXPECT_HALO ? 12 : 6));
    assert(top_idx == (halo_mode == 1 && EXPECT_HALO ? 8 : 4));
    return isaac_neutral_staging;
}
static void reset(void) {
    static const unsigned offsets[8] = {0,12,28,36,52,64,72,76};
    static const unsigned components[8] = {3,4,2,4,3,2,1,3};
    memset(input, 0x35, sizeof input); memset(output, 0, sizeof output);
    memset(&prog, 0, sizeof prog); memset(&vao, 0, sizeof vao);
    uniform.type = UNIFORM_SAMPLER; uniform.sampler_index = 0;
    prog.isaac_coloroffset_link_vertex_exact = 1; prog.attr_num = 8;
    prog.max_frag_texunit_idx = 1; prog.frag_texunits[0] = &uniform;
    vao.vertex_attrib_state = 0xff;
    for (unsigned i=0;i<8;i++) {
        prog.attr_map[i] = i; vao.vertex_attrib_offsets[i] = (uintptr_t)(input+offsets[i]);
        attrs[i] = (SceGxmVertexAttribute){i,offsets[i],7,components[i],i*4}; vertex_streams[i].stride = 88;
    }
    tex = (texture){TEX_VALID,input,{1}}; fragment_exact = 1; is_fbo_float = 0;
    metadata_calls = allocation_calls = proof_calls = copy_calls = halo_mode = 0;
    recovery = recovery_calls = mutate_vertex = event_count = 0; events[0] = 0; native_shadow = 1;
    memset(&framebuffer, 0, sizeof framebuffer);
}
int main(void) {
    unsigned expected = EXPECT_HALO && !EXPECT_REUSE ? 2 : 1;
    reset(); recovery=1; mutate_vertex=1;
    assert(submit(0) == ISAAC_COLOROFFSET_STAGING_NEUTRAL);
    assert(metadata_calls == expected && allocation_calls == 1 && proof_calls == 1 && copy_calls == 0);
    assert(recovery_calls == 1 && native_shadow == 0 && input[9] == (0x35^0x40));
    assert(!memcmp(input,output,sizeof input));
    assert(!strcmp(events, EXPECT_HALO ? (EXPECT_REUSE ? "MHAP" : "MHAMP") : "AMP"));
    /* A following draw observes new metadata: this must not become a cache. */
    attrs[4].offset++; metadata_calls=event_count=proof_calls=copy_calls=allocation_calls=0; events[0]=0;
    assert(submit(0) == ISAAC_COLOROFFSET_STAGING_UNKNOWN);
    assert(metadata_calls == expected && copy_calls == 1 && proof_calls == 0);
    assert(!memcmp(input,output,sizeof input));
    reset(); fragment_exact=0;
    assert(submit(0) == ISAAC_COLOROFFSET_STAGING_UNKNOWN && metadata_calls == expected);
    assert(!strcmp(events, EXPECT_HALO ? (EXPECT_REUSE ? "MAC" : "MAMC") : "AMC"));
    reset();
    assert(submit(1) == ISAAC_COLOROFFSET_STAGING_UNKNOWN && metadata_calls == expected);
    reset(); halo_mode=1;
    assert(submit(0) == (EXPECT_HALO ? ISAAC_COLOROFFSET_STAGING_PLAIN : ISAAC_COLOROFFSET_STAGING_NEUTRAL));
    assert(metadata_calls == 1 && allocation_calls == (EXPECT_HALO ? 0u : 1u));
    reset(); halo_mode=2; recovery=1;
    assert(submit(0) == ISAAC_COLOROFFSET_STAGING_NEUTRAL && metadata_calls == expected);
    assert(allocation_calls == (EXPECT_HALO ? 2u : 1u) && proof_calls == 1);
    /* Attached texture RGBA8 -> RGBA16F before ingress. Ingress latches old
     * global 0 then lazy attachment publishes fb=1. Recovery must make both
     * baseline and candidate reject the second check (no latent-bug fix). */
    reset(); framebuffer.texture_float=1; framebuffer.lazy_attach=1; scene_reset();
    assert(is_fbo_float==0 && framebuffer.is_float==1); recovery=1;
    assert(submit(0) == ISAAC_COLOROFFSET_STAGING_UNKNOWN);
    assert(metadata_calls == (EXPECT_HALO ? 2u : 1u) && copy_calls==1 && proof_calls==0);
    /* The inverse direction must retry an initially false predicate too. */
    reset(); framebuffer.is_float=1; framebuffer.lazy_attach=1; scene_reset();
    assert(is_fbo_float==1 && framebuffer.is_float==0); recovery=1;
    assert(submit(0) == ISAAC_COLOROFFSET_STAGING_NEUTRAL);
    assert(metadata_calls == (EXPECT_HALO ? 2u : 1u) && copy_calls==0 && proof_calls==1);
    /* Without recovery, preserve the baseline's stale-at-ingress behavior. */
    reset(); framebuffer.texture_float=1; framebuffer.lazy_attach=1; scene_reset();
    assert(submit(0) == ISAAC_COLOROFFSET_STAGING_NEUTRAL && metadata_calls==expected);
    assert(is_fbo_float==0 && framebuffer.is_float==1);
#ifdef HAVE_TEX_CACHE
    reset(); recovery=2;
    assert(submit(0) == ISAAC_COLOROFFSET_STAGING_UNKNOWN && copy_calls == 1 && proof_calls == 0);
    assert(metadata_calls == expected);
#endif
    puts("actual metadata/staging: OFF/zero/ON, halo success/reject, recovery, live bytes, cross-draw mutation and excluded TEX_CACHE PASS");
    return 0;
}
'''
    if halo_profile:
        wrapper += '''
#if defined(HAVE_ISAAC_LASER_HALO_PROFILE) && HAVE_ISAAC_LASER_HALO_PROFILE
    vglIsaacLaserHaloStats prior = isaac_halo_stats;
#endif
'''
        tests = tests.replace('    assert(ptrs[0] == output);', '''
#if defined(HAVE_ISAAC_LASER_HALO_PROFILE) && HAVE_ISAAC_LASER_HALO_PROFILE
    unsigned outcome = strchr(events, 'H') ? (halo_mode == 1 ? ISAAC_HALO_LIGHT : ISAAC_HALO_OUTPUT) : ISAAC_HALO_META;
    assert(isaac_halo_stats.attempts - prior.attempts == 1u);
    for (unsigned i=0; i<ISAAC_HALO_OUTCOMES; ++i)
        assert(isaac_halo_stats.outcome[i] - prior.outcome[i] == (i == outcome));
#endif
    assert(ptrs[0] == output);''')
    if white_profile:
        # Real final staging/single-stream/PLAIN request guards and the exact
        # observer hunk. Selection/opaque-recovery outcomes are boundary mocks;
        # the copied-byte proof is separately executed by the staging fixture.
        begin = after.index("\t\t\tif (isaac_staged && isaac_single_stream &&")
        gate = after[begin:after.index("#endif", begin)]
        begin = after.index("\t\t\t\tif (isaac_neutral_staging == ISAAC_COLOROFFSET_STAGING_PLAIN) {")
        terminal = after[begin:after.index("\n\t\t\t\t\tisaac_probe_fragment =", begin)]
        trace_guard = "#if defined(HAVE_ISAAC_LASER_HALO_PROFILE) && HAVE_ISAAC_LASER_HALO_PROFILE && defined(HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION) && HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION\n"
        prelude += trace_guard + '''
static unsigned white_result, white_token, late_float, final_staged=1, final_single=1;
static int isaac_coloroffset_copy_and_prove_white(void *dst, const void *src, size_t bytes,
        unsigned count, unsigned stride, uint32_t *token) {
    int result=isaac_coloroffset_copy_and_prove_neutral(dst,src,bytes,count,stride);
    *token=0u;
    if (white_result) { result=(int)white_result; if (result==ISAAC_COLOROFFSET_STAGING_PLAIN) *token=white_token; }
    return result;
}
static void final_request(int isaac_staged, int isaac_single_stream, int isaac_neutral_staging, uint32_t isaac_ordinary_white_proof) {
''' + gate + "{\n" + terminal + "\n}\n}\n}\n#endif\n"
        wrapper += trace_guard + "uint32_t isaac_ordinary_white_proof=0u;\n#endif\n"
        tests = tests.replace("    assert(ptrs[0] == output);", trace_guard + '''
    if (late_float) is_fbo_float=1; /* existing opaque selector may recover */
    final_request(final_staged,final_single,isaac_neutral_staging,isaac_ordinary_white_proof);
#endif
    assert(ptrs[0] == output);''')
        tests = tests.replace("    memset(&framebuffer, 0, sizeof framebuffer);", "    memset(&framebuffer, 0, sizeof framebuffer);\n" + trace_guard + "white_result=white_token=late_float=0; final_staged=final_single=1;\n#endif")
        extra = trace_guard + '''
    vglIsaacLaserHaloStats white_before;
    for (unsigned token=1;token<=2;++token) {
        reset(); white_result=ISAAC_COLOROFFSET_STAGING_PLAIN; white_token=token;
        white_before=isaac_halo_stats;
        assert(submit(0)==ISAAC_COLOROFFSET_STAGING_PLAIN);
        assert(isaac_halo_stats.ordinary_plain_requests-white_before.ordinary_plain_requests==1u);
        assert(isaac_halo_stats.ordinary_white_requests-white_before.ordinary_white_requests==(token==2u));
    }
    for (unsigned edge=0;edge<7;++edge) {
        reset(); white_result=ISAAC_COLOROFFSET_STAGING_PLAIN; white_token=2;
        if (edge==0) halo_mode=1; /* shortcut never calls traced ordinary copy */
        if (edge==1) final_staged=0;
        if (edge==2) final_single=0;
        if (edge==3) late_float=1;
        if (edge==4) white_result=ISAAC_COLOROFFSET_STAGING_NEUTRAL;
        if (edge==5) fragment_exact=0;
        if (edge==6) { framebuffer.texture_float=1; framebuffer.lazy_attach=1; scene_reset(); recovery=1; }
        white_before=isaac_halo_stats;
        (void)submit(0);
        assert(isaac_halo_stats.ordinary_plain_requests==white_before.ordinary_plain_requests);
        assert(isaac_halo_stats.ordinary_white_requests==white_before.ordinary_white_requests);
    }
    reset(); halo_mode=2; white_result=ISAAC_COLOROFFSET_STAGING_PLAIN; white_token=2;
    white_before=isaac_halo_stats;
    assert(submit(0)==ISAAC_COLOROFFSET_STAGING_PLAIN);
    assert(isaac_halo_stats.ordinary_plain_requests-white_before.ordinary_plain_requests==1u);
    reset(); /* no cross-draw pending */
    white_before=isaac_halo_stats; (void)submit(0);
    assert(isaac_halo_stats.ordinary_plain_requests==white_before.ordinary_plain_requests);
    puts("white actual owner: metadata/recovery, late float, final gates, shortcut exclusion, one request PASS");
#endif
'''
        tests = tests.replace('    puts("actual metadata/staging:', extra + '    puts("actual metadata/staging:')
    modes = (
        ("off", ["-DHAVE_ISAAC_LASER_LIGHT_HALO_CLIP=1"]),
        ("zero", ["-DHAVE_ISAAC_LASER_LIGHT_HALO_CLIP=1", "-DHAVE_ISAAC_COLOROFFSET_METADATA_ONCE=0"]),
        ("on", ["-DHAVE_ISAAC_LASER_LIGHT_HALO_CLIP=1", "-DHAVE_ISAAC_COLOROFFSET_METADATA_ONCE=1"]),
        ("halo-off", ["-DHAVE_ISAAC_COLOROFFSET_METADATA_ONCE=1"]),
        ("tex-cache", ["-DHAVE_ISAAC_LASER_LIGHT_HALO_CLIP=1", "-DHAVE_ISAAC_COLOROFFSET_METADATA_ONCE=1", "-DHAVE_TEX_CACHE=1"]),
    )
    if halo_profile:
        common = ["-DHAVE_ISAAC_LASER_LIGHT_HALO_CLIP=1", "-DHAVE_ISAAC_COLOROFFSET_METADATA_ONCE=1"]
        modes = (("off", common), ("zero", common + ["-DHAVE_ISAAC_LASER_HALO_PROFILE=0"]),
                 ("on", common + ["-DHAVE_ISAAC_LASER_HALO_PROFILE=1"]),
                 ("tex-cache", common + ["-DHAVE_ISAAC_LASER_HALO_PROFILE=1", "-DHAVE_TEX_CACHE=1"]))
        if white_profile:
            modes += (("white", common + ["-DHAVE_ISAAC_LASER_HALO_PROFILE=1", "-DHAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION=1"]),
                      ("white-fast", common + ["-DHAVE_ISAAC_LASER_HALO_PROFILE=1", "-DHAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION=1", "-ffast-math"]))
    for label, options in modes:
        definitions = "\n#ifdef HAVE_ISAAC_LASER_LIGHT_HALO_CLIP\n#define EXPECT_HALO 1\n#else\n#define EXPECT_HALO 0\n#endif\n"
        # A constant filename makes the OFF object check independent of paths.
        path = source / "metadata-fixture.c"
        objs = []
        compare = label in (("off", "zero") if halo_profile else ("off", "zero", "halo-off", "tex-cache"))
        for baseline in (True, False) if compare else (False,):
            # Keep host assert line-number payloads equal despite added
            # preprocessor lines; no object bytes except COFF time are masked.
            fixture = prelude + definitions + predicate + wrapper + block(before if baseline else after) + "\n#line 500\n" + tests
            # Baseline behavior is also two evaluations with an explicit ON
            # in the excluded HALO/TEX_CACHE modes, as EXPECT_REUSE records.
            path.write_text(fixture, encoding="utf-8", newline="\n")
            tag = label + ("-baseline" if baseline else "")
            obj = source / ("metadata-" + tag + ".o")
            exe = source / ("metadata-" + tag + ".exe")
            common = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I", str(recipe), *options]
            run([*common, "-c", str(path), "-o", str(obj)])
            run([*common, str(path), "-o", str(exe)])
            print(label, run([str(exe)]).decode().strip())
            blob = bytearray(obj.read_bytes())
            if os.name == "nt": blob[4:8] = b"\0"*4  # COFF timestamp only
            objs.append(blob)
        if len(objs) == 2:
            assert objs[0] == objs[1], f"metadata {label} changed the original complete host object"


def _check_sdk_sparse_canonical(recipe: Path, source: Path, draw: str,
                                original: str, cc: str, atlas: bool) -> None:
    extract = lambda text: re.search(
        r"(?ms)^GLboolean vglIsaacDrawCanonicalQuads\(GLsizei count\) \{.*?^\}", text).group(0)
    # SDK declarations precede the source-local adapter, exactly as in draw.c.
    prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int GLboolean, GLsizei, SceGxmPrimitiveType, SceGxmIndexFormat, SceGxmIndexSource;
typedef struct { int value; } SceGxmContext;
typedef struct { int value; } SceGxmTexture;
enum { GL_FALSE, GL_TRUE, SCE_GXM_PRIMITIVE_TRIANGLES=7, SCE_GXM_INDEX_FORMAT_U16=2,
       SCE_GXM_INDEX_SOURCE_INDEX_16BIT=4, MAX_IDX_NUMBER=65536, MODEL_CREATION=42 };
#define THREAD_SAFE()
static uint16_t indices[6];
static uint16_t *isaac_canonical_quads_idx_ptr=indices;
static int cur_program=1, phase, prim_is_non_native, legal=1;
static uint64_t now;
static unsigned sdk_calls, clock_calls, bind_calls;
static int draw_result, bind_failure, nearest;
static unsigned nested_draws, scene_extra_draws;
static SceGxmContext context, *gxm_context=&context;
GLboolean vglIsaacDrawCanonicalQuads(GLsizei count);
uint64_t sceKernelGetProcessTimeWide(void) { ++clock_calls; return now++; }
static int sceGxmDraw(SceGxmContext *c, SceGxmPrimitiveType p,
        SceGxmIndexFormat f, const void *i, unsigned count) {
    assert(c==&context && p==7 && f==2 && i==indices && count==6);
    ++sdk_calls; now += 7;
    if (nested_draws) { --nested_draws; assert(vglIsaacDrawCanonicalQuads(6)); }
    return draw_result;
}
static void scene_reset(void);
static void restore_polygon_mode(SceGxmPrimitiveType p) { assert(p==7); now += 2; }
'''
    if atlas:
        prelude += r'''
#define HAVE_ISAAC_LASER_ATLAS_NEAREST 1
typedef struct {
    SceGxmPrimitiveType primitive; uint16_t *indices; GLsizei count;
    SceGxmTexture nearest_original, nearest_point;
    GLboolean nearest_ready, atlas_nearest_ready;
} IsaacLightHaloDraw;
#define ISAAC_LIGHT_HALO_ARG(v) ,v
static int sceGxmSetFragmentTexture(SceGxmContext *c, unsigned unit, const SceGxmTexture *t) {
    assert(c==&context && unit==0 && t); ++bind_calls;
    return bind_failure ? -1 : 0;
}
#define sceClibPrintf(...) ((void)0)
static int _glDrawElements_CustomShadersIMPL(uint16_t *i, int n, unsigned top,
        unsigned base, int type, IsaacLightHaloDraw *halo) {
    assert(i==indices && n==6 && top==4 && base==0 && type==4);
    halo->atlas_nearest_ready=nearest; return legal;
}
'''
    else:
        prelude += r'''
static int _glDrawElements_CustomShadersIMPL(uint16_t *i, int n, unsigned top,
        unsigned base, int type) {
    assert(i==indices && n==6 && top==4 && base==0 && type==4); return legal;
}
'''
    off_object = None
    for mode in ("baseline", "undefined", "zero", "sparse"):
        sparse = mode == "sparse"
        fixture = prelude
        if mode != "baseline":
            guard = re.search(r"(?ms)^#if defined\(HAVE_ISAAC_GL_TIME_SDK_SPARSE\).*?^#endif\n", draw)
            assert guard
            fixture += guard.group(0)
        if atlas:
            fixture += '#include "isaac_laser_light_nearest.h"\n'
        fixture += r'''
static void scene_reset(void) {
    now += 3;
    while (scene_extra_draws) {
        --scene_extra_draws;
        assert(sceGxmDraw(&context, 7, 2, indices, 6)==draw_result);
    }
}
'''
        fixture += extract(original if mode == "baseline" else draw)
        if not sparse:
            fixture += '\nint main(void) { assert(vglIsaacDrawCanonicalQuads(6)); assert(sdk_calls==1 && clock_calls==0); return 0; }\n'
        else:
            fixture += r'''
int main(void) {
    IsaacSdkSparseProfile s;
    for (unsigned residue=0; residue<32; ++residue) {
        for (unsigned n=31; n<=33; ++n) {
            unsigned selected=0;
            vglIsaacSdkSparseTake(NULL, residue); sdk_calls=clock_calls=0;
            assert(!vglIsaacDrawCanonicalQuads(0));
            assert(isaac_sdk_sparse.canonical_seen==0 && clock_calls==0);
            for (unsigned i=0; i<n; ++i) {
                assert(vglIsaacDrawCanonicalQuads(6));
                selected += ((i&31)==residue);
            }
            assert(sdk_calls==n && clock_calls==4*selected);
            vglIsaacSdkSparseTake(&s, residue+1);
            assert(s.residue==residue && s.canonical_seen==n && s.canonical_selected==selected);
            assert(s.canonical_issued==n && !s.canonical_unpaired && !s.other_draw_calls);
            assert(!s.canonical_draw_errors);
            assert(s.bucket[ISAAC_SDK_DRAW].calls==selected && s.bucket[ISAAC_SDK_DRAW].us==8*selected);
            assert(s.bucket[ISAAC_SDK_CANONICAL].calls==selected);
            assert(s.bucket[ISAAC_SDK_CANONICAL].us==15*selected);
            assert(s.clock_reads==4*selected+2 && s.clock_reads==clock_calls);
            assert(s.bucket[ISAAC_SDK_CONTROL].calls==1 && s.bucket[ISAAC_SDK_CONTROL].us==1);
            assert(isaac_sdk_sparse.residue==((residue+1)&31));
        }
    }
    vglIsaacSdkSparseTake(NULL, 0); clock_calls=sdk_calls=0;
    draw_result=-17;
    assert(isaacSdkSparseDraw(&context, 7, 2, indices, 6)==-17);
    assert(clock_calls==0 && isaac_sdk_sparse.other_draw_calls==1 && sdk_calls==1);
    assert(vglIsaacDrawCanonicalQuads(6)); /* native return does not alter original GL return */
    vglIsaacSdkSparseTake(&s, 1);
    assert(s.canonical_issued==1 && s.bucket[ISAAC_SDK_DRAW].calls==1);
    assert(s.canonical_draw_errors==1); /* outside-span failure is not canonical */
    vglIsaacSdkSparseTake(NULL, 1); draw_result=3; clock_calls=sdk_calls=0;
    assert(vglIsaacDrawCanonicalQuads(6)); /* unsampled SDK error is counted too */
    vglIsaacSdkSparseTake(&s, 2);
    assert(s.canonical_issued==1 && !s.canonical_selected && s.canonical_draw_errors==1);
    assert(!s.bucket[ISAAC_SDK_DRAW].calls && s.clock_reads==2 && clock_calls==2);
    draw_result=-17;
    /* Use the actual patched canonical function and adapter, not a copied
     * model: a selected parent issues a draw which enters another canonical
     * span. Its already-read SDK interval must never enter either bucket. */
    vglIsaacSdkSparseTake(NULL, 0); clock_calls=sdk_calls=0; nested_draws=1;
    assert(vglIsaacDrawCanonicalQuads(6));
    assert(!isaac_sdk_active_span);
    vglIsaacSdkSparseTake(&s, 1);
    assert(sdk_calls==2 && s.canonical_seen==2 && s.canonical_issued==2);
    assert(s.canonical_draw_errors==2);
    assert(s.canonical_selected==1 && s.canonical_unpaired==1);
    assert(!s.bucket[ISAAC_SDK_DRAW].calls && !s.bucket[ISAAC_SDK_CANONICAL].calls);
    assert(!s.bucket[ISAAC_SDK_DRAW].us && !s.bucket[ISAAC_SDK_DRAW].max_us);
    assert(s.clock_reads==6 && clock_calls==6);
    /* The converse: only the nested child is selected, and it remains a valid
     * one-SDK-call pair when the unselected parent resumes. */
    vglIsaacSdkSparseTake(NULL, 1); clock_calls=sdk_calls=0; nested_draws=1;
    assert(vglIsaacDrawCanonicalQuads(6));
    assert(!isaac_sdk_active_span);
    vglIsaacSdkSparseTake(&s, 2);
    assert(sdk_calls==2 && s.canonical_seen==2 && s.canonical_issued==2);
    assert(s.canonical_draw_errors==2);
    assert(s.canonical_selected==1 && !s.canonical_unpaired);
    assert(s.bucket[ISAAC_SDK_DRAW].calls==1 && s.bucket[ISAAC_SDK_DRAW].us==8);
    assert(s.bucket[ISAAC_SDK_CANONICAL].calls==1 && s.bucket[ISAAC_SDK_CANONICAL].us==15);
    assert(s.clock_reads==6 && clock_calls==6);
    /* Two actual SDK calls in one selected span: both measured intervals and
     * the outer are discarded, without attempting to undo an eager max. */
    vglIsaacSdkSparseTake(NULL, 0); clock_calls=sdk_calls=0; scene_extra_draws=1;
    assert(vglIsaacDrawCanonicalQuads(6));
    assert(!isaac_sdk_active_span);
    vglIsaacSdkSparseTake(&s, 1);
    assert(sdk_calls==2 && s.canonical_seen==1 && s.canonical_issued==2);
    assert(s.canonical_draw_errors==2);
    assert(s.canonical_selected==1 && s.canonical_unpaired==1);
    assert(!s.bucket[ISAAC_SDK_DRAW].calls && !s.bucket[ISAAC_SDK_CANONICAL].calls);
    assert(!s.bucket[ISAAC_SDK_DRAW].us && !s.bucket[ISAAC_SDK_DRAW].max_us);
    assert(s.clock_reads==8 && clock_calls==8);
    vglIsaacSdkSparseTake(NULL, 0); sdk_calls=clock_calls=0; legal=0;
    assert(!vglIsaacDrawCanonicalQuads(6));
    vglIsaacSdkSparseTake(&s, 1);
#if defined(SKIP_ERROR_HANDLING) && !defined(HAVE_ISAAC_LASER_ATLAS_NEAREST)
    assert(sdk_calls==1 && s.canonical_issued==1 && !s.canonical_unpaired);
    assert(s.bucket[ISAAC_SDK_DRAW].calls==1 && s.clock_reads==6);
#else
    assert(!sdk_calls && !s.canonical_issued && s.canonical_unpaired==1);
    assert(!s.bucket[ISAAC_SDK_DRAW].calls && s.clock_reads==4);
#endif
    legal=1;
#ifdef HAVE_ISAAC_LASER_ATLAS_NEAREST
    vglIsaacSdkSparseTake(NULL, 0); nearest=1; bind_failure=1; sdk_calls=bind_calls=0;
    memset(&isaac_light_nearest_state, 0, sizeof isaac_light_nearest_state);
    assert(vglIsaacDrawCanonicalQuads(6));
    assert(sdk_calls==0 && bind_calls==2); /* real helper's failed bind + failed restore */
    vglIsaacSdkSparseTake(&s, 1);
    assert(s.canonical_seen==1 && s.canonical_selected==1 && s.canonical_issued==0);
    assert(s.canonical_unpaired==1 && s.bucket[ISAAC_SDK_DRAW].calls==0);
    assert(!s.canonical_draw_errors); /* failed texture binds are not SDK Draw */
    assert(s.bucket[ISAAC_SDK_CANONICAL].calls==0 && s.clock_reads==4);
#endif
    vglIsaacSdkSparseTake(NULL, 0);
    isaacSdkSparseAdd(ISAAC_SDK_QUEUE, 100, 99);
    isaacSdkSparseAdd(ISAAC_SDK_QUEUE, 0, (uint64_t)UINT32_MAX+1);
    isaacSdkSparseAdd(ISAAC_SDK_QUEUE, 0, 1);
    vglIsaacSdkSparseTake(&s, 3);
    assert(s.bad_clock==1 && s.saturation==2 && s.bucket[ISAAC_SDK_QUEUE].calls==3);
    assert(s.bucket[ISAAC_SDK_QUEUE].us==UINT32_MAX && s.bucket[ISAAC_SDK_QUEUE].max_us==UINT32_MAX);
    puts("SDK-sparse actual canonical/helper: residues, 31/32/33, actual calls, SDK return, skip/no-draw, nested/multiple paired populations, clocks, reset and saturation PASS");
    return 0;
}
'''
        path = source / f"sdk-sparse-{mode}-host.c"
        path.write_text(fixture, encoding="utf-8", newline="\n")
        options = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-variable", "-I", str(recipe)]
        extra = []
        if sparse:
            options += ["-DHAVE_ISAAC_GL_TIME_SDK_SPARSE=1", "-DHAVE_ISAAC_GL_TIME_PROFILE=1",
                        "-DHAVE_ISAAC_CANONICAL_QUAD_ZERO_COPY=1", "-DISAAC_SDK_SPARSE_HOST_TEST=1"]
            extra += [str(recipe / "isaac_sdk_sparse_profile.c")]
        if mode == "zero":
            options += ["-DHAVE_ISAAC_GL_TIME_SDK_SPARSE=0"]
        for skip_error in (False, True):
            skip_flags = ["-DSKIP_ERROR_HANDLING=1"] if skip_error else []
            exe = source / f"sdk-sparse-{mode}-skip{int(skip_error)}-host.exe"
            run([cc, *options, *skip_flags, str(path), *extra, "-o", str(exe)])
            print(run([str(exe)]).decode().strip())
        if not sparse:
            # Same input basename and NDEBUG remove assertion location bytes:
            # compare the actual optimized original vs patched OFF/explicit0
            # canonical translation unit, not just absence of clock symbols.
            obj_input = source / "sdk-sparse-off-object.c"
            obj_input.write_text(fixture, encoding="utf-8", newline="\n")
            obj = source / f"sdk-sparse-{mode}.obj"
            defines = ["-DHAVE_ISAAC_GL_TIME_SDK_SPARSE=0"] if mode == "zero" else []
            run([cc, "-std=c11", "-O2", "-DNDEBUG", "-I", str(recipe),
                 *defines, "-c", str(obj_input), "-o", str(obj)])
            identity = bytearray(obj.read_bytes())
            if os.name == "nt":
                # Only COFF TimeDateStamp differs between sequential builds;
                # retain every section, relocation, symbol and code byte.
                identity[4:8] = bytes(4)
            if off_object is None:
                off_object = identity
            else:
                assert identity == off_object, f"canonical {mode} object changed"
                note = " (COFF timestamp excluded)" if os.name == "nt" else ""
                print(f"SDK-sparse actual canonical {mode} object equals original PASS{note}")


def _check_sdk_sparse_edges(recipe: Path, source: Path, misc: str, gxm: str, cc: str) -> None:
    """Execute freshly patched SDK spans; count failed/retried actual calls."""
    spans = []
    for name in ("vertex", "fragment", "clear_draw"):
        match = re.search(rf"ISAAC_SDK_BEGIN\(isaac_sdk_{name}_t0\);.*?"
                          rf"ISAAC_SDK_END\([^;]*isaac_sdk_{name}_t0\);", misc, re.S)
        assert match, name
        spans.append(match.group(0))
    queue = re.search(r"ISAAC_SDK_BEGIN\(isaac_sdk_queue_t0\);.*?"
                      r"ISAAC_SDK_END\(ISAAC_SDK_QUEUE, isaac_sdk_queue_t0\);", gxm, re.S)
    aliases = re.findall(r"(?m)^#define sceGxm(?:Begin|End)Scene\(\.\.\.\) \\\n[^\n]+", gxm)
    assert queue and len(aliases) == 2
    fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "isaac_sdk_sparse_profile_internal.h"
static uint64_t now;
static unsigned reads, reserves, draws, queues, begins, ends;
static int ctx, *gxm_context=&ctx, idx, *depth_clear_indices=&idx;
static int sync[2], *gxm_sync_objects[2]={sync,sync+1};
static unsigned gxm_front_buffer_index, gxm_back_buffer_index=1;
static int queue_cb_data;
enum { SCE_GXM_PRIMITIVE_TRIANGLE_FAN=3, SCE_GXM_INDEX_FORMAT_U16=2 };
uint64_t sceKernelGetProcessTimeWide(void) { ++reads; return now++; }
static int sceGxmReserveVertexDefaultUniformBuffer(int *c, void **out)
{ assert(c==&ctx); *out=&idx; ++reserves; now+=3; return -5; }
static int sceGxmReserveFragmentDefaultUniformBuffer(int *c, void **out)
{ assert(c==&ctx); *out=&ctx; ++reserves; now+=5; return -6; }
static int sceGxmDraw(int *c, int p, int f, int *i, int count)
{ assert(c==&ctx && p==3 && f==2 && i==&idx && count==4); ++draws; now+=7; return -7; }
static int sceGxmDisplayQueueAddEntry(int *front, int *back, int *data)
{ assert(front==sync && back==sync+1 && data==&queue_cb_data); ++queues; now+=11; return -9; }
static int sceGxmBeginScene(int result) { ++begins; now+=9; return result; }
static int sceGxmEndScene(int result) { ++ends; now+=13; return result; }
'''
    fixture += "\n" + "\n".join(aliases)
    fixture += '\nstatic void clear_spans(void) { void *vbuffer, *fbuffer;\n'
    fixture += 'ISAAC_SDK_BEGIN(isaac_sdk_clear_t0);\n' + "\n".join(spans)
    fixture += '\nISAAC_SDK_END(ISAAC_SDK_CLEAR, isaac_sdk_clear_t0);\nassert(vbuffer==&idx && fbuffer==&ctx);\n}\n'
    fixture += '\nstatic void queue_span(void) {\n' + queue.group(0) + '\n}\n'
    fixture += r'''
int main(void) {
    IsaacSdkSparseProfile s;
    vglIsaacSdkSparseTake(NULL,1);
    assert(reads==0);
    clear_spans(); queue_span();
    /* One existing BeginScene bracket can enclose failed + retry attempts.
     * These are actual call aliases from the patched source, not predictions. */
    uint64_t t=isaacSdkSparseClock();
    int r=sceGxmBeginScene(-1);
    if (r) r=sceGxmBeginScene(0);
    assert(r==0 && isaacSdkSparseClock()-t==19);
    t=isaacSdkSparseClock();
    assert(sceGxmEndScene(-11)==-11);
    assert(isaacSdkSparseClock()-t==14);
    vglIsaacSdkSparseTake(&s,2);
    assert(reserves==2 && draws==1 && queues==1 && begins==2 && ends==1);
    assert(s.begin_calls==2 && s.end_calls==1 && s.clock_reads==16 && reads==16);
    assert(s.bucket[ISAAC_SDK_CLEAR].calls==1 && s.bucket[ISAAC_SDK_CLEAR].us==22);
    assert(s.bucket[ISAAC_SDK_CLEAR_VERTEX].us==4 && s.bucket[ISAAC_SDK_CLEAR_FRAGMENT].us==6);
    assert(s.bucket[ISAAC_SDK_CLEAR_DRAW].us==8 && s.bucket[ISAAC_SDK_QUEUE].us==12);
    assert(!s.canonical_issued && !s.canonical_seen && !s.bad_clock && !s.saturation);
    puts("SDK-sparse actual clear/queue spans + scene aliases: args/returns, failed calls, retry counts and exact clocks PASS");
    return 0;
}
'''
    path, exe = source / "sdk-sparse-edges-host.c", source / "sdk-sparse-edges-host.exe"
    path.write_text(fixture, encoding="utf-8", newline="\n")
    run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(recipe),
         "-DHAVE_ISAAC_GL_TIME_SDK_SPARSE=1", "-DHAVE_ISAAC_GL_TIME_PROFILE=1",
         "-DHAVE_ISAAC_CANONICAL_QUAD_ZERO_COPY=1", "-DISAAC_SDK_SPARSE_HOST_TEST=1",
         str(path), str(recipe / "isaac_sdk_sparse_profile.c"), "-o", str(exe)])
    print(run([str(exe)]).decode().strip())


def check_fbo_region_observe(source: Path, cc: str) -> None:
    """Execute the freshly applied 0009 code, not a copied region model."""
    gxm = (source / "source/gxm.c").read_text(encoding="utf-8")
    region = re.search(
        r"(?ms)^#ifdef HAVE_ISAAC_FBO_VALID_REGION\n/\* Isaac FBO valid region"
        r".*?(?=^#define MAX_SCENES_PER_FRAME)", gxm)
    accessor = re.search(
        r"static GLboolean isaac_fbo_region_scene_reset_pending\(void\) "
        r"\{\n\treturn needs_scene_reset;\n\}", gxm)
    assert region and accessor, "missing final 0009 region/accessor"
    fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define HAVE_ISAAC_FBO_VALID_REGION 1
typedef uint8_t GLboolean;
typedef struct { int value; } texture;
typedef struct { texture *tex; int width, height; } framebuffer;
typedef struct { uint32_t xMax, yMax; } SceGxmValidRegion;
static framebuffer *active_write_fb, *in_use_framebuffer;
static int is_rendering_display;
static GLboolean dirty_framebuffer, dirty_query;
static struct { int x, y, w, h; } gl_viewport;
'''
    fixture += region.group(0)
    # Same ordering as gxm.c: stock flag precedes the final accessor body.
    fixture += "\nstatic GLboolean needs_scene_reset;\n" + accessor.group(0)
    fixture += r'''
int main(void) {
    texture tex = {0};
    framebuffer fb = {&tex, 512, 512};
    uint32_t counters[8], census[20];
    int result;
    active_write_fb = in_use_framebuffer = &fb;
    gl_viewport.w = 480; gl_viewport.h = 270;
    assert(vglIsaacSetupFboValidRegion(0) == 0);
    isaacFboRegionNoteViewport(0, 0, 480, 270);
    assert(ISAAC_FBO_REGION_FOR_SCENE() == NULL);
    assert(isaac_fbo_region_known && !isaac_fbo_region_active);
    isaacFboRegionNoteViewport(0, 0, 480, 271);
    vglIsaacFboValidRegionStats(counters, census);
    assert(counters[0] == 1 && counters[1] == 1);
    assert(counters[3] == 0 && counters[5] == 1); /* OBSERVE catches it. */
    needs_scene_reset = 1;
    isaacFboRegionNoteViewport(0, 0, 480, 271);
    needs_scene_reset = 0; dirty_framebuffer = 1;
    isaacFboRegionNoteViewport(0, 0, 480, 271);
    dirty_framebuffer = 0; dirty_query = 1;
    isaacFboRegionNoteViewport(0, 0, 480, 271);
    dirty_query = 0; is_rendering_display = 1;
    isaacFboRegionNoteViewport(0, 0, 480, 271);
    is_rendering_display = 0; in_use_framebuffer = NULL;
    isaacFboRegionNoteViewport(0, 0, 480, 271);
    in_use_framebuffer = &fb;
    ISAAC_FBO_REGION_INTERNAL_VIEWPORT_BEGIN();
    isaacFboRegionNoteViewport(0, 0, 512, 512);
    ISAAC_FBO_REGION_INTERNAL_VIEWPORT_END();
    vglIsaacFboValidRegionStats(counters, census);
    assert(counters[5] == 0); /* No open-scene continuation / internal call. */
    assert(vglIsaacSetupFboValidRegion(1) == 1);
    assert(ISAAC_FBO_REGION_FOR_SCENE() != NULL);
    result = 1;
    ISAAC_FBO_REGION_RETRY_NULL(result, 0);
    assert(result == 0 && !isaac_fbo_region_known && !isaac_fbo_region_active);
    isaacFboRegionNoteViewport(0, 0, 480, 271);
    vglIsaacFboValidRegionStats(counters, census);
    assert(counters[4] == 1 && counters[5] == 0);
    assert(ISAAC_FBO_REGION_FOR_SCENE() != NULL);
    result = 0;
    ISAAC_FBO_REGION_RETRY_NULL(result, 1);
    assert(result == 0);
    isaacFboRegionNoteViewport(0, 0, 480, 271);
    vglIsaacFboValidRegionStats(counters, census);
    assert(counters[3] == 1 && counters[5] == 1 && !isaac_fbo_region_apply);
    gl_viewport.w = gl_viewport.h = 512;
    assert(ISAAC_FBO_REGION_FOR_SCENE() == NULL && !isaac_fbo_region_known);
    gl_viewport.w = 480; gl_viewport.h = 270;
    isaac_fbo_region_vp_tex = NULL;
    assert(ISAAC_FBO_REGION_FOR_SCENE() == NULL && !isaac_fbo_region_known);
    vglIsaacFboValidRegionStats(counters, census);
    assert(counters[2] == 1);
    puts("vitaGL native FBO region OBSERVE / reset / retry: PASS");
    return 0;
}
'''
    fixture_path = source / "fbo-region-observe.c"
    fixture_path.write_text(fixture, encoding="utf-8", newline="\n")
    executable = source / "fbo-region-observe.exe"
    run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
         str(fixture_path), "-o", str(executable)])
    print(run([str(executable)]).decode("utf-8").strip())


def check_fs_probe(source: Path, fs_probe: bool) -> None:
    shaders = (source / "source" / "custom_shaders.c").read_bytes()
    header = (source / "source" / "vitaGL.h").read_bytes()
    for needle in FS_PROBE_NEEDLES:
        if (needle in shaders) != fs_probe:
            raise AssertionError(
                f"custom_shaders.c evidence {needle!r}: "
                f"present={needle in shaders}, expected {fs_probe}"
            )
        if needle in header:
            raise AssertionError(
                f"ColorOffset FS probe leaked into vitaGL.h: {needle!r}")


def prove_fs_probe_matrix(
    recipe: Path,
    temporary: Path,
    stock_tar: Path | None,
    repository_archive: bytes | None,
) -> None:
    """0008 applies on the 0001+0002+0003 tree, then 0004, then the gxm.c
    patches with and without 0007 (build.sh order); it also applies after the
    gxm.c patches (disjoint files); and it is rejected without 0003."""
    base = ("0001-deterministic-build-and-init-oob.patch",
            "0002-exact-gpu-draw-optimizations.patch")
    policies = ("isaac_gpu_draw_policy.h", "isaac_gxm_state_policy.h")

    def stock(name: str) -> Path:
        source = materialize_stock(
            temporary / ("probe-" + name), stock_tar, repository_archive)
        for patch in base:
            apply_patch(source, recipe / patch)
        for policy in policies:
            shutil.copyfile(recipe / policy, source / "source" / policy)
        return source

    def coloroffset(source: Path) -> None:
        apply_patch(source, recipe / "0003-exact-coloroffset-gpu-optimizations.patch")
        policy = "isaac_coloroffset_gpu_policy.h"
        shutil.copyfile(recipe / policy, source / "source" / policy)

    orders = {
        # 0008 before 0004, then 0005+0006 only (GL_TIME_PROFILE without 0007).
        "gl1-fbo0": ("fs_probe", "shader_cache", "scene_timer", "scene_split"),
        # 0008 before 0004, then 0005+0006+0007 (the fbo-rt-8 flag set).
        "gl1-fbo1": ("fs_probe", "shader_cache", "scene_timer", "scene_split",
                     "fbo_rt_scenes"),
        # 0008 before 0004, then 0007 alone.
        "gl0-fbo1": ("fs_probe", "shader_cache", "fbo_rt_scenes"),
        # 0008 last: the gxm.c patches and 0004 do not disturb its context.
        "after-gxm": ("shader_cache", "scene_timer", "scene_split",
                      "fbo_rt_scenes", "fs_probe"),
    }
    patches = dict(GXM_PATCHES)
    patches["fs_probe"] = FS_PROBE_PATCH
    patches["shader_cache"] = "0004-hardened-custom-shader-cache.patch"
    digests = {}
    for name, order in orders.items():
        source = stock(name)
        coloroffset(source)
        for patch_name in order:
            apply_patch(source, recipe / patches[patch_name])
        check_fs_probe(source, True)
        check_gxm(source, "scene_split" in order, "fbo_rt_scenes" in order)
        digests[name] = sha256(source / "source" / "custom_shaders.c")
        print(f"vitaGL ColorOffset FS probe matrix {name}: {digests[name]}")
    if len(set(digests.values())) != 1:
        raise AssertionError(
            "0008 and 0004 do not commute on custom_shaders.c")
    if digests["gl1-fbo1"] != EXPECTED_HASHES["1:1:1"][0]:
        raise AssertionError(
            "probe matrix custom_shaders.c differs from the 1:1:1 matrix hash")

    # Negative: without the 0003 machinery the probe has nothing to hook.
    source = stock("without-0003")
    command = ["git", "-c", "core.autocrlf=false", "-c", "core.eol=lf",
               "apply", "--check", str(recipe / FS_PROBE_PATCH)]
    completed = subprocess.run(
        command, cwd=source, check=False, capture_output=True)
    if completed.returncode == 0:
        raise AssertionError("0008 applied without 0003")
    print("vitaGL ColorOffset FS probe matrix without-0003: rejected "
          f"(git apply --check rc={completed.returncode})")


def apply_patch(source: Path, patch: Path) -> None:
    # The first value simulates a hostile Windows/global setting.  The later
    # build-owned value must win and preserve the stock archive's canonical LF.
    command = [
        "git",
        "-c",
        "core.autocrlf=true",
        "-c",
        "core.eol=crlf",
        "-c",
        "core.autocrlf=false",
        "-c",
        "core.eol=lf",
        "apply",
    ]
    if patch.name in ("0016-isaac-laser-light-halo-clip.patch",
                      "0017-isaac-laser-light-nearest.patch", ATLAS_NEAREST_PATCH,
                      "0025-isaac-sdk-sparse-profile.patch"):
        command.append("--unidiff-zero")
    run(command + ["--check", str(patch)], cwd=source)
    run(command + [str(patch)], cwd=source)


def check_nearest_shadow(source: Path, recipe: Path, cc: str | None) -> None:
    """Use the freshly patched wrapper, not a copy of its failure policy."""
    draw = (source / "source/draw.c").read_text(encoding="utf-8")
    include = draw.index('#include "isaac_laser_light_nearest.h"')
    if include >= draw.index("#ifdef HAVE_ISAAC_CANONICAL_QUAD_ZERO_COPY"):
        raise AssertionError("nearest helper is inside the canonical-only guard/function")
    if cc is None:
        return
    shadow = (source / "source/isaac_gxm_state_shadow.c").read_text(encoding="utf-8")
    wrapper = re.search(r"(?ms)^int isaacGxmSetFragmentTexture\(.*?^\}", shadow)
    if wrapper is None:
        raise AssertionError("nearest lost the actual status-returning texture wrapper")
    fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "isaac_gxm_state_policy.h"
typedef struct { int unused; } SceGxmContext;
typedef struct { uint32_t words[4]; } SceGxmTexture;
static isaac_gxm_state_policy isaac_state_policy;
static unsigned native_calls, fail_at;
static SceGxmTexture actual_texture;
static int sceGxmSetFragmentTexture(SceGxmContext *context, unsigned unit,
        const SceGxmTexture *texture) {
    assert(context && unit == 0);
    if (++native_calls == fail_at) return -7;
    actual_texture = *texture;
    return 0;
}
'''
    fixture += wrapper.group(0)
    fixture += r'''
int main(void) {
    SceGxmContext context = {0};
    SceGxmTexture original = {{1, 2, 3, 4}}, point = {{1, 2, 99, 4}};
    isaac_gxm_state_begin_scene(&isaac_state_policy, (uintptr_t)&context, 1);
    assert(isaacGxmSetFragmentTexture(&context, 0, &original) == 0);
    assert(native_calls == 1);
    assert(isaacGxmSetFragmentTexture(&context, 0, &original) == 0);
    assert(native_calls == 1); /* real successful cached hit */
    fail_at = 2;
    assert(isaacGxmSetFragmentTexture(&context, 0, &point) == -7);
    assert(native_calls == 2 && !memcmp(&actual_texture, &original, 16));
    fail_at = 0;
    assert(isaacGxmSetFragmentTexture(&context, 0, &point) == 0);
    assert(native_calls == 3 && !memcmp(&actual_texture, &point, 16));
    fail_at = 4; /* restoring original fails: next original must not hit */
    assert(isaacGxmSetFragmentTexture(&context, 0, &original) == -7);
    assert(native_calls == 4 && !memcmp(&actual_texture, &point, 16));
    fail_at = 0;
    assert(isaacGxmSetFragmentTexture(&context, 0, &original) == 0);
    assert(native_calls == 5 && !memcmp(&actual_texture, &original, 16));
    assert(isaacGxmSetFragmentTexture(&context, 0, &original) == 0);
    assert(native_calls == 5);
    fail_at = 6;
    assert(isaacGxmSetFragmentTexture(&context, 0, &point) == -7);
    fail_at = 0;
    assert(isaacGxmSetFragmentTexture(&context, 0, &original) == 0);
    assert(native_calls == 7 && !memcmp(&actual_texture, &original, 16));
    puts("actual nearest GXM shadow: success hit / failed bind / failed restore / native retry PASS");
    return 0;
}
'''
    path = source / "nearest-shadow-host.c"
    path.write_text(fixture, encoding="utf-8", newline="\n")
    executable = source / "nearest-shadow-host.exe"
    run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(recipe),
         str(path), "-o", str(executable)])
    print(run([str(executable)]).decode("utf-8").strip())


def prove_staging_matrix(
    recipe: Path, temporary: Path, stock_tar: Path | None,
    repository_archive: bytes | None,
    cc: str | None = None,
) -> None:
    """The optional staging path preserves shader code and all other files,
    with both shader-cache and P8 prerequisite states."""
    for cache in (False, True):
        for p8 in (False, True):
            name = f"staging-cache{int(cache)}-p8{int(p8)}"
            source = materialize_stock(temporary / name, stock_tar, repository_archive)
            for patch in ("0001-deterministic-build-and-init-oob.patch",
                          "0002-exact-gpu-draw-optimizations.patch",
                          "0003-exact-coloroffset-gpu-optimizations.patch",
                          FS_PROBE_PATCH):
                apply_patch(source, recipe / patch)
            if cache:
                apply_patch(source, recipe / "0004-hardened-custom-shader-cache.patch")
            if p8:
                apply_patch(source, recipe / "0010-isaac-p8-safe-upload.patch")
            before = (source / "source/custom_shaders.c").read_text(encoding="utf-8")
            other_files = ("vitaGL.h", "gxm.c", "textures.c", "framebuffers.c")
            other_hashes = tuple(sha256(source / "source" / f) for f in other_files)
            apply_patch(source, recipe / STAGING_PATCH)
            after = (source / "source/custom_shaders.c").read_text(encoding="utf-8")
            if tuple(sha256(source / "source" / f) for f in other_files) != other_hashes:
                raise AssertionError("staging proof changed GPU memory/texture/RT or ABI code")
            fs_marker = "static const char isaac_coloroffset_fs_neutral_source[] ="
            fs_end = "static char *isaac_coloroffset_fs_probe_nodiscard_source;"
            if before.split(fs_marker)[1].split(fs_end)[0] != after.split(fs_marker)[1].split(fs_end)[0]:
                raise AssertionError("staging proof changed the neutral fragment shader")
            if after.count("isaac_coloroffset_fragment_program_is_exact(p)") != 1:
                raise AssertionError("staging proof did not share the draw-local immutable check")
            for needle in (
                '#define ISAAC_COLOROFFSET_STAGING_MEMCPY vgl_fast_memcpy',
                'const uint32_t isaac_copy_stride = streams[0].stride;',
                'gpu_alloc_mapped_temp(top_idx * isaac_copy_stride)',
                'isaac_neutral_staging == ISAAC_COLOROFFSET_STAGING_UNKNOWN',
                'if (isaac_staged && isaac_single_stream &&',
                'isaac_neutral_staging == ISAAC_COLOROFFSET_STAGING_NEUTRAL',
            ):
                if needle not in after:
                    raise AssertionError(f"staging proof lost {needle}")
            digest = sha256(source / "source/custom_shaders.c")
            if cache and digest != STAGING_WITH_CACHE_SHA256:
                raise AssertionError(f"staging source hash drifted: {digest}")
            print(f"vitaGL {name}: {digest}")
            # Preserve the P1+P3-only leg, then independently prove P2 and
            # P2+P3 in canonical recipe order on the same freshly staged input.
            single_source = temporary / (name + "-single-final-bind")
            shutil.copytree(source, single_source)
            apply_patch(single_source, recipe / SINGLE_BIND_PATCH)
            final = (single_source / "source/custom_shaders.c").read_text(encoding="utf-8")
            if tuple(sha256(single_source / "source" / f) for f in other_files) != other_hashes:
                raise AssertionError("single final bind changed an unrelated file")
            check_single_final_bind(after, final, single_source, cc if not cache and not p8 else None)
            digest = sha256(single_source / "source/custom_shaders.c")
            if cache and digest != SINGLE_BIND_WITH_CACHE_SHA256:
                raise AssertionError(f"single final bind source hash drifted: {digest}")
            print(f"vitaGL {name}-single-final-bind: {digest}")

            apply_patch(source, recipe / PLAIN_PATCH)
            plain = (source / "source/custom_shaders.c").read_text(encoding="utf-8")
            if tuple(sha256(source / "source" / f) for f in other_files) != other_hashes:
                raise AssertionError("plain specialization changed unrelated driver files")
            if before.split(fs_marker)[1].split(fs_end)[0] != plain.split(fs_marker)[1].split(fs_end)[0]:
                raise AssertionError("plain specialization changed the neutral fallback FS")
            for needle in (
                '#include "isaac_coloroffset_plain_vitagl.h"',
                'isaac_coloroffset_plain_select(p)',
                'isaac_coloroffset_plain_release(p)',
                'isaac_coloroffset_plain_link(p)',
                'progs[i].isaac_coloroffset_plain = NULL;',
                'isaac_neutral_staging == ISAAC_COLOROFFSET_STAGING_PLAIN',
                'isaac_coloroffset_fs_probe_select(',
            ):
                if needle not in plain:
                    raise AssertionError(f"plain specialization lost {needle}")
            digest = sha256(source / "source/custom_shaders.c")
            if cache and digest != PLAIN_WITH_CACHE_SHA256:
                raise AssertionError(f"plain source hash drifted: {digest}")
            print(f"vitaGL plain-{name}: {digest}")

            apply_patch(source, recipe / SINGLE_BIND_PATCH)
            final = (source / "source/custom_shaders.c").read_text(encoding="utf-8")
            if tuple(sha256(source / "source" / f) for f in other_files) != other_hashes:
                raise AssertionError("plain plus single final bind changed an unrelated file")
            check_single_final_bind(plain, final, source, cc if not cache and not p8 else None)
            digest = sha256(source / "source/custom_shaders.c")
            expected = (PLAIN_SINGLE_BIND_WITH_CACHE_SHA256 if cache else
                        PLAIN_SINGLE_BIND_WITHOUT_CACHE_SHA256)
            if digest != expected:
                raise AssertionError(f"plain plus single final bind source hash drifted: {digest}")
            print(f"vitaGL plain-{name}-single-final-bind: {digest}")
            if cache and p8:
                # Keep the ordinary P5 leg below on its own P2+P3 input.
                # Halo changes draw/shared ABI and must be a separate branch.
                halo_source = temporary / (name + "-light-halo")
                shutil.copytree(source, halo_source)
                for patch in (
                    "0005-isaac-scene-timer.patch", "0006-isaac-scene-split.patch",
                    "0007-isaac-fbo-rt-scenes.patch", "0009-isaac-fbo-valid-region.patch",
                    "0011-isaac-exact-laser-p8.patch",
                ):
                    apply_patch(halo_source, recipe / patch)
                pair_patch = recipe / "0015-isaac-coloroffset-plain-vertex-pair.patch"
                if pair_patch.is_file():
                    paired = temporary / (name + "-light-halo-paired")
                    shutil.copytree(halo_source, paired)
                    apply_patch(paired, pair_patch)
                    apply_patch(paired, recipe / "0016-isaac-laser-light-halo-clip.patch")
                    paired_source = paired / "source/custom_shaders.c"
                    if sha256(paired_source) != LIGHT_HALO_WITH_PAIR_SHA256:
                        raise AssertionError("light halo plus paired VS source hash drifted")
                    if "isaac_plain_vertex_pair_select(p, &isaac_pair_vertex)" not in paired_source.read_text(encoding="utf-8"):
                        raise AssertionError("light halo lost paired VS selection")
                    print(f"vitaGL light-halo+paired-VS: {sha256(paired_source)}")
                    apply_patch(paired, recipe / "0017-isaac-laser-light-nearest.patch")
                    check_nearest_shadow(paired, recipe, None)
                    if sha256(paired_source) != LIGHT_HALO_WITH_PAIR_SHA256:
                        raise AssertionError("nearest changed halo+pair custom shaders")
                    print("vitaGL light-halo+paired-VS+nearest: apply/source identity PASS")
                    atlas_sources = []
                    for transform_on in (False, True):
                        atlas = temporary / (name + f"-atlas-t{int(transform_on)}")
                        shutil.copytree(paired, atlas)
                        if transform_on:
                            apply_patch(atlas, recipe / TRANSFORM_PATCH)
                        original = (atlas / "source/custom_shaders.c").read_text(encoding="utf-8")
                        apply_patch(atlas, recipe / ATLAS_NEAREST_PATCH)
                        digest = sha256(atlas / "source/custom_shaders.c")
                        if digest != ATLAS_NEAREST_WITH_CACHE_SHA256[int(transform_on)]:
                            raise AssertionError(f"atlas nearest T{int(transform_on)} hash drifted: {digest}")
                        check_atlas_nearest_callers(atlas, recipe, original, cc)
                        atlas_sources.append((atlas / "source/draw.c").read_bytes())
                        print(f"vitaGL atlas nearest T{int(transform_on)}: {digest}")
                        apply_patch(atlas, recipe / "0020-isaac-native-resource-profile.patch")
                        assert sha256(atlas / "source/custom_shaders.c") == digest
                        assert (atlas / "source/draw.c").read_bytes() == atlas_sources[-1]
                        print(f"native resource profile after full atlas/P5/P8/VR T{int(transform_on)}: apply/shader/draw identity PASS")
                    if atlas_sources[0] != atlas_sources[1]:
                        raise AssertionError("optional Transform changed the atlas draw callers")
                    apply_patch(paired, recipe / TRANSFORM_PATCH)
                    if sha256(paired_source) != TRANSFORM_WITH_HALO_SHA256:
                        raise AssertionError("Transform plus halo/pair/nearest source hash drifted")
                    print("vitaGL Transform+light-halo+paired-VS+nearest: apply/source identity PASS")
                apply_patch(halo_source, recipe / "0016-isaac-laser-light-halo-clip.patch")
                digest = sha256(halo_source / "source/custom_shaders.c")
                if digest != LIGHT_HALO_WITH_CACHE_SHA256:
                    raise AssertionError(f"light halo native source hash drifted: {digest}")
                draw = (halo_source / "source/draw.c").read_text(encoding="utf-8")
                if draw.count("ISAAC_LIGHT_HALO_ARG(") != 6:
                    raise AssertionError("not every indexed-draw caller handles the optional output")
                for helper in ("isaac_laser_light_halo_policy.h", "isaac_laser_light_halo_vitagl.h"):
                    if not (recipe / helper).is_file():
                        raise AssertionError(f"missing light halo helper: {helper}")
                if f"shader_source_sha={digest}" not in (recipe / "build.sh").read_text(encoding="utf-8"):
                    raise AssertionError("build.sh does not pin the full light halo source")
                print(f"vitaGL light-halo+P8+valid-region: {digest}")
                apply_patch(halo_source, recipe / "0017-isaac-laser-light-nearest.patch")
                check_nearest_shadow(halo_source, recipe, cc)
                if sha256(halo_source / "source/custom_shaders.c") != digest:
                    raise AssertionError("nearest changed halo custom shaders")
                if not (recipe / "isaac_laser_light_nearest.h").is_file():
                    raise AssertionError("missing light laser nearest helper")
                print("vitaGL light-halo+P8+valid-region+nearest: apply/source identity PASS")

            apply_patch(source, recipe / PLAIN_VERTEX_PAIR_PATCH)
            paired = (source / "source/custom_shaders.c").read_text(encoding="utf-8")
            if tuple(sha256(source / "source" / f) for f in other_files) != other_hashes:
                raise AssertionError("plain vertex pair changed an unrelated driver file")
            if before.split(fs_marker)[1].split(fs_end)[0] != paired.split(fs_marker)[1].split(fs_end)[0]:
                raise AssertionError("plain vertex pair changed the neutral fallback FS")
            check_single_final_bind(plain, final, source,
                                    cc if not cache and not p8 else None, paired=paired)
            digest = sha256(source / "source/custom_shaders.c")
            expected = (PLAIN_VERTEX_PAIR_WITH_CACHE_SHA256 if cache else
                        PLAIN_VERTEX_PAIR_WITHOUT_CACHE_SHA256)
            if digest != expected:
                raise AssertionError(f"plain vertex pair source hash drifted: {digest}")
            print(f"vitaGL plain-{name}-vertex-pair: {digest}")
            if "isaac_coloroffset_transform_unchanged" in paired:
                raise AssertionError("Transform OFF changed materialized P5 source")
            apply_patch(source, recipe / TRANSFORM_PATCH)
            transform = (source / "source/custom_shaders.c").read_text(encoding="utf-8")
            if tuple(sha256(source / "source" / f) for f in other_files) != other_hashes:
                raise AssertionError("Transform suppression changed unrelated native files")
            added_include = '#include "isaac_coloroffset_transform_uniform.h"\n'
            added_guard = (
                '\t/* No extra source read for already-dirty or unsupported writes. */\n'
                '\tif (!dirty_shader_vert_unifs && count == 1 && !transpose && offs == 0 &&\n'
                '\t\t\tisaac_coloroffset_transform_unchanged(u, value)) {\n'
                '\t\treturn;\n\t}\n\n')
            if (transform.count(added_include) != 1 or transform.count(added_guard) != 1 or
                    transform.replace(added_include, "").replace(added_guard, "") != paired):
                raise AssertionError("Transform patch changed more than its include/early return")
            digest = sha256(source / "source/custom_shaders.c")
            expected = TRANSFORM_WITH_CACHE_SHA256 if cache else TRANSFORM_WITHOUT_CACHE_SHA256
            if digest != expected:
                raise AssertionError(f"Transform source hash drifted: {digest}")
            print(f"vitaGL plain-{name}-vertex-pair-transform: {digest}")


def check_atlas_nearest_callers(source: Path, recipe: Path, before: str,
                               cc: str | None) -> None:
    """Compile the freshly patched caller bodies and index-copy macros.
    Native allocation/GXM and shader selection are mocked, not caller control
    flow. This is pointer/legality evidence, not sampler quality or GPU timing.
    """
    after = (source / "source/custom_shaders.c").read_text(encoding="utf-8")
    include = ('#ifdef HAVE_ISAAC_LASER_ATLAS_NEAREST\n'
               '#include "isaac_laser_atlas_nearest.h"\n#endif\n')
    prepare = ('#ifdef HAVE_ISAAC_LASER_ATLAS_NEAREST\n'
               '\tisaac_laser_atlas_nearest_prepare(p, isaac_coloroffset_fragment_exact,\n'
               '\t\tisaac_coloroffset_texture, halo_submission);\n#endif\n')
    if (after.count(include) != 1 or after.count(prepare) != 1 or
            after.replace(include, "").replace(prepare, "") != before):
        raise AssertionError("atlas option changed more than its late prepare/include")
    if prepare + '#ifdef HAVE_PROFILING\n' not in after:
        raise AssertionError("atlas preparation moved before existing sampler/uniform completion")
    draw = (source / "source/draw.c").read_text(encoding="utf-8")
    shared = (source / "source/shared.h").read_text(encoding="utf-8")
    if draw.index('#include "isaac_laser_light_nearest.h"') >= draw.index(
            '#ifdef HAVE_ISAAC_CANONICAL_QUAD_ZERO_COPY'):
        raise AssertionError("atlas draw helper is canonical-only")
    if cc is None:
        return
    index_macros = draw[draw.index('#ifndef INDICES_DRAW_SPEEDHACK'):
                        draw.index('void glDrawArrays(')]
    transport = shared[shared.index('/* custom_shaders.c */'):
                       shared.index('void reset_custom_shaders(')]
    callers = draw[draw.index('#if defined(HAVE_ISAAC_LASER_LIGHT_NEAREST)'):
                   draw.index('void glDrawElementsBaseVertex(')]
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int GLboolean, GLenum, GLsizei, GLint, SceGxmPrimitiveType;
typedef void GLvoid;
typedef struct { int unused; } SceGxmContext;
typedef struct { uint32_t words[4]; } SceGxmTexture;
typedef struct texture texture;
typedef struct { void *ptr; unsigned last_frame; } vbo;
typedef struct { void *index_array_unit; } vao;
#define GL_TRUE 1
#define GL_FALSE 0
#define GL_TRIANGLES 1
#define GL_QUADS 2
#define GL_LINE_STRIP 3
#define GL_LINE_LOOP 4
#define GL_UNSIGNED_SHORT 5
#define GL_UNSIGNED_INT 6
#define GL_UNSIGNED_BYTE 7
#define SCE_GXM_PRIMITIVE_TRIANGLES 8
#define SCE_GXM_INDEX_FORMAT_U16 9
#define SCE_GXM_INDEX_FORMAT_U32 10
#define SCE_GXM_INDEX_SOURCE_INDEX_16BIT 11
#define SCE_GXM_INDEX_SOURCE_INDEX_32BIT 12
#define MODEL_CREATION 13
#define MAX_IDX_NUMBER 4096
#define THREAD_SAFE()
#define SET_GL_ERROR(...) return;
#define SET_GL_ERROR_WITH_VALUE(...) return;
#define vgl_fast_memcpy memcpy
#define gl_primitive_to_gxm(mode, dest, count) do { \
    dest = SCE_GXM_PRIMITIVE_TRIANGLES; \
    prim_is_non_native = ((mode) != GL_TRIANGLES); \
} while (0)
static unsigned vgl_framecount = 17;
static GLboolean prim_is_non_native;
static int phase, cur_program = 1, ffp_vertex_attrib_state = 1;
static vao vao_state, *cur_vao = &vao_state;
static SceGxmContext context, *gxm_context = &context;
static uint16_t canonical[6] = {0, 1, 3, 1, 2, 3};
static uint16_t *isaac_canonical_quads_idx_ptr = canonical;
static uint16_t client[6] = {0, 1, 2, 3, 4, 5};
static uint16_t resident[8] = {99, 0, 1, 2, 3, 4, 5, 99};
static uint16_t clipped[3] = {0, 1, 2};
static uint32_t arena[128];
static unsigned allocations, draws, binds, custom_calls, fixed_calls, restores;
static const void *last_indices, *shader_indices;
static GLsizei last_count;
static int legal, ready, replace_geometry;
static void *gpu_alloc_mapped_temp(size_t bytes) {
    assert(bytes <= sizeof(arena)); ++allocations; return arena;
}
static void scene_reset(void) {}
static void restore_polygon_mode(SceGxmPrimitiveType primitive) {
    assert(primitive == SCE_GXM_PRIMITIVE_TRIANGLES); ++restores;
}
static int sceGxmDraw(SceGxmContext *ctx, SceGxmPrimitiveType primitive,
        int format, const void *indices, GLsizei count) {
    assert(ctx == &context && primitive == SCE_GXM_PRIMITIVE_TRIANGLES);
    assert(format == SCE_GXM_INDEX_FORMAT_U16);
    ++draws; last_indices = indices; last_count = count; return 0;
}
static int sceGxmSetFragmentTexture(SceGxmContext *ctx, unsigned unit,
        const SceGxmTexture *value) {
    assert(ctx == &context && unit == 0 && value); ++binds; return 0;
}
static int sceClibPrintf(const char *format, ...) { (void)format; return 0; }
'''
    harness += transport
    harness += r'''
static GLboolean _glDrawElements_CustomShadersIMPL(uint16_t *indices,
        GLsizei count, uint32_t top, uint32_t base, int type ISAAC_LIGHT_HALO_DECL) {
    (void)top; assert(count == 6 && base == 0);
    assert(type == SCE_GXM_INDEX_SOURCE_INDEX_16BIT);
    ++custom_calls; shader_indices = indices;
    if (halo_submission) {
        halo_submission->atlas_nearest_ready = ready;
        halo_submission->nearest_ready = ready;
#ifdef HAVE_ISAAC_LASER_LIGHT_HALO_CLIP
        if (replace_geometry) {
            halo_submission->indices = clipped; halo_submission->count = 3;
        }
#endif
    }
    return legal;
}
static void _glDrawElements_FixedFunctionIMPL(uint16_t *indices, GLsizei count,
        uint32_t top, uint32_t base, int type) {
    (void)indices; (void)count; (void)top; (void)base; (void)type; ++fixed_calls;
}
'''
    harness += index_macros + callers
    harness += r'''
int main(void) {
    unsigned cases = 0;
    for (int path = 0; path < 4; ++path)
    for (legal = 0; legal < 2; ++legal)
    for (ready = 0; ready < 2; ++ready)
    for (replace_geometry = 0; replace_geometry < 2; ++replace_geometry) {
        vbo buffer = {resident, 0};
        int actual_legal = path == 3 || legal;
        int replaced = 0;
        const void *expected;
#ifndef HAVE_ISAAC_CANONICAL_QUAD_ZERO_COPY
        if (!path) continue;
#endif
        allocations = draws = binds = custom_calls = fixed_calls = restores = 0;
        last_indices = shader_indices = NULL; last_count = 0;
        vao_state.index_array_unit = path == 2 ? &buffer : NULL;
        cur_program = path == 3 ? 0 : 1;
        memset(&isaac_light_nearest_state, 0, sizeof(isaac_light_nearest_state));
        if (!path) {
#ifdef HAVE_ISAAC_CANONICAL_QUAD_ZERO_COPY
            assert(vglIsaacDrawCanonicalQuads(6) == legal);
#endif
        } else glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT,
            path == 2 ? (const void *)(uintptr_t)2 : client);
        assert(restores == 1);
        assert(custom_calls == (path != 3) && fixed_calls == (path == 3));
        if (path != 3)
            assert(shader_indices == (!path ? canonical : path == 2 ? resident + 1 : client));
        if (!actual_legal) {
            assert(draws == 0 && binds == 0 && allocations == 0);
            ++cases; continue;
        }
#ifdef HAVE_ISAAC_LASER_LIGHT_HALO_CLIP
        replaced = replace_geometry && path < 2;
#endif
        expected = replaced ? (const void *)clipped : !path ? (const void *)canonical :
            path == 2 ? (const void *)(resident + 1) : (const void *)arena;
        assert(draws == 1 && last_indices == expected && last_count == (replaced ? 3 : 6));
        assert(allocations == (unsigned)(!replaced && (path == 1 || path == 3)));
        assert(binds == (unsigned)(ready && path < 2 ? 2 : 0));
        if (last_indices == arena) assert(!memcmp(arena, client, sizeof(client)));
        if (path == 2) assert(buffer.last_frame == vgl_framecount);
        ++cases;
    }
    printf("actual atlas callers: %u cases; original GPU U16/canonical, replacement, FFP, illegal no-draw PASS\n", cases);
    return 0;
}
'''
    fixture = source / "atlas-callers-host.c"
    fixture.write_text(harness, encoding="utf-8")
    # Keep all four native macro combinations, even LIGHT_NEAREST=1/HALO=0
    # (recipe-rejected), to prove ATLAS transport never depends on HALO code.
    for halo in (False, True):
        for nearest in (False, True):
            for canonical in (False, True):
                for skip_error in (False, True):
                    name = f"h{int(halo)}-n{int(nearest)}-c{int(canonical)}-e{int(skip_error)}"
                    defines = ["-DHAVE_ISAAC_LASER_ATLAS_NEAREST=1"]
                    for enabled, define in (
                        (halo, "HAVE_ISAAC_LASER_LIGHT_HALO_CLIP"),
                        (nearest, "HAVE_ISAAC_LASER_LIGHT_NEAREST"),
                        (canonical, "HAVE_ISAAC_CANONICAL_QUAD_ZERO_COPY"),
                        (skip_error, "SKIP_ERROR_HANDLING"),
                    ):
                        if enabled:
                            defines.append(f"-D{define}=1")
                    executable = source / f"atlas-callers-{name}.exe"
                    # Untouched stock code casts guest GL offsets to 32 bits and
                    # aliases the generic U16/U32 source; do not relabel those
                    # existing 64-bit-host warnings as a new candidate failure.
                    run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                         "-Wno-pointer-to-int-cast", "-Wno-incompatible-pointer-types",
                         "-Wno-unused-variable", *defines, "-I", str(recipe),
                         str(fixture), "-o", str(executable)])
                    print(name + ": " + run([str(executable)]).decode("utf-8").strip())


def check_single_final_bind(before: str, after: str, source: Path, cc: str | None,
                            *, paired: str | None = None) -> None:
    """Execute the actual patched macros/boundaries, not a second policy model.
    All remaining DrawElements code (including uniforms) must be byte-identical.
    The host substitutes GXM calls only; it does not estimate their native cost.
    """
    def macro(text: str, name: str) -> str:
        lines = text[text.index(f"#define {name}() "):].splitlines(True)
        result = ""
        for line in lines:
            result += line
            if not line.rstrip().endswith("\\"):
                return result
        raise AssertionError(f"unterminated macro {name}")

    def draw(text: str) -> str:
        start = text.index("GLboolean _glDrawElements_CustomShadersIMPL(")
        end = text.index("\n\treturn GL_TRUE;\n}", start) + len("\n\treturn GL_TRUE;\n}")
        return text[start:end]

    old_setup = macro(before, "setup_frag_program")
    prepare = macro(after, "isaac_prepare_frag_program")
    new_setup = macro(after, "setup_frag_program")
    old_bind = "\t\tif (isaac_opaque_fragment)\n\t\t\tsceGxmSetFragmentProgram(gxm_context, isaac_opaque_fragment);"
    new_bind = ("\t\t/* Rejected/unavailable variants must explicitly restore the stock program. */\n"
                "\t\tsceGxmSetFragmentProgram(gxm_context,\n"
                "\t\t\tisaac_opaque_fragment ? isaac_opaque_fragment : p->fprog);")
    old_early = "\t// Check if a blend info rebuild is required and upload fragment program\n\tsetup_frag_program();"
    new_early = "\t// Prepare stock blend/float state now; bind the final winner before uniforms.\n\tisaac_prepare_frag_program();"
    assert draw(before).count(old_early) == draw(before).count(old_bind) == 1
    assert draw(before).replace(old_early, new_early).replace(old_bind, new_bind) == draw(after)
    assert prepare.replace("isaac_prepare_frag_program", "setup_frag_program").rstrip() == (
        old_setup.rsplit("\n\tsceGxmSetFragmentProgram", 1)[0].rstrip().removesuffix("\\").rstrip())
    # Other draw paths still expand to the same prepare-then-bind operation.
    assert new_setup == ("#define setup_frag_program() \\\n"
                         "\tisaac_prepare_frag_program(); \\\n"
                         "\tsceGxmSetFragmentProgram(gxm_context, p->fprog);\n")
    if paired is not None:
        base_draw, pair_draw = draw(after), draw(paired)
        pair_decls = pair_draw.split("\n", 1)[1].split("#ifdef HAVE_PROFILING", 1)[0]
        selector_begin = "\t\t\t\tif (isaac_neutral_staging == ISAAC_COLOROFFSET_STAGING_PLAIN)"
        selector_end = "\t\t\t\tif (isaac_probe_fragment) {"
        old_selector = base_draw[base_draw.index(selector_begin):base_draw.index(selector_end)]
        new_selector = pair_draw[pair_draw.index(selector_begin):pair_draw.index(selector_end)]
        vertex_begin = "\t// Uploading new vertex program"
        uniform_begin = "\t// Uploading both fragment and vertex uniforms data"
        old_vertex = base_draw[base_draw.index(vertex_begin):base_draw.index(uniform_begin)]
        pair_vertex = pair_draw[pair_draw.index(vertex_begin):pair_draw.index(uniform_begin)]
        # No changes outside the declaration/selection/VS-bind+restore boundary.
        assert (base_draw.replace("\n", "\n" + pair_decls, 1)
                .replace(old_selector, new_selector).replace(old_vertex, pair_vertex)) == pair_draw
        assert macro(after, "upload_uniforms") == macro(paired, "upload_uniforms")
        assert macro(after, "isaac_prepare_frag_program") == macro(paired, "isaac_prepare_frag_program")
        assert pair_draw.count(new_bind) == 1
        pair_selection_start = pair_draw.index("#ifdef HAVE_ISAAC_COLOROFFSET_FS_PROBE",
                                               pair_draw.index("isaac_opaque_fragment = isaac_coloroffset_select_opaque_fragment"))
        pair_selection_end = pair_draw.index("\n\t}\n#endif\n\n" + vertex_begin, pair_selection_start)
        pair_selection = pair_draw[pair_selection_start:pair_selection_end]
        pair_upload = pair_draw[pair_draw.index(uniform_begin):pair_draw.index("\t// Uploading vertex streams")]
        assert pair_upload.count("upload_uniforms();") == 1
    if cc is None:
        return
    guard_start = after.index("#if !defined(HAVE_ISAAC_COLOROFFSET_SINGLE_FINAL_BIND)")
    guard = after[guard_start:after.index("#endif", guard_start) + len("#endif")]
    defines = ["-DHAVE_ISAAC_COLOROFFSET_SINGLE_FINAL_BIND=1",
               "-DHAVE_ISAAC_COLOROFFSET_STAGING_PROOF=1",
               "-DHAVE_ISAAC_COLOROFFSET_FS_PROBE=3"]
    for flags, success in ((defines, True), (defines[1:], False),
                           (defines[:1] + defines[2:], False),
                           (defines[:2] + ["-DHAVE_ISAAC_COLOROFFSET_FS_PROBE=2"], False)):
        result = subprocess.run([cc, "-E", "-x", "c", "-", *flags],
                                input=guard, text=True, capture_output=True)
        assert (result.returncode == 0) == success, result.stderr
    if paired is not None:
        pair_guard_start = paired.index("#if !defined(HAVE_ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR)")
        pair_guard = paired[pair_guard_start:paired.index("#endif", pair_guard_start) + len("#endif")]
        for flags, success in ((["-DHAVE_ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR=1"], True),
                               ([], False), (["-DHAVE_ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR=0"], False)):
            result = subprocess.run([cc, "-E", "-x", "c", "-", *flags],
                                    input=pair_guard, text=True, capture_output=True)
            assert (result.returncode == 0) == success, result.stderr
    harness = r'''
#include <assert.h>
#include <stdio.h>
typedef int SceGxmProgram;
typedef int SceGxmFragmentProgram;
typedef int SceGxmVertexProgram;
enum { SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 };
typedef struct { int raw; } Blend;
typedef struct { int id; SceGxmProgram *prog; } Shader;
typedef struct { Blend blend_info; int is_fbo_float; Shader *fshader, *vshader;
    SceGxmFragmentProgram *fprog; SceGxmVertexProgram *vprog; int attr_num; } program;
static Blend blend_info;
static int is_fbo_float, gxm_context, binds, rebuilds, invalidations;
static SceGxmFragmentProgram stock[2], fast[2], *bound;
#define ISAAC_GXM_SHADER_STATE_INVALIDATE() (++invalidations)
static void rebuild_frag_shader(int id, SceGxmFragmentProgram **dst,
        SceGxmProgram *vertex, int format) {
    assert(id == 7 && vertex); ++rebuilds; *dst = &stock[format];
}
static void sceGxmSetFragmentProgram(int context, SceGxmFragmentProgram *value) {
    (void)context; assert(value); ++binds; bound = value;
}
'''
    harness += old_setup.replace("setup_frag_program", "old_setup_frag_program") + prepare + new_setup
    for version, early, final in (("old", "old_setup_frag_program();", old_bind),
                                  ("new", "isaac_prepare_frag_program();", new_bind),
                                  ("other", "setup_frag_program();", "")):
        harness += (f"\nstatic void draw_{version}(program *p, SceGxmFragmentProgram *isaac_opaque_fragment) {{\n"
                    f"(void)isaac_opaque_fragment; {early}\n"
                    "assert(p->blend_info.raw == blend_info.raw && p->is_fbo_float == is_fbo_float);\n"
                    f"{final}\n}}\n")
    if paired is not None:
        # Driver calls and selector outcomes are mocks; the selection, binding,
        # restoration and upload order below comes from actual patched C.
        harness += r'''
typedef int GLboolean;
enum { GL_FALSE, GL_TRUE };
/* Values copied from isaac_coloroffset_staging.h. */
enum { ISAAC_COLOROFFSET_STAGING_UNKNOWN, ISAAC_COLOROFFSET_STAGING_REJECTED,
    ISAAC_COLOROFFSET_STAGING_NEUTRAL, ISAAC_COLOROFFSET_STAGING_PLAIN };
#define HAVE_ISAAC_GPU_DRAW_OPTIMIZATIONS 1
#define HAVE_ISAAC_COLOROFFSET_FS_PROBE 3
#define ISAAC_COLOROFFSET_FS_PROBE_COUNT(name) ((void)0)
static int pair_available, p2_available, neutral_available;
static int pair_queries, p2_queries, neutral_queries, vertex_binds, vertex_patches;
static int dirty_shader_vert_unifs, restores, uploads, upload_calls;
static SceGxmVertexProgram private_vertex, stock_vertex, *bound_vertex;
static SceGxmFragmentProgram private_fragment, opaque_fragment;
static SceGxmFragmentProgram *isaac_plain_vertex_pair_select(program *p, SceGxmVertexProgram **out) {
    (void)p; ++pair_queries; *out = NULL;
    if (!pair_available) return NULL;
    *out = &private_vertex; return &private_fragment;
}
static SceGxmFragmentProgram *isaac_coloroffset_plain_select(program *p) {
    (void)p; ++p2_queries; return p2_available ? &fast[0] : NULL;
}
static SceGxmFragmentProgram *isaac_coloroffset_fs_probe_select(program *p, int opaque) {
    (void)p; (void)opaque; ++neutral_queries; return neutral_available ? &fast[1] : NULL;
}
static void isaac_patch_vertex_program_cached(program *p, int id, void *attrs,
        int attr_count, void *streams, int stream_count) {
    (void)p; (void)id; (void)attrs; (void)attr_count; (void)streams; (void)stream_count;
    ++vertex_patches;
}
static void sceGxmSetVertexProgram(int context, SceGxmVertexProgram *v) {
    (void)context; assert(binds == 1); ++vertex_binds; bound_vertex = v;
}
static void vglRestoreVertexUniformBuffer(void) {
    assert(vertex_binds == 1 && !dirty_shader_vert_unifs); ++restores;
}
static void upload_uniforms(void) {
    assert(vertex_binds == 1 && binds == 1); ++upload_calls;
    uploads += dirty_shader_vert_unifs; dirty_shader_vert_unifs = 0;
}
static void draw_pair(program *p, int isaac_staged, int isaac_single_stream,
        int isaac_neutral_staging, SceGxmFragmentProgram *isaac_opaque_fragment) {
    void *draw_attributes = NULL, *draw_streams = NULL;
    int draw_stream_count = 1;
'''
        harness += pair_decls + pair_selection + "\n" + pair_vertex + pair_upload + "}\n"
        harness += r'''
static void run_pair_cases(void) {
    Shader fs = {7, &stock[0]}, vs = {8, &stock[0]};
    program p = {{0}, 0, &fs, &vs, &stock[0], &stock_vertex, 3};
    int cases = 0, was_pair = 0;
    for (int proof = 0; proof < 4; ++proof)
    for (int staged = 0; staged < 2; ++staged)
    for (int single = 0; single < 2; ++single)
    for (pair_available = 0; pair_available < 2; ++pair_available)
    for (p2_available = 0; p2_available < 2; ++p2_available)
    for (neutral_available = 0; neutral_available < 2; ++neutral_available)
    for (int opaque = 0; opaque < 2; ++opaque)
    for (int dirty = 0; dirty < 2; ++dirty) {
        int eligible = staged && single && proof >= ISAAC_COLOROFFSET_STAGING_NEUTRAL;
        int asks_pair = eligible && proof == ISAAC_COLOROFFSET_STAGING_PLAIN;
        int uses_pair = asks_pair && pair_available;
        int asks_p2 = asks_pair && !uses_pair;
        int uses_p2 = asks_p2 && p2_available;
        int asks_neutral = eligible && !uses_pair && !uses_p2;
        SceGxmFragmentProgram *expected = uses_pair ? &private_fragment : uses_p2 ? &fast[0] :
            (asks_neutral && neutral_available) ? &fast[1] : opaque ? &opaque_fragment : p.fprog;
        binds = pair_queries = p2_queries = neutral_queries = 0;
        vertex_binds = vertex_patches = restores = uploads = upload_calls = 0;
        dirty_shader_vert_unifs = dirty;
        draw_pair(&p, staged, single, proof, opaque ? &opaque_fragment : NULL);
        assert(pair_queries == asks_pair && p2_queries == asks_p2 && neutral_queries == asks_neutral);
        assert(bound == expected && binds == 1);
        assert(bound_vertex == (uses_pair ? &private_vertex : &stock_vertex));
        assert(vertex_binds == 1 && vertex_patches == !uses_pair);
        assert(restores == ((uses_pair || was_pair) && !dirty));
        assert(upload_calls == 1 && uploads == dirty && !dirty_shader_vert_unifs);
        assert(p.fprog == &stock[0] && p.vprog == &stock_vertex);
        was_pair = uses_pair; ++cases;
    }
    /* Finish on a proven private draw, then force a non-dirty stock exit. */
    pair_available = 1; dirty_shader_vert_unifs = 0;
    binds = vertex_binds = 0;
    draw_pair(&p, 1, 1, ISAAC_COLOROFFSET_STAGING_PLAIN, NULL);
    binds = vertex_binds = restores = 0;
    draw_pair(&p, 0, 0, ISAAC_COLOROFFSET_STAGING_UNKNOWN, NULL);
    assert(restores == 1 && bound_vertex == &stock_vertex && bound == p.fprog);
    printf("actual paired VS selection/uniform boundary: PASS; %d cases plus private-to-stock exit\n", cases);
}
'''
    harness += r'''
int main(void) {
    Shader fs = {7, &stock[0]}, vs = {8, &stock[0]};
    int cases = 0;
    for (int blend = 0; blend < 2; ++blend)
    for (int floating = 0; floating < 2; ++floating)
    for (int winner = 0; winner < 3; ++winner)
    for (int previous = 0; previous < 2; ++previous)
    for (int path = 0; path < 3; ++path) {
        program p = {{0}, 0, &fs, &vs, &stock[0], NULL, 0};
        SceGxmFragmentProgram *selected = winner ? &fast[winner - 1] : NULL;
        blend_info.raw = blend; is_fbo_float = floating;
        bound = previous ? &fast[0] : &stock[0]; binds = rebuilds = invalidations = 0;
        if (!path) draw_old(&p, selected);
        else if (path == 1) draw_new(&p, selected);
        else draw_other(&p, selected);
        assert(rebuilds == (blend || floating) && invalidations == rebuilds);
        assert(p.fprog == &stock[floating]);
        assert(bound == ((path != 2 && selected) ? selected : p.fprog));
        assert(binds == ((!path && selected) ? 2 : 1));
        ++cases;
    }
    printf("actual fragment prepare/final-bind boundaries: PASS; %d cases\n", cases);
    return 0;
}
'''
    if paired is not None:
        harness = harness.replace("    return 0;\n}\n", "    run_pair_cases();\n    return 0;\n}\n")
    fixture = source / "single-final-bind-host.c"
    fixture.write_text(harness, encoding="utf-8")
    executable = source / "single-final-bind-host.exe"
    run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", str(fixture), "-o", str(executable)])
    print(run([str(executable)]).decode("utf-8").strip() + "; opt-in/neutral guards PASS")


def prove_combination(
    recipe: Path,
    temporary: Path,
    name: str,
    stock_tar: Path | None,
    repository_archive: bytes | None,
) -> None:
    gpu_draw, coloroffset, fs_probe = (
        part == "1" for part in name.split(":"))
    source = materialize_stock(
        temporary / ("combo-" + name.replace(":", "-")),
        stock_tar,
        repository_archive,
    )
    apply_patch(source, recipe / "0001-deterministic-build-and-init-oob.patch")
    if gpu_draw:
        apply_patch(source, recipe / "0002-exact-gpu-draw-optimizations.patch")
        for policy in ("isaac_gpu_draw_policy.h", "isaac_gxm_state_policy.h"):
            shutil.copyfile(recipe / policy, source / "source" / policy)
    if coloroffset:
        apply_patch(source, recipe / "0003-exact-coloroffset-gpu-optimizations.patch")
        policy = "isaac_coloroffset_gpu_policy.h"
        shutil.copyfile(recipe / policy, source / "source" / policy)
    if fs_probe:
        # build.sh order: the diagnostic probe sits between 0003 and 0004.
        apply_patch(source, recipe / FS_PROBE_PATCH)
    apply_patch(source, recipe / "0004-hardened-custom-shader-cache.patch")
    check_fs_probe(source, fs_probe)
    for policy in (
        "isaac_shader_cache_policy.h",
        "isaac_shader_cache_vitagl.h",
        "isaac_shader_cache_block_list.h",
    ):
        shutil.copyfile(recipe / policy, source / "source" / policy)

    targets = (
        source / "source" / "custom_shaders.c",
        source / "source" / "vitaGL.h",
    )
    if gpu_draw:
        # The GL time-profile scene timer/split, the FBO render-target
        # scenes patch and the FBO valid-region patch sit on top of the 0002
        # gxm.c (0009 also misc.c/tests.c) and must leave the two hashed
        # files byte-identical.
        before = tuple(sha256(target) for target in targets)
        for patch_name in ("scene_timer", "scene_split", "fbo_rt_scenes",
                           "fbo_valid_region"):
            apply_patch(source, recipe / GXM_PATCHES[patch_name])
        check_gxm(source, True, True, True)
        if tuple(sha256(target) for target in targets) != before:
            raise AssertionError("a gxm.c patch touched a hashed file")
    # The independent P8 prerequisite edits textures.c only.  Shader recipes
    # and ABI headers must remain unchanged with this optional patch too.
    apply_patch(source, recipe / "0010-isaac-p8-safe-upload.patch")
    texture_source = (source / "source/textures.c").read_text(encoding="utf-8")
    assert texture_source.count("isaac_p8_image_level0(tex,") == 1
    assert texture_source.count("isaac_p8_promote_rgba_subimage(tex,") == 1
    apply_patch(source, recipe / "0011-isaac-exact-laser-p8.patch")
    check_laser_lifecycle(source, texture_source)
    actual = tuple(sha256(target) for target in targets)
    if actual != EXPECTED_HASHES[name]:
        raise AssertionError(
            f"patch chain {name} hashes {actual}, expected {EXPECTED_HASHES[name]}"
        )
    for target in targets:
        if b"\r\n" in target.read_bytes():
            raise AssertionError(f"ambient CRLF conversion reached {name}: {target}")
    print(f"vitaGL patch chain {name}: {actual[0]} / {actual[1]}")


def main() -> int:
    parser = argparse.ArgumentParser()
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--stock-tar", type=Path)
    source.add_argument("--stock-repo", type=Path)
    parser.add_argument("--cc", help="optionally run P8, fragment-bind and FBO-region host checks")
    args = parser.parse_args()

    vita = Path(__file__).resolve().parent
    recipe = vita / "vitagl-stock-reference"
    verify_build_contract(recipe)
    check_laser_reference(recipe)
    repository_archive = None
    stock_tar = args.stock_tar
    if stock_tar is not None:
        if not stock_tar.is_file():
            raise AssertionError(f"stock tar is not a file: {stock_tar}")
        actual_archive_hash = sha256(stock_tar)
        if actual_archive_hash != SOURCE_ARCHIVE_SHA256:
            raise AssertionError(
                f"stock tar SHA-256 {actual_archive_hash}, "
                f"expected {SOURCE_ARCHIVE_SHA256}"
            )
    else:
        repository_archive = stock_archive_from_repo(args.stock_repo)

    with tempfile.TemporaryDirectory(prefix="isaac-vitagl-patch-chain-") as value:
        temporary = Path(value)
        for name in EXPECTED_HASHES:
            prove_combination(
                recipe, temporary, name, stock_tar, repository_archive
            )
        prove_gxm_matrix(recipe, temporary, stock_tar, repository_archive, args.cc)
        prove_fbo_float_sync(recipe, temporary, stock_tar, args.cc, repository_archive)
        prove_sdk_sparse(recipe, temporary, stock_tar, args.cc, repository_archive)
        prove_metadata_once(recipe, temporary, stock_tar, args.cc, repository_archive)
        if stock_tar is not None:
            prove_halo_profile(recipe, temporary, stock_tar, args.cc)
        prove_fbo_scissor_resize(recipe, temporary, stock_tar, args.cc, repository_archive)
        prove_fbo_scissor_replay(recipe, temporary, stock_tar, args.cc, repository_archive)
        prove_fs_probe_matrix(recipe, temporary, stock_tar, repository_archive)
        prove_staging_matrix(recipe, temporary, stock_tar, repository_archive, args.cc)
        if args.cc:
            executable = temporary / "p8-safety-host.exe"
            run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 str(vita / "host_tests/vitagl_p8_safety_host_test.c"),
                 "-o", str(executable)])
            print(run([str(executable)]).decode("utf-8").strip())
            for swizzle in (False, True):
                executable = temporary / f"p8-nearest-swizzle{int(swizzle)}-host.exe"
                options = ["-DHAVE_ISAAC_LASER_LIGHT_NEAREST=1"]
                if swizzle:
                    options.append("-DHAVE_ISAAC_LASER_P8_SWIZZLE=1")
                run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", *options,
                     str(vita / "host_tests/vitagl_p8_safety_host_test.c"),
                     "-o", str(executable)])
                print(run([str(executable)]).decode("utf-8").strip())
            for nearest in (False, True):
                executable = temporary / f"p8-halo-depth-nearest{int(nearest)}-host.exe"
                options = ["-DHAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX=1"]
                if nearest:
                    options += ["-DHAVE_ISAAC_LASER_LIGHT_NEAREST=1",
                                "-DHAVE_ISAAC_LASER_P8_SWIZZLE=1"]
                run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", *options,
                     str(vita / "host_tests/vitagl_p8_safety_host_test.c"),
                     "-o", str(executable)])
                print(run([str(executable)]).decode("utf-8").strip())
            for cap, nearest, depth, producer in (
                (0, True, True, None), (1, False, False, None),
                (1, True, False, None), (1, True, True, None),
                (0, True, True, 0), (1, True, True, 0),
                (0, False, False, 1), (1, False, False, 1),
                (0, True, True, 1), (1, True, True, 1),
            ):
                executable = temporary / f"p8-cap{cap}-nearest{int(nearest)}-depth{int(depth)}-producer{producer}-host.exe"
                options = [f"-DHAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP={cap}"]
                if producer is not None:
                    options.append(f"-DHAVE_ISAAC_LASER_LIGHT_PRODUCER_UV={producer}")
                if nearest:
                    options += ["-DHAVE_ISAAC_LASER_LIGHT_NEAREST=1",
                                "-DHAVE_ISAAC_LASER_P8_SWIZZLE=1"]
                if depth:
                    options.append("-DHAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX=1")
                run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", *options,
                     str(vita / "host_tests/vitagl_p8_safety_host_test.c"),
                     "-o", str(executable)])
                print(run([str(executable)]).decode("utf-8").strip())
            for profile, producer, cap, proven in (
                (0, 1, 1, None), (1, 1, 1, None), (1, 0, 1, None), (1, 1, 0, None),
                (0, 1, 0, 0), (1, 1, 1, 0),
                (0, 1, 0, 1), (0, 1, 1, 1), (1, 1, 0, 1), (1, 1, 1, 1),
            ):
                options = [f"-DHAVE_ISAAC_LASER_HALO_PROFILE={profile}",
                           f"-DHAVE_ISAAC_LASER_LIGHT_PRODUCER_UV={producer}",
                           f"-DHAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP={cap}",
                           "-DHAVE_ISAAC_LASER_LIGHT_NEAREST=1",
                           "-DHAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX=1"]
                if proven is not None:
                    options.append(f"-DHAVE_ISAAC_LASER_HALO_PROVEN_EMIT={proven}")
                for fast in ((False, True) if proven == 1 else (False,)):
                    executable = temporary / f"p8-profile{profile}-producer{producer}-cap{cap}-proven{proven}-fast{int(fast)}.exe"
                    run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", *options,
                         *(["-ffast-math"] if fast else []),
                         str(vita / "host_tests/vitagl_p8_safety_host_test.c"),
                         "-o", str(executable)])
                    print(run([str(executable)]).decode("utf-8").strip())
            for variant, extra in (
                ("atlas", []),
                ("atlas-swizzle", ["-DHAVE_ISAAC_LASER_P8_SWIZZLE=1"]),
                ("atlas-swizzle-old-nearest-depth", [
                    "-DHAVE_ISAAC_LASER_P8_SWIZZLE=1",
                    "-DHAVE_ISAAC_LASER_LIGHT_NEAREST=1",
                    "-DHAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX=1",
                ]),
            ):
                executable = temporary / f"p8-{variant}-host.exe"
                run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                     "-DHAVE_ISAAC_LASER_ATLAS_NEAREST=1", *extra,
                     str(vita / "host_tests/vitagl_p8_safety_host_test.c"),
                     "-o", str(executable)])
                print(run([str(executable)]).decode("utf-8").strip())
            # The existing fixture mocks the converter; this checks the
            # transactional caller/fallback, not NEON Morton conversion.
            executable = temporary / "p8-safety-swizzle-host.exe"
            run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-DHAVE_ISAAC_LASER_P8_SWIZZLE=1",
                 str(vita / "host_tests/vitagl_p8_safety_host_test.c"),
                 "-o", str(executable)])
            print(run([str(executable)]).decode("utf-8").strip())
    print("Isaac exact stock vitaGL patch-chain regression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
