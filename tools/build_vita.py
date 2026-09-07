#!/usr/bin/env python3
"""Reproducible one-command local PS Vita build.

This wrapper intentionally does not acquire or copy the proprietary game PE.
The caller supplies an already-unpacked, legally obtained executable.  The
wrapper rebuilds every generated stage from that file, verifies the generated
content contract, configures CMake, builds without a shell pipeline, and prints
    content identities for the four build artefacts and writes their canonical
    provenance to ``build-manifest.json`` only after all checks pass.

Typical use::

    python tools/build_vita.py \
        --pe /path/to/isaac-ng.exe.unpacked.exe \
        --vitasdk /path/to/vitasdk-softfp

Run the deterministic host-only checks with::

    python tools/build_vita.py --self-test
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import importlib.metadata
import json
import os
import platform
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import zipfile


REPO_ROOT = Path(__file__).resolve().parents[1]
RECOMP_DIR = REPO_ROOT / "recomp"
VITA_SOURCE_DIR = RECOMP_DIR / "vita"
DETERMINISTIC_VPK_WRAPPER = VITA_SOURCE_DIR / "deterministic_vpk.py"
DETERMINISTIC_VPK_CMAKE = VITA_SOURCE_DIR / "deterministic_vpk.cmake"
LUA53_CMAKE_MANIFEST = VITA_SOURCE_DIR / "lua53_vanilla.cmake"
RELEASE_AUDIT_PATH = REPO_ROOT / "tools" / "release_audit.py"
BUILD_MANIFEST_NAME = "build-manifest.json"
BUILD_MANIFEST_SCHEMA = 3
BUILD_ID_DOMAIN = b"build-id-v3\0"
RECEIPT_ID_DOMAIN = b"receipt-id-v3\0"
LINKER_MAP_NAME = "isaac_first_arm_fault.map"
VPK_LIVEAREA_ASSETS = (
    (
        "sce_sys/icon0.png",
        13_038,
        "0c0b2c1d637edfa5222ecef970f7a54e143c9aed70d094644f6aaadeea157f46",
    ),
    (
        "sce_sys/pic0.png",
        137_840,
        "7d8f862809ef87ea4dd69b7446b112abf4549f8a6e2b968e76a31157ccc68148",
    ),
    (
        "sce_sys/livearea/contents/bg0.png",
        169_345,
        "cff8dc535bad6f3e0f34e11c78488f1146b4e38c9f1e8d217f63e5278fc24661",
    ),
    (
        "sce_sys/livearea/contents/nicalis.png",
        4_060,
        "4836922217e21cc29f16b97f8a05567316b70e4d203c7084eba4145af6b112ec",
    ),
    (
        "sce_sys/livearea/contents/startup.png",
        8_982,
        "2600254658cc2667d787d60722c82779353204b4d16c4aecf0b4ca72791f0000",
    ),
    (
        "sce_sys/livearea/contents/template.xml",
        1_210,
        "889dd7af1c66ad86a2b765a239ee8f93356b023cdf984f8a48ca154500f5c945",
    ),
)
VPK_LIVEAREA_MEMBERS = tuple(
    member for member, unused_size, unused_sha256 in VPK_LIVEAREA_ASSETS
)
VPK_LIVEAREA_MEMBER_SET = frozenset(VPK_LIVEAREA_MEMBERS)
VPK_MANAGER_MEMBER = "isaac-manager.bin"
VPK_MANAGER_LICENSE_MEMBER = "licenses/libftpvita.txt"
VPK_MANAGER_LICENSE_SOURCE = (
    VITA_SOURCE_DIR / "third_party" / "libftpvita" / "LICENSE"
)
RAW_ALLOCATOR_GATE_RECEIPT_NAME = "vita-raw-allocator-gate.json"
RAW_ALLOCATOR_GATE_RAW_SYMBOLS = (
    "aligned_alloc", "calloc", "free", "malloc", "memalign", "realloc",
    "strdup",
)
RAW_ALLOCATOR_GATE_FORBIDDEN_SOURCE_FEATURE_VIEW_MACROS = (
    "_ATFILE_SOURCE", "_BSD_SOURCE", "_DARWIN_C_SOURCE",
    "_DEFAULT_SOURCE", "_FILE_OFFSET_BITS", "_FORTIFY_SOURCE",
    "_GNU_SOURCE", "_ISOC11_SOURCE", "_ISOC2X_SOURCE",
    "_LARGEFILE64_SOURCE", "_LARGEFILE_SOURCE", "_POSIX_C_SOURCE",
    "_POSIX_SOURCE", "_REENTRANT", "_SVID_SOURCE", "_THREAD_SAFE",
    "_TIME_BITS", "_XOPEN_SOURCE", "_XOPEN_SOURCE_EXTENDED",
)
RAW_ALLOCATOR_GATE_EXEMPT_SOURCES = (
    "recomp/runtime/guest.c",
    "recomp/runtime/host_vita_heap.c",
    "recomp/runtime/host_vita_openal_pool.c",
    "recomp/runtime/kage_vita_texture_memory.c",
)
RAW_ALLOCATOR_GATE_BASE_INPUTS = frozenset((
    "build_ninja", "cmake_cache", "compile_commands", "gate",
    "generated_manifest", "poison_header", "readelf",
))
RAW_ALLOCATOR_GATE_LUA_INPUTS = frozenset((
    "lua_ar", "lua_archive", "lua_manifest",
))
RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_INPUT = "generated_source_override"
RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_LOGICAL_SOURCE = "generated/guest_0144.c"
RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_OUTPUT = Path(
    "isaac-generated-overrides/direct-default/guest_0144.c"
)
RAW_ALLOCATOR_GATE_RECEIPT_KEYS = frozenset((
    "schema", "stage", "result", "target_name", "profile", "raw_symbols",
    "forbidden_source_feature_view_macros", "exempt_sources",
    "source_count", "object_count", "closure_sha256", "target", "inputs",
    "closure",
))
RAW_ALLOCATOR_GATE_LOGICAL_OBJECT_PREFIX = "objects/"
RAW_ALLOCATOR_GATE_PROFILE_CANONICAL = "canonical-exact"
RAW_ALLOCATOR_GATE_PROFILE_DIAGNOSTIC = "diagnostic-allowlist"
RAW_ALLOCATOR_GATE_CANONICAL_CACHE_FLAGS = (
    "ISAAC_VITA_ANM2_SCRATCH",
    "ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH",
    "ISAAC_VITA_HEAP_LEDGER_BACKSHIFT",
    "ISAAC_VITA_HEAP_LEDGER_MEMBLOCK",
    "ISAAC_VITA_HEAP_OVERFLOW_MSPACE",
    "ISAAC_VITA_ROOM_ENTRY_HYBRID",
    "ISAAC_VITA_ROOM_ENTRY_SLAB",
    "ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC",
    "ISAAC_VITA_TEXEL_SCRATCH",
)
RAW_ALLOCATOR_GATE_CANONICAL_ARG_FLAGS = (
    ("anm2_scratch", "--anm2-scratch"),
    ("heap_ledger_backshift", "--heap-ledger-backshift"),
    ("heap_ledger_memblock", "--heap-ledger-memblock"),
    ("heap_overflow_mspace", "--heap-overflow-mspace"),
    ("room_entry_hybrid", "--room-entry-hybrid"),
    ("room_entry_slab", "--room-entry-slab"),
    ("texel_oom_diagnostic", "--texel-oom-diagnostic"),
    ("texel_scratch", "--texel-scratch"),
)
RAW_ALLOCATOR_GATE_HEAP_CANONICAL = {
    ("vita_heap_guest_calloc_impl", "calloc", "R_ARM_THM_CALL"): 1,
    ("vita_heap_guest_calloc_impl", "free", "R_ARM_THM_CALL"): 2,
    ("vita_heap_guest_free_impl", "free", "R_ARM_THM_CALL"): 1,
    ("vita_heap_guest_malloc_with_reason", "free", "R_ARM_THM_CALL"): 2,
    ("vita_heap_guest_malloc_with_reason", "malloc", "R_ARM_THM_CALL"): 1,
    ("vita_heap_guest_realloc_impl.constprop.0", "free",
     "R_ARM_THM_CALL"): 4,
    ("vita_heap_guest_realloc_impl.constprop.0", "malloc",
     "R_ARM_THM_CALL"): 1,
    ("vita_heap_guest_realloc_impl.constprop.0", "realloc",
     "R_ARM_THM_CALL"): 1,
}
RAW_ALLOCATOR_GATE_HEAP_DIAGNOSTIC_MAX = {
    ("vita_heap_guest_calloc_impl", "calloc", "R_ARM_THM_CALL"): 1,
    ("vita_heap_guest_calloc_impl", "free", "R_ARM_THM_CALL"): 2,
    ("vita_heap_guest_calloc_impl.constprop.N", "calloc",
     "R_ARM_THM_CALL"): 1,
    ("vita_heap_guest_calloc_impl.constprop.N", "free",
     "R_ARM_THM_CALL"): 1,
    ("vita_heap_guest_free_impl", "free", "R_ARM_THM_CALL"): 1,
    ("vita_heap_guest_free_impl.constprop.N", "free", "R_ARM_THM_CALL"): 1,
    ("vita_heap_guest_malloc_with_reason", "free", "R_ARM_THM_CALL"): 2,
    ("vita_heap_guest_malloc_with_reason", "malloc", "R_ARM_THM_CALL"): 1,
    ("vita_heap_guest_realloc_impl.constprop.N", "free",
     "R_ARM_THM_CALL"): 4,
    ("vita_heap_guest_realloc_impl.constprop.N", "malloc",
     "R_ARM_THM_CALL"): 1,
    ("vita_heap_guest_realloc_impl.constprop.N", "realloc",
     "R_ARM_THM_CALL"): 1,
    ("vita_heap_ledger_rehash", "calloc", "R_ARM_THM_CALL"): 1,
    ("vita_heap_ledger_rehash", "free", "R_ARM_THM_CALL"): 2,
    ("vita_heap_ledger_reserve", "calloc", "R_ARM_THM_CALL"): 2,
    ("vita_heap_ledger_reserve", "free", "R_ARM_THM_CALL"): 2,
    ("vita_heap_ledger_reserve.constprop.N", "calloc",
     "R_ARM_THM_CALL"): 2,
    ("vita_heap_ledger_reserve.constprop.N", "free", "R_ARM_THM_CALL"): 2,
}
RAW_ALLOCATOR_GATE_HEAP_CONSTPROP_BASES = frozenset((
    "vita_heap_guest_calloc_impl", "vita_heap_guest_free_impl",
    "vita_heap_guest_realloc_impl", "vita_heap_ledger_reserve",
))
RAW_ALLOCATOR_GATE_HEAP_EXCLUSIVE_FUNCTIONS = (
    frozenset(("vita_heap_guest_calloc_impl",
               "vita_heap_guest_calloc_impl.constprop.N")),
    frozenset(("vita_heap_guest_free_impl",
               "vita_heap_guest_free_impl.constprop.N")),
    frozenset(("vita_heap_ledger_rehash", "vita_heap_ledger_reserve",
               "vita_heap_ledger_reserve.constprop.N")),
)
DEFAULT_GENERATED_DIR = Path("..") / "repentogxm-build" / "vita-generated"
STRICT_PATH_SEGMENT_RE = re.compile(r"^[A-Za-z0-9._-]+$")
GENERATED_PATH_SEGMENT_RE = re.compile(r"^[A-Za-z0-9._*?\[\]-]+$")
LUA53_VERSION = "5.3.3"
LUA53_SOURCE_COUNT = 58
LUA53_C_SOURCE_COUNT = 33
LUA53_ARCHIVE_SHA256 = (
    "5113c06884f7de453ce57702abaac1d618307f33f6789fa870e87a59d772aca2"
)
# Copy of the exact tracked manifest file identity, not a recalled source hash.
# The manifest itself contains the 58 individual source hashes used by CMake.
LUA53_CMAKE_MANIFEST_SHA256 = (
    "27977cf039f0855b98840dc77a360a7aaa9d52a5e5780ec031aad08b1794054c"
)
LUA53_LOGICAL_SOURCE_PREFIX = f"lua-{LUA53_VERSION}/src"
LUA53_ARCHIVE_NAME = "libisaac_lua53.a"
LUA53_REQUIRED_FINAL_SYMBOL = "lua_newstate"
LUA53_DISCARDED_ALLOCATOR_SYMBOLS = ("l_alloc", "luaL_newstate")
LUA53_DEAD_RAW_RELOCATIONS = (
    {
        "function": "l_alloc", "symbol": "free",
        "type": "R_ARM_THM_CALL", "count": 1,
    },
    {
        "function": "l_alloc", "symbol": "realloc",
        "type": "R_ARM_THM_JUMP24", "count": 1,
    },
)
LOADING_SPECIALIST_RELATIVE_PATH = (
    "recomp/runtime/kage_vita_loading_specialist.inc"
)

GUEST_BASE = 0x98000000
EXPECTED_PE_SIZE = 8_650_240
EXPECTED_PE_SHA256 = (
    "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
)
EXPECTED_PE_MACHINE = 0x014C
EXPECTED_PE_MAGIC = 0x010B
EXPECTED_PE_IMAGE_BASE = 0x00400000
EXPECTED_PE_ENTRY_RVA = 0x005EB83E
EXPECTED_PE_IMAGE_SIZE = 0x0085F000


def heap_mb_is_supported(heap_mb: int) -> bool:
    """Match the measured Vita allocator/fixed-image placement contract."""

    return 1 <= heap_mb <= 81 and heap_mb not in (64, 80)

HEX_DIGITS = frozenset("0123456789abcdef")
BOOTSTRAP_FLAG = "ISAAC_BUILD_VITA_RECOMP_BOOTSTRAP"
BOOTSTRAP_PE = "ISAAC_BUILD_VITA_RECOMP_PE"
BOOTSTRAP_ANALYSIS = "ISAAC_BUILD_VITA_RECOMP_ANALYSIS"
BOOTSTRAP_GENERATED = "ISAAC_BUILD_VITA_RECOMP_GENERATED"
BOOTSTRAP_KEYS = (
    BOOTSTRAP_FLAG,
    BOOTSTRAP_PE,
    BOOTSTRAP_ANALYSIS,
    BOOTSTRAP_GENERATED,
)

DEPENDENCY_LOCK_CANDIDATES = (
    "Pipfile.lock",
    "poetry.lock",
    "pyproject.toml",
    "requirements.txt",
    "requirements-dev.txt",
    "uv.lock",
    "recomp/requirements.txt",
)
VITAGL_STOCK_REFERENCE_COMMIT = (
    "73dd57a8857f89f2353881c6de5891959c5c1983"
)
VITAGL_STOCK_REFERENCE_SOURCE_SHA256 = (
    "f484dd9d2aec707ac5f91352c6e0631239f2330fe6769e8476cfe425331c400a"
)
VITAGL_STOCK_REFERENCE_OOB_FIX_COMMIT = (
    "f24ad3e66f7f34bebe70598d1302c34f4eff4a54"
)
VITAGL_STOCK_REFERENCE_INPUTS = (
    "recomp/vita/vitagl-stock-reference/build.sh",
    "recomp/vita/vitagl-stock-reference/0001-deterministic-build-and-init-oob.patch",
    "recomp/vita/vitagl-stock-reference/0002-exact-gpu-draw-optimizations.patch",
    "recomp/vita/vitagl-stock-reference/isaac_gpu_draw_policy.h",
    "recomp/vita/vitagl-stock-reference/isaac_gxm_state_policy.h",
    "recomp/vita/vitagl-stock-reference/0003-exact-coloroffset-gpu-optimizations.patch",
    "recomp/vita/vitagl-stock-reference/isaac_coloroffset_gpu_policy.h",
    "recomp/vita/vitagl-stock-reference/0004-hardened-custom-shader-cache.patch",
    "recomp/vita/vitagl-stock-reference/isaac_shader_cache_policy.h",
    "recomp/vita/vitagl-stock-reference/isaac_shader_cache_vitagl.h",
    "recomp/vita/vitagl-stock-reference/isaac_shader_cache_block_list.h",
)

BINUTIL_CACHE_KEYS = {
    "ar": "CMAKE_AR",
    "linker": "CMAKE_LINKER",
    "nm": "CMAKE_NM",
    "objcopy": "CMAKE_OBJCOPY",
    "objdump": "CMAKE_OBJDUMP",
    "ranlib": "CMAKE_RANLIB",
    "readelf": "CMAKE_READELF",
    "strip": "CMAKE_STRIP",
}

linker_map_contract = None


class BuildError(RuntimeError):
    """A fail-closed build precondition or output check failed."""


def _load_linker_map_contract() -> None:
    """Load the local map module only after production path validation."""

    global linker_map_contract
    if linker_map_contract is not None:
        return
    recomp_text = str(RECOMP_DIR)
    if recomp_text not in sys.path:
        sys.path.insert(0, recomp_text)
    linker_map_contract = __import__("linker_map_contract")


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            block = handle.read(1024 * 1024)
            if not block:
                return digest.hexdigest()
            digest.update(block)


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
        allow_nan=False,
    ).encode("utf-8")


def _is_sha256(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(char in HEX_DIGITS for char in value)
    )


def _file_record(path: Path, logical_path: str) -> dict[str, object]:
    path = _regular_file(path, logical_path)
    return {
        "path": logical_path,
        "size": path.stat().st_size,
        "sha256": _sha256_file(path),
    }


def _load_lua53_pinned_records() -> tuple[tuple[str, str], ...]:
    """Load the exact 58-file Lua closure shared with the CMake target."""

    manifest = _regular_file(
        LUA53_CMAKE_MANIFEST, "pinned Lua 5.3.3 CMake manifest"
    )
    actual_manifest_sha256 = _sha256_file(manifest)
    if actual_manifest_sha256 != LUA53_CMAKE_MANIFEST_SHA256:
        raise BuildError(
            "pinned Lua 5.3.3 CMake manifest hash mismatch: "
            f"{actual_manifest_sha256} != {LUA53_CMAKE_MANIFEST_SHA256}"
        )
    try:
        text = manifest.read_bytes().decode("ascii")
    except (OSError, UnicodeError) as exc:
        raise BuildError(f"cannot read pinned Lua 5.3.3 manifest: {exc}") from exc
    start = "set(ISAAC_LUA53_SOURCE_RECORDS\n"
    end = "\n\nset(ISAAC_LUA53_C_SOURCES)"
    if text.count(start) != 1 or text.count(end) != 1:
        raise BuildError("pinned Lua 5.3.3 manifest has an unexpected envelope")
    body = text.split(start, 1)[1].split(end, 1)[0]
    if not body.endswith(")"):
        raise BuildError("pinned Lua 5.3.3 manifest has no closing list marker")
    body = body[:-1]
    records: list[tuple[str, str]] = []
    for line in body.splitlines():
        match = re.fullmatch(
            r'  "([A-Za-z0-9._-]+)\|([0-9a-f]{64})"', line
        )
        if match is None:
            raise BuildError(
                f"pinned Lua 5.3.3 manifest has a malformed record: {line!r}"
            )
        records.append((match.group(1), match.group(2)))
    names = [name for name, unused_sha256 in records]
    if (
        len(records) != LUA53_SOURCE_COUNT
        or len(set(names)) != LUA53_SOURCE_COUNT
        or sum(name.endswith(".c") for name in names) != LUA53_C_SOURCE_COUNT
        or any(not name.endswith((".c", ".h")) for name in names)
    ):
        raise BuildError(
            "pinned Lua 5.3.3 manifest is not the exact 58-file/33-C closure"
        )
    return tuple(records)


def _lua53_source_set_sha256(records: tuple[tuple[str, str], ...]) -> str:
    identity = [
        {"path": f"{LUA53_LOGICAL_SOURCE_PREFIX}/{name}", "sha256": sha256}
        for name, sha256 in records
    ]
    return hashlib.sha256(_canonical_bytes(identity)).hexdigest()


def _validate_lua53_source_records(
    source_root: Path,
    expected_records: tuple[tuple[str, str], ...],
) -> list[dict[str, object]]:
    """Hash one already-resolved Lua root against an explicit source closure."""

    source_root = _directory(source_root, "Lua 5.3.3 source root")
    source_dir_candidate = source_root / "src"
    if source_dir_candidate.is_symlink():
        raise BuildError(
            f"Lua 5.3.3 src directory is symlinked: {source_dir_candidate}"
        )
    source_dir = _directory(
        source_dir_candidate, "Lua 5.3.3 src directory"
    )
    verified: list[dict[str, object]] = []
    for name, expected_sha256 in expected_records:
        path = source_dir / name
        if path.is_symlink() or not path.is_file():
            raise BuildError(
                f"Lua 5.3.3 source is missing or symlinked: {name}"
            )
        if path.resolve().parent != source_dir:
            raise BuildError(f"Lua 5.3.3 source escaped its root: {name}")
        actual_sha256 = _sha256_file(path)
        if actual_sha256 != expected_sha256:
            raise BuildError(
                f"Lua 5.3.3 source hash mismatch for {name}: "
                f"{actual_sha256} != {expected_sha256}"
            )
        verified.append({
            "path": f"{LUA53_LOGICAL_SOURCE_PREFIX}/{name}",
            "size": path.stat().st_size,
            "sha256": actual_sha256,
        })
    return verified


def validate_lua53_source(source_root: Path) -> list[dict[str, object]]:
    """Validate the pristine upstream Lua 5.3.3 root used by CMake."""

    return _validate_lua53_source_records(
        source_root, _load_lua53_pinned_records()
    )


def _lua53_dependency_records(source_root: Path) -> list[dict[str, object]]:
    return [
        _file_record(
            LUA53_CMAKE_MANIFEST, "recomp/vita/lua53_vanilla.cmake"
        ),
        *validate_lua53_source(source_root),
    ]


def _run_captured(
    command: list[str],
    *,
    cwd: Path = REPO_ROOT,
    environment: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[bytes]:
    try:
        completed = subprocess.run(
            command,
            cwd=str(cwd),
            env=environment,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except FileNotFoundError as exc:
        raise BuildError(f"command was not found: {command[0]}") from exc
    if completed.returncode != 0:
        detail = (completed.stdout + completed.stderr).decode(
            "utf-8", errors="replace"
        ).strip()
        suffix = f": {detail}" if detail else ""
        raise BuildError(
            f"command failed with exit status {completed.returncode}: "
            f"{command[0]}{suffix}"
        )
    return completed


def _git_bytes(root: Path, *arguments: str) -> bytes:
    return _run_captured(["git", *arguments], cwd=root).stdout


def collect_git_state(root: Path = REPO_ROOT) -> dict[str, object]:
    """Return a deterministic identity for HEAD and the current dirty state."""

    root = root.resolve()
    commit = _git_bytes(root, "rev-parse", "--verify", "HEAD").decode(
        "ascii", errors="strict"
    ).strip()
    tree = _git_bytes(root, "rev-parse", "--verify", "HEAD^{tree}").decode(
        "ascii", errors="strict"
    ).strip()
    timestamp_text = _git_bytes(root, "show", "-s", "--format=%ct", "HEAD").decode(
        "ascii", errors="strict"
    ).strip()
    if (
        len(commit) not in (40, 64)
        or len(tree) != len(commit)
        or not all(character in HEX_DIGITS for character in commit)
        or not all(character in HEX_DIGITS for character in tree)
    ):
        raise BuildError("Git returned an invalid commit/tree object identity")
    try:
        commit_timestamp = int(timestamp_text, 10)
    except ValueError as exc:
        raise BuildError(f"Git returned an invalid commit timestamp: {timestamp_text!r}") from exc
    if commit_timestamp < 0:
        raise BuildError(f"Git returned a negative commit timestamp: {commit_timestamp}")

    status = _git_bytes(
        root,
        "status",
        "--porcelain=v1",
        "-z",
        "--untracked-files=all",
        "--ignore-submodules=none",
    )
    return {
        "commit": commit,
        "tree": tree,
        "commit_timestamp": commit_timestamp,
        "dirty": bool(status),
        "status_sha256": hashlib.sha256(status).hexdigest(),
    }


def run_release_audit(
    root: Path = REPO_ROOT, audit_path: Path = RELEASE_AUDIT_PATH
) -> None:
    """Run the public-source audit as the authoritative metadata/content gate."""

    audit_path = _regular_file(audit_path, "release audit")
    completed = _run_captured(
        [
            sys.executable,
            str(audit_path),
            "--strict",
            "--include-untracked",
        ],
        cwd=root.resolve(),
    )
    output = (completed.stdout + completed.stderr).decode(
        "utf-8", errors="replace"
    )
    if "release audit: GO" not in output:
        raise BuildError("release audit returned success without its GO verdict")


def validate_release_prerequisites(
    args: argparse.Namespace,
    git_state: dict[str, object],
    output_directories: tuple[Path, ...],
    environment: dict[str, str],
) -> None:
    """Reject release builds whose result would depend on ambient/stale state."""

    if git_state["dirty"]:
        raise BuildError("--release requires a clean Git worktree, including untracked files")
    if not args.reanalyse:
        raise BuildError("--release requires --reanalyse for a fresh analysis corpus")
    if args.io_profile:
        raise BuildError("--release rejects the diagnostic --io-profile feature")
    if args.png_decode_profile:
        raise BuildError(
            "--release rejects the diagnostic --png-decode-profile feature"
        )
    if args.png_native_unfilter:
        raise BuildError(
            "--release rejects the experimental --png-native-unfilter feature"
        )
    if getattr(args, "lua", False):
        raise BuildError("--release rejects the experimental --lua feature")
    if args.exit_menu_profile:
        raise BuildError(
            "--release rejects the diagnostic --exit-menu-profile feature"
        )
    if args.texture_churn_profile:
        raise BuildError(
            "--release rejects the diagnostic --texture-churn-profile feature"
        )
    if args.sim_cadence_receipt:
        raise BuildError(
            "--release rejects the diagnostic --sim-cadence-receipt feature"
        )
    if getattr(args, "audio_stream_receipt", False):
        raise BuildError(
            "--release rejects the diagnostic --audio-stream-receipt feature"
        )
    if args.world_seam_diag:
        raise BuildError(
            "--release rejects the diagnostic --world-seam-diag feature"
        )
    disabled_allocator_features = [
        option for attribute, option in RAW_ALLOCATOR_GATE_CANONICAL_ARG_FLAGS
        if getattr(args, attribute, None) is not True
    ]
    if disabled_allocator_features:
        raise BuildError(
            "--release requires the canonical-exact allocator features: "
            + ", ".join(disabled_allocator_features)
        )
    if environment.get("PYTHONHASHSEED") != "0":
        raise BuildError(
            "--release requires PYTHONHASHSEED=0 before Python starts"
        )
    expected_epoch = str(git_state["commit_timestamp"])
    if environment.get("SOURCE_DATE_EPOCH") != expected_epoch:
        raise BuildError(
            "--release requires SOURCE_DATE_EPOCH equal to the HEAD commit "
            f"timestamp ({expected_epoch})"
        )
    ninja = shutil.which("ninja", path=environment.get("PATH"))
    if not ninja:
        raise BuildError("--release requires Ninja on PATH")
    for directory in output_directories:
        if directory.exists():
            if not directory.is_dir():
                raise BuildError(f"release output path is not a directory: {directory}")
            if any(directory.iterdir()):
                raise BuildError(
                    "--release requires absent or empty generated/analysis/build "
                    f"directories: {directory}"
                )


def _regular_file(path: Path, label: str) -> Path:
    path = path.expanduser().resolve()
    if not path.is_file():
        raise BuildError(f"{label} is not a regular file: {path}")
    return path


def _directory(path: Path, label: str) -> Path:
    path = path.expanduser().resolve()
    if not path.is_dir():
        raise BuildError(f"{label} is not a directory: {path}")
    return path


def validate_supported_pe(path: Path) -> tuple[int, str]:
    """Reject a wrong Isaac build before the expensive discovery pass."""

    path = _regular_file(path, "unpacked PE")
    size = path.stat().st_size
    digest = _sha256_file(path)
    if size != EXPECTED_PE_SIZE or digest != EXPECTED_PE_SHA256:
        raise BuildError(
            "unsupported unpacked PE: "
            f"size={size}, sha256={digest}; expected size={EXPECTED_PE_SIZE}, "
            f"sha256={EXPECTED_PE_SHA256}"
        )

    try:
        import pefile
    except ImportError as exc:
        raise BuildError(
            "Python dependency 'pefile' is missing; install the recompiler "
            "requirements before building"
        ) from exc

    try:
        pe = pefile.PE(str(path), fast_load=True)
    except Exception as exc:  # pefile exposes several parse exception types.
        raise BuildError(f"cannot parse unpacked PE {path}: {exc}") from exc
    try:
        actual = (
            pe.FILE_HEADER.Machine,
            pe.OPTIONAL_HEADER.Magic,
            pe.OPTIONAL_HEADER.ImageBase,
            pe.OPTIONAL_HEADER.AddressOfEntryPoint,
            pe.OPTIONAL_HEADER.SizeOfImage,
        )
    finally:
        pe.close()
    expected = (
        EXPECTED_PE_MACHINE,
        EXPECTED_PE_MAGIC,
        EXPECTED_PE_IMAGE_BASE,
        EXPECTED_PE_ENTRY_RVA,
        EXPECTED_PE_IMAGE_SIZE,
    )
    if actual != expected:
        raise BuildError(
            "unpacked PE header contract changed: "
            f"machine=0x{actual[0]:x}, magic=0x{actual[1]:x}, "
            f"image_base=0x{actual[2]:x}, entry=0x{actual[3]:x}, "
            f"image_size=0x{actual[4]:x}"
        )
    return size, digest


def validate_vitasdk(path: Path) -> tuple[Path, Path]:
    sdk = _directory(path, "VITASDK")
    toolchain = _regular_file(
        sdk / "share" / "vita.toolchain.cmake", "VitaSDK CMake toolchain"
    )
    _regular_file(sdk / "share" / "vita.cmake", "VitaSDK CMake helpers")

    compiler_names = (
        "arm-vita-eabi-gcc.exe" if os.name == "nt" else "arm-vita-eabi-gcc",
        "arm-vita-eabi-gcc",
    )
    if not any((sdk / "bin" / name).is_file() for name in compiler_names):
        raise BuildError(f"Vita compiler is missing below {sdk / 'bin'}")
    return sdk, toolchain


def _configure_recompiler_paths(
    pe_path: Path, analysis_dir: Path, generated_dir: Path
):
    """Bind legacy stage globals to caller-owned portable paths.

    The recompiler predates this public wrapper and its modules still carry
    developer-machine defaults.  Keeping all rebinding here avoids copying the
    PE and also gives Windows multiprocessing spawn children the same paths.
    """

    recomp_text = str(RECOMP_DIR)
    if recomp_text not in sys.path:
        sys.path.insert(0, recomp_text)

    import funcs
    import bounds
    import build_one
    import gen_all

    pe_text = str(pe_path)
    analysis_text = str(analysis_dir)
    generated_text = str(generated_dir)
    functions_path = analysis_dir / "functions.json"
    functions_manifest = analysis_dir / "functions.manifest.json"
    bounds_path = analysis_dir / "bounds.json"
    bounds_manifest = analysis_dir / "bounds.manifest.json"

    funcs.EXE = pe_text
    funcs.OUTDIR = analysis_text
    funcs.OUT = str(functions_path)
    funcs.MANIFEST = str(functions_manifest)

    bounds.EXE = pe_text
    bounds.FUNCS = str(functions_path)
    bounds.OUT = str(bounds_path)
    bounds.MANIFEST = str(bounds_manifest)

    build_one.EXE = pe_text
    build_one.OUTDIR = analysis_text

    gen_all.EXE = pe_text
    gen_all.FUNCS = str(functions_path)
    gen_all.BOUNDS = str(bounds_path)
    # These aliases are the same imported modules today.  Set them explicitly
    # so a later import refactor cannot silently resurrect a local path.
    gen_all.Funcs.EXE = pe_text
    gen_all.Funcs.OUTDIR = analysis_text
    gen_all.Funcs.OUT = str(functions_path)
    gen_all.Funcs.MANIFEST = str(functions_manifest)
    gen_all.Bounds.EXE = pe_text
    gen_all.Bounds.FUNCS = str(functions_path)
    gen_all.Bounds.OUT = str(bounds_path)
    gen_all.Bounds.MANIFEST = str(bounds_manifest)
    gen_all.B.EXE = pe_text
    gen_all.B.OUTDIR = analysis_text

    # gen_all.main receives --outdir, but setting these values as well makes
    # direct contract calls deterministic before argument parsing.
    gen_all.OUTDIR = generated_text
    gen_all.MANIFEST = str(generated_dir / "manifest.json")
    return funcs, bounds, gen_all


def _bootstrap_spawn_child() -> None:
    """Apply path rebinding while a multiprocessing spawn child imports us."""

    if os.environ.get(BOOTSTRAP_FLAG) != "1":
        return
    missing = [key for key in BOOTSTRAP_KEYS[1:] if not os.environ.get(key)]
    if missing:
        raise RuntimeError(
            "incomplete Vita recompiler spawn bootstrap: " + ", ".join(missing)
        )
    _configure_recompiler_paths(
        Path(os.environ[BOOTSTRAP_PE]),
        Path(os.environ[BOOTSTRAP_ANALYSIS]),
        Path(os.environ[BOOTSTRAP_GENERATED]),
    )


@contextlib.contextmanager
def _spawn_environment(pe_path: Path, analysis_dir: Path, generated_dir: Path):
    previous = {key: os.environ.get(key) for key in BOOTSTRAP_KEYS}
    os.environ.update(
        {
            BOOTSTRAP_FLAG: "1",
            BOOTSTRAP_PE: str(pe_path),
            BOOTSTRAP_ANALYSIS: str(analysis_dir),
            BOOTSTRAP_GENERATED: str(generated_dir),
        }
    )
    try:
        yield
    finally:
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


def _pipeline_lock_paths(
    generated_dir: Path, analysis_dir: Path, build_dir: Path
) -> tuple[Path, ...]:
    """Return unique pipeline locks in one canonical deadlock-free order."""

    generated_dir = generated_dir.resolve()
    analysis_dir = analysis_dir.resolve()
    build_dir = build_dir.resolve()
    candidates = (
        generated_dir.parent / "pipeline.lock",
        analysis_dir / "pipeline.lock",
        build_dir.parent / f".{build_dir.name}.repentogxm-pipeline.lock",
    )
    unique: dict[str, Path] = {}
    for path in candidates:
        identity = os.path.normcase(os.path.realpath(path))
        unique.setdefault(identity, path)
    return tuple(unique[identity] for identity in sorted(unique))


@contextlib.contextmanager
def _pipeline_locks(
    generated_dir: Path, analysis_dir: Path, build_dir: Path
):
    """Exclude all wrappers which share any mutable pipeline directory."""

    recomp_text = str(RECOMP_DIR)
    if recomp_text not in sys.path:
        sys.path.insert(0, recomp_text)
    import build_contract

    lock_paths = _pipeline_lock_paths(generated_dir, analysis_dir, build_dir)
    with contextlib.ExitStack() as locks:
        for lock_path in lock_paths:
            locks.enter_context(build_contract.exclusive_lock(lock_path))
        yield lock_paths


def run_fresh_generation(
    pe_path: Path,
    analysis_dir: Path,
    generated_dir: Path,
    jobs: int,
    reanalyse: bool,
) -> None:
    analysis_dir.mkdir(parents=True, exist_ok=True)
    generated_dir.parent.mkdir(parents=True, exist_ok=True)

    with _spawn_environment(pe_path, analysis_dir, generated_dir):
        funcs, bounds, gen_all = _configure_recompiler_paths(
            pe_path, analysis_dir, generated_dir
        )
        print("\n== fresh function discovery ==", flush=True)
        result = funcs.main()
        if result not in (None, 0):
            raise BuildError(f"function discovery returned {result}")

        print("\n== fresh boundary analysis ==", flush=True)
        result = bounds.main()
        if result not in (None, 0):
            raise BuildError(f"boundary analysis returned {result}")

        print("\n== fresh Vita code generation ==", flush=True)
        arguments = [
            "--force",
            "--jobs",
            str(jobs),
            "--image-base",
            f"0x{GUEST_BASE:08x}",
            "--outdir",
            str(generated_dir),
        ]
        if reanalyse:
            arguments.insert(1, "--reanalyse")
        result = gen_all.main(arguments)
        if result not in (None, 0):
            raise BuildError(f"code generation returned {result}")


def verify_generation_manifest(
    pe_path: Path, generated_dir: Path, guest_base: int = GUEST_BASE
) -> dict:
    """Verify the exact generated set independently of CMake."""

    manifest_path = generated_dir / "manifest.json"
    if not manifest_path.is_file():
        raise BuildError(f"generated manifest is missing: {manifest_path}")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BuildError(f"cannot read generated manifest: {exc}") from exc

    required_keys = {
        "schema",
        "stage",
        "recipe_id",
        "recipe",
        "outputs",
        "output_set_id",
    }
    if not isinstance(manifest, dict) or set(manifest) != required_keys:
        raise BuildError("generated manifest has an unexpected schema")
    if manifest["schema"] != 1 or manifest["stage"] != "generate":
        raise BuildError("generated manifest is not a schema-1 generate stage")
    if not _is_sha256(manifest["recipe_id"]):
        raise BuildError("generated manifest recipe_id is not lowercase SHA-256")
    if not _is_sha256(manifest["output_set_id"]):
        raise BuildError("generated manifest output_set_id is not lowercase SHA-256")

    recipe = manifest["recipe"]
    if not isinstance(recipe, dict) or recipe.get("stage") != "generate":
        raise BuildError("generated manifest embeds the wrong recipe")
    calculated_recipe = hashlib.sha256(_canonical_bytes(recipe)).hexdigest()
    if calculated_recipe != manifest["recipe_id"]:
        raise BuildError(
            "generated recipe_id does not match the embedded recipe: "
            f"{manifest['recipe_id']} != {calculated_recipe}"
        )
    try:
        recorded_pe = recipe["inputs"]["input/isaac-ng.exe.unpacked.exe"]
        recorded_base = recipe["params"]["image_base"]
    except (KeyError, TypeError) as exc:
        raise BuildError("generated recipe has no PE/base contract") from exc
    actual_pe = _sha256_file(pe_path)
    if recorded_pe != actual_pe:
        raise BuildError(
            "generated corpus belongs to a different PE: "
            f"{recorded_pe} != {actual_pe}"
        )
    if isinstance(recorded_base, bool) or recorded_base != guest_base:
        raise BuildError(
            "generated corpus has the wrong guest base: "
            f"{recorded_base!r} != 0x{guest_base:08x}"
        )

    outputs = manifest["outputs"]
    if not isinstance(outputs, list) or not outputs:
        raise BuildError("generated manifest has no outputs")
    verified = []
    names = set()
    for index, entry in enumerate(outputs):
        if not isinstance(entry, dict) or set(entry) != {"path", "size", "sha256"}:
            raise BuildError(f"generated output {index} has an invalid record")
        name = entry["path"]
        pure = PurePosixPath(name) if isinstance(name, str) else None
        if (
            pure is None
            or pure.is_absolute()
            or not pure.parts
            or any(part in ("", ".", "..") for part in pure.parts)
            or "\\" in name
            or pure.as_posix() != name
            or name in names
        ):
            raise BuildError(f"generated output {index} has an unsafe path: {name!r}")
        size = entry["size"]
        digest = entry["sha256"]
        if isinstance(size, bool) or not isinstance(size, int) or size < 0:
            raise BuildError(f"generated output {name} has an invalid size")
        if not _is_sha256(digest):
            raise BuildError(f"generated output {name} has an invalid SHA-256")
        output_path = generated_dir.joinpath(*pure.parts)
        if not output_path.is_file():
            raise BuildError(f"generated output is missing: {name}")
        actual_size = output_path.stat().st_size
        actual_digest = _sha256_file(output_path)
        if actual_size != size or actual_digest != digest:
            raise BuildError(
                f"generated output changed: {name}: "
                f"size={actual_size}/{size}, sha256={actual_digest}/{digest}"
            )
        names.add(name)
        verified.append({"path": name, "size": size, "sha256": digest})

    if verified != sorted(verified, key=lambda item: item["path"]):
        raise BuildError("generated output inventory is not canonically sorted")
    calculated_set = hashlib.sha256(_canonical_bytes(verified)).hexdigest()
    if calculated_set != manifest["output_set_id"]:
        raise BuildError(
            "generated output_set_id does not match the output inventory: "
            f"{manifest['output_set_id']} != {calculated_set}"
        )

    actual_names = {
        item.name
        for item in generated_dir.iterdir()
        if item.is_file() and item.name != "manifest.json"
    }
    if actual_names != names:
        raise BuildError(
            "generated directory differs from its manifest: "
            f"extra={sorted(actual_names - names)}, "
            f"missing={sorted(names - actual_names)}"
        )
    guest_units = [name for name in names if name.startswith("guest_") and name.endswith(".c")]
    if len(guest_units) < 3:
        raise BuildError("generated corpus contains fewer than three guest C units")
    return manifest


def cmake_configure_command(
    cmake: str,
    sdk: Path,
    toolchain: Path,
    pe_path: Path,
    generated_dir: Path,
    build_dir: Path,
    heap_mb: int,
    kage: bool,
    loading_specialist: bool,
    audio: bool,
    openal_pool: bool,
    io_profile: bool,
    archive_file_cache: bool,
    archive_validation_skip: bool,
    archive_validation_receipt: bool,
    fios_cache: bool,
    crt_seek_shadow: bool,
    continue_profile: bool,
    png_decode_profile: bool,
    png_native_unfilter: bool,
    texture_churn_profile: bool,
    game_log_batch: bool,
    continue_overlay: bool,
    texel_oom_diagnostic: bool,
    texel_scratch: bool,
    texture_align8_policy: bool,
    anm2_scratch: bool,
    fxlayers_null_rollback: bool,
    fxray_alpha_mask: bool,
    heap_ledger_memblock: bool,
    heap_ledger_backshift: bool,
    heap_overflow_mspace: bool,
    room_entry_slab: bool,
    room_entry_hybrid: bool,
    source_date_epoch: int,
    release: bool = False,
    exit_menu_profile: bool = False,
    lua: bool = False,
    lua_source: Path | None = None,
    sim_cadence_receipt: bool = False,
    audio_stream_receipt: bool = False,
    heap_terminal_fastpath: bool = False,
    world_seam_diag: bool = False,
    stable_30_presentation: bool = False,
    laser_ring_shadow_skip: bool = False,
    ogg_queue_emergency: bool = False,
) -> list[str]:
    on_off = lambda value: "ON" if value else "OFF"
    lua_source_value = _lua_source_option_value(lua, lua_source)
    command = [cmake]
    if release:
        command.extend(("-G", "Ninja"))
    command.extend([
        "-S",
        VITA_SOURCE_DIR.as_posix(),
        "-B",
        build_dir.as_posix(),
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain.as_posix()}",
        f"-DVITASDK={sdk.as_posix()}",
        f"-DISAAC_PE_PATH={pe_path.as_posix()}",
        f"-DISAAC_GENERATED_DIR={generated_dir.as_posix()}",
        f"-DSOURCE_DATE_EPOCH={source_date_epoch}",
        f"-DISAAC_GUEST_BASE=0x{GUEST_BASE:08x}",
        f"-DISAAC_VITA_HEAP_MB={heap_mb}",
        "-DISAAC_VITA_SCAFFOLD_ONLY=OFF",
        # The direct CMake project keeps an explicit OFF diagnostic A/B path,
        # but this canonical wrapper always builds the guarded corpus.  Pass
        # the value even for a reused build directory so an ambient cache
        # cannot silently select that A/B variant.
        "-DISAAC_VITA_GUEST_STACK_GUARD=ON",
        # Direct CMake keeps the narrow game-compatibility edge opt-in.  The
        # physical-hardware profile always selects it and pins reused caches.
        "-DISAAC_VITA_ANM2_MISSING_LAYER_GUARD=ON",
        # The frozen checksum seam is PE/body/caller authenticated and falls
        # back before mutation on every unsupported ABI/range/alias case.
        # Canonical physical builds keep it enabled and clear stale caches.
        "-DISAAC_VITA_SAVE_CHECKSUM_FASTPATH=ON",
        # The frozen memory Save Reader leaf has exact ABI/range/fault guards;
        # canonical builds select it while retaining translated fallback.
        "-DISAAC_VITA_SAVE_READER_FASTPATH=ON",
        # The exact read32 wrapper keeps both generated callees/fallbacks and
        # removes only its repeated indirect dispatch glue.
        "-DISAAC_VITA_SAVE_READ32_FUSED=ON",
        # Three frozen parser roots retain all parser logic while 326 exact
        # reader targets bypass only dynamic guest dispatch.
        "-DISAAC_VITA_SAVE_READER_DIRECT_EDGES=ON",
        # The 350-caller memset import thunk retains its original guest_call
        # fallback and bypasses only authenticated generic IAT classification.
        "-DISAAC_VITA_MEMSET_THUNK_FASTPATH=ON",
        # All libpng output crosses the frozen zlib 1.1.4 flush leaf.  Its
        # exact allocation/ABI gates retain the translated fallback.
        f"-DISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH="
        f"{on_off(kage and heap_overflow_mspace)}",
        # Every compressed PNG byte crosses the authenticated frozen crc32
        # leaf; canonical builds replace only that standard byte loop.
        "-DISAAC_VITA_PNG_CRC32_FASTPATH=ON",
        # ArchivedFile feeds every compressed archive frame through this
        # authenticated miniz v1.15 coroutine and retains translated fallback.
        f"-DISAAC_VITA_ARCHIVE_MINIZ_FASTPATH="
        f"{on_off(kage and heap_overflow_mspace)}",
        f"-DISAAC_VITA_KAGE={on_off(kage)}",
        # This experiment is opt-in.  Always pass both cache entries so an ON
        # build directory reused for an ordinary build is forced back to OFF
        # and cannot retain its machine-local source path.
        f"-DISAAC_VITA_LUA={on_off(lua)}",
        f"-DISAAC_LUA53_SOURCE_DIR={lua_source_value}",
        f"-DISAAC_VITA_LOADING_SPECIALIST={on_off(loading_specialist)}",
        f"-DISAAC_VITA_AUDIO={on_off(audio)}",
        f"-DISAAC_VITA_AUDIO_STREAM_RECEIPT={on_off(audio_stream_receipt)}",
        f"-DISAAC_VITA_OPENAL_POOL={on_off(openal_pool)}",
        f"-DISAAC_VITA_IO_PROFILE={on_off(io_profile)}",
        f"-DISAAC_VITA_ARCHIVE_FILE_CACHE={on_off(archive_file_cache)}",
        f"-DISAAC_VITA_ARCHIVE_VALIDATION_SKIP={on_off(archive_validation_skip)}",
        f"-DISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT={on_off(archive_validation_receipt)}",
        f"-DISAAC_VITA_FIOS_CACHE={on_off(fios_cache)}",
        f"-DISAAC_VITA_OGG_QUEUE_EMERGENCY={on_off(ogg_queue_emergency)}",
        f"-DISAAC_VITA_CRT_SEEK_SHADOW={on_off(crt_seek_shadow)}",
        f"-DISAAC_VITA_CONTINUE_PROFILE={on_off(continue_profile)}",
        f"-DISAAC_VITA_EXIT_MENU_PROFILE={on_off(exit_menu_profile)}",
        f"-DISAAC_VITA_PNG_DECODE_PROFILE={on_off(png_decode_profile)}",
        f"-DISAAC_VITA_PNG_NATIVE_UNFILTER={on_off(png_native_unfilter)}",
        f"-DISAAC_VITA_TEXTURE_CHURN_PROFILE={on_off(texture_churn_profile)}",
        f"-DISAAC_VITA_GAME_LOG_BATCH={on_off(game_log_batch)}",
        f"-DISAAC_VITA_CONTINUE_OVERLAY={on_off(continue_overlay)}",
        # The original-Vita effect policy remains an explicit experimental
        # profile until its combined hardware A/B is accepted.  Pin OFF even
        # in a reused build directory so a prior diagnostic cache cannot leak.
        "-DISAAC_VITA_ORIGINAL_EFFECTS_PROFILE=OFF",
        # Keep the J835 RenderFrame borrow experimental until a hardware A/B
        # closes.  An explicit OFF also clears any reused diagnostic cache.
        "-DISAAC_VITA_RENDERFRAME_FASTPATH=OFF",
        "-DISAAC_VITA_PILL_BLOOM_BYPASS=OFF",
        "-DISAAC_VITA_TRANSIENT_BLOOM_HALF_RES=OFF",
        "-DISAAC_VITA_LASER_PROFILE=OFF",
        f"-DISAAC_VITA_LASER_RING_SHADOW_SKIP={on_off(laser_ring_shadow_skip)}",
        # Driver prerequisites only; ordinary builds never inherit an
        # experimental P8 cache entry or opt game textures into P8 storage.
        "-DISAAC_VITA_VITAGL_P8_SAFE_UPLOAD=OFF",
        "-DISAAC_VITA_LASER_ATLAS_P8=OFF",
        "-DISAAC_VITA_LASER_LIGHT_HALO_CLIP=OFF",
        "-DISAAC_VITA_POOP_FX_PROFILE=OFF",
        "-DISAAC_VITA_POOP_FX_SINGLE_CLOUD=OFF",
        "-DISAAC_VITA_STALL_PROBE=OFF",
        "-DISAAC_VITA_STAGE_HEARTBEAT=OFF",
        f"-DISAAC_VITA_PHASE_PROFILE={on_off(texture_churn_profile)}",
        f"-DISAAC_VITA_SIM_CADENCE_RECEIPT={on_off(sim_cadence_receipt)}",
        f"-DISAAC_VITA_WORLD_SEAM_DIAG={on_off(world_seam_diag)}",
        "-DISAAC_VITA_FULLSPEED_SCHEDULER=OFF",
        f"-DISAAC_VITA_STABLE_30_PRESENTATION="
        f"{on_off(stable_30_presentation)}",
        "-DISAAC_VITA_GUEST_LOOKUP_CACHE=OFF",
        # Full IAT registration authenticates both hot sync IDs before use.
        # Canonical physical builds select this CPU-only dispatch shortcut.
        "-DISAAC_VITA_SYNC_IMPORT_FASTPATH=ON",
        f"-DISAAC_VITA_TEXEL_OOM_DIAGNOSTIC={on_off(texel_oom_diagnostic)}",
        # Stage-boundary lifetime transitions are required production state; walking
        # mallinfo and synchronously writing two records is not.  Pin the
        # diagnostic half OFF so a reused CMake cache cannot re-enable it.
        "-DISAAC_VITA_STAGE_MEMORY_DIAGNOSTICS=OFF",
        f"-DISAAC_VITA_TEXEL_SCRATCH={on_off(texel_scratch)}",
        f"-DISAAC_VITA_TEXTURE_ALIGN8_POLICY={on_off(texture_align8_policy)}",
        f"-DISAAC_VITA_ANM2_SCRATCH={on_off(anm2_scratch)}",
        f"-DISAAC_VITA_FXLAYERS_NULL_ROLLBACK={on_off(fxlayers_null_rollback)}",
        f"-DISAAC_VITA_FXRAY_ALPHA_MASK={on_off(fxray_alpha_mask)}",
        f"-DISAAC_VITA_HEAP_LEDGER_MEMBLOCK={on_off(heap_ledger_memblock)}",
        f"-DISAAC_VITA_HEAP_LEDGER_BACKSHIFT={on_off(heap_ledger_backshift)}",
        f"-DISAAC_VITA_HEAP_OVERFLOW_MSPACE={on_off(heap_overflow_mspace)}",
        f"-DISAAC_VITA_HEAP_TERMINAL_FASTPATH={on_off(heap_terminal_fastpath)}",
        f"-DISAAC_VITA_ROOM_ENTRY_SLAB={on_off(room_entry_slab)}",
        f"-DISAAC_VITA_ROOM_ENTRY_HYBRID={on_off(room_entry_hybrid)}",
    ])
    if texture_churn_profile:
        # The texture census pins the exact stock name allocator.  Its control
        # profile requires direct-default and rejects these legacy probes.
        # Pass them only for the opt-in diagnostic, preserving the wrapper's
        # previous feature-OFF cache behavior.
        command.extend((
            "-DISAAC_VITA_DIRECT_DEFAULT=ON",
            "-DISAAC_VITA_FIRST_FRAME_PROBE=OFF",
            "-DISAAC_VITA_RAW_GXM_PROBE=OFF",
            "-DISAAC_VITA_KNOWN_COLOR_PROBE=OFF",
            "-DISAAC_VITA_RASTER_PROBE=OFF",
            "-DISAAC_VITA_SCREENSHOT_PROBE=OFF",
            "-DISAAC_VITA_VITAGL_STOCK_REFERENCE=ON",
        ))
    return command


def _print_command(command: list[str]) -> None:
    print("+ " + shlex.join(command), flush=True)


def run_command(command: list[str], environment: dict[str, str]) -> None:
    _print_command(command)
    try:
        subprocess.run(
            command,
            cwd=str(REPO_ROOT),
            env=environment,
            check=True,
        )
    except FileNotFoundError as exc:
        raise BuildError(f"command was not found: {command[0]}") from exc
    except subprocess.CalledProcessError as exc:
        raise BuildError(
            f"command failed with exit status {exc.returncode}: {command[0]}"
        ) from exc


def run_softfp_link_probe(
    sdk: Path, build_dir: Path, environment: dict[str, str]
) -> None:
    """Fail before generation if this is the incompatible stock hardfp SDK."""

    compiler = sdk / "bin" / (
        "arm-vita-eabi-gcc.exe"
        if (sdk / "bin" / "arm-vita-eabi-gcc.exe").is_file()
        else "arm-vita-eabi-gcc"
    )
    with tempfile.TemporaryDirectory(
        prefix="softfp-probe-", dir=str(build_dir)
    ) as temporary:
        probe_dir = Path(temporary)
        source = probe_dir / "softfp_probe.c"
        output = probe_dir / "softfp_probe.elf"
        source.write_text(
            "#include <math.h>\n"
            "float probe(float x) { return sinf(x) * 2.0f; }\n"
            "int main(void) { return (int)probe(1.0f); }\n",
            encoding="ascii",
        )
        command = [
            str(compiler),
            "-mcpu=cortex-a9",
            "-mfpu=neon",
            "-mfloat-abi=softfp",
            "-mthumb",
            str(source),
            "-o",
            str(output),
            "-lm",
        ]
        print("\n== softfp VitaSDK link probe ==", flush=True)
        try:
            run_command(command, environment)
        except BuildError as exc:
            raise BuildError(
                "softfp VitaSDK link probe failed; a stock hardfp VitaSDK is "
                "not compatible with this target"
            ) from exc
        if not output.is_file() or output.read_bytes()[:4] != b"\x7fELF":
            raise BuildError("softfp VitaSDK link probe produced no valid ELF")


def _find_artifact(build_dir: Path, names: tuple[str, ...], label: str) -> Path:
    direct = [build_dir / name for name in names if (build_dir / name).is_file()]
    if len(direct) == 1:
        return direct[0]
    candidates = sorted(
        {
            item.resolve()
            for name in names
            for item in build_dir.rglob(name)
            if item.is_file()
        }
    )
    if len(candidates) != 1:
        raise BuildError(
            f"expected exactly one {label} artefact, found {len(candidates)}: "
            + ", ".join(str(path) for path in candidates)
        )
    return candidates[0]


def locate_artifacts(build_dir: Path) -> dict[str, Path]:
    return {
        "ELF": _find_artifact(
            build_dir,
            ("isaac_first_arm_fault", "isaac_first_arm_fault.elf"),
            "ELF",
        ),
        "VELF": _find_artifact(
            build_dir,
            ("isaac_first_arm_fault.velf", "isaac-first-arm-fault.velf"),
            "VELF",
        ),
        "eboot": _find_artifact(build_dir, ("eboot.bin",), "eboot"),
        "VPK": _find_artifact(
            build_dir, ("the-binding-of-isaac-repentance.vpk",), "VPK"
        ),
    }


def locate_linker_map(build_dir: Path) -> Path:
    """Return the one exact linker-map output named by the CMake link flags."""

    return _regular_file(build_dir / LINKER_MAP_NAME, "linker map")


def _raw_allocator_expected_profile(cache: dict[str, str]) -> str:
    return (
        RAW_ALLOCATOR_GATE_PROFILE_CANONICAL
        if all(
            _cache_bool(cache, key)
            for key in RAW_ALLOCATOR_GATE_CANONICAL_CACHE_FLAGS
        )
        else RAW_ALLOCATOR_GATE_PROFILE_DIAGNOSTIC
    )


def _normalise_raw_allocator_heap_function(function: str) -> str:
    match = re.fullmatch(r"(.+)\.constprop\.([0-9]+)", function)
    if match and match.group(1) in RAW_ALLOCATOR_GATE_HEAP_CONSTPROP_BASES:
        return match.group(1) + ".constprop.N"
    return function


def _verify_raw_allocator_heap_receipt(
    profile: str,
    relocations: dict[tuple[str, str, str], int],
) -> None:
    if profile == RAW_ALLOCATOR_GATE_PROFILE_CANONICAL:
        if relocations != RAW_ALLOCATOR_GATE_HEAP_CANONICAL:
            raise BuildError(
                "canonical raw allocator heap relocation contract changed"
            )
        return
    normalised: dict[tuple[str, str, str], int] = {}
    for (function, symbol, kind), count in relocations.items():
        key = (
            _normalise_raw_allocator_heap_function(function), symbol, kind
        )
        normalised[key] = normalised.get(key, 0) + count
    for key, count in normalised.items():
        maximum = RAW_ALLOCATOR_GATE_HEAP_DIAGNOSTIC_MAX.get(key)
        if maximum is None or count > maximum:
            raise BuildError(
                "diagnostic raw allocator heap allowlist changed: "
                f"{key} count={count} maximum={maximum}"
            )
    if sum(normalised.values()) > sum(
        RAW_ALLOCATOR_GATE_HEAP_CANONICAL.values()
    ):
        raise BuildError(
            "diagnostic raw allocator heap total exceeds measured maximum"
        )
    functions = {function for function, unused_symbol, unused_kind in normalised}
    for alternatives in RAW_ALLOCATOR_GATE_HEAP_EXCLUSIVE_FUNCTIONS:
        if len(alternatives & functions) > 1:
            raise BuildError(
                "diagnostic raw allocator heap has mutually-exclusive owners"
            )


def _verify_raw_allocator_lua_archive(
    record: object,
    *,
    archive_input: object,
) -> int:
    if not isinstance(record, dict) or set(record) != {
        "path", "size", "sha256", "manifest_sha256", "source_set_sha256",
        "source_count", "member_count", "members", "allocator_policy",
    }:
        raise BuildError("raw allocator gate Lua archive record is malformed")
    if (
        record.get("path") != LUA53_ARCHIVE_NAME
        or type(record.get("size")) is not int
        or record["size"] < 0
        or not _is_sha256(record.get("sha256"))
        or record.get("manifest_sha256") != LUA53_CMAKE_MANIFEST_SHA256
        or record.get("source_count") != LUA53_C_SOURCE_COUNT
        or record.get("member_count") != LUA53_C_SOURCE_COUNT
        or not isinstance(archive_input, dict)
        or set(archive_input) != {"size", "sha256"}
        or record["size"] != archive_input.get("size")
        or record["sha256"] != archive_input.get("sha256")
    ):
        raise BuildError("raw allocator gate Lua archive identity changed")
    pinned = _load_lua53_pinned_records()
    if record.get("source_set_sha256") != _lua53_source_set_sha256(pinned):
        raise BuildError("raw allocator gate Lua source-set identity changed")
    c_records = [item for item in pinned if item[0].endswith(".c")]
    members = record.get("members")
    if not isinstance(members, list) or len(members) != len(c_records):
        raise BuildError("raw allocator gate Lua member closure changed")
    object_paths: set[str] = set()
    member_names: set[str] = set()
    for row, (name, source_sha256) in zip(members, c_records):
        if not isinstance(row, dict) or set(row) != {
            "source", "source_sha256", "object", "member", "size",
            "sha256", "raw_relocations",
        }:
            raise BuildError("raw allocator gate Lua member row is malformed")
        logical_source = f"{LUA53_LOGICAL_SOURCE_PREFIX}/{name}"
        logical_object = (
            f"{RAW_ALLOCATOR_GATE_LOGICAL_OBJECT_PREFIX}"
            f"{logical_source}.obj"
        )
        member_name = name + ".obj"
        expected_relocations = (
            list(LUA53_DEAD_RAW_RELOCATIONS) if name == "lauxlib.c" else []
        )
        if (
            row.get("source") != logical_source
            or row.get("source_sha256") != source_sha256
            or row.get("object") != logical_object
            or row.get("member") != member_name
            or type(row.get("size")) is not int
            or row["size"] <= 0
            or not _is_sha256(row.get("sha256"))
            or row.get("raw_relocations") != expected_relocations
        ):
            raise BuildError(
                f"raw allocator gate Lua member identity changed: {name}"
            )
        if logical_object in object_paths or member_name in member_names:
            raise BuildError("raw allocator gate Lua member closure is duplicated")
        object_paths.add(logical_object)
        member_names.add(member_name)
    expected_policy = {
        "required_final_symbol": LUA53_REQUIRED_FINAL_SYMBOL,
        "discarded_final_symbols": list(LUA53_DISCARDED_ALLOCATOR_SYMBOLS),
        "dead_raw_relocations": list(LUA53_DEAD_RAW_RELOCATIONS),
    }
    if record.get("allocator_policy") != expected_policy:
        raise BuildError("raw allocator gate Lua allocator policy changed")
    return len(members)


def verify_raw_allocator_gate(
    build_dir: Path,
    elf: Path,
    *,
    require_canonical: bool = False,
) -> dict[str, object]:
    """Reject a missing/stale post-link allocator-gate receipt."""

    receipt_path = _regular_file(
        build_dir / RAW_ALLOCATOR_GATE_RECEIPT_NAME,
        "Vita raw allocator gate receipt",
    )
    try:
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as exc:
        raise BuildError(f"cannot read raw allocator gate receipt: {exc}") from exc
    if (
        not isinstance(receipt, dict)
        or set(receipt) != RAW_ALLOCATOR_GATE_RECEIPT_KEYS
    ):
        raise BuildError("raw allocator gate receipt has an unexpected schema")
    if (receipt.get("schema") != 1 or
            receipt.get("stage") != "vita-raw-allocator-gate" or
            receipt.get("result") != "PASS" or
            receipt.get("target_name") != "isaac_first_arm_fault"):
        raise BuildError("raw allocator gate receipt is not a PASS for this target")
    if receipt.get("raw_symbols") != list(RAW_ALLOCATOR_GATE_RAW_SYMBOLS):
        raise BuildError("raw allocator gate symbol contract changed")
    if receipt.get("forbidden_source_feature_view_macros") != list(
        RAW_ALLOCATOR_GATE_FORBIDDEN_SOURCE_FEATURE_VIEW_MACROS
    ):
        raise BuildError(
            "raw allocator gate feature-view macro contract changed"
        )
    profile = receipt.get("profile")
    if profile not in (
        RAW_ALLOCATOR_GATE_PROFILE_CANONICAL,
        RAW_ALLOCATOR_GATE_PROFILE_DIAGNOSTIC,
    ):
        raise BuildError("raw allocator gate profile is malformed")
    inputs = receipt.get("inputs")
    if not isinstance(inputs, dict):
        raise BuildError("raw allocator gate input closure changed")
    cache = _read_cmake_cache(build_dir)
    direct_default = _cache_bool(cache, "ISAAC_VITA_DIRECT_DEFAULT")
    lua_enabled = _cache_bool(cache, "ISAAC_VITA_LUA")
    expected_inputs = set(RAW_ALLOCATOR_GATE_BASE_INPUTS)
    if direct_default:
        expected_inputs.add(RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_INPUT)
    if lua_enabled:
        expected_inputs.update(RAW_ALLOCATOR_GATE_LUA_INPUTS)
    if set(inputs) != expected_inputs:
        raise BuildError("raw allocator gate input closure changed")
    for name, record in inputs.items():
        record_keys = {"size", "sha256"}
        if name == RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_INPUT:
            record_keys.add("logical_source")
        if (not isinstance(record, dict) or
                set(record) != record_keys or
                type(record["size"]) is not int or record["size"] < 0 or
                not _is_sha256(record["sha256"])):
            raise BuildError(f"raw allocator gate input record is malformed: {name}")
    if direct_default and inputs[
            RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_INPUT
    ]["logical_source"] != RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_LOGICAL_SOURCE:
        raise BuildError(
            "raw allocator gate generated-source override is malformed"
        )
    input_paths = {
        "build_ninja": build_dir / "build.ninja",
        "cmake_cache": build_dir / "CMakeCache.txt",
        "compile_commands": build_dir / "compile_commands.json",
        "gate": VITA_SOURCE_DIR / "vita_raw_allocator_gate.py",
        "poison_header": VITA_SOURCE_DIR / "isaac_vita_raw_allocator_poison.h",
    }
    for name, path in input_paths.items():
        path = _regular_file(path, f"raw allocator gate input {name}")
        current = {"size": path.stat().st_size, "sha256": _sha256_file(path)}
        if inputs[name] != current:
            raise BuildError(f"raw allocator gate input is stale: {name}")
    expected_profile = _raw_allocator_expected_profile(cache)
    if profile != expected_profile:
        raise BuildError(
            "raw allocator gate profile disagrees with CMake features: "
            f"{profile!r} != {expected_profile!r}"
        )
    if require_canonical and profile != RAW_ALLOCATOR_GATE_PROFILE_CANONICAL:
        raise BuildError(
            "--release requires the canonical-exact raw allocator gate profile"
        )
    input_paths["generated_manifest"] = (
        _cached_path(build_dir, cache, "ISAAC_GENERATED_DIR") / "manifest.json"
    )
    cmake_readelf = cache.get("CMAKE_READELF")
    readelf_path: Path | None = None
    if cmake_readelf and not cmake_readelf.endswith("-NOTFOUND"):
        candidate = Path(cmake_readelf).expanduser()
        if not candidate.is_absolute():
            candidate = build_dir / candidate
        candidate = candidate.resolve()
        if candidate.is_file():
            readelf_path = candidate
    if readelf_path is None:
        readelf_path = _cached_path(
            build_dir, cache, "ISAAC_VITA_READELF"
        )
    input_paths["readelf"] = readelf_path
    late_inputs = ["generated_manifest", "readelf"]
    if direct_default:
        input_paths[RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_INPUT] = (
            build_dir / RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_OUTPUT
        )
        late_inputs.append(RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_INPUT)
    if lua_enabled:
        lua_source = _cached_path(
            build_dir, cache, "ISAAC_LUA53_SOURCE_DIR"
        )
        validate_lua53_source(lua_source)
        input_paths.update({
            "lua_manifest": LUA53_CMAKE_MANIFEST,
            "lua_archive": build_dir / LUA53_ARCHIVE_NAME,
            "lua_ar": _cached_path(build_dir, cache, "CMAKE_AR"),
        })
        late_inputs.extend(sorted(RAW_ALLOCATOR_GATE_LUA_INPUTS))
    for name in late_inputs:
        path = _regular_file(input_paths[name], f"raw allocator gate input {name}")
        current = {"size": path.stat().st_size, "sha256": _sha256_file(path)}
        if name == RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_INPUT:
            current = {
                "logical_source": RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_LOGICAL_SOURCE,
                **current,
            }
        if inputs[name] != current:
            raise BuildError(f"raw allocator gate input is stale: {name}")
    target = receipt.get("target")
    if (not isinstance(target, dict) or
            set(target) != {"size", "sha256"} or
            target.get("size") != elf.stat().st_size or
            target.get("sha256") != _sha256_file(elf)):
        raise BuildError("raw allocator gate receipt belongs to another ELF")
    closure = receipt.get("closure")
    sources = closure.get("sources") if isinstance(closure, dict) else None
    linked_objects = (
        closure.get("linked_objects") if isinstance(closure, dict) else None
    )
    expected_closure_keys = {"sources", "linked_objects"}
    if lua_enabled:
        expected_closure_keys.add("lua_archive")
    if (not isinstance(closure, dict) or
            set(closure) != expected_closure_keys or
            not isinstance(sources, list) or
            not isinstance(linked_objects, list) or
            receipt.get("closure_sha256") != hashlib.sha256(
                _canonical_bytes(closure)).hexdigest()):
        raise BuildError("raw allocator gate closure identity is stale")
    lua_object_count = 0
    if lua_enabled:
        lua_object_count = _verify_raw_allocator_lua_archive(
            closure["lua_archive"], archive_input=inputs["lua_archive"]
        )
    if (
        receipt.get("source_count") != len(sources) + lua_object_count
        or receipt.get("object_count") != len(linked_objects) + lua_object_count
    ):
        raise BuildError("raw allocator gate closure counts are stale")

    def relative_path(value: object, label: str) -> str:
        if (not isinstance(value, str) or not value or "\\" in value or
                ":" in value or value.startswith("/") or
                any(part in ("", ".", "..") for part in value.split("/"))):
            raise BuildError(f"raw allocator gate {label} is not relative: {value!r}")
        return value

    source_paths: set[str] = set()
    source_order: list[str] = []
    object_paths: set[str] = set()
    actual_exempt: list[str] = []
    heap_relocations: dict[tuple[str, str, str], int] | None = None
    for row in sources:
        if (not isinstance(row, dict) or
                set(row) != {"source", "object", "exempt", "raw_relocations"}):
            raise BuildError("raw allocator gate source row is malformed")
        source = relative_path(row["source"], "source path")
        obj = relative_path(row["object"], "object path")
        expected_obj = (
            f"{RAW_ALLOCATOR_GATE_LOGICAL_OBJECT_PREFIX}{source}.obj"
        )
        if obj != expected_obj:
            raise BuildError(
                "raw allocator gate logical object identity changed: "
                f"{obj!r} != {expected_obj!r}"
            )
        if source in source_paths or obj in object_paths:
            raise BuildError("raw allocator gate source/object closure is duplicated")
        source_paths.add(source)
        source_order.append(source)
        object_paths.add(obj)
        if type(row["exempt"]) is not bool:
            raise BuildError("raw allocator gate exemption flag is malformed")
        relocations = row["raw_relocations"]
        if not isinstance(relocations, list):
            raise BuildError("raw allocator gate relocation list is malformed")
        relocation_keys: set[tuple[str, str, str]] = set()
        row_relocations: dict[tuple[str, str, str], int] = {}
        for relocation in relocations:
            if (not isinstance(relocation, dict) or
                    set(relocation) != {"function", "symbol", "type", "count"} or
                    not isinstance(relocation["function"], str) or
                    not relocation["function"] or
                    relocation["symbol"] not in RAW_ALLOCATOR_GATE_RAW_SYMBOLS or
                    not isinstance(relocation["type"], str) or
                    not relocation["type"].startswith("R_ARM_") or
                    type(relocation["count"]) is not int or
                    relocation["count"] <= 0):
                raise BuildError("raw allocator gate relocation row is malformed")
            key = (relocation["function"], relocation["symbol"], relocation["type"])
            if key in relocation_keys:
                raise BuildError("raw allocator gate relocation row is duplicated")
            relocation_keys.add(key)
            row_relocations[key] = relocation["count"]
        if source == "recomp/runtime/host_vita_heap.c":
            heap_relocations = row_relocations
        if row["exempt"]:
            actual_exempt.append(source)
        elif relocations:
            raise BuildError("raw allocator relocation leaked into an ordinary source")
    if source_order != sorted(source_order):
        raise BuildError("raw allocator gate source closure is not canonical")
    if (direct_default and
            RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_LOGICAL_SOURCE not in source_paths):
        raise BuildError(
            "raw allocator gate generated-source override is absent from closure"
        )
    linked_paths = [
        relative_path(value, "linked object path") for value in linked_objects
    ]
    if (len(linked_paths) != len(set(linked_paths)) or
            set(linked_paths) != object_paths):
        raise BuildError("raw allocator gate linked-object closure changed")
    expected_exempt = [
        source for source in RAW_ALLOCATOR_GATE_EXEMPT_SOURCES
        if source in source_paths
    ]
    if (receipt.get("exempt_sources") != expected_exempt or
            sorted(actual_exempt) != expected_exempt):
        raise BuildError("raw allocator gate exemption contract changed")
    if heap_relocations is None:
        raise BuildError("raw allocator gate omitted the heap owner")
    _verify_raw_allocator_heap_receipt(profile, heap_relocations)
    return receipt


def _linker_map_record(
    path: Path,
    *,
    source_root: Path,
    generated_root: Path,
    vitasdk_root: Path,
    generation_recipe_id: str,
) -> dict[str, object]:
    """Return exact evidence plus the path-independent linker-map identity."""

    path = _regular_file(path, "linker map")
    if path.name != LINKER_MAP_NAME:
        raise BuildError(
            f"linker map has the wrong name: {path.name!r} != {LINKER_MAP_NAME!r}"
        )
    try:
        payload = path.read_bytes()
    except OSError as exc:
        raise BuildError(f"cannot read linker map {path}: {exc}") from exc

    try:
        canonical = linker_map_contract.canonicalize(
            payload,
            source_root=source_root,
            generated_root=generated_root,
            vitasdk_root=vitasdk_root,
            generation_recipe_id=generation_recipe_id,
        )
    except linker_map_contract.LinkerMapContractError as exc:
        raise BuildError(f"invalid linker map {path}: {exc}") from exc

    return {
        "path": LINKER_MAP_NAME,
        "raw": {
            "size": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
        },
        "canonical": canonical.record,
    }


def verify_package(
    pe_path: Path,
    artifacts: dict[str, Path],
    *,
    audio: bool = False,
) -> None:
    if artifacts["ELF"].read_bytes()[:4] != b"\x7fELF":
        raise BuildError(f"built executable is not ELF: {artifacts['ELF']}")
    if artifacts["VELF"].read_bytes()[:4] != b"\x7fELF":
        raise BuildError(f"built VELF is not ELF: {artifacts['VELF']}")
    if artifacts["eboot"].read_bytes()[:4] != b"SCE\x00":
        raise BuildError(f"built eboot is not a Vita SELF: {artifacts['eboot']}")

    try:
        with zipfile.ZipFile(artifacts["VPK"], "r") as package:
            bad_member = package.testzip()
            if bad_member is not None:
                raise BuildError(f"VPK CRC failed for member {bad_member}")
            member_names = package.namelist()
            names = set(member_names)
            if len(names) != len(member_names):
                raise BuildError("VPK contains duplicate member names")
            if "alsof.conf" in names:
                raise BuildError(
                    "VPK contains misspelled alsof.conf; OpenAL requires "
                    "app0:/alsoft.conf"
                )
            expected = {
                "sce_sys/param.sfo",
                "eboot.bin",
                VPK_MANAGER_MEMBER,
                VPK_MANAGER_LICENSE_MEMBER,
                "isaac-ng.exe.unpacked.exe",
                *VPK_LIVEAREA_MEMBERS,
            }
            if audio:
                expected.add("alsoft.conf")
            if names != expected:
                missing = sorted(expected - names)
                unexpected = sorted(names - expected)
                raise BuildError(
                    "VPK member set differs from the release contract: "
                    f"missing={missing}, unexpected={unexpected}"
                )
            packaged_eboot = package.read("eboot.bin")
            packaged_manager = package.read(VPK_MANAGER_MEMBER)
            packaged_manager_license = package.read(VPK_MANAGER_LICENSE_MEMBER)
            packaged_pe = package.read("isaac-ng.exe.unpacked.exe")
            packaged_audio_config = package.read("alsoft.conf") if audio else None
            livearea_members = tuple(
                name
                for name in member_names
                if name in VPK_LIVEAREA_MEMBER_SET
            )
            if livearea_members != VPK_LIVEAREA_MEMBERS:
                raise BuildError(
                    "VPK LiveArea member order differs from the release contract: "
                    f"{livearea_members!r}"
                )
            packaged_livearea = {
                member: package.read(member)
                for member, unused_size, unused_sha256 in VPK_LIVEAREA_ASSETS
            }
    except (OSError, zipfile.BadZipFile) as exc:
        raise BuildError(f"cannot verify VPK {artifacts['VPK']}: {exc}") from exc

    if hashlib.sha256(packaged_eboot).hexdigest() != _sha256_file(artifacts["eboot"]):
        raise BuildError("VPK eboot.bin differs from the built sidecar")
    manager_sidecar = _regular_file(
        artifacts["VPK"].parent / VPK_MANAGER_MEMBER, "manager SELF"
    )
    if manager_sidecar.read_bytes()[:4] != b"SCE\x00":
        raise BuildError(f"built manager is not a Vita SELF: {manager_sidecar}")
    if packaged_manager != manager_sidecar.read_bytes():
        raise BuildError("VPK isaac-manager.bin differs from the built sidecar")
    manager_license = _regular_file(
        VPK_MANAGER_LICENSE_SOURCE, "libftpvita MIT licence"
    )
    if packaged_manager_license != manager_license.read_bytes():
        raise BuildError(
            "VPK libftpvita notice differs from the pinned MIT licence"
        )
    if (
        len(packaged_pe) != pe_path.stat().st_size
        or hashlib.sha256(packaged_pe).hexdigest() != _sha256_file(pe_path)
    ):
        raise BuildError("VPK packaged PE differs from the supplied generation input")
    if audio:
        audio_config = REPO_ROOT / "recomp" / "vita" / "alsoft.conf"
        if not audio_config.is_file():
            raise BuildError(f"OpenAL config source is missing: {audio_config}")
        if packaged_audio_config != audio_config.read_bytes():
            raise BuildError(
                "VPK alsoft.conf differs from recomp/vita/alsoft.conf"
            )
    for member, expected_size, expected_sha256 in VPK_LIVEAREA_ASSETS:
        payload = packaged_livearea[member]
        actual_sha256 = hashlib.sha256(payload).hexdigest()
        if len(payload) != expected_size or actual_sha256 != expected_sha256:
            raise BuildError(
                f"VPK LiveArea payload differs from the release contract: {member}; "
                f"size={len(payload)} sha256={actual_sha256}"
            )


def print_artifacts(artifacts: dict[str, Path]) -> None:
    print("\n== verified build artefacts ==")
    for label in ("ELF", "VELF", "eboot", "VPK"):
        path = artifacts[label]
        print(
            f"{label:5s} size={path.stat().st_size} "
            f"sha256={_sha256_file(path)} path={path}"
        )


def _read_cmake_cache(build_dir: Path) -> dict[str, str]:
    cache_path = _regular_file(build_dir / "CMakeCache.txt", "CMake cache")
    result: dict[str, str] = {}
    try:
        lines = cache_path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        raise BuildError(f"cannot read CMake cache {cache_path}: {exc}") from exc
    for line in lines:
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key_and_type, value = line.split("=", 1)
        if ":" not in key_and_type:
            continue
        key, unused_type = key_and_type.rsplit(":", 1)
        result[key] = value
    return result


def _cache_bool(cache: dict[str, str], key: str) -> bool:
    value = cache.get(key)
    if value in ("1", "ON", "TRUE", "YES", "Y"):
        return True
    if value in ("0", "OFF", "FALSE", "NO", "N"):
        return False
    raise BuildError(f"CMake cache has no canonical boolean for {key}: {value!r}")


def _cached_path(build_dir: Path, cache: dict[str, str], key: str) -> Path:
    value = cache.get(key)
    if not value or value.endswith("-NOTFOUND"):
        raise BuildError(f"CMake cache is missing tool/path {key}")
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = build_dir / path
    return path.resolve()


def _tool_version(path: Path, label: str) -> dict[str, object]:
    path = _regular_file(path, label)
    completed = _run_captured([str(path), "--version"])
    combined = (completed.stdout + completed.stderr).decode(
        "utf-8", errors="replace"
    )
    lines = [line.strip() for line in combined.splitlines() if line.strip()]
    if not lines:
        raise BuildError(f"{label} returned no version text: {path}")
    return {
        "name": path.name,
        "version": lines[0],
        "size": path.stat().st_size,
        "sha256": _sha256_file(path),
    }


def _sdk_tool(sdk: Path, stem: str) -> Path:
    candidates = (sdk / "bin" / f"arm-vita-eabi-{stem}.exe", sdk / "bin" / f"arm-vita-eabi-{stem}")
    found = [candidate for candidate in candidates if candidate.is_file()]
    if not found:
        raise BuildError(f"VitaSDK binutils tool is missing: arm-vita-eabi-{stem}")
    return found[0].resolve()


def validate_cmake_contract(
    cache: dict[str, str],
    *,
    pe_path: Path,
    generated_dir: Path,
    heap_mb: int,
    kage: bool,
    loading_specialist: bool,
    audio: bool,
    openal_pool: bool,
    io_profile: bool,
    archive_file_cache: bool,
    archive_validation_skip: bool,
    archive_validation_receipt: bool,
    fios_cache: bool,
    crt_seek_shadow: bool,
    continue_profile: bool,
    png_decode_profile: bool,
    png_native_unfilter: bool,
    texture_churn_profile: bool,
    game_log_batch: bool,
    continue_overlay: bool,
    texel_oom_diagnostic: bool,
    texel_scratch: bool,
    texture_align8_policy: bool,
    anm2_scratch: bool,
    fxlayers_null_rollback: bool,
    fxray_alpha_mask: bool,
    heap_ledger_memblock: bool,
    heap_ledger_backshift: bool,
    heap_overflow_mspace: bool,
    room_entry_slab: bool,
    room_entry_hybrid: bool,
    source_date_epoch: int,
    release: bool,
    exit_menu_profile: bool = False,
    lua: bool = False,
    lua_source: Path | None = None,
    sim_cadence_receipt: bool = False,
    audio_stream_receipt: bool = False,
    heap_terminal_fastpath: bool = False,
    world_seam_diag: bool = False,
    stable_30_presentation: bool = False,
    laser_ring_shadow_skip: bool = False,
    ogg_queue_emergency: bool = False,
) -> dict[str, object]:
    """Prove that CMake used the requested feature/base/heap contract."""

    lua_source_value = _lua_source_option_value(lua, lua_source)
    expected_text = {
        "CMAKE_BUILD_TYPE": "Release",
        "ISAAC_GUEST_BASE": f"0x{GUEST_BASE:08x}",
        "ISAAC_VITA_HEAP_MB": str(heap_mb),
        "SOURCE_DATE_EPOCH": str(source_date_epoch),
    }
    for key, expected in expected_text.items():
        actual = cache.get(key)
        if actual != expected:
            raise BuildError(f"CMake cache changed {key}: {actual!r} != {expected!r}")
    expected_flags = {
        "ISAAC_VITA_SCAFFOLD_ONLY": False,
        "ISAAC_VITA_GUEST_STACK_GUARD": True,
        "ISAAC_VITA_ANM2_MISSING_LAYER_GUARD": True,
        "ISAAC_VITA_SAVE_CHECKSUM_FASTPATH": True,
        "ISAAC_VITA_SAVE_READER_FASTPATH": True,
        "ISAAC_VITA_SAVE_READ32_FUSED": True,
        "ISAAC_VITA_SAVE_READER_DIRECT_EDGES": True,
        "ISAAC_VITA_MEMSET_THUNK_FASTPATH": True,
        "ISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH": (
            kage and heap_overflow_mspace
        ),
        "ISAAC_VITA_PNG_CRC32_FASTPATH": True,
        "ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH": (
            kage and heap_overflow_mspace
        ),
        "ISAAC_VITA_KAGE": kage,
        "ISAAC_VITA_LUA": lua,
        "ISAAC_VITA_LOADING_SPECIALIST": loading_specialist,
        "ISAAC_VITA_AUDIO": audio,
        "ISAAC_VITA_AUDIO_STREAM_RECEIPT": audio_stream_receipt,
        "ISAAC_VITA_OPENAL_POOL": openal_pool,
        "ISAAC_VITA_IO_PROFILE": io_profile,
        "ISAAC_VITA_ARCHIVE_FILE_CACHE": archive_file_cache,
        "ISAAC_VITA_ARCHIVE_VALIDATION_SKIP": archive_validation_skip,
        "ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT": archive_validation_receipt,
        "ISAAC_VITA_FIOS_CACHE": fios_cache,
        "ISAAC_VITA_OGG_QUEUE_EMERGENCY": ogg_queue_emergency,
        "ISAAC_VITA_CRT_SEEK_SHADOW": crt_seek_shadow,
        "ISAAC_VITA_CONTINUE_PROFILE": continue_profile,
        "ISAAC_VITA_EXIT_MENU_PROFILE": exit_menu_profile,
        "ISAAC_VITA_PNG_DECODE_PROFILE": png_decode_profile,
        "ISAAC_VITA_PNG_NATIVE_UNFILTER": png_native_unfilter,
        "ISAAC_VITA_TEXTURE_CHURN_PROFILE": texture_churn_profile,
        "ISAAC_VITA_GAME_LOG_BATCH": game_log_batch,
        "ISAAC_VITA_CONTINUE_OVERLAY": continue_overlay,
        "ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE": False,
        "ISAAC_VITA_RENDERFRAME_FASTPATH": False,
        "ISAAC_VITA_PILL_BLOOM_BYPASS": False,
        "ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES": False,
        "ISAAC_VITA_LASER_PROFILE": False,
        "ISAAC_VITA_LASER_RING_SHADOW_SKIP": laser_ring_shadow_skip,
        "ISAAC_VITA_VITAGL_P8_SAFE_UPLOAD": False,
        "ISAAC_VITA_LASER_ATLAS_P8": False,
        "ISAAC_VITA_LASER_LIGHT_HALO_CLIP": False,
        "ISAAC_VITA_POOP_FX_PROFILE": False,
        "ISAAC_VITA_POOP_FX_SINGLE_CLOUD": False,
        "ISAAC_VITA_STALL_PROBE": False,
        "ISAAC_VITA_STAGE_HEARTBEAT": False,
        "ISAAC_VITA_PHASE_PROFILE": texture_churn_profile,
        "ISAAC_VITA_SIM_CADENCE_RECEIPT": sim_cadence_receipt,
        "ISAAC_VITA_WORLD_SEAM_DIAG": world_seam_diag,
        "ISAAC_VITA_FULLSPEED_SCHEDULER": False,
        "ISAAC_VITA_STABLE_30_PRESENTATION": stable_30_presentation,
        "ISAAC_VITA_GUEST_LOOKUP_CACHE": False,
        "ISAAC_VITA_SYNC_IMPORT_FASTPATH": True,
        "ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC": texel_oom_diagnostic,
        "ISAAC_VITA_STAGE_MEMORY_DIAGNOSTICS": False,
        "ISAAC_VITA_TEXEL_SCRATCH": texel_scratch,
        "ISAAC_VITA_TEXTURE_ALIGN8_POLICY": texture_align8_policy,
        "ISAAC_VITA_ANM2_SCRATCH": anm2_scratch,
        "ISAAC_VITA_FXLAYERS_NULL_ROLLBACK": fxlayers_null_rollback,
        "ISAAC_VITA_FXRAY_ALPHA_MASK": fxray_alpha_mask,
        "ISAAC_VITA_HEAP_LEDGER_MEMBLOCK": heap_ledger_memblock,
        "ISAAC_VITA_HEAP_LEDGER_BACKSHIFT": heap_ledger_backshift,
        "ISAAC_VITA_HEAP_OVERFLOW_MSPACE": heap_overflow_mspace,
        "ISAAC_VITA_HEAP_TERMINAL_FASTPATH": heap_terminal_fastpath,
        "ISAAC_VITA_ROOM_ENTRY_SLAB": room_entry_slab,
        "ISAAC_VITA_ROOM_ENTRY_HYBRID": room_entry_hybrid,
    }
    if texture_churn_profile:
        expected_flags.update({
            "ISAAC_VITA_DIRECT_DEFAULT": True,
            "ISAAC_VITA_FIRST_FRAME_PROBE": False,
            "ISAAC_VITA_RAW_GXM_PROBE": False,
            "ISAAC_VITA_KNOWN_COLOR_PROBE": False,
            "ISAAC_VITA_RASTER_PROBE": False,
            "ISAAC_VITA_SCREENSHOT_PROBE": False,
            "ISAAC_VITA_VITAGL_STOCK_REFERENCE": True,
        })
    actual_flags = {key: _cache_bool(cache, key) for key in expected_flags}
    if actual_flags != expected_flags:
        raise BuildError(
            f"CMake feature flags differ: {actual_flags!r} != {expected_flags!r}"
        )
    for key, expected_path in (
        ("ISAAC_PE_PATH", pe_path),
        ("ISAAC_GENERATED_DIR", generated_dir),
    ):
        actual_path = Path(cache.get(key, "")).expanduser()
        if not actual_path.is_absolute():
            raise BuildError(f"CMake cache path {key} is not absolute: {actual_path}")
        if actual_path.resolve() != expected_path.resolve():
            raise BuildError(
                f"CMake cache changed {key}: {actual_path.resolve()} != "
                f"{expected_path.resolve()}"
            )
    cached_lua_source = cache.get("ISAAC_LUA53_SOURCE_DIR")
    if not lua:
        if cached_lua_source != "":
            raise BuildError(
                "CMake cache retained ISAAC_LUA53_SOURCE_DIR while Lua is OFF: "
                f"{cached_lua_source!r}"
            )
    else:
        actual_lua_source = Path(cached_lua_source or "").expanduser()
        if not actual_lua_source.is_absolute():
            raise BuildError(
                "CMake cache path ISAAC_LUA53_SOURCE_DIR is not absolute: "
                f"{actual_lua_source}"
            )
        expected_lua_source = Path(lua_source_value)
        if actual_lua_source.resolve() != expected_lua_source:
            raise BuildError(
                "CMake cache changed ISAAC_LUA53_SOURCE_DIR: "
                f"{actual_lua_source.resolve()} != {expected_lua_source}"
            )
    generator = cache.get("CMAKE_GENERATOR")
    if not generator:
        raise BuildError("CMake cache has no CMAKE_GENERATOR")
    if release and generator != "Ninja":
        raise BuildError(f"--release configured a non-Ninja generator: {generator}")
    lua53_source: dict[str, object] | None = None
    if lua:
        pinned_records = _load_lua53_pinned_records()
        lua53_source = {
            "version": LUA53_VERSION,
            "archive_sha256": LUA53_ARCHIVE_SHA256,
            "manifest_sha256": LUA53_CMAKE_MANIFEST_SHA256,
            "source_count": len(pinned_records),
            "source_set_sha256": _lua53_source_set_sha256(pinned_records),
        }
    return {
        "build_type": "Release",
        "guest_base": GUEST_BASE,
        "heap_mb": heap_mb,
        "features": actual_flags,
        "audio_active": bool(kage and audio),
        "lua53_source": lua53_source,
        "cmake_generator": generator,
    }


def collect_toolchain_identity(
    *,
    cmake_executable: Path,
    sdk: Path,
    build_dir: Path,
    cache: dict[str, str],
    environment: dict[str, str],
) -> dict[str, object]:
    compiler = _cached_path(build_dir, cache, "CMAKE_C_COMPILER")
    generator = cache.get("CMAKE_GENERATOR", "")
    ninja_path: Path | None = None
    if generator == "Ninja":
        ninja_path = _cached_path(build_dir, cache, "CMAKE_MAKE_PROGRAM")
    else:
        candidate = shutil.which("ninja", path=environment.get("PATH"))
        if candidate:
            ninja_path = Path(candidate).resolve()

    binutils: dict[str, object] = {}
    for label, cache_key in sorted(BINUTIL_CACHE_KEYS.items()):
        value = cache.get(cache_key)
        if value and not value.endswith("-NOTFOUND"):
            path = Path(value).expanduser()
            if not path.is_absolute():
                path = build_dir / path
            path = path.resolve()
        else:
            path = _sdk_tool(sdk, "ld" if label == "linker" else label)
        binutils[label] = _tool_version(path, f"Vita binutils {label}")

    python_packages: dict[str, str] = {}
    for distribution in ("capstone", "pefile"):
        try:
            python_packages[distribution] = importlib.metadata.version(distribution)
        except importlib.metadata.PackageNotFoundError as exc:
            raise BuildError(
                f"installed Python dependency has no package metadata: {distribution}"
            ) from exc
    return {
        "python": {
            "implementation": platform.python_implementation(),
            "version": platform.python_version(),
            "packages": python_packages,
        },
        "cmake": _tool_version(cmake_executable, "CMake"),
        "ninja": {
            "used": generator == "Ninja",
            "identity": _tool_version(ninja_path, "Ninja") if ninja_path else None,
        },
        "compiler": _tool_version(compiler, "Vita C compiler"),
        "binutils": binutils,
    }


def _read_key_value_contract(path: Path, label: str) -> dict[str, str]:
    path = _regular_file(path, label)
    try:
        payload = path.read_bytes()
        text = payload.decode("ascii")
    except (OSError, UnicodeError) as exc:
        raise BuildError(f"cannot read {label}: {exc}") from exc
    if not text.endswith("\n") or "\r" in text:
        raise BuildError(f"{label} is not canonical LF-terminated text")
    result: dict[str, str] = {}
    for line in text.splitlines():
        match = re.fullmatch(r"([a-z][a-z0-9_]*)=(.+)", line)
        if match is None:
            raise BuildError(f"{label} has a malformed record: {line!r}")
        key, value = match.groups()
        if key in result:
            raise BuildError(f"{label} repeats key {key!r}")
        result[key] = value
    if not result:
        raise BuildError(f"{label} is empty")
    return result


def _stock_vitagl_expected_contract(
    cache: dict[str, str], source_files: dict[str, Path]
) -> tuple[dict[str, str], str]:
    mode_specs = (
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
    modes: dict[str, str] = {}
    flags = [
        "SOFTFP_ABI=1", "NO_DEBUG=1", "NO_SPLASHSCREEN=1",
        "SINGLE_THREADED_GC=1",
    ]
    for cache_key, contract_key, enabled_flag in mode_specs:
        enabled = _cache_bool(cache, cache_key)
        modes[contract_key] = "1" if enabled else "0"
        if enabled:
            flags.append(enabled_flag)
    build_flags = " ".join(flags)
    hashes = {name: _sha256_file(path) for name, path in source_files.items()}
    expected = {
        "profile": "stock-vitagl-reference",
        "source_commit": VITAGL_STOCK_REFERENCE_COMMIT,
        "source_sha256": VITAGL_STOCK_REFERENCE_SOURCE_SHA256,
        "backported_oob_fix_commit": VITAGL_STOCK_REFERENCE_OOB_FIX_COMMIT,
        "patch_sha256": hashes[
            "0001-deterministic-build-and-init-oob.patch"
        ],
        "script_sha256": hashes["build.sh"],
        "flags": build_flags,
        "gpu_draw_optimizations": modes["gpu_draw_optimizations"],
        "gpu_draw_patch_sha256": hashes[
            "0002-exact-gpu-draw-optimizations.patch"
        ],
        "gpu_draw_policy_sha256": hashes["isaac_gpu_draw_policy.h"],
        "gpu_draw_policy": (
            "exact-single-stream-layout-cache-draw-stats-v2"
            if modes["gpu_draw_optimizations"] == "1" else "disabled"
        ),
        "canonical_quad_zero_copy": modes["canonical_quad_zero_copy"],
        "gxm_state_shadow": modes["gxm_state_shadow"],
        "gxm_state_policy_sha256": hashes["isaac_gxm_state_policy.h"],
        "coloroffset_gpu_optimizations": modes[
            "coloroffset_gpu_optimizations"
        ],
        "coloroffset_patch_sha256": hashes[
            "0003-exact-coloroffset-gpu-optimizations.patch"
        ],
        "coloroffset_policy_sha256": hashes[
            "isaac_coloroffset_gpu_policy.h"
        ],
        "coloroffset_policy": (
            "exact-stock-source-plus-opaque-noblend-v3"
            if modes["coloroffset_gpu_optimizations"] == "1" else "disabled"
        ),
        "phase_profile": modes["phase_profile"],
        "shader_cache": modes["shader_cache"],
        "shader_cache_patch_sha256": hashes[
            "0004-hardened-custom-shader-cache.patch"
        ],
        "shader_cache_policy_sha256": hashes["isaac_shader_cache_policy.h"],
        "shader_cache_integration_sha256": hashes[
            "isaac_shader_cache_vitagl.h"
        ],
        "shader_cache_block_list_sha256": hashes[
            "isaac_shader_cache_block_list.h"
        ],
        "shader_cache_root": "ux0:data/isaacr001/shader-cache/v2",
        "shared_render_targets": "off",
        "gxm_source_patch": "none",
    }
    recipe_parts = (
        expected["source_commit"], expected["source_sha256"],
        expected["patch_sha256"], expected["script_sha256"],
        expected["flags"], expected["backported_oob_fix_commit"],
        expected["gpu_draw_optimizations"],
        expected["gpu_draw_patch_sha256"],
        expected["gpu_draw_policy_sha256"],
        expected["canonical_quad_zero_copy"],
        expected["gxm_state_shadow"], expected["gxm_state_policy_sha256"],
        expected["coloroffset_gpu_optimizations"],
        expected["coloroffset_patch_sha256"],
        expected["coloroffset_policy_sha256"], expected["phase_profile"],
        expected["shader_cache"],
        expected["shader_cache_patch_sha256"],
        expected["shader_cache_policy_sha256"],
        expected["shader_cache_integration_sha256"],
        expected["shader_cache_block_list_sha256"],
    )
    recipe = hashlib.sha256(
        "".join(f"{part}\n" for part in recipe_parts).encode("ascii")
    ).hexdigest()
    return expected, recipe


def collect_dependency_inputs(
    *,
    sdk: Path,
    build_dir: Path,
    cache: dict[str, str],
    kage_active: bool,
    loading_specialist: bool,
    audio_active: bool,
    lua_source: Path | None = None,
) -> list[dict[str, object]]:
    records = [
        _file_record(sdk / "share" / "vita.toolchain.cmake", "vitasdk/share/vita.toolchain.cmake"),
        _file_record(sdk / "share" / "vita.cmake", "vitasdk/share/vita.cmake"),
        _file_record(
            DETERMINISTIC_VPK_WRAPPER,
            "recomp/vita/deterministic_vpk.py",
        ),
        _file_record(
            DETERMINISTIC_VPK_CMAKE,
            "recomp/vita/deterministic_vpk.cmake",
        ),
    ]
    real_vpk_packer = _cached_path(
        build_dir, cache, "ISAAC_VITA_REAL_PACK_VPK"
    )
    cached_vpk_packer = _cached_path(build_dir, cache, "VITA_PACK_VPK")
    if real_vpk_packer != cached_vpk_packer:
        raise BuildError(
            "deterministic VPK real packer differs from VitaSDK cache: "
            f"{real_vpk_packer} != {cached_vpk_packer}"
        )
    expected_vpk_packer = (
        sdk / "bin" /
        ("vita-pack-vpk.exe" if os.name == "nt" else "vita-pack-vpk")
    ).resolve()
    if real_vpk_packer != expected_vpk_packer:
        raise BuildError(
            "deterministic VPK packer is not the exact VitaSDK tool: "
            f"{real_vpk_packer} != {expected_vpk_packer}"
        )
    records.append(_file_record(
        real_vpk_packer, f"vitasdk/bin/{real_vpk_packer.name}"
    ))
    for relative in DEPENDENCY_LOCK_CANDIDATES:
        candidate = REPO_ROOT / Path(*PurePosixPath(relative).parts)
        if candidate.is_file():
            records.append(_file_record(candidate, relative))

    if loading_specialist:
        specialist = _regular_file(
            REPO_ROOT / "recomp" / "runtime" /
            "kage_vita_loading_specialist.inc",
            "Specialist loading include",
        )
        records.append(
            _file_record(
                specialist,
                "local-loading/kage_vita_loading_specialist.inc",
            )
        )

    if "ISAAC_VITA_LUA" in cache:
        cached_lua_enabled = _cache_bool(cache, "ISAAC_VITA_LUA")
        if cached_lua_enabled != (lua_source is not None):
            raise BuildError(
                "Lua dependency source selection disagrees with the CMake cache"
            )
    if lua_source is not None:
        cached_lua_source = _cached_path(
            build_dir, cache, "ISAAC_LUA53_SOURCE_DIR"
        )
        if cached_lua_source != lua_source.resolve():
            raise BuildError(
                "Lua dependency source differs from the CMake cache: "
                f"{lua_source.resolve()} != {cached_lua_source}"
            )
        records.extend(_lua53_dependency_records(lua_source))

    if kage_active:
        stock_reference = _cache_bool(
            cache, "ISAAC_VITA_VITAGL_STOCK_REFERENCE"
        )
        if stock_reference:
            source_files: dict[str, Path] = {}
            for relative in VITAGL_STOCK_REFERENCE_INPUTS:
                path = REPO_ROOT / Path(*PurePosixPath(relative).parts)
                records.append(_file_record(path, relative))
                source_files[path.name] = path
            if len(source_files) != len(VITAGL_STOCK_REFERENCE_INPUTS):
                raise BuildError("stock vitaGL source-input basenames collide")
            overlay = _cached_path(
                build_dir, cache, "ISAAC_VITA_VITAGL_STOCK_REFERENCE_DIR"
            )
            recipe_path = _regular_file(
                overlay / "recipe.txt", "stock vitaGL recipe"
            )
            recipe = recipe_path.read_text(encoding="ascii").strip()
            if not _is_sha256(recipe):
                raise BuildError(
                    f"stock vitaGL recipe is not SHA-256: {recipe!r}"
                )
            library = _regular_file(
                overlay / "prefix" / "lib" / "libvitaGL.a",
                "stock vitaGL archive",
            )
            library_checksum = _regular_file(
                overlay / "libvitaGL.sha256",
                "stock vitaGL archive checksum",
            )
            library_fields = library_checksum.read_text(
                encoding="ascii"
            ).split()
            if (not library_fields or
                    library_fields[0] != _sha256_file(library)):
                raise BuildError(
                    "stock vitaGL checksum does not match libvitaGL.a"
                )
            header = _regular_file(
                overlay / "prefix" / "include" / "vitaGL.h",
                "stock vitaGL header",
            )
            header_checksum = _regular_file(
                overlay / "vitaGL.h.sha256", "stock vitaGL header checksum"
            )
            header_fields = header_checksum.read_text(
                encoding="ascii"
            ).split()
            if (not header_fields or
                    header_fields[0] != _sha256_file(header)):
                raise BuildError(
                    "stock vitaGL checksum does not match vitaGL.h"
                )
            contract = _regular_file(
                overlay / "build-contract.txt",
                "stock vitaGL build contract",
            )
            actual_contract = _read_key_value_contract(
                contract, "stock vitaGL build contract"
            )
            expected_contract, expected_recipe = (
                _stock_vitagl_expected_contract(cache, source_files)
            )
            if actual_contract != expected_contract:
                differences = {
                    key: (actual_contract.get(key), expected_contract.get(key))
                    for key in sorted(set(actual_contract) | set(expected_contract))
                    if actual_contract.get(key) != expected_contract.get(key)
                }
                raise BuildError(
                    f"stock vitaGL build contract differs: {differences!r}"
                )
            if recipe != expected_recipe:
                raise BuildError(
                    "stock vitaGL recipe differs from selected source/modes: "
                    f"{recipe} != {expected_recipe}"
                )
            records.extend(
                (
                    _file_record(
                        recipe_path, "vitagl-stock-reference/recipe.txt"
                    ),
                    _file_record(
                        contract,
                        "vitagl-stock-reference/build-contract.txt",
                    ),
                    _file_record(
                        library, "vitagl-stock-reference/libvitaGL.a"
                    ),
                    _file_record(
                        header, "vitagl-stock-reference/vitaGL.h"
                    ),
                )
            )
        else:
            for relative in (
                "recomp/vita/vitagl-overlay/build.sh",
                "recomp/vita/vitagl-overlay/0001-isaac-build-contract.patch",
            ):
                records.append(_file_record(REPO_ROOT / relative, relative))
            overlay = _cached_path(
                build_dir, cache, "ISAAC_VITA_VITAGL_OVERLAY_DIR"
            )
            recipe_path = _regular_file(
                overlay / "recipe.txt", "vitaGL overlay recipe"
            )
            recipe = recipe_path.read_text(encoding="ascii").strip()
            if not _is_sha256(recipe):
                raise BuildError(
                    f"vitaGL overlay recipe is not SHA-256: {recipe!r}"
                )
            library = _regular_file(
                overlay / "prefix" / "lib" / "libvitaGL.a",
                "vitaGL overlay archive",
            )
            library_checksum = _regular_file(
                overlay / "libvitaGL.sha256", "vitaGL overlay archive checksum"
            )
            library_fields = library_checksum.read_text(
                encoding="ascii"
            ).split()
            if (not library_fields or
                    library_fields[0] != _sha256_file(library)):
                raise BuildError(
                    "vitaGL overlay checksum does not match libvitaGL.a"
                )
            header = _regular_file(
                overlay / "prefix" / "include" / "vitaGL.h",
                "vitaGL overlay header",
            )
            header_checksum = _regular_file(
                overlay / "vitaGL.h.sha256", "vitaGL overlay header checksum"
            )
            header_fields = header_checksum.read_text(
                encoding="ascii"
            ).split()
            if (not header_fields or
                    header_fields[0] != _sha256_file(header)):
                raise BuildError(
                    "vitaGL overlay checksum does not match vitaGL.h"
                )
            contract = _regular_file(
                overlay / "build-contract.txt", "vitaGL overlay build contract"
            )
            records.extend(
                (
                    _file_record(recipe_path, "vitagl-overlay/recipe.txt"),
                    _file_record(contract, "vitagl-overlay/build-contract.txt"),
                    _file_record(library, "vitagl-overlay/libvitaGL.a"),
                    _file_record(header, "vitagl-overlay/vitaGL.h"),
                )
            )

    if audio_active:
        for relative in (
            "recomp/vita/openal-overlay/build.sh",
            "recomp/vita/openal-overlay/0001-limit-initial-voices.patch",
            "recomp/vita/openal-overlay/0002-route-direct-allocators.patch",
            "recomp/vita/openal-overlay/openal_pool_redirect.h",
            "recomp/vita/openal-overlay/verify_deterministic_archive.py",
            "recomp/runtime/host_vita_openal_pool.h",
            "recomp/vita/alsoft.conf",
        ):
            records.append(_file_record(REPO_ROOT / relative, relative))
        overlay = _cached_path(build_dir, cache, "ISAAC_VITA_OPENAL_OVERLAY_DIR")
        recipe_path = _regular_file(overlay / "recipe.txt", "OpenAL overlay recipe")
        recipe = recipe_path.read_text(encoding="ascii").strip()
        if not _is_sha256(recipe):
            raise BuildError(f"OpenAL overlay recipe is not SHA-256: {recipe!r}")
        library = _regular_file(
            overlay / "prefix" / "lib" / "libopenal.a", "OpenAL overlay archive"
        )
        checksum_path = _regular_file(
            overlay / "libopenal.sha256", "OpenAL overlay checksum"
        )
        checksum_fields = checksum_path.read_text(encoding="ascii").split()
        if not checksum_fields or checksum_fields[0] != _sha256_file(library):
            raise BuildError("OpenAL overlay checksum does not match libopenal.a")
        records.extend(
            (
                _file_record(recipe_path, "openal-overlay/recipe.txt"),
                _file_record(library, "openal-overlay/libopenal.a"),
            )
        )

    records.sort(key=lambda record: str(record["path"]))
    paths = [record["path"] for record in records]
    if len(paths) != len(set(paths)):
        raise BuildError("dependency input inventory contains duplicate logical paths")
    return records


def _valid_size_sha256_record(value: object) -> bool:
    return (
        isinstance(value, dict)
        and set(value) == {"size", "sha256"}
        and type(value["size"]) is int
        and value["size"] >= 0
        and _is_sha256(value["sha256"])
    )


def _validate_identity_records(core: dict[str, object]) -> None:
    linker_map = core.get("linker_map")
    if (
        not isinstance(linker_map, dict)
        or set(linker_map) != {"path", "raw", "canonical"}
        or linker_map.get("path") != LINKER_MAP_NAME
        or not _valid_size_sha256_record(linker_map.get("raw"))
    ):
        raise BuildError("build manifest linker-map evidence is malformed")
    canonical = linker_map.get("canonical")
    if (
        not isinstance(canonical, dict)
        or set(canonical) != {
            "schema", "algorithm", "size", "sha256", "replacements", "end",
        }
        or canonical.get("schema") != linker_map_contract.SCHEMA
        or canonical.get("algorithm") != linker_map_contract.ALGORITHM
        or type(canonical.get("size")) is not int
        or canonical["size"] < 0
        or not _is_sha256(canonical.get("sha256"))
        or re.fullmatch(r"0x[0-9a-f]{8}", str(canonical.get("end"))) is None
    ):
        raise BuildError("build manifest canonical linker map is malformed")
    replacements = canonical.get("replacements")
    expected_replacements = {
        "source_root", "generated_root", "vitasdk_root", "guest_generation",
    }
    if (
        not isinstance(replacements, dict)
        or set(replacements) != expected_replacements
        or any(type(value) is not int or value <= 0
               for value in replacements.values())
        or replacements.get("guest_generation")
        != linker_map_contract.EXPECTED_GUEST_GENERATION_COUNT
    ):
        raise BuildError(
            "build manifest linker-map replacement census is malformed"
        )

    gate = core.get("raw_allocator_gate")
    expected_gate_keys = {
        "schema", "stage", "result", "target_name", "profile",
        "source_count", "object_count", "closure_sha256", "raw_symbols",
        "forbidden_source_feature_view_macros", "exempt_sources", "receipt",
    }
    if (
        not isinstance(gate, dict)
        or set(gate) != expected_gate_keys
        or gate.get("schema") != 1
        or gate.get("stage") != "vita-raw-allocator-gate"
        or gate.get("result") != "PASS"
        or gate.get("target_name") != "isaac_first_arm_fault"
        or gate.get("profile") not in {
            RAW_ALLOCATOR_GATE_PROFILE_CANONICAL,
            RAW_ALLOCATOR_GATE_PROFILE_DIAGNOSTIC,
        }
        or type(gate.get("source_count")) is not int
        or gate["source_count"] <= 0
        or type(gate.get("object_count")) is not int
        or gate["object_count"] <= 0
        or not _is_sha256(gate.get("closure_sha256"))
        or gate.get("raw_symbols") != list(RAW_ALLOCATOR_GATE_RAW_SYMBOLS)
        or gate.get("forbidden_source_feature_view_macros") != list(
            RAW_ALLOCATOR_GATE_FORBIDDEN_SOURCE_FEATURE_VIEW_MACROS
        )
        or not isinstance(gate.get("exempt_sources"), list)
    ):
        raise BuildError("build manifest raw allocator contract is malformed")
    receipt = gate.get("receipt")
    if (
        not isinstance(receipt, dict)
        or set(receipt) != {"path", "size", "sha256"}
        or receipt.get("path") != RAW_ALLOCATOR_GATE_RECEIPT_NAME
        or not _valid_size_sha256_record({
            "size": receipt.get("size"), "sha256": receipt.get("sha256")
        })
    ):
        raise BuildError("build manifest raw allocator receipt is malformed")


def _build_identity_view(core: dict[str, object]) -> dict[str, object]:
    """Remove exact run receipts while retaining their semantic contracts."""

    _validate_identity_records(core)
    semantic = dict(core)
    linker_map = dict(semantic["linker_map"])
    linker_map.pop("raw")
    semantic["linker_map"] = linker_map
    raw_allocator_gate = dict(semantic["raw_allocator_gate"])
    raw_allocator_gate.pop("receipt")
    semantic["raw_allocator_gate"] = raw_allocator_gate
    return semantic


def _build_identity(core: dict[str, object]) -> str:
    return hashlib.sha256(
        BUILD_ID_DOMAIN + _canonical_bytes(_build_identity_view(core))
    ).hexdigest()


def _receipt_identity(manifest_without_receipt_id: dict[str, object]) -> str:
    return hashlib.sha256(
        RECEIPT_ID_DOMAIN + _canonical_bytes(manifest_without_receipt_id)
    ).hexdigest()


def _read_canonical_raw_allocator_receipt(
    path: Path,
) -> tuple[dict[str, object], dict[str, object]]:
    path = _regular_file(path, "Vita raw allocator gate receipt")
    try:
        payload = path.read_bytes()
        receipt = json.loads(payload.decode("utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BuildError(
            f"cannot read canonical raw allocator gate receipt: {exc}"
        ) from exc
    if (
        not isinstance(receipt, dict)
        or set(receipt) != RAW_ALLOCATOR_GATE_RECEIPT_KEYS
        or payload != _canonical_bytes(receipt) + b"\n"
    ):
        raise BuildError("raw allocator gate receipt is not canonical schema-1 JSON")
    record = {
        "path": RAW_ALLOCATOR_GATE_RECEIPT_NAME,
        "size": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest(),
    }
    return receipt, record


def _raw_allocator_manifest_record(
    receipt: dict[str, object], file_record: dict[str, object],
) -> dict[str, object]:
    return {
        "schema": receipt.get("schema"),
        "stage": receipt.get("stage"),
        "result": receipt.get("result"),
        "target_name": receipt.get("target_name"),
        "profile": receipt.get("profile"),
        "source_count": receipt.get("source_count"),
        "object_count": receipt.get("object_count"),
        "closure_sha256": receipt.get("closure_sha256"),
        "raw_symbols": receipt.get("raw_symbols"),
        "forbidden_source_feature_view_macros": receipt.get(
            "forbidden_source_feature_view_macros"
        ),
        "exempt_sources": receipt.get("exempt_sources"),
        "receipt": file_record,
    }


def compose_build_manifest(
    *,
    release: bool,
    git_state: dict[str, object],
    pe_path: Path,
    generation_manifest: dict[str, object],
    generated_dir: Path,
    configuration: dict[str, object],
    toolchain: dict[str, object],
    dependency_inputs: list[dict[str, object]],
    artifacts: dict[str, Path],
    linker_map: Path,
    raw_allocator_gate: dict[str, object],
    source_root: Path,
    vitasdk_root: Path,
) -> dict[str, object]:
    expected_artifacts = {"ELF", "VELF", "eboot", "VPK"}
    if set(artifacts) != expected_artifacts:
        raise BuildError(
            f"artifact inventory differs: {sorted(artifacts)} != "
            f"{sorted(expected_artifacts)}"
        )
    generated_manifest_path = generated_dir / "manifest.json"
    artifact_records = {
        label: _file_record(path, path.name)
        for label, path in sorted(artifacts.items())
    }
    raw_allocator_receipt = artifacts["ELF"].parent / RAW_ALLOCATOR_GATE_RECEIPT_NAME
    live_gate, gate_file_record = _read_canonical_raw_allocator_receipt(
        raw_allocator_receipt
    )
    if live_gate != raw_allocator_gate:
        raise BuildError(
            "verified raw allocator gate differs from its live receipt"
        )
    raw_allocator_record = _raw_allocator_manifest_record(
        live_gate, gate_file_record
    )
    core: dict[str, object] = {
        "schema": BUILD_MANIFEST_SCHEMA,
        "kind": "isaac-repentance-vita-build",
        "release": release,
        "git": git_state,
        "input_pe": _file_record(pe_path, "isaac-ng.exe.unpacked.exe"),
        "generation": {
            "schema": generation_manifest["schema"],
            "stage": generation_manifest["stage"],
            "recipe_id": generation_manifest["recipe_id"],
            "recipe": generation_manifest["recipe"],
            "output_set_id": generation_manifest["output_set_id"],
            "manifest": _file_record(generated_manifest_path, "manifest.json"),
        },
        "configuration": configuration,
        "toolchain": toolchain,
        "dependency_lock_inputs": dependency_inputs,
        "raw_allocator_gate": raw_allocator_record,
        "artifacts": artifact_records,
        "linker_map": _linker_map_record(
            linker_map,
            source_root=source_root,
            generated_root=generated_dir,
            vitasdk_root=vitasdk_root,
            generation_recipe_id=str(generation_manifest["recipe_id"]),
        ),
    }
    build_id = _build_identity(core)
    with_build_id = {**core, "build_id": build_id}
    receipt_id = _receipt_identity(with_build_id)
    return {**with_build_id, "receipt_id": receipt_id}


def invalidate_build_manifest(build_dir: Path) -> None:
    for name in (BUILD_MANIFEST_NAME, f".{BUILD_MANIFEST_NAME}.tmp"):
        path = build_dir / name
        if path.exists() or path.is_symlink():
            if not path.is_file():
                raise BuildError(f"build manifest path is not a regular file: {path}")
            path.unlink()


def write_build_manifest(
    build_dir: Path,
    manifest: dict[str, object],
    *,
    source_root: Path,
    generated_root: Path,
    vitasdk_root: Path,
    generation_recipe_id: str,
) -> Path:
    required_keys = {
        "schema",
        "kind",
        "release",
        "git",
        "input_pe",
        "generation",
        "configuration",
        "toolchain",
        "dependency_lock_inputs",
        "raw_allocator_gate",
        "artifacts",
        "linker_map",
        "build_id",
        "receipt_id",
    }
    if set(manifest) != required_keys or manifest.get("schema") != BUILD_MANIFEST_SCHEMA:
        raise BuildError("build manifest has an unexpected schema")
    core = {
        key: value for key, value in manifest.items()
        if key not in ("build_id", "receipt_id")
    }
    expected_id = _build_identity(core)
    if manifest.get("build_id") != expected_id:
        raise BuildError("build manifest build_id does not match its canonical content")
    expected_receipt_id = _receipt_identity({
        **core, "build_id": manifest["build_id"]
    })
    if manifest.get("receipt_id") != expected_receipt_id:
        raise BuildError(
            "build manifest receipt_id does not match its canonical content"
        )

    generation = manifest.get("generation")
    if (
        not isinstance(generation, dict)
        or generation.get("recipe_id") != generation_recipe_id
    ):
        raise BuildError(
            "build manifest generation recipe differs from writer context"
        )
    linker_map = manifest.get("linker_map")
    live_map = _linker_map_record(
        build_dir / LINKER_MAP_NAME,
        source_root=source_root,
        generated_root=generated_root,
        vitasdk_root=vitasdk_root,
        generation_recipe_id=generation_recipe_id,
    )
    if linker_map != live_map:
        raise BuildError(
            "build manifest linker-map record differs from live canonical map"
        )
    gate = manifest.get("raw_allocator_gate")
    gate_receipt = gate.get("receipt") if isinstance(gate, dict) else None
    live_gate_receipt = _file_record(
        build_dir / RAW_ALLOCATOR_GATE_RECEIPT_NAME,
        RAW_ALLOCATOR_GATE_RECEIPT_NAME,
    )
    if gate_receipt != live_gate_receipt:
        raise BuildError("build manifest raw allocator gate receipt is stale")
    live_gate, canonical_gate_record = _read_canonical_raw_allocator_receipt(
        build_dir / RAW_ALLOCATOR_GATE_RECEIPT_NAME
    )
    if canonical_gate_record != live_gate_receipt:
        raise BuildError("raw allocator gate receipt changed during verification")
    if gate != _raw_allocator_manifest_record(live_gate, canonical_gate_record):
        raise BuildError(
            "build manifest raw allocator semantics differ from live receipt"
        )
    destination = build_dir / BUILD_MANIFEST_NAME
    temporary = build_dir / f".{BUILD_MANIFEST_NAME}.tmp"
    if destination.exists() or destination.is_symlink():
        raise BuildError(f"refusing to overwrite build manifest: {destination}")
    if temporary.exists() or temporary.is_symlink():
        raise BuildError(f"temporary build manifest already exists: {temporary}")
    data = _canonical_bytes(manifest) + b"\n"
    try:
        with temporary.open("xb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, destination)
    except OSError:
        with contextlib.suppress(OSError):
            temporary.unlink()
        raise
    return destination


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Regenerate and build the local Isaac Repentance PS Vita VPK. "
            "No game binary or resource is downloaded or published."
        )
    )
    parser.add_argument("--pe", type=Path, help="exact unpacked Repentance PE")
    parser.add_argument(
        "--vitasdk",
        type=Path,
        default=Path(os.environ["VITASDK"]) if os.environ.get("VITASDK") else None,
        help="softfp VitaSDK root (default: VITASDK environment variable)",
    )
    parser.add_argument(
        "--generated-dir",
        type=Path,
        default=DEFAULT_GENERATED_DIR,
        help=(
            "generated C directory outside the source checkout "
            f"(default: {DEFAULT_GENERATED_DIR.as_posix()})"
        ),
    )
    parser.add_argument(
        "--analysis-dir",
        type=Path,
        default=Path("build/vita-analysis"),
        help="functions/bounds work directory (default: build/vita-analysis)",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path("build/vita-release"),
        help="CMake build directory (default: build/vita-release)",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=min(8, os.cpu_count() or 1),
        help="generation/build parallelism, 1..32 (default: up to 8)",
    )
    parser.add_argument(
        "--heap-mb",
        type=int,
        default=81,
        help=(
            "newlib heap MiB below the fixed image: 1..63, 65..79, or 81 "
            "(default: 81)"
        ),
    )
    parser.add_argument(
        "--kage",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="enable the Vita KAGE/vitaGL backend (default: enabled)",
    )
    parser.add_argument(
        "--loading-specialist",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "show the Specialist Dance loading animation (default: enabled; "
            "--no-loading-specialist draws the procedural fallback)"
        ),
    )
    parser.add_argument(
        "--audio",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="enable ISAAC_VITA_AUDIO (default: enabled)",
    )
    parser.add_argument(
        "--audio-stream-receipt",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "log bounded OGG queue state around cooperative audio updates "
            "(default: disabled)"
        ),
    )
    parser.add_argument(
        "--ogg-queue-emergency",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="allow exact OGG Queue OOM recovery through the idle one-slot reserve",
    )
    parser.add_argument(
        "--lua",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="enable the experimental pinned Lua 5.3.3 bridge (default: disabled)",
    )
    parser.add_argument(
        "--lua-source",
        type=Path,
        help=(
            "strict absolute path to a pristine lua-5.3.3 source root; "
            "required only with --lua"
        ),
    )
    parser.add_argument(
        "--openal-pool",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "route direct OpenAL allocations through a dedicated USER_RW "
            "mspace (default: enabled)"
        ),
    )
    parser.add_argument(
        "--io-profile",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="enable diagnostic startup I/O profiling (default: disabled)",
    )
    parser.add_argument(
        "--archive-file-cache",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "reuse one idle read-only packed-archive FILE "
            "(default: enabled)"
        ),
    )
    parser.add_argument(
        "--archive-validation-skip",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "skip startup validation only for PE-baked archive checksum "
            "matches (default: disabled)"
        ),
    )
    parser.add_argument(
        "--archive-validation-receipt",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "reuse a fail-closed exact-corpus receipt for animations.a and "
            "afterbirth.a after one full trusted validation "
            "(default: disabled)"
        ),
    )
    parser.add_argument(
        "--fios-cache",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "cache repeated reads below ux0:data/isaacr001 in a bounded "
            "FIOS2 USER_RW block (default: disabled)"
        ),
    )
    parser.add_argument(
        "--crt-seek-shadow",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "answer read-only SEEK_END/ftell pairs (File::GetSize/IsEOF) "
            "from a cached fstat size instead of newlib lseek+refill "
            "(default: disabled)"
        ),
    )
    parser.add_argument(
        "--continue-profile",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "aggregate FILE-selection and Continue stage timings "
            "(default: disabled)"
        ),
    )
    parser.add_argument(
        "--exit-menu-profile",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "attribute up to four Game-qualified menu rebuilds, including "
            "Exit (default: disabled)"
        ),
    )
    parser.add_argument(
        "--png-decode-profile",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "sample one aligned whole-image PNG cohort per 32 images, with "
            "raw image/filter/inflate/fast clocks (default: disabled)"
        ),
    )
    parser.add_argument(
        "--png-native-unfilter",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "experimentally route the exact frozen PNG row-unfilter target "
            "through guarded native code (default: disabled)"
        ),
    )
    parser.add_argument(
        "--texture-churn-profile",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "attribute texture-name scans, lifetimes and upload paths in "
            "one aggregate record per 120 loops; requires --no-audio and "
            "pins the stock vitaGL/direct-default profile (default: disabled)"
        ),
    )
    parser.add_argument(
        "--sim-cadence-receipt",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "count exact Game::Update entries and report their rate beside "
            "the present heartbeat, without phase profiling or scheduler "
            "changes (default: disabled)"
        ),
    )
    parser.add_argument(
        "--stable-30-presentation",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "present only completed 30-Hz Game phases, suppress the optional "
            "interpolation render and pace without a second heavy render "
            "(default: disabled)"
        ),
    )
    parser.add_argument(
        "--laser-ring-shadow-skip",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "omit only sampled/ring laser floor shadows, preserving beam "
            "rendering and gameplay (default: disabled)"
        ),
    )
    parser.add_argument(
        "--world-seam-diag",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "capture bounded Room/Lua/late-overlay GL-state receipts for "
            "one control and two Utero-II frames (default: disabled)"
        ),
    )
    parser.add_argument(
        "--game-log-batch",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "flush the exact guest INFO logger every eight records while "
            "keeping WARN/ERROR/ASSERT synchronous (default: disabled)"
        ),
    )
    parser.add_argument(
        "--continue-overlay",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "show the coarse status overlay while --continue-profile runs "
            "(default: disabled)"
        ),
    )
    parser.add_argument(
        "--texel-oom-diagnostic",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="enable the one-shot recognized texel-loader OOM diagnostic (default: enabled)",
    )
    parser.add_argument(
        "--texel-scratch",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="enable the owner-scoped transient texel scratch allocator (default: enabled)",
    )
    parser.add_argument(
        "--texture-align8-policy",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="enable the accepted KAGE 8-pixel texture alignment policy (default: enabled)",
    )
    parser.add_argument(
        "--anm2-scratch",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="enable the accepted owner-scoped ANM2 scratch allocator (default: enabled)",
    )
    parser.add_argument(
        "--fxlayers-null-rollback",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="enable the accepted FXLayers NULL-image rollback (default: enabled)",
    )
    parser.add_argument(
        "--fxray-alpha-mask",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "store exact white 256x512 FXLayers ray images as alpha-only "
            "vitaGL textures (default: enabled)"
        ),
    )
    parser.add_argument(
        "--heap-ledger-memblock",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "store the native guest-ownership ledger in dedicated USER_RW "
            "blocks instead of the newlib heap (default: enabled)"
        ),
    )
    parser.add_argument(
        "--heap-ledger-backshift",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "delete ownership-ledger entries without tombstones or "
            "same-size rehashes (default: enabled)"
        ),
    )
    parser.add_argument(
        "--heap-overflow-mspace",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "use the bounded USER_RW SceClibMspace after newlib exhaustion "
            "(default: enabled)"
        ),
    )
    parser.add_argument(
        "--heap-terminal-fastpath",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "read the terminal heap latch without taking the allocator lock "
            "(default: disabled; requires --heap-overflow-mspace)"
        ),
    )
    parser.add_argument(
        "--room-entry-slab",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "pack the exact frozen 12-byte RoomConfig Entry arrays into "
            "retained overflow-mspace pages (default: enabled)"
        ),
    )
    parser.add_argument(
        "--room-entry-hybrid",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "extend the exact RoomConfig Entry slab to a bounded 192-page "
            "tier with lazy USER_RW chunks (default: enabled)"
        ),
    )
    parser.add_argument(
        "--reanalyse",
        action="store_true",
        help="discard and rebuild the switch-analysis cache too",
    )
    parser.add_argument(
        "--generate-only",
        action="store_true",
        help=(
            "stop after the C corpus in --generated-dir is regenerated and "
            "verified; configure and build nothing"
        ),
    )
    parser.add_argument(
        "--cmake",
        default="cmake",
        help="CMake executable name/path (default: cmake)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run deterministic host-only checks; do not generate or build",
    )
    parser.add_argument(
        "--release",
        action="store_true",
        help=(
            "fail-closed release-candidate build (no publishing): requires a clean "
            "audited tree, --reanalyse, fresh output directories, Ninja, "
            "PYTHONHASHSEED=0, and SOURCE_DATE_EPOCH=HEAD commit time"
        ),
    )
    return parser


def _resolve_output(path: Path) -> Path:
    return (REPO_ROOT / path).resolve() if not path.is_absolute() else path.resolve()


def _absolute_path_segments(path_text: str) -> tuple[list[str] | None, str | None]:
    """Split one canonical host absolute path without normalising it."""

    if not path_text:
        return None, "is empty"
    if path_text.startswith(("//", "\\\\")):
        return None, "must not be a UNC path"
    if path_text.endswith(("/", "\\")):
        return None, "must not end in a path separator"
    if os.name == "nt":
        if re.match(r"^[A-Za-z]:/", path_text) is None:
            return None, "must be canonical absolute with an X:/ prefix"
        tail = path_text[3:]
    else:
        if not path_text.startswith("/"):
            return None, "must be canonical absolute with a '/' prefix"
        tail = path_text[1:]
    if not tail:
        return None, "must contain a non-root path segment"
    segments = tail.split("/")
    if any(not segment for segment in segments):
        return None, "contains an empty path segment"
    for segment in segments:
        if segment in (".", ".."):
            return None, f"contains forbidden {segment!r} path segment"
    return segments, None


def _strict_absolute_path_problem(path_text: str) -> str | None:
    segments, problem = _absolute_path_segments(path_text)
    if problem is not None:
        return problem
    assert segments is not None
    for segment in segments:
        if STRICT_PATH_SEGMENT_RE.fullmatch(segment) is None:
            return f"has a non-portable strict path segment: {segment!r}"
    return None


def _lua_source_option_value(lua: bool, lua_source: Path | None) -> str:
    """Return the canonical CMake cache value for the opt-in Lua source."""

    if lua and lua_source is None:
        raise BuildError("--lua requires --lua-source")
    if not lua and lua_source is not None:
        raise BuildError("--lua-source is only valid with --lua")
    if lua_source is None:
        return ""
    source_text = lua_source.as_posix()
    problem = _strict_absolute_path_problem(source_text)
    if problem is not None:
        raise BuildError(
            f"strict Lua 5.3.3 source root {problem}: {source_text!r}"
        )
    resolved = lua_source.resolve()
    resolved_text = resolved.as_posix()
    resolved_problem = _strict_absolute_path_problem(resolved_text)
    if resolved_problem is not None:
        raise BuildError(
            "resolved strict Lua 5.3.3 source root "
            f"{resolved_problem}: {resolved_text!r}"
        )
    return resolved_text


def _generated_absolute_path_problem(path_text: str) -> str | None:
    segments, problem = _absolute_path_segments(path_text)
    if problem is not None:
        return problem
    assert segments is not None
    for segment in segments:
        if GENERATED_PATH_SEGMENT_RE.fullmatch(segment) is None:
            return f"has a non-portable generated path segment: {segment!r}"
        opens, closes = segment.count("["), segment.count("]")
        if opens != closes:
            return (
                "has unequal '[' and ']' counts in generated path segment "
                f"{segment!r} ({opens} != {closes})"
            )
    return None


def _portable_relative_path_problem(path_text: str) -> str | None:
    """Return why a Git/Specialist relative path is not cross-host portable."""

    if not path_text:
        return "is empty"
    if path_text.startswith(("/", "\\")) or re.match(
            r"^[A-Za-z]:", path_text):
        return "must be relative without a drive/UNC prefix"
    if path_text.endswith(("/", "\\")):
        return "must not end in a path separator"
    segments = path_text.split("/")
    if any(not segment for segment in segments):
        return "contains an empty path segment"
    for segment in segments:
        if segment in (".", ".."):
            return f"contains forbidden {segment!r} path segment"
        if STRICT_PATH_SEGMENT_RE.fullmatch(segment) is None:
            return f"has a non-portable relative path segment: {segment!r}"
    return None


def _validate_build_path_policy(
    *,
    source_root: Path,
    sdk_root: Path,
    toolchain: Path,
    pe_path: Path,
    generated_dir: Path,
    build_dir: Path,
    lua_source: Path | None = None,
) -> None:
    """Fail before filesystem probes when a build path is not portable."""

    strict_paths = [
        ("source root", source_root),
        ("VitaSDK root", sdk_root),
        ("VitaSDK toolchain", toolchain),
        ("input PE", pe_path),
        ("build directory", build_dir),
    ]
    if lua_source is not None:
        strict_paths.append(("Lua 5.3.3 source root", lua_source))
    for label, path in strict_paths:
        path_text = path.as_posix()
        problem = _strict_absolute_path_problem(path_text)
        if problem is not None:
            raise BuildError(
                f"strict build {label} {problem}: {path_text!r}"
            )
    generated_text = generated_dir.as_posix()
    generated_problem = _generated_absolute_path_problem(generated_text)
    if generated_problem is not None:
        raise BuildError(
            "generated directory " + generated_problem +
            f": {generated_text!r}"
        )


def _validate_source_relative_path_census(source_root: Path) -> tuple[str, ...]:
    """Validate every tracked source name; the Specialist include must be one."""

    raw = _git_bytes(source_root, "ls-files", "--cached", "-z")
    try:
        tracked = [item.decode("utf-8", errors="strict")
                   for item in raw.split(b"\0") if item]
    except UnicodeDecodeError as exc:
        raise BuildError("Git returned a non-UTF-8 tracked path") from exc
    if tracked != sorted(set(tracked)):
        raise BuildError("Git returned duplicate or noncanonical tracked paths")
    if LOADING_SPECIALIST_RELATIVE_PATH not in tracked:
        raise BuildError("Specialist loading include is not tracked")
    names = tuple(tracked)
    for name in names:
        problem = _portable_relative_path_problem(name)
        if problem is not None:
            raise BuildError(f"source relative path {name!r} {problem}")
    return names


def _contains(parent: Path, child: Path) -> bool:
    try:
        child.relative_to(parent)
    except ValueError:
        return False
    return True


def _validate_output_directories(
    pe_path: Path,
    sdk: Path,
    generated_dir: Path,
    analysis_dir: Path,
    build_dir: Path,
    lua_source: Path | None = None,
) -> None:
    outputs = {
        "generated": generated_dir,
        "analysis": analysis_dir,
        "build": build_dir,
    }
    if len(set(outputs.values())) != len(outputs):
        raise BuildError("generated, analysis, and build directories must differ")
    for left_name, left in outputs.items():
        for right_name, right in outputs.items():
            if left_name < right_name and (_contains(left, right) or _contains(right, left)):
                raise BuildError(
                    f"{left_name} and {right_name} directories must not contain one another"
                )

    protected = (
        REPO_ROOT,
        RECOMP_DIR,
        VITA_SOURCE_DIR,
        REPO_ROOT / "tools",
        REPO_ROOT / "include",
        sdk,
        *((lua_source,) if lua_source is not None else ()),
    )
    for label, output in outputs.items():
        for protected_path in protected:
            if output == protected_path or _contains(output, protected_path):
                raise BuildError(
                    f"refusing to use {label} directory {output}; it contains "
                    f"protected source/toolchain path {protected_path}"
                )
        if _contains(output, pe_path):
            raise BuildError(
                f"refusing to use {label} directory {output}; it contains the input PE"
            )
        if _contains(REPO_ROOT, output) and not any(
            _contains(scratch, output)
            for scratch in (REPO_ROOT / "build", RECOMP_DIR / "out")
        ):
            raise BuildError(
                f"refusing to use {label} directory inside the source tree: {output}; "
                "use build/ or recomp/out/"
            )

    # gen_all --force deletes C/H/JSON/TXT files directly in its output
    # directory.  Accept a clean directory or a recognisable interrupted/old
    # generation, but never reinterpret unrelated source as generated litter.
    if generated_dir.is_dir():
        victims = [
            item.name
            for item in generated_dir.iterdir()
            if item.is_file() and item.suffix.lower() in {".c", ".h", ".json", ".txt"}
        ]

        def generated_name(name: str) -> bool:
            if name in {
                "manifest.json",
                "guest_coverage.json",
                "guest_coverage_generated.h",
                "guest_funcs.h",
                "guest_stubs.c",
                "guest_table.c",
            }:
                return True
            return (
                len(name) == len("guest_0000.c")
                and name.startswith("guest_")
                and name.endswith(".c")
                and name[6:10].isdigit()
            )

        unrelated = sorted(name for name in victims if not generated_name(name))
        if unrelated:
            raise BuildError(
                "generated directory contains files that --force would delete: "
                + ", ".join(unrelated)
            )


def _validate_arguments(args: argparse.Namespace) -> None:
    if args.pe is None:
        raise BuildError("--pe is required")
    if args.vitasdk is None:
        raise BuildError("--vitasdk is required (or set VITASDK)")
    if args.lua and args.lua_source is None:
        raise BuildError("--lua requires --lua-source")
    if not args.lua and args.lua_source is not None:
        raise BuildError("--lua-source is only valid with --lua")
    if args.generate_only and args.release:
        raise BuildError("--generate-only is not valid with --release")
    if not 1 <= args.jobs <= 32:
        raise BuildError("--jobs must be between 1 and 32")
    if not heap_mb_is_supported(args.heap_mb):
        raise BuildError(
            "--heap-mb must be 1..63, 65..79, or 81; the measured Vita "
            "allocator alignment makes 64 and 80 MiB overlap the fixed "
            "0x98000000 image, and larger sizes lack a safe ANM2 gap"
        )
    if (args.anm2_scratch or args.texel_scratch or
            args.texel_oom_diagnostic) and args.heap_mb != 81:
        raise BuildError(
            "ANM2/texel scratch contracts require --heap-mb=81"
        )
    if args.heap_overflow_mspace and (
            args.heap_mb != 81 or
            not args.heap_ledger_memblock or
            not args.heap_ledger_backshift):
        raise BuildError(
            "--heap-overflow-mspace requires --heap-mb=81, "
            "--heap-ledger-memblock and --heap-ledger-backshift"
        )
    if args.heap_terminal_fastpath and not args.heap_overflow_mspace:
        raise BuildError(
            "--heap-terminal-fastpath requires --heap-overflow-mspace"
        )
    if args.room_entry_slab and not args.heap_overflow_mspace:
        raise BuildError(
            "--room-entry-slab requires --heap-overflow-mspace"
        )
    if args.room_entry_hybrid and (
            not args.room_entry_slab or not args.heap_overflow_mspace):
        raise BuildError(
            "--room-entry-hybrid requires --room-entry-slab and "
            "--heap-overflow-mspace"
        )
    if args.io_profile and not args.kage:
        raise BuildError("--io-profile requires --kage")
    if args.png_decode_profile and not args.kage:
        raise BuildError("--png-decode-profile requires --kage")
    if args.png_native_unfilter and not args.kage:
        raise BuildError("--png-native-unfilter requires --kage")
    if args.png_native_unfilter and not args.heap_overflow_mspace:
        raise BuildError(
            "--png-native-unfilter requires --heap-overflow-mspace"
        )
    if args.texture_churn_profile and not args.kage:
        raise BuildError("--texture-churn-profile requires --kage")
    if args.texture_churn_profile and args.audio:
        raise BuildError(
            "--texture-churn-profile requires --no-audio because its pinned "
            "stock vitaGL reference excludes the audio profile"
        )
    if args.texture_churn_profile and args.io_profile:
        raise BuildError(
            "--texture-churn-profile cannot be combined with --io-profile "
            "in the pinned stock vitaGL reference"
        )
    if args.sim_cadence_receipt and not args.kage:
        raise BuildError("--sim-cadence-receipt requires --kage")
    if args.sim_cadence_receipt and args.texture_churn_profile:
        raise BuildError(
            "--sim-cadence-receipt cannot be combined with "
            "--texture-churn-profile/ISAAC_VITA_PHASE_PROFILE"
        )
    if args.stable_30_presentation and not args.kage:
        raise BuildError("--stable-30-presentation requires --kage")
    if args.laser_ring_shadow_skip and not args.kage:
        raise BuildError("--laser-ring-shadow-skip requires --kage")
    if args.audio_stream_receipt and (not args.kage or not args.audio):
        raise BuildError(
            "--audio-stream-receipt requires --kage and --audio"
        )
    if args.world_seam_diag and not args.kage:
        raise BuildError("--world-seam-diag requires --kage")
    if args.continue_overlay and not args.continue_profile:
        raise BuildError("--continue-overlay requires --continue-profile")
    if args.continue_overlay and not args.kage:
        raise BuildError("--continue-overlay requires --kage")
    if (args.archive_validation_receipt and
            not args.archive_validation_skip):
        raise BuildError(
            "--archive-validation-receipt requires "
            "--archive-validation-skip"
        )
    kage_features = [
        name
        for name, enabled in (
            ("--loading-specialist", args.loading_specialist),
            ("--texel-oom-diagnostic", args.texel_oom_diagnostic),
            ("--texel-scratch", args.texel_scratch),
            ("--texture-align8-policy", args.texture_align8_policy),
            ("--fxlayers-null-rollback", args.fxlayers_null_rollback),
            ("--fxray-alpha-mask", args.fxray_alpha_mask),
            ("--png-decode-profile", args.png_decode_profile),
            ("--png-native-unfilter", args.png_native_unfilter),
            ("--texture-churn-profile", args.texture_churn_profile),
            ("--sim-cadence-receipt", args.sim_cadence_receipt),
            ("--stable-30-presentation", args.stable_30_presentation),
            ("--laser-ring-shadow-skip", args.laser_ring_shadow_skip),
            ("--audio-stream-receipt", args.audio_stream_receipt),
            ("--world-seam-diag", args.world_seam_diag),
        )
        if enabled
    ]
    if not args.kage and kage_features:
        raise BuildError(
            "--no-kage requires disabling KAGE-only features: "
            + ", ".join(kage_features)
        )


def _self_test_default_generated_dir() -> None:
    """Keep the documented no-override build outside gen_all's source guard."""

    defaults = _argument_parser().parse_args([])
    actual = _resolve_output(defaults.generated_dir)
    expected = (REPO_ROOT.parent / "repentogxm-build" / "vita-generated").resolve()
    assert actual == expected
    assert not _contains(REPO_ROOT, actual)

    recomp_text = str(RECOMP_DIR)
    if recomp_text not in sys.path:
        sys.path.insert(0, recomp_text)
    import gen_all

    validated = gen_all._validate_generation_outdir_path(str(actual))
    assert validated == os.path.normcase(os.path.realpath(actual))
    old_inside_default = REPO_ROOT / "build" / "vita-generated"
    try:
        gen_all._validate_generation_outdir_path(str(old_inside_default))
    except RuntimeError as exc:
        assert "inside the source repository" in str(exc)
    else:
        raise AssertionError("gen_all accepted the old inside-repository default")


def _self_test_build_path_policy(root: Path) -> None:
    """Pin role-specific portability and the pre-probe fail-fast order."""

    import inspect

    prefix = "C:/portable" if os.name == "nt" else "/portable"
    strict_pass = (
        prefix,
        prefix + "/Alpha_2/beta-3/file.name",
        prefix + "/.hidden/path_1",
    )
    generated_pass = (*strict_pass,
                      prefix + "/generated_[left]_]rev[_*_?")
    for candidate in strict_pass:
        assert _strict_absolute_path_problem(candidate) is None
    for candidate in generated_pass:
        assert _generated_absolute_path_problem(candidate) is None

    strict_red = (
        "", "relative/path", "//server/share", prefix + "/",
        prefix + "//empty", prefix + "/./dot", prefix + "/../dotdot",
        prefix + "/has space", prefix + "/bracket[x]",
        prefix + "/star*", prefix + "/question?", prefix + "/plus+",
        prefix + "/caret^", prefix + "/dollar$", prefix + "/semi;colon",
        prefix + "/back\\slash", prefix + "/pipe|", prefix + '/quote"',
        prefix + "/unicode-é",
    )
    for candidate in strict_red:
        assert _strict_absolute_path_problem(candidate) is not None, candidate
    generated_red = (
        "", "relative/path", "//server/share", prefix + "/",
        prefix + "//empty", prefix + "/./dot", prefix + "/../dotdot",
        prefix + "/ leading", prefix + "/trailing ", prefix + "/has space",
        prefix + "/open[/close]", prefix + "/open[", prefix + "/close]",
        prefix + "/plus+", prefix + "/caret^", prefix + "/dollar$",
        prefix + "/semi;colon", prefix + "/back\\slash",
        prefix + "/pipe|", prefix + "/quote'", prefix + '/quote"',
        prefix + "/unicode-é",
    )
    for candidate in generated_red:
        assert _generated_absolute_path_problem(candidate) is not None, candidate

    relative_pass = (
        ".gitattributes", "README.md", "recomp/runtime/guest.c",
        LOADING_SPECIALIST_RELATIVE_PATH,
    )
    for candidate in relative_pass:
        assert _portable_relative_path_problem(candidate) is None
    for candidate in (
        "", "/absolute", "../escape", "recomp//guest.c", "recomp/./x",
        "C:/drive", "space name", "unicode-é", "back\\slash",
    ):
        assert _portable_relative_path_problem(candidate) is not None, candidate
    source_names = _validate_source_relative_path_census(REPO_ROOT)
    tracked_names = tuple(
        item.decode("utf-8") for item in
        _git_bytes(REPO_ROOT, "ls-files", "--cached", "-z").split(b"\0")
        if item
    )
    assert source_names == tracked_names
    assert LOADING_SPECIALIST_RELATIVE_PATH in source_names

    safe_paths = {
        "source_root": root / "source",
        "sdk_root": root / "sdk",
        "toolchain": root / "sdk" / "share" / "vita.toolchain.cmake",
        "pe_path": root / "input.exe",
        "generated_dir": root / "generated_[left]_]rev[_*_?",
        "build_dir": root / "build",
    }
    _validate_build_path_policy(**safe_paths)
    for field, label in (
        ("source_root", "source root"),
        ("sdk_root", "VitaSDK root"),
        ("toolchain", "VitaSDK toolchain"),
        ("pe_path", "input PE"),
        ("build_dir", "build directory"),
    ):
        candidate_paths = {**safe_paths, field: root / f"bad {field}"}
        try:
            _validate_build_path_policy(**candidate_paths)
        except BuildError as exc:
            assert f"strict build {label}" in str(exc)
        else:
            raise AssertionError(f"unsafe strict {label} was accepted")
    try:
        _validate_build_path_policy(
            **{**safe_paths, "generated_dir": root / "generated+$"}
        )
    except BuildError as exc:
        assert "generated directory" in str(exc)
    else:
        raise AssertionError("unsafe generated directory was accepted")

    # Analysis never crosses into CMake, Ninja, or packaging.  Its existing
    # containment/lock policy deliberately remains independent of this class.
    _validate_output_directories(
        safe_paths["pe_path"], safe_paths["sdk_root"],
        safe_paths["generated_dir"], root / "analysis hostile +^$ [ ]",
        safe_paths["build_dir"],
    )
    assert list(root.iterdir()) == []

    # Exercise the real CLI boundary with nonexistent inputs.  Each invalid
    # role must win before PE/SDK existence checks and leave every requested
    # output path absent; the hostile analysis path is intentionally allowed.
    common = [
        sys.executable, "-B", str(Path(__file__).resolve()),
        "--pe", str(root / "input.exe"),
        "--vitasdk", str(root / "sdk"),
        "--generated-dir", str(root / "generated"),
        "--analysis-dir", str(root / "analysis hostile +^$ [ ]"),
        "--build-dir", str(root / "build"),
    ]
    cli_red = (
        ("--build-dir", root / "bad build", "strict build build directory"),
        ("--generated-dir", root / "generated has space", "generated directory"),
        ("--generated-dir", root / "generated+$", "generated directory"),
        ("--pe", root / "bad input.exe", "strict build input PE"),
        ("--vitasdk", root / "bad sdk", "strict build VitaSDK root"),
    )
    environment = {**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}
    for option, candidate, expected in cli_red:
        command = list(common)
        index = command.index(option)
        command[index + 1] = str(candidate)
        completed = subprocess.run(
            command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, check=False, env=environment,
        )
        assert completed.returncode == 1, (completed.stdout, completed.stderr)
        assert expected in completed.stderr, completed.stderr
        assert list(root.iterdir()) == []

    main_source = inspect.getsource(main)
    resolution_statement = "    source_root = REPO_ROOT.resolve()"
    validation_statement = "    _validate_build_path_policy("
    census_statement = "    _validate_source_relative_path_census(source_root)"
    local_import_statement = (
        "    _load_linker_map_contract()\n\n"
        '    pe_path = _regular_file(pe_path, "unpacked PE")'
    )
    existence_statement = '    pe_path = _regular_file(pe_path, "unpacked PE")'
    import_statement = "        import capstone  # noqa: F401"
    lock_statement = "    with _pipeline_locks(generated_dir, analysis_dir, build_dir):"
    for statement in (
        resolution_statement, validation_statement, census_statement,
        local_import_statement,
        existence_statement, import_statement, lock_statement,
    ):
        assert main_source.count(statement) == 1
    offsets = [main_source.index(statement) for statement in (
        resolution_statement, validation_statement, census_statement,
        local_import_statement,
        existence_statement, import_statement, lock_statement,
    )]
    assert offsets == sorted(offsets)
    validation_offset = main_source.index(validation_statement)
    for output_statement in (
        "        build_dir.mkdir(",
        "        run_softfp_link_probe(",
        "        run_fresh_generation(",
    ):
        assert main_source.index(output_statement) > validation_offset


def _self_test_lua_contract(root: Path) -> None:
    """Exercise the opt-in CLI and hostile Lua source identities."""

    pinned = _load_lua53_pinned_records()
    assert len(pinned) == LUA53_SOURCE_COUNT
    assert len({name for name, unused_sha256 in pinned}) == LUA53_SOURCE_COUNT
    assert sum(name.endswith(".c") for name, unused_sha256 in pinned) == 33
    assert _is_sha256(_lua53_source_set_sha256(pinned))

    parser = _argument_parser()
    base = ["--pe", "fixture", "--vitasdk", "sdk"]
    defaults = parser.parse_args(base)
    assert not defaults.lua
    assert defaults.lua_source is None
    explicit_off = parser.parse_args([*base, "--no-lua"])
    assert not explicit_off.lua
    assert explicit_off.lua_source is None
    for extra, expected in (
        (("--lua",), "--lua requires --lua-source"),
        (("--lua-source", str(root)), "--lua-source is only valid with --lua"),
    ):
        invalid = parser.parse_args([*base, *extra])
        try:
            _validate_arguments(invalid)
        except BuildError as exc:
            assert expected in str(exc)
        else:
            raise AssertionError(f"invalid Lua CLI was accepted: {extra!r}")

    assert _lua_source_option_value(False, None) == ""
    try:
        _lua_source_option_value(True, Path("relative-lua-5.3.3"))
    except BuildError as exc:
        assert "strict Lua 5.3.3 source root" in str(exc)
        assert "absolute" in str(exc)
    else:
        raise AssertionError("relative --lua-source was accepted")
    try:
        _lua_source_option_value(True, root / "lua source with spaces")
    except BuildError as exc:
        assert "non-portable strict path segment" in str(exc)
    else:
        raise AssertionError("non-strict absolute --lua-source was accepted")

    source_root = root / "lua-5.3.3-hostile"
    source_dir = source_root / "src"
    source_dir.mkdir(parents=True)
    first_name, unused_first_sha256 = pinned[0]
    try:
        validate_lua53_source(source_root)
    except BuildError as exc:
        assert f"source is missing or symlinked: {first_name}" in str(exc)
    else:
        raise AssertionError("missing pinned Lua source was accepted")
    (source_dir / first_name).write_bytes(b"not pristine Lua\n")
    try:
        validate_lua53_source(source_root)
    except BuildError as exc:
        assert f"source hash mismatch for {first_name}" in str(exc)
    else:
        raise AssertionError("wrong pinned Lua source hash was accepted")

    fixture_root = root / "lua-5.3.3-fixture"
    fixture_src = fixture_root / "src"
    fixture_src.mkdir(parents=True)
    fixture_payloads = {
        "fixture.c": b"fixture C\n",
        "fixture.h": b"fixture H\n",
    }
    fixture_records = tuple(
        (name, hashlib.sha256(payload).hexdigest())
        for name, payload in fixture_payloads.items()
    )
    for name, payload in fixture_payloads.items():
        (fixture_src / name).write_bytes(payload)
    verified = _validate_lua53_source_records(fixture_root, fixture_records)
    assert [record["path"] for record in verified] == [
        f"{LUA53_LOGICAL_SOURCE_PREFIX}/{name}" for name in fixture_payloads
    ]


def _self_test_pipeline_locks(root: Path) -> None:
    """Prove all mutable pipeline directories share fail-fast exclusion."""

    import inspect
    import threading

    recomp_text = str(RECOMP_DIR)
    if recomp_text not in sys.path:
        sys.path.insert(0, recomp_text)
    import build_contract

    generated = root / "generated-parent" / "generated"
    analysis = root / "analysis"
    build = root / "build-parent" / "release"
    expected = {
        generated.resolve().parent / "pipeline.lock",
        analysis.resolve() / "pipeline.lock",
        build.resolve().parent / ".release.repentogxm-pipeline.lock",
    }
    paths = _pipeline_lock_paths(generated, analysis, build)
    assert set(paths) == expected
    identities = [os.path.normcase(os.path.realpath(path)) for path in paths]
    assert identities == sorted(set(identities))

    probe = (
        "import sys\n"
        "sys.path.insert(0, sys.argv[1])\n"
        "import build_contract as BC\n"
        "try:\n"
        "    with BC.exclusive_lock(sys.argv[2]):\n"
        "        pass\n"
        "except BC.ContractIOError:\n"
        "    raise SystemExit(0)\n"
        "raise SystemExit(9)\n"
    )
    with _pipeline_locks(generated, analysis, build) as held_locks:
        assert held_locks == paths
        for held_lock in held_locks:
            # Legacy funcs/bounds/gen_all stages nest their exact lock here.
            with build_contract.exclusive_lock(held_lock):
                pass

            contender_errors: list[BaseException] = []
            contender_acquired: list[bool] = []

            def contend_in_thread() -> None:
                try:
                    with build_contract.exclusive_lock(held_lock):
                        contender_acquired.append(True)
                except BaseException as exc:
                    contender_errors.append(exc)

            contender = threading.Thread(target=contend_in_thread)
            contender.start()
            contender.join(timeout=5)
            assert not contender.is_alive()
            assert not contender_acquired
            assert len(contender_errors) == 1
            assert isinstance(contender_errors[0], build_contract.ContractIOError)

            competing_process = subprocess.run(
                [sys.executable, "-c", probe, recomp_text, str(held_lock)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )
            assert competing_process.returncode == 0, (
                competing_process.returncode,
                competing_process.stdout,
                competing_process.stderr,
            )

    # Both a completed build and a failed nested stage release every lock.
    for path in paths:
        with build_contract.exclusive_lock(path):
            pass

    main_source = inspect.getsource(main)
    lock_statement = "    with _pipeline_locks(generated_dir, analysis_dir, build_dir):"
    assert main_source.count(lock_statement) == 1
    lock_offset = main_source.index(lock_statement)
    for protected_statement in (
        "        build_dir.mkdir(",
        "        invalidate_build_manifest(",
        "        run_softfp_link_probe(",
        "        run_fresh_generation(",
        "        verify_package(",
        "        manifest_path = write_build_manifest(",
    ):
        assert main_source.count(protected_statement) == 1
        assert main_source.index(protected_statement) > lock_offset
    try:
        with _pipeline_locks(generated, analysis, build):
            with build_contract.exclusive_lock(generated.parent / "pipeline.lock"):
                raise BuildError("lock-release fixture")
    except BuildError as exc:
        assert str(exc) == "lock-release fixture"
    else:
        raise AssertionError("pipeline lock fixture did not raise")
    for path in paths:
        with build_contract.exclusive_lock(path):
            pass


def _self_test() -> None:
    """Exercise manifest, command, package, and negative checks without SDK."""

    if not __debug__:
        raise BuildError("build_vita --self-test requires assertions (do not use python -O)")

    with tempfile.TemporaryDirectory(prefix="isaac-build-vita-test-") as temporary:
        root = Path(temporary)
        _self_test_default_generated_dir()
        _self_test_build_path_policy(root)
        _self_test_lua_contract(root)
        _self_test_pipeline_locks(root)
        pe_path = root / "input.bin"
        pe_path.write_bytes(b"fixture-pe\x00")
        generated = root / "generated"
        generated.mkdir()
        for name, payload in (
            ("guest_0000.c", b"zero\n"),
            ("guest_0001.c", b"one\n"),
            ("guest_table.c", b"table\n"),
            ("guest_funcs.h", b"header\n"),
        ):
            (generated / name).write_bytes(payload)
        outputs = [
            {
                "path": item.name,
                "size": item.stat().st_size,
                "sha256": _sha256_file(item),
            }
            for item in sorted(generated.iterdir(), key=lambda item: item.name)
        ]
        recipe = {
            "inputs": {"input/isaac-ng.exe.unpacked.exe": _sha256_file(pe_path)},
            "params": {"image_base": GUEST_BASE},
            "stage": "generate",
            "tools": {"python": "fixture"},
        }
        manifest = {
            "schema": 1,
            "stage": "generate",
            "recipe_id": hashlib.sha256(_canonical_bytes(recipe)).hexdigest(),
            "recipe": recipe,
            "outputs": outputs,
            "output_set_id": hashlib.sha256(_canonical_bytes(outputs)).hexdigest(),
        }
        (generated / "manifest.json").write_bytes(_canonical_bytes(manifest) + b"\n")
        verified = verify_generation_manifest(pe_path, generated)
        assert verified["output_set_id"] == manifest["output_set_id"]
        _validate_output_directories(
            pe_path,
            root / "sdk",
            generated,
            root / "analysis",
            root / "build",
        )
        unrelated = generated / "unrelated.c"
        unrelated.write_text("not generated\n", encoding="utf-8")
        try:
            _validate_output_directories(
                pe_path,
                root / "sdk",
                generated,
                root / "analysis",
                root / "build",
            )
        except BuildError as exc:
            assert "would delete" in str(exc)
        else:
            raise AssertionError("unsafe generated directory was accepted")
        unrelated.unlink()

        command = cmake_configure_command(
            "cmake",
            root / "sdk",
            root / "sdk/share/vita.toolchain.cmake",
            pe_path,
            generated,
            root / "build",
            heap_mb=81,
            kage=True,
            loading_specialist=False,
            audio=False,
            openal_pool=True,
            io_profile=True,
            archive_file_cache=True,
            archive_validation_skip=True,
            archive_validation_receipt=True,
            fios_cache=True,
            crt_seek_shadow=True,
            continue_profile=True,
            png_decode_profile=True,
            png_native_unfilter=True,
            texture_churn_profile=False,
            game_log_batch=True,
            continue_overlay=True,
            texel_oom_diagnostic=True,
            texel_scratch=True,
            texture_align8_policy=False,
            anm2_scratch=True,
            fxlayers_null_rollback=False,
            fxray_alpha_mask=True,
            heap_ledger_memblock=True,
            heap_ledger_backshift=True,
            heap_overflow_mspace=True,
            room_entry_slab=True,
            room_entry_hybrid=True,
            source_date_epoch=1_700_000_000,
            release=True,
            exit_menu_profile=True,
        )
        expected_flags = {
            "-DISAAC_GUEST_BASE=0x98000000",
            "-DISAAC_VITA_HEAP_MB=81",
            "-DISAAC_VITA_KAGE=ON",
            "-DISAAC_VITA_LOADING_SPECIALIST=OFF",
            "-DISAAC_VITA_AUDIO=OFF",
            "-DISAAC_VITA_OPENAL_POOL=ON",
            "-DISAAC_VITA_IO_PROFILE=ON",
            "-DISAAC_VITA_ARCHIVE_FILE_CACHE=ON",
            "-DISAAC_VITA_ARCHIVE_VALIDATION_SKIP=ON",
            "-DISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT=ON",
            "-DISAAC_VITA_FIOS_CACHE=ON",
            "-DISAAC_VITA_CRT_SEEK_SHADOW=ON",
            "-DISAAC_VITA_CONTINUE_PROFILE=ON",
            "-DISAAC_VITA_EXIT_MENU_PROFILE=ON",
            "-DISAAC_VITA_PNG_DECODE_PROFILE=ON",
            "-DISAAC_VITA_PNG_NATIVE_UNFILTER=ON",
            "-DISAAC_VITA_TEXTURE_CHURN_PROFILE=OFF",
            "-DISAAC_VITA_GAME_LOG_BATCH=ON",
            "-DISAAC_VITA_CONTINUE_OVERLAY=ON",
            "-DISAAC_VITA_ORIGINAL_EFFECTS_PROFILE=OFF",
            "-DISAAC_VITA_RENDERFRAME_FASTPATH=OFF",
            "-DISAAC_VITA_PILL_BLOOM_BYPASS=OFF",
            "-DISAAC_VITA_TRANSIENT_BLOOM_HALF_RES=OFF",
            "-DISAAC_VITA_LASER_PROFILE=OFF",
            "-DISAAC_VITA_LASER_RING_SHADOW_SKIP=OFF",
            "-DISAAC_VITA_OGG_QUEUE_EMERGENCY=OFF",
            "-DISAAC_VITA_VITAGL_P8_SAFE_UPLOAD=OFF",
            "-DISAAC_VITA_LASER_ATLAS_P8=OFF",
            "-DISAAC_VITA_LASER_LIGHT_HALO_CLIP=OFF",
            "-DISAAC_VITA_POOP_FX_PROFILE=OFF",
            "-DISAAC_VITA_POOP_FX_SINGLE_CLOUD=OFF",
            "-DISAAC_VITA_STALL_PROBE=OFF",
            "-DISAAC_VITA_STAGE_HEARTBEAT=OFF",
            "-DISAAC_VITA_PHASE_PROFILE=OFF",
            "-DISAAC_VITA_WORLD_SEAM_DIAG=OFF",
            "-DISAAC_VITA_FULLSPEED_SCHEDULER=OFF",
            "-DISAAC_VITA_STABLE_30_PRESENTATION=OFF",
            "-DISAAC_VITA_GUEST_LOOKUP_CACHE=OFF",
            "-DISAAC_VITA_SYNC_IMPORT_FASTPATH=ON",
            "-DISAAC_VITA_TEXEL_OOM_DIAGNOSTIC=ON",
            "-DISAAC_VITA_STAGE_MEMORY_DIAGNOSTICS=OFF",
            "-DISAAC_VITA_TEXEL_SCRATCH=ON",
            "-DISAAC_VITA_TEXTURE_ALIGN8_POLICY=OFF",
            "-DISAAC_VITA_ANM2_SCRATCH=ON",
            "-DISAAC_VITA_FXLAYERS_NULL_ROLLBACK=OFF",
            "-DISAAC_VITA_FXRAY_ALPHA_MASK=ON",
            "-DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=ON",
            "-DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=ON",
            "-DISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON",
            "-DISAAC_VITA_HEAP_TERMINAL_FASTPATH=OFF",
            "-DISAAC_VITA_ROOM_ENTRY_SLAB=ON",
            "-DISAAC_VITA_ROOM_ENTRY_HYBRID=ON",
            "-DISAAC_VITA_SCAFFOLD_ONLY=OFF",
            "-DISAAC_VITA_GUEST_STACK_GUARD=ON",
            "-DISAAC_VITA_ANM2_MISSING_LAYER_GUARD=ON",
            "-DISAAC_VITA_SAVE_CHECKSUM_FASTPATH=ON",
            "-DISAAC_VITA_SAVE_READER_FASTPATH=ON",
            "-DISAAC_VITA_SAVE_READ32_FUSED=ON",
            "-DISAAC_VITA_SAVE_READER_DIRECT_EDGES=ON",
            "-DISAAC_VITA_MEMSET_THUNK_FASTPATH=ON",
            "-DISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH=ON",
            "-DISAAC_VITA_PNG_CRC32_FASTPATH=ON",
            "-DISAAC_VITA_ARCHIVE_MINIZ_FASTPATH=ON",
            "-DISAAC_VITA_LUA=OFF",
            "-DISAAC_LUA53_SOURCE_DIR=",
            "-DSOURCE_DATE_EPOCH=1700000000",
        }
        assert expected_flags.issubset(set(command))
        assert command.count("-DISAAC_VITA_GUEST_STACK_GUARD=ON") == 1
        assert "-DISAAC_VITA_GUEST_STACK_GUARD=OFF" not in command
        assert command.count(
            "-DISAAC_VITA_ANM2_MISSING_LAYER_GUARD=ON"
        ) == 1
        assert "-DISAAC_VITA_ANM2_MISSING_LAYER_GUARD=OFF" not in command
        assert command[1:3] == ["-G", "Ninja"]
        texture_command = cmake_configure_command(
            "cmake",
            root / "sdk",
            root / "sdk/share/vita.toolchain.cmake",
            pe_path,
            generated,
            root / "build-texture-profile",
            heap_mb=81,
            kage=True,
            loading_specialist=False,
            audio=False,
            openal_pool=True,
            io_profile=False,
            archive_file_cache=False,
            archive_validation_skip=False,
            archive_validation_receipt=False,
            fios_cache=False,
            crt_seek_shadow=False,
            continue_profile=False,
            png_decode_profile=False,
            png_native_unfilter=False,
            texture_churn_profile=True,
            game_log_batch=False,
            continue_overlay=False,
            texel_oom_diagnostic=False,
            texel_scratch=False,
            texture_align8_policy=False,
            anm2_scratch=False,
            fxlayers_null_rollback=False,
            fxray_alpha_mask=False,
            heap_ledger_memblock=False,
            heap_ledger_backshift=False,
            heap_overflow_mspace=False,
            room_entry_slab=False,
            room_entry_hybrid=False,
            source_date_epoch=1_700_000_000,
            release=True,
        )
        texture_flags = {
            "-DISAAC_VITA_TEXTURE_CHURN_PROFILE=ON",
            "-DISAAC_VITA_IO_PROFILE=OFF",
            "-DISAAC_VITA_DIRECT_DEFAULT=ON",
            "-DISAAC_VITA_FIRST_FRAME_PROBE=OFF",
            "-DISAAC_VITA_RAW_GXM_PROBE=OFF",
            "-DISAAC_VITA_KNOWN_COLOR_PROBE=OFF",
            "-DISAAC_VITA_RASTER_PROBE=OFF",
            "-DISAAC_VITA_SCREENSHOT_PROBE=OFF",
            "-DISAAC_VITA_PHASE_PROFILE=ON",
            "-DISAAC_VITA_VITAGL_STOCK_REFERENCE=ON",
        }
        assert texture_flags.issubset(set(texture_command))
        for selector in (
            "ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS",
            "ISAAC_VITA_VITAGL_SHADER_CACHE",
            "ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY",
            "ISAAC_VITA_GXM_STATE_SHADOW",
            "ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS",
        ):
            assert not any(
                item.startswith(f"-D{selector}=") for item in texture_command
            ), selector
        rollback_command = cmake_configure_command(
            "cmake",
            root / "sdk",
            root / "sdk/share/vita.toolchain.cmake",
            pe_path,
            generated,
            root / "build-rollback",
            heap_mb=81,
            kage=True,
            loading_specialist=False,
            audio=False,
            openal_pool=True,
            io_profile=True,
            archive_file_cache=True,
            archive_validation_skip=True,
            archive_validation_receipt=True,
            fios_cache=False,
            crt_seek_shadow=False,
            continue_profile=False,
            png_decode_profile=False,
            png_native_unfilter=False,
            texture_churn_profile=False,
            game_log_batch=False,
            continue_overlay=False,
            texel_oom_diagnostic=True,
            texel_scratch=True,
            texture_align8_policy=False,
            anm2_scratch=True,
            fxlayers_null_rollback=False,
            fxray_alpha_mask=False,
            heap_ledger_memblock=True,
            heap_ledger_backshift=True,
            heap_overflow_mspace=True,
            room_entry_slab=True,
            room_entry_hybrid=False,
            source_date_epoch=1_700_000_000,
            release=True,
            heap_terminal_fastpath=True,
        )
        assert rollback_command.count(
            "-DISAAC_VITA_HEAP_TERMINAL_FASTPATH=ON"
        ) == 1
        assert "-DISAAC_VITA_HEAP_TERMINAL_FASTPATH=OFF" not in rollback_command
        assert "-DISAAC_VITA_ROOM_ENTRY_HYBRID=OFF" in rollback_command
        assert "-DISAAC_VITA_ROOM_ENTRY_HYBRID=ON" not in rollback_command
        assert "-DISAAC_VITA_FIOS_CACHE=OFF" in rollback_command
        assert "-DISAAC_VITA_CRT_SEEK_SHADOW=OFF" in rollback_command
        assert "-DISAAC_VITA_CONTINUE_PROFILE=OFF" in rollback_command
        assert "-DISAAC_VITA_EXIT_MENU_PROFILE=OFF" in rollback_command
        assert "-DISAAC_VITA_PNG_DECODE_PROFILE=OFF" in rollback_command
        assert "-DISAAC_VITA_PNG_NATIVE_UNFILTER=OFF" in rollback_command
        assert "-DISAAC_VITA_GAME_LOG_BATCH=OFF" in rollback_command
        assert "-DISAAC_VITA_CONTINUE_OVERLAY=OFF" in rollback_command

        elf = root / "isaac_first_arm_fault"
        velf = root / "isaac_first_arm_fault.velf"
        eboot = root / "eboot.bin"
        manager = root / VPK_MANAGER_MEMBER
        vpk = root / "the-binding-of-isaac-repentance.vpk"
        elf.write_bytes(b"\x7fELFfixture")
        velf.write_bytes(b"\x7fELFvelf-fixture")
        eboot.write_bytes(b"SCE\x00fixture")
        manager.write_bytes(b"SCE\x00manager-fixture")
        try:
            locate_linker_map(root)
        except BuildError as exc:
            assert "is not a regular file" in str(exc)
        else:
            raise AssertionError("missing linker map was accepted")
        sdk = root / "sdk"
        source_prefix = os.fsencode(root.resolve().as_posix())
        generated_prefix = os.fsencode(generated.resolve().as_posix())
        sdk_prefix = os.fsencode(sdk.resolve().as_posix())
        generation_id = manifest["recipe_id"].encode("ascii")
        linker_map_payload = b"".join((
            b"Linker script and memory map\r\n",
            b"LOAD ", source_prefix, b"/recomp/runtime/guest.c.obj\r\n",
            b"LOAD ", generated_prefix, b"/guest_0000.c.obj\r\n",
            b"LOAD ", sdk_prefix, b"/lib/libc.a\r\n",
            b" .rodata.guest_generation_", generation_id, b"\r\n",
            b"                0x0000000081000000 guest_generation_",
            generation_id, b"\r\n",
            b"                0x000000008297a6d0                _end = .\r\n",
        ))
        linker_map_path = root / LINKER_MAP_NAME
        linker_map_path.write_bytes(linker_map_payload)
        linker_map = locate_linker_map(root)
        assert _linker_map_record(
            linker_map,
            source_root=root,
            generated_root=generated,
            vitasdk_root=sdk,
            generation_recipe_id=manifest["recipe_id"],
        ) == {
            "path": LINKER_MAP_NAME,
            "raw": {
                "size": len(linker_map_payload),
                "sha256": hashlib.sha256(linker_map_payload).hexdigest(),
            },
            "canonical": linker_map_contract.canonicalize(
                linker_map_payload,
                source_root=root,
                generated_root=generated,
                vitasdk_root=sdk,
                generation_recipe_id=manifest["recipe_id"],
            ).record,
        }

        def write_fixture_livearea(
            package: zipfile.ZipFile,
            *,
            mutate: str | None = None,
        ) -> None:
            for member, unused_size, unused_sha256 in VPK_LIVEAREA_ASSETS:
                source = VITA_SOURCE_DIR / "assets" / member
                payload = source.read_bytes()
                if member == mutate:
                    payload += b"mutated"
                package.writestr(member, payload)
            package.writestr(VPK_MANAGER_MEMBER, manager.read_bytes())
            package.writestr(
                VPK_MANAGER_LICENSE_MEMBER,
                VPK_MANAGER_LICENSE_SOURCE.read_bytes(),
            )

        with zipfile.ZipFile(vpk, "w", compression=zipfile.ZIP_STORED) as package:
            package.writestr("sce_sys/param.sfo", b"fixture sfo")
            package.writestr("eboot.bin", eboot.read_bytes())
            package.writestr("isaac-ng.exe.unpacked.exe", pe_path.read_bytes())
            write_fixture_livearea(package)
        artifacts = locate_artifacts(root)
        assert artifacts == {
            "ELF": elf,
            "VELF": velf,
            "eboot": eboot,
            "VPK": vpk,
        }
        verify_package(pe_path, artifacts)

        vpk.unlink()
        with zipfile.ZipFile(vpk, "w", compression=zipfile.ZIP_STORED) as package:
            package.writestr("sce_sys/param.sfo", b"fixture sfo")
            package.writestr("eboot.bin", eboot.read_bytes())
            package.writestr("isaac-ng.exe.unpacked.exe", pe_path.read_bytes())
            write_fixture_livearea(
                package, mutate="sce_sys/livearea/contents/startup.png"
            )
        try:
            verify_package(pe_path, artifacts)
        except BuildError as exc:
            assert "VPK LiveArea payload differs" in str(exc)
            assert "startup.png" in str(exc)
        else:
            raise AssertionError("mutated LiveArea package payload was accepted")

        vpk.unlink()
        with zipfile.ZipFile(vpk, "w", compression=zipfile.ZIP_STORED) as package:
            package.writestr("sce_sys/param.sfo", b"fixture sfo")
            package.writestr("eboot.bin", eboot.read_bytes())
            package.writestr("isaac-ng.exe.unpacked.exe", pe_path.read_bytes())
            write_fixture_livearea(package)

        gate_readelf = root / "raw-gate-readelf"
        gate_readelf.write_bytes(b"fixture readelf\n")
        gate_compile_commands = root / "compile_commands.json"
        gate_compile_commands.write_text("[]\n", encoding="ascii")
        gate_build_ninja = root / "build.ninja"
        gate_build_ninja.write_text("# fixture ninja\n", encoding="ascii")
        gate_cache = root / "CMakeCache.txt"
        gate_cache.write_text(
            (
                f"ISAAC_GENERATED_DIR:PATH={generated.as_posix()}\n"
                f"CMAKE_READELF:FILEPATH="
                f"{(root / 'missing-readelf').as_posix()}\n"
                f"ISAAC_VITA_READELF:FILEPATH={gate_readelf.as_posix()}\n"
                "ISAAC_VITA_DIRECT_DEFAULT:BOOL=OFF\n"
                "ISAAC_VITA_LUA:BOOL=OFF\n"
                "ISAAC_LUA53_SOURCE_DIR:PATH=\n"
                + "".join(
                    f"{flag}:BOOL=ON\n"
                    for flag in RAW_ALLOCATOR_GATE_CANONICAL_CACHE_FLAGS
                )
            ),
            encoding="utf-8",
        )
        gate_input_paths = {
            "generated_manifest": generated / "manifest.json",
            "cmake_cache": gate_cache,
            "compile_commands": gate_compile_commands,
            "build_ninja": gate_build_ninja,
            "poison_header": (
                VITA_SOURCE_DIR / "isaac_vita_raw_allocator_poison.h"
            ),
            "gate": VITA_SOURCE_DIR / "vita_raw_allocator_gate.py",
            "readelf": gate_readelf,
        }
        gate_inputs = {
            name: {"size": path.stat().st_size, "sha256": _sha256_file(path)}
            for name, path in gate_input_paths.items()
        }
        gate_source_names = (RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_LOGICAL_SOURCE,
                             *RAW_ALLOCATOR_GATE_EXEMPT_SOURCES,
                             "recomp/runtime/manual_portable.c")

        def fixture_relocations(source_name: str) -> list[dict[str, object]]:
            if source_name == "recomp/runtime/host_vita_heap.c":
                return [
                    {
                        "function": function,
                        "symbol": symbol,
                        "type": kind,
                        "count": count,
                    }
                    for (function, symbol, kind), count in sorted(
                        RAW_ALLOCATOR_GATE_HEAP_CANONICAL.items()
                    )
                ]
            if source_name in RAW_ALLOCATOR_GATE_EXEMPT_SOURCES:
                return [{
                    "function": "owner", "symbol": "malloc",
                    "type": "R_ARM_THM_CALL", "count": 1,
                }]
            return []

        gate_closure = {
            "sources": [
                {
                    "source": source_name,
                    "object": (
                        f"{RAW_ALLOCATOR_GATE_LOGICAL_OBJECT_PREFIX}"
                        f"{source_name}.obj"
                    ),
                    "exempt": source_name in RAW_ALLOCATOR_GATE_EXEMPT_SOURCES,
                    "raw_relocations": fixture_relocations(source_name),
                }
                for source_name in gate_source_names
            ],
            "linked_objects": [
                f"{RAW_ALLOCATOR_GATE_LOGICAL_OBJECT_PREFIX}{source_name}.obj"
                for source_name in gate_source_names
            ],
        }
        gate_receipt = {
            "schema": 1,
            "stage": "vita-raw-allocator-gate",
            "result": "PASS",
            "target_name": "isaac_first_arm_fault",
            "profile": RAW_ALLOCATOR_GATE_PROFILE_CANONICAL,
            "raw_symbols": list(RAW_ALLOCATOR_GATE_RAW_SYMBOLS),
            "forbidden_source_feature_view_macros": list(
                RAW_ALLOCATOR_GATE_FORBIDDEN_SOURCE_FEATURE_VIEW_MACROS
            ),
            "exempt_sources": list(RAW_ALLOCATOR_GATE_EXEMPT_SOURCES),
            "source_count": len(gate_source_names),
            "object_count": len(gate_source_names),
            "closure_sha256": hashlib.sha256(
                _canonical_bytes(gate_closure)).hexdigest(),
            "target": {
                "size": elf.stat().st_size,
                "sha256": _sha256_file(elf),
            },
            "inputs": gate_inputs,
            "closure": gate_closure,
        }
        gate_receipt_path = root / RAW_ALLOCATOR_GATE_RECEIPT_NAME
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        assert verify_raw_allocator_gate(root, elf) == gate_receipt
        assert verify_raw_allocator_gate(
            root, elf, require_canonical=True
        ) == gate_receipt

        gate_override = root / RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_OUTPUT
        gate_override.parent.mkdir(parents=True)
        gate_override.write_bytes(b"direct-default fixture\n")
        gate_override_record = {
            "logical_source": RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_LOGICAL_SOURCE,
            "size": gate_override.stat().st_size,
            "sha256": _sha256_file(gate_override),
        }

        # The override is an eighth input exactly when the configured build
        # selects it.  Keep the receipt tied to both its physical bytes and the
        # one logical generated source it replaces.
        gate_receipt["inputs"][RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_INPUT] = (
            dict(gate_override_record)
        )
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "input closure changed" in str(exc)
        else:
            raise AssertionError("OFF direct-default accepted an override input")
        del gate_receipt["inputs"][RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_INPUT]

        direct_off_cache_payload = gate_cache.read_bytes()
        direct_on_cache_payload = direct_off_cache_payload.replace(
            b"ISAAC_VITA_DIRECT_DEFAULT:BOOL=OFF",
            b"ISAAC_VITA_DIRECT_DEFAULT:BOOL=ON",
        )
        assert direct_on_cache_payload != direct_off_cache_payload
        gate_cache.write_bytes(direct_on_cache_payload)
        gate_receipt["inputs"]["cmake_cache"] = {
            "size": gate_cache.stat().st_size,
            "sha256": _sha256_file(gate_cache),
        }
        gate_receipt["inputs"][RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_INPUT] = (
            dict(gate_override_record)
        )
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        assert verify_raw_allocator_gate(root, elf) == gate_receipt

        saved_override_record = gate_receipt["inputs"].pop(
            RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_INPUT
        )
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "input closure changed" in str(exc)
        else:
            raise AssertionError("ON direct-default accepted a missing override input")
        gate_receipt["inputs"][RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_INPUT] = (
            saved_override_record
        )

        for field, hostile, expected in (
            ("logical_source", "generated/guest_0000.c", "is malformed"),
            ("size", gate_override_record["size"] + 1, "input is stale"),
            ("sha256", "0" * 64, "input is stale"),
        ):
            original = saved_override_record[field]
            saved_override_record[field] = hostile
            gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
            try:
                verify_raw_allocator_gate(root, elf)
            except BuildError as exc:
                assert expected in str(exc)
            else:
                raise AssertionError(
                    f"direct-default accepted hostile override {field}"
                )
            saved_override_record[field] = original

        saved_override_record["unexpected"] = "must fail closed"
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "input record is malformed" in str(exc)
        else:
            raise AssertionError("direct-default accepted an extra override field")
        del saved_override_record["unexpected"]

        generated_source_row = gate_closure["sources"].pop(0)
        generated_linked_object = gate_closure["linked_objects"].pop(0)
        gate_receipt["source_count"] = len(gate_closure["sources"])
        gate_receipt["object_count"] = len(gate_closure["linked_objects"])
        gate_receipt["closure_sha256"] = hashlib.sha256(
            _canonical_bytes(gate_closure)
        ).hexdigest()
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "override is absent from closure" in str(exc)
        else:
            raise AssertionError("direct-default accepted an absent logical owner")
        gate_closure["sources"].insert(0, generated_source_row)
        gate_closure["linked_objects"].insert(0, generated_linked_object)

        gate_closure["sources"].insert(1, dict(generated_source_row))
        gate_closure["linked_objects"].insert(1, generated_linked_object)
        gate_receipt["source_count"] = len(gate_closure["sources"])
        gate_receipt["object_count"] = len(gate_closure["linked_objects"])
        gate_receipt["closure_sha256"] = hashlib.sha256(
            _canonical_bytes(gate_closure)
        ).hexdigest()
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "source/object closure is duplicated" in str(exc)
        else:
            raise AssertionError("direct-default accepted a duplicate logical owner")
        gate_closure["sources"].pop(1)
        gate_closure["linked_objects"].pop(1)
        gate_receipt["source_count"] = len(gate_closure["sources"])
        gate_receipt["object_count"] = len(gate_closure["linked_objects"])
        gate_receipt["closure_sha256"] = hashlib.sha256(
            _canonical_bytes(gate_closure)
        ).hexdigest()
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        assert verify_raw_allocator_gate(root, elf) == gate_receipt

        del gate_receipt["inputs"][RAW_ALLOCATOR_GATE_DIRECT_DEFAULT_INPUT]
        gate_cache.write_bytes(direct_off_cache_payload)
        gate_receipt["inputs"]["cmake_cache"] = {
            "size": gate_cache.stat().st_size,
            "sha256": _sha256_file(gate_cache),
        }
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        assert verify_raw_allocator_gate(root, elf) == gate_receipt

        gate_receipt["target"]["unexpected"] = "must fail closed"
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "belongs to another ELF" in str(exc)
        else:
            raise AssertionError("unknown raw allocator target field was accepted")
        del gate_receipt["target"]["unexpected"]

        first_source_row = gate_closure["sources"][0]
        first_logical_object = first_source_row["object"]
        first_source_row["object"] = "objects/wrong-logical-owner.c.obj"
        gate_receipt["closure_sha256"] = hashlib.sha256(
            _canonical_bytes(gate_closure)
        ).hexdigest()
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "logical object identity changed" in str(exc)
        else:
            raise AssertionError("non-derived logical object ID was accepted")
        first_source_row["object"] = first_logical_object

        gate_closure["sources"][0:2] = reversed(
            gate_closure["sources"][0:2]
        )
        gate_receipt["closure_sha256"] = hashlib.sha256(
            _canonical_bytes(gate_closure)
        ).hexdigest()
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "source closure is not canonical" in str(exc)
        else:
            raise AssertionError("non-canonical raw allocator source order passed")
        gate_closure["sources"][0:2] = reversed(
            gate_closure["sources"][0:2]
        )
        gate_receipt["closure_sha256"] = hashlib.sha256(
            _canonical_bytes(gate_closure)
        ).hexdigest()
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        assert verify_raw_allocator_gate(root, elf) == gate_receipt

        subset_receipt = json.loads(_canonical_bytes(gate_receipt))
        subset_exempt = list(RAW_ALLOCATOR_GATE_EXEMPT_SOURCES[:2])
        absent_optional = set(RAW_ALLOCATOR_GATE_EXEMPT_SOURCES[2:])
        subset_sources = [
            row for row in subset_receipt["closure"]["sources"]
            if row["source"] not in absent_optional
        ]
        subset_objects = {row["object"] for row in subset_sources}
        subset_receipt["closure"]["sources"] = subset_sources
        subset_receipt["closure"]["linked_objects"] = [
            obj for obj in subset_receipt["closure"]["linked_objects"]
            if obj in subset_objects
        ]
        subset_receipt["exempt_sources"] = subset_exempt
        subset_receipt["source_count"] = len(subset_sources)
        subset_receipt["object_count"] = len(subset_objects)
        subset_receipt["closure_sha256"] = hashlib.sha256(
            _canonical_bytes(subset_receipt["closure"])
        ).hexdigest()
        gate_receipt_path.write_bytes(_canonical_bytes(subset_receipt) + b"\n")
        assert verify_raw_allocator_gate(root, elf) == subset_receipt

        subset_receipt["exempt_sources"] = subset_exempt[:-1]
        gate_receipt_path.write_bytes(_canonical_bytes(subset_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "exemption contract changed" in str(exc)
        else:
            raise AssertionError("missing present allocator exemption was accepted")
        subset_receipt["exempt_sources"] = [
            *subset_exempt, RAW_ALLOCATOR_GATE_EXEMPT_SOURCES[2]
        ]
        gate_receipt_path.write_bytes(_canonical_bytes(subset_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "exemption contract changed" in str(exc)
        else:
            raise AssertionError("extra absent allocator exemption was accepted")
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")

        canonical_cache_payload = gate_cache.read_bytes()
        diagnostic_cache_payload = canonical_cache_payload.replace(
            b"ISAAC_VITA_ANM2_SCRATCH:BOOL=ON",
            b"ISAAC_VITA_ANM2_SCRATCH:BOOL=OFF",
        )
        assert diagnostic_cache_payload != canonical_cache_payload
        heap_row = next(
            row for row in gate_closure["sources"]
            if row["source"] == "recomp/runtime/host_vita_heap.c"
        )
        canonical_heap_relocations = json.loads(
            _canonical_bytes(heap_row["raw_relocations"])
        )
        diagnostic_heap_relocations = [{
            "function": "vita_heap_ledger_reserve.constprop.7",
            "symbol": "free", "type": "R_ARM_THM_CALL", "count": 1,
        }]
        gate_cache.write_bytes(diagnostic_cache_payload)
        gate_receipt["inputs"]["cmake_cache"] = {
            "size": gate_cache.stat().st_size,
            "sha256": _sha256_file(gate_cache),
        }
        gate_receipt["profile"] = RAW_ALLOCATOR_GATE_PROFILE_DIAGNOSTIC
        heap_row["raw_relocations"] = diagnostic_heap_relocations
        gate_receipt["closure_sha256"] = hashlib.sha256(
            _canonical_bytes(gate_closure)
        ).hexdigest()
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        assert verify_raw_allocator_gate(root, elf) == gate_receipt
        try:
            verify_raw_allocator_gate(root, elf, require_canonical=True)
        except BuildError as exc:
            assert "--release requires the canonical-exact" in str(exc)
        else:
            raise AssertionError("release accepted a diagnostic allocator profile")

        hostile_heap_relocations = {
            "new function": [{
                "function": "new_owner", "symbol": "malloc",
                "type": "R_ARM_THM_CALL", "count": 1,
            }],
            "wrong symbol": [{
                "function": "vita_heap_guest_malloc_with_reason",
                "symbol": "strdup", "type": "R_ARM_THM_CALL", "count": 1,
            }],
            "wrong type": [{
                "function": "vita_heap_guest_malloc_with_reason",
                "symbol": "malloc", "type": "R_ARM_ABS32", "count": 1,
            }],
            "count overflow": [{
                "function": "vita_heap_guest_malloc_with_reason",
                "symbol": "free", "type": "R_ARM_THM_CALL", "count": 3,
            }],
        }
        for label, hostile in hostile_heap_relocations.items():
            heap_row["raw_relocations"] = hostile
            gate_receipt["closure_sha256"] = hashlib.sha256(
                _canonical_bytes(gate_closure)
            ).hexdigest()
            gate_receipt_path.write_bytes(
                _canonical_bytes(gate_receipt) + b"\n"
            )
            try:
                verify_raw_allocator_gate(root, elf)
            except BuildError as exc:
                assert "diagnostic raw allocator heap allowlist changed" in str(exc)
            else:
                raise AssertionError(
                    f"diagnostic allocator receipt accepted {label}"
                )
        heap_row["raw_relocations"] = diagnostic_heap_relocations
        gate_receipt["profile"] = RAW_ALLOCATOR_GATE_PROFILE_CANONICAL
        gate_receipt["closure_sha256"] = hashlib.sha256(
            _canonical_bytes(gate_closure)
        ).hexdigest()
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "profile disagrees with CMake features" in str(exc)
        else:
            raise AssertionError("mutated allocator profile was accepted")

        gate_cache.write_bytes(canonical_cache_payload)
        gate_receipt["inputs"]["cmake_cache"] = {
            "size": gate_cache.stat().st_size,
            "sha256": _sha256_file(gate_cache),
        }
        gate_receipt["profile"] = RAW_ALLOCATOR_GATE_PROFILE_CANONICAL
        heap_row["raw_relocations"] = canonical_heap_relocations
        gate_receipt["closure_sha256"] = hashlib.sha256(
            _canonical_bytes(gate_closure)
        ).hexdigest()
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        assert verify_raw_allocator_gate(root, elf) == gate_receipt

        compile_payload = gate_compile_commands.read_bytes()
        compile_stat = gate_compile_commands.stat()
        gate_compile_commands.write_bytes(compile_payload + b" ")
        os.utime(
            gate_compile_commands,
            ns=(compile_stat.st_atime_ns, compile_stat.st_mtime_ns),
        )
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "input is stale: compile_commands" in str(exc)
        else:
            raise AssertionError("same-mtime stale compile_commands was accepted")
        gate_compile_commands.write_bytes(compile_payload)

        readelf_payload = gate_readelf.read_bytes()
        readelf_stat = gate_readelf.stat()
        gate_readelf.write_bytes(readelf_payload + b" ")
        os.utime(gate_readelf, ns=(readelf_stat.st_atime_ns, readelf_stat.st_mtime_ns))
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "input is stale: readelf" in str(exc)
        else:
            raise AssertionError("same-mtime stale readelf was accepted")
        gate_readelf.write_bytes(readelf_payload)

        for input_name in ("poison_header", "gate"):
            input_sha256 = gate_receipt["inputs"][input_name]["sha256"]
            gate_receipt["inputs"][input_name]["sha256"] = "0" * 64
            gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
            try:
                verify_raw_allocator_gate(root, elf)
            except BuildError as exc:
                assert f"input is stale: {input_name}" in str(exc)
            else:
                raise AssertionError(f"stale {input_name} identity was accepted")
            gate_receipt["inputs"][input_name]["sha256"] = input_sha256

        gate_receipt["raw_symbols"] = list(RAW_ALLOCATOR_GATE_RAW_SYMBOLS[:-1])
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "symbol contract changed" in str(exc)
        else:
            raise AssertionError("mutated raw allocator symbol contract was accepted")
        gate_receipt["raw_symbols"] = list(RAW_ALLOCATOR_GATE_RAW_SYMBOLS)

        gate_receipt["forbidden_source_feature_view_macros"] = list(
            RAW_ALLOCATOR_GATE_FORBIDDEN_SOURCE_FEATURE_VIEW_MACROS[:-1]
        )
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "feature-view macro contract changed" in str(exc)
        else:
            raise AssertionError(
                "mutated raw allocator feature-view contract was accepted"
            )
        gate_receipt["forbidden_source_feature_view_macros"] = list(
            RAW_ALLOCATOR_GATE_FORBIDDEN_SOURCE_FEATURE_VIEW_MACROS
        )

        gate_receipt["unmodelled_semantic_field"] = "must fail closed"
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "unexpected schema" in str(exc)
        else:
            raise AssertionError("unknown raw allocator receipt field was accepted")
        del gate_receipt["unmodelled_semantic_field"]

        readelf_record = gate_receipt["inputs"].pop("readelf")
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "input closure changed" in str(exc)
        else:
            raise AssertionError("raw allocator receipt without readelf was accepted")
        gate_receipt["inputs"]["readelf"] = readelf_record

        gate_receipt["exempt_sources"] = list(
            RAW_ALLOCATOR_GATE_EXEMPT_SOURCES[:-1]
        )
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "exemption contract changed" in str(exc)
        else:
            raise AssertionError("mutated allocator exemption contract was accepted")
        gate_receipt["exempt_sources"] = list(RAW_ALLOCATOR_GATE_EXEMPT_SOURCES)

        exempt_source_row = next(
            row for row in gate_closure["sources"] if row["exempt"]
        )
        exempt_source_row["exempt"] = False
        gate_receipt["closure_sha256"] = hashlib.sha256(
            _canonical_bytes(gate_closure)).hexdigest()
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "ordinary source" in str(exc)
        else:
            raise AssertionError("ordinary raw relocation in receipt was accepted")
        exempt_source_row["exempt"] = True

        last_linked = gate_closure["linked_objects"][-1]
        gate_closure["linked_objects"][-1] = gate_closure["linked_objects"][0]
        gate_receipt["closure_sha256"] = hashlib.sha256(
            _canonical_bytes(gate_closure)).hexdigest()
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "linked-object closure changed" in str(exc)
        else:
            raise AssertionError("duplicate allocator linked object was accepted")
        gate_closure["linked_objects"][-1] = last_linked

        gate_receipt["closure_sha256"] = "0" * 64
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")
        try:
            verify_raw_allocator_gate(root, elf)
        except BuildError as exc:
            assert "closure identity is stale" in str(exc)
        else:
            raise AssertionError("stale raw allocator gate receipt was accepted")
        gate_receipt["closure_sha256"] = hashlib.sha256(
            _canonical_bytes(gate_closure)).hexdigest()
        gate_receipt_path.write_bytes(_canonical_bytes(gate_receipt) + b"\n")

        with zipfile.ZipFile(vpk, "a", compression=zipfile.ZIP_STORED) as package:
            package.writestr("alsof.conf", b"misspelled fixture\n")
        try:
            verify_package(
                pe_path,
                artifacts,
                audio=True,
            )
        except BuildError as exc:
            assert "misspelled alsof.conf" in str(exc)
        else:
            raise AssertionError("misspelled OpenAL VPK member was accepted")

        vpk.unlink()
        with zipfile.ZipFile(vpk, "w", compression=zipfile.ZIP_STORED) as package:
            package.writestr("sce_sys/param.sfo", b"fixture sfo")
            package.writestr("eboot.bin", eboot.read_bytes())
            package.writestr("isaac-ng.exe.unpacked.exe", pe_path.read_bytes())
            write_fixture_livearea(package)
            package.writestr(
                "alsoft.conf",
                (REPO_ROOT / "recomp" / "vita" / "alsoft.conf").read_bytes(),
            )
        verify_package(
            pe_path,
            artifacts,
            audio=True,
        )

        (sdk / "share").mkdir(parents=True)
        (sdk / "share" / "vita.toolchain.cmake").write_text(
            "# fixture toolchain\n", encoding="ascii"
        )
        (sdk / "share" / "vita.cmake").write_text(
            "# fixture helpers\n", encoding="ascii"
        )
        fixture_vpk_packer = sdk / "bin" / (
            "vita-pack-vpk.exe" if os.name == "nt" else "vita-pack-vpk"
        )
        fixture_vpk_packer.parent.mkdir()
        fixture_vpk_packer.write_bytes(b"fixture vita-pack-vpk\n")
        build = root / "build"
        build.mkdir()
        vitagl_overlay = root / "vitagl-overlay"
        vitagl_library = vitagl_overlay / "prefix" / "lib" / "libvitaGL.a"
        vitagl_header = vitagl_overlay / "prefix" / "include" / "vitaGL.h"
        vitagl_library.parent.mkdir(parents=True)
        vitagl_header.parent.mkdir(parents=True)
        vitagl_library.write_bytes(b"!<arch>\nfixture-vitaGL")
        vitagl_header.write_text("/* fixture vitaGL header */\n", encoding="ascii")
        (vitagl_overlay / "recipe.txt").write_text("a" * 64 + "\n", encoding="ascii")
        (vitagl_overlay / "build-contract.txt").write_text(
            "flags=fixture softfp single-threaded-gc shared-rt-1\n",
            encoding="ascii",
        )
        (vitagl_overlay / "libvitaGL.sha256").write_text(
            f"{_sha256_file(vitagl_library)}  {vitagl_library}\n",
            encoding="ascii",
        )
        (vitagl_overlay / "vitaGL.h.sha256").write_text(
            f"{_sha256_file(vitagl_header)}  {vitagl_header}\n",
            encoding="ascii",
        )
        cache_text = "\n".join(
            (
                "CMAKE_BUILD_TYPE:STRING=Release",
                "CMAKE_GENERATOR:INTERNAL=Ninja",
                f"VITA_PACK_VPK:PATH={fixture_vpk_packer}",
                f"ISAAC_VITA_REAL_PACK_VPK:FILEPATH={fixture_vpk_packer}",
                f"ISAAC_PE_PATH:FILEPATH={pe_path}",
                f"ISAAC_GENERATED_DIR:PATH={generated}",
                "ISAAC_GUEST_BASE:STRING=0x98000000",
                "ISAAC_VITA_HEAP_MB:STRING=81",
                "ISAAC_VITA_SCAFFOLD_ONLY:BOOL=OFF",
                "ISAAC_VITA_GUEST_STACK_GUARD:BOOL=ON",
                "ISAAC_VITA_ANM2_MISSING_LAYER_GUARD:BOOL=ON",
                "ISAAC_VITA_SAVE_CHECKSUM_FASTPATH:BOOL=ON",
                "ISAAC_VITA_SAVE_READER_FASTPATH:BOOL=ON",
                "ISAAC_VITA_SAVE_READ32_FUSED:BOOL=ON",
                "ISAAC_VITA_SAVE_READER_DIRECT_EDGES:BOOL=ON",
                "ISAAC_VITA_MEMSET_THUNK_FASTPATH:BOOL=ON",
                "ISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH:BOOL=ON",
                "ISAAC_VITA_PNG_CRC32_FASTPATH:BOOL=ON",
                "ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH:BOOL=ON",
                "ISAAC_VITA_KAGE:BOOL=ON",
                "ISAAC_VITA_LUA:BOOL=OFF",
                "ISAAC_LUA53_SOURCE_DIR:PATH=",
                "ISAAC_VITA_LOADING_SPECIALIST:BOOL=OFF",
                "ISAAC_VITA_AUDIO:BOOL=OFF",
                "ISAAC_VITA_AUDIO_STREAM_RECEIPT:BOOL=OFF",
                "ISAAC_VITA_OPENAL_POOL:BOOL=ON",
                "ISAAC_VITA_IO_PROFILE:BOOL=ON",
                "ISAAC_VITA_ARCHIVE_FILE_CACHE:BOOL=ON",
                "ISAAC_VITA_ARCHIVE_VALIDATION_SKIP:BOOL=ON",
                "ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT:BOOL=ON",
                "ISAAC_VITA_FIOS_CACHE:BOOL=ON",
                "ISAAC_VITA_CRT_SEEK_SHADOW:BOOL=ON",
                "ISAAC_VITA_CONTINUE_PROFILE:BOOL=ON",
                "ISAAC_VITA_EXIT_MENU_PROFILE:BOOL=OFF",
                "ISAAC_VITA_PNG_DECODE_PROFILE:BOOL=ON",
                "ISAAC_VITA_PNG_NATIVE_UNFILTER:BOOL=ON",
                "ISAAC_VITA_TEXTURE_CHURN_PROFILE:BOOL=OFF",
                "ISAAC_VITA_GAME_LOG_BATCH:BOOL=ON",
                "ISAAC_VITA_CONTINUE_OVERLAY:BOOL=ON",
                "ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE:BOOL=OFF",
                "ISAAC_VITA_RENDERFRAME_FASTPATH:BOOL=OFF",
                "ISAAC_VITA_PILL_BLOOM_BYPASS:BOOL=OFF",
                "ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES:BOOL=OFF",
                "ISAAC_VITA_LASER_PROFILE:BOOL=OFF",
                "ISAAC_VITA_LASER_RING_SHADOW_SKIP:BOOL=OFF",
                "ISAAC_VITA_OGG_QUEUE_EMERGENCY:BOOL=OFF",
                "ISAAC_VITA_VITAGL_P8_SAFE_UPLOAD:BOOL=OFF",
                "ISAAC_VITA_LASER_ATLAS_P8:BOOL=OFF",
                "ISAAC_VITA_LASER_LIGHT_HALO_CLIP:BOOL=OFF",
                "ISAAC_VITA_POOP_FX_PROFILE:BOOL=OFF",
                "ISAAC_VITA_POOP_FX_SINGLE_CLOUD:BOOL=OFF",
                "ISAAC_VITA_STALL_PROBE:BOOL=OFF",
                "ISAAC_VITA_STAGE_HEARTBEAT:BOOL=OFF",
                "ISAAC_VITA_DIRECT_DEFAULT:BOOL=OFF",
                "ISAAC_VITA_FIRST_FRAME_PROBE:BOOL=OFF",
                "ISAAC_VITA_RAW_GXM_PROBE:BOOL=OFF",
                "ISAAC_VITA_KNOWN_COLOR_PROBE:BOOL=OFF",
                "ISAAC_VITA_RASTER_PROBE:BOOL=OFF",
                "ISAAC_VITA_SCREENSHOT_PROBE:BOOL=OFF",
                "ISAAC_VITA_PHASE_PROFILE:BOOL=OFF",
                "ISAAC_VITA_SIM_CADENCE_RECEIPT:BOOL=OFF",
                "ISAAC_VITA_WORLD_SEAM_DIAG:BOOL=OFF",
                "ISAAC_VITA_VITAGL_STOCK_REFERENCE:BOOL=OFF",
                "ISAAC_VITA_FULLSPEED_SCHEDULER:BOOL=OFF",
                "ISAAC_VITA_STABLE_30_PRESENTATION:BOOL=OFF",
                "ISAAC_VITA_GUEST_LOOKUP_CACHE:BOOL=OFF",
                "ISAAC_VITA_SYNC_IMPORT_FASTPATH:BOOL=ON",
                "ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC:BOOL=ON",
                "ISAAC_VITA_STAGE_MEMORY_DIAGNOSTICS:BOOL=OFF",
                "ISAAC_VITA_TEXEL_SCRATCH:BOOL=ON",
                "ISAAC_VITA_TEXTURE_ALIGN8_POLICY:BOOL=OFF",
                "ISAAC_VITA_ANM2_SCRATCH:BOOL=ON",
                "ISAAC_VITA_FXLAYERS_NULL_ROLLBACK:BOOL=OFF",
                "ISAAC_VITA_FXRAY_ALPHA_MASK:BOOL=ON",
                "ISAAC_VITA_HEAP_LEDGER_MEMBLOCK:BOOL=ON",
                "ISAAC_VITA_HEAP_LEDGER_BACKSHIFT:BOOL=ON",
                "ISAAC_VITA_HEAP_OVERFLOW_MSPACE:BOOL=ON",
                "ISAAC_VITA_HEAP_TERMINAL_FASTPATH:BOOL=OFF",
                "ISAAC_VITA_ROOM_ENTRY_SLAB:BOOL=ON",
                "ISAAC_VITA_ROOM_ENTRY_HYBRID:BOOL=ON",
                "SOURCE_DATE_EPOCH:STRING=1700000000",
                f"ISAAC_VITA_VITAGL_OVERLAY_DIR:PATH={vitagl_overlay}",
                "",
            )
        )
        (build / "CMakeCache.txt").write_text(cache_text, encoding="utf-8")
        cache = _read_cmake_cache(build)
        configuration = validate_cmake_contract(
            cache,
            pe_path=pe_path,
            generated_dir=generated,
            heap_mb=81,
            kage=True,
            loading_specialist=False,
            audio=False,
            openal_pool=True,
            io_profile=True,
            archive_file_cache=True,
            archive_validation_skip=True,
            archive_validation_receipt=True,
            fios_cache=True,
            crt_seek_shadow=True,
            continue_profile=True,
            png_decode_profile=True,
            png_native_unfilter=True,
            texture_churn_profile=False,
            game_log_batch=True,
            continue_overlay=True,
            texel_oom_diagnostic=True,
            texel_scratch=True,
            texture_align8_policy=False,
            anm2_scratch=True,
            fxlayers_null_rollback=False,
            fxray_alpha_mask=True,
            heap_ledger_memblock=True,
            heap_ledger_backshift=True,
            heap_overflow_mspace=True,
            room_entry_slab=True,
            room_entry_hybrid=True,
            source_date_epoch=1_700_000_000,
            release=True,
        )
        assert configuration["features"][
            "ISAAC_VITA_GUEST_STACK_GUARD"
        ] is True
        assert configuration["features"][
            "ISAAC_VITA_ANM2_MISSING_LAYER_GUARD"
        ] is True
        texture_cache = dict(cache)
        texture_cache.update({
            "ISAAC_VITA_IO_PROFILE": "OFF",
            "ISAAC_VITA_TEXTURE_CHURN_PROFILE": "ON",
            "ISAAC_VITA_DIRECT_DEFAULT": "ON",
            "ISAAC_VITA_FIRST_FRAME_PROBE": "OFF",
            "ISAAC_VITA_RAW_GXM_PROBE": "OFF",
            "ISAAC_VITA_KNOWN_COLOR_PROBE": "OFF",
            "ISAAC_VITA_RASTER_PROBE": "OFF",
            "ISAAC_VITA_SCREENSHOT_PROBE": "OFF",
            "ISAAC_VITA_PHASE_PROFILE": "ON",
            "ISAAC_VITA_VITAGL_STOCK_REFERENCE": "ON",
        })
        texture_contract_arguments = {
            "pe_path": pe_path,
            "generated_dir": generated,
            "heap_mb": 81,
            "kage": True,
            "loading_specialist": False,
            "audio": False,
            "openal_pool": True,
            "io_profile": False,
            "archive_file_cache": True,
            "archive_validation_skip": True,
            "archive_validation_receipt": True,
            "fios_cache": True,
            "crt_seek_shadow": True,
            "continue_profile": True,
            "png_decode_profile": True,
            "png_native_unfilter": True,
            "texture_churn_profile": True,
            "game_log_batch": True,
            "continue_overlay": True,
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
            "release": True,
        }
        texture_configuration = validate_cmake_contract(
            texture_cache, **texture_contract_arguments
        )
        assert texture_configuration["features"][
            "ISAAC_VITA_TEXTURE_CHURN_PROFILE"
        ] is True
        assert texture_configuration["features"][
            "ISAAC_VITA_VITAGL_STOCK_REFERENCE"
        ] is True
        terminal_cache = dict(texture_cache)
        terminal_cache["ISAAC_VITA_HEAP_TERMINAL_FASTPATH"] = "ON"
        terminal_configuration = validate_cmake_contract(
            terminal_cache, heap_terminal_fastpath=True,
            **texture_contract_arguments,
        )
        assert terminal_configuration["features"][
            "ISAAC_VITA_HEAP_TERMINAL_FASTPATH"
        ] is True
        for requested, stale_value in ((True, "OFF"), (False, "ON")):
            stale_terminal_cache = dict(texture_cache)
            stale_terminal_cache["ISAAC_VITA_HEAP_TERMINAL_FASTPATH"] = stale_value
            try:
                validate_cmake_contract(
                    stale_terminal_cache, heap_terminal_fastpath=requested,
                    **texture_contract_arguments,
                )
            except BuildError as exc:
                assert "ISAAC_VITA_HEAP_TERMINAL_FASTPATH" in str(exc)
            else:
                raise AssertionError("terminal fastpath accepted stale cache")
        for stale_key, stale_value in (
            ("ISAAC_VITA_DIRECT_DEFAULT", "OFF"),
            ("ISAAC_VITA_RASTER_PROBE", "ON"),
            ("ISAAC_VITA_PHASE_PROFILE", "OFF"),
            ("ISAAC_VITA_VITAGL_STOCK_REFERENCE", "OFF"),
        ):
            stale_texture_cache = dict(texture_cache)
            stale_texture_cache[stale_key] = stale_value
            try:
                validate_cmake_contract(
                    stale_texture_cache, **texture_contract_arguments
                )
            except BuildError as exc:
                assert "CMake feature flags differ" in str(exc)
                assert stale_key in str(exc)
            else:
                raise AssertionError(
                    f"texture profile accepted stale {stale_key} cache"
                )
        stale_guard_cache = dict(cache)
        stale_guard_cache["ISAAC_VITA_GUEST_STACK_GUARD"] = "OFF"
        try:
            validate_cmake_contract(
                stale_guard_cache,
                pe_path=pe_path,
                generated_dir=generated,
                heap_mb=81,
                kage=True,
                loading_specialist=False,
                audio=False,
                openal_pool=True,
                io_profile=True,
                archive_file_cache=True,
                archive_validation_skip=True,
                archive_validation_receipt=True,
                fios_cache=True,
                crt_seek_shadow=True,
                continue_profile=True,
                png_decode_profile=True,
                png_native_unfilter=True,
                texture_churn_profile=False,
                game_log_batch=True,
                continue_overlay=True,
                texel_oom_diagnostic=True,
                texel_scratch=True,
                texture_align8_policy=False,
                anm2_scratch=True,
                fxlayers_null_rollback=False,
                fxray_alpha_mask=True,
                heap_ledger_memblock=True,
                heap_ledger_backshift=True,
                heap_overflow_mspace=True,
                room_entry_slab=True,
                room_entry_hybrid=True,
                source_date_epoch=1_700_000_000,
                release=True,
            )
        except BuildError as exc:
            assert "CMake feature flags differ" in str(exc)
            assert "ISAAC_VITA_GUEST_STACK_GUARD" in str(exc)
        else:
            raise AssertionError("stale OFF guest-stack guard cache was accepted")
        stale_epoch_cache = dict(cache)
        stale_epoch_cache["SOURCE_DATE_EPOCH"] = "1700000002"
        try:
            validate_cmake_contract(
                stale_epoch_cache,
                pe_path=pe_path,
                generated_dir=generated,
                heap_mb=81,
                kage=True,
                loading_specialist=False,
                audio=False,
                openal_pool=True,
                io_profile=True,
                archive_file_cache=True,
                archive_validation_skip=True,
                archive_validation_receipt=True,
                fios_cache=True,
                crt_seek_shadow=True,
                continue_profile=True,
                png_decode_profile=True,
                png_native_unfilter=True,
                texture_churn_profile=False,
                game_log_batch=True,
                continue_overlay=True,
                texel_oom_diagnostic=True,
                texel_scratch=True,
                texture_align8_policy=False,
                anm2_scratch=True,
                fxlayers_null_rollback=False,
                fxray_alpha_mask=True,
                heap_ledger_memblock=True,
                heap_ledger_backshift=True,
                heap_overflow_mspace=True,
                room_entry_slab=True,
                room_entry_hybrid=True,
                source_date_epoch=1_700_000_000,
                release=True,
            )
        except BuildError as exc:
            assert "SOURCE_DATE_EPOCH" in str(exc)
        else:
            raise AssertionError("stale deterministic VPK epoch cache was accepted")
        rollback_cache = dict(cache)
        rollback_cache["ISAAC_VITA_ROOM_ENTRY_HYBRID"] = "OFF"
        rollback_configuration = validate_cmake_contract(
            rollback_cache,
            pe_path=pe_path,
            generated_dir=generated,
            heap_mb=81,
            kage=True,
            loading_specialist=False,
            audio=False,
            openal_pool=True,
            io_profile=True,
            archive_file_cache=True,
            archive_validation_skip=True,
            archive_validation_receipt=True,
            fios_cache=True,
            crt_seek_shadow=True,
            continue_profile=True,
            png_decode_profile=True,
            png_native_unfilter=True,
            texture_churn_profile=False,
            game_log_batch=True,
            continue_overlay=True,
            texel_oom_diagnostic=True,
            texel_scratch=True,
            texture_align8_policy=False,
            anm2_scratch=True,
            fxlayers_null_rollback=False,
            fxray_alpha_mask=True,
            heap_ledger_memblock=True,
            heap_ledger_backshift=True,
            heap_overflow_mspace=True,
            room_entry_slab=True,
            room_entry_hybrid=False,
            source_date_epoch=1_700_000_000,
            release=True,
        )
        assert rollback_configuration["features"][
            "ISAAC_VITA_ROOM_ENTRY_HYBRID"
        ] is False
        dependency_inputs = collect_dependency_inputs(
            sdk=sdk,
            build_dir=build,
            cache=cache,
            kage_active=True,
            loading_specialist=False,
            audio_active=False,
        )
        assert {
            record["path"] for record in dependency_inputs
        }.issuperset(
            {
                "vitasdk/share/vita.toolchain.cmake",
                "vitasdk/share/vita.cmake",
                f"vitasdk/bin/{fixture_vpk_packer.name}",
                "recomp/vita/deterministic_vpk.py",
                "recomp/vita/deterministic_vpk.cmake",
                "vitagl-overlay/recipe.txt",
                "vitagl-overlay/build-contract.txt",
                "vitagl-overlay/libvitaGL.a",
                "vitagl-overlay/vitaGL.h",
            }
        )
        wrong_vpk_packer = sdk / "bin" / "wrong-vita-pack-vpk"
        wrong_vpk_packer.write_bytes(b"wrong packer identity\n")
        wrong_packer_cache = dict(cache)
        wrong_packer_cache["ISAAC_VITA_REAL_PACK_VPK"] = str(wrong_vpk_packer)
        try:
            collect_dependency_inputs(
                sdk=sdk,
                build_dir=build,
                cache=wrong_packer_cache,
                kage_active=True,
                loading_specialist=False,
                audio_active=False,
            )
        except BuildError as exc:
            assert "real packer differs from VitaSDK cache" in str(exc)
        else:
            raise AssertionError("wrong deterministic VPK packer was accepted")
        external_vpk_packer = root / fixture_vpk_packer.name
        external_vpk_packer.write_bytes(b"external packer identity\n")
        external_packer_cache = dict(cache)
        external_packer_cache["VITA_PACK_VPK"] = str(external_vpk_packer)
        external_packer_cache["ISAAC_VITA_REAL_PACK_VPK"] = str(
            external_vpk_packer
        )
        try:
            collect_dependency_inputs(
                sdk=sdk,
                build_dir=build,
                cache=external_packer_cache,
                kage_active=True,
                loading_specialist=False,
                audio_active=False,
            )
        except BuildError as exc:
            assert "not the exact VitaSDK tool" in str(exc)
        else:
            raise AssertionError("external deterministic VPK packer was accepted")
        (vitagl_overlay / "libvitaGL.sha256").write_text(
            "0" * 64 + "  corrupted-fixture\n", encoding="ascii"
        )
        try:
            collect_dependency_inputs(
                sdk=sdk,
                build_dir=build,
                cache=cache,
                kage_active=True,
                loading_specialist=False,
                audio_active=False,
            )
        except BuildError as exc:
            assert "does not match libvitaGL.a" in str(exc)
        else:
            raise AssertionError("corrupt vitaGL overlay checksum was accepted")
        (vitagl_overlay / "libvitaGL.sha256").write_text(
            f"{_sha256_file(vitagl_library)}  {vitagl_library}\n",
            encoding="ascii",
        )
        clean_status = hashlib.sha256(b"").hexdigest()
        git_fixture = {
            "commit": "1" * 40,
            "tree": "2" * 40,
            "commit_timestamp": 1_700_000_000,
            "dirty": False,
            "status_sha256": clean_status,
        }
        toolchain_fixture = {
            "python": {
                "implementation": "CPython",
                "version": "fixture",
                "packages": {"capstone": "fixture", "pefile": "fixture"},
            },
            "cmake": {"name": "cmake", "version": "fixture"},
            "ninja": {"used": True, "identity": {"version": "fixture"}},
            "compiler": {"name": "arm-vita-eabi-gcc", "version": "fixture"},
            "binutils": {"linker": {"version": "fixture"}},
        }
        build_manifest = compose_build_manifest(
            release=True,
            git_state=git_fixture,
            pe_path=pe_path,
            generation_manifest=verified,
            generated_dir=generated,
            configuration=configuration,
            toolchain=toolchain_fixture,
            dependency_inputs=dependency_inputs,
            artifacts=artifacts,
            linker_map=linker_map,
            raw_allocator_gate=gate_receipt,
            source_root=root,
            vitasdk_root=sdk,
        )
        repeated_manifest = compose_build_manifest(
            release=True,
            git_state=git_fixture,
            pe_path=pe_path,
            generation_manifest=verified,
            generated_dir=generated,
            configuration=configuration,
            toolchain=toolchain_fixture,
            dependency_inputs=dependency_inputs,
            artifacts=artifacts,
            linker_map=linker_map,
            raw_allocator_gate=gate_receipt,
            source_root=root,
            vitasdk_root=sdk,
        )
        rollback_manifest = compose_build_manifest(
            release=True,
            git_state=git_fixture,
            pe_path=pe_path,
            generation_manifest=verified,
            generated_dir=generated,
            configuration=rollback_configuration,
            toolchain=toolchain_fixture,
            dependency_inputs=dependency_inputs,
            artifacts=artifacts,
            linker_map=linker_map,
            raw_allocator_gate=gate_receipt,
            source_root=root,
            vitasdk_root=sdk,
        )
        assert build_manifest == repeated_manifest
        assert build_manifest["schema"] == BUILD_MANIFEST_SCHEMA
        assert build_manifest["raw_allocator_gate"]["closure_sha256"] == (
            gate_receipt["closure_sha256"]
        )
        assert build_manifest["raw_allocator_gate"]["receipt"] == _file_record(
            gate_receipt_path, RAW_ALLOCATOR_GATE_RECEIPT_NAME
        )
        assert build_manifest["linker_map"]["raw"] == {
            "size": len(linker_map_payload),
            "sha256": hashlib.sha256(linker_map_payload).hexdigest(),
        }
        assert build_manifest["linker_map"]["canonical"]["end"] == (
            "0x8297a6d0"
        )
        assert build_manifest["linker_map"]["canonical"]["schema"] == (
            linker_map_contract.SCHEMA
        )
        assert rollback_manifest["configuration"]["features"][
            "ISAAC_VITA_ROOM_ENTRY_HYBRID"
        ] is False
        assert rollback_manifest["build_id"] != build_manifest["build_id"]
        assert build_manifest["configuration"]["features"][
            "ISAAC_VITA_TEXEL_SCRATCH"
        ] is True
        assert build_manifest["configuration"]["features"][
            "ISAAC_VITA_ARCHIVE_FILE_CACHE"
        ] is True
        assert build_manifest["configuration"]["features"][
            "ISAAC_VITA_ARCHIVE_VALIDATION_SKIP"
        ] is True
        assert build_manifest["configuration"]["features"][
            "ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT"
        ] is True
        assert build_manifest["configuration"]["features"][
            "ISAAC_VITA_FIOS_CACHE"
        ] is True
        assert build_manifest["configuration"]["features"][
            "ISAAC_VITA_CRT_SEEK_SHADOW"
        ] is True
        assert build_manifest["configuration"]["features"][
            "ISAAC_VITA_HEAP_LEDGER_BACKSHIFT"
        ] is True
        assert build_manifest["configuration"]["features"][
            "ISAAC_VITA_HEAP_OVERFLOW_MSPACE"
        ] is True
        assert build_manifest["configuration"]["features"][
            "ISAAC_VITA_ROOM_ENTRY_SLAB"
        ] is True
        assert build_manifest["configuration"]["features"][
            "ISAAC_VITA_ROOM_ENTRY_HYBRID"
        ] is True
        assert build_manifest["configuration"]["features"][
            "ISAAC_VITA_GUEST_STACK_GUARD"
        ] is True
        assert build_manifest["configuration"]["features"][
            "ISAAC_VITA_ANM2_MISSING_LAYER_GUARD"
        ] is True
        assert _is_sha256(build_manifest["build_id"])
        assert _is_sha256(build_manifest["receipt_id"])
        assert build_manifest["build_id"] != build_manifest["receipt_id"]
        generation_mutated_core = {
            key: value for key, value in json.loads(
                _canonical_bytes(build_manifest)
            ).items()
            if key not in ("build_id", "receipt_id")
        }
        generation_mutated_core["generation"]["recipe"]["tools"][
            "python"
        ] = "fixture-new-recipe"
        generation_mutated_core["generation"]["recipe_id"] = hashlib.sha256(
            _canonical_bytes(generation_mutated_core["generation"]["recipe"])
        ).hexdigest()
        assert _build_identity(generation_mutated_core) != (
            build_manifest["build_id"]
        )

        unguarded_configuration = {
            **configuration,
            "features": {
                **configuration["features"],
                "ISAAC_VITA_GUEST_STACK_GUARD": False,
            },
        }
        unguarded_manifest = compose_build_manifest(
            release=True,
            git_state=git_fixture,
            pe_path=pe_path,
            generation_manifest=verified,
            generated_dir=generated,
            configuration=unguarded_configuration,
            toolchain=toolchain_fixture,
            dependency_inputs=dependency_inputs,
            artifacts=artifacts,
            linker_map=linker_map,
            raw_allocator_gate=gate_receipt,
            source_root=root,
            vitasdk_root=sdk,
        )
        assert unguarded_manifest["build_id"] != build_manifest["build_id"]

        gate_receipt_payload = gate_receipt_path.read_bytes()
        raw_only_gate_receipt = json.loads(_canonical_bytes(gate_receipt))
        raw_only_gate_receipt["inputs"]["build_ninja"]["sha256"] = "4" * 64
        gate_receipt_path.write_bytes(
            _canonical_bytes(raw_only_gate_receipt) + b"\n"
        )
        gate_mutated_manifest = compose_build_manifest(
            release=True,
            git_state=git_fixture,
            pe_path=pe_path,
            generation_manifest=verified,
            generated_dir=generated,
            configuration=configuration,
            toolchain=toolchain_fixture,
            dependency_inputs=dependency_inputs,
            artifacts=artifacts,
            linker_map=linker_map,
            raw_allocator_gate=raw_only_gate_receipt,
            source_root=root,
            vitasdk_root=sdk,
        )
        assert gate_mutated_manifest["raw_allocator_gate"]["receipt"] != (
            build_manifest["raw_allocator_gate"]["receipt"]
        )
        assert gate_mutated_manifest["build_id"] == build_manifest["build_id"]
        assert gate_mutated_manifest["receipt_id"] != (
            build_manifest["receipt_id"]
        )
        gate_receipt_path.write_bytes(gate_receipt_payload)

        semantic_gate_receipt = json.loads(_canonical_bytes(gate_receipt))
        semantic_gate_receipt["closure_sha256"] = "3" * 64
        gate_receipt_path.write_bytes(
            _canonical_bytes(semantic_gate_receipt) + b"\n"
        )
        semantic_gate_manifest = compose_build_manifest(
            release=True,
            git_state=git_fixture,
            pe_path=pe_path,
            generation_manifest=verified,
            generated_dir=generated,
            configuration=configuration,
            toolchain=toolchain_fixture,
            dependency_inputs=dependency_inputs,
            artifacts=artifacts,
            linker_map=linker_map,
            raw_allocator_gate=semantic_gate_receipt,
            source_root=root,
            vitasdk_root=sdk,
        )
        assert semantic_gate_manifest["build_id"] != build_manifest["build_id"]
        gate_receipt_path.write_bytes(gate_receipt_payload)
        try:
            compose_build_manifest(
                release=True,
                git_state=git_fixture,
                pe_path=pe_path,
                generation_manifest=verified,
                generated_dir=generated,
                configuration=configuration,
                toolchain=toolchain_fixture,
                dependency_inputs=dependency_inputs,
                artifacts=artifacts,
                linker_map=linker_map,
                raw_allocator_gate=semantic_gate_receipt,
                source_root=root,
                vitasdk_root=sdk,
            )
        except BuildError as exc:
            assert "differs from its live receipt" in str(exc)
        else:
            raise AssertionError(
                "allocator semantic summary/raw receipt mismatch was accepted"
            )

        linker_map_path.write_bytes(linker_map_payload + b"# content mutation\n")
        map_mutated_manifest = compose_build_manifest(
            release=True,
            git_state=git_fixture,
            pe_path=pe_path,
            generation_manifest=verified,
            generated_dir=generated,
            configuration=configuration,
            toolchain=toolchain_fixture,
            dependency_inputs=dependency_inputs,
            artifacts=artifacts,
            linker_map=linker_map,
            raw_allocator_gate=gate_receipt,
            source_root=root,
            vitasdk_root=sdk,
        )
        assert map_mutated_manifest["linker_map"]["canonical"]["end"] == (
            "0x8297a6d0"
        )
        assert map_mutated_manifest["linker_map"]["canonical"]["sha256"] != (
            build_manifest["linker_map"]["canonical"]["sha256"]
        )
        assert map_mutated_manifest["build_id"] != build_manifest["build_id"]
        assert map_mutated_manifest["receipt_id"] != build_manifest["receipt_id"]

        relocated = root / "relocated root [long]+$"
        relocated_source = relocated / "source tree"
        relocated_generated = relocated_source / "nested generated (exact)"
        relocated_sdk = relocated / "VitaSDK softfp"
        relocated_generated.mkdir(parents=True)
        relocated_sdk.mkdir(parents=True)
        (relocated_generated / "manifest.json").write_bytes(
            (generated / "manifest.json").read_bytes()
        )
        relocated_source_prefix = os.fsencode(
            relocated_source.resolve().as_posix()
        )
        relocated_generated_prefix = os.fsencode(
            relocated_generated.resolve().as_posix()
        )
        relocated_sdk_prefix = os.fsencode(relocated_sdk.resolve().as_posix())
        relocated_map_payload = b"".join((
            b"Linker script and memory map\r\n",
            b"LOAD ", relocated_source_prefix,
            b"/recomp/runtime/guest.c.obj\r\n",
            b"LOAD ", relocated_generated_prefix, b"/guest_0000.c.obj\r\n",
            b"LOAD ", relocated_sdk_prefix, b"/lib/libc.a\r\n",
            b" .rodata.guest_generation_", generation_id, b"\r\n",
            b"                0x0000000081000000 guest_generation_",
            generation_id, b"\r\n",
            b"                0x000000008297a6d0                _end = .\r\n",
        ))
        relocated_map = relocated / LINKER_MAP_NAME
        relocated_map.write_bytes(relocated_map_payload)
        relocated_manifest = compose_build_manifest(
            release=True,
            git_state=git_fixture,
            pe_path=pe_path,
            generation_manifest=verified,
            generated_dir=relocated_generated,
            configuration=configuration,
            toolchain=toolchain_fixture,
            dependency_inputs=dependency_inputs,
            artifacts=artifacts,
            linker_map=relocated_map,
            raw_allocator_gate=gate_receipt,
            source_root=relocated_source,
            vitasdk_root=relocated_sdk,
        )
        assert relocated_manifest["linker_map"]["raw"] != (
            build_manifest["linker_map"]["raw"]
        )
        assert relocated_manifest["linker_map"]["canonical"] == (
            build_manifest["linker_map"]["canonical"]
        )
        assert relocated_manifest["build_id"] == build_manifest["build_id"]
        assert relocated_manifest["receipt_id"] != build_manifest["receipt_id"]

        invalid_linker_maps = (
            ("missing", b"Linker script and memory map\n", "missing '_end = .'"),
            (
                "duplicate",
                linker_map_payload
                + b"                0x000000008297a6d4                _end = .\n",
                "duplicate '_end = .'",
            ),
            (
                "malformed",
                b"not-a-hex-address _end = .\n",
                "malformed '_end = .'",
            ),
            (
                "out-of-range",
                b"0x0000000100000000 _end = .\n",
                "outside uint32",
            ),
        )
        try:
            for label, payload, expected_error in invalid_linker_maps:
                linker_map_path.write_bytes(payload)
                try:
                    _linker_map_record(
                        linker_map,
                        source_root=root,
                        generated_root=generated,
                        vitasdk_root=sdk,
                        generation_recipe_id=manifest["recipe_id"],
                    )
                except BuildError as exc:
                    assert expected_error in str(exc)
                else:
                    raise AssertionError(f"{label} linker map was accepted")
        finally:
            linker_map_path.write_bytes(linker_map_payload)

        def write_fixture_manifest(candidate: dict[str, object]) -> Path:
            return write_build_manifest(
                root,
                candidate,
                source_root=root,
                generated_root=generated,
                vitasdk_root=sdk,
                generation_recipe_id=str(verified["recipe_id"]),
            )

        manifest_path = write_fixture_manifest(build_manifest)
        assert manifest_path.read_bytes() == _canonical_bytes(build_manifest) + b"\n"
        try:
            write_fixture_manifest(build_manifest)
        except BuildError as exc:
            assert "refusing to overwrite" in str(exc)
        else:
            raise AssertionError("existing build manifest was overwritten")
        invalidate_build_manifest(root)
        assert not manifest_path.exists()

        linker_map_path.write_bytes(linker_map_payload + b"raw evidence drift\n")
        try:
            write_fixture_manifest(build_manifest)
        except BuildError as exc:
            assert "differs from live canonical map" in str(exc)
        else:
            raise AssertionError("stale raw linker-map evidence was accepted")
        linker_map_path.write_bytes(linker_map_payload)

        forged_canonical_map = json.loads(_canonical_bytes(build_manifest))
        forged_canonical_map["linker_map"]["canonical"]["sha256"] = "5" * 64
        forged_core = {
            key: value for key, value in forged_canonical_map.items()
            if key not in ("build_id", "receipt_id")
        }
        forged_canonical_map["build_id"] = _build_identity(forged_core)
        forged_canonical_map["receipt_id"] = _receipt_identity({
            **forged_core,
            "build_id": forged_canonical_map["build_id"],
        })
        try:
            write_fixture_manifest(forged_canonical_map)
        except BuildError as exc:
            assert "differs from live canonical map" in str(exc)
        else:
            raise AssertionError(
                "self-consistent IDs hid a forged canonical linker-map record"
            )

        gate_receipt_path.write_bytes(gate_receipt_payload + b"raw drift\n")
        try:
            write_fixture_manifest(build_manifest)
        except BuildError as exc:
            assert "raw allocator gate receipt is stale" in str(exc)
        else:
            raise AssertionError("stale raw allocator receipt was accepted")
        gate_receipt_path.write_bytes(gate_receipt_payload)

        malformed_manifests: list[tuple[str, dict[str, object], str]] = []
        wrong_algorithm = json.loads(_canonical_bytes(build_manifest))
        wrong_algorithm["linker_map"]["canonical"]["algorithm"] = "future-v2"
        malformed_manifests.append((
            "wrong canonical algorithm", wrong_algorithm,
            "canonical linker map is malformed",
        ))
        wrong_count = json.loads(_canonical_bytes(build_manifest))
        wrong_count["linker_map"]["canonical"]["replacements"][
            "guest_generation"
        ] = 3
        malformed_manifests.append((
            "wrong replacement count", wrong_count,
            "replacement census is malformed",
        ))
        wrong_map_schema = json.loads(_canonical_bytes(build_manifest))
        wrong_map_schema["linker_map"]["canonical"]["schema"] = 2
        malformed_manifests.append((
            "wrong canonical schema", wrong_map_schema,
            "canonical linker map is malformed",
        ))
        missing_map_hash = json.loads(_canonical_bytes(build_manifest))
        del missing_map_hash["linker_map"]["canonical"]["sha256"]
        malformed_manifests.append((
            "missing canonical field", missing_map_hash,
            "canonical linker map is malformed",
        ))
        extra_map_field = json.loads(_canonical_bytes(build_manifest))
        extra_map_field["linker_map"]["physical_root"] = "must not be stored"
        malformed_manifests.append((
            "extra linker-map field", extra_map_field,
            "linker-map evidence is malformed",
        ))
        for label, hostile, expected_error in malformed_manifests:
            try:
                write_fixture_manifest(hostile)
            except BuildError as exc:
                assert expected_error in str(exc)
            else:
                raise AssertionError(f"{label} was accepted")

        gate_summary_mismatch = json.loads(_canonical_bytes(build_manifest))
        gate_summary_mismatch["raw_allocator_gate"]["closure_sha256"] = "3" * 64
        mismatch_core = {
            key: value for key, value in gate_summary_mismatch.items()
            if key not in ("build_id", "receipt_id")
        }
        gate_summary_mismatch["build_id"] = _build_identity(mismatch_core)
        gate_summary_mismatch["receipt_id"] = _receipt_identity({
            **mismatch_core,
            "build_id": gate_summary_mismatch["build_id"],
        })
        try:
            write_fixture_manifest(gate_summary_mismatch)
        except BuildError as exc:
            assert "semantics differ from live receipt" in str(exc)
        else:
            raise AssertionError(
                "self-consistent IDs hid a gate summary/raw receipt mismatch"
            )

        tampered_receipt_id = dict(build_manifest)
        tampered_receipt_id["receipt_id"] = "0" * 64
        try:
            write_fixture_manifest(tampered_receipt_id)
        except BuildError as exc:
            assert "receipt_id does not match" in str(exc)
        else:
            raise AssertionError("tampered receipt_id was accepted")

        tampered_manifest = dict(build_manifest)
        tampered_manifest["release"] = False
        try:
            write_fixture_manifest(tampered_manifest)
        except BuildError as exc:
            assert "build_id does not match" in str(exc)
        else:
            raise AssertionError("tampered build manifest was written")

        manifest["recipe"]["params"]["image_base"] = 0x30000000
        manifest["recipe_id"] = hashlib.sha256(
            _canonical_bytes(manifest["recipe"])
        ).hexdigest()
        (generated / "manifest.json").write_bytes(_canonical_bytes(manifest) + b"\n")
        try:
            verify_generation_manifest(pe_path, generated)
        except BuildError as exc:
            assert "wrong guest base" in str(exc)
        else:
            raise AssertionError("wrong-base manifest was accepted")

    with tempfile.TemporaryDirectory(prefix="isaac-release-gate-test-") as temporary:
        fixture = Path(temporary)

        def fixture_git(*arguments: str) -> None:
            _run_captured(["git", *arguments], cwd=fixture)

        fixture_git("init", "--quiet")
        fixture_git("config", "user.name", "build_vita fixture")
        fixture_git("config", "user.email", "fixture@example.invalid")
        (fixture / "LICENSE").write_text(
            "Fixture-only audit metadata; not a project licence.\n", encoding="utf-8"
        )
        (fixture / "DISTRIBUTION.md").write_text(
            "# Fixture\n\nProvenance-Status: complete\n", encoding="utf-8"
        )
        (fixture / "safe.txt").write_text("fixture\n", encoding="utf-8")
        fixture_git("add", "-A")
        fixture_git("commit", "--quiet", "-m", "fixture baseline")

        fixture_state = collect_git_state(fixture)
        assert not fixture_state["dirty"]
        assert fixture_state["status_sha256"] == hashlib.sha256(b"").hexdigest()
        run_release_audit(fixture)
        (fixture / "untracked.txt").write_text("dirty\n", encoding="utf-8")
        assert collect_git_state(fixture)["dirty"]
        (fixture / "untracked.txt").unlink()

        fake_bin = fixture / "fake-bin"
        fake_bin.mkdir()
        fake_ninja = fake_bin / ("ninja.exe" if os.name == "nt" else "ninja")
        fake_ninja.write_text("fixture\n", encoding="ascii")
        fake_ninja.chmod(0o755)
        release_environment = os.environ.copy()
        release_environment.update(
            {
                "PATH": str(fake_bin) + os.pathsep + release_environment.get("PATH", ""),
                "PYTHONHASHSEED": "0",
                "SOURCE_DATE_EPOCH": str(fixture_state["commit_timestamp"]),
            }
        )
        release_args = argparse.Namespace(
            reanalyse=True,
            io_profile=False,
            png_decode_profile=False,
            png_native_unfilter=False,
            texture_churn_profile=False,
            sim_cadence_receipt=False,
            world_seam_diag=False,
            exit_menu_profile=False,
            **{
                attribute: True
                for attribute, unused_option in
                RAW_ALLOCATOR_GATE_CANONICAL_ARG_FLAGS
            },
        )
        release_outputs = tuple(fixture / name for name in ("gen", "analysis", "out"))
        validate_release_prerequisites(
            release_args, fixture_state, release_outputs, release_environment
        )
        release_args.png_decode_profile = True
        try:
            validate_release_prerequisites(
                release_args, fixture_state, release_outputs,
                release_environment,
            )
        except BuildError as exc:
            assert "--png-decode-profile" in str(exc)
        else:
            raise AssertionError("release accepted the PNG diagnostic profile")
        release_args.png_decode_profile = False
        release_args.png_native_unfilter = True
        try:
            validate_release_prerequisites(
                release_args, fixture_state, release_outputs,
                release_environment,
            )
        except BuildError as exc:
            assert "--png-native-unfilter" in str(exc)
        else:
            raise AssertionError("release accepted the PNG native experiment")
        release_args.png_native_unfilter = False
        release_args.exit_menu_profile = True
        try:
            validate_release_prerequisites(
                release_args, fixture_state, release_outputs,
                release_environment,
            )
        except BuildError as exc:
            assert "--exit-menu-profile" in str(exc)
        else:
            raise AssertionError(
                "release accepted the Exit-menu diagnostic profile"
            )
        release_args.exit_menu_profile = False
        release_args.texture_churn_profile = True
        try:
            validate_release_prerequisites(
                release_args, fixture_state, release_outputs,
                release_environment,
            )
        except BuildError as exc:
            assert "--texture-churn-profile" in str(exc)
        else:
            raise AssertionError(
                "release accepted the texture-churn diagnostic profile"
            )
        release_args.texture_churn_profile = False
        release_args.sim_cadence_receipt = True
        try:
            validate_release_prerequisites(
                release_args, fixture_state, release_outputs,
                release_environment,
            )
        except BuildError as exc:
            assert "--sim-cadence-receipt" in str(exc)
        else:
            raise AssertionError(
                "release accepted the simulation-cadence diagnostic"
            )
        release_args.sim_cadence_receipt = False
        release_args.audio_stream_receipt = True
        try:
            validate_release_prerequisites(
                release_args, fixture_state, release_outputs,
                release_environment,
            )
        except BuildError as exc:
            assert "--audio-stream-receipt" in str(exc)
        else:
            raise AssertionError("release accepted the audio stream receipt")
        release_args.audio_stream_receipt = False
        release_args.world_seam_diag = True
        try:
            validate_release_prerequisites(
                release_args, fixture_state, release_outputs,
                release_environment,
            )
        except BuildError as exc:
            assert "--world-seam-diag" in str(exc)
        else:
            raise AssertionError("release accepted the world-seam diagnostic")
        release_args.world_seam_diag = False
        release_args.anm2_scratch = False
        try:
            validate_release_prerequisites(
                release_args, fixture_state, release_outputs,
                release_environment,
            )
        except BuildError as exc:
            assert "canonical-exact allocator features" in str(exc)
            assert "--anm2-scratch" in str(exc)
        else:
            raise AssertionError(
                "release accepted a noncanonical raw allocator feature set"
            )
        release_args.anm2_scratch = True
        missing_seed = release_environment.copy()
        missing_seed.pop("PYTHONHASHSEED")
        try:
            validate_release_prerequisites(
                release_args, fixture_state, release_outputs, missing_seed
            )
        except BuildError as exc:
            assert "PYTHONHASHSEED=0" in str(exc)
        else:
            raise AssertionError("release accepted an unset Python hash seed")
        release_outputs[-1].mkdir()
        (release_outputs[-1] / "stale.txt").write_text("stale\n", encoding="ascii")
        try:
            validate_release_prerequisites(
                release_args, fixture_state, release_outputs, release_environment
            )
        except BuildError as exc:
            assert "absent or empty" in str(exc)
        else:
            raise AssertionError("release accepted a non-empty output directory")

        (fixture / "DISTRIBUTION.md").write_text(
            "# Fixture\n\nProvenance-Status: incomplete\n", encoding="utf-8"
        )
        fixture_git("add", "DISTRIBUTION.md")
        fixture_git("commit", "--quiet", "-m", "incomplete provenance")
        try:
            run_release_audit(fixture)
        except BuildError as exc:
            assert "provenance is not complete" in str(exc)
        else:
            raise AssertionError("release audit accepted incomplete provenance")
        (fixture / "DISTRIBUTION.md").write_text(
            "# Fixture\n\nProvenance-Status: complete\n", encoding="utf-8"
        )
        (fixture / "LICENSE").unlink()
        fixture_git("add", "-A")
        fixture_git("commit", "--quiet", "-m", "missing licence")
        try:
            run_release_audit(fixture)
        except BuildError as exc:
            assert "missing required root LICENSE" in str(exc)
        else:
            raise AssertionError("release audit accepted a missing LICENSE")

    parser = _argument_parser()
    assert parser.parse_args(["--release"]).release
    defaults = parser.parse_args(["--pe", "fixture", "--vitasdk", "sdk"])
    assert defaults.heap_mb == 81
    assert all(heap_mb_is_supported(value) for value in (1, 32, 63, 65, 79, 81))
    assert all(not heap_mb_is_supported(value) for value in (0, 64, 80, 82))
    assert defaults.loading_specialist
    assert defaults.archive_file_cache
    assert not defaults.archive_validation_skip
    assert not defaults.archive_validation_receipt
    assert not defaults.fios_cache
    assert not defaults.crt_seek_shadow
    assert not defaults.continue_profile
    assert not defaults.exit_menu_profile
    assert not defaults.png_decode_profile
    assert not defaults.png_native_unfilter
    assert not defaults.texture_churn_profile
    assert not defaults.sim_cadence_receipt
    assert not defaults.stable_30_presentation
    assert not defaults.laser_ring_shadow_skip
    assert not defaults.ogg_queue_emergency
    assert parser.parse_args(["--ogg-queue-emergency"]).ogg_queue_emergency
    laser_args = parser.parse_args([
        "--pe", "fixture", "--vitasdk", "sdk", "--laser-ring-shadow-skip",
    ])
    assert laser_args.laser_ring_shadow_skip
    _validate_arguments(laser_args)
    stable_args = parser.parse_args([
        "--pe", "fixture", "--vitasdk", "sdk", "--kage",
        "--stable-30-presentation",
    ])
    assert stable_args.stable_30_presentation
    _validate_arguments(stable_args)
    stable_without_kage = parser.parse_args([
        "--pe", "fixture", "--vitasdk", "sdk",
        "--no-kage", "--stable-30-presentation",
    ])
    try:
        _validate_arguments(stable_without_kage)
    except BuildError as exc:
        assert "--stable-30-presentation requires --kage" in str(exc)
    else:
        raise AssertionError("stable30 presentation accepted without KAGE")
    assert not defaults.audio_stream_receipt
    assert not defaults.world_seam_diag
    assert not defaults.game_log_batch
    assert not defaults.continue_overlay
    assert defaults.texel_oom_diagnostic
    assert defaults.texel_scratch
    assert defaults.texture_align8_policy
    assert defaults.anm2_scratch
    assert defaults.fxlayers_null_rollback
    assert defaults.fxray_alpha_mask
    assert defaults.heap_ledger_memblock
    assert defaults.heap_ledger_backshift
    assert defaults.heap_overflow_mspace
    assert not defaults.heap_terminal_fastpath
    terminal_args = parser.parse_args([
        "--pe", "fixture", "--vitasdk", "sdk", "--heap-terminal-fastpath",
    ])
    assert terminal_args.heap_terminal_fastpath
    _validate_arguments(terminal_args)
    assert not parser.parse_args([
        "--heap-terminal-fastpath", "--no-heap-terminal-fastpath",
    ]).heap_terminal_fastpath
    terminal_args.heap_overflow_mspace = False
    try:
        _validate_arguments(terminal_args)
    except BuildError as exc:
        assert "--heap-terminal-fastpath requires --heap-overflow-mspace" in str(exc)
    else:
        raise AssertionError("terminal fastpath accepted without overflow mspace")
    assert defaults.room_entry_slab
    assert defaults.room_entry_hybrid
    assert defaults.openal_pool
    disabled = parser.parse_args(
        [
            "--pe", "fixture", "--vitasdk", "sdk",
            "--no-texel-oom-diagnostic",
            "--no-archive-file-cache",
            "--no-texel-scratch",
            "--no-texture-align8-policy",
            "--no-anm2-scratch",
            "--no-fxlayers-null-rollback",
            "--no-fxray-alpha-mask",
            "--no-heap-ledger-memblock",
            "--no-heap-ledger-backshift",
            "--no-heap-overflow-mspace",
            "--no-room-entry-slab",
            "--no-room-entry-hybrid",
            "--no-openal-pool",
        ]
    )
    assert not disabled.texel_oom_diagnostic
    assert not disabled.archive_file_cache
    assert not disabled.texel_scratch
    assert not disabled.texture_align8_policy
    assert not disabled.anm2_scratch
    assert not disabled.fxlayers_null_rollback
    assert not disabled.fxray_alpha_mask
    assert not disabled.heap_ledger_memblock
    assert not disabled.heap_ledger_backshift
    assert not disabled.heap_overflow_mspace
    assert not disabled.room_entry_slab
    assert not disabled.room_entry_hybrid
    assert not disabled.openal_pool
    enabled_skip = parser.parse_args(
        [
            "--pe", "fixture", "--vitasdk", "sdk",
            "--archive-validation-skip",
        ]
    )
    assert enabled_skip.archive_validation_skip
    enabled_receipt = parser.parse_args(
        [
            "--pe", "fixture", "--vitasdk", "sdk",
            "--archive-validation-skip",
            "--archive-validation-receipt",
        ]
    )
    assert enabled_receipt.archive_validation_receipt
    receipt_without_skip = parser.parse_args(
        [
            "--pe", "fixture", "--vitasdk", "sdk",
            "--archive-validation-receipt",
        ]
    )
    try:
        _validate_arguments(receipt_without_skip)
    except BuildError as exc:
        assert "--archive-validation-receipt requires" in str(exc)
    else:
        raise AssertionError("archive receipt was accepted without archive skip")
    enabled_fios = parser.parse_args(
        ["--pe", "fixture", "--vitasdk", "sdk", "--fios-cache"]
    )
    assert enabled_fios.fios_cache
    enabled_seek_shadow = parser.parse_args(
        ["--pe", "fixture", "--vitasdk", "sdk", "--crt-seek-shadow"]
    )
    assert enabled_seek_shadow.crt_seek_shadow
    enabled_continue = parser.parse_args(
        [
            "--pe", "fixture", "--vitasdk", "sdk",
            "--continue-profile", "--continue-overlay",
        ]
    )
    _validate_arguments(enabled_continue)
    enabled_exit_menu = parser.parse_args(
        ["--pe", "fixture", "--vitasdk", "sdk", "--exit-menu-profile"]
    )
    _validate_arguments(enabled_exit_menu)
    assert enabled_exit_menu.exit_menu_profile
    enabled_png_profile = parser.parse_args(
        ["--pe", "fixture", "--vitasdk", "sdk", "--png-decode-profile"]
    )
    _validate_arguments(enabled_png_profile)
    assert enabled_png_profile.png_decode_profile
    enabled_png_native = parser.parse_args(
        ["--pe", "fixture", "--vitasdk", "sdk", "--png-native-unfilter"]
    )
    _validate_arguments(enabled_png_native)
    assert enabled_png_native.png_native_unfilter
    native_without_overflow = parser.parse_args(
        [
            "--pe", "fixture", "--vitasdk", "sdk",
            "--png-native-unfilter", "--no-heap-overflow-mspace",
            "--no-room-entry-slab", "--no-room-entry-hybrid",
        ]
    )
    try:
        _validate_arguments(native_without_overflow)
    except BuildError as exc:
        assert "--heap-overflow-mspace" in str(exc)
    else:
        raise AssertionError("PNG native experiment accepted no range ledger")
    texture_with_default_audio = parser.parse_args(
        ["--pe", "fixture", "--vitasdk", "sdk", "--texture-churn-profile"]
    )
    try:
        _validate_arguments(texture_with_default_audio)
    except BuildError as exc:
        assert "--texture-churn-profile requires --no-audio" in str(exc)
    else:
        raise AssertionError("texture profile accepted the incompatible audio default")
    texture_with_io = parser.parse_args(
        [
            "--pe", "fixture", "--vitasdk", "sdk",
            "--texture-churn-profile", "--no-audio", "--io-profile",
        ]
    )
    try:
        _validate_arguments(texture_with_io)
    except BuildError as exc:
        assert "cannot be combined with --io-profile" in str(exc)
    else:
        raise AssertionError("texture profile accepted the incompatible IO profile")
    enabled_texture_profile = parser.parse_args(
        [
            "--pe", "fixture", "--vitasdk", "sdk",
            "--texture-churn-profile", "--no-audio",
        ]
    )
    _validate_arguments(enabled_texture_profile)
    assert enabled_texture_profile.texture_churn_profile
    enabled_log_batch = parser.parse_args(
        ["--pe", "fixture", "--vitasdk", "sdk", "--game-log-batch"]
    )
    assert enabled_log_batch.game_log_batch
    enabled_audio_receipt = parser.parse_args(
        ["--pe", "fixture", "--vitasdk", "sdk", "--audio-stream-receipt"]
    )
    _validate_arguments(enabled_audio_receipt)
    audio_receipt_without_audio = parser.parse_args(
        [
            "--pe", "fixture", "--vitasdk", "sdk",
            "--audio-stream-receipt", "--no-audio",
        ]
    )
    try:
        _validate_arguments(audio_receipt_without_audio)
    except BuildError as exc:
        assert "--audio-stream-receipt requires" in str(exc)
    else:
        raise AssertionError("audio stream receipt was accepted without audio")
    overlay_without_profile = parser.parse_args(
        [
            "--pe", "fixture", "--vitasdk", "sdk",
            "--continue-overlay",
        ]
    )
    try:
        _validate_arguments(overlay_without_profile)
    except BuildError as exc:
        assert "--continue-overlay requires --continue-profile" in str(exc)
    else:
        raise AssertionError("Continue overlay was accepted without profile")
    overflow_negative_cases = (
        (
            [
                "--heap-mb", "79",
                "--no-anm2-scratch",
                "--no-texel-scratch",
                "--no-texel-oom-diagnostic",
            ],
            "--heap-overflow-mspace requires --heap-mb=81",
        ),
        (["--no-heap-ledger-memblock"], "--heap-ledger-memblock"),
        (["--no-heap-ledger-backshift"], "--heap-ledger-backshift"),
        (["--no-heap-overflow-mspace"], "--room-entry-slab requires"),
        (["--no-room-entry-slab"], "--room-entry-hybrid requires"),
    )
    for extra, expected_message in overflow_negative_cases:
        invalid = parser.parse_args(
            ["--pe", "fixture", "--vitasdk", "sdk", *extra]
        )
        try:
            _validate_arguments(invalid)
        except BuildError as exc:
            assert expected_message in str(exc)
        else:
            raise AssertionError(
                f"overflow dependency combination was accepted: {extra!r}"
            )
    cmake_source = (VITA_SOURCE_DIR / "CMakeLists.txt").read_text(encoding="utf-8")
    assert 'option(ISAAC_VITA_CRT_SEEK_SHADOW' in cmake_source
    assert cmake_source.count("ISAAC_VITA_CRT_SEEK_SHADOW=1") == 1
    assert cmake_source.count(
        "ISAAC_VITA_CRT_SEEK_SHADOW requires ISAAC_VITA_ARCHIVE_FILE_CACHE=ON"
    ) == 1
    assert 'option(ISAAC_VITA_HEAP_OVERFLOW_MSPACE' in cmake_source
    assert '"Add a bounded USER_RW SceClibMspace after newlib exhaustion" OFF)' in cmake_source
    assert cmake_source.count("ISAAC_VITA_HEAP_OVERFLOW_MSPACE=1") == 1
    assert (
        "if(ISAAC_VITA_FXLAYERS_NULL_ROLLBACK OR\n"
        "     ISAAC_VITA_HEAP_OVERFLOW_MSPACE)"
    ) in cmake_source
    assert cmake_source.count("host_vita_heap_overflow_mspace.c") == 1
    assert 'option(ISAAC_VITA_ROOM_ENTRY_SLAB' in cmake_source
    assert cmake_source.count("host_vita_room_entry_slab.c") == 2
    assert cmake_source.count("ISAAC_VITA_ROOM_ENTRY_SLAB=1") == 1
    assert 'option(ISAAC_VITA_ROOM_ENTRY_HYBRID' in cmake_source
    assert cmake_source.count("host_vita_room_entry_external.c") == 1
    assert cmake_source.count("ISAAC_VITA_ROOM_ENTRY_HYBRID=1") == 1
    assert 'option(ISAAC_VITA_ANM2_MISSING_LAYER_GUARD' in cmake_source
    assert (
        '"Skip absent signed ANM2 layer-map entries before array indexing" OFF)'
        in cmake_source
    )
    assert cmake_source.count("ISAAC_VITA_ANM2_MISSING_LAYER_GUARD=1") == 1
    assert cmake_source.count("sub_00005250") == 1
    assert 'option(ISAAC_VITA_SAVE_CHECKSUM_FASTPATH' in cmake_source
    assert cmake_source.count("ISAAC_VITA_SAVE_CHECKSUM_FASTPATH=1") == 1
    assert cmake_source.count("sub_0025b340") >= 2
    assert cmake_source.count("host_vita_save_checksum_native.c") == 1
    assert cmake_source.count("host_vita_save_checksum_guest.c") == 2
    assert 'option(ISAAC_VITA_SAVE_READER_FASTPATH' in cmake_source
    assert cmake_source.count("ISAAC_VITA_SAVE_READER_FASTPATH=1") == 1
    assert cmake_source.count("sub_0025ba60") == 1
    assert cmake_source.count("host_vita_save_reader_guest.c") == 3
    assert 'option(ISAAC_VITA_SAVE_READ32_FUSED' in cmake_source
    assert cmake_source.count("ISAAC_VITA_SAVE_READ32_FUSED=1") == 1
    assert cmake_source.count("sub_0052ea90") == 1
    assert cmake_source.count("host_vita_save_read32_guest.c") == 3
    assert 'option(ISAAC_VITA_SAVE_READER_DIRECT_EDGES' in cmake_source
    assert cmake_source.count("ISAAC_VITA_SAVE_READER_DIRECT_EDGES=1") == 1
    assert cmake_source.count("host_vita_save_reader_direct.c") == 3
    assert 'option(ISAAC_VITA_MEMSET_THUNK_FASTPATH' in cmake_source
    assert cmake_source.count("ISAAC_VITA_MEMSET_THUNK_FASTPATH=1") == 1
    assert cmake_source.count("sub_005ec152") == 1
    assert cmake_source.count("host_vita_memset_thunk_direct.c") == 3
    assert 'option(ISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH' in cmake_source
    assert cmake_source.count(
        "ISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH=1"
    ) == 1
    assert cmake_source.count("sub_005d6720") == 1
    assert cmake_source.count(
        "host_vita_zlib_inflate_flush_native.c"
    ) == 1
    assert cmake_source.count(
        "host_vita_zlib_inflate_flush_guest.c"
    ) == 3
    assert 'option(ISAAC_VITA_PNG_CRC32_FASTPATH' in cmake_source
    assert cmake_source.count("ISAAC_VITA_PNG_CRC32_FASTPATH=1") == 1
    assert cmake_source.count("sub_005c3320") == 1
    assert cmake_source.count("host_vita_png_crc32_native.c") == 1
    assert cmake_source.count("host_vita_png_crc32_guest.c") == 2
    assert 'option(ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH' in cmake_source
    assert cmake_source.count("ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH=1") == 1
    assert 'REGEX "^void sub_005aeb00' in cmake_source
    assert cmake_source.count("host_vita_archive_miniz_native.c") == 3
    assert cmake_source.count("host_vita_archive_miniz_guest.c") == 3
    assert 'option(ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE' in cmake_source
    assert cmake_source.count("ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE=1") == 1
    assert cmake_source.count("sub_0047fbe0") == 1
    assert 'option(ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT' in cmake_source
    assert cmake_source.count("host_vita_archive_receipt.c") == 1
    assert cmake_source.count("ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT=1") == 1
    assert 'option(ISAAC_VITA_RENDERFRAME_FASTPATH' in cmake_source
    assert cmake_source.count("ISAAC_VITA_RENDERFRAME_FASTPATH=1") == 1
    assert cmake_source.count("sub_00003e50") == 1
    assert 'option(ISAAC_VITA_FXRAY_ALPHA_MASK' in cmake_source
    assert cmake_source.count("ISAAC_VITA_FXRAY_ALPHA_MASK=1") == 1
    assert cmake_source.count('"${ISAAC_RUNTIME}/gl_vita_backend.c"') >= 1
    assert 'option(ISAAC_VITA_PILL_BLOOM_BYPASS' in cmake_source
    assert cmake_source.count("ISAAC_VITA_PILL_BLOOM_BYPASS=1") == 2
    assert 'option(ISAAC_VITA_WORLD_SEAM_DIAG' in cmake_source
    assert cmake_source.count("ISAAC_VITA_WORLD_SEAM_DIAG=1") == 1
    assert cmake_source.count("kage_vita_world_seam_diag.c") == 3
    assert cmake_source.count("sub_002ce770") == 2
    assert 'option(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES' in cmake_source
    assert cmake_source.count("ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES=1") == 2
    assert cmake_source.count('sub_002c(7e70|e770)') == 1
    assert 'option(ISAAC_VITA_LASER_PROFILE' in cmake_source
    assert cmake_source.count("ISAAC_VITA_LASER_PROFILE=1") == 2
    assert cmake_source.count('sub_004d(1330|5090)') == 1
    assert 'option(ISAAC_VITA_POOP_FX_PROFILE' in cmake_source
    assert 'option(ISAAC_VITA_POOP_FX_SINGLE_CLOUD' in cmake_source
    assert cmake_source.count("ISAAC_VITA_POOP_FX_PROFILE=1") == 2
    assert cmake_source.count("ISAAC_VITA_POOP_FX_SINGLE_CLOUD=1") == 1
    assert cmake_source.count('sub_004e(c3b0|daa0)') == 1
    assert 'option(ISAAC_VITA_PNG_DECODE_PROFILE' in cmake_source
    # The combined WINDOW+DECODE build suppresses the outer module's aliases:
    # the sampled module alone owns image_begin/end and forwards timestamps.
    # Pin both source-scoped assignments, not only their textual count.
    png_owner_definition = (
        'set_property(SOURCE\n'
        '      ${ISAAC_VITA_PNG_DECODE_PROFILE_OWNERS}\n'
        '      "${ISAAC_RUNTIME}/kage_vita_png_decode_profile.c"\n'
        '      ${ISAAC_VITA_PNG_DECODE_PROFILE_NATIVE_SOURCES}\n'
        '      APPEND PROPERTY COMPILE_DEFINITIONS\n'
        '        ISAAC_VITA_PNG_DECODE_PROFILE=1)'
    )
    png_combined_definition = (
        'set_property(SOURCE "${ISAAC_RUNTIME}/host_vita_png_outer_profile.c"\n'
        '          APPEND PROPERTY COMPILE_DEFINITIONS '
        'ISAAC_VITA_PNG_DECODE_PROFILE=1)'
    )
    assert cmake_source.count(png_owner_definition) == 1
    assert cmake_source.count(png_combined_definition) == 1
    assert "ISAAC_VITA_PNG_DECODE_PROFILE=1" not in cmake_source.replace(
        png_owner_definition, ""
    ).replace(png_combined_definition, "")
    assert (
        'ISAAC_VITA_PNG_DECODE_PROFILE_BUILD_ID=\\"'
        '${ISAAC_VITA_GUEST_LINK_ID}\\"'
    ) in cmake_source
    for root in (
            "sub_005a0c50", "sub_005a0cd0", "sub_005b1500",
            "sub_005c5fb0", "sub_005c6fa0", "sub_005d6d80"):
        assert cmake_source.count(root) >= 1
    assert 'option(ISAAC_VITA_PNG_NATIVE_UNFILTER' in cmake_source
    assert cmake_source.count("ISAAC_VITA_PNG_NATIVE_UNFILTER=1") == 1
    assert cmake_source.count("sub_005c6bf0") == 1
    assert cmake_source.count("host_vita_png_unfilter_native.c") == 2
    assert cmake_source.count("host_vita_png_unfilter_guest.c") == 4
    assert 'option(ISAAC_VITA_TEXTURE_CHURN_PROFILE' in cmake_source
    assert cmake_source.count("ISAAC_VITA_TEXTURE_CHURN_PROFILE=1") == 1
    assert "one aggregate x record per phase window" in cmake_source
    assert 'option(ISAAC_VITA_OPENAL_POOL' in cmake_source
    assert cmake_source.count("host_vita_openal_pool.c") == 3
    assert cmake_source.count("isaac_vita_raw_allocator_poison.h") == 1
    assert cmake_source.count("vita_raw_allocator_gate.py") == 1
    assert cmake_source.count('"${ISAAC_VITA_READELF}"') == 2
    no_kage = parser.parse_args(["--pe", "fixture", "--vitasdk", "sdk", "--no-kage"])
    try:
        _validate_arguments(no_kage)
    except BuildError as exc:
        assert "--no-kage requires disabling KAGE-only features" in str(exc)
    else:
        raise AssertionError("--no-kage accepted enabled KAGE-only features")
    print("build_vita self-test: PASS")


def main(argv: list[str] | None = None) -> int:
    args = _argument_parser().parse_args(argv)
    if args.self_test:
        if args.release:
            raise BuildError("--self-test and --release are mutually exclusive")
        _load_linker_map_contract()
        _self_test()
        return 0
    _validate_arguments(args)

    # Resolve every caller-selected path first, then enforce the portable path
    # boundary before existence checks, hashing, dependency imports, locks,
    # output creation, compiler probes, or generation.
    source_root = REPO_ROOT.resolve()
    pe_path = args.pe.expanduser().resolve()
    sdk_candidate = args.vitasdk.expanduser().resolve()
    lua_source = (
        Path(_lua_source_option_value(args.lua, args.lua_source))
        if args.lua else None
    )
    toolchain_candidate = (
        sdk_candidate / "share" / "vita.toolchain.cmake"
    ).resolve()
    generated_dir = _resolve_output(args.generated_dir)
    analysis_dir = _resolve_output(args.analysis_dir)
    build_dir = _resolve_output(args.build_dir)
    _validate_build_path_policy(
        source_root=source_root,
        sdk_root=sdk_candidate,
        toolchain=toolchain_candidate,
        pe_path=pe_path,
        generated_dir=generated_dir,
        build_dir=build_dir,
        lua_source=lua_source,
    )
    _validate_source_relative_path_census(source_root)
    if lua_source is not None:
        lua_records = validate_lua53_source(lua_source)
        print(
            "Lua 5.3.3 source contract verified: "
            f"files={len(lua_records)} "
            f"set={_lua53_source_set_sha256(_load_lua53_pinned_records())}"
        )
    _load_linker_map_contract()

    pe_path = _regular_file(pe_path, "unpacked PE")
    sdk, toolchain = validate_vitasdk(sdk_candidate)
    if toolchain != toolchain_candidate:
        raise BuildError("resolved VitaSDK toolchain changed during validation")
    validate_supported_pe(pe_path)
    try:
        import capstone  # noqa: F401
    except ImportError as exc:
        raise BuildError(
            "Python dependency 'capstone' is missing; install the recompiler "
            "requirements before building"
        ) from exc

    _validate_output_directories(
        pe_path, sdk, generated_dir, analysis_dir, build_dir,
        lua_source=lua_source,
    )

    cmake_candidate = Path(args.cmake).expanduser()
    if cmake_candidate.is_file():
        cmake_executable = str(cmake_candidate.resolve())
    else:
        cmake_executable = shutil.which(args.cmake)
    if not cmake_executable:
        raise BuildError(f"CMake executable was not found: {args.cmake}")

    environment = os.environ.copy()
    environment["VITASDK"] = str(sdk)
    environment["PATH"] = str(sdk / "bin") + os.pathsep + environment.get("PATH", "")
    initial_git_state = collect_git_state()
    source_date_epoch = int(initial_git_state["commit_timestamp"])
    if args.release:
        if initial_git_state["dirty"]:
            raise BuildError(
                "--release requires a clean Git worktree, including untracked files"
            )
        run_release_audit()
        validate_release_prerequisites(
            args,
            initial_git_state,
            (generated_dir, analysis_dir, build_dir),
            environment,
        )
        environment.update({"LC_ALL": "C", "TZ": "UTC", "ZERO_AR_DATE": "1"})

    # Acquire every shared-output lock before the first build-dir mutation.
    # Canonical ordering prevents crossed custom paths from deadlocking; the
    # stage-local funcs/bounds/gen_all locks then re-enter in this same thread.
    with _pipeline_locks(generated_dir, analysis_dir, build_dir):
        build_dir.mkdir(parents=True, exist_ok=True)
        invalidate_build_manifest(build_dir)
        run_softfp_link_probe(sdk, build_dir, environment)

        run_fresh_generation(
            pe_path,
            analysis_dir,
            generated_dir,
            args.jobs,
            args.reanalyse,
        )
        manifest = verify_generation_manifest(pe_path, generated_dir)
        print(
            "generated contract verified: "
            f"recipe={manifest['recipe_id']} outputs={manifest['output_set_id']}"
        )
        if args.generate_only:
            print(f"generate-only: corpus ready in {generated_dir}")
            return 0

        configure = cmake_configure_command(
            str(cmake_executable),
            sdk,
            toolchain,
            pe_path,
            generated_dir,
            build_dir,
            args.heap_mb,
            args.kage,
            args.loading_specialist,
            args.audio,
            args.openal_pool,
            args.io_profile,
            args.archive_file_cache,
            args.archive_validation_skip,
            args.archive_validation_receipt,
            args.fios_cache,
            args.crt_seek_shadow,
            args.continue_profile,
            args.png_decode_profile,
            args.png_native_unfilter,
            args.texture_churn_profile,
            args.game_log_batch,
            args.continue_overlay,
            args.texel_oom_diagnostic,
            args.texel_scratch,
            args.texture_align8_policy,
            args.anm2_scratch,
            args.fxlayers_null_rollback,
            args.fxray_alpha_mask,
            args.heap_ledger_memblock,
            args.heap_ledger_backshift,
            args.heap_overflow_mspace,
            args.room_entry_slab,
            args.room_entry_hybrid,
            source_date_epoch,
            args.release,
            exit_menu_profile=args.exit_menu_profile,
            lua=args.lua,
            lua_source=lua_source,
            sim_cadence_receipt=args.sim_cadence_receipt,
            audio_stream_receipt=args.audio_stream_receipt,
            heap_terminal_fastpath=args.heap_terminal_fastpath,
            world_seam_diag=args.world_seam_diag,
            stable_30_presentation=args.stable_30_presentation,
            laser_ring_shadow_skip=args.laser_ring_shadow_skip,
            ogg_queue_emergency=args.ogg_queue_emergency,
        )
        print("\n== CMake configure ==", flush=True)
        run_command(configure, environment)
        print("\n== CMake build ==", flush=True)
        run_command(
            [
                str(cmake_executable),
                "--build",
                str(build_dir),
                "--parallel",
                str(args.jobs),
            ],
            environment,
        )

        artifacts = locate_artifacts(build_dir)
        raw_allocator_receipt = verify_raw_allocator_gate(
            build_dir, artifacts["ELF"], require_canonical=args.release
        )
        print(
            "raw allocator gate verified: "
            f"sources={raw_allocator_receipt['source_count']} "
            f"objects={raw_allocator_receipt['object_count']} "
            f"closure={raw_allocator_receipt['closure_sha256']}"
        )
        audio_active = bool(args.kage and args.audio)
        verify_package(pe_path, artifacts, audio=audio_active)
        linker_map = locate_linker_map(build_dir)
        cache = _read_cmake_cache(build_dir)
        configuration = validate_cmake_contract(
            cache,
            pe_path=pe_path,
            generated_dir=generated_dir,
            heap_mb=args.heap_mb,
            kage=args.kage,
            loading_specialist=args.loading_specialist,
            audio=args.audio,
            openal_pool=args.openal_pool,
            io_profile=args.io_profile,
            archive_file_cache=args.archive_file_cache,
            archive_validation_skip=args.archive_validation_skip,
            archive_validation_receipt=args.archive_validation_receipt,
            fios_cache=args.fios_cache,
            crt_seek_shadow=args.crt_seek_shadow,
            continue_profile=args.continue_profile,
            png_decode_profile=args.png_decode_profile,
            png_native_unfilter=args.png_native_unfilter,
            texture_churn_profile=args.texture_churn_profile,
            game_log_batch=args.game_log_batch,
            continue_overlay=args.continue_overlay,
            texel_oom_diagnostic=args.texel_oom_diagnostic,
            texel_scratch=args.texel_scratch,
            texture_align8_policy=args.texture_align8_policy,
            anm2_scratch=args.anm2_scratch,
            fxlayers_null_rollback=args.fxlayers_null_rollback,
            fxray_alpha_mask=args.fxray_alpha_mask,
            heap_ledger_memblock=args.heap_ledger_memblock,
            heap_ledger_backshift=args.heap_ledger_backshift,
            heap_overflow_mspace=args.heap_overflow_mspace,
            room_entry_slab=args.room_entry_slab,
            room_entry_hybrid=args.room_entry_hybrid,
            source_date_epoch=source_date_epoch,
            release=args.release,
            exit_menu_profile=args.exit_menu_profile,
            lua=args.lua,
            lua_source=lua_source,
            sim_cadence_receipt=args.sim_cadence_receipt,
            audio_stream_receipt=args.audio_stream_receipt,
            heap_terminal_fastpath=args.heap_terminal_fastpath,
            world_seam_diag=args.world_seam_diag,
            stable_30_presentation=args.stable_30_presentation,
            laser_ring_shadow_skip=args.laser_ring_shadow_skip,
            ogg_queue_emergency=args.ogg_queue_emergency,
        )
        toolchain_identity = collect_toolchain_identity(
            cmake_executable=Path(cmake_executable).resolve(),
            sdk=sdk,
            build_dir=build_dir,
            cache=cache,
            environment=environment,
        )
        dependency_inputs = collect_dependency_inputs(
            sdk=sdk,
            build_dir=build_dir,
            cache=cache,
            kage_active=args.kage,
            loading_specialist=args.loading_specialist,
            audio_active=audio_active,
            lua_source=lua_source,
        )

        final_git_state = collect_git_state()
        if (final_git_state["commit"] != initial_git_state["commit"] or
                final_git_state["commit_timestamp"] != source_date_epoch):
            raise BuildError(
                "Git HEAD/commit epoch changed during deterministic VPK build"
            )
        if args.release:
            if final_git_state["dirty"]:
                raise BuildError("Git worktree became dirty during --release build")
            for key in ("commit", "tree"):
                if final_git_state[key] != initial_git_state[key]:
                    raise BuildError(f"Git {key} changed during --release build")
            run_release_audit()

        build_manifest = compose_build_manifest(
            release=args.release,
            git_state=final_git_state,
            pe_path=pe_path,
            generation_manifest=manifest,
            generated_dir=generated_dir,
            configuration=configuration,
            toolchain=toolchain_identity,
            dependency_inputs=dependency_inputs,
            artifacts=artifacts,
            linker_map=linker_map,
            raw_allocator_gate=raw_allocator_receipt,
            source_root=REPO_ROOT,
            vitasdk_root=sdk,
        )
        manifest_path = write_build_manifest(
            build_dir,
            build_manifest,
            source_root=REPO_ROOT,
            generated_root=generated_dir,
            vitasdk_root=sdk,
            generation_recipe_id=str(manifest["recipe_id"]),
        )
        print_artifacts(artifacts)
        print(
            f"manifest size={manifest_path.stat().st_size} "
            f"sha256={_sha256_file(manifest_path)} "
            f"build_id={build_manifest['build_id']} "
            f"receipt_id={build_manifest['receipt_id']} path={manifest_path}"
        )
        return 0


# multiprocessing's spawn mode imports the main script before it unpickles the
# gen_all worker initializer.  Rebind the legacy paths during that import so a
# child never falls back to a hard-coded developer-machine temporary path.
_bootstrap_spawn_child()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
