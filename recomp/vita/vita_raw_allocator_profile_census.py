#!/usr/bin/env python3
"""Reproduce the 88-configuration Vita heap raw-relocation census."""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import hashlib
import itertools
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
from typing import Mapping, Sequence

import vita_raw_allocator_gate as gate


SCHEMA = 1
STAGE = "vita-raw-allocator-profile-census"
EXPECTED_CONFIGURATION_COUNT = 88
EXPECTED_DISTINCT_PROFILE_COUNT = 7
ANCILLARY_FEATURES = (
    "ISAAC_VITA_ANM2_SCRATCH",
    "ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC",
    "ISAAC_VITA_TEXEL_SCRATCH",
)
FIXED_ON_FEATURES = ("ISAAC_VITA_OGG_EMERGENCY",)
OVERFLOW_OFF_FEATURES = (
    "ISAAC_VITA_HEAP_LEDGER_MEMBLOCK",
    "ISAAC_VITA_HEAP_LEDGER_BACKSHIFT",
    "ISAAC_VITA_HEAP_RANGE_LEASE",
)
ROOM_TIERS = ((), ("ISAAC_VITA_ROOM_ENTRY_SLAB",),
              ("ISAAC_VITA_ROOM_ENTRY_SLAB",
               "ISAAC_VITA_ROOM_ENTRY_HYBRID"))


class CensusError(RuntimeError):
    pass


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("ascii")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _file_record(path: Path) -> dict[str, object]:
    path = path.resolve()
    if not path.is_file():
        raise CensusError(f"required regular file is missing: {path}")
    return {"size": path.stat().st_size, "sha256": _sha256(path)}


def _tool_record(path: Path) -> dict[str, object]:
    record = _file_record(path)
    try:
        completed = subprocess.run(
            [str(path), "--version"], check=False,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise CensusError(f"cannot execute tool {path}: {exc}") from exc
    if completed.returncode != 0:
        raise CensusError(f"tool version query failed: {path}")
    lines = [
        line.strip() for line in
        (completed.stdout + completed.stderr).decode(
            "utf-8", "replace"
        ).splitlines() if line.strip()
    ]
    if not lines:
        raise CensusError(f"tool returned no version identity: {path}")
    return {"name": path.name, "version": lines[0], **record}


def _resolve(value: str, directory: Path) -> Path:
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = directory / path
    return path.resolve()


def _definition_name(value: str) -> str:
    return value.partition("=")[0]


def _without_profile_definitions(tokens: Sequence[str]) -> list[str]:
    result: list[str] = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token == "-D":
            if index + 1 >= len(tokens):
                raise CensusError("compile command ends after -D")
            definition = tokens[index + 1]
            if _definition_name(definition) in gate.HEAP_CANONICAL_DEFINES:
                index += 2
                continue
            result.extend((token, definition))
            index += 2
            continue
        if token.startswith("-D") and len(token) > 2:
            if _definition_name(token[2:]) in gate.HEAP_CANONICAL_DEFINES:
                index += 1
                continue
        result.append(token)
        index += 1
    return result


def _without_dependency_flags(tokens: Sequence[str]) -> list[str]:
    result: list[str] = []
    no_argument = {"-MD", "-MMD", "-MP", "-MG"}
    with_argument = ("-MF", "-MT", "-MQ")
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token in no_argument:
            index += 1
            continue
        if token in with_argument:
            if index + 1 >= len(tokens):
                raise CensusError(f"compile command ends after {token}")
            index += 2
            continue
        joined = next(
            (prefix for prefix in with_argument
             if token.startswith(prefix) and len(token) > len(prefix)),
            None,
        )
        if joined is not None:
            index += 1
            continue
        if token in ("-M", "-MM") or token.startswith("-Wp,-M"):
            raise CensusError(
                f"unsupported dependency-only compiler option: {token}"
            )
        result.append(token)
        index += 1
    return result


def _replace_output(tokens: list[str], output: Path) -> list[str]:
    result = list(tokens)
    positions = [index for index, token in enumerate(result) if token == "-o"]
    if len(positions) != 1 or positions[0] + 1 >= len(result):
        raise CensusError("heap compile command does not have one -o argument")
    result[positions[0] + 1] = str(output)
    index = 0
    while index < len(result):
        if result[index] == "-MF":
            if index + 1 >= len(result):
                raise CensusError("compile command ends after -MF")
            result[index + 1] = str(output.with_suffix(".d"))
            index += 2
            continue
        if result[index].startswith("-MF") and len(result[index]) > 3:
            result[index] = "-MF" + str(output.with_suffix(".d"))
        index += 1
    return result


def _parse_depfile_text(text: str) -> list[str]:
    joined = text.replace("\\\r\n", "").replace("\\\n", "")
    rules = [line.strip() for line in joined.splitlines() if line.strip()]
    if len(rules) != 1 or ":" not in rules[0]:
        raise CensusError(
            f"compiler depfile does not contain one rule: {len(rules)}"
        )
    target, dependencies = rules[0].split(":", 1)
    if not target.strip():
        raise CensusError("compiler depfile has an empty target")
    try:
        paths = shlex.split(dependencies, comments=False, posix=True)
    except ValueError as exc:
        raise CensusError(f"cannot parse compiler depfile: {exc}") from exc
    if not paths:
        raise CensusError("compiler depfile has no dependencies")
    return paths


def _dependency_records(
    depfile: Path, directory: Path,
) -> dict[Path, dict[str, object]]:
    try:
        paths = _parse_depfile_text(depfile.read_text(encoding="utf-8"))
    except (OSError, UnicodeError) as exc:
        raise CensusError(f"cannot read compiler depfile {depfile}: {exc}") from exc
    result: dict[Path, dict[str, object]] = {}
    for value in paths:
        path = _resolve(value, directory)
        if path in result:
            continue
        result[path] = _file_record(path)
    return result


def _logical_dependency(
    path: Path, *, repo_root: Path, sdk_root: Path, build_root: Path,
) -> str:
    for label, root in (
        ("repo", repo_root), ("vitasdk", sdk_root), ("build", build_root),
    ):
        try:
            return label + "/" + path.relative_to(root).as_posix()
        except ValueError:
            pass
    return "absolute/" + path.as_posix().lstrip("/")


def _relocation_rows(
    relocations: Mapping[gate.RelocKey, int],
) -> list[dict[str, object]]:
    return [
        {"function": function, "symbol": symbol, "type": kind,
         "count": count}
        for (function, symbol, kind), count in sorted(relocations.items())
    ]


def configurations() -> list[tuple[str, ...]]:
    result: list[tuple[str, ...]] = []
    for ancillary_values in itertools.product(
        (False, True), repeat=len(ANCILLARY_FEATURES)
    ):
        common = set(FIXED_ON_FEATURES)
        common.update(
            feature for feature, active in
            zip(ANCILLARY_FEATURES, ancillary_values) if active
        )
        for core_values in itertools.product(
            (False, True), repeat=len(OVERFLOW_OFF_FEATURES)
        ):
            active = set(common)
            active.update(
                feature for feature, enabled in
                zip(OVERFLOW_OFF_FEATURES, core_values) if enabled
            )
            result.append(tuple(sorted(active)))
        overflow = {
            "ISAAC_VITA_HEAP_LEDGER_MEMBLOCK",
            "ISAAC_VITA_HEAP_LEDGER_BACKSHIFT",
            "ISAAC_VITA_HEAP_OVERFLOW_MSPACE",
            "ISAAC_VITA_HEAP_RANGE_LEASE",
        }
        for room_tier in ROOM_TIERS:
            result.append(tuple(sorted(common | overflow | set(room_tier))))
    if (len(result) != EXPECTED_CONFIGURATION_COUNT or
            len(result) != len(set(result))):
        raise CensusError(
            f"configuration enumeration changed: {len(result)} records"
        )
    return result


def _compile_one(
    *, index: int, active: tuple[str, ...], base_tokens: Sequence[str],
    directory: Path, source: Path, compiler: Path, readelf: Path,
    work: Path,
) -> tuple[
    int,
    dict[str, object],
    collections.Counter[gate.RelocKey],
    dict[Path, dict[str, object]],
]:
    object_path = work / f"heap-{index:02d}.o"
    depfile = work / f"heap-{index:02d}.d"
    command = _replace_output(
        [base_tokens[0], *(f"-D{name}=1" for name in active),
         *base_tokens[1:]],
        object_path,
    )
    command.extend(("-MD", "-MF", str(depfile), "-MT", str(object_path)))
    try:
        completed = subprocess.run(
            command, cwd=directory, check=False,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise CensusError(f"cannot execute compiler for config {index}: {exc}") from exc
    if completed.returncode != 0:
        detail = (completed.stdout + completed.stderr).decode(
            "utf-8", "replace"
        ).strip()
        raise CensusError(f"heap config {index} compile failed: {detail}")
    relocations = gate.object_raw_relocations(readelf, object_path)
    record = {
        "source": source,
        "exempt": True,
        "definitions": gate._definitions(command),
    }
    profile = gate.heap_profile(record)
    gate.verify_record_relocations(record, relocations, profile)
    dependencies = _dependency_records(depfile, directory)
    if source not in dependencies:
        raise CensusError(
            f"heap config {index} depfile omitted its source: {source}"
        )
    active_record = list(active)
    config_id = hashlib.sha256(_canonical_bytes(active_record)).hexdigest()
    return index, {
        "index": index,
        "configuration_id": config_id,
        "active_defines": active_record,
        "profile": profile,
        "object": _file_record(object_path),
        "raw_relocations": _relocation_rows(relocations),
    }, relocations, dependencies


def run_census(args: argparse.Namespace) -> dict[str, object]:
    compile_commands = args.compile_commands.resolve()
    readelf = args.readelf.resolve()
    receipt = args.receipt.resolve()
    if not readelf.is_file():
        raise CensusError(f"readelf is not a regular file: {readelf}")
    try:
        entries = json.loads(compile_commands.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as exc:
        raise CensusError(f"cannot read compile_commands.json: {exc}") from exc
    if not isinstance(entries, list):
        raise CensusError("compile_commands.json root is not a list")
    heap_entries = [
        entry for entry in entries
        if isinstance(entry, dict) and
        isinstance(entry.get("file"), str) and
        Path(entry["file"]).name == "host_vita_heap.c" and
        isinstance(entry.get("output"), str) and
        entry["output"].replace("\\", "/").startswith(
            f"CMakeFiles/{gate.TARGET}.dir/"
        )
    ]
    if len(heap_entries) != 1:
        raise CensusError(
            f"expected one target heap compile record, found {len(heap_entries)}"
        )
    entry = heap_entries[0]
    directory_value = entry.get("directory")
    if not isinstance(directory_value, str):
        raise CensusError("heap compile record has no directory")
    directory = Path(directory_value).resolve()
    tokens = gate._command_tokens(entry)
    if not tokens:
        raise CensusError("heap compile command is empty")
    compiler = _resolve(tokens[0], directory)
    source = _resolve(entry["file"], directory)
    if not compiler.is_file() or not source.is_file():
        raise CensusError("heap compiler/source input is missing")
    definitions = gate._definitions(tokens)
    if definitions.get("ISAAC_VITA_RAW_ALLOCATOR_GATE") != ["1"] or \
            definitions.get("ISAAC_VITA_RAW_ALLOCATOR_EXEMPT") != ["1"]:
        raise CensusError("heap compile record is not the exact exempt gate owner")
    poison_headers = gate._forced_includes(tokens, directory)
    if len(poison_headers) != 1 or not poison_headers[0].is_file():
        raise CensusError("heap compile record has no exact poison header")
    base_tokens = _without_dependency_flags(
        _without_profile_definitions(tokens)
    )
    base_tokens[0] = str(compiler)
    config_list = configurations()
    rows: list[dict[str, object] | None] = [None] * len(config_list)
    raw_results: list[collections.Counter[gate.RelocKey] | None] = (
        [None] * len(config_list)
    )
    dependency_results: list[
        dict[Path, dict[str, object]] | None
    ] = [None] * len(config_list)
    receipt.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="isaac-vita-raw-profile-census-", dir=receipt.parent
    ) as temporary:
        work = Path(temporary)
        with concurrent.futures.ThreadPoolExecutor(
            max_workers=args.jobs
        ) as executor:
            futures = [
                executor.submit(
                    _compile_one, index=index, active=active,
                    base_tokens=base_tokens, directory=directory,
                    source=source, compiler=compiler, readelf=readelf,
                    work=work,
                )
                for index, active in enumerate(config_list)
            ]
            for future in concurrent.futures.as_completed(futures):
                index, row, relocations, dependencies = future.result()
                rows[index] = row
                raw_results[index] = relocations
                dependency_results[index] = dependencies
    if any(row is None for row in rows) or any(
        item is None for item in raw_results
    ) or any(item is None for item in dependency_results):
        raise CensusError("parallel census lost a configuration result")
    final_rows = [row for row in rows if row is not None]
    final_raw = [item for item in raw_results if item is not None]
    final_dependencies = [
        item for item in dependency_results if item is not None
    ]
    canonical_rows = [
        row for row in final_rows
        if row["profile"] == gate.PROFILE_CANONICAL
    ]
    if len(canonical_rows) != 1:
        raise CensusError(
            f"expected one canonical configuration, found {len(canonical_rows)}"
        )

    repo_root = Path(__file__).resolve().parents[2]
    sdk_root = compiler.parent.parent.resolve()
    build_root = compile_commands.parent.resolve()
    dependency_paths: dict[str, Path] = {}
    dependency_union: dict[str, dict[str, object]] = {}
    dependency_groups: dict[str, dict[str, object]] = {}
    poison_header = poison_headers[0].resolve()
    for row, dependencies in zip(final_rows, final_dependencies):
        if poison_header not in dependencies:
            raise CensusError(
                f"heap config {row['index']} depfile omitted the poison header"
            )
        logical_paths: list[str] = []
        for path, record in sorted(
            dependencies.items(), key=lambda item: item[0].as_posix()
        ):
            logical = _logical_dependency(
                path, repo_root=repo_root, sdk_root=sdk_root,
                build_root=build_root,
            )
            previous_path = dependency_paths.setdefault(logical, path)
            if previous_path != path:
                raise CensusError(
                    f"dependency logical path collision: {logical}: "
                    f"{previous_path} != {path}"
                )
            previous_record = dependency_union.setdefault(logical, record)
            if previous_record != record:
                raise CensusError(
                    f"dependency changed during census: {logical}: "
                    f"{previous_record} != {record}"
                )
            logical_paths.append(logical)
        dependency_set_id = hashlib.sha256(
            _canonical_bytes(logical_paths)
        ).hexdigest()
        row["dependency_set_sha256"] = dependency_set_id
        group = dependency_groups.setdefault(dependency_set_id, {
            "dependency_set_sha256": dependency_set_id,
            "configuration_ids": [],
            "paths": logical_paths,
        })
        if group["paths"] != logical_paths:
            raise CensusError(
                f"dependency-set hash collision: {dependency_set_id}"
            )
        group["configuration_ids"].append(row["configuration_id"])
    for logical, path in dependency_paths.items():
        if _file_record(path) != dependency_union[logical]:
            raise CensusError(
                f"dependency changed after compilation: {logical}"
            )

    observed_union: collections.Counter[gate.RelocKey] = collections.Counter()
    profile_groups: dict[str, dict[str, object]] = {}
    max_total = 0
    for row, raw in zip(final_rows, final_raw):
        normalised = gate._normalise_heap_relocations(raw)
        for key, count in normalised.items():
            observed_union[key] = max(observed_union[key], count)
        max_total = max(max_total, sum(raw.values()))
        relocation_rows = row["raw_relocations"]
        profile_id = hashlib.sha256(
            _canonical_bytes(relocation_rows)
        ).hexdigest()
        group = profile_groups.setdefault(profile_id, {
            "profile_id": profile_id,
            "configuration_ids": [],
            "raw_relocations": relocation_rows,
        })
        group["configuration_ids"].append(row["configuration_id"])
    if len(profile_groups) != EXPECTED_DISTINCT_PROFILE_COUNT:
        raise CensusError(
            f"distinct relocation profiles changed: {len(profile_groups)}"
        )
    if observed_union != gate.HEAP_DIAGNOSTIC_MAX:
        raise CensusError(
            "observed relocation union differs from the diagnostic allowlist: "
            f"missing={sorted((gate.HEAP_DIAGNOSTIC_MAX - observed_union).items())}, "
            f"extra={sorted((observed_union - gate.HEAP_DIAGNOSTIC_MAX).items())}"
        )
    if max_total != gate.HEAP_DIAGNOSTIC_TOTAL_MAX:
        raise CensusError(
            f"maximum raw relocation total changed: {max_total} != "
            f"{gate.HEAP_DIAGNOSTIC_TOTAL_MAX}"
        )
    normalised_command = list(base_tokens)
    normalised_command[0] = "<COMPILER>"
    normalised_command = _replace_output(normalised_command, Path("<OBJECT>"))
    result: dict[str, object] = {
        "schema": SCHEMA,
        "stage": STAGE,
        "result": "PASS",
        "configuration_count": len(final_rows),
        "distinct_profile_count": len(profile_groups),
        "dependency_set_count": len(dependency_groups),
        "maximum_raw_relocation_total": max_total,
        "enumeration": {
            "feature_universe": sorted(gate.HEAP_CANONICAL_DEFINES),
            "fixed_on": list(FIXED_ON_FEATURES),
            "binary_ancillary": list(ANCILLARY_FEATURES),
            "overflow_off_binary": list(OVERFLOW_OFF_FEATURES),
            "overflow_off_forced_off": [
                "ISAAC_VITA_HEAP_OVERFLOW_MSPACE",
                "ISAAC_VITA_ROOM_ENTRY_HYBRID",
                "ISAAC_VITA_ROOM_ENTRY_SLAB",
            ],
            "overflow_on_forced_on": [
                "ISAAC_VITA_HEAP_LEDGER_BACKSHIFT",
                "ISAAC_VITA_HEAP_LEDGER_MEMBLOCK",
                "ISAAC_VITA_HEAP_OVERFLOW_MSPACE",
                "ISAAC_VITA_HEAP_RANGE_LEASE",
            ],
            "overflow_on_room_tiers": [list(item) for item in ROOM_TIERS],
        },
        "inputs": {
            "census": _file_record(Path(__file__)),
            "compile_commands": _file_record(compile_commands),
            "gate": _file_record(Path(gate.__file__)),
            "heap_source": _file_record(source),
            "poison_header": _file_record(poison_headers[0]),
        },
        "toolchain": {
            "compiler": _tool_record(compiler),
            "readelf": _tool_record(readelf),
        },
        "base_compile_arguments": normalised_command,
        "base_compile_sha256": hashlib.sha256(
            _canonical_bytes(normalised_command)
        ).hexdigest(),
        "dependency_compile_arguments": [
            "-MD", "-MF", "<DEPFILE>", "-MT", "<OBJECT>",
        ],
        "dependency_union": [
            {"path": logical, **record}
            for logical, record in sorted(dependency_union.items())
        ],
        "dependency_sets": [
            {
                **group,
                "configuration_count": len(group["configuration_ids"]),
            }
            for unused_id, group in sorted(dependency_groups.items())
        ],
        "observed_union_max": _relocation_rows(observed_union),
        "profiles": [
            {
                **group,
                "configuration_count": len(group["configuration_ids"]),
            }
            for unused_id, group in sorted(profile_groups.items())
        ],
        "configurations": final_rows,
    }
    temporary_receipt = receipt.with_name(receipt.name + ".tmp")
    if temporary_receipt.exists():
        temporary_receipt.unlink()
    temporary_receipt.write_bytes(_canonical_bytes(result) + b"\n")
    os.replace(temporary_receipt, receipt)
    return result


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compile-commands", required=True, type=Path)
    parser.add_argument("--readelf", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=4)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if not 1 <= args.jobs <= 16:
        print("Vita raw allocator profile census: FAIL: --jobs must be 1..16",
              file=sys.stderr)
        return 2
    try:
        result = run_census(args)
    except (CensusError, gate.GateError) as exc:
        print(f"Vita raw allocator profile census: FAIL: {exc}", file=sys.stderr)
        return 1
    receipt = args.receipt.resolve()
    print(
        "Vita raw allocator profile census: PASS; "
        f"configs={result['configuration_count']} "
        f"profiles={result['distinct_profile_count']} "
        f"receipt_sha256={_sha256(receipt)} receipt={receipt}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
