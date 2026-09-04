#!/usr/bin/env python3
"""Hostile self-test for selected stock-vitaGL manifest provenance."""

from __future__ import annotations

import hashlib
import importlib.util
import os
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BUILD_VITA_PATH = ROOT / "tools" / "build_vita.py"
STOCK_SOURCE = ROOT / "recomp" / "vita" / "vitagl-stock-reference"
SOURCE_COMMIT = "73dd57a8857f89f2353881c6de5891959c5c1983"
SOURCE_SHA256 = (
    "f484dd9d2aec707ac5f91352c6e0631239f2330fe6769e8476cfe425331c400a"
)
OOB_FIX_COMMIT = "f24ad3e66f7f34bebe70598d1302c34f4eff4a54"
MODE_SPECS = (
    ("ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS",
     "gpu_draw_optimizations", "HAVE_ISAAC_GPU_DRAW_OPTIMIZATIONS=1"),
    ("ISAAC_VITA_VITAGL_SHADER_CACHE",
     "shader_cache", "HAVE_ISAAC_SHADER_CACHE=1"),
    ("ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY",
     "canonical_quad_zero_copy", "HAVE_ISAAC_CANONICAL_QUAD_ZERO_COPY=1"),
    ("ISAAC_VITA_GXM_STATE_SHADOW",
     "gxm_state_shadow", "HAVE_ISAAC_GXM_STATE_SHADOW=1"),
    ("ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS",
     "coloroffset_gpu_optimizations",
     "HAVE_ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS=1"),
    ("ISAAC_VITA_PHASE_PROFILE",
     "phase_profile", "HAVE_ISAAC_PHASE_PROFILE=1"),
)


def load_build_vita():
    spec = importlib.util.spec_from_file_location(
        "build_vita_stock_dependency_test", BUILD_VITA_PATH
    )
    if spec is None or spec.loader is None:
        raise AssertionError("cannot load tools/build_vita.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_ascii_lf(path: Path, text: str) -> None:
    path.write_bytes(text.encode("ascii"))


def write_artifact_files(root: Path, prefix: bytes) -> tuple[Path, Path]:
    library = root / "prefix" / "lib" / "libvitaGL.a"
    header = root / "prefix" / "include" / "vitaGL.h"
    library.parent.mkdir(parents=True, exist_ok=True)
    header.parent.mkdir(parents=True, exist_ok=True)
    library.write_bytes(b"!<arch>\n" + prefix + b"-lib")
    header.write_bytes(b"/* " + prefix + b"-header */\n")
    (root / "libvitaGL.sha256").write_text(
        f"{sha256(library)}  {library}\n", encoding="ascii"
    )
    (root / "vitaGL.h.sha256").write_text(
        f"{sha256(header)}  {header}\n", encoding="ascii"
    )
    return library, header


def write_overlay(root: Path) -> None:
    write_artifact_files(root, b"overlay")
    (root / "recipe.txt").write_text("a" * 64 + "\n", encoding="ascii")
    (root / "build-contract.txt").write_text(
        "flags=fixture softfp single-threaded-gc shared-rt-1\n",
        encoding="ascii",
    )


def stock_contract(modes: dict[str, bool]) -> tuple[dict[str, str], str]:
    files = {
        path.name: path
        for path in STOCK_SOURCE.iterdir()
        if path.name in {
            "build.sh",
            "0001-deterministic-build-and-init-oob.patch",
            "0002-exact-gpu-draw-optimizations.patch",
            "isaac_gpu_draw_policy.h",
            "isaac_gxm_state_policy.h",
            "0003-exact-coloroffset-gpu-optimizations.patch",
            "isaac_coloroffset_gpu_policy.h",
            "0004-hardened-custom-shader-cache.patch",
            "isaac_shader_cache_policy.h",
            "isaac_shader_cache_vitagl.h",
            "isaac_shader_cache_block_list.h",
        }
    }
    flags = [
        "SOFTFP_ABI=1", "NO_DEBUG=1", "NO_SPLASHSCREEN=1",
        "SINGLE_THREADED_GC=1",
    ]
    values: dict[str, str] = {}
    for cache_key, contract_key, flag in MODE_SPECS:
        values[contract_key] = "1" if modes[cache_key] else "0"
        if modes[cache_key]:
            flags.append(flag)
    contract = {
        "profile": "stock-vitagl-reference",
        "source_commit": SOURCE_COMMIT,
        "source_sha256": SOURCE_SHA256,
        "backported_oob_fix_commit": OOB_FIX_COMMIT,
        "patch_sha256": sha256(
            files["0001-deterministic-build-and-init-oob.patch"]
        ),
        "script_sha256": sha256(files["build.sh"]),
        "flags": " ".join(flags),
        "gpu_draw_optimizations": values["gpu_draw_optimizations"],
        "gpu_draw_patch_sha256": sha256(
            files["0002-exact-gpu-draw-optimizations.patch"]
        ),
        "gpu_draw_policy_sha256": sha256(files["isaac_gpu_draw_policy.h"]),
        "gpu_draw_policy": (
            "exact-single-stream-layout-cache-draw-stats-v2"
            if values["gpu_draw_optimizations"] == "1" else "disabled"
        ),
        "canonical_quad_zero_copy": values["canonical_quad_zero_copy"],
        "gxm_state_shadow": values["gxm_state_shadow"],
        "gxm_state_policy_sha256": sha256(files["isaac_gxm_state_policy.h"]),
        "coloroffset_gpu_optimizations": values[
            "coloroffset_gpu_optimizations"
        ],
        "coloroffset_patch_sha256": sha256(
            files["0003-exact-coloroffset-gpu-optimizations.patch"]
        ),
        "coloroffset_policy_sha256": sha256(
            files["isaac_coloroffset_gpu_policy.h"]
        ),
        "coloroffset_policy": (
            "exact-stock-source-plus-opaque-noblend-v3"
            if values["coloroffset_gpu_optimizations"] == "1" else "disabled"
        ),
        "phase_profile": values["phase_profile"],
        "shader_cache": values["shader_cache"],
        "shader_cache_patch_sha256": sha256(
            files["0004-hardened-custom-shader-cache.patch"]
        ),
        "shader_cache_policy_sha256": sha256(
            files["isaac_shader_cache_policy.h"]
        ),
        "shader_cache_integration_sha256": sha256(
            files["isaac_shader_cache_vitagl.h"]
        ),
        "shader_cache_block_list_sha256": sha256(
            files["isaac_shader_cache_block_list.h"]
        ),
        "shader_cache_root": "ux0:data/isaacr001/shader-cache/v2",
        "shared_render_targets": "off",
        "gxm_source_patch": "none",
    }
    parts = (
        contract["source_commit"], contract["source_sha256"],
        contract["patch_sha256"], contract["script_sha256"],
        contract["flags"], contract["backported_oob_fix_commit"],
        contract["gpu_draw_optimizations"],
        contract["gpu_draw_patch_sha256"],
        contract["gpu_draw_policy_sha256"],
        contract["canonical_quad_zero_copy"],
        contract["gxm_state_shadow"], contract["gxm_state_policy_sha256"],
        contract["coloroffset_gpu_optimizations"],
        contract["coloroffset_patch_sha256"],
        contract["coloroffset_policy_sha256"], contract["phase_profile"],
        contract["shader_cache"],
        contract["shader_cache_patch_sha256"],
        contract["shader_cache_policy_sha256"],
        contract["shader_cache_integration_sha256"],
        contract["shader_cache_block_list_sha256"],
    )
    recipe = hashlib.sha256(
        "".join(f"{part}\n" for part in parts).encode("ascii")
    ).hexdigest()
    return contract, recipe


def write_stock(root: Path, modes: dict[str, bool]) -> None:
    write_artifact_files(root, b"stock")
    contract, recipe = stock_contract(modes)
    write_ascii_lf(
        root / "build-contract.txt",
        "".join(f"{key}={value}\n" for key, value in contract.items()),
    )
    write_ascii_lf(root / "recipe.txt", recipe + "\n")


def expect_failure(module, cache: dict[str, str], sdk: Path,
                   build: Path, expected: str) -> None:
    try:
        module.collect_dependency_inputs(
            sdk=sdk,
            build_dir=build,
            cache=cache,
            kage_active=True,
            loading_specialist=False,
            audio_active=False,
        )
    except module.BuildError as exc:
        if expected not in str(exc):
            raise AssertionError(
                f"wrong failure for {expected!r}: {exc}"
            ) from exc
    else:
        raise AssertionError(f"hostile fixture was accepted: {expected}")


def main() -> int:
    module = load_build_vita()
    with tempfile.TemporaryDirectory(
            prefix="isaac-stock-vitagl-provenance-") as value:
        root = Path(value)
        sdk = root / "sdk"
        build = root / "build"
        overlay = root / "overlay"
        stock = root / "stock"
        (sdk / "share").mkdir(parents=True)
        (sdk / "share" / "vita.toolchain.cmake").write_text(
            "# fixture\n", encoding="ascii"
        )
        (sdk / "share" / "vita.cmake").write_text(
            "# fixture\n", encoding="ascii"
        )
        packer = sdk / "bin" / (
            "vita-pack-vpk.exe" if os.name == "nt" else "vita-pack-vpk"
        )
        packer.parent.mkdir()
        packer.write_bytes(b"fixture packer\n")
        build.mkdir()
        write_overlay(overlay)
        modes = {
            cache_key: True
            for cache_key, unused_contract, unused_flag in MODE_SPECS
        }
        write_stock(stock, modes)
        cache = {
            "VITA_PACK_VPK": str(packer),
            "ISAAC_VITA_REAL_PACK_VPK": str(packer),
            "ISAAC_VITA_VITAGL_STOCK_REFERENCE": "OFF",
            "ISAAC_VITA_VITAGL_OVERLAY_DIR": str(overlay),
            "ISAAC_VITA_VITAGL_STOCK_REFERENCE_DIR": str(stock),
            **{key: "ON" for key in modes},
        }

        ordinary_cache = dict(cache)
        ordinary_cache.pop("ISAAC_VITA_VITAGL_STOCK_REFERENCE_DIR")
        for key in modes:
            ordinary_cache.pop(key)
        ordinary = module.collect_dependency_inputs(
            sdk=sdk, build_dir=build, cache=ordinary_cache,
            kage_active=True, loading_specialist=False, audio_active=False,
        )
        ordinary_paths = {record["path"] for record in ordinary}
        assert "vitagl-overlay/libvitaGL.a" in ordinary_paths
        assert not any(path.startswith("vitagl-stock-reference/")
                       for path in ordinary_paths)

        stock_cache = dict(cache)
        stock_cache["ISAAC_VITA_VITAGL_STOCK_REFERENCE"] = "ON"
        stock_cache.pop("ISAAC_VITA_VITAGL_OVERLAY_DIR")
        selected = module.collect_dependency_inputs(
            sdk=sdk, build_dir=build, cache=stock_cache,
            kage_active=True, loading_specialist=False, audio_active=False,
        )
        selected_paths = {record["path"] for record in selected}
        assert {
            "vitagl-stock-reference/recipe.txt",
            "vitagl-stock-reference/build-contract.txt",
            "vitagl-stock-reference/libvitaGL.a",
            "vitagl-stock-reference/vitaGL.h",
        }.issubset(selected_paths)
        assert set(module.VITAGL_STOCK_REFERENCE_INPUTS).issubset(selected_paths)
        assert not any(path.startswith("vitagl-overlay/")
                       for path in selected_paths)

        disabled_modes = {key: False for key in modes}
        write_stock(stock, disabled_modes)
        disabled_cache = dict(stock_cache)
        disabled_cache.update({key: "OFF" for key in disabled_modes})
        disabled = module.collect_dependency_inputs(
            sdk=sdk, build_dir=build, cache=disabled_cache,
            kage_active=True, loading_specialist=False, audio_active=False,
        )
        assert "vitagl-stock-reference/libvitaGL.a" in {
            record["path"] for record in disabled
        }

        mode_vectors: dict[str, dict[str, bool]] = {}
        for cache_key in modes:
            vector = {key: False for key in modes}
            vector[cache_key] = True
            if cache_key in {
                "ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY",
                "ISAAC_VITA_GXM_STATE_SHADOW",
                "ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS",
            }:
                vector["ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS"] = True
            mode_vectors[cache_key] = vector
        for cache_key, vector in mode_vectors.items():
            write_stock(stock, vector)
            vector_cache = dict(stock_cache)
            vector_cache.update({
                key: "ON" if enabled else "OFF"
                for key, enabled in vector.items()
            })
            vector_records = module.collect_dependency_inputs(
                sdk=sdk, build_dir=build, cache=vector_cache,
                kage_active=True, loading_specialist=False,
                audio_active=False,
            )
            assert "vitagl-stock-reference/build-contract.txt" in {
                record["path"] for record in vector_records
            }, cache_key

        missing_root = dict(stock_cache)
        missing_root.pop("ISAAC_VITA_VITAGL_STOCK_REFERENCE_DIR")
        expect_failure(
            module, missing_root, sdk, build,
            "ISAAC_VITA_VITAGL_STOCK_REFERENCE_DIR",
        )

        write_stock(stock, modes)
        contract_path = stock / "build-contract.txt"
        contract_text = contract_path.read_text(encoding="ascii")
        write_ascii_lf(
            contract_path,
            contract_text.replace(
                f"source_commit={SOURCE_COMMIT}",
                f"source_commit={'0' * 40}",
            ),
        )
        expect_failure(module, stock_cache, sdk, build, "source_commit")

        write_stock(stock, modes)
        contract_text = contract_path.read_text(encoding="ascii")
        write_ascii_lf(
            contract_path,
            contract_text.replace(
                "flags=SOFTFP_ABI=1 NO_DEBUG=1",
                "flags=SOFTFP_ABI=1 NO_DEBUG=0",
            ),
        )
        expect_failure(module, stock_cache, sdk, build, "flags")

        write_stock(stock, modes)
        (stock / "libvitaGL.sha256").write_text(
            "0" * 64 + "  hostile\n", encoding="ascii"
        )
        expect_failure(
            module, stock_cache, sdk, build,
            "checksum does not match libvitaGL.a",
        )

        write_stock(stock, modes)
        write_ascii_lf(contract_path, "malformed\n")
        expect_failure(module, stock_cache, sdk, build, "malformed record")

        write_stock(stock, modes)
        with contract_path.open("a", encoding="ascii", newline="") as handle:
            handle.write("flags=duplicate\n")
        expect_failure(module, stock_cache, sdk, build, "repeats key 'flags'")

        for unused_cache_key, contract_key, unused_flag in MODE_SPECS:
            write_stock(stock, modes)
            contract_text = contract_path.read_text(encoding="ascii")
            old = f"{contract_key}=1\n"
            if contract_text.count(old) != 1:
                raise AssertionError(
                    f"hostile mode fixture has no unique {contract_key}"
                )
            write_ascii_lf(
                contract_path,
                contract_text.replace(old, f"{contract_key}=0\n"),
            )
            expect_failure(module, stock_cache, sdk, build, contract_key)

        write_stock(stock, modes)
        write_ascii_lf(stock / "recipe.txt", "0" * 64 + "\n")
        expect_failure(
            module, stock_cache, sdk, build,
            "recipe differs from selected source/modes",
        )

    print(
        "build_vita stock dependency provenance self-test: PASS; "
        "ordinary/stock-all-OFF/Bundle5-all-ON/six-mode-vectors/root/"
        "commit/flags/checksum/malformed/duplicate/six-mode-hostile/recipe"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
