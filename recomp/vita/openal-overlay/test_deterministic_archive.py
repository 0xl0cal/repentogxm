#!/usr/bin/env python3
"""Build the pinned OpenAL overlay twice and prove exact archive identity."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


SOURCE_ROOT = Path(__file__).resolve().parents[3]
BUILD_SCRIPT = Path(__file__).resolve().with_name("build.sh")
VERIFIER = Path(__file__).resolve().with_name("verify_deterministic_archive.py")
DOWNLOAD_NAMES = (
    "openal-soft-1.19.1.tar.gz",
    "openal-soft-1.19.1-vita-1.patch",
)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _record(path: Path) -> tuple[int, str, int]:
    stat_result = path.stat()
    return stat_result.st_size, _sha256(path), stat_result.st_mtime_ns


def _raw_archive_entries(payload: bytes) -> list[bytes]:
    if not payload.startswith(b"!<arch>\n"):
        raise RuntimeError("fixture is not a GNU ar archive")
    entries: list[bytes] = []
    offset = 8
    while offset < len(payload):
        header = payload[offset : offset + 60]
        if len(header) != 60 or header[58:60] != b"`\n":
            raise RuntimeError(f"malformed fixture header at offset {offset}")
        size = int(header[48:58])
        end = offset + 60 + size + (size & 1)
        if end > len(payload):
            raise RuntimeError(f"truncated fixture entry at offset {offset}")
        entries.append(payload[offset:end])
        offset = end
    return entries


def _run(
    command: list[str],
    *,
    cwd: Path,
    env: dict[str, str],
    log: Path,
    expect_success: bool = True,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    log.write_text(result.stdout, encoding="utf-8")
    if expect_success and result.returncode != 0:
        raise RuntimeError(
            f"command failed with RC {result.returncode}: {command!r}; log={log}"
        )
    if not expect_success and result.returncode == 0:
        raise RuntimeError(f"hostile command unexpectedly passed: {command!r}")
    return result


def _verify(
    archive: Path,
    expected_members: int,
    env: dict[str, str],
    log: Path,
) -> dict[str, object]:
    result = _run(
        [
            sys.executable,
            "-B",
            str(VERIFIER),
            "--expected-regular-members",
            str(expected_members),
            str(archive),
        ],
        cwd=SOURCE_ROOT,
        env=env,
        log=log,
    )
    return json.loads(result.stdout)


def _assert_generated_archive_rule(build: Path, vitasdk: Path) -> None:
    rules_path = build / "CMakeFiles" / "rules.ninja"
    rules = rules_path.read_text(encoding="utf-8")
    marker = "rule C_STATIC_LIBRARY_LINKER__OpenAL_Release\n"
    if rules.count(marker) != 1:
        raise RuntimeError(f"generated OpenAL archive rule count changed: {rules_path}")
    block = rules.split(marker, 1)[1].split("\n\n", 1)[0]
    ar = vitasdk / "bin" / "arm-vita-eabi-ar"
    ranlib = vitasdk / "bin" / "arm-vita-eabi-ranlib"
    expected = (
        "  command = $PRE_LINK && /usr/bin/cmake -E rm -f $TARGET_FILE && "
        f"{ar} qcD $TARGET_FILE $LINK_FLAGS $in && "
        f"{ranlib} -D $TARGET_FILE && $POST_BUILD"
    )
    commands = [line for line in block.splitlines() if line.startswith("  command = ")]
    if commands != [expected]:
        raise RuntimeError(f"generated deterministic archive rule changed: {commands!r}")

    append = (
        "CMAKE_C_ARCHIVE_APPEND:STRING="
        "<CMAKE_AR> qD <TARGET> <LINK_FLAGS> <OBJECTS>"
    )
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines()
    if cache.count(append) != 1:
        raise RuntimeError("configured deterministic archive APPEND rule changed")


def _seed_downloads(root: Path, seed: Path | None) -> None:
    if seed is None:
        return
    downloads = root / "downloads"
    downloads.mkdir(parents=True)
    for name in DOWNLOAD_NAMES:
        source = seed / name
        if not source.is_file() or source.is_symlink():
            raise RuntimeError(f"download seed is missing a regular {name}: {source}")
        shutil.copyfile(source, downloads / name)


def _recipe_build(root: Path) -> tuple[str, Path]:
    recipe = (root / "recipe.txt").read_text(encoding="ascii").strip()
    if not re.fullmatch(r"[0-9a-f]{64}", recipe):
        raise RuntimeError(f"malformed recipe ID in {root}: {recipe!r}")
    build = root / "work" / f"build-{recipe}"
    if not build.is_dir():
        raise RuntimeError(f"recipe build directory is absent: {build}")
    return recipe, build


def _assert_no_work(
    ninja: str,
    build: Path,
    *,
    env: dict[str, str],
    log: Path,
) -> None:
    result = _run(
        [ninja, "-C", str(build), "-n", "libopenal.a"],
        cwd=SOURCE_ROOT,
        env=env,
        log=log,
    )
    if result.stdout.count("ninja: no work to do.") != 1:
        raise RuntimeError(f"OpenAL archive is not at a no-work fixed point: {log}")
    if re.search(r"^\[[0-9]+/[0-9]+\]", result.stdout, re.MULTILINE):
        raise RuntimeError(f"OpenAL dry run retained build actions: {log}")


def _openal_objects(build: Path) -> dict[str, Path]:
    base = build / "CMakeFiles" / "OpenAL.dir"
    objects = {
        path.relative_to(base).as_posix(): path
        for path in base.rglob("*.obj")
        if path.is_file() and not path.is_symlink()
    }
    if len(objects) != 53:
        raise RuntimeError(f"expected 53 OpenAL objects in {build}, found {len(objects)}")
    return objects


def _exercise_append_rule(
    base: Path,
    build: Path,
    vitasdk: Path,
    *,
    env: dict[str, str],
    logs: Path,
) -> None:
    objects = sorted(_openal_objects(build).values())[:2]
    probe = base / "append-rule-probe"
    probe.mkdir()
    names = (
        "first-append-probe-long-name.obj",
        "second-append-probe-long-name.obj",
    )
    for source, name in zip(objects, names, strict=True):
        shutil.copyfile(source, probe / name)
    archive = probe / "append-rule-probe.a"
    ar = vitasdk / "bin" / "arm-vita-eabi-ar"
    ranlib = vitasdk / "bin" / "arm-vita-eabi-ranlib"
    _run(
        [str(ar), "qcD", str(archive), names[0]],
        cwd=probe,
        env=env,
        log=logs / "append-create.log",
    )
    _run(
        [str(ar), "qD", str(archive), names[1]],
        cwd=probe,
        env=env,
        log=logs / "append-append.log",
    )
    _run(
        [str(ranlib), "-D", str(archive)],
        cwd=probe,
        env=env,
        log=logs / "append-finish.log",
    )
    receipt = _verify(archive, 2, env, logs / "append-verify.log")
    if receipt["container_entries"] != 4:
        raise RuntimeError("APPEND probe did not retain exact /,//,2 topology")
    if [
        [row["raw_name"], row["container_ordinal"]]
        for row in receipt["special_members"]
    ] != [["/", 0], ["//", 1]]:
        raise RuntimeError("APPEND probe special-table topology changed")
    if [row["name"] for row in receipt["members"]] != list(names):
        raise RuntimeError("APPEND probe member order changed")


def _exercise(base: Path, args: argparse.Namespace) -> None:
    logs = base / "logs"
    logs.mkdir()
    root_a = base / "A"
    root_b = base / "B-relocated-path-significantly-longer"
    root_a.mkdir()
    root_b.mkdir()
    if len(str(root_a)) == len(str(root_b)):
        raise RuntimeError("two-root fixture paths accidentally have equal length")
    _seed_downloads(root_a, args.download_seed)
    _seed_downloads(root_b, args.download_seed)

    env = dict(os.environ)
    env.update(
        {
            "VITASDK": str(args.vitasdk),
            "ISAAC_OPENAL_JOBS": str(args.jobs),
            "LC_ALL": "C",
            "TZ": "UTC",
            "PYTHONHASHSEED": "0",
            "PYTHONDONTWRITEBYTECODE": "1",
            "SOURCE_DATE_EPOCH": "1700000000",
        }
    )
    _run(
        ["bash", str(BUILD_SCRIPT), str(root_a)],
        cwd=SOURCE_ROOT,
        env=env,
        log=logs / "A-build.log",
    )
    _run(
        ["bash", str(BUILD_SCRIPT), str(root_b)],
        cwd=SOURCE_ROOT,
        env=env,
        log=logs / "B-build.log",
    )

    recipe_a, build_a = _recipe_build(root_a)
    recipe_b, build_b = _recipe_build(root_b)
    if recipe_a != recipe_b:
        raise RuntimeError(f"two-root recipe IDs differ: {recipe_a} != {recipe_b}")
    library_a = root_a / "prefix" / "lib" / "libopenal.a"
    library_b = root_b / "prefix" / "lib" / "libopenal.a"
    if library_a.read_bytes() != library_b.read_bytes():
        raise RuntimeError("fresh two-root OpenAL archives are not byte-identical")

    _assert_generated_archive_rule(build_a, args.vitasdk)
    _assert_generated_archive_rule(build_b, args.vitasdk)
    receipt_a = _verify(library_a, 53, env, logs / "A-verify.log")
    receipt_b = _verify(library_b, 53, env, logs / "B-verify.log")
    if receipt_a != receipt_b:
        raise RuntimeError("fresh two-root deterministic archive receipts differ")
    if (
        receipt_a["container_entries"] != 55
        or receipt_a["regular_members"] != 53
        or [
            [row["raw_name"], row["container_ordinal"]]
            for row in receipt_a["special_members"]
        ] != [["/", 0], ["//", 1]]
        or [row["mode"] for row in receipt_a["special_members"]] != ["0", ""]
        or {row["mode"] for row in receipt_a["members"]} != {"644"}
    ):
        raise RuntimeError("OpenAL /,//,53 topology or canonical modes changed")
    for root, receipt in ((root_a, receipt_a), (root_b, receipt_b)):
        produced = json.loads(
            (root / "libopenal.archive.json").read_text(encoding="ascii")
        )
        if produced != receipt:
            raise RuntimeError(f"build-time archive receipt is stale: {root}")
        checksum = (root / "libopenal.sha256").read_text(encoding="ascii").split()
        if not checksum or checksum[0] != receipt["archive"]["sha256"]:
            raise RuntimeError(f"archive checksum is stale: {root}")

    null_members = [
        row for row in receipt_a["members"] if row["name"] == "null.c.obj"
    ]
    if (
        len(null_members) != 2
        or null_members[0]["ordinal"] == null_members[1]["ordinal"]
        or null_members[0]["sha256"] == null_members[1]["sha256"]
    ):
        raise RuntimeError("ordinal-based duplicate null.c.obj proof changed")

    objects_a = _openal_objects(build_a)
    objects_b = _openal_objects(build_b)
    if set(objects_a) != set(objects_b):
        raise RuntimeError("fresh two-root OpenAL object sets differ")
    if not any(
        objects_a[name].stat().st_mtime_ns != objects_b[name].stat().st_mtime_ns
        for name in objects_a
    ):
        raise RuntimeError("fresh two-root fixture did not vary physical object mtimes")

    _exercise_append_rule(
        base,
        build_a,
        args.vitasdk,
        env=env,
        logs=logs,
    )

    ninja = shutil.which("ninja")
    if ninja is None:
        raise RuntimeError("ninja is unavailable")
    _assert_no_work(ninja, build_a, env=env, log=logs / "A-no-work-before.log")
    _assert_no_work(ninja, build_b, env=env, log=logs / "B-no-work.log")
    before = _record(library_a)
    _run(
        ["bash", str(BUILD_SCRIPT), str(root_a)],
        cwd=SOURCE_ROOT,
        env=env,
        log=logs / "A-rerun.log",
    )
    after = _record(library_a)
    if before != after:
        raise RuntimeError(f"no-work rerun changed archive record: {before} != {after}")
    _assert_no_work(ninja, build_a, env=env, log=logs / "A-no-work-after.log")
    if library_a.read_bytes() != library_b.read_bytes():
        raise RuntimeError("no-work rerun broke two-root archive identity")

    tampered = base / "hostile-nonzero-mtime.a"
    shutil.copyfile(library_a, tampered)
    payload = bytearray(tampered.read_bytes())
    mtime_start = 8 + 16
    if payload[mtime_start : mtime_start + 12] != b"0           ":
        raise RuntimeError("unexpected symbol-table mtime before hostile mutation")
    payload[mtime_start : mtime_start + 12] = b"1           "
    tampered.write_bytes(payload)
    hostile = _run(
        [sys.executable, "-B", str(VERIFIER), str(tampered)],
        cwd=SOURCE_ROOT,
        env=env,
        log=logs / "hostile-nonzero-mtime.log",
        expect_success=False,
    )
    if "nonzero mtime at container ordinal 0" not in hostile.stdout:
        raise RuntimeError("hostile nonzero-mtime rejection lost its exact reason")

    reordered = base / "hostile-special-tables-not-leading.a"
    source_payload = library_a.read_bytes()
    entries = _raw_archive_entries(source_payload)
    if len(entries) != 55:
        raise RuntimeError("hostile reorder fixture lost exact 55-entry source")
    reordered.write_bytes(b"!<arch>\n" + b"".join([entries[2], *entries[:2], *entries[3:]]))
    hostile_order = _run(
        [
            sys.executable,
            "-B",
            str(VERIFIER),
            "--expected-regular-members",
            "53",
            str(reordered),
        ],
        cwd=SOURCE_ROOT,
        env=env,
        log=logs / "hostile-special-tables-not-leading.log",
        expect_success=False,
    )
    if "expected leading GNU symbol/string tables at ordinals 0/1" not in hostile_order.stdout:
        raise RuntimeError("hostile special-table ordinal rejection lost its reason")

    print(
        "OpenAL deterministic archive: PASS "
        f"(recipe={recipe_a}; archive={receipt_a['archive']['sha256']}; "
        f"ordered_members={receipt_a['ordered_members_sha256']}; "
        "fresh two-root exact; duplicate-name ordinal proof; hostile mtime RED; "
        "generated qcD/ranlib-D; executed qD; /,//,53 modes; hostile order RED; "
        "no-work fixed point)"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--vitasdk",
        type=Path,
        default=Path(os.environ["VITASDK"]) if os.environ.get("VITASDK") else None,
    )
    parser.add_argument("--download-seed", type=Path)
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    args = parser.parse_args(argv)
    if args.vitasdk is None:
        parser.error("--vitasdk or VITASDK is required")
    args.vitasdk = args.vitasdk.resolve()
    if not args.vitasdk.is_dir():
        parser.error(f"VitaSDK directory is absent: {args.vitasdk}")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.download_seed is not None:
        args.download_seed = args.download_seed.resolve()
        if not args.download_seed.is_dir():
            parser.error(f"download seed directory is absent: {args.download_seed}")

    if args.output_root is not None:
        output_root = args.output_root.resolve()
        if output_root.exists():
            parser.error(f"--output-root must be absent: {output_root}")
        output_root.mkdir(parents=True)
        _exercise(output_root, args)
    else:
        with tempfile.TemporaryDirectory(prefix="isaac-openal-determinism-") as temp:
            _exercise(Path(temp), args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
