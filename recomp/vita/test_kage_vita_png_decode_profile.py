#!/usr/bin/env python3
"""Compile and run the hostile aggregate PNG profile oracle on the host."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def compiler() -> str:
    for name in ("cc", "gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    candidates = (
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
        "LLVM" / "bin" / "clang.exe",
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
        "Microsoft Visual Studio" / "2022" / "Community" / "VC" /
        "Tools" / "Llvm" / "x64" / "bin" / "clang.exe",
    )
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise RuntimeError("no host C compiler found")


def cmake_build_id_quote_oracle(vita: Path, cc: str) -> None:
    cmake = shutil.which("cmake")
    ninja = shutil.which("ninja")
    if ninja is None:
        candidate = (
            Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
            "Microsoft Visual Studio" / "2022" / "Community" / "Common7" /
            "IDE" / "CommonExtensions" / "Microsoft" / "CMake" /
            "Ninja" / "ninja.exe"
        )
        if candidate.is_file():
            ninja = str(candidate)
    if cmake is None or ninja is None:
        raise RuntimeError("cmake/ninja are required for PNG build-ID oracle")
    windows_toolchain: list[str] = []
    if os.name == "nt":
        llvm_rc = Path(cc).with_name("llvm-rc.exe")
        llvm_ar = Path(cc).with_name("llvm-ar.exe")
        if not llvm_rc.is_file() or not llvm_ar.is_file():
            raise RuntimeError("LLVM rc/ar are required for PNG build-ID oracle")
        windows_toolchain = [
            f"-DCMAKE_RC_COMPILER={llvm_rc.as_posix()}",
            f"-DCMAKE_AR={llvm_ar.as_posix()}",
            "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
        ]

    quote_block = (
        'set_property(SOURCE\n'
        '      "${ISAAC_RUNTIME}/kage_vita_png_decode_profile.c"\n'
        '      APPEND PROPERTY COMPILE_DEFINITIONS\n'
        '        ISAAC_VITA_PNG_DECODE_PROFILE_BUILD_ID='
        '\\"${ISAAC_VITA_GUEST_LINK_ID}\\")'
    )
    production = (vita / "CMakeLists.txt").read_text(encoding="utf-8")
    if production.count(quote_block) != 1:
        raise AssertionError("production PNG build-ID CMake block drifted")
    section_start = production.index(
        "  if(ISAAC_VITA_PNG_DECODE_PROFILE)\n"
        "    # gen_all proves eight unique frozen owners"
    )
    section_end = production.index(
        "  if(ISAAC_VITA_ANM2_MISSING_LAYER_GUARD)", section_start
    )
    block = production[section_start:section_end]

    with tempfile.TemporaryDirectory(prefix="isaac-png-cmake-quote-") as value:
        root = Path(value)
        (root / "guest_0001.c").write_text(
            "typedef struct CPU CPU;\n"
            "void sub_005a0c50(CPU *__restrict c)\n{(void)c;}\n"
            "void sub_005a0cd0(CPU *__restrict c)\n{(void)c;}\n",
            encoding="utf-8",
        )
        (root / "guest_0002.c").write_text(
            "typedef struct CPU CPU;\n"
            "void sub_005b1500(CPU *__restrict c)\n{(void)c;}\n"
            "void sub_005c5fb0(CPU *__restrict c)\n{(void)c;}\n",
            encoding="utf-8",
        )
        (root / "guest_0003.c").write_text(
            "typedef struct CPU CPU;\n"
            "void sub_005c6fa0(CPU *__restrict c)\n{(void)c;}\n"
            "void sub_005d6d80(CPU *__restrict c)\n{(void)c;}\n",
            encoding="utf-8",
        )
        (root / "guest_9999.c").write_text(
            "int unrelated_guest(void){return 0;}\n", encoding="utf-8"
        )
        (root / "kage_vita_png_decode_profile.c").write_text(
            "#ifndef ISAAC_VITA_PNG_DECODE_PROFILE\n"
            "# error missing PNG profile feature macro\n"
            "#endif\n"
            "#ifndef ISAAC_VITA_PNG_DECODE_PROFILE_BUILD_ID\n"
            "# error missing PNG profile build ID\n"
            "#endif\n"
            "static const char *const build_id = "
            "ISAAC_VITA_PNG_DECODE_PROFILE_BUILD_ID;\n"
            "int png_profile_quote_fixture(void){return build_id[0];}\n",
            encoding="utf-8",
        )
        (root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(png_profile_quote C)\n"
            "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n"
            "set(ISAAC_RUNTIME \"${CMAKE_CURRENT_SOURCE_DIR}\")\n"
            "set(ISAAC_VITA_GUEST_LINK_ID \"perf:png.profile-fixture\")\n"
            "set(ISAAC_GENERATED_C\n"
            "  \"${CMAKE_CURRENT_SOURCE_DIR}/guest_0001.c\"\n"
            "  \"${CMAKE_CURRENT_SOURCE_DIR}/guest_0002.c\"\n"
            "  \"${CMAKE_CURRENT_SOURCE_DIR}/guest_0003.c\"\n"
            "  \"${CMAKE_CURRENT_SOURCE_DIR}/guest_9999.c\")\n"
            "set(ISAAC_VITA_RUNTIME_SOURCES)\n" + block +
            "add_library(png_profile_quote OBJECT ${ISAAC_GENERATED_C} "
            "${ISAAC_VITA_RUNTIME_SOURCES})\n",
            encoding="utf-8",
        )
        for enabled in (False, True):
            build = root / ("build-on" if enabled else "build-off")
            configure_command = [
                    cmake, "-S", str(root), "-B", str(build), "-G", "Ninja",
                    f"-DCMAKE_MAKE_PROGRAM={Path(ninja).as_posix()}",
                    f"-DCMAKE_C_COMPILER={Path(cc).as_posix()}",
                    "-DISAAC_VITA_PNG_DECODE_PROFILE=" +
                    ("ON" if enabled else "OFF"),
                ] + windows_toolchain
            configured = subprocess.run(
                configure_command,
                check=False, capture_output=True, text=True,
            )
            if configured.returncode != 0:
                raise AssertionError(
                    "PNG scope fixture configure failed: "
                    f"stdout={configured.stdout!r} "
                    f"stderr={configured.stderr!r}"
                )
            subprocess.run(
                [cmake, "--build", str(build)],
                check=True, capture_output=True, text=True,
            )
            commands = json.loads(
                (build / "compile_commands.json").read_text(encoding="utf-8")
            )
            expected_names = {
                "guest_0001.c", "guest_0002.c", "guest_0003.c",
                "guest_9999.c",
            }
            if enabled:
                expected_names.add("kage_vita_png_decode_profile.c")
            if (not isinstance(commands, list) or
                    {Path(item["file"]).name for item in commands} !=
                    expected_names):
                raise AssertionError("PNG CMake compile closure changed")
            for record in commands:
                rendered = record.get("command")
                if not isinstance(rendered, str):
                    arguments = record.get("arguments")
                    if not isinstance(arguments, list):
                        raise AssertionError("PNG scope fixture has no command")
                    rendered = " ".join(str(item) for item in arguments)
                name = Path(record["file"]).name
                owns_hook = name in {
                    "guest_0001.c", "guest_0002.c", "guest_0003.c",
                    "kage_vita_png_decode_profile.c",
                }
                if ("ISAAC_VITA_PNG_DECODE_PROFILE=1" in rendered) != (
                        enabled and owns_hook):
                    raise AssertionError(
                        f"PNG feature macro scope changed: {name}: {rendered}"
                    )
                quoted = (
                    'ISAAC_VITA_PNG_DECODE_PROFILE_BUILD_ID='
                    '\\"perf:png.profile-fixture\\"'
                ) in rendered
                if quoted != (
                        enabled and name ==
                        "kage_vita_png_decode_profile.c"):
                    raise AssertionError(
                        f"PNG build-ID scope/quoting changed: {name}: {rendered}"
                    )


def main() -> int:
    vita = Path(__file__).resolve().parent
    runtime = vita.parent / "runtime"
    cc = compiler()
    cmake_build_id_quote_oracle(vita, cc)
    with tempfile.TemporaryDirectory(prefix="isaac-png-profile-") as value:
        executable = Path(value) / (
            "png-profile.exe" if os.name == "nt" else "png-profile"
        )
        command = [
            cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-DISAAC_KAGE_VITA_PNG_DECODE_PROFILE_ORACLE=1",
            "-DISAAC_VITA_PNG_DECODE_PROFILE=1",
            "-DISAAC_VITA_PNG_DECODE_PROFILE_BUILD_ID=\""
            + "p" * 64 + "\"",
            f"-I{runtime}",
            str(runtime / "kage_vita_png_decode_profile.c"),
            str(runtime / "kage_vita_png_decode_profile_oracle.c"),
            "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        completed = subprocess.run(
            [str(executable)], check=False, text=True, capture_output=True,
        )
        if completed.returncode != 0:
            raise AssertionError(
                "PNG profile oracle failed: "
                f"stdout={completed.stdout!r} stderr={completed.stderr!r}"
            )
        if "Vita PNG decode aligned cohort profiler oracle: PASS" not in \
                completed.stdout:
            raise AssertionError(f"unexpected oracle output: {completed.stdout!r}")
    print(
        "Vita PNG decode aligned cohort profiler host/CMake ON-OFF scope oracle: "
        "PASS"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
