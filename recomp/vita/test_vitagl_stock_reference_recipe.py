#!/usr/bin/env python3
"""Static fail-closed contract for the stock vitaGL reference recipe."""

from __future__ import annotations

import hashlib
import re
import shlex
from collections.abc import Callable
from pathlib import Path

SOURCE_COMMIT = "73dd57a8857f89f2353881c6de5891959c5c1983"
SOURCE_SHA256 = "f484dd9d2aec707ac5f91352c6e0631239f2330fe6769e8476cfe425331c400a"
OOB_FIX_COMMIT = "f24ad3e66f7f34bebe70598d1302c34f4eff4a54"
PATCH_SHA256 = "8b932a61c19ecdde25e1281a99f2107d6179441ac9222308b984d70bd70fda03"
GPU_DRAW_PATCH_SHA256 = "a640d5f89487aa09ace46b5260c94126a6588c8ba0f1e99b4b1120bfeea680f9"
GPU_DRAW_POLICY_SHA256 = "9d4721994dc9c786e6cd78b64276f4fd5b18b0b03c7aed1f3c5732e81102731f"
GXM_STATE_POLICY_SHA256 = "21aeacfbcb9b0a2999e40645ed4c6c664dfb401c2c447fabed364688e02b80f5"
COLOROFFSET_PATCH_SHA256 = "e623e862fea045c6423113ac960e43d1144123eb9e93835a77a2067a8db48873"
COLOROFFSET_POLICY_SHA256 = "dcc5e7d29f803cfc86b8cdc029281607806edb09da97c4f7d5101e0de73562e2"
# 0008: diagnostic ColorOffset fragment-shader probe (custom_shaders.c only).
COLOROFFSET_FS_PROBE_PATCH_SHA256 = "d1bede6396c170f6054f783c2f397d24616981d2c673fe97ab7830c108aaf75b"
FLAGS = "SOFTFP_ABI=1 NO_DEBUG=1 NO_SPLASHSCREEN=1 SINGLE_THREADED_GC=1"
EXPECTED_MAKE_COMMAND = (
    "env",
    "-i",
    "PATH=$PATH",
    "VITASDK=$VITASDK",
    "SOURCE_DATE_EPOCH=$source_epoch",
    "make",
    "--directory=$source_dir",
    "--jobs=$jobs",
    "VGL_GIT_HASH=$source_short",
    "EXTRA_CFLAGS=$extra_cflags",
    "SOFTFP_ABI=1",
    "NO_DEBUG=1",
    "NO_SPLASHSCREEN=1",
    "SINGLE_THREADED_GC=1",
    "libvitaGL.a",
)


def require(text: str, needle: str, owner: str) -> None:
    if needle not in text:
        raise AssertionError(f"{owner} lost required text: {needle}")


def verify_exact_make_command(build: str) -> None:
    invocations = re.findall(
        r"(?m)^[ \t]*env[ \t]+-i(?:[^\r\n]*\\\r?\n)*[^\r\n]*$",
        build,
    )
    if len(invocations) != 1:
        raise AssertionError(
            "build.sh must contain exactly one continued env -i invocation: "
            f"found {len(invocations)}"
        )
    logical_command = re.sub(r"\\\r?\n", " ", invocations[0])
    try:
        actual = tuple(shlex.split(logical_command, posix=True))
    except ValueError as exc:
        raise AssertionError(
            f"stock make invocation is not valid shell syntax: {exc}"
        ) from exc
    if actual != EXPECTED_MAKE_COMMAND:
        raise AssertionError(
            "stock env/make argv changed:\n"
            f"actual={actual!r}\nexpected={EXPECTED_MAKE_COMMAND!r}"
        )


def verify_contract(
    build: str,
    patch_bytes: bytes,
    cmake: str,
    backend: str,
) -> None:
    actual_patch_sha = hashlib.sha256(patch_bytes).hexdigest()
    if actual_patch_sha != PATCH_SHA256:
        raise AssertionError(
            "stock upstream patch SHA-256 changed: "
            f"{actual_patch_sha} != {PATCH_SHA256}"
        )
    patch = patch_bytes.decode("utf-8")

    for name, expected in (
        ("source_commit", SOURCE_COMMIT),
        ("source_sha", SOURCE_SHA256),
        ("oob_fix_commit", OOB_FIX_COMMIT),
        ("build_flags", FLAGS),
    ):
        match = re.search(rf"(?m)^{name}=([^\n]+)$", build)
        actual = None if not match else match.group(1).strip('"')
        if actual != expected:
            raise AssertionError(
                f"build.sh {name} changed: "
                f"{actual!r} != {expected!r}"
            )
    require(build, "source_epoch=1786822413", "build.sh")
    require(build, 'script_sha=$(sha256sum "$script_dir/build.sh"', "build.sh")
    require(build, '"$patch_sha" "$script_sha"', "recipe cache key")
    require(build, "printf 'script_sha256=%s\\n'", "build contract")
    verify_exact_make_command(build)
    require(build, "gxm_source_patch=%s\\n' \"$gxm_source_patch_name\"", "build contract")

    changed = re.findall(r"(?m)^diff --git a/(\S+) b/(\S+)$", patch)
    if changed != [("Makefile", "Makefile"), ("source/vgl.c", "source/vgl.c")]:
        raise AssertionError(f"stock upstream patch surface changed: {changed}")
    for needle in (
        "+VGL_GIT_HASH ?= $(shell git rev-parse --short HEAD 2>/dev/null)",
        "+CFLAGS += $(EXTRA_CFLAGS)",
        "+\t$(AR) -rcD $@ $^",
        "-\tblit_streams[0].indexSource = blit_streams[1].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;",
        "+\tblit_streams[0].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;",
    ):
        require(patch, needle, "upstream patch")
    if "source/gxm.c" in patch or patch.count("diff --git ") != 2:
        raise AssertionError("stock patch gained a GXM/runtime overlay")

    if not re.search(
        r"option\(ISAAC_VITA_VITAGL_STOCK_REFERENCE\s+"
        r'"[^"]+" OFF\)',
        cmake,
    ):
        raise AssertionError("stock CMake option is not default OFF")
    for needle in (
        "vitagl-stock-reference/build.sh",
        "stock-reference-ready.stamp",
        "ISAAC_VITA_VITAGL_STOCK_REFERENCE=1",
        "Building pinned stock vitaGL 73dd57a with",
    ):
        require(cmake, needle, "production CMake")
    for needle in (
        "(void)vglInitExtended(",
        "0, KAGE_VITA_DISPLAY_WIDTH, KAGE_VITA_DISPLAY_HEIGHT,",
        "KAGE_VITA_RAM_THRESHOLD, SCE_GXM_MULTISAMPLE_NONE);",
        "profile=stock-vitagl-reference ",
        "source=73dd57a physical=960x544 logical=960x540 ",
        "rt-scenes=%u GL=%s",
        '"rt-scenes=%u" KAGE_VITA_STOCK_FBO_RT_SCENES_BANNER',
        'KAGE_VITA_STOCK_FBO_VALID_REGION_BANNER " GL=%s\\n"',
        '# define KAGE_VITA_STOCK_FBO_RT_SCENES_BANNER " fbo-rt-scenes=%u"',
        '# define KAGE_VITA_STOCK_FBO_VALID_REGION_BANNER " fbo-valid-region=%s"',
        'uint8_t vglIsaacSetupFboValidRegion(uint8_t apply);',
    ):
        require(backend, needle, "stock runtime branch")


def verify_gpu_draw_contract(
    build: str,
    patch_bytes: bytes,
    policy_bytes: bytes,
    state_policy_bytes: bytes,
    cmake: str,
) -> None:
    actual_patch_sha = hashlib.sha256(patch_bytes).hexdigest()
    if actual_patch_sha != GPU_DRAW_PATCH_SHA256:
        raise AssertionError(
            "GPU draw patch SHA-256 changed: "
            f"{actual_patch_sha} != {GPU_DRAW_PATCH_SHA256}"
        )
    actual_policy_sha = hashlib.sha256(policy_bytes).hexdigest()
    if actual_policy_sha != GPU_DRAW_POLICY_SHA256:
        raise AssertionError(
            "GPU draw policy SHA-256 changed: "
            f"{actual_policy_sha} != {GPU_DRAW_POLICY_SHA256}"
        )
    actual_state_policy_sha = hashlib.sha256(state_policy_bytes).hexdigest()
    if actual_state_policy_sha != GXM_STATE_POLICY_SHA256:
        raise AssertionError(
            "GXM state policy SHA-256 changed: "
            f"{actual_state_policy_sha} != {GXM_STATE_POLICY_SHA256}"
        )

    patch = patch_bytes.decode("utf-8")
    policy = policy_bytes.decode("utf-8")
    state_policy = state_policy_bytes.decode("utf-8")
    changed = re.findall(r"(?m)^diff --git a/(\S+) b/(\S+)$", patch)
    expected_changed = [
        ("source/custom_shaders.c", "source/custom_shaders.c"),
        ("source/draw.c", "source/draw.c"),
        ("source/ffp.c", "source/ffp.c"),
        ("source/framebuffers.c", "source/framebuffers.c"),
        ("source/gxm.c", "source/gxm.c"),
        (
            "source/isaac_gxm_state_shadow.c",
            "source/isaac_gxm_state_shadow.c",
        ),
        (
            "source/isaac_gxm_state_shadow.h",
            "source/isaac_gxm_state_shadow.h",
        ),
        ("source/misc.c", "source/misc.c"),
        ("source/shared.h", "source/shared.h"),
        ("source/vgl.c", "source/vgl.c"),
        ("source/vitaGL.h", "source/vitaGL.h"),
    ]
    if changed != expected_changed:
        raise AssertionError(f"GPU draw patch surface changed: {changed}")
    if patch.count("diff --git ") != len(expected_changed):
        raise AssertionError("GPU draw patch gained an untracked source hunk")

    for needle in (
        "gpu_draw_optimizations=${ISAAC_GPU_DRAW_OPTIMIZATIONS:-0}",
        "0|1) ;;",
        'if [ "$gpu_draw_optimizations" = 1 ]; then',
        'apply_source_patch "$gpu_draw_patch"',
        'cp -- "$gpu_draw_policy"',
        'cp -- "$gxm_state_policy"',
        "-DHAVE_ISAAC_GPU_DRAW_OPTIMIZATIONS=1",
        "-DHAVE_ISAAC_CANONICAL_QUAD_ZERO_COPY=1",
        "-DHAVE_ISAAC_GXM_STATE_SHADOW=1",
        "gpu_draw_optimizations=%s\\n",
        "canonical_quad_zero_copy=%s\\n",
        "gxm_state_shadow=%s\\n",
        "gpu_draw_patch_sha256=%s\\n",
        "gpu_draw_policy_sha256=%s\\n",
        "gxm_state_policy_sha256=%s\\n",
        "vglGetIsaacGpuDrawStats",
        "vglIsaacDrawCanonicalQuads",
        'nm_symbols=$("$VITASDK/bin/arm-vita-eabi-nm" -g "$built_library")',
        "for endpoint in isaacGxmSetVertexProgram isaacGxmSetFragmentProgram",
        "GXM state-shadow endpoint escaped its opt-in build",
    ):
        require(build, needle, "stock GPU draw recipe")

    for needle in (
        "#ifdef HAVE_ISAAC_GPU_DRAW_OPTIMIZATIONS",
        "isaac_try_coalesce_single_stream",
        "isaac_patch_vertex_program_cached",
        "sceGxmShaderPatcherReleaseVertexProgram",
        "sceGxmFinish(gxm_context);",
        "sceGxmSetVertexProgram(gxm_context, p->vprog);",
        "p->vprog = NULL;",
        "void vglGetIsaacGpuDrawStats(vglIsaacGpuDrawStats *stats);",
        "GLboolean vglIsaacDrawCanonicalQuads(GLsizei count)",
        "_Static_assert(MAX_IDX_NUMBER == 0xC000",
        "top_idx = ((uint32_t)count / 6u) * 4u;",
        "count > MAX_IDX_NUMBER || count % 6",
        "_Static_assert(sizeof(SceGxmTexture) == ISAAC_GXM_STATE_TEXTURE_BYTES",
        "#define sceGxmSetVertexProgram isaacGxmSetVertexProgram",
        "#define sceGxmSetFragmentProgram isaacGxmSetFragmentProgram",
        "#define sceGxmSetFragmentTexture isaacGxmSetFragmentTexture",
        "isaacGxmStateBeginScene(gxm_context, r);",
        "isaacGxmStateEndScene(gxm_context);",
        "isaacGxmStateInvalidate(gxm_context);",
    ):
        require(patch, needle, "GPU draw patch")
    for index, vertex in enumerate((0, 2, 1, 1, 2, 3)):
        slot = "i * 6" if index == 0 else f"i * 6 + {index}"
        value = "i * 4" if vertex == 0 else f"i * 4 + {vertex}"
        require(
            patch,
            f"isaac_canonical_quads_idx_ptr[{slot}] = {value};",
            "canonical KAGE winding",
        )
    if "+#define DRAW_SPEEDHACK" in patch or "+#define INDICES_DRAW_SPEEDHACK" in patch:
        raise AssertionError("GPU draw patch enabled an unsafe draw speedhack")

    for needle in (
        "attributes[index].stream_index != index",
        "stream_bases[index] != stream_bases[0]",
        "left->stride == right->stride",
        "left->index_source == right->index_source",
        "left->offset == right->offset",
        "left->format == right->format",
        "left->component_count == right->component_count",
        "left->reg_index == right->reg_index",
        "left_attribute_count != right_attribute_count",
        "left_stream_count != right_stream_count",
    ):
        require(policy, needle, "GPU draw exact policy")
    if "memcmp" in policy:
        raise AssertionError("GPU draw layout key must not compare padding")

    for needle in (
        "ISAAC_GXM_STATE_CONTEXT_CAPACITY 4u",
        "ISAAC_GXM_STATE_TEXTURE_UNITS 16u",
        "ISAAC_GXM_STATE_TEXTURE_BYTES 16u",
        "state->active_scene",
        "state->context == context",
        "state->vertex_program == program",
        "state->fragment_program == program",
        "isaac_gxm_state_bytes_equal(state->texture[unit], bytes)",
        "isaac_gxm_state_invalidate_values",
        "isaac_gxm_state_forget(state)",
    ):
        require(state_policy, needle, "GXM exact state policy")
    if "memcmp" in state_policy:
        raise AssertionError("GXM texture state must compare all bytes explicitly")

    if not re.search(
        r"option\(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS\s+"
        r'"[^"]+" OFF\)',
        cmake,
    ):
        raise AssertionError("GPU draw CMake option is not default OFF")
    for needle in (
        "NOT ISAAC_VITA_VITAGL_STOCK_REFERENCE",
        "0002-exact-gpu-draw-optimizations.patch",
        "vitagl-stock-reference/isaac_gpu_draw_policy.h",
        "ISAAC_GPU_DRAW_OPTIMIZATIONS=${ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS_MODE}",
        "ISAAC_CANONICAL_QUAD_ZERO_COPY=${ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY_MODE}",
        "ISAAC_GXM_STATE_SHADOW=${ISAAC_VITA_GXM_STATE_SHADOW_MODE}",
        "ISAAC_PHASE_PROFILE=${ISAAC_VITA_PHASE_PROFILE_MODE}",
        "vitagl-stock-reference/isaac_gxm_state_policy.h",
    ):
        require(cmake, needle, "GPU draw production CMake")
    for option in (
        "ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY",
        "ISAAC_VITA_GXM_STATE_SHADOW",
    ):
        if not re.search(rf"option\({option}\s+\"[^\"]+\" OFF\)", cmake):
            raise AssertionError(f"{option} is not default OFF")


def verify_coloroffset_contract(
    build: str,
    patch_bytes: bytes,
    policy_bytes: bytes,
    cmake: str,
    source_matcher: str,
    source_oracle: str,
    policy_oracle: str,
    gl_backend: str,
    readme: str,
) -> None:
    actual_patch_sha = hashlib.sha256(patch_bytes).hexdigest()
    if actual_patch_sha != COLOROFFSET_PATCH_SHA256:
        raise AssertionError(
            "ColorOffset patch SHA-256 changed: "
            f"{actual_patch_sha} != {COLOROFFSET_PATCH_SHA256}"
        )
    actual_policy_sha = hashlib.sha256(policy_bytes).hexdigest()
    if actual_policy_sha != COLOROFFSET_POLICY_SHA256:
        raise AssertionError(
            "ColorOffset policy SHA-256 changed: "
            f"{actual_policy_sha} != {COLOROFFSET_POLICY_SHA256}"
        )

    patch = patch_bytes.decode("utf-8")
    policy = policy_bytes.decode("utf-8")
    changed = re.findall(r"(?m)^diff --git a/(\S+) b/(\S+)$", patch)
    expected_changed = [
        ("source/custom_shaders.c", "source/custom_shaders.c"),
        ("source/vitaGL.h", "source/vitaGL.h"),
    ]
    if changed != expected_changed or patch.count("diff --git ") != 2:
        raise AssertionError(
            f"ColorOffset patch surface changed: {changed}"
        )

    for needle in (
        "coloroffset_optimizations=${ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS:-0}",
        'if [ "$coloroffset_optimizations" = 1 ] &&',
        'apply_source_patch "$coloroffset_patch"',
        'cp -- "$coloroffset_policy"',
        "-DHAVE_ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS=1",
        "coloroffset_gpu_optimizations=%s\\n",
        "coloroffset_patch_sha256=%s\\n",
        "coloroffset_policy_sha256=%s\\n",
        "vglIsaacMarkColorOffsetShader",
        "vglGetIsaacColorOffsetStats",
    ):
        require(build, needle, "stock ColorOffset recipe")

    for needle in (
        "#ifdef HAVE_ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS",
        '#include "isaac_coloroffset_gpu_policy.h"',
        "isaac_coloroffset_compiled_generation",
        "isaac_coloroffset_link_source_generation",
        "isaac_coloroffset_record_link",
        "isaac_coloroffset_fragment_program_is_exact",
        "isaac_coloroffset_blend_is_exact",
        "Physical raw 0x5151110f uses ONE here",
        "blend_info.info.colorSrc == SCE_GXM_BLEND_FACTOR_ONE",
        "SCE_GXM_BLEND_FACTOR_SRC_ALPHA",
        "SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA",
        "SCE_GXM_TEXTURE_FORMAT_U8U8U8_BGR",
        "vglGetTexFormat(&tex->gxm_tex)",
        "tex->status != TEX_VALID",
        "isaac_coloroffset_contiguous_vertices_are_opaque",
        "isaac_coloroffset_indexed_vertices_are_opaque",
        "address >= (uintptr_t)&shaders[MAX_CUSTOM_SHADERS]",
        "isaac_coloroffset_advance_shader_generation",
        "isaac_coloroffset_link_vertex_generation",
        "isaac_coloroffset_fragment_shader_is_exact(p->fshader)",
        "isaac_coloroffset_original_index_type",
        "void vglShaderGxpBinary(",
        "sceGxmShaderPatcherCreateFragmentProgram",
        "SCE_GXM_BLEND_FUNC_NONE",
        "isaac_coloroffset_release_fragment_cache(p);",
        "sceGxmFinish(gxm_context);",
        "void vglIsaacMarkColorOffsetShader(",
        "void vglGetIsaacColorOffsetStats(",
    ):
        require(patch, needle, "ColorOffset vitaGL patch")
    if patch.index("sceGxmFinish(gxm_context);") > patch.index(
        "isaac_coloroffset_release_fragment_cache(p);"
    ):
        raise AssertionError("ColorOffset cache is released before GPU finish")
    for forbidden in (
        "source/gxm.c", "ISAAC_VITA_DISPLAY_RASTER_720",
        "isaac_coloroffset_fast_path_source", "gl_FragColor = Color0",
    ):
        if forbidden in patch:
            raise AssertionError(
                f"ColorOffset patch gained forbidden surface: {forbidden}"
            )

    for needle in (
        "ISAAC_COLOROFFSET_ATTRIBUTE_COUNT 8u",
        "ISAAC_COLOROFFSET_VERTEX_STRIDE 88u",
        "ISAAC_COLOROFFSET_TEXTURE_WIDTH 432u",
        "ISAAC_COLOROFFSET_TEXTURE_HEIGHT 240u",
        "ISAAC_COLOROFFSET_VIEWPORT_WIDTH 960u",
        "ISAAC_COLOROFFSET_VIEWPORT_HEIGHT 540u",
        "ISAAC_COLOROFFSET_VERTEX_GXP_SIZE 562u",
        "ISAAC_COLOROFFSET_VERTEX_GXP_FNV1A 0x4f7dce51u",
        "ISAAC_COLOROFFSET_FRAGMENT_GXP_SIZE 809u",
        "ISAAC_COLOROFFSET_FRAGMENT_GXP_FNV1A 0xb6517f58u",
        "isaac_coloroffset_vertex_is_default_opaque",
        "0x3f800000u",
        "pixelation <= 0.0f",
        "isaac_coloroffset_cache_key_equal",
        "left->source_generation == right->source_generation",
        "left->source_blend_raw == right->source_blend_raw",
        "left->output_format == right->output_format",
        "left->multisample_mode == right->multisample_mode",
    ):
        require(policy, needle, "ColorOffset exact policy")

    for needle in (
        "ISAAC_COLOROFFSET_SOURCE_SIZE 2052u",
        "ISAAC_COLOROFFSET_SOURCE_FNV1A 0x2b3ccf4au",
        "ISAAC_COLOROFFSET_SOURCE_FIRST512_FNV1A 0x75ef2322u",
        "source_size != signature->size",
        "signature->full_fnv1a",
        "signature->first512_fnv1a",
        "isaac_coloroffset_match_source",
        "explicit interior NUL",
        "This is selection only",
    ):
        require(source_matcher, needle, "ColorOffset source matcher")
    for needle in (
        "single-byte hostile mutation passed the full-source gate",
        "split source with explicit lengths/trailing empty part failed",
        "owned production asset failed exact split-source selection",
        "non-stock fixture passed production hashes",
    ):
        require(source_oracle, needle, "ColorOffset source oracle")
    for needle in (
        "short hostile vertex span passed",
        "base/index overflow passed",
        "RGB24/Color0 output-alpha proof failed",
        "physical ColorOffset blend receipt decoded incorrectly",
        "captured vertex GXP does not match the full binary receipt",
        "captured fragment GXP does not match the full binary receipt",
        "source/relink generation omitted from key",
        "viewport width omitted from key",
    ):
        require(policy_oracle, needle, "ColorOffset policy oracle")

    for needle in (
        "selected = isaac_coloroffset_match_source(",
        "Keep the original count, lengths,",
        "match.source_first512_fnv1a",
    ):
        require(gl_backend, needle, "byte-identical ColorOffset GL boundary")
    if "replacement" in gl_backend[
        gl_backend.index("static void vita_glShaderSource"):
        gl_backend.index("static void vita_glTexImage2D")
    ]:
        raise AssertionError("ColorOffset GL boundary still rewrites source")

    for needle in (
        "keeps the game's `resources/shaders/coloroffset.fs`",
        "The proposed GLSL early return was excluded",
        "static_eligible`, `static_hits`, and `static_pixels`",
        "opaque_viewport_pixels",
        "not a GPU fragment counter",
    ):
        require(readme, needle,
                "stock reference README (ColorOffset safety statements)")

    if not re.search(
        r"option\(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS\s+"
        r'"[^"]+" OFF\)',
        cmake,
    ):
        raise AssertionError("ColorOffset CMake option is not default OFF")
    for needle in (
        "NOT ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS",
        "0003-exact-coloroffset-gpu-optimizations.patch",
        "vitagl-stock-reference/isaac_coloroffset_gpu_policy.h",
        "ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS=${ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS_MODE}",
        "gl_vita_coloroffset_source.c",
    ):
        require(cmake, needle, "ColorOffset production CMake")
    for needle in (
        "phase_profile=${ISAAC_PHASE_PROFILE:-0}",
        "-DHAVE_ISAAC_PHASE_PROFILE=1",
        "phase_profile=%s\\n",
    ):
        require(build, needle, "quiet stock vitaGL counter switch")


def verify_coloroffset_fs_probe_contract(
    build: str,
    patch_bytes: bytes,
    cmake: str,
    phase_profile: str,
    phase_profile_header: str,
    probe_header: str,
    gl_backend: str,
    readme: str,
) -> None:
    """0008 owns diagnostic modes 1/2 and the separate opt-in neutral mode 3.
    All default OFF, custom_shaders.c only, stock fallback and shared ABI."""
    actual_patch_sha = hashlib.sha256(patch_bytes).hexdigest()
    if actual_patch_sha != COLOROFFSET_FS_PROBE_PATCH_SHA256:
        raise AssertionError(
            "ColorOffset FS probe patch SHA-256 changed: "
            f"{actual_patch_sha} != {COLOROFFSET_FS_PROBE_PATCH_SHA256}"
        )
    patch = patch_bytes.decode("utf-8")
    touched = set(re.findall(r"(?m)^\+\+\+ b/(.+)$", patch))
    if touched != {"source/custom_shaders.c"}:
        raise AssertionError(
            f"ColorOffset FS probe patch surface drifted: {sorted(touched)!r}")
    for needle in (
        "#ifdef HAVE_ISAAC_COLOROFFSET_FS_PROBE",
        '#error "Isaac ColorOffset FS probe requires the exact ColorOffset bundle"',
        "#if HAVE_ISAAC_COLOROFFSET_FS_PROBE < 1 || HAVE_ISAAC_COLOROFFSET_FS_PROBE > 3",
        "isaac_coloroffset_fs_probe_note_exact_source(s);",
        "isaac_coloroffset_fs_probe_link(p);",
        "isaac_coloroffset_fs_probe_release(p);",
        "isaac_coloroffset_fs_probe_select(",
        "void vglGetIsaacColorOffsetFsProbeStats(",
        # NODISCARD derivation receipts on the authenticated 2052-byte source.
        "#define ISAAC_COLOROFFSET_FS_PROBE_DISCARD_OFFSET 903u",
        "#define ISAAC_COLOROFFSET_FS_PROBE_DISCARD_SIZE 73u",
        "#define ISAAC_COLOROFFSET_FS_PROBE_DISCARD_FNV1A 0xeed28c60u",
        "#define ISAAC_COLOROFFSET_FS_PROBE_NODISCARD_SIZE 1979u",
        "#define ISAAC_COLOROFFSET_FS_PROBE_NODISCARD_FNV1A 0xfc8c5c9bu",
        # fail-closed link checks and translator bracketing
        "sceGxmProgramCheck(candidate) != 0",
        "sceGxmProgramGetType(candidate) != SCE_GXM_FRAGMENT_PROGRAM",
        "sceGxmProgramGetParameterCount(candidate) != 1u",
        "stock_fragment->unif_buf_size",
        'sceGxmProgramFindParameterByName(candidate, "Texture0")',
        "if (!is_shark_online && !start_shader_compiler()) {",
        "glsl_sema_mode = VGL_MODE_SHADER_PAIR;",
        "glsl_bindings_map = saved_bindings;",
        "glsl_translator_set_process(p->vshader, &probe);",
        # the probe fragment program mirrors the stock blend/output/MSAA
        "&blend_info.info, p->vshader->prog, &entry->fragment_program);",
        "SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 :",
        "#define ISAAC_COLOROFFSET_FS_PROBE_CACHE_CAPACITY 4u",
        "#if HAVE_ISAAC_COLOROFFSET_FS_PROBE == 3",
        "isaac_coloroffset_neutral_draw_is_eligible(",
        "isaac_coloroffset_contiguous_vertices_are_neutral(vertices,",
        "p->attr_map[index] != index",
        "size < 0x9cu",
        "vec3 Colorized = mix(Color.rgb, dot(Color.rgb, _lum) * ColorizeOut.rgb, ColorizeOut.a);",
        "fragColor = vec4(Colorized + ColorOffsetOut * Color.a, Color.a);",
    ):
        require(patch, needle, "ColorOffset FS probe patch")
    for forbidden in (
        # never the public header, never gxm.c
        "source/vitaGL.h", "source/gxm.c",
        # no guest-visible shader handles for the probe object
        "glCreateShader(", "glShaderSource(", "glCompileShader(",
        # NODISCARD is derived at runtime; neutral mode contains no clip/pixelate.
        "ClipPlaneOut", "PixelationAmountOut",
        # the stock shader object and its 0003 link record stay untouched
        "isaac_coloroffset_source_exact[index] = 0;",
        "p->fshader->id = ",
        "p->fprog = ",
    ):
        if forbidden in patch:
            raise AssertionError(
                f"ColorOffset FS probe patch contains {forbidden!r}")

    for needle in (
        "coloroffset_fs_probe=${ISAAC_COLOROFFSET_FS_PROBE:-0}",
        'coloroffset_fs_probe_patch="$script_dir/0008-isaac-coloroffset-fs-probe.patch"',
        "coloroffset_fs_probe must be exactly 0, 1 (TRIVIAL), 2 (NODISCARD) or 3 (NEUTRAL)",
        "ColorOffset FS probe requires ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS=1",
        'apply_source_patch "$coloroffset_fs_probe_patch"',
        "HAVE_ISAAC_COLOROFFSET_FS_PROBE=$coloroffset_fs_probe",
        "-DHAVE_ISAAC_COLOROFFSET_FS_PROBE=$coloroffset_fs_probe",
        '"$coloroffset_fs_probe" "$coloroffset_fs_probe_patch_sha"',
        "ColorOffset FS probe leaked into vitaGL.h",
        "ColorOffset FS probe endpoint escaped its opt-in build",
        "coloroffset_fs_probe=%s\\n",
        "coloroffset_fs_probe_mode=%s\\n",
        "coloroffset_fs_probe_patch_sha256=%s\\n",
    ):
        require(build, needle, "ColorOffset FS probe build.sh contract")
    order = [build.index(f'apply_source_patch "${name}_patch"')
             for name in ("coloroffset", "coloroffset_fs_probe", "shader_cache")]
    if order != sorted(order):
        raise AssertionError("build.sh applies 0008 outside 0003..0004")

    if not re.search(
        r"set\(ISAAC_VITA_COLOROFFSET_FS_PROBE OFF CACHE STRING\s*\n\s*\"",
        cmake,
    ):
        raise AssertionError("ColorOffset FS probe CMake knob is not default OFF")
    if not re.search(
        r"option\(ISAAC_VITA_COLOROFFSET_NEUTRAL_FASTPATH\s+"
        r'"[^"]+" OFF\)', cmake,
    ):
        raise AssertionError("ColorOffset neutral fastpath is not default OFF")
    for needle in (
        'ISAAC_VITA_COLOROFFSET_FS_PROBE_UPPER STREQUAL "TRIVIAL"',
        'ISAAC_VITA_COLOROFFSET_FS_PROBE_UPPER STREQUAL "NODISCARD"',
        "ISAAC_VITA_COLOROFFSET_FS_PROBE must be OFF, TRIVIAL or NODISCARD",
        "NOT ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS OR",
        "NOT ISAAC_VITA_COLOROFFSET_NEUTRAL_FASTPATH AND",
        "NOT ISAAC_VITA_PHASE_PROFILE)))",
        "option(ISAAC_VITA_COLOROFFSET_NEUTRAL_FASTPATH",
        "set(ISAAC_VITA_COLOROFFSET_FS_PROBE_MODE 3)",
        "ISAAC_COLOROFFSET_FS_PROBE=${ISAAC_VITA_COLOROFFSET_FS_PROBE_MODE}",
        "vitagl-stock-reference/0008-isaac-coloroffset-fs-probe.patch",
        "ISAAC_VITA_COLOROFFSET_FS_PROBE=${ISAAC_VITA_COLOROFFSET_FS_PROBE_MODE})",
        "never ship this build",
    ):
        require(cmake, needle, "ColorOffset FS probe production CMake")

    for needle in (
        '"[kage-vita] ph120.kp bid=%.32s win=%u loops=%u mode=%u "',
        "vglGetIsaacColorOffsetFsProbeStats(&s_window_fs_probe);",
        "kage_vita_profile_coloroffset_fs_probe_delta(",
        '# include "gl_vita_coloroffset_fs_probe.h"',
    ):
        require(phase_profile, needle, "ColorOffset FS probe ph120.kp owner")
    require(phase_profile_header, ",k,kp,b,", "ph120 record order")
    for needle in (
        "typedef struct vglIsaacColorOffsetFsProbeStats {",
        "uint32_t draws_opaque_overridden;",
        "== 21u * sizeof(uint32_t)",
        "void vglGetIsaacColorOffsetFsProbeStats(vglIsaacColorOffsetFsProbeStats *stats);",
    ):
        require(probe_header, needle, "ColorOffset FS probe consumer ABI")
    for needle in (
        '"KAGE VITA COLOROFFSET FS PROBE LINK: prog=%u mode=%u "',
        "gl_vita_backend_coloroffset_fs_probe_link_receipt(program);",
    ):
        require(gl_backend, needle, "ColorOffset FS probe link receipt")

    for needle in (
        "ISAAC_VITA_COLOROFFSET_FS_PROBE",
        "0008-isaac-coloroffset-fs-probe.patch",
        "ph120.kp",
        "never ship",
        # the production profile's deferred-path statement stays true
        "static_eligible`, `static_hits`, and `static_pixels`",
        "remain zero by construction",
    ):
        require(readme, needle,
                "stock reference README (ColorOffset FS probe statements)")
    for needle in (
        "| `ISAAC_VITA_COLOROFFSET_FS_PROBE` | 0008 |",
        "0008-isaac-coloroffset-fs-probe.patch",
        "never ships",
    ):
        require(readme, needle, "stock reference README")


def expect_rejected(
    label: str,
    expected_error: str,
    check: Callable[[], None],
) -> None:
    try:
        check()
    except AssertionError as exc:
        if expected_error not in str(exc):
            raise AssertionError(
                f"{label}: wrong rejection: {exc}"
            ) from exc
        return
    raise AssertionError(f"hostile mutation was accepted: {label}")


def main() -> int:
    vita = Path(__file__).resolve().parent
    runtime = vita.parent / "runtime"
    recipe_dir = vita / "vitagl-stock-reference"
    build = (recipe_dir / "build.sh").read_text(encoding="utf-8")
    patch_bytes = (
        recipe_dir / "0001-deterministic-build-and-init-oob.patch"
    ).read_bytes()
    gpu_draw_patch_bytes = (
        recipe_dir / "0002-exact-gpu-draw-optimizations.patch"
    ).read_bytes()
    gpu_draw_policy_bytes = (
        recipe_dir / "isaac_gpu_draw_policy.h"
    ).read_bytes()
    gxm_state_policy_bytes = (
        recipe_dir / "isaac_gxm_state_policy.h"
    ).read_bytes()
    coloroffset_patch_bytes = (
        recipe_dir / "0003-exact-coloroffset-gpu-optimizations.patch"
    ).read_bytes()
    coloroffset_policy_bytes = (
        recipe_dir / "isaac_coloroffset_gpu_policy.h"
    ).read_bytes()
    source_matcher = (
        runtime / "gl_vita_coloroffset_source.h"
    ).read_text(encoding="utf-8") + (
        runtime / "gl_vita_coloroffset_source.c"
    ).read_text(encoding="utf-8")
    source_oracle = (
        runtime / "gl_vita_coloroffset_source_oracle.c"
    ).read_text(encoding="utf-8")
    coloroffset_policy_oracle = (
        recipe_dir / "coloroffset_gpu_policy_oracle.c"
    ).read_text(encoding="utf-8")
    gpu_draw_oracle = (
        recipe_dir / "gpu_draw_policy_oracle.c"
    ).read_text(encoding="utf-8")
    cmake = (vita / "CMakeLists.txt").read_text(encoding="utf-8")
    backend = (runtime / "kage_vita_backend.c").read_text(encoding="utf-8")
    gl_backend = (runtime / "gl_vita_backend.c").read_text(encoding="utf-8")
    fs_probe_patch_bytes = (
        recipe_dir / "0008-isaac-coloroffset-fs-probe.patch"
    ).read_bytes()
    phase_profile = (
        runtime / "kage_vita_phase_profile.c"
    ).read_text(encoding="utf-8")
    phase_profile_header = (
        runtime / "kage_vita_phase_profile.h"
    ).read_text(encoding="utf-8")
    fs_probe_header = (
        runtime / "gl_vita_coloroffset_fs_probe.h"
    ).read_text(encoding="utf-8")
    stock_readme = (recipe_dir / "README.md").read_text(encoding="utf-8")

    verify_contract(build, patch_bytes, cmake, backend)
    verify_coloroffset_fs_probe_contract(
        build, fs_probe_patch_bytes, cmake, phase_profile,
        phase_profile_header, fs_probe_header, gl_backend, stock_readme,
    )
    verify_gpu_draw_contract(
        build, gpu_draw_patch_bytes, gpu_draw_policy_bytes,
        gxm_state_policy_bytes, cmake
    )
    verify_coloroffset_contract(
        build, coloroffset_patch_bytes, coloroffset_policy_bytes, cmake,
        source_matcher, source_oracle, coloroffset_policy_oracle, gl_backend,
        stock_readme,
    )
    for needle in (
        "0x7054709cu",
        "streams[index].stride = 88u",
        "prove_fetch_equivalence",
        "assert_cache_miss_for_every_field",
        "ISAAC_VITAGL_VERTEX_CACHE_CAPACITY",
        "oracle_release_cache",
    ):
        require(gpu_draw_oracle, needle, "GPU draw host oracle")

    make_insert = "    SINGLE_THREADED_GC=1 \\\n"
    for assignment in ("NO_DMAC=1", "HAVE_VITA3K_SUPPORT=1"):
        hostile_build = build.replace(
            make_insert,
            f"    {assignment} \\\n{make_insert}",
            1,
        )
        if hostile_build == build:
            raise AssertionError("make hostile-mutation seam disappeared")
        expect_rejected(
            assignment,
            "stock env/make argv changed",
            lambda value=hostile_build: verify_contract(
                value, patch_bytes, cmake, backend
            ),
        )

    hostile_makefile_patch = patch_bytes.replace(
        b"+++ b/Makefile\n",
        b"+++ b/Makefile\n"
        b"@@ -1,3 +1,4 @@\n"
        b"+# hostile extra Makefile hunk\n"
        b" TARGET          := libvitaGL\n"
        b" SOURCES         := source source/utils source/utils/preprocessor\n"
        b" \n",
        1,
    )
    hostile_makefile_patch = hostile_makefile_patch.replace(
        b"@@ -11,8 +11,11 @@", b"@@ -11,8 +12,11 @@", 1
    ).replace(
        b"@@ -275,7 +278,7 @@", b"@@ -275,7 +279,7 @@", 1
    )
    hostile_vgl_patch = patch_bytes.replace(
        b"+++ b/source/vgl.c\n",
        b"+++ b/source/vgl.c\n"
        b"@@ -1,3 +1,4 @@\n"
        b"+/* hostile extra source/vgl.c hunk */\n"
        b" /*\n"
        b"  * This file is part of vitaGL\n"
        b"  * Copyright 2017, 2018, 2019, 2020 Rinnegatamante\n",
        1,
    ).replace(
        b"@@ -303,7 +303,7 @@", b"@@ -303,7 +304,7 @@", 1
    )
    hostile_patches = (
        (
            hostile_makefile_patch,
            "extra valid Makefile hunk",
        ),
        (
            hostile_vgl_patch,
            "extra valid source/vgl.c hunk",
        ),
    )
    for hostile_patch, label in hostile_patches:
        if hostile_patch == patch_bytes:
            raise AssertionError(f"{label}: patch mutation seam disappeared")
        expect_rejected(
            label,
            "stock upstream patch SHA-256 changed",
            lambda value=hostile_patch: verify_contract(
                build, value, cmake, backend
            ),
        )

    print(
        "Stock vitaGL recipe static gate: PASS; exact env/make argv/patch SHA/"
        "hostile extra flags+hunks/runtime attribution and opt-in GPU draw/"
        "canonical-quad/GXM-state surfaces and default-off ColorOffset "
        "byte-identical-source/opaque/lifecycle gate fixed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
