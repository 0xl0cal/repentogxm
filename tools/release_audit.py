#!/usr/bin/env python3
"""Fail closed on material that must not enter a public source repository.

The audit checks current indexed files, optionally non-ignored untracked files,
the one known embedded-asset path even when ignored, and every blob reachable
from Git history.  It is dependency-free so it can run before staging or
packaging.  Warnings become failures with ``--strict``.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys


FORBIDDEN_SUFFIXES = {
    ".7z",
    ".a",
    ".apk",
    ".bin",
    ".cert",
    ".dat",
    ".dll",
    ".dmp",
    ".elf",
    ".exe",
    ".ilk",
    ".ipa",
    ".keys",
    ".lib",
    ".nca",
    ".nro",
    ".nso",
    ".nsp",
    ".nsz",
    ".obj",
    ".pdb",
    ".pkg",
    ".rar",
    ".rpl",
    ".self",
    ".sfo",
    ".skprx",
    ".sprx",
    ".suprx",
    ".tar",
    ".tik",
    ".velf",
    ".vpk",
    ".zip",
}

FORBIDDEN_BASENAMES = {
    "eboot.bin",
    "libshacccg.suprx",
    "kubridge.skprx",
    "param.sfo",
    "prod.keys",
    "title.keys",
}

FORBIDDEN_COMPONENTS = {
    "documents",
    "exefs",
    "romfs",
    "steamapps",
    "workshop",
}

# These five package images are pinned byte-for-byte by
# recomp/vita/test_livearea_package.py.  This exception only bypasses the
# broad media-extension heuristic; it does not grant redistribution rights or
# override the repository-wide provenance gate.  Never allowlist a directory
# or an extension.
ALLOWED_MEDIA_ASSETS: frozenset[str] = frozenset({
    "recomp/vita/assets/sce_sys/icon0.png",
    "recomp/vita/assets/sce_sys/pic0.png",
    "recomp/vita/assets/sce_sys/livearea/contents/bg0.png",
    "recomp/vita/assets/sce_sys/livearea/contents/nicalis.png",
    "recomp/vita/assets/sce_sys/livearea/contents/startup.png",
})

SUSPICIOUS_MEDIA_SUFFIXES = {
    ".aac",
    ".anm2",
    ".avi",
    ".bmp",
    ".dds",
    ".flac",
    ".gif",
    ".ico",
    ".jpeg",
    ".jpg",
    ".m4a",
    ".mkv",
    ".mov",
    ".mp3",
    ".mp4",
    ".ogg",
    ".ogv",
    ".opus",
    ".otf",
    ".pcx",
    ".png",
    ".svg",
    ".tga",
    ".ttf",
    ".wav",
    ".webm",
    ".webp",
}

GENERATED_GUEST_RE = re.compile(r"(?:^|/)guest_[0-9]{4}\.c$", re.I)

PRIVATE_MARKERS = (
    b"-----BEGIN " + b"PRIVATE KEY-----",
    b"-----BEGIN RSA " + b"PRIVATE KEY-----",
    b"-----BEGIN OPENSSH " + b"PRIVATE KEY-----",
)

MAGIC_SIGNATURES = (
    ("PE", (b"MZ",)),
    ("ELF", (b"\x7fELF",)),
    ("SELF", (b"SCE\x00",)),
    ("ZIP", (b"PK\x03\x04", b"PK\x05\x06", b"PK\x07\x08")),
)

LOCAL_PATH_PATTERNS = (
    re.compile(r"[A-Za-z]:\\+(?:Users|Temp|SteamLibrary)\\+", re.I),
    re.compile(r"/home/[A-Za-z0-9_.-]+/"),
)

PUBLIC_DOCS = {
    "DISTRIBUTION.md",
    "INSTALL.md",
    "PLAN.md",
    "README.md",
    "THIRD_PARTY.md",
    "TROUBLESHOOTING.md",
}

PROVENANCE_STATUS_RE = re.compile(
    r"^Provenance-Status:[ \t]*([^\r\n]+?)[ \t]*$", re.I | re.M
)


def run_git(root: Path, *args: str, input_data: bytes | None = None) -> bytes:
    completed = subprocess.run(
        ["git", *args],
        cwd=root,
        check=False,
        input=input_data,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        message = completed.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"git {' '.join(args)} failed: {message}")
    return completed.stdout


def repository_root() -> Path:
    raw = run_git(Path.cwd(), "rev-parse", "--show-toplevel")
    return Path(os.fsdecode(raw).strip()).resolve()


def indexed_paths(root: Path) -> list[str]:
    raw = run_git(root, "ls-files", "-z")
    return [os.fsdecode(item) for item in raw.split(b"\0") if item]


def untracked_paths(root: Path) -> list[str]:
    raw = run_git(root, "ls-files", "--others", "--exclude-standard", "-z")
    return [os.fsdecode(item) for item in raw.split(b"\0") if item]


def normalize_path(rel: str) -> PurePosixPath:
    return PurePosixPath(rel.replace("\\", "/"))


def magic_kind(data: bytes) -> str | None:
    for name, signatures in MAGIC_SIGNATURES:
        if data.startswith(signatures):
            return name
    return None


def path_blockers(rel: str, scope: str) -> list[str]:
    """Return release blockers implied by one source or historical path."""
    posix = normalize_path(rel)
    normalized = str(posix)
    folded = normalized.casefold()
    suffix = posix.suffix.casefold()
    basename = posix.name.casefold()
    components = {part.casefold() for part in posix.parts[:-1]}
    blockers: list[str] = []

    if suffix in FORBIDDEN_SUFFIXES:
        blockers.append(f"forbidden binary/key suffix in {scope}: {rel}")
    if (
        suffix in SUSPICIOUS_MEDIA_SUFFIXES
        and folded not in ALLOWED_MEDIA_ASSETS
    ):
        blockers.append(f"suspicious media asset in {scope}: {rel}")
    if basename in FORBIDDEN_BASENAMES:
        blockers.append(f"forbidden runtime/input file in {scope}: {rel}")
    if GENERATED_GUEST_RE.search(normalized):
        blockers.append(f"generated translated guest code in {scope}: {rel}")

    bad_components = components & FORBIDDEN_COMPONENTS
    if bad_components:
        blockers.append(
            f"forbidden proprietary/extracted path component "
            f"{sorted(bad_components)!r} in {scope}: {rel}"
        )
    return blockers


def inspect_metadata(root: Path) -> list[str]:
    blockers: list[str] = []
    root_entries = {item.name: item for item in root.iterdir()}
    licence = root_entries.get("LICENSE")
    if licence is None or not licence.is_file():
        blockers.append("missing required root LICENSE")
    else:
        try:
            if not licence.read_bytes().strip():
                blockers.append("required root LICENSE is empty")
        except OSError as exc:
            blockers.append(f"required root LICENSE cannot be read: {exc}")

    policy = root_entries.get("DISTRIBUTION.md")
    if policy is None or not policy.is_file():
        blockers.append("missing required root DISTRIBUTION.md provenance policy")
        return blockers
    try:
        text = policy.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        blockers.append(f"DISTRIBUTION.md cannot be read as UTF-8: {exc}")
        return blockers

    statuses = PROVENANCE_STATUS_RE.findall(text)
    if len(statuses) != 1:
        blockers.append(
            "DISTRIBUTION.md must contain exactly one Provenance-Status field"
        )
    elif statuses[0].strip().casefold() != "complete":
        blockers.append(
            f"source/dependency provenance is not complete: {statuses[0].strip()}"
        )
    return blockers


def inspect_current(root: Path, paths: list[str]) -> tuple[list[str], list[str]]:
    blockers: list[str] = []
    warnings: list[str] = []
    folded: dict[str, str] = {}

    for rel in paths:
        posix = normalize_path(rel)
        normalized = str(posix)
        key = normalized.casefold()
        previous = folded.setdefault(key, rel)
        if previous != rel:
            blockers.append(f"case-colliding source paths: {previous!r} and {rel!r}")

        if any(
            part.casefold().startswith(("backup-", "backup_"))
            for part in posix.parts
        ):
            warnings.append(f"backup material is not release source: {rel}")

        blockers.extend(path_blockers(rel, "current files"))
        path = root / Path(*posix.parts)
        try:
            data = path.read_bytes()
        except OSError as exc:
            blockers.append(f"current source file cannot be read: {rel}: {exc}")
            continue

        if any(marker in data for marker in PRIVATE_MARKERS):
            blockers.append(f"private-key marker in current file: {rel}")
        kind = magic_kind(data)
        if kind is not None:
            blockers.append(f"{kind} magic in current file: {rel}")
        if len(data) > 10 * 1024 * 1024:
            warnings.append(f"current file exceeds 10 MiB; verify provenance: {rel}")

        if normalized in PUBLIC_DOCS:
            text = data.decode("utf-8", errors="replace")
            if any(pattern.search(text) for pattern in LOCAL_PATH_PATTERNS):
                warnings.append(f"public documentation contains a local path: {rel}")

    return blockers, warnings


def history_objects(root: Path) -> tuple[list[str], dict[str, str]]:
    raw = run_git(root, "rev-list", "--objects", "--all")
    object_ids: list[str] = []
    paths: dict[str, str] = {}
    seen: set[str] = set()
    for line in raw.splitlines():
        oid_bytes, separator, path_bytes = line.partition(b" ")
        try:
            oid = oid_bytes.decode("ascii")
        except UnicodeDecodeError as exc:
            raise RuntimeError(f"non-ASCII object id from git rev-list: {exc}") from exc
        if not oid:
            continue
        if oid not in seen:
            seen.add(oid)
            object_ids.append(oid)
        if separator and oid not in paths:
            paths[oid] = os.fsdecode(path_bytes)
    return object_ids, paths


def history_blobs(root: Path) -> list[tuple[str, str | None, bytes]]:
    object_ids, paths = history_objects(root)
    if not object_ids:
        return []

    query = ("\n".join(object_ids) + "\n").encode("ascii")
    checked = run_git(
        root,
        "cat-file",
        "--batch-check=%(objectname) %(objecttype) %(objectsize)",
        input_data=query,
    ).splitlines()
    if len(checked) != len(object_ids):
        raise RuntimeError("git cat-file --batch-check returned an incomplete inventory")

    blob_sizes: list[tuple[str, int]] = []
    for expected, line in zip(object_ids, checked):
        fields = line.split()
        if len(fields) != 3:
            raise RuntimeError(f"malformed git cat-file inventory line: {line!r}")
        oid = fields[0].decode("ascii", errors="strict")
        kind = fields[1].decode("ascii", errors="strict")
        if oid != expected:
            raise RuntimeError("git cat-file inventory order changed unexpectedly")
        if kind == "blob":
            try:
                size = int(fields[2])
            except ValueError as exc:
                raise RuntimeError(f"invalid blob size for {oid}") from exc
            blob_sizes.append((oid, size))

    if not blob_sizes:
        return []
    blob_query = ("\n".join(oid for oid, _ in blob_sizes) + "\n").encode("ascii")
    stream = run_git(root, "cat-file", "--batch", input_data=blob_query)
    offset = 0
    result: list[tuple[str, str | None, bytes]] = []
    for expected_oid, expected_size in blob_sizes:
        header_end = stream.find(b"\n", offset)
        if header_end < 0:
            raise RuntimeError("git cat-file --batch omitted a blob header")
        header = stream[offset:header_end].split()
        if len(header) != 3:
            raise RuntimeError(f"malformed git blob header: {header!r}")
        oid = header[0].decode("ascii", errors="strict")
        kind = header[1].decode("ascii", errors="strict")
        try:
            size = int(header[2])
        except ValueError as exc:
            raise RuntimeError(f"invalid streamed blob size for {oid}") from exc
        if oid != expected_oid or kind != "blob" or size != expected_size:
            raise RuntimeError("git cat-file --batch blob metadata mismatch")
        data_start = header_end + 1
        data_end = data_start + size
        if data_end >= len(stream) or stream[data_end:data_end + 1] != b"\n":
            raise RuntimeError(f"git cat-file --batch truncated blob {oid}")
        result.append((oid, paths.get(oid), stream[data_start:data_end]))
        offset = data_end + 1
    if offset != len(stream):
        raise RuntimeError("git cat-file --batch returned trailing data")
    return result


def inspect_history(root: Path) -> tuple[list[str], int]:
    blockers: list[str] = []
    blobs = history_blobs(root)
    for oid, rel, data in blobs:
        label = rel if rel is not None else f"blob {oid}"
        if rel is not None:
            blockers.extend(path_blockers(rel, "Git history"))
        kind = magic_kind(data)
        if kind is not None:
            blockers.append(f"{kind} magic in Git history: {label} ({oid})")
    return blockers, len(blobs)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--strict",
        action="store_true",
        help="treat warnings as release-blocking failures",
    )
    parser.add_argument(
        "--include-untracked",
        action="store_true",
        help="also inspect non-ignored untracked files before staging",
    )
    args = parser.parse_args()

    history_count = 0
    try:
        root = repository_root()
        paths = set(indexed_paths(root))
        if args.include_untracked:
            paths.update(untracked_paths(root))
        sorted_paths = sorted(paths)
        blockers = inspect_metadata(root)
        current_blockers, warnings = inspect_current(root, sorted_paths)
        blockers.extend(current_blockers)
        history_blockers, history_count = inspect_history(root)
        blockers.extend(history_blockers)
    except (OSError, RuntimeError, UnicodeError) as exc:
        print(f"release audit: ERROR: {exc}", file=sys.stderr)
        return 2

    blockers = sorted(set(blockers))
    warnings = sorted(set(warnings))
    for item in blockers:
        print(f"BLOCKER: {item}")
    for item in warnings:
        print(f"WARNING: {item}")

    counts = (
        f"{len(blockers)} blockers, {len(warnings)} warnings, "
        f"{len(sorted_paths)} current files, {history_count} historical blobs"
    )
    if blockers or (args.strict and warnings):
        print(f"release audit: NO-GO ({counts})")
        return 1

    print(f"release audit: GO ({counts})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
