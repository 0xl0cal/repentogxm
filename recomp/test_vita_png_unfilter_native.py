#!/usr/bin/env python3
"""Build and run the standalone native PNG-unfilter differential oracle."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
IMPLEMENTATION = HERE / "runtime" / "host_vita_png_unfilter_native.c"
ORACLE = HERE / "runtime" / "host_vita_png_unfilter_native_oracle.c"
GUEST_SHIM = HERE / "runtime" / "host_vita_png_unfilter_guest.c"
GUEST_ORACLE = HERE / "runtime" / "host_vita_png_unfilter_guest_oracle.c"
ENABLED_RESULT = "PNG native unfilter oracle: PASS; cases=433613; mode=enabled"
DISABLED_RESULT = "PNG native unfilter oracle: PASS; cases=2; mode=default-off"
GUEST_RESULT = (
    "PNG native guest shim oracle: PASS; hostile=22; release-fail=2"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(command: list[str], *, env: dict[str, str] | None = None) -> str:
    result = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}): {command!r}\n{result.stdout}"
        )
    return result.stdout


def find_host_compiler(explicit: str | None) -> str:
    if explicit:
        return explicit
    if os.environ.get("CC"):
        return os.environ["CC"]
    for candidate in ("clang", "cc", "gcc"):
        found = shutil.which(candidate)
        if found:
            return found
    raise AssertionError("no GCC-compatible host compiler found; pass --cc")


def compile_host(
    compiler: str, output: Path, *, enabled: bool, sanitized: bool = False
) -> None:
    command = [
        compiler,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-O1" if sanitized else "-O2",
    ]
    if enabled:
        command.append("-DISAAC_VITA_PNG_NATIVE_UNFILTER=1")
    if sanitized:
        command.extend(
            ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        )
    command.extend([str(IMPLEMENTATION), str(ORACLE), "-o", str(output)])
    run(command)


def compile_guest_shim(
    compiler: str, output: Path, *, sanitized: bool = False
) -> None:
    command = [
        compiler,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-O1" if sanitized else "-O2",
        "-DGUEST_STACK_REQUIRED=1",
        "-DISAAC_VITA_HEAP_RANGE_LEASE=1",
        "-DISAAC_VITA_PNG_NATIVE_UNFILTER=1",
        "-DISAAC_VITA_PNG_DECODE_PROFILE=1",
        "-I",
        str(HERE / "runtime"),
    ]
    if sanitized:
        command.extend(
            ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        )
    command.extend([str(GUEST_SHIM), str(GUEST_ORACLE), "-o", str(output)])
    run(command)


def sanitizer_environment(compiler: str) -> dict[str, str]:
    environment = os.environ.copy()
    if os.name != "nt" or "clang" not in Path(compiler).name.lower():
        return environment
    resource_dir = run([compiler, "-print-resource-dir"]).strip()
    runtime_dir = str(Path(resource_dir) / "lib" / "windows")
    environment["PATH"] = runtime_dir + os.pathsep + environment.get("PATH", "")
    return environment


def infer_objdump(compiler: str, explicit: str | None) -> str:
    if explicit:
        return explicit
    path = Path(compiler)
    if "clang" in path.name.lower():
        candidate = path.with_name(
            "llvm-objdump.exe" if os.name == "nt" else "llvm-objdump"
        )
    else:
        candidate = path.with_name(path.name.replace("gcc", "objdump"))
    require(candidate.is_file(), "cannot infer ARM objdump; pass --objdump")
    return str(candidate)


def compile_arm(compiler: str, objdump: str | None, output: Path) -> None:
    command = [compiler]
    if "clang" in Path(compiler).name.lower():
        command.append("--target=armv7-none-eabi")
    command.extend(
        [
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-ffreestanding",
            "-mcpu=cortex-a9",
            "-mfpu=neon",
            "-mfloat-abi=softfp",
            "-mthumb",
            "-D__vita__=1",
            "-DISAAC_VITA_PNG_NATIVE_UNFILTER=1",
            "-c",
            str(IMPLEMENTATION),
            "-o",
            str(output),
        ]
    )
    run(command)
    disassembly = run([infer_objdump(compiler, objdump), "-d", str(output)])
    for instruction in ("vld1.8", "vadd.i8", "vst1.8"):
        require(
            instruction in disassembly,
            f"ARM object has no {instruction} in the bounded Up path",
        )


def cmake_scope_oracle(compiler: str) -> None:
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
    require(cmake is not None and ninja is not None,
            "cmake/ninja are required for PNG native scope oracle")

    production = (HERE / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    start = production.index("  if(ISAAC_VITA_PNG_NATIVE_UNFILTER)\n")
    end = production.index("  if(ISAAC_VITA_PNG_DECODE_PROFILE)\n", start)
    block = production[start:end]
    windows_toolchain: list[str] = []
    if os.name == "nt":
        llvm_rc = Path(compiler).with_name("llvm-rc.exe")
        llvm_ar = Path(compiler).with_name("llvm-ar.exe")
        require(llvm_rc.is_file() and llvm_ar.is_file(),
                "LLVM rc/ar are required for PNG native scope oracle")
        windows_toolchain = [
            f"-DCMAKE_RC_COMPILER={llvm_rc.as_posix()}",
            f"-DCMAKE_AR={llvm_ar.as_posix()}",
            "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
        ]

    with tempfile.TemporaryDirectory(prefix="isaac-png-native-cmake-") as value:
        root = Path(value)
        (root / "guest_0001.c").write_text(
            "typedef struct CPU CPU;\n"
            "void sub_005c6bf0(CPU *__restrict c)\n{(void)c;}\n",
            encoding="utf-8",
        )
        (root / "guest_9999.c").write_text(
            "int unrelated_guest(void){return 0;}\n", encoding="utf-8"
        )
        for name in (
            "host_vita_heap.c",
            "host_vita_png_unfilter_native.c",
            "host_vita_png_unfilter_guest.c",
        ):
            (root / name).write_text(
                f"int fixture_{name.replace('.', '_')}(void){{return 0;}}\n",
                encoding="utf-8",
            )
        (root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(png_native_scope C)\n"
            "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n"
            "set(ISAAC_RUNTIME \"${CMAKE_CURRENT_SOURCE_DIR}\")\n"
            "set(ISAAC_GENERATED_C\n"
            "  \"${CMAKE_CURRENT_SOURCE_DIR}/guest_0001.c\"\n"
            "  \"${CMAKE_CURRENT_SOURCE_DIR}/guest_9999.c\")\n"
            "set(ISAAC_VITA_RUNTIME_SOURCES "
            "\"${ISAAC_RUNTIME}/host_vita_heap.c\")\n" + block +
            "add_library(png_native_scope OBJECT ${ISAAC_GENERATED_C} "
            "${ISAAC_VITA_RUNTIME_SOURCES})\n",
            encoding="utf-8",
        )
        for enabled in (False, True):
            build = root / ("build-on" if enabled else "build-off")
            command = [
                cmake, "-S", str(root), "-B", str(build), "-G", "Ninja",
                f"-DCMAKE_MAKE_PROGRAM={Path(ninja).as_posix()}",
                f"-DCMAKE_C_COMPILER={Path(compiler).as_posix()}",
                "-DISAAC_VITA_PNG_NATIVE_UNFILTER=" +
                ("ON" if enabled else "OFF"),
            ] + windows_toolchain
            run(command)
            run([cmake, "--build", str(build)])
            records = json.loads(
                (build / "compile_commands.json").read_text(encoding="utf-8")
            )
            expected = {"guest_0001.c", "guest_9999.c", "host_vita_heap.c"}
            if enabled:
                expected.update((
                    "host_vita_heap.c",
                    "host_vita_png_unfilter_native.c",
                    "host_vita_png_unfilter_guest.c",
                ))
            actual = {Path(item["file"]).name for item in records}
            require(actual == expected,
                    f"PNG native CMake source closure changed: "
                    f"{sorted(actual)} != {sorted(expected)}")
            for record in records:
                rendered = record.get("command") or " ".join(
                    str(item) for item in record.get("arguments", [])
                )
                name = Path(record["file"]).name
                native_owner = name in {
                    "guest_0001.c", "host_vita_heap.c",
                    "host_vita_png_unfilter_native.c",
                    "host_vita_png_unfilter_guest.c",
                }
                require(
                    ("ISAAC_VITA_PNG_NATIVE_UNFILTER=1" in rendered) ==
                    (enabled and native_owner),
                    f"PNG native CMake macro scope changed for {name}",
                )
                require(
                    ("ISAAC_VITA_HEAP_RANGE_LEASE=1" in rendered) ==
                    (enabled and name == "host_vita_png_unfilter_guest.c"),
                    f"PNG native range-lease scope changed for {name}",
                )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", help="GCC-compatible host C compiler")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--arm-cc", help="optional ARMv7/Vita or Clang compiler")
    parser.add_argument("--objdump", help="objdump matching --arm-cc")
    arguments = parser.parse_args()
    compiler = find_host_compiler(arguments.cc)
    cmake_scope_oracle(compiler)

    with tempfile.TemporaryDirectory(prefix="isaac-png-unfilter-") as temporary:
        root = Path(temporary)
        suffix = ".exe" if os.name == "nt" else ""
        disabled = root / f"oracle-disabled{suffix}"
        enabled = root / f"oracle-enabled{suffix}"
        guest = root / f"guest-shim-oracle{suffix}"
        compile_host(compiler, disabled, enabled=False)
        compile_host(compiler, enabled, enabled=True)
        compile_guest_shim(compiler, guest)
        require(
            run([str(disabled)]).strip() == DISABLED_RESULT,
            "default-off oracle result changed",
        )
        require(
            run([str(enabled)]).strip() == ENABLED_RESULT,
            "enabled differential oracle result changed",
        )
        require(
            run([str(guest)]).strip() == GUEST_RESULT,
            "guest-shim hostile oracle result changed",
        )

        if arguments.sanitize:
            sanitized = root / f"oracle-sanitized{suffix}"
            compile_host(compiler, sanitized, enabled=True, sanitized=True)
            guest_sanitized = root / f"guest-shim-sanitized{suffix}"
            compile_guest_shim(compiler, guest_sanitized, sanitized=True)
            require(
                run([str(sanitized)], env=sanitizer_environment(compiler)).strip()
                == ENABLED_RESULT,
                "sanitized differential oracle result changed",
            )
            require(
                run(
                    [str(guest_sanitized)],
                    env=sanitizer_environment(compiler),
                ).strip() == GUEST_RESULT,
                "sanitized guest-shim oracle result changed",
            )

        if arguments.arm_cc:
            compile_arm(
                arguments.arm_cc, arguments.objdump, root / "png-unfilter-armv7.o"
            )

    modes = "host-off+host-on+guest-shim-hostile"
    if arguments.sanitize:
        modes += "+asan+ubsan"
    if arguments.arm_cc:
        modes += "+armv7-neon-object"
    print(f"Vita PNG native unfilter build oracle: PASS; modes={modes}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
