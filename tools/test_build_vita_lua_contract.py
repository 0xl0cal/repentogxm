#!/usr/bin/env python3
"""Focused hostile oracle for the opt-in Lua wrapper contract."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BUILD_VITA = ROOT / "tools" / "build_vita.py"


def load_build_vita():
    spec = importlib.util.spec_from_file_location(
        "build_vita_lua_contract_test", BUILD_VITA
    )
    if spec is None or spec.loader is None:
        raise AssertionError("cannot load tools/build_vita.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def options(lua: bool, lua_source: Path | None) -> dict[str, object]:
    return {
        "heap_mb": 81,
        "kage": True,
        "loading_specialist": False,
        "audio": False,
        "openal_pool": True,
        "io_profile": False,
        "archive_file_cache": False,
        "archive_validation_skip": False,
        "archive_validation_receipt": False,
        "fios_cache": False,
        "continue_profile": False,
        "png_decode_profile": False,
        "png_native_unfilter": True,
        "texture_churn_profile": False,
        "game_log_batch": False,
        "continue_overlay": False,
        "texel_oom_diagnostic": True,
        "texel_scratch": True,
        "texture_align8_policy": False,
        "anm2_scratch": True,
        "fxlayers_null_rollback": False,
        "fxray_alpha_mask": True,
        "heap_ledger_memblock": True,
        "heap_ledger_backshift": True,
        "heap_overflow_mspace": True,
        "room_entry_slab": True,
        "room_entry_hybrid": True,
        "source_date_epoch": 1_700_000_000,
        "release": False,
        "exit_menu_profile": False,
        "lua": lua,
        "lua_source": lua_source,
    }


def cache_from_command(command: list[str]) -> dict[str, str]:
    cache = {"CMAKE_GENERATOR": "focused-lua-oracle"}
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


def must_fail(module, label: str, callback, needle: str) -> None:
    try:
        callback()
    except module.BuildError as exc:
        if needle not in str(exc):
            raise AssertionError(f"{label}: wrong failure: {exc}") from exc
    else:
        raise AssertionError(f"{label}: hostile fixture passed")


def main(argv: list[str] | None = None) -> int:
    argument_parser = argparse.ArgumentParser(description=__doc__)
    argument_parser.add_argument(
        "--lua-source", type=Path,
        help="optional pristine lua-5.3.3 root for the full 58-file check",
    )
    arguments = argument_parser.parse_args(argv)
    module = load_build_vita()

    wrapper_parser = module._argument_parser()
    base = ["--pe", "fixture", "--vitasdk", "sdk"]
    defaults = wrapper_parser.parse_args(base)
    assert defaults.lua is False and defaults.lua_source is None
    explicit_off = wrapper_parser.parse_args([*base, "--no-lua"])
    assert explicit_off.lua is False and explicit_off.lua_source is None
    must_fail(
        module, "Lua without source",
        lambda: module._validate_arguments(
            wrapper_parser.parse_args([*base, "--lua"])
        ),
        "--lua requires --lua-source",
    )
    must_fail(
        module, "source while OFF",
        lambda: module._validate_arguments(
            wrapper_parser.parse_args([*base, "--lua-source", "fixture"])
        ),
        "--lua-source is only valid with --lua",
    )
    must_fail(
        module, "relative source",
        lambda: module._lua_source_option_value(
            True, Path("relative/lua-5.3.3")
        ),
        "absolute",
    )

    with tempfile.TemporaryDirectory(
        prefix="isaac-vita-lua-contract-"
    ) as value:
        root = Path(value).resolve()
        sdk = root / "sdk"
        pe = root / "isaac-ng.exe.unpacked.exe"
        generated = root / "generated"
        build = root / "build"
        generated.mkdir()
        pe.write_bytes(b"focused Lua contract\n")
        lua_source = (
            arguments.lua_source.expanduser().resolve()
            if arguments.lua_source is not None else root / "lua-5.3.3"
        )

        commands: dict[bool, list[str]] = {}
        configurations: dict[bool, dict[str, object]] = {}
        caches: dict[bool, dict[str, str]] = {}
        for enabled in (False, True):
            selected_source = lua_source if enabled else None
            command = module.cmake_configure_command(
                "cmake",
                sdk,
                sdk / "share" / "vita.toolchain.cmake",
                pe,
                generated,
                build,
                **options(enabled, selected_source),
            )
            flag = f"-DISAAC_VITA_LUA={'ON' if enabled else 'OFF'}"
            source_flag = (
                f"-DISAAC_LUA53_SOURCE_DIR={lua_source.as_posix()}"
                if enabled else "-DISAAC_LUA53_SOURCE_DIR="
            )
            assert command.count(flag) == 1
            assert command.count(source_flag) == 1
            cache = cache_from_command(command)
            configuration = module.validate_cmake_contract(
                cache,
                pe_path=pe,
                generated_dir=generated,
                **options(enabled, selected_source),
            )
            assert configuration["features"]["ISAAC_VITA_LUA"] is enabled
            if enabled:
                assert configuration["lua53_source"] == {
                    "version": module.LUA53_VERSION,
                    "archive_sha256": module.LUA53_ARCHIVE_SHA256,
                    "manifest_sha256": module.LUA53_CMAKE_MANIFEST_SHA256,
                    "source_count": module.LUA53_SOURCE_COUNT,
                    "source_set_sha256": module._lua53_source_set_sha256(
                        module._load_lua53_pinned_records()
                    ),
                }
            else:
                assert configuration["lua53_source"] is None
            commands[enabled] = command
            configurations[enabled] = configuration
            caches[enabled] = cache

        assert commands[False] != commands[True]
        assert configurations[False] != configurations[True]
        assert module._receipt_identity({
            "configuration": configurations[False]
        }) != module._receipt_identity({
            "configuration": configurations[True]
        })
        must_fail(
            module, "stale OFF cache reused as ON",
            lambda: module.validate_cmake_contract(
                caches[False], pe_path=pe, generated_dir=generated,
                **options(True, lua_source),
            ),
            "ISAAC_VITA_LUA",
        )
        must_fail(
            module, "stale ON cache reused as OFF",
            lambda: module.validate_cmake_contract(
                caches[True], pe_path=pe, generated_dir=generated,
                **options(False, None),
            ),
            "ISAAC_VITA_LUA",
        )
        retained = dict(caches[False])
        retained["ISAAC_LUA53_SOURCE_DIR"] = lua_source.as_posix()
        must_fail(
            module, "OFF cache retained Lua root",
            lambda: module.validate_cmake_contract(
                retained, pe_path=pe, generated_dir=generated,
                **options(False, None),
            ),
            "retained ISAAC_LUA53_SOURCE_DIR",
        )
        changed = dict(caches[True])
        changed["ISAAC_LUA53_SOURCE_DIR"] = (root / "other-lua").as_posix()
        must_fail(
            module, "ON cache changed Lua root",
            lambda: module.validate_cmake_contract(
                changed, pe_path=pe, generated_dir=generated,
                **options(True, lua_source),
            ),
            "changed ISAAC_LUA53_SOURCE_DIR",
        )

        hostile = root / "hostile" / "src"
        hostile.mkdir(parents=True)
        payload = b"known pristine fixture\n"
        fixture_records = (("fixture.c", hashlib.sha256(payload).hexdigest()),)
        must_fail(
            module, "missing source",
            lambda: module._validate_lua53_source_records(
                hostile.parent, fixture_records
            ),
            "source is missing or symlinked: fixture.c",
        )
        (hostile / "fixture.c").write_bytes(b"wrong fixture\n")
        must_fail(
            module, "wrong source hash",
            lambda: module._validate_lua53_source_records(
                hostile.parent, fixture_records
            ),
            "source hash mismatch for fixture.c",
        )

        full = "synthetic hostile checks"
        if arguments.lua_source is not None:
            verified = module.validate_lua53_source(lua_source)
            dependencies = module._lua53_dependency_records(lua_source)
            assert len(verified) == module.LUA53_SOURCE_COUNT
            assert len(dependencies) == module.LUA53_SOURCE_COUNT + 1
            full = "full pinned 58-file source check"

    print(
        "Vita Lua wrapper default/OFF + configure/cache/configuration/receipt "
        f"identity + {full}: PASS"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
