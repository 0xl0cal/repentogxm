#!/usr/bin/env python3
"""Fail-closed Vita raw-C-allocator source/object/link closure gate."""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
from typing import Iterable, Mapping, Sequence


TARGET = "isaac_first_arm_fault"
SCHEMA = 1
LOGICAL_OBJECT_PREFIX = "objects/"
DIRECT_DEFAULT_CACHE_KEY = "ISAAC_VITA_DIRECT_DEFAULT"
STOCK_REFERENCE_CACHE_KEY = "ISAAC_VITA_VITAGL_STOCK_REFERENCE"
DISPLAY_RASTER_CACHE_KEY = "ISAAC_VITA_DISPLAY_RASTER_720"
TYPED_STATE_CACHE_KEY = "ISAAC_VITA_GL_TYPED_STATE_CACHE"
GAME_LOG_BATCH_CACHE_KEY = "ISAAC_VITA_GAME_LOG_BATCH"
ARCHIVE_VALIDATION_RECEIPT_CACHE_KEY = (
    "ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT"
)
RENDERFRAME_FASTPATH_CACHE_KEY = "ISAAC_VITA_RENDERFRAME_FASTPATH"
CONTINUE_PROFILE_CACHE_KEY = "ISAAC_VITA_CONTINUE_PROFILE"
EXIT_MENU_PROFILE_CACHE_KEY = "ISAAC_VITA_EXIT_MENU_PROFILE"
PNG_DECODE_PROFILE_CACHE_KEY = "ISAAC_VITA_PNG_DECODE_PROFILE"
PNG_NATIVE_UNFILTER_CACHE_KEY = "ISAAC_VITA_PNG_NATIVE_UNFILTER"
PNG_INFLATE_FLUSH_FASTPATH_CACHE_KEY = (
    "ISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH"
)
PNG_CRC32_FASTPATH_CACHE_KEY = "ISAAC_VITA_PNG_CRC32_FASTPATH"
ARCHIVE_MINIZ_FASTPATH_CACHE_KEY = "ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH"
LUA_CACHE_KEY = "ISAAC_VITA_LUA"
LUA_SOURCE_DIR_CACHE_KEY = "ISAAC_LUA53_SOURCE_DIR"
LUA_TARGET = "isaac_lua53"
LUA_ARCHIVE_NAME = "libisaac_lua53.a"
LUA_VERSION = "5.3.3"
LUA_SOURCE_COUNT = 58
LUA_C_SOURCE_COUNT = 33
LUA_MANIFEST_SHA256 = (
    "27977cf039f0855b98840dc77a360a7aaa9d52a5e5780ec031aad08b1794054c"
)
LUA_LOGICAL_SOURCE_PREFIX = f"lua-{LUA_VERSION}/src"
LUA_REQUIRED_COMPILE_OPTIONS = (
    "-Wl,-q", "-O3", "-DNDEBUG", "-std=gnu11", "-O2",
    "-mcpu=cortex-a9", "-mfpu=neon", "-mfloat-abi=softfp", "-mthumb",
    "-fno-strict-aliasing", "-ffunction-sections", "-fdata-sections",
    "-Wall", "-Wextra",
)
LUA_REQUIRED_FINAL_SYMBOL = "lua_newstate"
LUA_DISCARDED_ALLOCATOR_SYMBOLS = frozenset(("l_alloc", "luaL_newstate"))
SAVE_CHECKSUM_FASTPATH_CACHE_KEY = "ISAAC_VITA_SAVE_CHECKSUM_FASTPATH"
SAVE_READER_FASTPATH_CACHE_KEY = "ISAAC_VITA_SAVE_READER_FASTPATH"
SAVE_READ32_FUSED_CACHE_KEY = "ISAAC_VITA_SAVE_READ32_FUSED"
SAVE_READER_DIRECT_EDGES_CACHE_KEY = "ISAAC_VITA_SAVE_READER_DIRECT_EDGES"
SYNC_IMPORT_FASTPATH_CACHE_KEY = "ISAAC_VITA_SYNC_IMPORT_FASTPATH"
MEMSET_THUNK_FASTPATH_CACHE_KEY = "ISAAC_VITA_MEMSET_THUNK_FASTPATH"
KAGE_MUTEX_SEAM_CACHE_KEY = "ISAAC_VITA_KAGE_MUTEX_SEAM"
KAGE_MUTEX_SEAM_ROOTS = (
    ("void sub_00562e00(CPU *__restrict c)",
     "KAGE Mutex::Lock(-1) native critical-section seam"),
    ("void sub_00562ec0(CPU *__restrict c)",
     "KAGE Mutex::Unlock native critical-section seam"),
)
KAGE_REFCOUNT_SEAM_CACHE_KEY = "ISAAC_VITA_KAGE_REFCOUNT_SEAM"
KAGE_REFCOUNT_SEAM_ROOTS = (
    ("void sub_00007af0(CPU *__restrict c)",
     "KAGE ReferenceCount::Release native seam"),
    ("void sub_00007b50(CPU *__restrict c)",
     "KAGE ReferenceCount::AddRef native seam"),
    ("void sub_00007b70(CPU *__restrict c)",
     "KAGE ReferenceCount weak-lock native seam"),
)
FLOOR_THUNK_FASTPATH_CACHE_KEY = "ISAAC_VITA_FLOOR_THUNK_FASTPATH"
TEXTURE_CHURN_PROFILE_CACHE_KEY = "ISAAC_VITA_TEXTURE_CHURN_PROFILE"
AUDIO_STREAM_RECEIPT_CACHE_KEY = "ISAAC_VITA_AUDIO_STREAM_RECEIPT"
SIM_CADENCE_RECEIPT_CACHE_KEY = "ISAAC_VITA_SIM_CADENCE_RECEIPT"
STABLE30_PRESENTATION_CACHE_KEY = "ISAAC_VITA_STABLE_30_PRESENTATION"
STABLE30_BUILD_ID_MACRO = "ISAAC_VITA_STABLE30_BUILD_ID"
SIM_CADENCE_RECEIPT_BUILD_ID_MACRO = (
    "ISAAC_VITA_SIM_CADENCE_RECEIPT_BUILD_ID"
)
WORLD_SEAM_DIAG_CACHE_KEY = "ISAAC_VITA_WORLD_SEAM_DIAG"
WORLD_SEAM_DIAG_BUILD_ID_MACRO = "ISAAC_VITA_WORLD_SEAM_DIAG_BUILD_ID"
PNG_DECODE_PROFILE_BUILD_ID_MACRO = (
    "ISAAC_VITA_PNG_DECODE_PROFILE_BUILD_ID"
)
GUEST_LINK_ID_CACHE_KEY = "ISAAC_VITA_GUEST_LINK_ID"
DIRECT_DEFAULT_SOURCE_NAME = "guest_0144.c"
DIRECT_DEFAULT_OUTPUT_RELATIVE = Path(
    "isaac-generated-overrides/direct-default/guest_0144.c"
)
DIRECT_DEFAULT_OUTPUT_SIZE = 1_116_115
DIRECT_DEFAULT_OUTPUT_SHA256 = (
    "8e6c45d6a502d7fe4dae75e4bda3f661f748ae8952a3ec5cb8a85605968dad6f"
)
# ISAAC_VITA_RENDER_SURFACE_NATIVE selects the second derivation mode of the
# same guest_0144.c (Render Surface kept, HQX and Color Correction bypassed).
RENDER_SURFACE_NATIVE_CACHE_KEY = "ISAAC_VITA_RENDER_SURFACE_NATIVE"
RENDER_SURFACE_NATIVE_OUTPUT_SIZE = 1_116_121
RENDER_SURFACE_NATIVE_OUTPUT_SHA256 = (
    "b53d55930fbf7adba71008843591a654e0ae782a06ce057310fb2de1547511a1"
)
DIRECT_DEFAULT_DEFINITION_OWNERS = frozenset(
    ("gl_vita_backend.c", "kage_vita_generated_hooks.c")
)
STOCK_REFERENCE_DEFINITION_OWNERS = frozenset(("kage_vita_backend.c",))
DISPLAY_RASTER_DEFINITION_OWNERS = frozenset(
    ("gl_vita_backend.c", "kage_vita_backend.c")
)
TYPED_STATE_CACHE_BASE_DEFINITION_OWNERS = frozenset(("gl_vita_backend.c",))
GAME_LOG_BATCH_BASE_DEFINITION_OWNERS = frozenset(("host_vita_crt.c",))
SYNC_IMPORT_FASTPATH_DEFINITION_OWNERS = frozenset(("guest.c",))
TEXTURE_CHURN_PROFILE_DEFINITION_OWNERS = frozenset(
    ("gl_vita_backend.c", "kage_vita_phase_profile.c")
)
AUDIO_STREAM_RECEIPT_DEFINITION_OWNERS = frozenset(
    ("host_vita_audio_cooperative.c",)
)
SIM_CADENCE_RECEIPT_RUNTIME_DEFINITION_OWNERS = frozenset(
    ("kage_vita_backend.c", "kage_vita_sim_cadence_receipt.c")
)
STABLE30_RUNTIME_DEFINITION_OWNERS = frozenset((
    "kage_vita_backend.c",
    "kage_vita_generated_hooks.c",
    "kage_vita_stable30.c",
))
WORLD_SEAM_DIAG_RUNTIME_DEFINITION_OWNERS = frozenset(
    ("gl_vita_backend.c", "kage_vita_world_seam_diag.c")
)
RAW_SYMBOLS = frozenset(
    ("malloc", "calloc", "realloc", "free", "memalign",
     "aligned_alloc", "strdup")
)
# A forced include necessarily runs before directives in the translation unit.
# Reject source-local libc feature-view changes instead of silently moving them
# after <stdlib.h>/<string.h> and compiling against a different declaration set.
FEATURE_VIEW_MACROS = frozenset(
    (
        "_ATFILE_SOURCE", "_BSD_SOURCE", "_DARWIN_C_SOURCE",
        "_DEFAULT_SOURCE", "_FILE_OFFSET_BITS", "_FORTIFY_SOURCE",
        "_GNU_SOURCE", "_ISOC11_SOURCE", "_ISOC2X_SOURCE",
        "_LARGEFILE_SOURCE", "_LARGEFILE64_SOURCE", "_POSIX_SOURCE",
        "_POSIX_C_SOURCE", "_REENTRANT", "_SVID_SOURCE", "_THREAD_SAFE",
        "_TIME_BITS", "_XOPEN_SOURCE", "_XOPEN_SOURCE_EXTENDED",
    )
)
EXEMPT_RUNTIME = frozenset(
    ("guest.c", "host_vita_heap.c", "host_vita_openal_pool.c",
     "kage_vita_texture_memory.c")
)

BASE_RUNTIME = frozenset(
    (
        "guest.c", "entry_vita.c", "host_vita_first_fault.c",
        "host_vita_import_id.c", "host_vita_com.c",
        "host_vita_console.c", "host_vita_crt.c",
        "host_vita_exception.c", "host_vita_file_lock.c",
        "host_vita_filesystem.c", "host_vita_find.c", "host_vita_fls.c",
        "host_vita_gl.c", "host_vita_heap.c",
        "host_vita_ogg_emergency.c", "kage_vita_fx_rollback.c",
        "host_vita_math.c", "host_vita_sync.c", "host_vita_memory.c",
        "host_vita_post_com.c", "host_vita_rtti.c",
        "host_vita_startup.c", "host_vita_steam.c",
        "host_vita_user32.c", "vita_sync_services.c", "gl_bridge.c",
        "manual_portable.c",
    )
)
KAGE_RUNTIME = frozenset(
    (
        "gl_vita_backend.c", "host_vita_xinput.c", "kage_vita_backend.c",
        "kage_vita_input.c", "kage_vita_touch.c", "kage_vita_loading.c",
        "kage_vita_generated_hooks.c", "host_vita_audio_cooperative.c",
        "manual_kage_vita.c",
    )
)
COLOROFFSET_GPU_CACHE_KEY = "ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS"

RelocKey = tuple[str, str, str]


def _counter(items: Iterable[tuple[int, str, str, str]]) -> collections.Counter[RelocKey]:
    result: collections.Counter[RelocKey] = collections.Counter()
    for count, function, symbol, kind in items:
        result[(function, symbol, kind)] += count
    return result


LUA_EXPECTED_RAW_RELOCATIONS = _counter(
    (
        (1, "l_alloc", "free", "R_ARM_THM_CALL"),
        (1, "l_alloc", "realloc", "R_ARM_THM_JUMP24"),
    )
)


EXPECTED_FIXED: Mapping[str, collections.Counter[RelocKey]] = {
    "guest.c": _counter(
        (
            (1, "guest_fs_base", "calloc", "R_ARM_THM_CALL"),
            (1, "guest_image_free", "free", "R_ARM_THM_CALL"),
            (1, "guest_image_load", "calloc", "R_ARM_THM_CALL"),
            (19, "guest_image_load", "free", "R_ARM_THM_CALL"),
            (1, "guest_image_load", "malloc", "R_ARM_THM_CALL"),
            (1, "guest_pe_raw_alloc", "malloc", "R_ARM_THM_JUMP24"),
            (1, "guest_pe_raw_free", "free", "R_ARM_THM_JUMP24"),
            (1, "guest_stack_free", "free", "R_ARM_THM_CALL"),
            (1, "guest_stack_init", "free", "R_ARM_THM_CALL"),
            (1, "guest_stack_init", "memalign", "R_ARM_THM_CALL"),
        )
    ),
    "host_vita_openal_pool.c": _counter(
        (
            (1, "isaac_vita_openal_pool_aligned_alloc", "aligned_alloc",
             "R_ARM_THM_CALL"),
            (1, "isaac_vita_openal_pool_aligned_alloc", "aligned_alloc",
             "R_ARM_THM_JUMP24"),
            (1, "isaac_vita_openal_pool_calloc", "calloc", "R_ARM_THM_CALL"),
            (1, "isaac_vita_openal_pool_calloc", "calloc", "R_ARM_THM_JUMP24"),
            (1, "isaac_vita_openal_pool_free", "free", "R_ARM_THM_JUMP24"),
            (1, "isaac_vita_openal_pool_malloc", "malloc", "R_ARM_THM_CALL"),
            (1, "isaac_vita_openal_pool_malloc", "malloc", "R_ARM_THM_JUMP24"),
            (1, "isaac_vita_openal_pool_realloc", "realloc", "R_ARM_THM_CALL"),
            (1, "isaac_vita_openal_pool_realloc", "realloc",
             "R_ARM_THM_JUMP24"),
        )
    ),
}

EXPECTED_HEAP_CANONICAL = _counter(
    (
        (1, "vita_heap_guest_calloc_impl", "calloc", "R_ARM_THM_CALL"),
        (2, "vita_heap_guest_calloc_impl", "free", "R_ARM_THM_CALL"),
        (1, "vita_heap_guest_free_impl", "free", "R_ARM_THM_CALL"),
        (2, "vita_heap_guest_malloc_with_reason", "free", "R_ARM_THM_CALL"),
        (1, "vita_heap_guest_malloc_with_reason", "malloc", "R_ARM_THM_CALL"),
        (4, "vita_heap_guest_realloc_impl.constprop.0", "free",
         "R_ARM_THM_CALL"),
        (1, "vita_heap_guest_realloc_impl.constprop.0", "malloc",
         "R_ARM_THM_CALL"),
        (1, "vita_heap_guest_realloc_impl.constprop.0", "realloc",
         "R_ARM_THM_CALL"),
    )
)

# The default/release feature closure above is an exact ABI profile.  Legal
# non-release A/B builds can compile the same owner with newlib-backed ledger
# storage and without the overflow router, which changes inlining and the raw
# fallback edges.  Keep those builds gated by the measured union of all 88
# legal feature combinations: only these owner/function/symbol/type keys may
# remain, and no key may exceed its observed maximum.
PROFILE_CANONICAL = "canonical-exact"
PROFILE_DIAGNOSTIC = "diagnostic-allowlist"
HEAP_DIAGNOSTIC_MAX = _counter(
    (
        (1, "vita_heap_guest_calloc_impl", "calloc", "R_ARM_THM_CALL"),
        (2, "vita_heap_guest_calloc_impl", "free", "R_ARM_THM_CALL"),
        (1, "vita_heap_guest_calloc_impl.constprop.N", "calloc",
         "R_ARM_THM_CALL"),
        (1, "vita_heap_guest_calloc_impl.constprop.N", "free",
         "R_ARM_THM_CALL"),
        (1, "vita_heap_guest_free_impl", "free", "R_ARM_THM_CALL"),
        (1, "vita_heap_guest_free_impl.constprop.N", "free",
         "R_ARM_THM_CALL"),
        (2, "vita_heap_guest_malloc_with_reason", "free",
         "R_ARM_THM_CALL"),
        (1, "vita_heap_guest_malloc_with_reason", "malloc",
         "R_ARM_THM_CALL"),
        (4, "vita_heap_guest_realloc_impl.constprop.N", "free",
         "R_ARM_THM_CALL"),
        (1, "vita_heap_guest_realloc_impl.constprop.N", "malloc",
         "R_ARM_THM_CALL"),
        (1, "vita_heap_guest_realloc_impl.constprop.N", "realloc",
         "R_ARM_THM_CALL"),
        (1, "vita_heap_ledger_rehash", "calloc", "R_ARM_THM_CALL"),
        (2, "vita_heap_ledger_rehash", "free", "R_ARM_THM_CALL"),
        (2, "vita_heap_ledger_reserve", "calloc", "R_ARM_THM_CALL"),
        (2, "vita_heap_ledger_reserve", "free", "R_ARM_THM_CALL"),
        (2, "vita_heap_ledger_reserve.constprop.N", "calloc",
         "R_ARM_THM_CALL"),
        (2, "vita_heap_ledger_reserve.constprop.N", "free",
         "R_ARM_THM_CALL"),
    )
)
HEAP_DIAGNOSTIC_TOTAL_MAX = sum(EXPECTED_HEAP_CANONICAL.values())
_HEAP_CONSTPROP_BASES = frozenset(
    (
        "vita_heap_guest_calloc_impl", "vita_heap_guest_free_impl",
        "vita_heap_guest_realloc_impl", "vita_heap_ledger_reserve",
    )
)
_HEAP_MUTUALLY_EXCLUSIVE_FUNCTIONS = (
    frozenset(("vita_heap_guest_calloc_impl",
               "vita_heap_guest_calloc_impl.constprop.N")),
    frozenset(("vita_heap_guest_free_impl",
               "vita_heap_guest_free_impl.constprop.N")),
    frozenset(("vita_heap_ledger_rehash", "vita_heap_ledger_reserve",
               "vita_heap_ledger_reserve.constprop.N")),
)

EXPECTED_TEXTURE_DIAGNOSTIC = _counter(
    (
        (2, "kage_vita_texel_oom_diagnostic", "free", "R_ARM_THM_CALL"),
        (2, "kage_vita_texel_oom_diagnostic", "malloc", "R_ARM_THM_CALL"),
    )
)

HEAP_CANONICAL_DEFINES = frozenset(
    (
        "ISAAC_VITA_ANM2_SCRATCH", "ISAAC_VITA_HEAP_LEDGER_BACKSHIFT",
        "ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH",
        "ISAAC_VITA_HEAP_LEDGER_MEMBLOCK", "ISAAC_VITA_HEAP_OVERFLOW_MSPACE",
        "ISAAC_VITA_HEAP_RANGE_LEASE", "ISAAC_VITA_OGG_EMERGENCY",
        "ISAAC_VITA_ROOM_ENTRY_HYBRID", "ISAAC_VITA_ROOM_ENTRY_SLAB",
        "ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC", "ISAAC_VITA_TEXEL_SCRATCH",
    )
)


class GateError(RuntimeError):
    pass


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _file_record(path: Path) -> dict[str, object]:
    if not path.is_file():
        raise GateError(f"required regular file is missing: {path}")
    return {"size": path.stat().st_size, "sha256": _sha256(path)}


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True).encode("ascii")


def _path(value: str | os.PathLike[str], base: Path | None = None) -> Path:
    candidate = Path(value)
    if not candidate.is_absolute():
        if base is None:
            raise GateError(f"relative path has no owner directory: {value}")
        candidate = base / candidate
    return candidate.resolve(strict=False)


def read_cache(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key_type, value = line.split("=", 1)
        key = key_type.split(":", 1)[0]
        if key in result:
            raise GateError(f"duplicate CMake cache key: {key}")
        result[key] = value
    return result


def _feature(cache: Mapping[str, str], name: str) -> bool:
    try:
        value = cache[name].upper()
    except KeyError as exc:
        raise GateError(f"CMake cache is missing {name}") from exc
    if value not in ("ON", "OFF"):
        raise GateError(f"CMake cache {name} is not ON/OFF: {value!r}")
    return value == "ON"


def lua_source_records(
    lua_source: Path, manifest: Path
) -> tuple[tuple[str, str], ...]:
    """Authenticate the exact upstream 58-file Lua 5.3.3 source closure."""

    if not lua_source.is_absolute():
        raise GateError(f"Lua source root is not absolute: {lua_source}")
    if not manifest.is_absolute():
        raise GateError(f"Lua source manifest is not absolute: {manifest}")
    lua_source = lua_source.resolve()
    manifest = manifest.resolve()
    if manifest.name != "lua53_vanilla.cmake" or not manifest.is_file():
        raise GateError(f"Lua source manifest is not the canonical file: {manifest}")
    actual_manifest_sha256 = _sha256(manifest)
    if actual_manifest_sha256 != LUA_MANIFEST_SHA256:
        raise GateError(
            "Lua source manifest hash mismatch: "
            f"{actual_manifest_sha256} != {LUA_MANIFEST_SHA256}"
        )
    try:
        text = manifest.read_bytes().decode("ascii")
    except (OSError, UnicodeError) as exc:
        raise GateError(f"cannot read Lua source manifest: {exc}") from exc
    start = "set(ISAAC_LUA53_SOURCE_RECORDS\n"
    end = "\n\nset(ISAAC_LUA53_C_SOURCES)"
    if text.count(start) != 1 or text.count(end) != 1:
        raise GateError("Lua source manifest has an unexpected envelope")
    body = text.split(start, 1)[1].split(end, 1)[0]
    if not body.endswith(")"):
        raise GateError("Lua source manifest has no closing list marker")
    records: list[tuple[str, str]] = []
    for line in body[:-1].splitlines():
        match = re.fullmatch(
            r'  "([A-Za-z0-9._-]+)\|([0-9a-f]{64})"', line
        )
        if match is None:
            raise GateError(f"Lua source manifest record is malformed: {line!r}")
        records.append((match.group(1), match.group(2)))
    names = [name for name, unused_sha256 in records]
    if (
        len(records) != LUA_SOURCE_COUNT
        or len(set(names)) != LUA_SOURCE_COUNT
        or sum(name.endswith(".c") for name in names) != LUA_C_SOURCE_COUNT
        or any(not name.endswith((".c", ".h")) for name in names)
    ):
        raise GateError("Lua source manifest is not the exact 58-file/33-C closure")

    source_dir = lua_source / "src"
    if source_dir.is_symlink() or not source_dir.is_dir():
        raise GateError(f"Lua src is not a regular non-symlink directory: {source_dir}")
    source_dir = source_dir.resolve()
    for name, expected_sha256 in records:
        source = source_dir / name
        if source.is_symlink() or not source.is_file():
            raise GateError(f"Lua 5.3.3 source is missing or symlinked: {name}")
        if source.resolve().parent != source_dir:
            raise GateError(f"Lua 5.3.3 source escaped its root: {name}")
        actual_sha256 = _sha256(source)
        if actual_sha256 != expected_sha256:
            raise GateError(
                f"Lua 5.3.3 source hash mismatch for {name}: "
                f"{actual_sha256} != {expected_sha256}"
            )
    return tuple(records)


def lua_source_set_sha256(records: Sequence[tuple[str, str]]) -> str:
    identity = [
        {"path": f"{LUA_LOGICAL_SOURCE_PREFIX}/{name}", "sha256": sha256}
        for name, sha256 in records
    ]
    return hashlib.sha256(_canonical_bytes(identity)).hexdigest()


def lua_gate_paths(
    cache: Mapping[str, str],
    *,
    source_root: Path,
    build_dir: Path,
    lua_source: Path | None,
    lua_manifest: Path | None,
    lua_archive: Path | None,
    ar: Path | None,
) -> tuple[Path, Path, Path, Path] | None:
    """Bind the opt-in gate inputs to the exact CMake cache and build tree."""

    enabled = _feature(cache, LUA_CACHE_KEY)
    supplied = (lua_source, lua_manifest, lua_archive, ar)
    if not enabled:
        if any(value is not None for value in supplied):
            raise GateError("Lua gate inputs were supplied while ISAAC_VITA_LUA=OFF")
        if cache.get(LUA_SOURCE_DIR_CACHE_KEY, "") != "":
            raise GateError(
                f"CMake cache retained {LUA_SOURCE_DIR_CACHE_KEY} while Lua is OFF"
            )
        return None
    if any(value is None for value in supplied):
        raise GateError(
            "ISAAC_VITA_LUA=ON requires --lua-source, --lua-manifest, "
            "--lua-archive, and --ar"
        )

    assert lua_source is not None
    assert lua_manifest is not None
    assert lua_archive is not None
    assert ar is not None
    named = {
        "Lua source root": lua_source,
        "Lua source manifest": lua_manifest,
        "Lua archive": lua_archive,
        "Lua archiver": ar,
    }
    for label, value in named.items():
        if not value.is_absolute():
            raise GateError(f"{label} is not absolute: {value}")

    source_root = source_root.resolve()
    build_dir = build_dir.resolve()
    lua_source = lua_source.resolve()
    lua_manifest = lua_manifest.resolve()
    lua_archive = lua_archive.resolve()
    ar = ar.resolve()
    cached_source_text = cache.get(LUA_SOURCE_DIR_CACHE_KEY)
    if cached_source_text is None:
        raise GateError(f"CMake cache is missing {LUA_SOURCE_DIR_CACHE_KEY}")
    cached_source = Path(cached_source_text)
    if not cached_source.is_absolute():
        raise GateError(
            f"CMake cache {LUA_SOURCE_DIR_CACHE_KEY} is not absolute: "
            f"{cached_source_text!r}"
        )
    if cached_source.resolve() != lua_source:
        raise GateError(
            f"Lua source differs from {LUA_SOURCE_DIR_CACHE_KEY}: "
            f"{lua_source} != {cached_source.resolve()}"
        )
    expected_manifest = (source_root / "recomp" / "vita" /
                         "lua53_vanilla.cmake").resolve()
    if lua_manifest != expected_manifest:
        raise GateError(
            f"Lua source manifest is not the tracked canonical file: "
            f"{lua_manifest} != {expected_manifest}"
        )
    expected_archive = (build_dir / LUA_ARCHIVE_NAME).resolve()
    if lua_archive != expected_archive or lua_archive.name != LUA_ARCHIVE_NAME:
        raise GateError(
            f"Lua archive is not the exact build-owned {LUA_ARCHIVE_NAME}: "
            f"{lua_archive} != {expected_archive}"
        )
    cached_ar_text = cache.get("CMAKE_AR")
    if not cached_ar_text:
        raise GateError("CMake cache is missing CMAKE_AR")
    cached_ar = Path(cached_ar_text)
    if not cached_ar.is_absolute():
        cached_ar = build_dir / cached_ar
    if cached_ar.resolve() != ar:
        raise GateError(
            f"Lua archiver differs from CMAKE_AR: {ar} != {cached_ar.resolve()}"
        )
    return lua_source, lua_manifest, lua_archive, ar


def expected_sources(
    source_root: Path,
    generated_manifest: Path,
    cache: Mapping[str, str],
    generated_overrides: Mapping[str, Path] | None = None,
) -> set[Path]:
    source_root = source_root.resolve()
    runtime_root = source_root / "recomp" / "runtime"
    vita_root = source_root / "recomp" / "vita"
    generated_root = generated_manifest.resolve().parent
    try:
        manifest = json.loads(generated_manifest.read_text(encoding="utf-8"))
        outputs = manifest["outputs"]
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise GateError(f"cannot read generated output closure: {exc}") from exc
    if not isinstance(outputs, list) or not outputs:
        raise GateError("generated output closure is empty")
    generated_names: list[str] = []
    for item in outputs:
        if not isinstance(item, dict) or not isinstance(item.get("path"), str):
            raise GateError("generated output closure has a malformed record")
        name = item["path"]
        if re.fullmatch(r"guest_(?:[0-9]{4}|stubs|table)\.c", name):
            generated_names.append(name)
    if len(generated_names) < 3 or len(generated_names) != len(set(generated_names)):
        raise GateError("generated C closure is incomplete or duplicated")
    overrides = dict(generated_overrides or {})
    unknown_overrides = set(overrides) - set(generated_names)
    if unknown_overrides:
        raise GateError(
            "generated source override has no manifest owner: "
            f"{sorted(unknown_overrides)}"
        )
    generated_sources = [
        overrides.get(name, generated_root / name).resolve()
        for name in generated_names
    ]
    if len(generated_sources) != len(set(generated_sources)):
        raise GateError("generated source override aliases another physical source")

    runtime = set(BASE_RUNTIME)
    optional = (
        (LUA_CACHE_KEY, "host_vita_lua.c"),
        ("ISAAC_VITA_HEAP_LEDGER_MEMBLOCK", "host_vita_heap_ledger_memblock.c"),
        ("ISAAC_VITA_HEAP_OVERFLOW_MSPACE", "host_vita_heap_overflow_mspace.c"),
        ("ISAAC_VITA_ROOM_ENTRY_SLAB", "host_vita_room_entry_slab.c"),
        ("ISAAC_VITA_ROOM_ENTRY_HYBRID", "host_vita_room_entry_external.c"),
        ("ISAAC_VITA_ARCHIVE_FILE_CACHE", "host_vita_archive_cache.c"),
        (ARCHIVE_VALIDATION_RECEIPT_CACHE_KEY,
         "host_vita_archive_receipt.c"),
        ("ISAAC_VITA_FIOS_CACHE", "host_vita_fios_cache.c"),
        ("ISAAC_VITA_ASYNC_SAVE_WRITE", "host_vita_async_write.c"),
        ("ISAAC_VITA_LOG_ASYNC", "host_vita_log_async.c"),
        ("ISAAC_VITA_ANM2_SCRATCH", "host_vita_anm2_scratch.c"),
        ("ISAAC_VITA_TEXEL_SCRATCH", "host_vita_texel_scratch.c"),
    )
    for feature, name in optional:
        if _feature(cache, feature):
            runtime.add(name)
    kage = _feature(cache, "ISAAC_VITA_KAGE")
    audio = _feature(cache, "ISAAC_VITA_AUDIO")
    audio_stream_receipt = _feature(cache, AUDIO_STREAM_RECEIPT_CACHE_KEY)
    coloroffset_gpu = _feature(cache, COLOROFFSET_GPU_CACHE_KEY)
    renderframe_fastpath = _feature(cache, RENDERFRAME_FASTPATH_CACHE_KEY)
    continue_profile = _feature(cache, CONTINUE_PROFILE_CACHE_KEY)
    exit_menu_profile = _feature(cache, EXIT_MENU_PROFILE_CACHE_KEY)
    png_decode_profile = _feature(cache, PNG_DECODE_PROFILE_CACHE_KEY)
    png_native_unfilter = _feature(cache, PNG_NATIVE_UNFILTER_CACHE_KEY)
    png_inflate_flush_fastpath = _feature(
        cache, PNG_INFLATE_FLUSH_FASTPATH_CACHE_KEY
    )
    png_crc32_fastpath = _feature(cache, PNG_CRC32_FASTPATH_CACHE_KEY)
    archive_miniz_fastpath = _feature(cache, ARCHIVE_MINIZ_FASTPATH_CACHE_KEY)
    save_checksum_fastpath = _feature(
        cache, SAVE_CHECKSUM_FASTPATH_CACHE_KEY
    )
    save_reader_fastpath = _feature(
        cache, SAVE_READER_FASTPATH_CACHE_KEY
    )
    save_read32_fused = _feature(cache, SAVE_READ32_FUSED_CACHE_KEY)
    save_reader_direct_edges = _feature(
        cache, SAVE_READER_DIRECT_EDGES_CACHE_KEY
    )
    memset_thunk_fastpath = _feature(
        cache, MEMSET_THUNK_FASTPATH_CACHE_KEY
    )
    kage_mutex_seam = _feature(cache, KAGE_MUTEX_SEAM_CACHE_KEY)
    kage_refcount_seam = _feature(cache, KAGE_REFCOUNT_SEAM_CACHE_KEY)
    floor_thunk_fastpath = _feature(
        cache, FLOOR_THUNK_FASTPATH_CACHE_KEY
    )
    heap_overflow_mspace = _feature(
        cache, "ISAAC_VITA_HEAP_OVERFLOW_MSPACE"
    )
    texture_churn_profile = _feature(
        cache, TEXTURE_CHURN_PROFILE_CACHE_KEY
    )
    sim_cadence_receipt = _feature(cache, SIM_CADENCE_RECEIPT_CACHE_KEY)
    stable30_presentation = _feature(cache, STABLE30_PRESENTATION_CACHE_KEY)
    world_seam_diag = _feature(cache, WORLD_SEAM_DIAG_CACHE_KEY)
    # The lookup cache changes compile definitions on existing owners only,
    # but its CMake selection is still part of this fail-closed feature view.
    _feature(cache, "ISAAC_VITA_GUEST_LOOKUP_CACHE")
    # Validated sync-ID routing likewise changes only guest.c dispatch code.
    _feature(cache, SYNC_IMPORT_FASTPATH_CACHE_KEY)
    # Display raster likewise changes only the two existing graphics owners.
    _feature(cache, DISPLAY_RASTER_CACHE_KEY)
    # The typed GL state cache also changes definitions on existing owners only.
    _feature(cache, TYPED_STATE_CACHE_KEY)
    # Logger batching changes only the CRT boundary and optional cold report.
    _feature(cache, GAME_LOG_BATCH_CACHE_KEY)
    if renderframe_fastpath and not kage:
        raise GateError(
            f"{RENDERFRAME_FASTPATH_CACHE_KEY} requires ISAAC_VITA_KAGE=ON"
        )
    if continue_profile and not kage:
        raise GateError(
            f"{CONTINUE_PROFILE_CACHE_KEY} requires ISAAC_VITA_KAGE=ON"
        )
    if png_decode_profile and not kage:
        raise GateError(
            f"{PNG_DECODE_PROFILE_CACHE_KEY} requires ISAAC_VITA_KAGE=ON"
        )
    if png_native_unfilter and not kage:
        raise GateError(
            f"{PNG_NATIVE_UNFILTER_CACHE_KEY} requires ISAAC_VITA_KAGE=ON"
        )
    if png_native_unfilter and not heap_overflow_mspace:
        raise GateError(
            f"{PNG_NATIVE_UNFILTER_CACHE_KEY} requires "
            "ISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON"
        )
    if png_inflate_flush_fastpath and not kage:
        raise GateError(
            f"{PNG_INFLATE_FLUSH_FASTPATH_CACHE_KEY} requires "
            "ISAAC_VITA_KAGE=ON"
        )
    if png_inflate_flush_fastpath and not heap_overflow_mspace:
        raise GateError(
            f"{PNG_INFLATE_FLUSH_FASTPATH_CACHE_KEY} requires "
            "ISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON"
        )
    if archive_miniz_fastpath and not kage:
        raise GateError(
            f"{ARCHIVE_MINIZ_FASTPATH_CACHE_KEY} requires ISAAC_VITA_KAGE=ON"
        )
    if archive_miniz_fastpath and not heap_overflow_mspace:
        raise GateError(
            f"{ARCHIVE_MINIZ_FASTPATH_CACHE_KEY} requires "
            "ISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON"
        )
    if texture_churn_profile and not kage:
        raise GateError(
            f"{TEXTURE_CHURN_PROFILE_CACHE_KEY} requires ISAAC_VITA_KAGE=ON"
        )
    if audio_stream_receipt and (not kage or not audio):
        raise GateError(
            f"{AUDIO_STREAM_RECEIPT_CACHE_KEY} requires "
            "ISAAC_VITA_KAGE=ON and ISAAC_VITA_AUDIO=ON"
        )
    if sim_cadence_receipt and not kage:
        raise GateError(
            f"{SIM_CADENCE_RECEIPT_CACHE_KEY} requires ISAAC_VITA_KAGE=ON"
        )
    if sim_cadence_receipt and _feature(cache, "ISAAC_VITA_PHASE_PROFILE"):
        raise GateError(
            f"{SIM_CADENCE_RECEIPT_CACHE_KEY} requires "
            "ISAAC_VITA_PHASE_PROFILE=OFF"
        )
    if stable30_presentation and not kage:
        raise GateError(
            f"{STABLE30_PRESENTATION_CACHE_KEY} requires ISAAC_VITA_KAGE=ON"
        )
    if stable30_presentation and _feature(
            cache, "ISAAC_VITA_FULLSPEED_SCHEDULER"):
        raise GateError(
            f"{STABLE30_PRESENTATION_CACHE_KEY} and "
            "ISAAC_VITA_FULLSPEED_SCHEDULER are exclusive"
        )
    if world_seam_diag and not kage:
        raise GateError(
            f"{WORLD_SEAM_DIAG_CACHE_KEY} requires ISAAC_VITA_KAGE=ON"
        )
    if exit_menu_profile:
        runtime.add("kage_vita_exit_menu_profile.c")
    if png_crc32_fastpath:
        runtime.update((
            "host_vita_png_crc32_native.c",
            "host_vita_png_crc32_guest.c",
        ))
    if save_checksum_fastpath:
        runtime.update((
            "host_vita_save_checksum_native.c",
            "host_vita_save_checksum_guest.c",
        ))
    if save_reader_fastpath:
        runtime.add("host_vita_save_reader_guest.c")
    if save_read32_fused:
        if not save_reader_fastpath or not save_checksum_fastpath:
            raise GateError(
                f"{SAVE_READ32_FUSED_CACHE_KEY} requires both save fast paths"
            )
        runtime.add("host_vita_save_read32_guest.c")
    if save_reader_direct_edges:
        if not save_reader_fastpath:
            raise GateError(
                f"{SAVE_READER_DIRECT_EDGES_CACHE_KEY} requires "
                f"{SAVE_READER_FASTPATH_CACHE_KEY}=ON"
            )
        runtime.add("host_vita_save_reader_direct.c")
    if memset_thunk_fastpath:
        runtime.add("host_vita_memset_thunk_direct.c")
    if kage_mutex_seam:
        if not _feature(cache, "ISAAC_VITA_SYNC_INLINE_FASTPATH"):
            raise GateError(
                f"{KAGE_MUTEX_SEAM_CACHE_KEY} requires "
                "ISAAC_VITA_SYNC_INLINE_FASTPATH=ON"
            )
        runtime.add("host_vita_kage_mutex_seam.c")
    if kage_refcount_seam:
        if not kage_mutex_seam:
            raise GateError(
                f"{KAGE_REFCOUNT_SEAM_CACHE_KEY} requires "
                f"{KAGE_MUTEX_SEAM_CACHE_KEY}=ON"
            )
        # The seam TU is exact for the GUEST_FLAGS_LOCAL=1 corpus only and
        # CMake fails the configure otherwise; a real cache carries both
        # keys, so they are re-checked when present (synthetic caches may
        # omit them).
        for key in ("ISAAC_VITA_TRANSLATED_CPU",
                    "ISAAC_VITA_TRANSLATED_CPU_FLAGS_LOCAL"):
            if key in cache and not _feature(cache, key):
                raise GateError(
                    f"{KAGE_REFCOUNT_SEAM_CACHE_KEY} requires {key}=ON"
                )
        runtime.add("host_vita_kage_refcount_seam.c")
    if floor_thunk_fastpath:
        runtime.add("host_vita_floor_thunk_direct.c")
    if texture_churn_profile and not _feature(
            cache, "ISAAC_VITA_PHASE_PROFILE"):
        raise GateError(
            f"{TEXTURE_CHURN_PROFILE_CACHE_KEY} requires "
            "ISAAC_VITA_PHASE_PROFILE=ON"
        )
    if texture_churn_profile and not _feature(
            cache, STOCK_REFERENCE_CACHE_KEY):
        raise GateError(
            f"{TEXTURE_CHURN_PROFILE_CACHE_KEY} requires "
            f"{STOCK_REFERENCE_CACHE_KEY}=ON"
        )
    if kage:
        runtime.update(KAGE_RUNTIME)
        if audio:
            runtime.update(("host_vita_audio.c", "host_vita_openal_pool.c"))
        if _feature(cache, "ISAAC_VITA_IO_PROFILE"):
            runtime.add("kage_vita_io_profile.c")
        if _feature(cache, "ISAAC_VITA_STALL_PROBE"):
            runtime.update(
                ("kage_vita_stall_probe.c", "kage_vita_preloop_phase.c")
            )
        if _feature(cache, "ISAAC_VITA_PHASE_PROFILE"):
            runtime.add("kage_vita_phase_profile.c")
        if sim_cadence_receipt:
            runtime.add("kage_vita_sim_cadence_receipt.c")
        if world_seam_diag:
            runtime.add("kage_vita_world_seam_diag.c")
        if _feature(cache, "ISAAC_VITA_FULLSPEED_SCHEDULER"):
            runtime.add("kage_vita_fullspeed_scheduler.c")
        if stable30_presentation:
            runtime.add("kage_vita_stable30.c")
        if renderframe_fastpath:
            runtime.add("host_vita_renderframe_fastpath.c")
        if continue_profile:
            runtime.add("kage_vita_continue_profile.c")
        if png_decode_profile:
            runtime.add("kage_vita_png_decode_profile.c")
        if png_native_unfilter:
            runtime.update((
                "host_vita_png_unfilter_native.c",
                "host_vita_png_unfilter_guest.c",
            ))
        if png_inflate_flush_fastpath:
            runtime.update((
                "host_vita_zlib_inflate_flush_native.c",
                "host_vita_zlib_inflate_flush_guest.c",
            ))
        if archive_miniz_fastpath:
            runtime.update((
                "host_vita_archive_miniz_native.c",
                "host_vita_archive_miniz_guest.c",
            ))
        if _feature(cache, "ISAAC_VITA_NATIVE_PNG"):
            # the native PNG decoder inflates with the in-tree tinfl
            runtime.update((
                "host_vita_native_png.c",
                "host_vita_archive_miniz_native.c",
            ))
        if coloroffset_gpu:
            runtime.add("gl_vita_coloroffset_source.c")
        if _feature(cache, "ISAAC_VITA_RAW_GXM_PROBE"):
            runtime.add("kage_vita_raw_gxm_probe.c")
        if _feature(cache, "ISAAC_VITA_SCREENSHOT_PROBE"):
            runtime.add("kage_vita_screenshot_probe.c")
        if (_feature(cache, "ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC") or
                _feature(cache, "ISAAC_VITA_TEXTURE_ALIGN8_POLICY")):
            runtime.add("kage_vita_texture_memory.c")
    else:
        runtime.update(("kage_pc_disabled.c", "manual_kage_disabled.c"))

    sources = {
        (vita_root / name).resolve()
        for name in ("main.c", "fixed_image.c", "platform.c")
    }
    sources.update((runtime_root / name).resolve() for name in runtime)
    before_generated = len(sources)
    sources.update(generated_sources)
    if len(sources) != before_generated + len(generated_sources):
        raise GateError("generated source override collides with a runtime source")
    return sources


def direct_default_overrides(
    cache: Mapping[str, str],
    requested: Path | None,
    build_dir: Path,
) -> dict[str, Path]:
    active = _feature(cache, DIRECT_DEFAULT_CACHE_KEY)
    if active != (requested is not None):
        raise GateError(
            "direct-default cache/override selection disagrees: "
            f"cache={'ON' if active else 'OFF'}, "
            f"override={'present' if requested is not None else 'absent'}"
        )
    if requested is None:
        return {}
    render_surface_native = _feature(cache, RENDER_SURFACE_NATIVE_CACHE_KEY)
    if not requested.is_absolute():
        raise GateError(
            f"direct-default source override is not absolute: {requested}"
        )
    expected_lexical = build_dir / DIRECT_DEFAULT_OUTPUT_RELATIVE
    if requested != expected_lexical:
        raise GateError(
            "direct-default source override has the wrong build-owned path: "
            f"{requested} != {expected_lexical}"
        )
    if requested.is_symlink() or not requested.is_file():
        raise GateError(
            "direct-default source override is not a regular non-symlink file: "
            f"{requested}"
        )
    owned_directories = (requested.parent.parent, requested.parent)
    for directory in owned_directories:
        if directory.is_symlink():
            raise GateError(
                "direct-default source override has a symlinked build-owned "
                f"directory: {directory}"
            )
    resolved = requested.resolve()
    expected = (build_dir.resolve() / DIRECT_DEFAULT_OUTPUT_RELATIVE).resolve()
    if resolved != expected:
        raise GateError(
            "direct-default source override has the wrong build-owned path: "
            f"{resolved} != {expected}"
        )
    if resolved.parent.is_symlink() or not resolved.parent.is_dir():
        raise GateError(
            "direct-default source override parent is not a regular directory"
        )
    entries = list(resolved.parent.iterdir())
    if (
        len(entries) != 1
        or entries[0].name != DIRECT_DEFAULT_SOURCE_NAME
        or entries[0].is_symlink()
        or not entries[0].is_file()
    ):
        raise GateError(
            "direct-default source override directory inventory changed: "
            f"{sorted(entry.name for entry in entries)}"
        )
    record = _file_record(resolved)
    if render_surface_native:
        expected_record = {
            "size": RENDER_SURFACE_NATIVE_OUTPUT_SIZE,
            "sha256": RENDER_SURFACE_NATIVE_OUTPUT_SHA256,
        }
    else:
        expected_record = {
            "size": DIRECT_DEFAULT_OUTPUT_SIZE,
            "sha256": DIRECT_DEFAULT_OUTPUT_SHA256,
        }
    if record != expected_record:
        raise GateError(
            "direct-default source override identity changed "
            f"(render-surface-native={'ON' if render_surface_native else 'OFF'}): "
            f"{record} != {expected_record}"
        )
    return {DIRECT_DEFAULT_SOURCE_NAME: resolved}


def verify_direct_default_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, DIRECT_DEFAULT_CACHE_KEY)
    stock_reference = _feature(cache, STOCK_REFERENCE_CACHE_KEY)
    first_frame = _feature(cache, "ISAAC_VITA_FIRST_FRAME_PROBE")
    if stock_reference and not active:
        raise GateError("stock vitaGL reference requires direct-default")
    if active and stock_reference:
        if first_frame:
            raise GateError("stock vitaGL reference forbids first-frame probe")
        expected = frozenset()
    else:
        if active and not first_frame:
            raise GateError(
                "direct-default requires ISAAC_VITA_FIRST_FRAME_PROBE=ON "
                "outside the stock vitaGL reference"
            )
        expected = DIRECT_DEFAULT_DEFINITION_OWNERS if active else frozenset()
    actual: set[str] = set()
    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if DIRECT_DEFAULT_CACHE_KEY in undefinitions:
            raise GateError(
                f"direct-default compile policy is undefined by {source}"
            )
        values = definitions.get(DIRECT_DEFAULT_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical direct-default "
                    f"definition: {values}"
                )
            actual.add(source.name)
    if actual != expected:
        raise GateError(
            "direct-default compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_stock_reference_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, STOCK_REFERENCE_CACHE_KEY)
    actual: set[str] = set()
    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if STOCK_REFERENCE_CACHE_KEY in undefinitions:
            raise GateError(
                f"stock-reference compile policy is undefined by {source}"
            )
        values = definitions.get(STOCK_REFERENCE_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical stock-reference "
                    f"definition: {values}"
                )
            actual.add(source.name)
    expected = STOCK_REFERENCE_DEFINITION_OWNERS if active else frozenset()
    if actual != expected:
        raise GateError(
            "stock-reference compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_display_raster_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, DISPLAY_RASTER_CACHE_KEY)
    actual: set[str] = set()
    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if DISPLAY_RASTER_CACHE_KEY in undefinitions:
            raise GateError(
                f"display-raster compile policy is undefined by {source}"
            )
        values = definitions.get(DISPLAY_RASTER_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical display-raster "
                    f"definition: {values}"
                )
            actual.add(source.name)
    expected = DISPLAY_RASTER_DEFINITION_OWNERS if active else frozenset()
    if actual != expected:
        raise GateError(
            "display-raster compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_typed_state_cache_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, TYPED_STATE_CACHE_KEY)
    phase_profile = _feature(cache, "ISAAC_VITA_PHASE_PROFILE")
    actual: set[str] = set()
    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if TYPED_STATE_CACHE_KEY in undefinitions:
            raise GateError(
                f"typed-state-cache compile policy is undefined by {source}"
            )
        values = definitions.get(TYPED_STATE_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical typed-state-cache "
                    f"definition: {values}"
                )
            actual.add(source.name)
    expected = set(TYPED_STATE_CACHE_BASE_DEFINITION_OWNERS) if active else set()
    if active and phase_profile:
        expected.add("kage_vita_phase_profile.c")
    if actual != expected:
        raise GateError(
            "typed-state-cache compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_game_log_batch_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, GAME_LOG_BATCH_CACHE_KEY)
    continue_profile = _feature(cache, "ISAAC_VITA_CONTINUE_PROFILE")
    actual: set[str] = set()
    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if GAME_LOG_BATCH_CACHE_KEY in undefinitions:
            raise GateError(
                f"game-log-batch compile policy is undefined by {source}"
            )
        values = definitions.get(GAME_LOG_BATCH_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical game-log-batch "
                    f"definition: {values}"
                )
            actual.add(source.name)
    expected = set(GAME_LOG_BATCH_BASE_DEFINITION_OWNERS) if active else set()
    if active and continue_profile:
        expected.add("kage_vita_continue_profile.c")
    if actual != expected:
        raise GateError(
            "game-log-batch compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_sync_import_fastpath_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, SYNC_IMPORT_FASTPATH_CACHE_KEY)
    actual: set[str] = set()
    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if SYNC_IMPORT_FASTPATH_CACHE_KEY in undefinitions:
            raise GateError(
                f"sync-import compile policy is undefined by {source}"
            )
        values = definitions.get(SYNC_IMPORT_FASTPATH_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical sync-import "
                    f"definition: {values}"
                )
            actual.add(source.name)
    expected = (
        SYNC_IMPORT_FASTPATH_DEFINITION_OWNERS if active else frozenset()
    )
    if actual != expected:
        raise GateError(
            "sync-import compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_exit_menu_profile_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, EXIT_MENU_PROFILE_CACHE_KEY)
    actual: set[str] = set()
    expected: set[str] = set()
    roots = (
        "sub_0025b340", "sub_004b0010", "sub_004b43d0",
        "sub_004b4500", "sub_004b47a0",
    )
    definition_count = 0
    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if EXIT_MENU_PROFILE_CACHE_KEY in undefinitions:
            raise GateError(
                f"Exit-menu compile policy is undefined by {source}"
            )
        values = definitions.get(EXIT_MENU_PROFILE_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical Exit-menu "
                    f"definition: {values}"
                )
            actual.add(source.name)
        if active and source.name.startswith("guest_") and \
                source.suffix == ".c":
            try:
                text = source.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                raise GateError(
                    f"cannot inspect Exit-menu generated owner {source}: "
                    f"{exc}"
                ) from exc
            matches = sum(
                text.count(f"void {root}(CPU *__restrict c)")
                for root in roots
            )
            if matches:
                expected.add(source.name)
                definition_count += matches
    if active:
        expected.update((
            "host_vita_crt.c", "kage_vita_exit_menu_profile.c",
        ))
        if definition_count != len(roots):
            raise GateError(
                "Exit-menu generated definition census changed: "
                f"{definition_count} != {len(roots)}"
            )
    if actual != expected:
        raise GateError(
            "Exit-menu compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_png_decode_profile_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, PNG_DECODE_PROFILE_CACHE_KEY)
    expected_build_id_value: str | None = None
    if active:
        try:
            guest_link_id = cache[GUEST_LINK_ID_CACHE_KEY]
        except KeyError as exc:
            raise GateError(
                f"CMake cache is missing {GUEST_LINK_ID_CACHE_KEY}"
            ) from exc
        if re.fullmatch(r"[A-Za-z0-9:._-]{1,64}", guest_link_id) is None:
            raise GateError(
                f"CMake cache {GUEST_LINK_ID_CACHE_KEY} is not a controlled "
                f"build ID: {guest_link_id!r}"
            )
        expected_build_id_value = f'"{guest_link_id}"'
    actual: set[str] = set()
    build_id_actual: set[str] = set()
    expected: set[str] = set()
    roots = (
        "sub_005a0c50", "sub_005a0cd0", "sub_005b1500",
        "sub_005c5fb0", "sub_005c6fa0", "sub_005d6d80",
    )
    definition_count = 0
    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if PNG_DECODE_PROFILE_CACHE_KEY in undefinitions:
            raise GateError(
                f"PNG profile compile policy is undefined by {source}"
            )
        if PNG_DECODE_PROFILE_BUILD_ID_MACRO in undefinitions:
            raise GateError(
                f"PNG profile build ID is undefined by {source}"
            )
        values = definitions.get(PNG_DECODE_PROFILE_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical PNG profile "
                    f"definition: {values}"
                )
            actual.add(source.name)
        build_ids = definitions.get(PNG_DECODE_PROFILE_BUILD_ID_MACRO, [])
        if build_ids:
            if active and build_ids != [expected_build_id_value]:
                raise GateError(
                    f"{source.name} PNG profile build ID disagrees with "
                    f"{GUEST_LINK_ID_CACHE_KEY}: {build_ids} != "
                    f"{[expected_build_id_value]}"
                )
            build_id_actual.add(source.name)
        if active and source.name.startswith("guest_") and source.suffix == ".c":
            try:
                text = source.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                raise GateError(
                    f"cannot inspect PNG profile generated owner {source}: {exc}"
                ) from exc
            matches = sum(
                text.count(f"void {root}(CPU *__restrict c)")
                for root in roots
            )
            if matches:
                expected.add(source.name)
                definition_count += matches
    if active:
        expected.add("kage_vita_png_decode_profile.c")
        if _feature(cache, PNG_NATIVE_UNFILTER_CACHE_KEY):
            expected.add("host_vita_png_unfilter_guest.c")
        if definition_count != len(roots):
            raise GateError(
                "PNG profile generated definition census changed: "
                f"{definition_count} != {len(roots)}"
            )
    if actual != expected:
        raise GateError(
            "PNG profile compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )
    expected_build_id = (
        {"kage_vita_png_decode_profile.c"} if active else set()
    )
    if build_id_actual != expected_build_id:
        raise GateError(
            "PNG profile build-ID compile-definition scope changed: "
            f"missing={sorted(expected_build_id - build_id_actual)}, "
            f"extra={sorted(build_id_actual - expected_build_id)}"
        )


def verify_png_native_unfilter_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, PNG_NATIVE_UNFILTER_CACHE_KEY)
    expected: set[str] = set()
    actual: set[str] = set()
    owner_count = 0
    guest_has_range_lease = False

    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if PNG_NATIVE_UNFILTER_CACHE_KEY in undefinitions:
            raise GateError(
                f"PNG native-unfilter policy is undefined by {source}"
            )
        values = definitions.get(PNG_NATIVE_UNFILTER_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical PNG native-unfilter "
                    f"definition: {values}"
                )
            actual.add(source.name)
        if source.name == "host_vita_png_unfilter_guest.c":
            range_values = definitions.get("ISAAC_VITA_HEAP_RANGE_LEASE", [])
            if range_values not in ([], ["1"]):
                raise GateError(
                    f"{source.name} has a non-canonical heap range-lease "
                    f"definition: {range_values}"
                )
            guest_has_range_lease = range_values == ["1"]
        if active and source.name.startswith("guest_") and source.suffix == ".c":
            try:
                text = source.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                raise GateError(
                    f"cannot inspect PNG native generated owner {source}: {exc}"
                ) from exc
            matches = text.count("void sub_005c6bf0(CPU *__restrict c)")
            if matches:
                expected.add(source.name)
                owner_count += matches

    if active:
        expected.update((
            "host_vita_heap.c",
            "host_vita_png_unfilter_native.c",
            "host_vita_png_unfilter_guest.c",
        ))
        if owner_count != 1:
            raise GateError(
                "PNG native-unfilter generated definition census changed: "
                f"{owner_count} != 1"
            )
        if not guest_has_range_lease:
            raise GateError(
                "PNG native-unfilter guest shim lost its heap range lease"
            )
    if actual != expected:
        raise GateError(
            "PNG native-unfilter compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_png_inflate_flush_fastpath_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, PNG_INFLATE_FLUSH_FASTPATH_CACHE_KEY)
    expected: set[str] = set()
    actual: set[str] = set()
    owner_count = 0
    seam_count = 0
    guest_has_range_lease = False

    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if PNG_INFLATE_FLUSH_FASTPATH_CACHE_KEY in undefinitions:
            raise GateError(
                f"PNG inflate-flush policy is undefined by {source}"
            )
        values = definitions.get(PNG_INFLATE_FLUSH_FASTPATH_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical PNG inflate-flush "
                    f"definition: {values}"
                )
            actual.add(source.name)
        if source.name == "host_vita_zlib_inflate_flush_guest.c":
            range_values = definitions.get("ISAAC_VITA_HEAP_RANGE_LEASE", [])
            if range_values not in ([], ["1"]):
                raise GateError(
                    f"{source.name} has a non-canonical heap range-lease "
                    f"definition: {range_values}"
                )
            guest_has_range_lease = range_values == ["1"]
        if active and source.name.startswith("guest_") and \
                source.suffix == ".c":
            try:
                text = source.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                raise GateError(
                    "cannot inspect PNG inflate-flush generated owner "
                    f"{source}: {exc}"
                ) from exc
            roots = text.count("void sub_005d6720(CPU *__restrict c)")
            seams = text.count(
                "isaac_vita_zlib_inflate_flush_guest_try(c)"
            )
            if roots or seams:
                expected.add(source.name)
                owner_count += roots
                seam_count += seams

    if active:
        expected.update((
            "host_vita_heap.c",
            "host_vita_zlib_inflate_flush_guest.c",
        ))
        if owner_count != 1 or seam_count != 1:
            raise GateError(
                "PNG inflate-flush generated owner/seam census changed: "
                f"owners={owner_count}, seams={seam_count}"
            )
        if not guest_has_range_lease:
            raise GateError(
                "PNG inflate-flush guest shim lost its heap range lease"
            )
    if actual != expected:
        raise GateError(
            "PNG inflate-flush compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_png_crc32_fastpath_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, PNG_CRC32_FASTPATH_CACHE_KEY)
    expected: set[str] = set()
    actual: set[str] = set()
    owner_count = 0
    seam_count = 0

    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if PNG_CRC32_FASTPATH_CACHE_KEY in undefinitions:
            raise GateError(f"PNG CRC32 policy is undefined by {source}")
        values = definitions.get(PNG_CRC32_FASTPATH_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical PNG CRC32 "
                    f"definition: {values}"
                )
            actual.add(source.name)
        if active and source.name.startswith("guest_") and \
                source.suffix == ".c":
            try:
                text = source.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                raise GateError(
                    f"cannot inspect PNG CRC32 generated owner {source}: "
                    f"{exc}"
                ) from exc
            roots = text.count("void sub_005c3320(CPU *__restrict c)")
            seams = text.count("isaac_vita_png_crc32_guest_try(c)")
            if roots or seams:
                expected.add(source.name)
                owner_count += roots
                seam_count += seams

    if active:
        expected.add("host_vita_png_crc32_guest.c")
        if owner_count != 1 or seam_count != 1:
            raise GateError(
                "PNG CRC32 generated owner/seam census changed: "
                f"owners={owner_count}, seams={seam_count}"
            )
    if actual != expected:
        raise GateError(
            "PNG CRC32 compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_archive_miniz_fastpath_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, ARCHIVE_MINIZ_FASTPATH_CACHE_KEY)
    expected: set[str] = set()
    actual: set[str] = set()
    root_count = 0
    seam_count = 0
    guest_has_range_lease = False

    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if ARCHIVE_MINIZ_FASTPATH_CACHE_KEY in undefinitions:
            raise GateError(f"archive MiniZ policy is undefined by {source}")
        values = definitions.get(ARCHIVE_MINIZ_FASTPATH_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical archive MiniZ "
                    f"definition: {values}"
                )
            actual.add(source.name)
        if source.name == "host_vita_archive_miniz_guest.c":
            range_values = definitions.get("ISAAC_VITA_HEAP_RANGE_LEASE", [])
            if range_values not in ([], ["1"]):
                raise GateError(
                    f"{source.name} has a non-canonical heap range-lease "
                    f"definition: {range_values}"
                )
            guest_has_range_lease = range_values == ["1"]
        if active and source.name.startswith("guest_") and \
                source.suffix == ".c":
            try:
                text = source.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                raise GateError(
                    f"cannot inspect archive MiniZ generated owner "
                    f"{source}: {exc}"
                ) from exc
            roots = text.count("void sub_005aeb00(CPU *__restrict c)")
            seams = text.count("isaac_vita_archive_miniz_guest_try(c)")
            if roots or seams:
                expected.add(source.name)
                root_count += roots
                seam_count += seams

    if active:
        expected.update((
            "host_vita_heap.c",
            "host_vita_archive_miniz_guest.c",
        ))
        if root_count != 1 or seam_count != 1:
            raise GateError(
                "archive MiniZ generated owner/seam census changed: "
                f"roots={root_count}, seams={seam_count}"
            )
        if not guest_has_range_lease:
            raise GateError(
                "archive MiniZ guest shim lost its heap range lease"
            )
    if actual != expected:
        raise GateError(
            "archive MiniZ compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_save_checksum_fastpath_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, SAVE_CHECKSUM_FASTPATH_CACHE_KEY)
    expected: set[str] = set()
    actual: set[str] = set()
    owner_count = 0
    seam_count = 0

    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if SAVE_CHECKSUM_FASTPATH_CACHE_KEY in undefinitions:
            raise GateError(
                f"save-checksum policy is undefined by {source}"
            )
        values = definitions.get(SAVE_CHECKSUM_FASTPATH_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical save-checksum "
                    f"definition: {values}"
                )
            actual.add(source.name)
        if active and source.name.startswith("guest_") and \
                source.suffix == ".c":
            try:
                text = source.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                raise GateError(
                    f"cannot inspect save-checksum generated owner "
                    f"{source}: {exc}"
                ) from exc
            roots = text.count(
                "void sub_0025b340(CPU *__restrict c)"
            )
            seams = text.count(
                "Frozen save checksum native fast path"
            )
            if roots or seams:
                expected.add(source.name)
                owner_count += roots
                seam_count += seams

    if active:
        expected.add("host_vita_save_checksum_guest.c")
        if owner_count != 1 or seam_count != 1:
            raise GateError(
                "save-checksum generated owner/seam census changed: "
                f"owners={owner_count}, seams={seam_count}"
            )
    if actual != expected:
        raise GateError(
            "save-checksum compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_save_reader_fastpath_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, SAVE_READER_FASTPATH_CACHE_KEY)
    expected: set[str] = set()
    actual: set[str] = set()
    owner_count = 0
    seam_count = 0

    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if SAVE_READER_FASTPATH_CACHE_KEY in undefinitions:
            raise GateError(
                f"save-reader policy is undefined by {source}"
            )
        values = definitions.get(SAVE_READER_FASTPATH_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical save-reader "
                    f"definition: {values}"
                )
            actual.add(source.name)
        if active and source.name.startswith("guest_") and \
                source.suffix == ".c":
            try:
                text = source.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                raise GateError(
                    f"cannot inspect save-reader generated owner "
                    f"{source}: {exc}"
                ) from exc
            roots = text.count(
                "void sub_0025ba60(CPU *__restrict c)"
            )
            seams = text.count(
                "Frozen memory Save Reader native fast path"
            )
            if roots or seams:
                expected.add(source.name)
                owner_count += roots
                seam_count += seams

    if active:
        expected.add("host_vita_save_reader_guest.c")
        if owner_count != 1 or seam_count != 1:
            raise GateError(
                "save-reader generated owner/seam census changed: "
                f"owners={owner_count}, seams={seam_count}"
            )
    if actual != expected:
        raise GateError(
            "save-reader compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_save_read32_fused_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, SAVE_READ32_FUSED_CACHE_KEY)
    expected: set[str] = set()
    actual: set[str] = set()
    owner_count = 0
    seam_count = 0

    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if SAVE_READ32_FUSED_CACHE_KEY in undefinitions:
            raise GateError(f"save-read32 policy is undefined by {source}")
        values = definitions.get(SAVE_READ32_FUSED_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical save-read32 "
                    f"definition: {values}"
                )
            actual.add(source.name)
        if active and source.name.startswith("guest_") and \
                source.suffix == ".c":
            try:
                text = source.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                raise GateError(
                    f"cannot inspect save-read32 generated owner "
                    f"{source}: {exc}"
                ) from exc
            roots = text.count(
                "void sub_0052ea90(CPU *__restrict c)"
            )
            seams = text.count(
                "Frozen Save Reader read32/checksum dispatch fusion"
            )
            if roots or seams:
                expected.add(source.name)
                owner_count += roots
                seam_count += seams

    if active:
        expected.add("host_vita_save_read32_guest.c")
        if owner_count != 1 or seam_count != 1:
            raise GateError(
                "save-read32 generated owner/seam census changed: "
                f"owners={owner_count}, seams={seam_count}"
            )
    if actual != expected:
        raise GateError(
            "save-read32 compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_save_reader_direct_edges_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, SAVE_READER_DIRECT_EDGES_CACHE_KEY)
    expected: set[str] = set()
    actual: set[str] = set()
    root_count = 0
    seam_count = 0

    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if SAVE_READER_DIRECT_EDGES_CACHE_KEY in undefinitions:
            raise GateError(f"save-reader direct policy undefined by {source}")
        values = definitions.get(SAVE_READER_DIRECT_EDGES_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has non-canonical save-reader direct "
                    f"definition: {values}"
                )
            actual.add(source.name)
        if active and source.name.startswith("guest_") and \
                source.suffix == ".c":
            try:
                text = source.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                raise GateError(
                    f"cannot inspect save-reader direct owner {source}: {exc}"
                ) from exc
            roots = (text.count("void sub_005237a0(CPU *__restrict c)") +
                     text.count("void sub_00524320(CPU *__restrict c)") +
                     text.count("void sub_005282d0(CPU *__restrict c)"))
            seams = text.count(
                "isaac_vita_save_reader_direct_try(c, _target)"
            )
            if roots or seams:
                expected.add(source.name)
                root_count += roots
                seam_count += seams

    if active:
        expected.add("host_vita_save_reader_direct.c")
        if root_count != 3 or seam_count != 326:
            raise GateError(
                "save-reader direct root/seam census changed: "
                f"roots={root_count}, seams={seam_count}"
            )
    if actual != expected:
        raise GateError(
            "save-reader direct compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_memset_thunk_fastpath_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, MEMSET_THUNK_FASTPATH_CACHE_KEY)
    expected: set[str] = set()
    actual: set[str] = set()
    root_count = 0
    seam_count = 0

    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if MEMSET_THUNK_FASTPATH_CACHE_KEY in undefinitions:
            raise GateError(f"memset-thunk policy undefined by {source}")
        values = definitions.get(MEMSET_THUNK_FASTPATH_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has non-canonical memset-thunk "
                    f"definition: {values}"
                )
            actual.add(source.name)
        if active and source.name.startswith("guest_") and \
                source.suffix == ".c":
            try:
                text = source.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                raise GateError(
                    f"cannot inspect memset-thunk generated owner "
                    f"{source}: {exc}"
                ) from exc
            roots = text.count("void sub_005ec152(CPU *__restrict c)")
            seams = text.count("Authenticated memset IAT fast dispatch")
            if roots or seams:
                expected.add(source.name)
                root_count += roots
                seam_count += seams

    if active:
        expected.add("host_vita_memset_thunk_direct.c")
        if root_count != 1 or seam_count != 1:
            raise GateError(
                "memset-thunk generated owner/seam census changed: "
                f"owners={root_count}, seams={seam_count}"
            )
    if actual != expected:
        raise GateError(
            "memset-thunk compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_kage_mutex_seam_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    """ISAAC_VITA_KAGE_MUTEX_SEAM=1 reaches exactly the generated owner(s) of
    the two frozen mutex wrappers and the seam helper; each root is defined
    once and carries exactly one seam line in the same owner."""
    active = _feature(cache, KAGE_MUTEX_SEAM_CACHE_KEY)
    expected: set[str] = set()
    actual: set[str] = set()
    root_counts = [0] * len(KAGE_MUTEX_SEAM_ROOTS)
    seam_counts = [0] * len(KAGE_MUTEX_SEAM_ROOTS)

    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if KAGE_MUTEX_SEAM_CACHE_KEY in undefinitions:
            raise GateError(f"KAGE mutex seam policy undefined by {source}")
        values = definitions.get(KAGE_MUTEX_SEAM_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has non-canonical KAGE mutex seam "
                    f"definition: {values}"
                )
            actual.add(source.name)
        if active and source.name.startswith("guest_") and \
                source.suffix == ".c":
            try:
                text = source.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                raise GateError(
                    f"cannot inspect KAGE mutex generated owner "
                    f"{source}: {exc}"
                ) from exc
            owner = False
            for index, (root, marker) in enumerate(KAGE_MUTEX_SEAM_ROOTS):
                roots = text.count(root)
                seams = text.count(marker)
                if roots != seams or roots > 1:
                    raise GateError(
                        "KAGE mutex owner has a missing or duplicate seam: "
                        f"{source.name} roots={roots} seams={seams}"
                    )
                root_counts[index] += roots
                seam_counts[index] += seams
                owner = owner or roots > 0
            if owner:
                expected.add(source.name)

    if active:
        expected.add("host_vita_kage_mutex_seam.c")
        if root_counts != [1] * len(KAGE_MUTEX_SEAM_ROOTS) or \
                seam_counts != [1] * len(KAGE_MUTEX_SEAM_ROOTS):
            raise GateError(
                "KAGE mutex generated owner/seam census changed: "
                f"roots={root_counts}, seams={seam_counts}"
            )
    if actual != expected:
        raise GateError(
            "KAGE mutex seam compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_kage_refcount_seam_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    """ISAAC_VITA_KAGE_REFCOUNT_SEAM=1 reaches exactly the one generated owner
    of the three frozen ReferenceCount helpers and the seam helper TU; each
    root is defined once and carries exactly one seam line in that owner."""
    active = _feature(cache, KAGE_REFCOUNT_SEAM_CACHE_KEY)
    expected: set[str] = set()
    actual: set[str] = set()
    owners: set[str] = set()
    root_counts = [0] * len(KAGE_REFCOUNT_SEAM_ROOTS)
    seam_counts = [0] * len(KAGE_REFCOUNT_SEAM_ROOTS)

    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if KAGE_REFCOUNT_SEAM_CACHE_KEY in undefinitions:
            raise GateError(
                f"KAGE refcount seam policy undefined by {source}")
        values = definitions.get(KAGE_REFCOUNT_SEAM_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has non-canonical KAGE refcount seam "
                    f"definition: {values}"
                )
            actual.add(source.name)
        if active and source.name.startswith("guest_") and \
                source.suffix == ".c":
            try:
                text = source.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                raise GateError(
                    f"cannot inspect KAGE refcount generated owner "
                    f"{source}: {exc}"
                ) from exc
            owner = False
            for index, (root, marker) in enumerate(KAGE_REFCOUNT_SEAM_ROOTS):
                roots = text.count(root)
                seams = text.count(marker)
                if roots != seams or roots > 1:
                    raise GateError(
                        "KAGE refcount owner has a missing or duplicate "
                        f"seam: {source.name} roots={roots} seams={seams}"
                    )
                root_counts[index] += roots
                seam_counts[index] += seams
                owner = owner or roots > 0
            if owner:
                expected.add(source.name)
                owners.add(source.name)

    if active:
        expected.add("host_vita_kage_refcount_seam.c")
        if root_counts != [1] * len(KAGE_REFCOUNT_SEAM_ROOTS) or \
                seam_counts != [1] * len(KAGE_REFCOUNT_SEAM_ROOTS) or \
                len(owners) != 1:
            raise GateError(
                "KAGE refcount generated owner/seam census changed: "
                f"roots={root_counts}, seams={seam_counts}, "
                f"owners={sorted(owners)}"
            )
    if actual != expected:
        raise GateError(
            "KAGE refcount seam compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_floor_thunk_fastpath_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, FLOOR_THUNK_FASTPATH_CACHE_KEY)
    expected: set[str] = set()
    actual: set[str] = set()
    root_count = 0
    seam_count = 0

    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if FLOOR_THUNK_FASTPATH_CACHE_KEY in undefinitions:
            raise GateError(f"floor-thunk policy undefined by {source}")
        values = definitions.get(FLOOR_THUNK_FASTPATH_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has non-canonical floor-thunk "
                    f"definition: {values}"
                )
            actual.add(source.name)
        if active and source.name.startswith("guest_") and \
                source.suffix == ".c":
            try:
                text = source.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                raise GateError(
                    f"cannot inspect floor-thunk generated owner "
                    f"{source}: {exc}"
                ) from exc
            roots = text.count("void sub_005ec3b2(CPU *__restrict c)")
            seams = text.count("Authenticated floor IAT fast dispatch")
            if roots or seams:
                expected.add(source.name)
                root_count += roots
                seam_count += seams

    if active:
        expected.add("host_vita_floor_thunk_direct.c")
        if root_count != 1 or seam_count != 1:
            raise GateError(
                "floor-thunk generated owner/seam census changed: "
                f"owners={root_count}, seams={seam_count}"
            )
    if actual != expected:
        raise GateError(
            "floor-thunk compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_texture_churn_profile_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, TEXTURE_CHURN_PROFILE_CACHE_KEY)
    actual: set[str] = set()
    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if TEXTURE_CHURN_PROFILE_CACHE_KEY in undefinitions:
            raise GateError(
                f"texture-churn compile policy is undefined by {source}"
            )
        values = definitions.get(TEXTURE_CHURN_PROFILE_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical texture-churn "
                    f"definition: {values}"
                )
            actual.add(source.name)
    expected = (
        set(TEXTURE_CHURN_PROFILE_DEFINITION_OWNERS) if active else set()
    )
    if actual != expected:
        raise GateError(
            "texture-churn compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_audio_stream_receipt_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, AUDIO_STREAM_RECEIPT_CACHE_KEY)
    actual: set[str] = set()
    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if AUDIO_STREAM_RECEIPT_CACHE_KEY in undefinitions:
            raise GateError(
                f"audio-stream receipt compile policy is undefined by {source}"
            )
        values = definitions.get(AUDIO_STREAM_RECEIPT_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical audio-stream receipt "
                    f"definition: {values}"
                )
            actual.add(source.name)
    expected = (
        set(AUDIO_STREAM_RECEIPT_DEFINITION_OWNERS) if active else set()
    )
    if actual != expected:
        raise GateError(
            "audio-stream receipt compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def verify_sim_cadence_receipt_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, SIM_CADENCE_RECEIPT_CACHE_KEY)
    expected_build_id: str | None = None
    if active:
        try:
            guest_link_id = cache[GUEST_LINK_ID_CACHE_KEY]
        except KeyError as exc:
            raise GateError(
                f"CMake cache is missing {GUEST_LINK_ID_CACHE_KEY}"
            ) from exc
        if re.fullmatch(r"[A-Za-z0-9:._-]{1,64}", guest_link_id) is None:
            raise GateError(
                f"CMake cache {GUEST_LINK_ID_CACHE_KEY} is not a controlled "
                f"build ID: {guest_link_id!r}"
            )
        expected_build_id = f'"{guest_link_id}"'

    actual: set[str] = set()
    actual_build_id: set[str] = set()
    generated_owners: set[str] = set()
    owner_count = 0
    seam_count = 0
    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if SIM_CADENCE_RECEIPT_CACHE_KEY in undefinitions:
            raise GateError(
                f"simulation-cadence compile policy is undefined by {source}"
            )
        if SIM_CADENCE_RECEIPT_BUILD_ID_MACRO in undefinitions:
            raise GateError(
                f"simulation-cadence build ID is undefined by {source}"
            )
        values = definitions.get(SIM_CADENCE_RECEIPT_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical simulation-cadence "
                    f"definition: {values}"
                )
            actual.add(source.name)
        build_ids = definitions.get(SIM_CADENCE_RECEIPT_BUILD_ID_MACRO, [])
        if build_ids:
            if not active or build_ids != [expected_build_id]:
                raise GateError(
                    f"{source.name} simulation-cadence build ID disagrees "
                    f"with {GUEST_LINK_ID_CACHE_KEY}: {build_ids} != "
                    f"{[expected_build_id] if active else []}"
                )
            actual_build_id.add(source.name)
        if active and re.fullmatch(r"guest_(?:[0-9]{4}|stubs|table)\.c",
                                   source.name):
            try:
                text = source.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                raise GateError(
                    "cannot inspect simulation-cadence generated owner "
                    f"{source}: {exc}"
                ) from exc
            roots = text.count("void sub_002cdcf0(CPU *__restrict c)")
            seams = text.count(
                "kage_vita_sim_cadence_note_game_update("
            )
            if roots or seams:
                generated_owners.add(source.name)
                owner_count += roots
                seam_count += seams

    expected = set()
    expected_build_id_owners = set()
    if active:
        if owner_count != 1 or seam_count != 2 or \
                len(generated_owners) != 1:
            raise GateError(
                "simulation-cadence generated owner/seam census changed: "
                f"owners={owner_count}, seams={seam_count}, "
                f"files={sorted(generated_owners)}"
            )
        expected = (
            set(SIM_CADENCE_RECEIPT_RUNTIME_DEFINITION_OWNERS) |
            generated_owners
        )
        expected_build_id_owners = {"kage_vita_sim_cadence_receipt.c"}
    if actual != expected:
        raise GateError(
            "simulation-cadence compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )
    if actual_build_id != expected_build_id_owners:
        raise GateError(
            "simulation-cadence build-ID scope changed: "
            f"missing={sorted(expected_build_id_owners - actual_build_id)}, "
            f"extra={sorted(actual_build_id - expected_build_id_owners)}"
        )


def verify_stable30_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, STABLE30_PRESENTATION_CACHE_KEY)
    expected_build_id: str | None = None
    if active:
        try:
            guest_link_id = cache[GUEST_LINK_ID_CACHE_KEY]
        except KeyError as exc:
            raise GateError(
                f"CMake cache is missing {GUEST_LINK_ID_CACHE_KEY}"
            ) from exc
        if re.fullmatch(r"[A-Za-z0-9:._-]{1,64}", guest_link_id) is None:
            raise GateError(
                f"CMake cache {GUEST_LINK_ID_CACHE_KEY} is not a controlled "
                f"build ID: {guest_link_id!r}"
            )
        expected_build_id = f'"{guest_link_id}"'

    actual: set[str] = set()
    actual_build_id: set[str] = set()
    generated_owners: set[str] = set()
    app_roots = 0
    manager_roots = 0
    render_roots = 0
    seams = 0
    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if STABLE30_PRESENTATION_CACHE_KEY in undefinitions:
            raise GateError(f"stable30 policy is undefined by {source}")
        if STABLE30_BUILD_ID_MACRO in undefinitions:
            raise GateError(f"stable30 build ID is undefined by {source}")
        values = definitions.get(STABLE30_PRESENTATION_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical stable30 "
                    f"definition: {values}"
                )
            actual.add(source.name)
        build_ids = definitions.get(STABLE30_BUILD_ID_MACRO, [])
        if build_ids:
            if not active or build_ids != [expected_build_id]:
                raise GateError(
                    f"{source.name} stable30 build ID disagrees with "
                    f"{GUEST_LINK_ID_CACHE_KEY}: {build_ids}"
                )
            actual_build_id.add(source.name)
        if active and re.fullmatch(r"guest_(?:[0-9]{4}|stubs|table)\.c",
                                   source.name):
            try:
                text = source.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                raise GateError(
                    f"cannot inspect stable30 generated owner {source}: "
                    f"{exc}"
                ) from exc
            app = text.count("void sub_0048bc50(CPU *__restrict c)")
            manager = text.count("void sub_004b0010(CPU *__restrict c)")
            render = text.count("void sub_004b0600(CPU *__restrict c)")
            file_seams = sum(text.count(marker) for marker in (
                "if (kage_pc_backend_stable30_bypass_limiter())",
                "if (kage_pc_backend_stable30_manager_entry(c->ecx))",
                "if (kage_pc_backend_stable30_plan_render(",
            ))
            if app or manager or render or file_seams:
                generated_owners.add(source.name)
                app_roots += app
                manager_roots += manager
                render_roots += render
                seams += file_seams

    expected = set()
    expected_build_id_owners = set()
    if active:
        if (app_roots, manager_roots, render_roots, seams) != (1, 1, 1, 3) or \
                len(generated_owners) != 2:
            raise GateError(
                "stable30 generated owner/seam census changed: "
                f"roots={app_roots}/{manager_roots}/{render_roots}, "
                f"seams={seams}, files={sorted(generated_owners)}"
            )
        expected = set(STABLE30_RUNTIME_DEFINITION_OWNERS) | generated_owners
        expected_build_id_owners = {"kage_vita_stable30.c"}
    if actual != expected:
        raise GateError(
            "stable30 compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )
    if actual_build_id != expected_build_id_owners:
        raise GateError(
            "stable30 build-ID scope changed: "
            f"missing={sorted(expected_build_id_owners - actual_build_id)}, "
            f"extra={sorted(actual_build_id - expected_build_id_owners)}"
        )


def verify_world_seam_diag_compile_scope(
    records: Mapping[Path, Mapping[str, object]],
    cache: Mapping[str, str],
) -> None:
    active = _feature(cache, WORLD_SEAM_DIAG_CACHE_KEY)
    expected_build_id: str | None = None
    if active:
        try:
            guest_link_id = cache[GUEST_LINK_ID_CACHE_KEY]
        except KeyError as exc:
            raise GateError(
                f"CMake cache is missing {GUEST_LINK_ID_CACHE_KEY}"
            ) from exc
        if re.fullmatch(r"[A-Za-z0-9:._-]{1,64}", guest_link_id) is None:
            raise GateError(
                f"CMake cache {GUEST_LINK_ID_CACHE_KEY} is not a controlled "
                f"build ID: {guest_link_id!r}"
            )
        expected_build_id = f'"{guest_link_id}"'

    actual: set[str] = set()
    actual_build_id: set[str] = set()
    generated_owners: set[str] = set()
    owner_count = 0
    seam_count = 0
    for record in records.values():
        definitions = record.get("definitions")
        undefinitions = record.get("undefinitions", [])
        source = record.get("source")
        if (not isinstance(definitions, dict) or
                not isinstance(undefinitions, list) or
                not isinstance(source, Path)):
            raise GateError("compile record is missing source definitions")
        if WORLD_SEAM_DIAG_CACHE_KEY in undefinitions:
            raise GateError(f"world-seam policy is undefined by {source}")
        if WORLD_SEAM_DIAG_BUILD_ID_MACRO in undefinitions:
            raise GateError(f"world-seam build ID is undefined by {source}")
        values = definitions.get(WORLD_SEAM_DIAG_CACHE_KEY, [])
        if values:
            if values != ["1"]:
                raise GateError(
                    f"{source.name} has a non-canonical world-seam "
                    f"definition: {values}"
                )
            actual.add(source.name)
        build_ids = definitions.get(WORLD_SEAM_DIAG_BUILD_ID_MACRO, [])
        if build_ids:
            if not active or build_ids != [expected_build_id]:
                raise GateError(
                    f"{source.name} world-seam build ID disagrees with "
                    f"{GUEST_LINK_ID_CACHE_KEY}: {build_ids}"
                )
            actual_build_id.add(source.name)
        if active and re.fullmatch(r"guest_(?:[0-9]{4}|stubs|table)\.c",
                                   source.name):
            try:
                text = source.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                raise GateError(
                    f"cannot inspect world-seam generated owner {source}: "
                    f"{exc}"
                ) from exc
            roots = text.count("void sub_002ce770(CPU *__restrict c)")
            seams = sum(text.count(name + "();") for name in (
                "kage_vita_world_seam_diag_post_room",
                "kage_vita_world_seam_diag_post_lua",
                "kage_vita_world_seam_diag_pre_hud",
            ))
            seams += text.count("kage_vita_world_seam_diag_begin(") - \
                text.count("extern void kage_vita_world_seam_diag_begin(")
            if roots or seams:
                generated_owners.add(source.name)
                owner_count += roots
                seam_count += seams

    expected = set()
    expected_build_id_owners = set()
    if active:
        if owner_count != 1 or seam_count != 4 or \
                len(generated_owners) != 1:
            raise GateError(
                "world-seam generated owner/seam census changed: "
                f"owners={owner_count}, seams={seam_count}, "
                f"files={sorted(generated_owners)}"
            )
        expected = set(WORLD_SEAM_DIAG_RUNTIME_DEFINITION_OWNERS) | \
            generated_owners
        expected_build_id_owners = {"kage_vita_world_seam_diag.c"}
    if actual != expected:
        raise GateError(
            "world-seam compile-definition scope changed: "
            f"missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )
    if actual_build_id != expected_build_id_owners:
        raise GateError(
            "world-seam build-ID scope changed: "
            f"missing={sorted(expected_build_id_owners - actual_build_id)}, "
            f"extra={sorted(actual_build_id - expected_build_id_owners)}"
        )


def _command_tokens(entry: Mapping[str, object]) -> list[str]:
    arguments = entry.get("arguments")
    if isinstance(arguments, list) and all(isinstance(item, str) for item in arguments):
        return list(arguments)
    command = entry.get("command")
    if not isinstance(command, str):
        raise GateError("compile record has neither arguments nor command")
    try:
        return shlex.split(command, posix=True)
    except ValueError as exc:
        raise GateError(f"cannot parse compile command: {exc}") from exc


def _definitions(tokens: Sequence[str]) -> dict[str, list[str]]:
    result: dict[str, list[str]] = collections.defaultdict(list)
    index = 0
    while index < len(tokens):
        token = tokens[index]
        value: str | None = None
        if token == "-D":
            index += 1
            if index >= len(tokens):
                raise GateError("compile command ends after -D")
            value = tokens[index]
        elif token.startswith("-D") and len(token) > 2:
            value = token[2:]
        if value is not None:
            name, _, assigned = value.partition("=")
            result[name].append(assigned if _ else "1")
        index += 1
    return dict(result)


def _undefinitions(tokens: Sequence[str]) -> list[str]:
    result: list[str] = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        value: str | None = None
        if token == "-U":
            index += 1
            if index >= len(tokens):
                raise GateError("compile command ends after -U")
            value = tokens[index]
        elif token.startswith("-U") and len(token) > 2:
            value = token[2:]
        if value is not None:
            result.append(value)
        index += 1
    return result


def _forced_includes(tokens: Sequence[str], directory: Path) -> list[Path]:
    result: list[Path] = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token.startswith("@"):
            raise GateError("compile response files are forbidden by the allocator gate")
        if token == "-include":
            index += 1
            if index >= len(tokens):
                raise GateError("compile command ends after -include")
            result.append(_path(tokens[index], directory))
        elif token.startswith("-include"):
            raise GateError(f"unknown joined forced-include option: {token}")
        index += 1
    return result


def _forbidden_source(path: Path) -> bool:
    name = path.name.lower()
    parts = {part.lower() for part in path.parts}
    return (name == "host_win32.c" or name.startswith("host_win32.c.") or
            "oracle" in name or name.startswith("test_") or
            "_test.c" in name or
            "tests" in parts or "test" in parts)


_FEATURE_VIEW_DIRECTIVE_RE = re.compile(
    r"^[ \t]*#[ \t]*(?:define|undef)[ \t]+([A-Za-z_][A-Za-z0-9_]*)\b",
    re.MULTILINE,
)


def verify_source_feature_view(path: Path) -> None:
    try:
        source = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise GateError(f"cannot inspect production source {path}: {exc}") from exc
    conflicts = sorted(
        {
            match.group(1)
            for match in _FEATURE_VIEW_DIRECTIVE_RE.finditer(source)
            if (match.group(1) in FEATURE_VIEW_MACROS or
                match.group(1) == DIRECT_DEFAULT_CACHE_KEY or
                match.group(1).startswith("__STDC_WANT_"))
        }
    )
    if conflicts:
        raise GateError(
            f"source-local libc feature-view directive conflicts with the "
            f"forced allocator header in {path}: {conflicts}"
        )


TRANSLATED_CPU_CACHE_KEY = "ISAAC_VITA_TRANSLATED_CPU"
TRANSLATED_CPU_HOT_LAYOUT_CACHE_KEY = "ISAAC_VITA_TRANSLATED_CPU_HOT_LAYOUT"
TRANSLATED_CPU_LAYOUT_HEADER_RELATIVE = Path(
    "isaac-generated-overrides/translated-cpu/guest_hot_layout.h"
)
_GENERATED_UNIT_RE = re.compile(r"^guest_[0-9]{4}\.c$")


def translated_cpu_layout_header(cache: Mapping[str, str],
                                 build_dir: Path) -> Path | None:
    """The second forced include generated units carry while the hot layout
    knob is ON.  Older caches predate the keys; absent means OFF."""
    active = cache.get(TRANSLATED_CPU_CACHE_KEY, "OFF").upper()
    layout = cache.get(TRANSLATED_CPU_HOT_LAYOUT_CACHE_KEY, "ON").upper()
    for name, value in ((TRANSLATED_CPU_CACHE_KEY, active),
                        (TRANSLATED_CPU_HOT_LAYOUT_CACHE_KEY, layout)):
        if value not in ("ON", "OFF"):
            raise GateError(f"CMake cache {name} is not ON/OFF: {value!r}")
    if active != "ON" or layout != "ON":
        return None
    header = (build_dir / TRANSLATED_CPU_LAYOUT_HEADER_RELATIVE).resolve()
    if not header.is_file():
        raise GateError(f"translated-cpu layout header is missing: {header}")
    return header


def compile_closure(compile_commands: Path, expected: set[Path],
                    poison_header: Path, require_objects: bool = True,
                    layout_header: Path | None = None
                    ) -> dict[Path, dict[str, object]]:
    try:
        entries = json.loads(compile_commands.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise GateError(f"cannot read compile_commands.json: {exc}") from exc
    if not isinstance(entries, list):
        raise GateError("compile_commands.json root is not a list")
    build_dir = compile_commands.resolve().parent
    target_prefix = f"CMakeFiles/{TARGET}.dir/"
    by_source: dict[Path, dict[str, object]] = {}
    by_object: dict[Path, Path] = {}
    poison_header = poison_header.resolve()
    for raw_entry in entries:
        if not isinstance(raw_entry, dict):
            raise GateError("compile_commands.json has a non-object record")
        output = raw_entry.get("output")
        if not isinstance(output, str):
            continue
        output_posix = output.replace("\\", "/")
        tokens = _command_tokens(raw_entry)
        directory_value = raw_entry.get("directory")
        if not isinstance(directory_value, str):
            raise GateError("compile record lacks directory")
        directory = _path(directory_value)
        includes = _forced_includes(tokens, directory)
        definitions = _definitions(tokens)
        undefinitions = _undefinitions(tokens)
        is_target = (output_posix.startswith(target_prefix) and
                     output_posix.endswith(".obj"))
        if not is_target:
            gate_definitions = sorted(
                name for name in definitions
                if name in ("ISAAC_VITA_RAW_ALLOCATOR_GATE",
                            "ISAAC_VITA_RAW_ALLOCATOR_EXEMPT")
            )
            if poison_header in includes or gate_definitions:
                raise GateError(
                    "allocator forced include/definition leaked into external "
                    f"compile output {output_posix}: includes={includes}, "
                    f"definitions={gate_definitions}"
                )
            continue
        source_value = raw_entry.get("file")
        if not isinstance(source_value, str) or not isinstance(directory_value, str):
            raise GateError("target compile record lacks file/directory")
        if directory != build_dir:
            raise GateError(f"target compile record uses another build directory: {directory}")
        source = _path(source_value, directory)
        object_path = _path(output, directory)
        if source in by_source:
            raise GateError(f"duplicate compile source: {source}")
        if object_path in by_object:
            raise GateError(f"duplicate compile output: {object_path}")
        if _forbidden_source(source):
            raise GateError(f"forbidden test/oracle/host source entered production: {source}")
        verify_source_feature_view(source)
        if DIRECT_DEFAULT_CACHE_KEY in undefinitions:
            raise GateError(
                f"direct-default compile policy is undefined by {source}"
            )
        if require_objects and not object_path.is_file():
            raise GateError(f"compiled target object is missing: {object_path}")
        expected_includes = [poison_header]
        if layout_header is not None and _GENERATED_UNIT_RE.match(source.name):
            expected_includes.append(layout_header)
        if includes != expected_includes:
            raise GateError(
                f"{source.name} forced-include closure is {includes}, "
                f"expected exactly {expected_includes}"
            )
        if definitions.get("ISAAC_VITA_RAW_ALLOCATOR_GATE") != ["1"]:
            raise GateError(f"{source.name} lacks the exact allocator gate marker")
        exempt = source.parent.name == "runtime" and source.name in EXEMPT_RUNTIME
        exemption = definitions.get("ISAAC_VITA_RAW_ALLOCATOR_EXEMPT", [])
        if exempt and exemption != ["1"]:
            raise GateError(f"allocator owner is not exactly exempt: {source}")
        if not exempt and exemption:
            raise GateError(f"allocator exemption leaked into ordinary source: {source}")
        if any(name in undefinitions for name in
               ("ISAAC_VITA_RAW_ALLOCATOR_GATE",
                "ISAAC_VITA_RAW_ALLOCATOR_EXEMPT")):
            raise GateError(f"allocator gate macro is undefined by {source}")
        record = {
            "source": source,
            "object": object_path,
            "output": output_posix,
            "definitions": definitions,
            "undefinitions": undefinitions,
            "exempt": exempt,
        }
        by_source[source] = record
        by_object[object_path] = source
    actual = set(by_source)
    if actual != expected:
        raise GateError(
            "production compile-source closure changed: "
            f"missing={[str(item) for item in sorted(expected - actual)]}, "
            f"extra={[str(item) for item in sorted(actual - expected)]}"
        )
    return by_source


def _include_directories(tokens: Sequence[str], directory: Path) -> list[Path]:
    result: list[Path] = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        value: str | None = None
        if token == "-I":
            index += 1
            if index >= len(tokens):
                raise GateError("compile command ends after -I")
            value = tokens[index]
        elif token.startswith("-I") and len(token) > 2:
            value = token[2:]
        if value is not None:
            result.append(_path(value, directory))
        index += 1
    return result


def verify_lua_compile_options(tokens: Sequence[str]) -> None:
    if not tokens:
        raise GateError("Lua compile command is empty")
    options: list[str] = []
    index = 1  # compiler executable is an independently-recorded tool input.
    while index < len(tokens):
        token = tokens[index]
        if token == "-I":
            index += 2
            continue
        if token.startswith("-I") and len(token) > 2:
            index += 1
            continue
        if token in ("-o", "-c"):
            index += 2
            continue
        options.append(token)
        index += 1
    if tuple(options) != LUA_REQUIRED_COMPILE_OPTIONS:
        raise GateError(
            "Lua compile option sequence changed: "
            f"{options} != {list(LUA_REQUIRED_COMPILE_OPTIONS)}"
        )


def verify_lua_compile_io(
    tokens: Sequence[str], directory: Path, source: Path, object_path: Path
) -> None:
    outputs: list[Path] = []
    sources: list[Path] = []
    index = 1
    while index < len(tokens):
        token = tokens[index]
        if token in ("-o", "-c"):
            if index + 1 >= len(tokens):
                raise GateError(f"Lua compile command ends after {token}")
            value = _path(tokens[index + 1], directory)
            (outputs if token == "-o" else sources).append(value)
            index += 2
            continue
        index += 1
    if outputs != [object_path] or sources != [source]:
        raise GateError(
            "Lua compile command/file/output binding changed: "
            f"sources={sources}, outputs={outputs}, "
            f"expected_source={source}, expected_output={object_path}"
        )


def lua_compile_closure(
    compile_commands: Path,
    lua_source: Path,
    source_records: Sequence[tuple[str, str]],
    *,
    require_objects: bool = True,
) -> dict[Path, dict[str, object]]:
    """Certify all and only the 33 pristine sources compiled into Lua."""

    try:
        entries = json.loads(compile_commands.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise GateError(f"cannot read compile_commands.json: {exc}") from exc
    if not isinstance(entries, list):
        raise GateError("compile_commands.json root is not a list")
    build_dir = compile_commands.resolve().parent
    target_dir = (build_dir / "CMakeFiles" / f"{LUA_TARGET}.dir").resolve()
    target_prefix = f"CMakeFiles/{LUA_TARGET}.dir/"
    source_dir = (lua_source.resolve() / "src").resolve()
    expected_order = [
        (source_dir / name).resolve()
        for name, unused_sha256 in source_records
        if name.endswith(".c")
    ]
    expected = set(expected_order)
    if len(expected_order) != LUA_C_SOURCE_COUNT or len(expected) != len(expected_order):
        raise GateError("Lua source records are not the exact 33-C closure")
    by_source: dict[Path, dict[str, object]] = {}
    by_object: dict[Path, Path] = {}
    for raw_entry in entries:
        if not isinstance(raw_entry, dict):
            raise GateError("compile_commands.json has a non-object record")
        output = raw_entry.get("output")
        if not isinstance(output, str):
            continue
        output_posix = output.replace("\\", "/")
        if not (output_posix.startswith(target_prefix) and
                output_posix.endswith(".obj")):
            continue
        directory_value = raw_entry.get("directory")
        source_value = raw_entry.get("file")
        if not isinstance(directory_value, str) or not isinstance(source_value, str):
            raise GateError("Lua compile record lacks file/directory")
        directory = _path(directory_value)
        if directory != build_dir:
            raise GateError(
                f"Lua compile record uses another build directory: {directory}"
            )
        source = _path(source_value, directory)
        object_path = _path(output, directory)
        try:
            object_path.relative_to(target_dir)
        except ValueError as exc:
            raise GateError(
                f"Lua compile output escaped its target directory: {object_path}"
            ) from exc
        if source in by_source:
            raise GateError(f"duplicate Lua compile source: {source}")
        if object_path in by_object:
            raise GateError(f"duplicate Lua compile output: {object_path}")
        if require_objects and not object_path.is_file():
            raise GateError(f"compiled Lua object is missing: {object_path}")
        tokens = _command_tokens(raw_entry)
        if _forced_includes(tokens, directory):
            raise GateError(f"Lua compile gained a forced include: {source}")
        definitions = _definitions(tokens)
        undefinitions = _undefinitions(tokens)
        if definitions != {"NDEBUG": ["1"]} or undefinitions:
            raise GateError(
                f"Lua compile preprocessor policy changed for {source.name}: "
                f"definitions={definitions}, undefinitions={undefinitions}"
            )
        verify_lua_compile_options(tokens)
        verify_lua_compile_io(
            tokens, directory, source, object_path
        )
        includes = _include_directories(tokens, directory)
        if includes != [source_dir]:
            raise GateError(
                f"Lua compile include closure changed for {source.name}: "
                f"{includes} != {[source_dir]}"
            )
        record = {
            "source": source,
            "object": object_path,
            "output": output_posix,
        }
        by_source[source] = record
        by_object[object_path] = source
    actual = set(by_source)
    if actual != expected:
        raise GateError(
            "Lua compile-source closure changed: "
            f"missing={[str(item) for item in sorted(expected - actual)]}, "
            f"extra={[str(item) for item in sorted(actual - expected)]}"
        )
    if [by_source[source]["object"].name for source in expected_order] != [
            source.name + ".obj" for source in expected_order]:
        raise GateError("Lua compile object basenames changed")
    return by_source


def _ninja_tokens(text: str) -> list[str]:
    result: list[str] = []
    token: list[str] = []
    index = 0
    while index < len(text):
        char = text[index]
        if char.isspace():
            if token:
                result.append("".join(token))
                token = []
            index += 1
            continue
        if char == "$":
            index += 1
            if index >= len(text):
                raise GateError("dangling Ninja escape")
            escaped = text[index]
            if escaped in (" ", ":", "$"):
                token.append(escaped)
            elif escaped in ("\n", "\r"):
                pass
            else:
                raise GateError(f"unsupported Ninja escape: ${escaped}")
            index += 1
            continue
        token.append(char)
        index += 1
    if token:
        result.append("".join(token))
    return result


def _ninja_target_link_record(build_ninja: Path) -> tuple[list[str], list[str]]:
    text = build_ninja.read_text(encoding="utf-8")
    # Ninja's physical-line continuation is "$" followed by a newline.
    text = text.replace("$\r\n", "").replace("$\n", "")
    prefix = f"build {TARGET}:"
    lines = text.splitlines()
    indexes = [index for index, line in enumerate(lines) if line.startswith(prefix)]
    if len(indexes) != 1:
        raise GateError(
            f"expected one {TARGET} Ninja link edge, found {len(indexes)}"
        )
    index = indexes[0]
    tokens = _ninja_tokens(lines[index][len(prefix):])
    if len(tokens) < 2:
        raise GateError("target Ninja link edge is empty")
    link_library_lines: list[str] = []
    for line in lines[index + 1:]:
        if line and not line[0].isspace():
            break
        stripped = line.strip()
        if stripped.startswith("LINK_LIBRARIES ="):
            link_library_lines.append(stripped.split("=", 1)[1])
    if len(link_library_lines) > 1:
        raise GateError("target Ninja link edge has duplicate LINK_LIBRARIES")
    libraries = (
        _ninja_tokens(link_library_lines[0]) if link_library_lines else []
    )
    return tokens, libraries


def ninja_link_objects(build_ninja: Path, build_dir: Path) -> list[Path]:
    tokens, unused_libraries = _ninja_target_link_record(build_ninja)
    inputs: list[str] = []
    for token in tokens[1:]:  # first token is the rule name
        if token in ("|", "||"):
            break
        inputs.append(token)
    objects = [_path(item, build_dir) for item in inputs if item.endswith(".obj")]
    if len(objects) != len(inputs):
        unexpected = [item for item in inputs if not item.endswith(".obj")]
        raise GateError(f"non-object direct target link input: {unexpected}")
    if len(objects) != len(set(objects)):
        raise GateError("target Ninja link input contains a duplicate object")
    return objects


def _lua_archive_tokens(tokens: Sequence[str], build_dir: Path) -> list[Path]:
    result: list[Path] = []
    for token in tokens:
        if token in ("|", "||"):
            continue
        candidate_text = token.replace("\\", "/")
        if candidate_text.rsplit("/", 1)[-1] == LUA_ARCHIVE_NAME:
            result.append(_path(token, build_dir))
    return result


def verify_lua_link_closure(
    build_ninja: Path, lua_archive: Path | None
) -> None:
    build_dir = build_ninja.resolve().parent
    edge, libraries = _ninja_target_link_record(build_ninja)
    try:
        implicit_marker = edge.index("|")
        order_only_marker = edge.index("||")
    except ValueError:
        implicit_marker = len(edge)
        order_only_marker = len(edge)
    if order_only_marker < implicit_marker:
        raise GateError("target Ninja dependency separators are out of order")
    direct_archives = _lua_archive_tokens(
        edge[1:implicit_marker], build_dir
    )
    implicit_archives = _lua_archive_tokens(
        edge[implicit_marker + 1:order_only_marker], build_dir
    )
    order_only_archives = _lua_archive_tokens(
        edge[order_only_marker + 1:], build_dir
    )
    library_archives = _lua_archive_tokens(libraries, build_dir)
    if lua_archive is None:
        if (direct_archives or implicit_archives or order_only_archives or
                library_archives):
            raise GateError("Lua archive leaked into an ISAAC_VITA_LUA=OFF link")
        return
    expected = lua_archive.resolve()
    if direct_archives:
        raise GateError(
            "Lua archive entered the direct game-object link closure"
        )
    if implicit_archives != [expected] or order_only_archives != [expected]:
        raise GateError(
            "target Ninja dependencies do not contain the exact sole Lua "
            f"archive: implicit={implicit_archives}, "
            f"order_only={order_only_archives}, expected={[expected]}"
        )
    if library_archives != [expected]:
        raise GateError(
            f"target Ninja LINK_LIBRARIES does not contain the exact sole Lua "
            f"archive: {library_archives} != {[expected]}"
        )


def verify_link_closure(build_ninja: Path,
                        records: Mapping[Path, Mapping[str, object]],
                        lua_archive: Path | None = None) -> list[Path]:
    build_dir = build_ninja.resolve().parent
    linked = ninja_link_objects(build_ninja, build_dir)
    verify_lua_link_closure(build_ninja, lua_archive)
    for path in linked:
        if _forbidden_source(path):
            raise GateError(f"forbidden object entered production link: {path}")
    compiled = {record["object"] for record in records.values()}
    linked_set = set(linked)
    if linked_set != compiled:
        raise GateError(
            "compile/link object closure changed: "
            f"missing={[str(item) for item in sorted(compiled - linked_set)]}, "
            f"extra={[str(item) for item in sorted(linked_set - compiled)]}"
        )
    return linked


_SECTION_RE = re.compile(r"^Relocation section '([^']+)'")
_RELOC_RE = re.compile(
    r"^\s*[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+"
    r"(R_[A-Za-z0-9_]+)\s+[0-9A-Fa-f]+\s+(\S+)"
)


def parse_raw_relocations(text: str) -> collections.Counter[RelocKey]:
    section: str | None = None
    result: collections.Counter[RelocKey] = collections.Counter()
    for line in text.splitlines():
        match = _SECTION_RE.match(line)
        if match:
            section = match.group(1)
            continue
        match = _RELOC_RE.match(line)
        if not match:
            continue
        kind, decorated = match.groups()
        symbol = decorated.split("@", 1)[0]
        if symbol not in RAW_SYMBOLS:
            continue
        if section is None:
            raise GateError(f"raw relocation has no owner section: {line}")
        function = section
        for prefix in (".rel.text.", ".rela.text."):
            if function.startswith(prefix):
                function = function[len(prefix):]
                break
        else:
            raise GateError(
                f"raw allocator relocation is outside a function section: "
                f"{section} -> {symbol}"
            )
        result[(function, symbol, kind)] += 1
    return result


def heap_profile(record: Mapping[str, object]) -> str:
    source = record["source"]
    assert isinstance(source, Path)
    if source.name != "host_vita_heap.c":
        raise GateError(f"allocator profile requested for non-heap source: {source}")
    definitions = record["definitions"]
    assert isinstance(definitions, dict)
    for key in HEAP_CANONICAL_DEFINES:
        values = definitions.get(key, [])
        if values not in ([], ["1"]):
            raise GateError(
                f"host_vita_heap.c has a non-canonical feature definition "
                f"for {key}: {values}"
            )
    active = frozenset(
        key for key in HEAP_CANONICAL_DEFINES
        if definitions.get(key) == ["1"]
    )
    return (PROFILE_CANONICAL if active == HEAP_CANONICAL_DEFINES
            else PROFILE_DIAGNOSTIC)


def allocator_profile(
    records: Mapping[Path, Mapping[str, object]],
) -> str:
    heap_records = [
        record for record in records.values()
        if isinstance(record.get("source"), Path) and
        record["source"].name == "host_vita_heap.c"
    ]
    if len(heap_records) != 1:
        raise GateError(
            f"expected one host_vita_heap.c compile record, found "
            f"{len(heap_records)}"
        )
    return heap_profile(heap_records[0])


def _normalise_heap_relocations(
    actual: collections.Counter[RelocKey],
) -> collections.Counter[RelocKey]:
    result: collections.Counter[RelocKey] = collections.Counter()
    for (function, symbol, kind), count in actual.items():
        match = re.fullmatch(r"(.+)\.constprop\.([0-9]+)", function)
        if match and match.group(1) in _HEAP_CONSTPROP_BASES:
            function = match.group(1) + ".constprop.N"
        result[(function, symbol, kind)] += count
    return result


def verify_heap_diagnostic_relocations(
    source: Path, actual: collections.Counter[RelocKey],
) -> None:
    normalised = _normalise_heap_relocations(actual)
    extra = normalised - HEAP_DIAGNOSTIC_MAX
    if extra:
        raise GateError(
            f"diagnostic heap raw-relocation allowlist changed in {source}: "
            f"extra={sorted(extra.items())}"
        )
    if sum(normalised.values()) > HEAP_DIAGNOSTIC_TOTAL_MAX:
        raise GateError(
            f"diagnostic heap raw-relocation count exceeds measured maximum "
            f"in {source}: {sum(normalised.values())} > "
            f"{HEAP_DIAGNOSTIC_TOTAL_MAX}"
        )
    functions = {function for function, unused_symbol, unused_kind in normalised}
    for alternatives in _HEAP_MUTUALLY_EXCLUSIVE_FUNCTIONS:
        present = alternatives & functions
        if len(present) > 1:
            raise GateError(
                f"diagnostic heap mutually-exclusive owners co-exist in "
                f"{source}: {sorted(present)}"
            )


def expected_relocations(record: Mapping[str, object]
                         ) -> collections.Counter[RelocKey]:
    source = record["source"]
    assert isinstance(source, Path)
    name = source.name
    definitions = record["definitions"]
    assert isinstance(definitions, dict)
    if name in EXPECTED_FIXED:
        return EXPECTED_FIXED[name].copy()
    if name == "kage_vita_texture_memory.c":
        return (EXPECTED_TEXTURE_DIAGNOSTIC.copy()
                if definitions.get("ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC") == ["1"]
                else collections.Counter())
    if name == "host_vita_heap.c":
        if heap_profile(record) != PROFILE_CANONICAL:
            raise GateError(
                "host_vita_heap.c has no exact relocation profile outside "
                "the canonical feature closure"
            )
        return EXPECTED_HEAP_CANONICAL.copy()
    raise GateError(f"unrecognised allocator exemption: {source}")


def object_raw_relocations(readelf: Path, object_path: Path
                           ) -> collections.Counter[RelocKey]:
    try:
        completed = subprocess.run(
            [str(readelf), "-Wr", str(object_path)], check=False,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise GateError(f"cannot execute readelf: {exc}") from exc
    if completed.returncode != 0:
        error = completed.stderr.decode("utf-8", "replace").strip()
        raise GateError(f"readelf failed for {object_path}: {error}")
    return parse_raw_relocations(completed.stdout.decode("utf-8", "replace"))


def _relocation_rows(
    relocations: collections.Counter[RelocKey],
) -> list[dict[str, object]]:
    return [
        {"function": function, "symbol": symbol, "type": kind, "count": count}
        for (function, symbol, kind), count in sorted(relocations.items())
    ]


def _archive_members(ar: Path, archive: Path) -> list[str]:
    try:
        completed = subprocess.run(
            [str(ar), "t", str(archive)], check=False,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise GateError(f"cannot execute CMAKE_AR: {exc}") from exc
    if completed.returncode != 0:
        error = completed.stderr.decode("utf-8", "replace").strip()
        raise GateError(f"CMAKE_AR failed to list {archive}: {error}")
    try:
        text = completed.stdout.decode("utf-8")
    except UnicodeError as exc:
        raise GateError("Lua archive member list is not UTF-8") from exc
    members = text.splitlines()
    if any(not member or member.strip() != member for member in members):
        raise GateError(f"Lua archive has a malformed member name: {members}")
    return members


def _archive_member_payload(ar: Path, archive: Path, member: str) -> bytes:
    try:
        completed = subprocess.run(
            [str(ar), "p", str(archive), member], check=False,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise GateError(f"cannot execute CMAKE_AR: {exc}") from exc
    if completed.returncode != 0:
        error = completed.stderr.decode("utf-8", "replace").strip()
        raise GateError(
            f"CMAKE_AR failed to extract {archive}({member}): {error}"
        )
    return completed.stdout


def verify_lua_archive_members(
    actual_members: Sequence[str], expected_members: Sequence[str]
) -> None:
    if list(actual_members) != list(expected_members):
        raise GateError(
            "Lua archive member closure changed: "
            f"{list(actual_members)} != {list(expected_members)}"
        )
    if (len(actual_members) != LUA_C_SOURCE_COUNT or
            len(set(actual_members)) != LUA_C_SOURCE_COUNT):
        raise GateError("Lua archive is not the exact unique 33-member closure")


def verify_lua_archive_member_payload(
    member: str, member_payload: bytes, object_payload: bytes
) -> None:
    if member_payload != object_payload:
        raise GateError(
            f"Lua archive member differs from its compile output: {member}"
        )


def verify_lua_allocator_relocations(
    source_name: str, actual: collections.Counter[RelocKey]
) -> None:
    expected = (
        LUA_EXPECTED_RAW_RELOCATIONS
        if source_name == "lauxlib.c" else collections.Counter()
    )
    if actual != expected:
        raise GateError(
            f"Lua raw allocator relocation policy changed in {source_name}: "
            f"missing={sorted((expected - actual).items())}, "
            f"extra={sorted((actual - expected).items())}"
        )


def _defined_symbol_counts(readelf: Path, target: Path) -> collections.Counter[str]:
    try:
        completed = subprocess.run(
            [str(readelf), "-Ws", str(target)], check=False,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise GateError(f"cannot execute readelf: {exc}") from exc
    if completed.returncode != 0:
        error = completed.stderr.decode("utf-8", "replace").strip()
        raise GateError(f"readelf failed for {target}: {error}")
    result: collections.Counter[str] = collections.Counter()
    for line in completed.stdout.decode("utf-8", "replace").splitlines():
        fields = line.split()
        if len(fields) < 8 or not fields[0].endswith(":"):
            continue
        section = fields[-2]
        name = fields[-1].split("@", 1)[0]
        if section != "UND":
            result[name] += 1
    return result


def verify_lua_final_symbols(readelf: Path, target: Path) -> None:
    symbols = _defined_symbol_counts(readelf, target)
    if symbols[LUA_REQUIRED_FINAL_SYMBOL] < 1:
        raise GateError(
            f"final ELF does not retain required Lua symbol: "
            f"{LUA_REQUIRED_FINAL_SYMBOL}"
        )
    retained = sorted(
        symbol for symbol in LUA_DISCARDED_ALLOCATOR_SYMBOLS
        if symbols[symbol]
    )
    if retained:
        raise GateError(
            "final ELF retained the dead Lua allocator owner(s): "
            f"{retained}"
        )


def lua_archive_closure(
    *,
    ar: Path,
    readelf: Path,
    archive: Path,
    target: Path,
    source_records: Sequence[tuple[str, str]],
    compile_records: Mapping[Path, Mapping[str, object]],
    lua_source: Path,
) -> dict[str, object]:
    """Prove source -> compile object -> exact archive member -> final ELF."""

    if archive.is_symlink() or not archive.is_file():
        raise GateError(f"Lua archive is not a regular non-symlink file: {archive}")
    if ar.is_symlink() or not ar.is_file():
        raise GateError(f"CMAKE_AR is not a regular non-symlink file: {ar}")
    source_dir = (lua_source.resolve() / "src").resolve()
    c_records = [record for record in source_records if record[0].endswith(".c")]
    expected_members: list[str] = []
    ordered: list[tuple[str, str, Path, Path]] = []
    for name, source_sha256 in c_records:
        source = (source_dir / name).resolve()
        record = compile_records.get(source)
        if record is None:
            raise GateError(f"Lua archive source has no compile record: {name}")
        object_path = record.get("object")
        if not isinstance(object_path, Path):
            raise GateError(f"Lua compile object is malformed: {name}")
        member = object_path.name
        expected_members.append(member)
        ordered.append((name, source_sha256, source, object_path))
    actual_members = _archive_members(ar, archive)
    verify_lua_archive_members(actual_members, expected_members)

    member_rows: list[dict[str, object]] = []
    for name, source_sha256, source, object_path in ordered:
        member = object_path.name
        try:
            object_payload = object_path.read_bytes()
        except OSError as exc:
            raise GateError(f"cannot read compiled Lua object {object_path}: {exc}") from exc
        member_payload = _archive_member_payload(ar, archive, member)
        verify_lua_archive_member_payload(
            member, member_payload, object_payload
        )
        actual_relocations = object_raw_relocations(readelf, object_path)
        verify_lua_allocator_relocations(name, actual_relocations)
        logical_source = f"{LUA_LOGICAL_SOURCE_PREFIX}/{name}"
        logical_object = _logical_object(logical_source)
        member_rows.append(
            {
                "source": logical_source,
                "source_sha256": source_sha256,
                "object": logical_object,
                "member": member,
                "size": len(object_payload),
                "sha256": hashlib.sha256(object_payload).hexdigest(),
                "raw_relocations": _relocation_rows(actual_relocations),
            }
        )
    verify_lua_final_symbols(readelf, target)
    archive_record = _file_record(archive)
    return {
        "path": LUA_ARCHIVE_NAME,
        **archive_record,
        "manifest_sha256": LUA_MANIFEST_SHA256,
        "source_set_sha256": lua_source_set_sha256(source_records),
        "source_count": len(c_records),
        "member_count": len(member_rows),
        "members": member_rows,
        "allocator_policy": {
            "required_final_symbol": LUA_REQUIRED_FINAL_SYMBOL,
            "discarded_final_symbols": sorted(LUA_DISCARDED_ALLOCATOR_SYMBOLS),
            "dead_raw_relocations": _relocation_rows(
                LUA_EXPECTED_RAW_RELOCATIONS
            ),
        },
    }


def verify_record_relocations(record: Mapping[str, object],
                              actual: collections.Counter[RelocKey],
                              profile: str | None = None) -> None:
    source = record["source"]
    assert isinstance(source, Path)
    if record["exempt"]:
        if source.name == "host_vita_heap.c":
            actual_profile = heap_profile(record)
            if profile is None:
                profile = actual_profile
            if profile != actual_profile:
                raise GateError(
                    f"allocator profile disagrees with heap compile features: "
                    f"{profile} != {actual_profile}"
                )
            if profile == PROFILE_DIAGNOSTIC:
                verify_heap_diagnostic_relocations(source, actual)
                return
        expected = expected_relocations(record)
        if actual != expected:
            raise GateError(
                f"raw allocator relocation contract changed in {source}: "
                f"missing={sorted((expected - actual).items())}, "
                f"extra={sorted((actual - expected).items())}"
            )
    elif actual:
        raise GateError(
            f"ordinary source retained raw allocator relocations: "
            f"{source}: {sorted(actual.items())}"
        )


def verify_relocations(readelf: Path,
                       records: Mapping[Path, Mapping[str, object]],
                       profile: str) -> dict[Path, collections.Counter[RelocKey]]:
    observed: dict[Path, collections.Counter[RelocKey]] = {}
    for source in sorted(records):
        record = records[source]
        object_path = record["object"]
        assert isinstance(object_path, Path)
        actual = object_raw_relocations(readelf, object_path)
        verify_record_relocations(record, actual, profile)
        observed[source] = actual
    return observed


def _logical_source(path: Path, source_root: Path,
                    generated_root: Path) -> str:
    resolved = path.resolve()
    source_root = source_root.resolve()
    generated_root = generated_root.resolve()
    if source_root == generated_root:
        raise GateError(
            "source and generated roots resolve to the same directory: "
            f"{source_root}"
        )
    owners = ((source_root, ""), (generated_root, "generated/"))
    for root, prefix in sorted(
        owners, key=lambda item: len(item[0].parts), reverse=True
    ):
        try:
            return prefix + resolved.relative_to(root).as_posix()
        except ValueError:
            pass
    raise GateError(f"production source has no logical closure owner: {path}")


def _logical_object(logical_source: str) -> str:
    if (not logical_source or "\\" in logical_source or
            ":" in logical_source or logical_source.startswith("/") or
            any(part in ("", ".", "..")
                for part in logical_source.split("/"))):
        raise GateError(
            f"production source has a malformed logical path: {logical_source!r}"
        )
    return f"{LOGICAL_OBJECT_PREFIX}{logical_source}.obj"


def logical_closure(
    records: Mapping[Path, Mapping[str, object]],
    raw: Mapping[Path, collections.Counter[RelocKey]],
    linked: Sequence[Path],
    source_root: Path,
    generated_root: Path,
    generated_source_aliases: Mapping[Path, str] | None = None,
) -> dict[str, object]:
    """Translate a verified physical closure into path-independent identities."""

    record_sources = set(records)
    raw_sources = set(raw)
    if raw_sources != record_sources:
        raise GateError(
            "raw-relocation/source closure changed: "
            f"missing={[str(item) for item in sorted(record_sources - raw_sources)]}, "
            f"extra={[str(item) for item in sorted(raw_sources - record_sources)]}"
        )

    aliases: dict[Path, str] = {}
    for physical, logical in (generated_source_aliases or {}).items():
        if not isinstance(physical, Path) or not physical.is_absolute():
            raise GateError(
                f"generated source alias has a non-absolute owner: {physical!r}"
            )
        if (not isinstance(logical, str) or
                not re.fullmatch(
                    r"generated/guest_(?:[0-9]{4}|stubs|table)\.c", logical
                )):
            raise GateError(
                f"generated source alias has a malformed logical path: {logical!r}"
            )
        resolved = physical.resolve()
        if resolved in aliases:
            raise GateError(
                f"duplicate physical generated source alias: {resolved}"
            )
        _logical_object(logical)
        aliases[resolved] = logical
    unused_aliases = set(aliases)

    source_rows: list[dict[str, object]] = []
    physical_sources: set[Path] = set()
    logical_sources: set[str] = set()
    logical_objects: set[str] = set()
    physical_to_logical_object: dict[Path, str] = {}
    for source, record in records.items():
        if not isinstance(source, Path):
            raise GateError(f"compile closure has a non-path source key: {source!r}")
        physical_source = source.resolve()
        if physical_source in physical_sources:
            raise GateError(f"duplicate physical compile source: {physical_source}")
        physical_sources.add(physical_source)
        record_source = record.get("source")
        if (not isinstance(record_source, Path) or
                record_source.resolve() != physical_source):
            raise GateError(f"compile source key/record mismatch: {source}")

        logical_source = aliases.get(physical_source)
        if logical_source is None:
            logical_source = _logical_source(
                physical_source, source_root, generated_root
            )
        else:
            unused_aliases.remove(physical_source)
        logical_object = _logical_object(logical_source)
        if logical_source in logical_sources:
            raise GateError(f"duplicate logical compile source: {logical_source}")
        if logical_object in logical_objects:
            raise GateError(f"duplicate logical compile object: {logical_object}")
        logical_sources.add(logical_source)
        logical_objects.add(logical_object)

        object_path = record.get("object")
        if not isinstance(object_path, Path):
            raise GateError(f"compile record has a non-path object: {source}")
        physical_object = object_path.resolve()
        if physical_object in physical_to_logical_object:
            raise GateError(f"duplicate physical compile object: {physical_object}")
        physical_to_logical_object[physical_object] = logical_object

        relocations = [
            {"function": function, "symbol": symbol, "type": kind,
             "count": count}
            for (function, symbol, kind), count in sorted(raw[source].items())
        ]
        source_rows.append(
            {
                "source": logical_source,
                "object": logical_object,
                "exempt": bool(record["exempt"]),
                "raw_relocations": relocations,
            }
        )
    if unused_aliases:
        raise GateError(
            "generated source alias has no compile-source mapping: "
            f"{[str(item) for item in sorted(unused_aliases)]}"
        )
    source_rows.sort(key=lambda row: str(row["source"]))

    linked_objects: list[str] = []
    linked_physical: set[Path] = set()
    linked_logical: set[str] = set()
    for value in linked:
        if not isinstance(value, Path):
            raise GateError(f"link closure has a non-path object: {value!r}")
        physical_object = value.resolve()
        if physical_object in linked_physical:
            raise GateError(f"duplicate physical linked object: {physical_object}")
        linked_physical.add(physical_object)
        logical_object = physical_to_logical_object.get(physical_object)
        if logical_object is None:
            raise GateError(
                f"linked object has no compile-source mapping: {physical_object}"
            )
        if logical_object in linked_logical:
            raise GateError(f"duplicate logical linked object: {logical_object}")
        linked_logical.add(logical_object)
        linked_objects.append(logical_object)

    missing = set(physical_to_logical_object) - linked_physical
    if missing:
        raise GateError(
            "logical link closure is missing compiled objects: "
            f"{[str(item) for item in sorted(missing)]}"
        )
    return {"sources": source_rows, "linked_objects": linked_objects}


def run_gate(args: argparse.Namespace) -> dict[str, object]:
    source_root = args.source_root.resolve()
    generated_manifest = args.generated_manifest.resolve()
    generated_root = generated_manifest.parent
    cmake_cache = args.cmake_cache.resolve()
    compile_commands = args.compile_commands.resolve()
    build_ninja = args.build_ninja.resolve()
    poison_header = args.poison_header.resolve()
    target = args.target.resolve()
    readelf = args.readelf.resolve()
    receipt = args.receipt.resolve()
    for path in (generated_manifest, cmake_cache, compile_commands, build_ninja,
                 poison_header, target, readelf):
        if not path.is_file():
            raise GateError(f"gate input is not a regular file: {path}")
    cache = read_cache(cmake_cache)
    if _feature(cache, "ISAAC_VITA_SCAFFOLD_ONLY"):
        raise GateError("raw allocator gate cannot certify a scaffold target")
    lua_paths = lua_gate_paths(
        cache,
        source_root=source_root,
        build_dir=compile_commands.parent,
        lua_source=args.lua_source,
        lua_manifest=args.lua_manifest,
        lua_archive=args.lua_archive,
        ar=args.ar,
    )
    lua_source: Path | None = None
    lua_manifest: Path | None = None
    lua_archive: Path | None = None
    ar: Path | None = None
    lua_source_manifest: tuple[tuple[str, str], ...] = ()
    lua_compile_records: dict[Path, dict[str, object]] = {}
    if lua_paths is not None:
        lua_source, lua_manifest, lua_archive, ar = lua_paths
        for path in (lua_manifest, lua_archive, ar):
            if not path.is_file():
                raise GateError(f"Lua gate input is not a regular file: {path}")
        lua_source_manifest = lua_source_records(lua_source, lua_manifest)
        lua_compile_records = lua_compile_closure(
            compile_commands, lua_source, lua_source_manifest
        )
    generated_overrides = direct_default_overrides(
        cache, args.generated_source_override, compile_commands.parent
    )
    expected = expected_sources(
        source_root, generated_manifest, cache, generated_overrides
    )
    records = compile_closure(
        compile_commands, expected, poison_header,
        layout_header=translated_cpu_layout_header(cache, compile_commands.parent),
    )
    verify_direct_default_compile_scope(records, cache)
    verify_stock_reference_compile_scope(records, cache)
    verify_display_raster_compile_scope(records, cache)
    verify_typed_state_cache_compile_scope(records, cache)
    verify_game_log_batch_compile_scope(records, cache)
    verify_sync_import_fastpath_compile_scope(records, cache)
    verify_png_native_unfilter_compile_scope(records, cache)
    verify_png_inflate_flush_fastpath_compile_scope(records, cache)
    verify_png_crc32_fastpath_compile_scope(records, cache)
    verify_archive_miniz_fastpath_compile_scope(records, cache)
    verify_save_checksum_fastpath_compile_scope(records, cache)
    verify_save_reader_fastpath_compile_scope(records, cache)
    verify_save_read32_fused_compile_scope(records, cache)
    verify_save_reader_direct_edges_compile_scope(records, cache)
    verify_memset_thunk_fastpath_compile_scope(records, cache)
    verify_kage_mutex_seam_compile_scope(records, cache)
    verify_kage_refcount_seam_compile_scope(records, cache)
    verify_floor_thunk_fastpath_compile_scope(records, cache)
    verify_exit_menu_profile_compile_scope(records, cache)
    verify_png_decode_profile_compile_scope(records, cache)
    verify_texture_churn_profile_compile_scope(records, cache)
    verify_audio_stream_receipt_compile_scope(records, cache)
    verify_sim_cadence_receipt_compile_scope(records, cache)
    verify_stable30_compile_scope(records, cache)
    verify_world_seam_diag_compile_scope(records, cache)
    linked = verify_link_closure(build_ninja, records, lua_archive)
    profile = allocator_profile(records)
    raw = verify_relocations(readelf, records, profile)
    closure = logical_closure(
        records,
        raw,
        linked,
        source_root,
        generated_root,
        {
            physical: f"generated/{name}"
            for name, physical in generated_overrides.items()
        },
    )
    lua_archive_record: dict[str, object] | None = None
    if lua_paths is not None:
        assert lua_source is not None
        assert lua_manifest is not None
        assert lua_archive is not None
        assert ar is not None
        lua_archive_record = lua_archive_closure(
            ar=ar,
            readelf=readelf,
            archive=lua_archive,
            target=target,
            source_records=lua_source_manifest,
            compile_records=lua_compile_records,
            lua_source=lua_source,
        )
        closure["lua_archive"] = lua_archive_record
    source_rows = closure["sources"]
    assert isinstance(source_rows, list)
    lua_object_count = (
        int(lua_archive_record["member_count"])
        if lua_archive_record is not None else 0
    )
    result: dict[str, object] = {
        "schema": SCHEMA,
        "stage": "vita-raw-allocator-gate",
        "result": "PASS",
        "target_name": TARGET,
        "profile": profile,
        "raw_symbols": sorted(RAW_SYMBOLS),
        "forbidden_source_feature_view_macros": sorted(FEATURE_VIEW_MACROS),
        "exempt_sources": sorted(
            row["source"] for row in source_rows if row["exempt"]
        ),
        "source_count": len(source_rows) + lua_object_count,
        "object_count": len(linked) + lua_object_count,
        "closure_sha256": hashlib.sha256(_canonical_bytes(closure)).hexdigest(),
        "target": _file_record(target),
        "inputs": {
            "generated_manifest": _file_record(generated_manifest),
            "cmake_cache": _file_record(cmake_cache),
            "compile_commands": _file_record(compile_commands),
            "build_ninja": _file_record(build_ninja),
            "poison_header": _file_record(poison_header),
            "gate": _file_record(Path(__file__).resolve()),
            "readelf": _file_record(readelf),
        },
        "closure": closure,
    }
    if lua_archive_record is not None:
        assert lua_manifest is not None
        assert lua_archive is not None
        assert ar is not None
        result["inputs"].update({
            "lua_manifest": _file_record(lua_manifest),
            "lua_archive": _file_record(lua_archive),
            "lua_ar": _file_record(ar),
        })
    if generated_overrides:
        override = generated_overrides[DIRECT_DEFAULT_SOURCE_NAME]
        result["inputs"]["generated_source_override"] = {
            "logical_source": f"generated/{DIRECT_DEFAULT_SOURCE_NAME}",
            **_file_record(override),
        }
    receipt.parent.mkdir(parents=True, exist_ok=True)
    temporary = receipt.with_name(receipt.name + ".tmp")
    if temporary.exists():
        temporary.unlink()
    temporary.write_bytes(_canonical_bytes(result) + b"\n")
    os.replace(temporary, receipt)
    return result


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--generated-manifest", required=True, type=Path)
    parser.add_argument("--cmake-cache", required=True, type=Path)
    parser.add_argument("--compile-commands", required=True, type=Path)
    parser.add_argument("--build-ninja", required=True, type=Path)
    parser.add_argument("--poison-header", required=True, type=Path)
    parser.add_argument("--target", required=True, type=Path)
    parser.add_argument("--readelf", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    parser.add_argument("--generated-source-override", type=Path)
    parser.add_argument("--lua-source", type=Path)
    parser.add_argument("--lua-manifest", type=Path)
    parser.add_argument("--lua-archive", type=Path)
    parser.add_argument("--ar", type=Path)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = build_parser().parse_args(argv)
        result = run_gate(args)
    except GateError as exc:
        print(f"Vita raw allocator gate: FAIL: {exc}", file=sys.stderr)
        return 1
    print(
        "Vita raw allocator gate: PASS; "
        f"profile={result['profile']} sources={result['source_count']} "
        f"objects={result['object_count']} "
        f"closure={result['closure_sha256']} receipt={args.receipt.resolve()}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
