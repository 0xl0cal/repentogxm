#!/usr/bin/env python3
"""Regenerate the RT patch chain and execute its actual pressure/fallback seams."""

from __future__ import annotations

import argparse
import tempfile
import subprocess
from pathlib import Path

from test_vitagl_stock_patch_chain import (
    SOURCE_ARCHIVE_SHA256, apply_patch, materialize_stock, run, sha256,
)


def function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock-tar", required=True, type=Path)
    parser.add_argument("--cc", required=True)
    args = parser.parse_args()
    assert sha256(args.stock_tar) == SOURCE_ARCHIVE_SHA256
    vita = Path(__file__).resolve().parent
    recipe = vita / "vitagl-stock-reference"
    policy = recipe / "isaac_fbo_rt_reuse.c"
    with tempfile.TemporaryDirectory(prefix="isaac-rt-lease-") as value:
        temporary = Path(value)
        for profile in (0, 1):
            source = materialize_stock(temporary / f"profile{profile}", args.stock_tar, None)
            patches = ["0001-deterministic-build-and-init-oob.patch",
                       "0002-exact-gpu-draw-optimizations.patch"]
            if profile:
                patches += ["0005-isaac-scene-timer.patch", "0006-isaac-scene-split.patch"]
            patches += ["0007-isaac-fbo-rt-scenes.patch"]
            if profile:
                patches += ["0020-isaac-native-resource-profile.patch"]
            patches += ["0021-isaac-fbo-rt-reuse.patch"]
            for patch in patches:
                apply_patch(source, recipe / patch)
            common = [args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                      "-DHAVE_SINGLE_THREADED_GC=1", "-I", str(recipe)]
            if profile:
                common += ["-DHAVE_ISAAC_NATIVE_RESOURCE_PROFILE=1"]
            # OFF uses the same actual policy, not a copied baseline model.
            executable = source / "policy-off.exe"
            run([*common, str(vita / "host_tests/vita_fbo_rt_reuse_test.c"),
                 str(policy), "-o", str(executable)])
            print(run([str(executable)]).decode().strip())
            if profile:
                executable = source / "policy-profile-boundaries.exe"
                run([*common, "-DISAAC_FBO_RT_REUSE_PROFILE_TEST=1",
                     str(vita / "host_tests/vita_fbo_rt_reuse_test.c"), "-o", str(executable)])
                print(run([str(executable)]).decode().strip())
            assert "RecoveryBegin" not in (source / "source/gxm.c").read_text()
            def emit_native():
                gxm = (source / "source/gxm.c").read_text()
                gpu = (source / "source/utils/gpu_utils.c").read_text()
                shared = (source / "source/shared.h").read_text()
                mem = (source / "source/utils/mem_utils.h").read_text()
                # Bind the reduced bucket model to actual native chronology.
                gc = function(gxm, "int garbage_collector(")
                assert gc.index("sceGxmDestroyRenderTarget(") < gc.index("isaacFboRtCollect(frame_purge_clean_idx)")
                assert gc.index("isaacFboRtCollect(frame_purge_clean_idx)") < gc.index("frame_purge_clean_idx =")
                assert "frame_purge_idx =" in gc and "frame_purge_idx" in shared
                assert "frame_purge_idx" in mem and "mark_rt_as_dirty" in mem
                start = gxm.index("int r = setup_render_target(&active_write_fb->target,")
                stop = gxm.index("#ifdef LOG_ERRORS", start)
                native = ""
                if "void vglIsaacFboRtDrainAfterFinish(void)" in gxm:
                    native += "#if defined(HAVE_ISAAC_FBO_RT_REUSE_LEASE) && HAVE_ISAAC_FBO_RT_REUSE_LEASE\n"
                    native += function(gxm, "void vglIsaacFboRtDrainAfterFinish(void)") + "#endif\n"
                native += "static int unsafe_allocator_counter;\n"
                for kind in ("cpu", "gpu"):
                    native += function(gpu, f"void *gpu_alloc_mapped_aligned_unsafe_for_{kind}(")
                native += "static int native_create(void) {\n" + gxm[start:stop] + "return r;\n}\n"
                (source / "native_rt_lease.h").write_text(native, encoding="utf-8")

            def native_leg(name, flags, source_policy=policy):
                executable = source / f"native-{name}.exe"
                run([*common, *flags, "-I", str(source),
                     str(vita / "host_tests/vita_fbo_rt_lease_native_test.c"),
                     str(source_policy), "-o", str(executable)])
                return executable

            emit_native()
            baseline = run([str(native_leg("original", []))]).decode().strip()
            print("original native:", baseline)
            apply_patch(source, recipe / "0024-isaac-fbo-rt-bounded-lease.patch")
            emit_native()
            for name, flags in (("off", []), ("zero", ["-DHAVE_ISAAC_FBO_RT_REUSE_LEASE=0"])):
                result = run([str(native_leg(name, flags))]).decode().strip()
                assert result == baseline, (name, result, baseline)
                print(name, "native original Finish/GC/retry counts:", result)
            on = ["-DHAVE_ISAAC_FBO_RT_REUSE_LEASE=1"]
            for kind in ("lease", "lease_native"):
                executable = source / f"policy-{kind}.exe"
                run([*common, *on, "-I", str(source),
                     str(vita / f"host_tests/vita_fbo_rt_{kind}_test.c"),
                     *([str(policy)] if kind == "lease_native" else []),
                     "-o", str(executable)])
                print(run([str(executable)]).decode().strip())
            # The old b1 early-detach rule must fail the independent mark0 /
            # clean1 open-scene first-success retry, not merely another assertion.
            text = policy.read_text()
            guard = "if (!rt_reuse.recovery_depth || !rt_reuse.spare.target ||\n            !rt_reuse.lease_started)"
            assert text.count(guard) == 1
            mutant = source / "old_early_take.c"
            mutant.write_text(text.replace(guard, "if (!rt_reuse.recovery_depth)"), encoding="utf-8")
            bad = subprocess.run([str(native_leg("old-early-take", on, mutant))], capture_output=True, text=True)
            assert bad.returncode != 0 and "deadline_seen[target->id]" in bad.stderr, bad
            print("old early-Take negative fixture: rejected at independent retirement deadline")
            native_header = source / "native_rt_lease.h"
            guarded_header = native_header.read_text()
            assert guarded_header.count("if (isaacFboRtHasMatureSpare())") == 1
            native_header.write_text(guarded_header.replace("if (isaacFboRtHasMatureSpare())", "if (1)"), encoding="utf-8")
            bad = subprocess.run([str(native_leg("old-create-finish", on))], capture_output=True, text=True)
            native_header.write_text(guarded_header, encoding="utf-8")
            assert bad.returncode != 0 and "finishes == expected_finish" in bad.stderr, bad
            print("old unconditional create-Finish negative fixture: rejected before original deadline")
            print(f"RT lease fresh stock + profile={profile}: patch and native seams PASS")

        # Current complete functional recipe, including metadata0026 and SDK0025:
        # source composition only; no broad SDK/native execution claim.
        composed = materialize_stock(temporary / "composed", args.stock_tar, None)
        by_number = {int(p.name[:4]): p for p in recipe.glob("[0-9][0-9][0-9][0-9]-*.patch")}
        for number in (1, 2, 3, 8, 4, 12, 13, 14, 15, 5, 6, 7, 10, 9, 11, 16, 17, 18, 20, 21, 24, 22, 23, 26, 25):
            apply_patch(composed, by_number[number])
        assert sha256(composed / "source/custom_shaders.c") == "1a432f2a70ae62acf2b50d00cf61170c29daaa0004543c285c310224d1cc0989"
        print("current metadata + SDK + repaired RT full source composition: PASS")


if __name__ == "__main__":
    main()
