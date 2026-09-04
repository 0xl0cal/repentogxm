#!/usr/bin/env python3
"""Focused oracle for the Vita audio configure/cache/receipt contract."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BUILD_VITA = ROOT / "tools" / "build_vita.py"


def load_build_vita():
    spec = importlib.util.spec_from_file_location(
        "build_vita_audio_contract_test", BUILD_VITA
    )
    if spec is None or spec.loader is None:
        raise AssertionError("cannot load tools/build_vita.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_options(audio: bool) -> dict[str, object]:
    return {
        "heap_mb": 81,
        "kage": True,
        "loading_specialist": False,
        "audio": audio,
        "openal_pool": True,
        "io_profile": False,
        "archive_file_cache": True,
        "archive_validation_skip": True,
        "archive_validation_receipt": True,
        "fios_cache": True,
        "crt_seek_shadow": False,
        "continue_profile": False,
        "png_decode_profile": False,
        "png_native_unfilter": True,
        "texture_churn_profile": False,
        "game_log_batch": True,
        "continue_overlay": False,
        "texel_oom_diagnostic": True,
        "texel_scratch": True,
        "texture_align8_policy": True,
        "anm2_scratch": True,
        "fxlayers_null_rollback": True,
        "fxray_alpha_mask": True,
        "heap_ledger_memblock": True,
        "heap_ledger_backshift": True,
        "heap_overflow_mspace": True,
        "room_entry_slab": True,
        "room_entry_hybrid": True,
        "source_date_epoch": 1_700_000_000,
        "release": False,
        "exit_menu_profile": False,
    }


def cache_from_command(command: list[str]) -> dict[str, str]:
    cache = {"CMAKE_GENERATOR": "focused-audio-oracle"}
    for argument in command:
        if not argument.startswith("-D") or "=" not in argument:
            continue
        key, value = argument[2:].split("=", 1)
        if ":" in key:
            key = key.rsplit(":", 1)[0]
        if key in cache:
            raise AssertionError(f"duplicate CMake cache key: {key}")
        cache[key] = value
    return cache


def expect_cache_mismatch(module, cache: dict[str, str], *,
                          pe: Path, generated: Path,
                          expected_audio: bool) -> None:
    try:
        module.validate_cmake_contract(
            cache,
            pe_path=pe,
            generated_dir=generated,
            **build_options(expected_audio),
        )
    except module.BuildError as exc:
        if "ISAAC_VITA_AUDIO" not in str(exc):
            raise AssertionError(f"wrong stale-audio-cache failure: {exc}") from exc
    else:
        raise AssertionError("stale ISAAC_VITA_AUDIO cache was accepted")


def main() -> int:
    module = load_build_vita()
    with tempfile.TemporaryDirectory(
            prefix="isaac-vita-audio-contract-") as value:
        root = Path(value).resolve()
        sdk = root / "sdk"
        pe = root / "isaac-ng.exe.unpacked.exe"
        generated = root / "generated"
        build = root / "build"
        generated.mkdir()
        pe.write_bytes(b"focused audio contract\n")

        commands: dict[bool, list[str]] = {}
        configurations: dict[bool, dict[str, object]] = {}
        caches: dict[bool, dict[str, str]] = {}
        for audio in (False, True):
            command = module.cmake_configure_command(
                "cmake",
                sdk,
                sdk / "share" / "vita.toolchain.cmake",
                pe,
                generated,
                build,
                **build_options(audio),
            )
            expected = f"-DISAAC_VITA_AUDIO={'ON' if audio else 'OFF'}"
            opposite = f"-DISAAC_VITA_AUDIO={'OFF' if audio else 'ON'}"
            assert command.count(expected) == 1
            assert opposite not in command
            assert command.count("-DISAAC_VITA_OPENAL_POOL=ON") == 1
            cache = cache_from_command(command)
            configuration = module.validate_cmake_contract(
                cache,
                pe_path=pe,
                generated_dir=generated,
                **build_options(audio),
            )
            assert configuration["features"]["ISAAC_VITA_AUDIO"] is audio
            assert configuration["features"]["ISAAC_VITA_OPENAL_POOL"] is True
            assert configuration["audio_active"] is audio
            commands[audio] = command
            caches[audio] = cache
            configurations[audio] = configuration

        assert commands[False] != commands[True]
        assert configurations[False] != configurations[True]
        assert module._receipt_identity({
            "configuration": configurations[False]
        }) != module._receipt_identity({
            "configuration": configurations[True]
        })
        expect_cache_mismatch(
            module, caches[False], pe=pe, generated=generated,
            expected_audio=True,
        )
        expect_cache_mismatch(
            module, caches[True], pe=pe, generated=generated,
            expected_audio=False,
        )

    parser = module._argument_parser()
    incompatible = parser.parse_args([
        "--pe", "fixture", "--vitasdk", "sdk",
        "--texture-churn-profile",
    ])
    try:
        module._validate_arguments(incompatible)
    except module.BuildError as exc:
        assert "--texture-churn-profile requires --no-audio" in str(exc)
    else:
        raise AssertionError("texture-churn diagnostic accepted audio")

    print(
        "Vita audio configure/cache/receipt ON/OFF + "
        "texture-churn audio-off contract: PASS"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
