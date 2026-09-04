#!/usr/bin/env python3
"""Fail closed on non-deterministic GNU ar metadata.

The ordered member census is read from the container itself.  Member names are
not identifiers: OpenAL deliberately contains two different ``null.c.obj``
members, so every row retains its ordinal, size, and payload digest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


AR_MAGIC = b"!<arch>\n"
HEADER_SIZE = 60
CONTRACT = "gnu-ar-deterministic-v1"
_DECIMAL_FIELD = re.compile(rb"^[0-9]+ *$")


class ArchiveError(RuntimeError):
    pass


def _sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _canonical(value: object) -> bytes:
    return (
        json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
        .encode("ascii")
        + b"\n"
    )


def _text_field(header: bytes, start: int, end: int, label: str) -> str:
    try:
        return header[start:end].decode("ascii").rstrip(" ")
    except UnicodeDecodeError as exc:
        raise ArchiveError(f"non-ASCII {label} field") from exc


def _decimal_field(header: bytes, start: int, end: int, label: str) -> int:
    field = header[start:end]
    if not _DECIMAL_FIELD.fullmatch(field):
        raise ArchiveError(f"malformed {label} field: {field!r}")
    return int(field.rstrip(b" "))


def _resolved_name(raw_name: str, long_names: bytes | None) -> str:
    if raw_name in ("/", "//"):
        return raw_name
    if raw_name.startswith("/") and raw_name[1:].isdigit():
        if long_names is None:
            raise ArchiveError("long-name reference precedes the GNU string table")
        offset = int(raw_name[1:])
        if offset >= len(long_names):
            raise ArchiveError(f"long-name offset is out of range: {offset}")
        end = long_names.find(b"/\n", offset)
        if end < 0:
            raise ArchiveError(f"unterminated long name at offset {offset}")
        try:
            return long_names[offset:end].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ArchiveError(f"non-UTF-8 long name at offset {offset}") from exc
    return raw_name[:-1] if raw_name.endswith("/") else raw_name


def inspect_archive(path: Path, expected_regular_members: int | None) -> dict[str, object]:
    try:
        archive = path.read_bytes()
    except OSError as exc:
        raise ArchiveError(f"cannot read archive {path}: {exc}") from exc
    if not path.is_file() or path.is_symlink():
        raise ArchiveError(f"archive is not a regular non-symlink file: {path}")
    if not archive.startswith(AR_MAGIC):
        raise ArchiveError("missing GNU ar global header")

    offset = len(AR_MAGIC)
    container_ordinal = 0
    regular_ordinal = 0
    symbol_tables = 0
    string_tables = 0
    long_names: bytes | None = None
    members: list[dict[str, object]] = []
    special_members: list[dict[str, object]] = []

    while offset < len(archive):
        if offset + HEADER_SIZE > len(archive):
            raise ArchiveError(f"truncated member header at offset {offset}")
        header = archive[offset : offset + HEADER_SIZE]
        if header[58:60] != b"`\n":
            raise ArchiveError(f"bad member trailer at ordinal {container_ordinal}")
        raw_name = _text_field(header, 0, 16, "name")
        mtime = _text_field(header, 16, 28, "mtime")
        uid = _text_field(header, 28, 34, "uid")
        gid = _text_field(header, 34, 40, "gid")
        mode = _text_field(header, 40, 48, "mode")
        size = _decimal_field(header, 48, 58, "size")
        body_start = offset + HEADER_SIZE
        body_end = body_start + size
        if body_end > len(archive):
            raise ArchiveError(f"truncated member payload at ordinal {container_ordinal}")
        body = archive[body_start:body_end]

        if raw_name == "//":
            string_tables += 1
            if string_tables != 1 or long_names is not None:
                raise ArchiveError("duplicate GNU string table")
            if mtime or uid or gid or mode:
                raise ArchiveError("GNU string table metadata is not canonical blank")
            long_names = body
        else:
            if mtime != "0":
                raise ArchiveError(
                    f"nonzero mtime at container ordinal {container_ordinal}: {mtime!r}"
                )
            if uid != "0" or gid != "0":
                raise ArchiveError(
                    f"nonzero owner metadata at container ordinal {container_ordinal}"
                )
            expected_mode = "0" if raw_name == "/" else "644"
            if mode != expected_mode:
                raise ArchiveError(
                    f"noncanonical mode at container ordinal {container_ordinal}: "
                    f"{mode!r} != {expected_mode!r}"
                )

        name = _resolved_name(raw_name, long_names)
        row = {
            "container_ordinal": container_ordinal,
            "name": name,
            "raw_name": raw_name,
            "mode": mode,
            "size": size,
            "sha256": _sha256(body),
        }
        if raw_name == "/":
            symbol_tables += 1
            special_members.append(row)
        elif raw_name == "//":
            special_members.append(row)
        else:
            row["ordinal"] = regular_ordinal
            members.append(row)
            regular_ordinal += 1

        next_offset = body_end + (size & 1)
        if size & 1:
            if body_end >= len(archive) or archive[body_end : body_end + 1] != b"\n":
                raise ArchiveError(f"bad odd-member padding at ordinal {container_ordinal}")
        offset = next_offset
        container_ordinal += 1

    if offset != len(archive):
        raise ArchiveError("archive parser did not consume the complete file")
    if symbol_tables != 1:
        raise ArchiveError(f"expected exactly one symbol table, found {symbol_tables}")
    special_topology = [
        [row["raw_name"], row["container_ordinal"]] for row in special_members
    ]
    if special_topology != [["/", 0], ["//", 1]]:
        raise ArchiveError(
            "expected leading GNU symbol/string tables at ordinals 0/1, "
            f"found {special_topology!r}"
        )
    if expected_regular_members is not None and len(members) != expected_regular_members:
        raise ArchiveError(
            f"regular-member count {len(members)} != {expected_regular_members}"
        )

    ordered_payload = _canonical(
        [
            [row["ordinal"], row["name"], row["size"], row["sha256"]]
            for row in members
        ]
    )
    return {
        "schema": 1,
        "stage": "isaac-openal-deterministic-archive",
        "result": "PASS",
        "contract": CONTRACT,
        "archive": {"size": len(archive), "sha256": _sha256(archive)},
        "container_entries": container_ordinal,
        "regular_members": len(members),
        "ordered_members_sha256": _sha256(ordered_payload),
        "members": members,
        "special_members": special_members,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=Path)
    parser.add_argument("--expected-regular-members", type=int)
    args = parser.parse_args(argv)
    if args.expected_regular_members is not None and args.expected_regular_members < 0:
        parser.error("--expected-regular-members must be nonnegative")
    try:
        result = inspect_archive(args.archive, args.expected_regular_members)
    except ArchiveError as exc:
        print(f"deterministic ar verification failed: {exc}", file=sys.stderr)
        return 1
    sys.stdout.buffer.write(_canonical(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
