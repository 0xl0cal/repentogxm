#!/usr/bin/env python3
"""Host proof for the opt-in Isaac vitaGL shader cache and loading labels."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile


VITA = Path(__file__).resolve().parent
RECOMP = VITA.parent
RUNTIME = RECOMP / "runtime"
RECIPE = VITA / "vitagl-stock-reference"
SHADER_CACHE_INTEGRATION_SHA256 = (
    "70066e0aebada1612bb247fd560d132f5fea94d89793f29b15e310495616e7ee"
)


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


def prove_observer_recipe_identity() -> None:
    """Execute the actual pure build.sh recipe code, never downloads/builds."""
    build = (RECIPE / "build.sh").read_text(encoding="utf-8")
    # Observer patches do not touch translator/source/metadata production.
    # Shared-header redirects concern only RT/Finish, not shaders; sparse
    # SDK timers must stay outside custom_shaders.c and public vitaGL.h.
    for patch_name, expected in (
        ("0005-isaac-scene-timer.patch", {"source/gxm.c"}),
        ("0006-isaac-scene-split.patch", {"source/gxm.c"}),
        ("0020-isaac-native-resource-profile.patch", {
            "source/framebuffers.c", "source/gxm.c", "source/shared.h",
            "source/textures.c", "source/utils/gpu_utils.c", "source/utils/mem_utils.h"}),
        ("0025-isaac-sdk-sparse-profile.patch", {
            "source/draw.c", "source/gxm.c", "source/misc.c"}),
        ("0027-isaac-laser-halo-profile.patch", {"source/custom_shaders.c"}),
    ):
        patch = (RECIPE / patch_name).read_text(encoding="utf-8")
        changed_paths = set(re.findall(
            r"^diff --git a/([^ ]+) b/([^\n]+)$", patch, re.M))
        assert changed_paths == {(name, name) for name in expected}, patch_name
    internal = (RECIPE / "isaac_native_resource_profile_internal.h").read_text(encoding="utf-8")
    assert set(re.findall(r"^#define (sce\w+) ", internal, re.M)) == {
        "sceGxmFinish", "sceGxmCreateRenderTarget", "sceGxmDestroyRenderTarget"}
    for name in ("isaac_shader_cache_policy.h", "isaac_shader_cache_vitagl.h",
                 "isaac_shader_cache_block_list.h"):
        text = (RECIPE / name).read_text(encoding="utf-8")
        assert "HAVE_ISAAC_GL_TIME_PROFILE" not in text
        assert "HAVE_ISAAC_NATIVE_RESOURCE_PROFILE" not in text
        assert "HAVE_ISAAC_GL_TIME_SDK_SPARSE" not in text
    begin = build.index("emit_recipe_inputs()\n{")
    end = build.index('source_parent="$work/source-$recipe"', begin)
    fragment = build[begin:end]
    body = fragment[:fragment.index("\nrecipe=$(")]
    printf_body = body[body.index("    printf '%s\\n' "):
                       body.index("    # ON changes")]
    names = re.findall(r'"\$([a-z0-9_]+)"', printf_body)
    aliases = {"recipe_build_flags": "build_flags",
               "recipe_gl_time": "gl_time_profile",
               "recipe_native_resource": "native_resource_profile"}
    names = [aliases.get(name, name) for name in names]
    # All mandatory FBO repairs, including both replay variants, are functional.
    assert names[-4:] == ["fbo_float_sync_patch_sha", "fbo_scissor_resize_patch_sha",
                          "fbo_scissor_replay_patch_sha", "fbo_scissor_replay_region_patch_sha"]
    assert len(names) == 94 and names.count("coloroffset_staging_outline") == 1
    prior_outline_names = [name for name in names if name != "coloroffset_staging_outline"]
    assert len(prior_outline_names) == 93 and prior_outline_names.count("coloroffset_staging_limits_neon") == 1
    prior_limits_names = [name for name in prior_outline_names if name != "coloroffset_staging_limits_neon"]
    assert len(prior_limits_names) == 92 and prior_limits_names.count("coloroffset_staging_finite_neon") == 1
    prior_neon_names = [name for name in prior_limits_names if name != "coloroffset_staging_finite_neon"]
    assert len(prior_neon_names) == 91 and prior_neon_names.count("coloroffset_staging_plain_fusion") == 1
    prior_fusion_names = [name for name in prior_neon_names if name != "coloroffset_staging_plain_fusion"]
    assert len(prior_fusion_names) == 90
    prior_fbo_names = prior_fusion_names[:-4]
    # Proven emission is functional, never normalized with diagnostic inputs.
    assert prior_fbo_names.count("laser_halo_proven_emit") == 1 and len(prior_fbo_names) == 86
    prior_producer_names = [name for name in prior_fbo_names if name != "laser_halo_proven_emit"]
    # Both quality admission options are functional inputs in BOTH keys.
    assert prior_producer_names.count("laser_light_producer_uv") == 1 and len(prior_producer_names) == 85
    prior_cap_names = [name for name in prior_producer_names if name != "laser_light_producer_uv"]
    # The red-cap option is a functional input in BOTH keys. Removing it
    # recovers the previous lease revision's prefix (checked below).
    assert prior_cap_names.count("laser_red_cap_side_clip") == 1 and len(prior_cap_names) == 84
    prior_lease_names = [name for name in prior_cap_names if name != "laser_red_cap_side_clip"]
    # The lease option and patch are functional inputs in BOTH keys. Removing
    # exactly those two recovers the frozen metadata revision's81-field prefix.
    lease_inputs = ["fbo_rt_reuse_lease", "fbo_rt_reuse_lease_patch_sha"]
    lease_offset = names.index("fbo_rt_reuse_source_sha") + 1
    assert names[lease_offset:lease_offset + 2] == lease_inputs
    assert all(names.count(name) == 1 for name in lease_inputs)
    previous_names = [name for name in prior_lease_names if name not in lease_inputs]
    assert len(prior_lease_names) == 83 and len(previous_names) == 81 and hashlib.sha256(
        ("\n".join(previous_names) + "\n").encode()).hexdigest() == (
            "949e381c85a89bc4b4228b7f282f2ce78de0b47676c5b9257e728a6a10517a00")
    metadata_names = ("coloroffset_metadata_once", "coloroffset_metadata_once_patch_sha")
    assert all(names.count(name) == 1 for name in metadata_names)
    historical = [name for name in previous_names if name not in metadata_names]
    assert len(historical) == 79 and hashlib.sha256(
        ("\n".join(historical) + "\n").encode()).hexdigest() == (
            "6a555a1892d313c7cd0b5c6f75a0d7600b0a691fc2d4e3ac462ddf9406c76709")
    # The diagnostic suffix is outside that historical prefix. It always
    # identifies the full native artifact, including disabled helper inputs,
    # but is deliberately absent from the observer-compatible shader key.
    sparse_names = (
        "gl_time_sdk_sparse", "sdk_sparse_patch_sha", "sdk_sparse_api_sha",
        "sdk_sparse_internal_sha", "sdk_sparse_source_sha",
    )
    halo_names = ("laser_halo_profile", "laser_halo_profile_patch_sha", "laser_halo_profile_api_sha", "white_census_patch_sha")
    require(body, "emit_sdk_sparse_recipe_inputs()\n{", "native-only sparse recipe")
    bash = os.environ.get("BASH") or (
        "C:/Program Files/Git/bin/bash.exe" if os.name == "nt" and
        Path("C:/Program Files/Git/bin/bash.exe").is_file() else shutil.which("bash"))
    if not bash:
        raise AssertionError("Bash is required to execute actual recipe code")
    base = {name: name + "-fixture" for name in (*names, *sparse_names, *halo_names)}
    base.update(shader_cache="1", phase_profile="1", gl_time_sdk_sparse="0", laser_halo_profile="0")
    flags = "SOFTFP_ABI=1 HAVE_ISAAC_PHASE_PROFILE=1 HAVE_ISAAC_SHADER_CACHE=1"
    observer_defines = {
        "HAVE_ISAAC_GL_TIME_PROFILE=1", "HAVE_ISAAC_NATIVE_RESOURCE_PROFILE=1",
        "HAVE_ISAAC_GL_TIME_SDK_SPARSE=1",
        "HAVE_ISAAC_LASER_HALO_PROFILE=1",
    }

    def expected_prefix(values, compat):
        result = "\n".join(values[name] for name in names) + "\n"
        if compat:
            result += "shader-cache-observer-compat-v1=1\n"
        return result

    def run(values, compat):
        script = "set -eu\n" + "\n".join(
            name + "=" + shlex.quote(value) for name, value in values.items())
        script += "\nshader_cache_observer_compat=" + str(compat) + "\n" + fragment
        script += '\nprintf "%s %s\\n" "$recipe" "$shader_cache_recipe"\n'
        script += 'emit_recipe_inputs "$gl_time_profile" "$native_resource_profile" "$build_flags"\n'
        script += 'emit_sdk_sparse_recipe_inputs\n'
        script += 'emit_halo_profile_recipe_inputs\n'
        result = subprocess.run([bash], input=script, text=True,
                                capture_output=True, check=True)
        first, raw = result.stdout.split("\n", 1)
        native, cache = first.split()
        expected = expected_prefix(values, compat)
        expected += "\n".join(values[name] for name in sparse_names) + "\n"
        expected += "\n".join(values[name] for name in halo_names) + "\n"
        assert raw == expected, "native historical prefix/sparse suffix bytes changed"
        assert native == hashlib.sha256(raw.encode()).hexdigest()
        if compat:
            normalized = dict(values, gl_time_profile="0", native_resource_profile="0")
            normalized["build_flags"] = " ".join(
                token for token in values["build_flags"].split()
                if token not in observer_defines)
            shader_bytes = expected_prefix(normalized, 1)
        else:
            shader_bytes = expected
        assert cache == hashlib.sha256(shader_bytes.encode()).hexdigest(), (
            "shader key did not preserve its exact selected input population")
        return native, cache

    on, off = [], []
    for gl_time, native_resource in ((0, 0), (1, 0), (0, 1), (1, 1)):
        for sparse in (0, 1):
            values = dict(base, gl_time_profile=str(gl_time),
                          native_resource_profile=str(native_resource),
                          gl_time_sdk_sparse=str(sparse), build_flags=flags)
            if gl_time:
                values["build_flags"] += " HAVE_ISAAC_GL_TIME_PROFILE=1"
            if native_resource:
                values["build_flags"] += " HAVE_ISAAC_NATIVE_RESOURCE_PROFILE=1"
            if sparse:
                values["build_flags"] += " HAVE_ISAAC_GL_TIME_SDK_SPARSE=1"
            off.append(run(values, 0))
            on.append(run(values, 1))
    assert all(native == cache for native, cache in off)
    assert len({native for native, _ in off}) == 8
    assert len({native for native, _ in on}) == 8
    assert len({cache for _, cache in on}) == 1
    assert all(off[i][0] != on[i][0] for i in range(8)), "opt toggle reused native artifact"

    stable = dict(base, gl_time_profile="1", native_resource_profile="1",
                  gl_time_sdk_sparse="1", build_flags=flags +
                  " HAVE_ISAAC_GL_TIME_PROFILE=1 HAVE_ISAAC_NATIVE_RESOURCE_PROFILE=1"
                  " HAVE_ISAAC_GL_TIME_SDK_SPARSE=1")
    reference = run(stable, 1)
    counted = dict(stable, laser_halo_profile="1",
                   build_flags=stable["build_flags"] + " HAVE_ISAAC_LASER_HALO_PROFILE=1")
    counted_keys = run(counted, 1)
    assert counted_keys[0] != reference[0] and counted_keys[1] == reference[1]
    for halo in (0, 1):
        baseline = counted if halo else stable
        for compat in (0, 1):
            baseline_keys = run(baseline, compat)
            for name in halo_names:
                changed = dict(baseline)
                changed[name] += "-changed"
                native, cache = run(changed, compat)
                assert native != baseline_keys[0]
                assert (cache == baseline_keys[1]) == bool(compat), (halo, compat, name)
    for name in names:
        if name in ("gl_time_profile", "native_resource_profile", "build_flags"):
            continue
        changed = dict(stable)
        changed[name] += "-changed"
        native, cache = run(changed, 1)
        assert native != reference[0] and cache != reference[1], name
    # Every sparse-only suffix field is sensitive in the native key even
    # when sparse is OFF. Compatibility alone excludes it from shader keys.
    for sparse in (0, 1):
        baseline = dict(stable, gl_time_sdk_sparse=str(sparse))
        if not sparse:
            baseline["build_flags"] = baseline["build_flags"].removesuffix(
                " HAVE_ISAAC_GL_TIME_SDK_SPARSE=1")
        for compat in (0, 1):
            baseline_keys = run(baseline, compat)
            for name in sparse_names:
                changed = dict(baseline)
                changed[name] = (str(1 - sparse) if name == "gl_time_sdk_sparse"
                                 else changed[name] + "-changed")
                native, cache = run(changed, compat)
                assert native != baseline_keys[0], (sparse, compat, name)
                assert (cache == baseline_keys[1]) == bool(compat), (
                    sparse, compat, name)
    # PHASE and even similar-looking define names must not be stripped.
    for suffix in (" HAVE_ISAAC_PHASE_PROFILE=0", " HAVE_ISAAC_GL_TIME_PROFILE_EXTRA=1",
                   " HAVE_ISAAC_NATIVE_RESOURCE_PROFILE_EXTRA=1",
                   " HAVE_ISAAC_GL_TIME_PROFILE=10", " HAVE_ISAAC_NATIVE_RESOURCE_PROFILE=10",
                   " HAVE_ISAAC_GL_TIME_SDK_SPARSE_EXTRA=1",
                   " HAVE_ISAAC_GL_TIME_SDK_SPARSE=10", " HAVE_ISAAC_GL_TIME_SDK_SPARSE=0",
                   " HAVE_ISAAC_LASER_HALO_PROFILE_EXTRA=1", " HAVE_ISAAC_LASER_HALO_PROFILE=10",
                   " HAVE_ISAAC_LASER_HALO_PROFILE=0"):
        changed = dict(stable, build_flags=stable["build_flags"] + suffix)
        assert run(changed, 1)[1] != reference[1], suffix
    for proven in (0, 1):
        changed = dict(stable, laser_halo_proven_emit=str(proven),
                       build_flags=stable["build_flags"] + f" HAVE_ISAAC_LASER_HALO_PROVEN_EMIT={proven}")
        assert run(changed, 1)[1] != reference[1]
    for fusion in (0, 1):
        changed = dict(stable, coloroffset_staging_plain_fusion=str(fusion),
                       build_flags=stable["build_flags"] + f" HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION={fusion}")
        assert run(changed, 1)[1] != reference[1]
    require(build, 'coloroffset_staging_plain_fusion=${ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION:-0}', "staging fusion default OFF")
    require(build, '"coloroffset_staging_plain_fusion:$coloroffset_staging_plain_fusion"', "staging fusion strict 0/1 option")
    require(build, 'ColorOffset staging PLAIN fusion requires STAGING_PROOF=1 and PLAIN_FASTPATH=1', "staging fusion dependencies")
    require(build, "printf 'coloroffset_staging_plain_fusion=%s\\n'", "staging fusion build contract")
    for neon in (0, 1):
        changed = dict(stable, coloroffset_staging_finite_neon=str(neon),
                       build_flags=stable["build_flags"] + f" HAVE_ISAAC_COLOROFFSET_STAGING_FINITE_NEON={neon}")
        assert run(changed, 1)[1] != reference[1]
    require(build, 'coloroffset_staging_finite_neon=${ISAAC_COLOROFFSET_STAGING_FINITE_NEON:-0}', "staging finite NEON default OFF")
    require(build, '"coloroffset_staging_finite_neon:$coloroffset_staging_finite_neon"', "staging finite NEON strict 0/1 option")
    require(build, 'ColorOffset staging finite NEON requires STAGING_PROOF=1 and STAGING_PLAIN_FUSION=1', "staging finite NEON dependencies")
    require(build, "printf 'coloroffset_staging_finite_neon=%s\\n'", "staging finite NEON build contract")
    for limits in (0, 1):
        changed = dict(stable, coloroffset_staging_limits_neon=str(limits),
                       build_flags=stable["build_flags"] + f" HAVE_ISAAC_COLOROFFSET_STAGING_LIMITS_NEON={limits}")
        assert run(changed, 1)[1] != reference[1]
    require(build, 'coloroffset_staging_limits_neon=${ISAAC_COLOROFFSET_STAGING_LIMITS_NEON:-0}', "staging limits NEON default OFF")
    require(build, '"coloroffset_staging_limits_neon:$coloroffset_staging_limits_neon"', "staging limits NEON strict 0/1 option")
    require(build, 'ColorOffset staging limits NEON requires STAGING_FINITE_NEON=1', "staging limits NEON dependency")
    require(build, "printf 'coloroffset_staging_limits_neon=%s\\n'", "staging limits NEON build contract")
    for outline in (0, 1):
        changed = dict(stable, coloroffset_staging_outline=str(outline),
                       build_flags=stable["build_flags"] + f" HAVE_ISAAC_COLOROFFSET_STAGING_OUTLINE={outline}")
        native, cache = run(changed, 1)
        assert native != reference[0] and cache != reference[1]
    require(build, 'coloroffset_staging_outline=${ISAAC_COLOROFFSET_STAGING_OUTLINE:-0}', "staging outline default OFF")
    require(build, '"coloroffset_staging_outline:$coloroffset_staging_outline"', "staging outline strict 0/1 option")
    require(build, 'ColorOffset staging outline requires STAGING_LIMITS_NEON=1', "staging outline dependency")
    require(build, "printf 'coloroffset_staging_outline=%s\\n'", "staging outline build contract")
    require(build, 'laser_halo_proven_emit=${ISAAC_LASER_HALO_PROVEN_EMIT:-0}', "proven emission default OFF")
    require(build, '"laser_halo_proven_emit:$laser_halo_proven_emit"', "proven emission strict 0/1 option")
    require(build, 'Proven halo emission requires ISAAC_LASER_LIGHT_PRODUCER_UV=1', "proven emission dependency")
    require(build, 'shader_cache_observer_compat=${ISAAC_SHADER_CACHE_OBSERVER_COMPAT:-0}', "default OFF")
    require(build, '"shader_cache_observer_compat:$shader_cache_observer_compat"', "strict 0/1 option")
    require(build, 'gl_time_sdk_sparse=${ISAAC_GL_TIME_SDK_SPARSE:-0}', "sparse default OFF")
    require(build, 'laser_halo_profile=${ISAAC_LASER_HALO_PROFILE:-0}', "halo default OFF")
    require(build, '"laser_halo_profile:$laser_halo_profile"', "halo strict 0/1 option")
    require(build, '"gl_time_sdk_sparse:$gl_time_sdk_sparse"', "sparse strict 0/1 option")
    require(build, 'shader-cache observer compatibility requires ISAAC_SHADER_CACHE=1', "cache dependency")
    require(build, 'shader_cache_identity=observer-gl-time-native-resource-v1', "identity contract")
    require(build, 'source_parent="$work/source-$recipe"', "full native source marker")
    require(build, 'printf \'%s\\n\' "$recipe" > "$output_root/recipe.txt"', "full native recipe output")
    assert re.search(r"extra_cflags=.*ISAAC_SHADER_CACHE_BUILD_HEX=.*\$shader_cache_recipe", build)
    print("shader-cache observer identity: PASS 8 observer triples; historical prefix exact; "
          f"all {len(names) - 3} other inputs sensitive; 5 native-only sparse + 4 halo inputs sensitive ON/OFF; halo flags compatible")


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
    # Three pre-existing patches and the SDK-sparse diagnostic patch are the
    # only zero-context exceptions.
    # Pin both the allowlist and assignments rather than silently accepting an
    # arbitrary $patch_context expansion in the check/apply commands below.
    require(build, '''    patch_context=
    if [ "$patch" = "$laser_light_halo_patch" ] ||
       [ "$patch" = "$laser_light_nearest_patch" ] ||
       [ "$patch" = "$laser_atlas_nearest_patch" ] ||
       [ "$patch" = "$sdk_sparse_patch" ]; then
        patch_context=--unidiff-zero
    fi
''', "bounded native patch-context exceptions")
    if re.findall(r"^\s*patch_context=(.*)$", build, re.MULTILINE) != [
        "", "--unidiff-zero"
    ]:
        raise AssertionError("native patch-context assignments exceeded allowlist")
    for needle in (
        'shader_cache=${ISAAC_SHADER_CACHE:-0}',
        'git -c core.autocrlf=false -c core.eol=lf \\\n            apply $patch_context --check "$patch"',
        'git -c core.autocrlf=false -c core.eol=lf \\\n            apply $patch_context "$patch"',
        'apply_source_patch "$contract_patch"',
        'apply_source_patch "$gpu_draw_patch"',
        'apply_source_patch "$coloroffset_patch"',
        'apply_source_patch "$shader_cache_patch"',
        'cp -- "$shader_cache_policy"',
        'cp -- "$shader_cache_integration"',
        'cp -- "$shader_cache_block_list"',
        '"$shader_cache_integration_sha"',
        'shader_cache_integration_sha=$(sha256sum "$shader_cache_integration"',
        "verify_sha \"$source_dir/source/custom_shaders.c\"",
        "verify_sha \"$source_dir/source/vitaGL.h\"",
        "shader_cache_integration_sha256=",
        "shader_cache_block_list_sha256=",
        "ux0:data/isaacr001/shader-cache/v2",
    ):
        require(build, needle, "pinned vitaGL recipe")
    # build.sh hashes the integration header at run time (verify_sha against
    # $shader_cache_integration_sha); the pin of its content lives here.
    integration_sha = hashlib.sha256(
        (RECIPE / "isaac_shader_cache_vitagl.h").read_bytes()
    ).hexdigest()
    if integration_sha != SHADER_CACHE_INTEGRATION_SHA256:
        raise AssertionError(
            "pinned vitaGL recipe integration header drifted: "
            f"{integration_sha} != {SHADER_CACHE_INTEGRATION_SHA256}"
        )
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
    prove_observer_recipe_identity()
    prove_host_oracles()
    print("Isaac vitaGL shader-cache/static loading contract: PASS")


if __name__ == "__main__":
    main()
