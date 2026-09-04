#!/usr/bin/env python3
"""Apply every supported vitaGL patch chain to the exact pinned stock source."""

from __future__ import annotations

import argparse
import hashlib
import io
import re
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path

SOURCE_COMMIT = "73dd57a8857f89f2353881c6de5891959c5c1983"
SOURCE_ARCHIVE_SHA256 = (
    "f484dd9d2aec707ac5f91352c6e0631239f2330fe6769e8476cfe425331c400a"
)
SOURCE_ROOT = f"vitaGL-{SOURCE_COMMIT}"
EXPECTED_HASHES = {
    "0:0": (
        "51377c7a9f75a27a093c86ba3106314fb3a08dff824fe6228ebbd796a106c9ed",
        "a0c2141c568048797a9431c3f3947d1975a013448d4327edb53316894d990a4e",
    ),
    "1:0": (
        "1a8285ee3377855359268306d65a879f9697e2235e337aa9be4f496108da14d5",
        "4ed165659b6909b4a329e974b78ff48cab1d777dcd638a3dc066128dc1260bcc",
    ),
    "1:1": (
        "6a9ca09632ab40e86af8d6752789bb75c6c0a10ebd03174339264e306c78f70e",
        "59e81f903cf6e6a0f5fd8f5b8785bea3b3d5ee65d5f1724f3e54d0808e6f0224",
    ),
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command: list[str], *, cwd: Path | None = None) -> bytes:
    return subprocess.run(
        command, cwd=cwd, check=True, stdout=subprocess.PIPE
    ).stdout


def safe_extract(bundle: tarfile.TarFile, destination: Path) -> None:
    resolved_destination = destination.resolve()
    for member in bundle.getmembers():
        target = (destination / member.name).resolve()
        if target != resolved_destination and resolved_destination not in target.parents:
            raise AssertionError(f"unsafe stock archive member: {member.name}")
        if member.name != SOURCE_ROOT and not member.name.startswith(SOURCE_ROOT + "/"):
            raise AssertionError(f"unexpected stock archive root: {member.name}")
    bundle.extractall(destination)


def stock_archive_from_repo(repository: Path) -> bytes:
    if not repository.is_dir():
        raise AssertionError(f"stock repository is not a directory: {repository}")
    resolved = run(
        [
            "git",
            "-c",
            "core.autocrlf=false",
            "-c",
            "core.eol=lf",
            "-C",
            str(repository),
            "rev-parse",
            "--verify",
            f"{SOURCE_COMMIT}^{{commit}}",
        ]
    ).decode("ascii").strip()
    if resolved != SOURCE_COMMIT:
        raise AssertionError(
            f"stock repository resolved {resolved}, expected {SOURCE_COMMIT}"
        )
    return run(
        [
            "git",
            "-c",
            "core.autocrlf=false",
            "-c",
            "core.eol=lf",
            "-C",
            str(repository),
            "archive",
            "--format=tar",
            f"--prefix={SOURCE_ROOT}/",
            SOURCE_COMMIT,
        ]
    )


def materialize_stock(
    destination: Path, stock_tar: Path | None, repository_archive: bytes | None
) -> Path:
    destination.mkdir()
    if stock_tar is not None:
        with tarfile.open(stock_tar, mode="r:*") as bundle:
            safe_extract(bundle, destination)
    else:
        assert repository_archive is not None
        with tarfile.open(fileobj=io.BytesIO(repository_archive), mode="r:") as bundle:
            safe_extract(bundle, destination)
    source = destination / SOURCE_ROOT
    for required in (
        source / ".gitattributes",
        source / "source" / "custom_shaders.c",
        source / "source" / "vitaGL.h",
    ):
        if not required.is_file():
            raise AssertionError(f"stock source is incomplete: {required}")
    return source


def verify_build_contract(recipe: Path) -> None:
    build = (recipe / "build.sh").read_text(encoding="utf-8")
    matrix_pattern = re.compile(
        r"(?m)^    (0:0|1:0|1:1)\)\n"
        r"        shader_source_sha=([0-9a-f]{64})\n"
        r"        shader_header_sha=([0-9a-f]{64})$"
    )
    actual = {
        name: (source_hash, header_hash)
        for name, source_hash, header_hash in matrix_pattern.findall(build)
    }
    if actual != EXPECTED_HASHES:
        raise AssertionError(
            f"build.sh shader hash matrix drifted: {actual} != {EXPECTED_HASHES}"
        )
    if build.count("git -c core.autocrlf=false -c core.eol=lf") != 2:
        raise AssertionError("build.sh lost the canonical-LF git apply helper")
    for patch_name in ("contract", "gpu_draw", "coloroffset", "shader_cache",
                       "scene_timer"):
        needle = f'apply_source_patch "${patch_name}_patch"'
        if needle not in build:
            raise AssertionError(f"build.sh bypasses the patch helper: {needle}")


def apply_patch(source: Path, patch: Path) -> None:
    # The first value simulates a hostile Windows/global setting.  The later
    # build-owned value must win and preserve the stock archive's canonical LF.
    command = [
        "git",
        "-c",
        "core.autocrlf=true",
        "-c",
        "core.eol=crlf",
        "-c",
        "core.autocrlf=false",
        "-c",
        "core.eol=lf",
        "apply",
    ]
    run(command + ["--check", str(patch)], cwd=source)
    run(command + [str(patch)], cwd=source)


def prove_combination(
    recipe: Path,
    temporary: Path,
    name: str,
    stock_tar: Path | None,
    repository_archive: bytes | None,
) -> None:
    gpu_draw, coloroffset = (part == "1" for part in name.split(":"))
    source = materialize_stock(
        temporary / ("combo-" + name.replace(":", "-")),
        stock_tar,
        repository_archive,
    )
    apply_patch(source, recipe / "0001-deterministic-build-and-init-oob.patch")
    if gpu_draw:
        apply_patch(source, recipe / "0002-exact-gpu-draw-optimizations.patch")
        for policy in ("isaac_gpu_draw_policy.h", "isaac_gxm_state_policy.h"):
            shutil.copyfile(recipe / policy, source / "source" / policy)
    if coloroffset:
        apply_patch(source, recipe / "0003-exact-coloroffset-gpu-optimizations.patch")
        policy = "isaac_coloroffset_gpu_policy.h"
        shutil.copyfile(recipe / policy, source / "source" / policy)
    apply_patch(source, recipe / "0004-hardened-custom-shader-cache.patch")
    for policy in (
        "isaac_shader_cache_policy.h",
        "isaac_shader_cache_vitagl.h",
        "isaac_shader_cache_block_list.h",
    ):
        shutil.copyfile(recipe / policy, source / "source" / policy)

    targets = (
        source / "source" / "custom_shaders.c",
        source / "source" / "vitaGL.h",
    )
    if gpu_draw:
        # The GL time-profile scene timer sits on top of the 0002 gxm.c and
        # must leave the two hashed files byte-identical.
        before = tuple(sha256(target) for target in targets)
        apply_patch(source, recipe / "0005-isaac-scene-timer.patch")
        gxm = (source / "source" / "gxm.c").read_bytes()
        for needle in (b"vglIsaacSceneTimes", b"HAVE_ISAAC_GL_TIME_PROFILE",
                       b"ISAAC_SCENE_TIME_END_BEGIN_SCENE"):
            if needle not in gxm:
                raise AssertionError(f"scene-timer patch lost {needle!r}")
        if b"\r\n" in gxm:
            raise AssertionError("scene-timer patch introduced CRLF into gxm.c")
        if tuple(sha256(target) for target in targets) != before:
            raise AssertionError("scene-timer patch touched a hashed file")
    actual = tuple(sha256(target) for target in targets)
    if actual != EXPECTED_HASHES[name]:
        raise AssertionError(
            f"patch chain {name} hashes {actual}, expected {EXPECTED_HASHES[name]}"
        )
    for target in targets:
        if b"\r\n" in target.read_bytes():
            raise AssertionError(f"ambient CRLF conversion reached {name}: {target}")
    print(f"vitaGL patch chain {name}: {actual[0]} / {actual[1]}")


def main() -> int:
    parser = argparse.ArgumentParser()
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--stock-tar", type=Path)
    source.add_argument("--stock-repo", type=Path)
    args = parser.parse_args()

    vita = Path(__file__).resolve().parent
    recipe = vita / "vitagl-stock-reference"
    verify_build_contract(recipe)
    repository_archive = None
    stock_tar = args.stock_tar
    if stock_tar is not None:
        if not stock_tar.is_file():
            raise AssertionError(f"stock tar is not a file: {stock_tar}")
        actual_archive_hash = sha256(stock_tar)
        if actual_archive_hash != SOURCE_ARCHIVE_SHA256:
            raise AssertionError(
                f"stock tar SHA-256 {actual_archive_hash}, "
                f"expected {SOURCE_ARCHIVE_SHA256}"
            )
    else:
        repository_archive = stock_archive_from_repo(args.stock_repo)

    with tempfile.TemporaryDirectory(prefix="isaac-vitagl-patch-chain-") as value:
        temporary = Path(value)
        for name in EXPECTED_HASHES:
            prove_combination(
                recipe, temporary, name, stock_tar, repository_archive
            )
    print("Isaac exact stock vitaGL patch-chain regression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
