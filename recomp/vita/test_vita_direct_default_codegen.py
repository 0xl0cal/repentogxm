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
import re
import shutil
import subprocess
import sys
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


def cmake_owner_scan_tests(root: Path) -> None:
    """Run production selection/scans with missing and hostile stale outputs."""
    from test_kage_vita_png_decode_profile import compiler

    vita = Path(__file__).resolve().parent
    production = (vita / "CMakeLists.txt").read_text(encoding="utf-8")

    def section(begin: str, end: str) -> str:
        start = production.index(begin)
        return production[start:production.index(end, start)]

    prepare = section("  # Feature-owner discovery must inspect", "  set(ISAAC_VITA_RUNTIME_SOURCES")
    scans = section("  if(ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH)\n    # gen_all.py pins", "  if(ISAAC_VITA_LIGHT_SURFACE_RASTER_416 OR")
    scans += section("      if(ISAAC_VITA_DEEP_PROFILE)\n        set(ISAAC_DEEP_GENERATED_OWNERS", "      if(ISAAC_VITA_NATIVE_RESOURCE_PROFILE)")
    finalize = section("  # Finalize the compiled generated list", "  target_include_directories(isaac_first_arm_fault PRIVATE\n    \"${ISAAC_GENERATED_DIR}\")")
    derivation = section("  if(ISAAC_VITA_DIRECT_DEFAULT)\n    # The derived unit's identity", "  if(ISAAC_VITA_LUA)\n    list(APPEND ISAAC_VITA_RAW_ALLOCATOR_LUA_ARGS")
    cmake, ninja, cc = shutil.which("cmake"), shutil.which("ninja"), compiler()
    if ninja is None and os.name == "nt":
        # Same installed-tool fallback as the existing PNG CMake oracle.
        candidate = (Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
                     "Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe")
        if candidate.is_file():
            ninja = str(candidate)
    if not cmake or not ninja:
        raise RuntimeError("cmake/ninja are required for direct-default owner tests")
    toolchain = [f"-DCMAKE_C_COMPILER={Path(cc).as_posix()}",
                 f"-DCMAKE_MAKE_PROGRAM={Path(ninja).as_posix()}"]
    if os.name == "nt":
        toolchain += [f"-DCMAKE_RC_COMPILER={Path(cc).with_name('llvm-rc.exe').as_posix()}",
                      f"-DCMAKE_AR={Path(cc).with_name('llvm-ar.exe').as_posix()}",
                      "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY"]
    root.mkdir()
    canonical = root / codegen.SOURCE_NAME
    # Co-locate the three real selectors in the replaced owner. The fixture
    # exercises discovery/property transport, not the separately tested hash gate.
    canonical.write_text(
        "void sub_005aeb00(CPU *__restrict c)\n{\n"
        "  isaac_vita_archive_miniz_guest_try(c);\n}\n"
        "void sub_0059c690(CPU *__restrict c)\n{\n"
        "  isaac_vita_wav_buffered_rewind_try(c);\n"
        + "  KAGE_VITA_DEEP_SCOPE(KVD_ROOM_SWITCH);\n" * 16
        + "".join(f"  KAGE_VITA_DEEP_SCOPE({name});\n" for name in (
            "KVD_ANM2_LOAD", "KVD_ANM2_GRAPHICS", "KVD_ROOM_STATE_RESET", "KVD_ROOM_SNAPSHOT"))
        + "}\n", encoding="utf-8")
    (root / "unrelated.c").write_text("int unrelated;\n", encoding="utf-8")
    (root / "manifest.json").write_text("{}\n", encoding="utf-8")
    for name in set(re.findall(r'\$\{ISAAC_RUNTIME\}/([^"/]+\.c)', scans)):
        (root / name).write_text("/* runtime placeholder */\n", encoding="utf-8")
    for name in ("owner-include", "derived-include"):
        (root / name).mkdir()
    for name in ("owner.dep", "derived.dep"):
        (root / name).write_text("fixture\n", encoding="utf-8")
    (root / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\nproject(direct_owner C)\n"
        "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n"
        f'include("{(vita / "generated_definitions.cmake").as_posix()}")\n'
        "function(isaac_validate_strict_absolute_path path label)\n"
        "  if(NOT IS_ABSOLUTE \"${path}\")\nmessage(FATAL_ERROR \"${label}\")\nendif()\nendfunction()\n"
        "set(ISAAC_RUNTIME \"${CMAKE_CURRENT_SOURCE_DIR}\")\n"
        "set(ISAAC_GENERATED_DIR \"${CMAKE_CURRENT_SOURCE_DIR}\")\n"
        f'set(Python3_EXECUTABLE "{Path(sys.executable).as_posix()}")\n'
        "set(ISAAC_VITA_RUNTIME_SOURCES)\n"
        "set(ISAAC_GENERATED_C \"${ISAAC_RUNTIME}/unrelated.c\" \"${ISAAC_RUNTIME}/guest_0144.c\")\n"
        "set(ISAAC_GENERATED_C_COUNT 2)\n"
        "set(ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE \"${ISAAC_RUNTIME}/guest_0144.c\")\n"
        f"set(ISAAC_VITA_DIRECT_DEFAULT_SOURCE_SIZE {codegen.SOURCE_SIZE})\n"
        f"set(ISAAC_VITA_DIRECT_DEFAULT_MANIFEST_SIZE {codegen.SOURCE_SIZE})\n"
        f"set(ISAAC_VITA_DIRECT_DEFAULT_SOURCE_SHA {codegen.SOURCE_SHA256})\n"
        f"set(ISAAC_VITA_DIRECT_DEFAULT_MANIFEST_SHA {codegen.SOURCE_SHA256})\n"
        + prepare
        + f'set(ISAAC_VITA_DIRECT_DEFAULT_CODEGEN "{(vita / "vita_direct_default_codegen.py").as_posix()}")\n'
        + "set(ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH ON)\nset(ISAAC_VITA_WAV_BUFFERED_REWIND ON)\nset(ISAAC_VITA_DEEP_PROFILE ON)\n"
        + scans
        + "set_property(SOURCE \"${ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE}\" APPEND PROPERTY COMPILE_DEFINITIONS OWNER_ONLY=1 SHARED=1)\n"
        "set_property(SOURCE \"${ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE}\" PROPERTY COMPILE_OPTIONS -DOWNER_OPTION=1)\n"
        "set_property(SOURCE \"${ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE}\" PROPERTY COMPILE_FLAGS \"-DOWNER_FLAGS=1\")\n"
        "set_property(SOURCE \"${ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE}\" PROPERTY INCLUDE_DIRECTORIES \"${ISAAC_RUNTIME}/owner-include\")\n"
        "set_property(SOURCE \"${ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE}\" PROPERTY OBJECT_DEPENDS \"${ISAAC_RUNTIME}/owner.dep\")\n"
        "if(ISAAC_VITA_DIRECT_DEFAULT)\n"
        "set_property(SOURCE \"${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}\" PROPERTY COMPILE_DEFINITIONS DERIVED_ONLY=1 SHARED=1)\n"
        "set_property(SOURCE \"${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}\" PROPERTY COMPILE_OPTIONS -DDERIVED_OPTION=1)\n"
        "set_property(SOURCE \"${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}\" PROPERTY COMPILE_FLAGS \"-DDERIVED_FLAGS=1\")\n"
        "set_property(SOURCE \"${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}\" PROPERTY INCLUDE_DIRECTORIES \"${ISAAC_RUNTIME}/derived-include\")\n"
        "set_property(SOURCE \"${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}\" PROPERTY OBJECT_DEPENDS \"${ISAAC_RUNTIME}/derived.dep\")\nendif()\n"
        "add_library(isaac_first_arm_fault OBJECT)\n"
        + finalize + derivation
        + "list(GET ISAAC_GENERATED_C 1 compiled_owner)\n"
        "get_property(owner_definitions SOURCE \"${compiled_owner}\" PROPERTY COMPILE_DEFINITIONS)\n"
        "get_property(owner_dependencies SOURCE \"${compiled_owner}\" PROPERTY OBJECT_DEPENDS)\n"
        "file(WRITE \"${CMAKE_BINARY_DIR}/owner-properties.txt\" \"${ISAAC_GENERATED_C}\\n${owner_definitions}\\n${owner_dependencies}\\n\")\n",
        encoding="utf-8")
    for enabled, stale in ((False, False), (True, False), (True, True)):
        build = root / f"build-{int(enabled)}-{int(stale)}"
        output = build.joinpath(*codegen.OVERRIDE_PARENT, codegen.SOURCE_NAME)
        hostile = b"void sub_0059c690(CPU *__restrict c)\nvoid sub_0059c690(CPU *__restrict c)\n"
        if stale:
            output.parent.mkdir(parents=True)
            output.write_bytes(hostile)
        completed = subprocess.run([cmake, "-S", str(root), "-B", str(build), "-G", "Ninja",
                                    *toolchain, f"-DISAAC_VITA_DIRECT_DEFAULT={'ON' if enabled else 'OFF'}",
                                    f"-DISAAC_VITA_RENDER_SURFACE_NATIVE={'ON' if stale else 'OFF'}"],
                                   capture_output=True, text=True)
        if completed.returncode:
            raise AssertionError(f"owner configure failed: {completed.stdout}\n{completed.stderr}")
        if output.exists() != stale or (stale and output.read_bytes() != hostile):
            raise AssertionError("configure materialized or rewrote the derived source")
        lines = (build / "owner-properties.txt").read_text().splitlines()
        expected_owner = output if enabled else canonical
        if [Path(path) for path in lines[0].split(";")] != [root / "unrelated.c", expected_owner]:
            raise AssertionError(f"compiled source replacement changed order/count: {lines[0]}")
        definitions = lines[1].split(";")
        if definitions.count("SHARED=1") != 1:
            raise AssertionError("shared source definition was duplicated")
        dependencies = {Path(path) for path in lines[2].split(";")}
        if dependencies != {root / "owner.dep"} | ({root / "derived.dep"} if enabled else set()):
            raise AssertionError("source OBJECT_DEPENDS did not survive substitution")
        commands = json.loads((build / "compile_commands.json").read_text())
        owners = [item for item in commands if Path(item["file"]) == expected_owner]
        if len(owners) != 1:
            raise AssertionError("compiled derived owner is missing or duplicated")
        rendered = owners[0]["command"]
        for token in ("ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH=1", "ISAAC_VITA_WAV_BUFFERED_REWIND=1",
                      "ISAAC_VITA_DEEP_PROFILE=1", "OWNER_ONLY=1", "OWNER_OPTION=1",
                      "OWNER_FLAGS=1", "owner-include"):
            if token not in rendered:
                raise AssertionError(f"compiled owner lost {token}: {rendered}")
        for token in ("DERIVED_ONLY=1", "DERIVED_OPTION=1", "DERIVED_FLAGS=1", "derived-include"):
            if (token in rendered) != enabled:
                raise AssertionError(f"explicit derived setting changed: {token}")
        unrelated = next(item["command"] for item in commands if Path(item["file"]).name == "unrelated.c")
        if any(token in unrelated for token in ("ISAAC_VITA_DEEP_PROFILE=1", "OWNER_ONLY=1", "DERIVED_ONLY=1")):
            raise AssertionError("owner definition escaped to the unrelated generated unit")
    print("Direct-default CMake canonical scans: OFF/missing/stale; compiled owner properties PASS")


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
        cmake_owner_scan_tests(root / "cmake")
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
