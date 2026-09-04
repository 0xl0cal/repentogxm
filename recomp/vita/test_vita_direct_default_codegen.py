#!/usr/bin/env python3
"""Hostile fixtures for the one-file Vita direct-default derivation."""

from __future__ import annotations

import argparse
from contextlib import redirect_stdout
import hashlib
import io
import json
import os
from pathlib import Path
import tempfile

import vita_direct_default_codegen as codegen


def must_fail(label: str, callback, needle: str) -> None:
    try:
        callback()
    except codegen.DirectDefaultCodegenError as exc:
        if needle not in str(exc):
            raise AssertionError(f"{label}: wrong failure: {exc}") from exc
    else:
        raise AssertionError(f"{label}: hostile fixture passed")


def identity(data: bytes) -> tuple[int, str]:
    return len(data), hashlib.sha256(data).hexdigest()


def production_identity_contract() -> None:
    vita = Path(__file__).resolve().parent
    cmake = (vita / "CMakeLists.txt").read_text(encoding="utf-8")
    allocator_gate = (vita / "vita_raw_allocator_gate.py").read_text(
        encoding="utf-8"
    )
    cmake_needles = (
        f"set(ISAAC_VITA_DIRECT_DEFAULT_SOURCE_SIZE {codegen.SOURCE_SIZE})",
        f'"{codegen.SOURCE_SHA256}")',
    )
    gate_needles = (
        f"DIRECT_DEFAULT_OUTPUT_SIZE = {codegen.OUTPUT_SIZE:_}",
        f'"{codegen.OUTPUT_SHA256}"',
        "RENDER_SURFACE_NATIVE_OUTPUT_SIZE = "
        f"{codegen.RENDER_SURFACE_NATIVE_OUTPUT_SIZE:_}",
        f'"{codegen.RENDER_SURFACE_NATIVE_OUTPUT_SHA256}"',
    )
    if cmake.count('--mode "${ISAAC_VITA_DIRECT_DEFAULT_MODE}"') != 1:
        raise AssertionError(
            "production CMake does not pass the derivation mode to the codegen"
        )
    if codegen.RENDER_SURFACE_NATIVE_OUTPUT_SIZE == codegen.OUTPUT_SIZE:
        raise AssertionError("the two derivation modes share an identity")
    for needle in cmake_needles:
        if cmake.count(needle) != 1:
            raise AssertionError(
                f"production CMake lost the frozen direct-default identity: {needle}"
            )
    for needle in gate_needles:
        if allocator_gate.count(needle) != 1:
            raise AssertionError(
                f"allocator gate lost the derived direct-default identity: {needle}"
            )


def exact_replacement_tests() -> bytes:
    source = b"/* synthetic canonical prefix */\n" + b"".join(
        old + b"/* seam boundary */\n"
        for old, unused_new in codegen.REPLACEMENTS
    )
    derived = codegen._apply_exact_replacements(source)
    if derived.count(codegen.MARKER) != 4:
        raise AssertionError("derived fixture does not contain exactly four markers")
    for unused_old, new in codegen.REPLACEMENTS:
        if derived.count(new) != 1:
            raise AssertionError("an exact direct-default seam was not preserved")

    native = codegen._apply_exact_replacements(
        source, codegen.MODE_RENDER_SURFACE_NATIVE
    )
    if native.count(codegen.MARKER) != 4:
        raise AssertionError("native fixture does not contain exactly four markers")
    if native.count(codegen.REPLACEMENTS[0][1]) != 0:
        raise AssertionError("render-surface-native skipped the Render Surface")
    if native.count(b"goto L_004b0712;") != 0:
        raise AssertionError("render-surface-native emitted the Render Surface goto")
    if native.count(codegen.RENDER_SURFACE_NATIVE_REPLACEMENTS[0][1]) != 1:
        raise AssertionError("render-surface-native lost its self-description")
    for unused_old, new in codegen.REPLACEMENTS[1:]:
        if native.count(new) != 1:
            raise AssertionError("render-surface-native lost an HQX/colour seam")
    if native == derived:
        raise AssertionError("the two derivation modes produced identical bytes")
    must_fail(
        "unknown derivation mode",
        lambda: codegen._apply_exact_replacements(source, "hqx-only"),
        "unknown derivation mode",
    )

    must_fail(
        "missing seam",
        lambda: codegen._apply_exact_replacements(
            source.replace(codegen.REPLACEMENTS[0][0], b"", 1)
        ),
        "seam count changed: 0 != 1",
    )
    must_fail(
        "duplicate seam",
        lambda: codegen._apply_exact_replacements(
            source + codegen.REPLACEMENTS[1][0]
        ),
        "seam count changed: 2 != 1",
    )
    must_fail(
        "prepatched canonical input",
        lambda: codegen._apply_exact_replacements(codegen.MARKER + source),
        "already contains",
    )
    must_fail(
        "unpinned exact renderer",
        lambda: codegen.render_exact_source(source),
        "input identity changed",
    )
    return source


def writer_tests(root: Path, source: bytes) -> None:
    derived = codegen._apply_exact_replacements(source)
    saved = (
        codegen.SOURCE_SIZE,
        codegen.SOURCE_SHA256,
        codegen.OUTPUT_SIZE,
        codegen.OUTPUT_SHA256,
    )
    codegen.SOURCE_SIZE, codegen.SOURCE_SHA256 = identity(source)
    codegen.OUTPUT_SIZE, codegen.OUTPUT_SHA256 = identity(derived)
    try:
        canonical = root / "canonical" / codegen.SOURCE_NAME
        output = (
            root / "build" / codegen.OVERRIDE_PARENT[0] /
            codegen.OVERRIDE_PARENT[1] / codegen.SOURCE_NAME
        )
        canonical.parent.mkdir(parents=True)
        canonical.write_bytes(source)

        if not codegen.derive_file(canonical, output):
            raise AssertionError("first derivation did not publish its output")
        if output.read_bytes() != derived:
            raise AssertionError("published derivation differs from exact bytes")
        stable_mtime = output.stat().st_mtime_ns
        if codegen.derive_file(canonical, output):
            raise AssertionError("byte-identical derivation rewrote its output")
        if output.stat().st_mtime_ns != stable_mtime:
            raise AssertionError("BYTES STABLE changed the output timestamp")

        stdout = io.StringIO()
        with redirect_stdout(stdout):
            status = codegen.main(
                ["--input", str(canonical), "--output", str(output)]
            )
        if status != 0 or "BYTES STABLE" not in stdout.getvalue():
            raise AssertionError("stable CLI invocation lost its explicit state")
        if "mode=direct-default" not in stdout.getvalue():
            raise AssertionError("CLI does not report the derivation mode")

        native_expected = codegen._apply_exact_replacements(
            source, codegen.MODE_RENDER_SURFACE_NATIVE
        )
        saved_native = (
            codegen.RENDER_SURFACE_NATIVE_OUTPUT_SIZE,
            codegen.RENDER_SURFACE_NATIVE_OUTPUT_SHA256,
        )
        must_fail(
            "unpinned render-surface-native renderer",
            lambda: codegen.render_exact_source(
                source, codegen.MODE_RENDER_SURFACE_NATIVE
            ),
            "derived render-surface-native output identity changed",
        )
        (
            codegen.RENDER_SURFACE_NATIVE_OUTPUT_SIZE,
            codegen.RENDER_SURFACE_NATIVE_OUTPUT_SHA256,
        ) = identity(native_expected)
        try:
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                status = codegen.main([
                    "--input", str(canonical), "--output", str(output),
                    "--mode", codegen.MODE_RENDER_SURFACE_NATIVE,
                ])
            if status != 0 or "UPDATED" not in stdout.getvalue():
                raise AssertionError("mode switch did not republish the output")
            if "mode=render-surface-native" not in stdout.getvalue():
                raise AssertionError("CLI does not report the native mode")
            if output.read_bytes() != native_expected:
                raise AssertionError("native derivation differs from exact bytes")
            if not codegen.derive_file(canonical, output):
                raise AssertionError("switching back did not republish the output")
            if output.read_bytes() != derived:
                raise AssertionError("switching back lost the direct-default bytes")
        finally:
            (
                codegen.RENDER_SURFACE_NATIVE_OUTPUT_SIZE,
                codegen.RENDER_SURFACE_NATIVE_OUTPUT_SHA256,
            ) = saved_native

        output_before = output.read_bytes()
        canonical.write_bytes(source + b"hostile identity mutation\n")
        must_fail(
            "canonical identity mutation",
            lambda: codegen.derive_file(canonical, output),
            "input identity changed",
        )
        if output.read_bytes() != output_before:
            raise AssertionError("failed derivation changed the prior output")
        canonical.write_bytes(source)

        foreign = output.parent / "foreign.txt"
        foreign.write_bytes(b"unowned\n")
        must_fail(
            "unowned output sibling",
            lambda: codegen.derive_file(canonical, output),
            "unowned entry",
        )
        foreign.unlink()

        must_fail(
            "wrong output ownership",
            lambda: codegen.derive_file(canonical, root / codegen.SOURCE_NAME),
            "must end in",
        )
        must_fail(
            "relative input",
            lambda: codegen.derive_file(
                Path(codegen.SOURCE_NAME), output
            ),
            "canonical input must be absolute",
        )

        symlink_output = (
            root / "symlink-build" / codegen.OVERRIDE_PARENT[0] /
            codegen.OVERRIDE_PARENT[1] / codegen.SOURCE_NAME
        )
        symlink_output.parent.mkdir(parents=True)
        try:
            os.symlink(output, symlink_output)
        except OSError:
            pass
        else:
            must_fail(
                "symlink output",
                lambda: codegen.derive_file(canonical, symlink_output),
                "non-symlink file",
            )

        ancestor_build = root / "ancestor-build"
        ancestor_build.mkdir()
        outside_owner = root / "outside-owner"
        outside_owner.mkdir()
        linked_owner = ancestor_build / codegen.OVERRIDE_PARENT[0]
        try:
            os.symlink(outside_owner, linked_owner, target_is_directory=True)
        except OSError:
            pass
        else:
            linked_output = (
                linked_owner / codegen.OVERRIDE_PARENT[1] /
                codegen.SOURCE_NAME
            )
            must_fail(
                "symlinked output owner",
                lambda: codegen.derive_file(canonical, linked_output),
                "symlinked build-owned directory",
            )
    finally:
        (
            codegen.SOURCE_SIZE,
            codegen.SOURCE_SHA256,
            codegen.OUTPUT_SIZE,
            codegen.OUTPUT_SHA256,
        ) = saved


def frozen_corpus_tests(source: Path, root: Path) -> None:
    source = source.resolve()
    data = codegen._require_input(source)
    if identity(data) != (codegen.SOURCE_SIZE, codegen.SOURCE_SHA256):
        raise AssertionError("frozen corpus canonical identity disagrees with pins")

    manifest_path = source.parent / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    records = [
        record for record in manifest.get("outputs", [])
        if record.get("path") == codegen.SOURCE_NAME
    ]
    expected_record = {
        "path": codegen.SOURCE_NAME,
        "size": codegen.SOURCE_SIZE,
        "sha256": codegen.SOURCE_SHA256,
    }
    if records != [expected_record]:
        raise AssertionError(
            "frozen corpus manifest lost the exact canonical identity"
        )

    output = (
        root / "build" / codegen.OVERRIDE_PARENT[0] /
        codegen.OVERRIDE_PARENT[1] / codegen.SOURCE_NAME
    ).resolve()
    if not codegen.derive_file(source, output):
        raise AssertionError("frozen corpus first derivation was not published")
    derived = output.read_bytes()
    if identity(derived) != (codegen.OUTPUT_SIZE, codegen.OUTPUT_SHA256):
        raise AssertionError("frozen corpus derived identity disagrees with pins")
    if derived.count(codegen.MARKER) != len(codegen.REPLACEMENTS):
        raise AssertionError("frozen corpus derived marker census changed")
    stable_mtime = output.stat().st_mtime_ns
    if codegen.derive_file(source, output):
        raise AssertionError("frozen corpus byte-stable derivation rewrote output")
    if output.stat().st_mtime_ns != stable_mtime:
        raise AssertionError("frozen corpus BYTES STABLE changed output mtime")

    # render-surface-native against the same frozen corpus: the pinned
    # identity must hold, the Render Surface activation at 0x4b06f4 must
    # survive with no bypass goto in front of it, and the HQX / Color
    # Correction seams must still carry their gotos.
    if not codegen.derive_file(
        source, output, codegen.MODE_RENDER_SURFACE_NATIVE
    ):
        raise AssertionError("frozen corpus native derivation was not published")
    native = output.read_bytes()
    if identity(native) != (
        codegen.RENDER_SURFACE_NATIVE_OUTPUT_SIZE,
        codegen.RENDER_SURFACE_NATIVE_OUTPUT_SHA256,
    ):
        raise AssertionError(
            "frozen corpus render-surface-native identity disagrees with pins"
        )
    if native.count(codegen.MARKER) != len(codegen.REPLACEMENTS):
        raise AssertionError("frozen corpus native marker census changed")
    if native.count(b"goto L_004b0712;\n    /* 004b06f4") != 0:
        raise AssertionError("frozen corpus native bypassed the Render Surface")
    if native.count(codegen.RENDER_SURFACE_NATIVE_REPLACEMENTS[0][1]) != 1:
        raise AssertionError("frozen corpus native lost the preserved seam")
    for seam in (b"goto L_004b0777;\n    /* 004b0759",
                 b"goto L_004b07a1;\n    /* 004b078a",
                 b"goto L_004b0c34;\n    /* 004b0af7"):
        if native.count(seam) != 1:
            raise AssertionError("frozen corpus native lost an HQX/colour seam")
    if len(native) - len(derived) != (
        codegen.RENDER_SURFACE_NATIVE_OUTPUT_SIZE - codegen.OUTPUT_SIZE
    ):
        raise AssertionError("frozen corpus mode size delta drifted")
    if not codegen.derive_file(source, output):
        raise AssertionError("frozen corpus switch back was not published")
    if output.read_bytes() != derived:
        raise AssertionError("frozen corpus switch back lost direct-default bytes")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source", type=Path,
        help="optional frozen full-corpus guest_0144.c identity oracle",
    )
    args = parser.parse_args()
    production_identity_contract()
    source = exact_replacement_tests()
    with tempfile.TemporaryDirectory(
        prefix="isaac-vita-direct-default-codegen-"
    ) as value:
        root = Path(value)
        writer_tests(root, source)
        if args.source is not None:
            frozen_corpus_tests(args.source, root / "frozen")
    print(
        "Vita direct-default codegen hostile self-test: PASS; "
        "production identity/four exact seams/hash mismatch/path ownership/"
        "atomic preservation/BYTES STABLE"
        + ("/frozen corpus+manifest, both modes covered"
           if args.source else " covered")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
