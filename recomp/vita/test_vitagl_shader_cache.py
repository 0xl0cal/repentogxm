#!/usr/bin/env python3
"""Host proof for the opt-in Isaac vitaGL shader cache and loading labels."""

from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import tempfile


VITA = Path(__file__).resolve().parent
RECOMP = VITA.parent
RUNTIME = RECOMP / "runtime"
RECIPE = VITA / "vitagl-stock-reference"


def require(text: str, needle: str, owner: str) -> None:
    if needle not in text:
        raise AssertionError(f"{owner} lost required evidence: {needle}")


def require_order(text: str, needles: tuple[str, ...], owner: str) -> None:
    cursor = -1
    for needle in needles:
        position = text.find(needle, cursor + 1)
        if position < 0:
            raise AssertionError(f"{owner} lost ordered evidence: {needle}")
        cursor = position


def prove_host_oracles() -> None:
    compiler = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory(prefix="isaac-shader-cache-") as temporary:
        for source_name, output_name, extra_flags, evidence in (
            (
                "shader_cache_policy_oracle.c",
                "shader-cache-policy-oracle",
                (),
                "shader-cache hostile oracle: PASS",
            ),
            (
                "shader_cache_ubo_oracle.c",
                "shader-cache-ubo-oracle",
                ("-Wno-unused-function",),
                "shader-cache GXP/UBO oracle: PASS",
            ),
        ):
            executable = Path(temporary) / output_name
            subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    *extra_flags,
                    str(RECIPE / source_name),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            result = subprocess.run(
                [str(executable)], check=True, text=True, capture_output=True
            )
            require(result.stdout, evidence, source_name)


def prove_static_contract() -> None:
    patch = (RECIPE / "0004-hardened-custom-shader-cache.patch").read_text(
        encoding="utf-8"
    )
    policy = (RECIPE / "isaac_shader_cache_policy.h").read_text(
        encoding="utf-8"
    )
    integration = (RECIPE / "isaac_shader_cache_vitagl.h").read_text(
        encoding="utf-8"
    )
    block_adapter = (RECIPE / "isaac_shader_cache_block_list.h").read_text(
        encoding="utf-8"
    )
    build = (RECIPE / "build.sh").read_text(encoding="utf-8")
    cmake = (VITA / "CMakeLists.txt").read_text(encoding="utf-8")
    loading = (RUNTIME / "kage_vita_loading.c").read_text(encoding="utf-8")
    loading_header = (RUNTIME / "kage_vita_loading.h").read_text(
        encoding="utf-8"
    )
    gl_backend = (RUNTIME / "gl_vita_backend.c").read_text(encoding="utf-8")

    changed_paths = re.findall(
        r"^diff --git a/([^ ]+) b/([^\n]+)$", patch, flags=re.MULTILINE
    )
    expected_paths = {
        ("source/custom_shaders.c", "source/custom_shaders.c"),
        ("source/vitaGL.h", "source/vitaGL.h"),
    }
    if set(changed_paths) != expected_paths:
        raise AssertionError(f"shader patch escaped its two-file surface: {changed_paths}")
    for needle in (
        '#include "isaac_shader_cache_vitagl.h"',
        "HAVE_ISAAC_SHADER_CACHE",
        "isaac_shader_cache_capture_source(s)",
        "isaac_shader_cache_load_shader",
        "vglGetIsaacShaderCacheStats",
        "+#ifndef HAVE_ISAAC_SHADER_CACHE\n \t// If vitaShaRK is not enabled",
        "A valid hit needs no runtime compiler at all",
        "uint32_t source_size = s->size;",
        "s->size = source_size;",
    ):
        require(patch, needle, "vitaGL cache patch")
    link_patch = patch[patch.index("void glLinkProgram(GLuint progr)") :]
    require_order(
        link_patch,
        (
            "isaac_shader_cache_load_shader(",
            "if (!is_shark_online && !start_shader_compiler())",
            "isaac_cache_key = isaac_vertex_key",
        ),
        "warm pair compiler bypass",
    )

    for forbidden in ("git apply --unidiff-zero", "git apply --check --unidiff-zero"):
        if forbidden in build:
            raise AssertionError(f"build retained unsafe patch mode: {forbidden}")
    for needle in (
        'shader_cache=${ISAAC_SHADER_CACHE:-0}',
        'git -c core.autocrlf=false -c core.eol=lf \\\n            apply --check "$patch"',
        'git -c core.autocrlf=false -c core.eol=lf \\\n            apply "$patch"',
        'apply_source_patch "$contract_patch"',
        'apply_source_patch "$gpu_draw_patch"',
        'apply_source_patch "$coloroffset_patch"',
        'apply_source_patch "$shader_cache_patch"',
        'cp -- "$shader_cache_policy"',
        'cp -- "$shader_cache_integration"',
        'cp -- "$shader_cache_block_list"',
        '"$shader_cache_integration_sha"',
        "d8b68dfa9d24eae246f521fcdbe174bfcb827be1ffe2fed75c4977017e94c5de",
        "verify_sha \"$source_dir/source/custom_shaders.c\"",
        "verify_sha \"$source_dir/source/vitaGL.h\"",
        "shader_cache_integration_sha256=",
        "shader_cache_block_list_sha256=",
        "ux0:data/isaacr001/shader-cache/v2",
    ):
        require(build, needle, "pinned vitaGL recipe")
    if "option(ISAAC_VITA_VITAGL_SHADER_CACHE\n" not in cmake or not re.search(
        r"option\(ISAAC_VITA_VITAGL_SHADER_CACHE\s+\n?\s*\"[^\"]+\" OFF\)",
        cmake,
    ):
        raise AssertionError("shader cache is not a default-OFF CMake option")
    for needle in (
        "ISAAC_VITA_VITAGL_SHADER_CACHE requires translated KAGE",
        "ISAAC_VITA_VITAGL_SHADER_CACHE_INTEGRATION",
        "ISAAC_SHADER_CACHE=${ISAAC_VITA_VITAGL_SHADER_CACHE_MODE}",
        "APPEND PROPERTY COMPILE_DEFINITIONS\n          ISAAC_VITA_VITAGL_SHADER_CACHE=1",
        "HAVE_ISAAC_SHADER_CACHE=1",
    ):
        require(cmake, needle, "CMake cache closure")

    for needle in (
        '"ux0:data/isaacr001/shader-cache"',
        "ISAAC_SHADER_CACHE_VERSION       2u",
        "ISAAC_SHADER_CACHE_GXP_MAX",
        "ISAAC_SHADER_CACHE_METADATA_MAX",
        "ISAAC_SHADER_CACHE_MATRIX_MAX",
        "ISAAC_SHADER_CACHE_BLOCK_MAX",
        "ISAAC_SHADER_CACHE_METADATA_HEADER_SIZE",
        "ISAAC_SHADER_CACHE_BLOCK_RECORD_SIZE",
        "'I', 'S', 'M', '2'",
        "ISAAC_SHADER_CACHE_BUILD_HEX must be the exact 64-digit recipe SHA-256",
        "SCE_O_EXCL",
        "isaac_shader_cache_io_sync(descriptor)",
        "isaac_shader_cache_io_close(descriptor)",
        "isaac_shader_cache_read_path(\n            temp_path",
        "isaac_shader_cache_io_rename(temp_path, final_path)",
        'isaac_shader_cache_io_sync_device("ux0:")',
        "isaac_shader_cache_validate_body",
    ):
        require(policy, needle, "cache policy")
    for field in (
        "publish_attempts",
        "publish_with_block_list",
        "publish_ready",
        "publish_block_records",
        "publish_reject_shape",
        "publish_reject_semantics",
        "publish_reject_matrices",
        "publish_reject_blocks",
        "publish_reject_incomplete",
        "publish_reject_alloc",
    ):
        require(policy, f"uint64_t {field};", "internal cache statistics")
        require(patch, f"+\tuint64_t {field};", "public cache statistics")
        require(gl_backend, f"stats.{field}", "cache telemetry")
    require_order(
        policy,
        (
            "isaac_shader_cache_read_exact(\n            descriptor, header",
            "isaac_shader_cache_header_valid(",
            "body = (unsigned char *)isaac_shader_cache_alloc(body_size)",
            "isaac_shader_cache_read_exact(descriptor, body, body_size)",
            "isaac_shader_cache_validate_body(",
        ),
        "bounded record reader",
    )

    load_start = integration.index("static int isaac_shader_cache_load_shader")
    load_end = integration.index("static int isaac_shader_cache_publish_shader")
    load = integration[load_start:load_end]
    require_order(
        load,
        (
            "isaac_shader_cache_check_program(",
            "isaac_shader_cache_metadata_decode_header(",
            "isaac_shader_cache_gxp_blocks_valid(",
            "sceGxmProgramGetParameterCount(",
            "isaac_shader_cache_rebuild_blocks(",
            "isaac_shader_cache_register_program(",
            "s->prog = (const SceGxmProgram *)record.body",
            "s->unif_blk = blocks",
            "vgl_free(s->source)",
        ),
        "cache-hit transaction",
    )
    for needle in (
        "compiler_opts",
        "compiler_fastmath",
        "compiler_fastprecision",
        "compiler_fastint",
        "ISAAC_SHADER_CACHE_FLAG_BINDINGS",
        "ISAAC_SHADER_CACHE_FLAG_PAIR",
        "ISAAC_SHADER_CACHE_FLAG_GLSL",
        "ISAAC_SHADER_CACHE_FLAG_PEER_GLSL",
        "source->sha256",
        "peer_source->sha256",
        "isaac_shader_cache_semantics_valid",
        "isaac_shader_cache_collect_gxp_blocks",
        "SCE_GXM_PARAMETER_CATEGORY_UNIFORM_BUFFER",
        "sceGxmProgramFindParameterByName",
        "isaac_shader_cache_encode_block",
        "isaac_shader_cache_note_publish_attempt",
        "isaac_shader_cache_note_publish_ready",
        "memchr(semantics->texcoord_names[index]",
        "memchr(semantics->color_names[index]",
        "if (!matrix->ptr)",
        "program_result != ISAAC_SHADER_CACHE_PROGRAM_ACCEPT",
        "isaac_shader_cache_reject_corrupt(key, &record)",
    ):
        require(integration, needle, "vitaGL integration")
    for needle in (
        "isaac_shader_cache_rebuild_blocks",
        "isaac_shader_cache_blocks_unique",
        "block->chain = NULL",
        "isaac_shader_cache_free_blocks(blocks)",
    ):
        require(block_adapter, needle, "block reconstruction adapter")
    publish = integration[integration.index(
        "static int isaac_shader_cache_publish_shader"
    ) :]
    if "metadata_complete || s->unif_blk" in publish:
        raise AssertionError("publish still rejects every compiler block list")
    require_order(
        publish,
        (
            "isaac_shader_cache_canonical_gxp_size(",
            "key, s->prog, canonical_gxp_size,",
        ),
        "canonical unpadded GXP publication",
    )
    require(gl_backend, "KAGE VITA SHADER CACHE PUBLISH:", "cache telemetry")
    require_order(
        patch,
        (
            "sceGxmShaderPatcherRegisterProgram(",
            "+\t\tif (!res)",
            "+\t\tvgl_free(s->source)",
        ),
        "native compile transaction",
    )

    for stale in (
        "KAGE_LOADING_FREAD_TOTAL",
        "KAGE_LOADING_PERCENT_CAP",
        "kage_vita_loading_percent",
        '"ABOUT 00%"',
        "approx=",
    ):
        if stale in loading or stale in loading_header:
            raise AssertionError(f"loading UI retained false progress: {stale}")
    for needle in (
        "KAGE_VITA_LOADING_VERIFY",
        "KAGE_VITA_LOADING_ARCHIVES",
        "KAGE_VITA_LOADING_SHADERS",
        "verify_notes=%u",
        "archive_notes=%u",
        "shader_notes=%u",
        "vglSetDisplayCallback(NULL)",
        "sceGxmDisplayQueueFinish()",
    ):
        require(loading + loading_header, needle, "loading lifecycle")
    require_order(
        loading,
        (
            "vglSetDisplayCallback(NULL)",
            "sceGxmDisplayQueueFinish()",
            "kage_loading_snapshot_store(0u)",
            "kage_loading_control_unlock()",
        ),
        "display callback handoff",
    )
    require(gl_backend, "kage_vita_loading_note_shader();", "typed GL boundary")


def main() -> None:
    prove_static_contract()
    prove_host_oracles()
    print("Isaac vitaGL shader-cache/static loading contract: PASS")


if __name__ == "__main__":
    main()
