#!/usr/bin/env python3
"""Compile/run and source-check the bounded Utero II world-seam receipt."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


def compiler() -> str:
    for name in ("cc", "gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    for candidate in (
        Path(r"C:\Program Files\LLVM\bin\clang.exe"),
        Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community") /
        "VC/Tools/Llvm/x64/bin/clang.exe",
    ):
        if candidate.is_file():
            return str(candidate)
    raise RuntimeError("no host C compiler found")


def run(command: list[str]) -> str:
    completed = subprocess.run(
        command, check=False, text=True, capture_output=True
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed ({completed.returncode}): {command!r}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed.stdout


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"void {re.escape(name)}\([^)]*\)\s*\{{(?P<body>.*?)\n\}}",
        source,
        re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"missing function body: {name}")
    return match.group("body")


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    runtime = root / "recomp" / "runtime"
    source_path = runtime / "kage_vita_world_seam_diag.c"
    header_path = runtime / "kage_vita_world_seam_diag.h"
    oracle_path = runtime / "kage_vita_world_seam_diag_oracle.c"
    source = source_path.read_text(encoding="utf-8")
    header = header_path.read_text(encoding="utf-8")
    backend = (runtime / "gl_vita_backend.c").read_text(encoding="utf-8")
    cmake = (root / "recomp" / "vita" / "CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    wrapper = (root / "tools" / "build_vita.py").read_text(encoding="utf-8")
    raw_gate = (root / "recomp" / "vita" /
                "vita_raw_allocator_gate.py").read_text(encoding="utf-8")

    if "#if defined(ISAAC_VITA_WORLD_SEAM_DIAG)" not in header:
        raise AssertionError("world-seam header lost its default-off split")
    for name in (
        "reset", "note_active_texture", "note_bind_texture",
        "note_gen_texture", "note_delete_texture", "note_tex_image",
        "note_bind_framebuffer", "note_delete_framebuffer",
        "note_framebuffer_texture", "note_use_program", "note_viewport",
        "note_clear_color", "note_clear", "note_draw",
    ):
        macro = f"#define kage_vita_world_seam_diag_{name}("
        if header.count(macro) != 1 or "((void)0)" not in \
                header[header.index(macro):header.index("\n", header.index(macro))]:
            raise AssertionError(f"default-off no-op macro changed: {name}")

    for name in (
        "note_active_texture", "note_bind_texture", "note_gen_texture",
        "note_delete_texture", "note_tex_image", "note_bind_framebuffer",
        "note_delete_framebuffer", "note_framebuffer_texture",
        "note_use_program", "note_viewport", "note_clear_color",
        "note_clear", "note_draw", "reset",
    ):
        call = f"kage_vita_world_seam_diag_{name}("
        if backend.count(call) != 1:
            raise AssertionError(f"GL state hook census changed: {name}")
    draw_note = function_body(source, "kage_vita_world_seam_diag_note_draw")
    for forbidden in (
        "WORLD_SEAM_LOG", "isaac_vita_log", "printf", "sceKernel",
        "malloc", "calloc", "realloc", "free", "time", "clock",
        "sleep", "delay",
    ):
        if forbidden in draw_note:
            raise AssertionError(f"draw hot note acquired forbidden work: {forbidden}")
    if source.count("world_seam_log(\"") != 4:
        raise AssertionError("world-seam receipt is not exactly four lines")
    for point in ("begin", "room", "lua", "late"):
        if source.count(f'world_seam_log("{point}")') != 1:
            raise AssertionError(f"world-seam point changed: {point}")
    if "WORLD_SEAM_CONTROL_LIMIT       1u" not in source or \
            "WORLD_SEAM_TARGET_LIMIT        2u" not in source:
        raise AssertionError("world-seam process-lifetime quota changed")

    for needle in (
        "option(ISAAC_VITA_WORLD_SEAM_DIAG",
        '"${ISAAC_RUNTIME}/kage_vita_world_seam_diag.c"',
        'REGEX "^void sub_002ce770\\\\(CPU \\\\*__restrict c\\\\)$"',
        "ISAAC_VITA_WORLD_SEAM_OWNER_COUNT EQUAL 1",
        "ISAAC_VITA_WORLD_SEAM_DIAG_BUILD_ID=",
    ):
        if needle not in cmake:
            raise AssertionError(f"CMake world-seam contract lost: {needle}")
    for needle in (
        '"-DISAAC_VITA_WORLD_SEAM_DIAG=',
        '"ISAAC_VITA_WORLD_SEAM_DIAG": world_seam_diag',
        '"--world-seam-diag"',
        "--release rejects the diagnostic --world-seam-diag feature",
    ):
        if needle not in wrapper:
            raise AssertionError(f"canonical build world-seam contract lost: {needle}")
    for needle in (
        'WORLD_SEAM_DIAG_CACHE_KEY = "ISAAC_VITA_WORLD_SEAM_DIAG"',
        'runtime.add("kage_vita_world_seam_diag.c")',
        "def verify_world_seam_diag_compile_scope(",
        "verify_world_seam_diag_compile_scope(records, cache)",
    ):
        if needle not in raw_gate:
            raise AssertionError(f"raw gate world-seam contract lost: {needle}")

    with tempfile.TemporaryDirectory(prefix="isaac-world-seam-") as temp_value:
        temp = Path(temp_value)
        suffix = ".exe" if os.name == "nt" else ""
        executable = temp / f"world_seam_oracle{suffix}"
        output = run([
            compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-DISAAC_VITA_WORLD_SEAM_DIAG=1",
            "-DISAAC_KAGE_VITA_WORLD_SEAM_DIAG_ORACLE=1",
            '-DISAAC_VITA_WORLD_SEAM_DIAG_BUILD_ID="0123456789abcdefghijklmn"',
            f"-I{runtime}", str(source_path), str(oracle_path),
            "-o", str(executable),
        ])
        if output:
            raise AssertionError(f"compiler emitted unexpected stdout: {output}")
        oracle_output = run([str(executable)])

        off_source = temp / "world_seam_off.c"
        off_source.write_text(
            '#include "kage_vita_world_seam_diag.h"\n'
            "static int touched;\n"
            "unsigned side_effect(void) { ++touched; return 1u; }\n"
            "int main(void) {\n"
            "  kage_vita_world_seam_diag_reset();\n"
            "  kage_vita_world_seam_diag_note_draw(side_effect());\n"
            "  kage_vita_world_seam_diag_note_bind_texture(0u, side_effect());\n"
            "  return touched != 0;\n"
            "}\n",
            encoding="utf-8",
        )
        off_executable = temp / f"world_seam_off{suffix}"
        output = run([
            compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{runtime}", str(off_source), "-o", str(off_executable),
        ])
        if output:
            raise AssertionError(
                f"default-off compiler emitted unexpected stdout: {output}"
            )
        run([str(off_executable)])

    if not re.fullmatch(
            r"world-seam diagnostic oracle: PASS \([0-9]+ checks\)\n",
            oracle_output):
        raise AssertionError(f"unexpected oracle output: {oracle_output!r}")
    print(oracle_output, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
