#!/usr/bin/env python3
"""Cross-language path policy and actual Ninja generated-path regression."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import string
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
HELPER = HERE / "literal_glob_path.cmake"
PROBE = HERE / "test_literal_glob_path.cmake"
PRODUCTION = HERE / "CMakeLists.txt"
TOOLS = REPO / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
import build_vita


GENERATED_NAMES = (
    "generated_header.h", "guest_0000.c", "manifest.json",
)
CONFIGURE_PROJECT = r'''cmake_minimum_required(VERSION 3.19)

foreach(_required ISAAC_PATH_HELPER ISAAC_GENERATED_DIR ISAAC_TEST_SDK
        ISAAC_TEST_PE CMAKE_TOOLCHAIN_FILE)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "${_required} is required")
  endif()
endforeach()
include("${ISAAC_PATH_HELPER}")
foreach(_strict IN ITEMS CMAKE_CURRENT_SOURCE_DIR CMAKE_BINARY_DIR
        ISAAC_TEST_SDK CMAKE_TOOLCHAIN_FILE ISAAC_TEST_PE)
  isaac_validate_strict_absolute_path("${${_strict}}" "${_strict}")
endforeach()
isaac_validate_generated_absolute_path(
  "${ISAAC_GENERATED_DIR}" "ISAAC_GENERATED_DIR")

project(isaac_path_policy_object LANGUAGES C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

isaac_literal_directory_files(
  "${ISAAC_GENERATED_DIR}" _generated_files)
set(_expected_files generated_header.h guest_0000.c manifest.json)
list(SORT _expected_files)
if(NOT "${_generated_files}" STREQUAL "${_expected_files}")
  message(FATAL_ERROR
    "literal directory set mismatch: actual='${_generated_files}' "
    "expected='${_expected_files}'")
endif()
list(LENGTH _generated_files _generated_count)
if(NOT _generated_count EQUAL 3)
  message(FATAL_ERROR "generated source/header/manifest count changed")
endif()

set(_guest_source "${ISAAC_GENERATED_DIR}/guest_0000.c")
set(_guest_header "${ISAAC_GENERATED_DIR}/generated_header.h")
add_library(literal_generated_object OBJECT "${_guest_source}")
target_include_directories(literal_generated_object PRIVATE
  "${ISAAC_GENERATED_DIR}")

set(_copied_header "${CMAKE_BINARY_DIR}/copied-generated-header.h")
add_custom_command(
  OUTPUT "${_copied_header}"
  COMMAND "${CMAKE_COMMAND}" -E copy "${_guest_header}" "${_copied_header}"
  DEPENDS "${_guest_header}"
  VERBATIM)
add_custom_target(literal_generated_copy ALL DEPENDS "${_copied_header}")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${ISAAC_GENERATED_DIR}/manifest.json" "${_guest_source}" "${_guest_header}")
get_property(_configure_depends DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS)
set(_expected_depends
  "${ISAAC_GENERATED_DIR}/manifest.json" "${_guest_source}" "${_guest_header}")
list(SORT _configure_depends)
list(SORT _expected_depends)
if(NOT "${_configure_depends}" STREQUAL "${_expected_depends}")
  message(FATAL_ERROR "CMAKE_CONFIGURE_DEPENDS absolute set changed")
endif()
list(LENGTH _configure_depends _configure_count)
file(WRITE "${CMAKE_BINARY_DIR}/path-policy-configure.txt"
  "generated=${_generated_count}\nconfigure_depends=${_configure_count}\n")
'''


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(
    command: list[str], *, cwd: Path | None = None,
    environment: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command, cwd=None if cwd is None else str(cwd),
        env=environment,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, check=False,
    )


def require_pass(completed: subprocess.CompletedProcess[str], label: str) -> None:
    if completed.returncode != 0:
        raise AssertionError(
            f"{label} unexpectedly failed (rc={completed.returncode})\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )


def require_red(
    completed: subprocess.CompletedProcess[str], label: str, token: str,
) -> None:
    combined = completed.stdout + completed.stderr
    if completed.returncode == 0 or token not in combined:
        raise AssertionError(
            f"{label} did not fail with {token!r} (rc={completed.returncode})\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )


def escaped_glob(value: str) -> str:
    return value.replace("[", "[[]").replace("*", "[*]").replace("?", "[?]")


def policy_probe(
    cmake: str, role: str, candidate: str,
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["ISAAC_PATH_POLICY_INPUT"] = candidate
    return run([
        cmake,
        f"-DISAAC_LITERAL_GLOB_HELPER={HELPER.as_posix()}",
        f"-DISAAC_PATH_POLICY_ROLE={role}",
        "-DISAAC_PATH_POLICY_USE_ENV=ON",
        f"-DWIN32={'OFF' if os.name == 'nt' else 'ON'}",
        "-P", str(PROBE),
    ], environment=environment)


def glob_probe(
    cmake: str, generated: Path, expected: tuple[str, ...] = GENERATED_NAMES,
) -> subprocess.CompletedProcess[str]:
    root = generated.as_posix()
    return run([
        cmake,
        f"-DISAAC_LITERAL_GLOB_HELPER={HELPER.as_posix()}",
        f"-DISAAC_LITERAL_GLOB_ROOT={root}",
        f"-DISAAC_LITERAL_GLOB_EXPECT_ESCAPED={escaped_glob(root)}",
        f"-DISAAC_LITERAL_GLOB_EXPECTED={';'.join(expected)}",
        "-P", str(PROBE),
    ])


def python_accepts(role: str, candidate: str) -> bool:
    problem = (
        build_vita._strict_absolute_path_problem(candidate)
        if role == "strict" else
        build_vita._generated_absolute_path_problem(candidate)
    )
    return problem is None


def assert_python_cmake_parity(cmake: str) -> int:
    prefix = "C:/portable" if os.name == "nt" else "/portable"
    strict_allowed = frozenset(string.ascii_letters + string.digits + "._-")
    generated_allowed = frozenset(
        string.ascii_letters + string.digits + "._-*?[]"
    )
    count = 0

    def check(role: str, candidate: str, expected: bool) -> None:
        nonlocal count
        completed = policy_probe(cmake, role, candidate)
        cmake_accepts = completed.returncode == 0
        python_result = python_accepts(role, candidate)
        if cmake_accepts != expected or python_result != expected:
            raise AssertionError(
                f"independent {role} policy differs for {candidate!r}: "
                f"expected={expected} python={python_result} "
                f"cmake={cmake_accepts}\nstdout:\n{completed.stdout}\n"
                f"stderr:\n{completed.stderr}"
            )
        count += 1

    # Exhaust every printable ASCII byte as an interior segment byte.  The
    # expectation comes from literals in this test, never the implementation
    # regex.  Slash is exercised separately as the path separator.
    for character in map(chr, range(0x20, 0x7F)):
        if character == "/":
            continue
        check("strict", prefix + "/a" + character + "b",
              character in strict_allowed)
        generated_expected = character in generated_allowed
        if character in "[]":
            generated_expected = False  # one bracket is structurally unmatched
        check("generated", prefix + "/a" + character + "b",
              generated_expected)

    for control in ("\x01", "\x09", "\x0a", "\x0d", "\x1f", "\x7f"):
        check("strict", prefix + "/a" + control + "b", False)
        check("generated", prefix + "/a" + control + "b", False)
    for role in ("strict", "generated"):
        for candidate, expected in (
            (prefix, True), (prefix + "/alpha/beta", True),
            ("", False), ("relative/path", False),
            ("//server/share", False), ("\\\\server\\share", False),
            (prefix + "/", False), (prefix + "//empty", False),
            (prefix + "/./dot", False), (prefix + "/../dotdot", False),
            (prefix + "/unicode-é", False),
        ):
            check(role, candidate, expected)
    for candidate, expected in (
        (prefix + "/generated_[left]_]rev[_*_?", True),
        (prefix + "/balanced[x]", True),
        (prefix + "/reversed]x[", True),
        (prefix + "/has space", False),
        (prefix + "/ leading", False),
        (prefix + "/trailing ", False),
        (prefix + "/open[", False),
        (prefix + "/close]", False),
        (prefix + "/open[/close]", False),
    ):
        check("generated", candidate, expected)
    return count


def populate_generated(root: Path, value: int = 7) -> None:
    root.mkdir(parents=True, exist_ok=True)
    (root / "generated_header.h").write_text(
        f"#define ISAAC_GENERATED_VALUE {value}\n", encoding="ascii"
    )
    (root / "guest_0000.c").write_text(
        '#include "generated_header.h"\n'
        "int isaac_generated_value(void) { return ISAAC_GENERATED_VALUE; }\n",
        encoding="ascii",
    )
    (root / "manifest.json").write_text(
        '{"schema":1,"stage":"path-policy-fixture"}\n', encoding="ascii"
    )


def configure_command(
    cmake: str, source: Path, build: Path, generated: Path,
    sdk: Path, pe: Path, toolchain: Path,
) -> list[str]:
    return [
        cmake, "-G", "Ninja", "-S", str(source), "-B", str(build),
        f"-DISAAC_PATH_HELPER={HELPER.as_posix()}",
        f"-DISAAC_GENERATED_DIR={generated.as_posix()}",
        f"-DISAAC_TEST_SDK={sdk.as_posix()}",
        f"-DISAAC_TEST_PE={pe.as_posix()}",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain.as_posix()}",
    ]


def production_missing_sdk_red(cmake: str, base: Path) -> None:
    """Prove explicit toolchain cannot carry an empty SDK into project()."""

    build = base / "production-red-build"
    toolchain = base / "production-red-toolchain.cmake"
    sentinel = base / "toolchain-loaded.sentinel"
    toolchain.write_text(
        f'file(WRITE "{sentinel.as_posix()}" "TOOLCHAIN_LOADED\\n")\n',
        encoding="ascii",
    )
    environment = os.environ.copy()
    environment.pop("VITASDK", None)
    completed = run([
        cmake, "-G", "Ninja", "-S", str(HERE), "-B", str(build),
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain.as_posix()}",
        "-DISAAC_VITA_SCAFFOLD_ONLY=ON",
    ], environment=environment)
    require_red(completed, "production missing-VITASDK pre-project",
                "Set VITASDK to the strict portable softfp SDK root")
    cache = build / "CMakeCache.txt"
    if cache.exists() and "CMAKE_C_COMPILER" in cache.read_text(
            encoding="utf-8", errors="replace"):
        raise AssertionError("missing-VITASDK RED cached a C compiler")
    if (build / "build.ninja").exists():
        raise AssertionError("missing-VITASDK RED wrote build.ninja")
    compiler_ids = list(build.rglob("*CompilerId*")) if build.exists() else []
    if compiler_ids:
        raise AssertionError(f"missing-VITASDK RED ran compiler-id: {compiler_ids}")
    if sentinel.exists():
        raise AssertionError("missing-VITASDK RED loaded explicit toolchain")


def actual_ninja_regression(cmake: str, ninja: str, base: Path) -> dict[str, object]:
    source = base / "source"
    build = base / "build"
    sdk = base / "sdk"
    pe = base / "input.exe"
    toolchain = base / "toolchain.cmake"
    hostile_component = "generated_[left]_]rev["
    if os.name != "nt":
        hostile_component += "_*_?"
    generated = base / hostile_component
    for path in (source, sdk):
        path.mkdir(parents=True)
    pe.write_bytes(b"path-policy synthetic PE\n")
    toolchain.write_text("# compiler discovery is intentionally native\n",
                         encoding="ascii")
    (source / "CMakeLists.txt").write_text(CONFIGURE_PROJECT, encoding="utf-8")
    populate_generated(generated)

    command = configure_command(cmake, source, build, generated, sdk, pe, toolchain)
    require_pass(run(command), "initial Ninja configure")
    require_pass(run([cmake, "--build", str(build)]), "initial Ninja build")
    receipt = build / "path-policy-configure.txt"
    if receipt.read_text(encoding="ascii") != "generated=3\nconfigure_depends=3\n":
        raise AssertionError("configure source/dependency census changed")
    copied = build / "copied-generated-header.h"
    if copied.read_bytes() != (generated / "generated_header.h").read_bytes():
        raise AssertionError("VERBATIM copy did not preserve generated header")

    compile_commands = json.loads(
        (build / "compile_commands.json").read_text(encoding="utf-8")
    )
    if len(compile_commands) != 1:
        raise AssertionError(f"compile command count changed: {len(compile_commands)}")
    entry = compile_commands[0]
    if Path(entry["file"]).resolve() != (generated / "guest_0000.c").resolve():
        raise AssertionError("compile_commands lost the exact generated source")
    command_text = entry.get("command", " ".join(entry.get("arguments", ())))
    if generated.as_posix() not in command_text.replace("\\", "/"):
        raise AssertionError("compile command lost the generated include/source root")
    object_root = build / "CMakeFiles" / "literal_generated_object.dir"
    objects = [path for path in object_root.rglob("*")
               if path.is_file() and path.suffix in (".o", ".obj")]
    if len(objects) != 1 or objects[0].stat().st_size == 0:
        raise AssertionError(f"OBJECT compilation census changed: {objects}")
    # Mutate real configure-dependency content without manufacturing a future
    # timestamp.  The observed mtime relation must itself make a plain build
    # re-run CMake through the hostile physical generated path.
    generated_manifest = generated / "manifest.json"
    generated_manifest.write_text(
        '{"schema":1,"stage":"path-policy-fixture-mutated"}\n',
        encoding="ascii",
    )
    if generated_manifest.stat().st_mtime_ns <= (
            build / "build.ninja").stat().st_mtime_ns:
        raise AssertionError(
            "content-mutated manifest is not newer than observed build.ninja"
        )
    rebuilt = run([cmake, "--build", str(build)])
    require_pass(rebuilt, "configure-dependency rebuild")
    if "Configuring done" not in rebuilt.stdout + rebuilt.stderr:
        raise AssertionError("manifest mutation did not trigger CMake reconfigure")
    if copied.read_bytes() != (generated / "generated_header.h").read_bytes():
        raise AssertionError("VERBATIM copied header changed during reconfigure")

    (generated / "unexpected.c").write_bytes(b"extra\n")
    require_red(run(command), "extra generated output", "literal directory set mismatch")
    (generated / "unexpected.c").unlink()
    require_pass(run(command), "restored set before missing probe")
    guest_payload = (generated / "guest_0000.c").read_bytes()
    (generated / "guest_0000.c").unlink()
    require_red(run(command), "missing generated output", "literal directory set mismatch")
    (generated / "guest_0000.c").write_bytes(guest_payload)
    require_pass(run(command), "final restored reconfigure")
    require_pass(run([cmake, "--build", str(build)]), "final restored build")

    ninja_payload = (build / "build.ninja").read_bytes()
    for forbidden in (b"$\\ ", b".+^$", b"generated+$"):
        if forbidden in ninja_payload:
            raise AssertionError(f"Ninja file retained forbidden path token {forbidden!r}")
    return {
        "physical_star_question": os.name != "nt",
        "compile_commands": len(compile_commands),
        "objects": len(objects), "copied_header": copied.stat().st_size,
        "build_ninja_sha256": hashlib.sha256(ninja_payload).hexdigest(),
    }


def assert_production_contract() -> None:
    production = PRODUCTION.read_text(encoding="utf-8")
    project_offset = production.index("project(isaac_first_arm_fault LANGUAGES C)")
    required_pre_project = (
        'include("${CMAKE_CURRENT_LIST_DIR}/literal_glob_path.cmake")',
        'isaac_validate_strict_absolute_path(\n'
        '    "${${ISAAC_STRICT_BUILD_PATH}}" "${ISAAC_STRICT_BUILD_PATH}")',
        'isaac_validate_strict_absolute_path(\n'
        '  "${CMAKE_TOOLCHAIN_FILE}" "CMAKE_TOOLCHAIN_FILE")',
        'if(NOT DEFINED VITASDK OR "${VITASDK}" STREQUAL "")',
        'set(CMAKE_TOOLCHAIN_FILE "${VITASDK}/share/vita.toolchain.cmake"',
        'isaac_validate_generated_absolute_path(\n'
        '    "${ISAAC_GENERATED_DIR}" "ISAAC_GENERATED_DIR")',
        'isaac_validate_strict_absolute_path(\n'
        '    "${ISAAC_PE_PATH}" "ISAAC_PE_PATH")',
    )
    for contract in required_pre_project:
        if contract not in production or production.index(contract) > project_offset:
            raise AssertionError(f"pre-project production policy moved: {contract}")
    for declaration in (
        'set(ISAAC_GENERATED_DIR "" CACHE PATH',
        'set(ISAAC_PE_PATH "" CACHE FILEPATH',
        'option(ISAAC_VITA_SCAFFOLD_ONLY',
    ):
        if production.count(declaration) != 1 or production.index(declaration) > project_offset:
            raise AssertionError(f"pre-project cache declaration moved: {declaration}")
    if "isaac_validate_cmake_list_path" in production:
        raise AssertionError("production retained the withdrawn broad path validator")
    if re.search(r"\bif\(WIN32\)", (
            HERE / "literal_glob_path.cmake").read_text(encoding="utf-8")):
        raise AssertionError("path helper uses target WIN32 instead of host predicate")
    if "if(CMAKE_HOST_WIN32)" not in (
            HERE / "literal_glob_path.cmake").read_text(encoding="utf-8"):
        raise AssertionError("path helper lost the host filesystem predicate")
    production_call = (
        "isaac_literal_directory_files(\n"
        "    \"${ISAAC_GENERATED_DIR}\" ISAAC_GENERATED_DIRECTORY_FILES)"
    )
    if production_call not in production:
        raise AssertionError("production CMake lost literal generated enumeration")


def main() -> None:
    cmake = shutil.which("cmake")
    ninja = shutil.which("ninja")
    if cmake is None or ninja is None:
        raise SystemExit("cmake and ninja are required")
    version_text = run([cmake, "--version"]).stdout.splitlines()[0]
    match = re.search(r"([0-9]+)[.]([0-9]+)[.]([0-9]+)", version_text)
    if match is None or tuple(map(int, match.groups())) < (3, 28, 0):
        raise SystemExit(f"CMake >=3.28 is required, got: {version_text}")
    assert_production_contract()
    parity_count = assert_python_cmake_parity(cmake)

    with tempfile.TemporaryDirectory(prefix="isaac-path-policy-") as temporary:
        base = Path(temporary).resolve()
        if build_vita._strict_absolute_path_problem(base.as_posix()) is not None:
            raise AssertionError(f"temporary root is not strict-portable: {base}")
        production_missing_sdk_red(cmake, base)
        result = actual_ninja_regression(cmake, ninja, base)
        ordinary = base / "ordinary-generated"
        populate_generated(ordinary)
        require_pass(glob_probe(cmake, ordinary), "ordinary literal enumeration")
        hostile = base / ("generated_[left]_]rev[" +
                          ("_*_?" if os.name != "nt" else ""))
        require_pass(glob_probe(cmake, hostile), "hostile literal enumeration")

    print(
        "portable build path policy: PASS; "
        f"cmake={version_text!r} parity_cases={parity_count} "
        f"physical_star_question={str(result['physical_star_question']).lower()} "
        "configure=PASS object=1 header=PASS verbatim_copy=PASS "
        "auto_reconfigure=PASS extra=RED missing=RED restore=PASS "
        "production_missing_sdk=RED_PRE_PROJECT"
    )


if __name__ == "__main__":
    main()
