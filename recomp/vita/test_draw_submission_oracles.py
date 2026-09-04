#!/usr/bin/env python3
"""Host-only oracles for the batched Isaac draw-submission changes."""

from __future__ import annotations

import argparse
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path


STOCK_COMMIT = "73dd57a8857f89f2353881c6de5891959c5c1983"
SETTERS = (
    "sceGxmSetVertexProgram",
    "sceGxmSetFragmentProgram",
    "sceGxmSetFragmentTexture",
)


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


def run(command: list[str], *, cwd: Path | None = None) -> None:
    subprocess.run(command, cwd=cwd, check=True)


def compile_oracles(vita: Path, temporary: Path) -> None:
    cc = compiler()
    runtime = vita.parent / "runtime"
    recipe = vita / "vitagl-stock-reference"
    sources = (
        (
            runtime / "kage_vita_canonical_quads_oracle.c",
            runtime,
            "canonical-quad",
            (),
        ),
        (
            recipe / "gxm_state_policy_oracle.c",
            recipe,
            "gxm-state-quiet",
            (),
        ),
        (
            recipe / "gxm_state_policy_oracle.c",
            recipe,
            "gxm-state-profile",
            ("-DHAVE_ISAAC_PHASE_PROFILE=1",),
        ),
        (
            recipe / "gpu_draw_policy_oracle.c",
            recipe,
            "existing-gpu-draw",
            (),
        ),
    )
    for source, include, name, extra_flags in sources:
        executable = temporary / (name + (".exe" if os.name == "nt" else ""))
        run([
            cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            *extra_flags, f"-I{include}", str(source), "-o", str(executable),
        ])
        run([str(executable)])


def verify_stock_patch(vita: Path, stock_repo: Path, temporary: Path) -> None:
    archive = subprocess.run(
        ["git", "-C", str(stock_repo), "archive", STOCK_COMMIT],
        check=True,
        stdout=subprocess.PIPE,
    ).stdout
    source = temporary / "stock"
    source.mkdir()
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as bundle:
        for member in bundle.getmembers():
            target = (source / member.name).resolve()
            if source.resolve() not in target.parents and target != source.resolve():
                raise AssertionError(f"unsafe stock archive member: {member.name}")
        bundle.extractall(source)

    recipe = vita / "vitagl-stock-reference"
    patches = (
        recipe / "0001-deterministic-build-and-init-oob.patch",
        recipe / "0002-exact-gpu-draw-optimizations.patch",
        recipe / "0003-exact-coloroffset-gpu-optimizations.patch",
    )
    for patch in patches:
        run(["git", "apply", "--check", str(patch)], cwd=source)
        run(["git", "apply", str(patch)], cwd=source)

    shadow_source = source / "source" / "isaac_gxm_state_shadow.c"
    shadow_header = source / "source" / "isaac_gxm_state_shadow.h"
    if not shadow_source.is_file() or not shadow_header.is_file():
        raise AssertionError("state-shadow sources did not materialize")
    for path in (source / "source").glob("*.c"):
        text = path.read_text(encoding="utf-8")
        if any(setter in text for setter in SETTERS):
            if '#include "shared.h"' not in text:
                raise AssertionError(f"raw setter escapes shared.h: {path.name}")
    gxm = (source / "source" / "gxm.c").read_text(encoding="utf-8")
    if gxm.count("isaacGxmStateBeginScene(gxm_context, r);") != 2:
        raise AssertionError("not every stock main-context BeginScene is intercepted")
    if "isaacGxmStateEndScene(gxm_context);" not in gxm:
        raise AssertionError("stock main-context EndScene is not intercepted")
    draw = (source / "source" / "draw.c").read_text(encoding="utf-8")
    start = draw.index("GLboolean vglIsaacDrawCanonicalQuads(")
    end = draw.index("void glDrawElements(", start)
    endpoint = draw[start:end]
    if "vgl_log" in endpoint or "printf(" in endpoint or \
            "sceKernelGetProcessTime" in endpoint:
        raise AssertionError("canonical fast path gained per-draw logging/timing")

    header = (source / "source" / "vitaGL.h").read_text(encoding="utf-8")
    for name, expected_fields in (
        ("vglIsaacGpuDrawStats", 31),
        ("vglIsaacColorOffsetStats", 24),
    ):
        start_marker = f"typedef struct {name} {{"
        end_marker = f"}} {name};"
        start = header.index(start_marker)
        end = header.index(end_marker, start)
        field_count = header[start:end].count("uint32_t ")
        if field_count != expected_fields:
            raise AssertionError(
                f"{name} ABI drifted: {field_count} uint32_t fields, "
                f"expected {expected_fields}"
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path)
    parser.add_argument("--generated", type=Path)
    parser.add_argument("--stock-repo", type=Path)
    args = parser.parse_args()
    vita = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="isaac-draw-submission-") as name:
        temporary = Path(name)
        compile_oracles(vita, temporary)
        if args.stock_repo:
            verify_stock_patch(vita, args.stock_repo, temporary)
    run([sys.executable, str(vita / "test_vitagl_stock_reference_recipe.py")])
    run([sys.executable, str(vita / "test_kage_vita_phase_profile.py")])
    cmake = shutil.which("cmake")
    if cmake:
        run([cmake, "-P", str(vita / "test_kage_vita_stock_reference_cmake.cmake")])
    if args.pe:
        command = [
            sys.executable,
            str(vita.parent / "test_kage_canonical_quad_callsites.py"),
            "--pe", str(args.pe),
        ]
        if args.generated:
            command.extend(["--generated", str(args.generated)])
        run(command)
    elif args.generated:
        raise AssertionError("--generated requires --pe")
    print("Isaac draw-submission host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
