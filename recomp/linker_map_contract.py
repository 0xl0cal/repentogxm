#!/usr/bin/env python3
"""Path-independent, fail-closed identity for the Vita GNU linker map."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import re


SCHEMA = 1
ALGORITHM = "isaac-vita-linker-map-v1"
EXPECTED_GUEST_GENERATION_COUNT = 2

_TOKENS = {
    "source_root": b"@ISAAC_SOURCE_ROOT@",
    "generated_root": b"@ISAAC_GENERATED_ROOT@",
    "vitasdk_root": b"@ISAAC_VITASDK_ROOT@",
}
_GENERATION_TOKEN = b"@ISAAC_GENERATION_RECIPE_ID@"
_GUEST_GENERATION_PREFIX = b"guest_generation_"
_CMAKE_OBJECT_PREFIX = b"CMakeFiles/isaac_first_arm_fault.dir"
_PATH_DELIMITERS = frozenset(b" \t\r\n(=:\"")
_LOWER_HEX_64 = re.compile(rb"[0-9a-f]{64}")
_IDENTIFIER_BYTE = re.compile(rb"[A-Za-z0-9_]")


class LinkerMapContractError(ValueError):
    """The linker map cannot be assigned the canonical contract identity."""


@dataclass(frozen=True)
class CanonicalLinkerMap:
    payload: bytes
    record: dict[str, object]


def _root_prefix(path: Path, label: str) -> tuple[Path, bytes]:
    candidate = path.expanduser()
    if not candidate.is_absolute():
        raise LinkerMapContractError(f"{label} root is not absolute: {path}")
    resolved = candidate.resolve()
    if resolved.parent == resolved:
        raise LinkerMapContractError(f"{label} root is too broad: {resolved}")
    encoded = os.fsencode(resolved.as_posix())
    if not encoded or b"\x00" in encoded:
        raise LinkerMapContractError(f"{label} root has an unsafe encoding")
    return resolved, encoded + b"/"


def _sole_end(payload: bytes) -> int:
    candidate_pattern = re.compile(
        rb"(?<![A-Za-z0-9_])_end(?![A-Za-z0-9_])[ \t]*=[ \t]*\."
    )
    candidates = [
        line
        for line in payload.splitlines()
        for unused_match in candidate_pattern.finditer(line)
    ]
    if not candidates:
        raise LinkerMapContractError("linker map is missing '_end = .'")
    if len(candidates) != 1:
        raise LinkerMapContractError(
            "linker map has duplicate '_end = .' assignments: "
            f"found {len(candidates)}"
        )
    match = re.fullmatch(
        rb"[ \t]*(0[xX][0-9A-Fa-f]+)[ \t]+_end[ \t]*=[ \t]*\.[ \t]*",
        candidates[0],
    )
    if match is None:
        raise LinkerMapContractError(
            "linker map has malformed '_end = .' assignment"
        )
    end = int(match.group(1), 16)
    if end > 0xFFFFFFFF:
        raise LinkerMapContractError(
            f"linker map '_end = .' is outside uint32: 0x{end:x}"
        )
    return end


def _guest_generation_identity_positions(payload: bytes) -> list[int]:
    positions: list[int] = []
    prefix_size = len(_GUEST_GENERATION_PREFIX)
    for match in re.finditer(re.escape(_GUEST_GENERATION_PREFIX), payload):
        preceding = payload[match.start() - 1:match.start()]
        if preceding and _IDENTIFIER_BYTE.fullmatch(preceding):
            raise LinkerMapContractError(
                "linker map embeds guest_generation inside another identifier"
            )
        start = match.start() + prefix_size
        # guest_generation_id is the separate runtime accessor symbol.  It is
        # semantic map content, not generation provenance, and remains exact.
        if payload[start:start + 2] == b"id":
            following = payload[start + 2:start + 3]
            if not following or _IDENTIFIER_BYTE.fullmatch(following) is None:
                continue
        positions.append(match.start())
    return positions


def _replace_path_prefix(
    payload: bytes, prefix: bytes, token: bytes, label: str,
) -> tuple[bytes, int]:
    positions: list[int] = []
    offset = 0
    while True:
        position = payload.find(prefix, offset)
        if position < 0:
            break
        preceding = payload[position - 1] if position else None
        marker_start = position - len(_CMAKE_OBJECT_PREFIX)
        follows_cmake_object_prefix = (
            marker_start >= 0
            and payload[marker_start:position] == _CMAKE_OBJECT_PREFIX
        )
        if (
            preceding is not None
            and preceding not in _PATH_DELIMITERS
            and not follows_cmake_object_prefix
        ):
            raise LinkerMapContractError(
                f"linker map embeds {label} outside a filename boundary"
            )
        positions.append(position)
        offset = position + len(prefix)
    if not positions:
        raise LinkerMapContractError(
            f"linker map is missing required {label} prefix"
        )
    chunks: list[bytes] = []
    offset = 0
    for position in positions:
        chunks.extend((payload[offset:position], token))
        offset = position + len(prefix)
    chunks.append(payload[offset:])
    return b"".join(chunks), len(positions)


def canonicalize(
    payload: bytes,
    *,
    source_root: Path,
    generated_root: Path,
    vitasdk_root: Path,
    generation_recipe_id: str,
) -> CanonicalLinkerMap:
    """Replace only verified path/recipe provenance, preserving all other bytes."""

    if not isinstance(payload, bytes):
        raise LinkerMapContractError("linker map payload is not bytes")
    if (
        not isinstance(generation_recipe_id, str)
        or re.fullmatch(r"[0-9a-f]{64}", generation_recipe_id) is None
    ):
        raise LinkerMapContractError(
            "generation recipe_id is not lowercase SHA-256"
        )

    end = _sole_end(payload)
    for token in (*_TOKENS.values(), _GENERATION_TOKEN):
        if token in payload:
            raise LinkerMapContractError(
                f"linker map already contains canonical token {token!r}"
            )

    roots = {
        "source_root": _root_prefix(source_root, "source"),
        "generated_root": _root_prefix(generated_root, "generated"),
        "vitasdk_root": _root_prefix(vitasdk_root, "VitaSDK"),
    }
    resolved_roots = [value[0] for value in roots.values()]
    if len(set(resolved_roots)) != len(resolved_roots):
        raise LinkerMapContractError("linker map roots are duplicated")

    canonical = payload
    counts: dict[str, int] = {}
    # A custom generated directory or SDK may legitimately live under the
    # source tree.  Replace the narrowest (longest) exact prefix first.
    ordered = sorted(
        roots.items(), key=lambda item: len(item[1][1]), reverse=True
    )
    for label, (unused_resolved, prefix) in ordered:
        canonical, count = _replace_path_prefix(
            canonical, prefix, _TOKENS[label] + b"/", label
        )
        counts[label] = count

    # A bare or near-prefix occurrence (for example /src2 for root /src)
    # would otherwise remain machine-specific while looking almost covered.
    for label, (unused_resolved, prefix) in roots.items():
        bare = prefix[:-1]
        if bare in canonical:
            raise LinkerMapContractError(
                f"linker map retains unsafe {label} root occurrence"
            )

    positions = _guest_generation_identity_positions(canonical)
    if len(positions) != EXPECTED_GUEST_GENERATION_COUNT:
        raise LinkerMapContractError(
            "linker map guest_generation occurrence count changed: "
            f"{len(positions)} != {EXPECTED_GUEST_GENERATION_COUNT}"
        )
    expected = generation_recipe_id.encode("ascii")
    prefix_size = len(_GUEST_GENERATION_PREFIX)
    for position in positions:
        start = position + prefix_size
        candidate = canonical[start:start + 64]
        following = canonical[start + 64:start + 65]
        if (
            _LOWER_HEX_64.fullmatch(candidate) is None
            or (following and _IDENTIFIER_BYTE.fullmatch(following))
        ):
            raise LinkerMapContractError(
                "linker map has malformed guest_generation identity"
            )
        if candidate != expected:
            raise LinkerMapContractError(
                "linker map has foreign guest_generation identity: "
                f"{candidate.decode('ascii')} != {generation_recipe_id}"
            )
    needle = _GUEST_GENERATION_PREFIX + expected
    if canonical.count(needle) != EXPECTED_GUEST_GENERATION_COUNT:
        raise LinkerMapContractError(
            "linker map guest_generation replacement census changed"
        )
    canonical = canonical.replace(
        needle, _GUEST_GENERATION_PREFIX + _GENERATION_TOKEN
    )
    counts["guest_generation"] = EXPECTED_GUEST_GENERATION_COUNT

    ordered_counts = {
        key: counts[key]
        for key in (
            "source_root", "generated_root", "vitasdk_root",
            "guest_generation",
        )
    }
    record: dict[str, object] = {
        "schema": SCHEMA,
        "algorithm": ALGORITHM,
        "size": len(canonical),
        "sha256": hashlib.sha256(canonical).hexdigest(),
        "replacements": ordered_counts,
        "end": f"0x{end:08x}",
    }
    return CanonicalLinkerMap(payload=canonical, record=record)
