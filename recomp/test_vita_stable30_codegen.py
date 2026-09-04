#!/usr/bin/env python3
"""Frozen-J835 proof for the opt-in stable 30-Hz presentation branches."""

from __future__ import annotations

import argparse
import copy
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import gen_all as G  # noqa: E402
from image import DEFAULT_BASE, Image  # noqa: E402


PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
VITA_IMAGE_BASE = 0x98000000
APP_ROOT = 0x0048BC50
MANAGER_ROOT = 0x004B0010
MANAGER_END = 0x004B0600
RENDER_ROOT = 0x004B0600
RENDER_END = 0x004B1090
MANAGER_SWITCH_ENTRIES = (
    0x004B0298, 0x004B047F, 0x004B0486, 0x004B0493, 0x004B049E,
)
MANAGER_SWITCH_TARGETS = (
    0x004B047F, 0x004B0298, 0x004B0486, 0x004B049E, 0x004B0493,
)
RENDER_SWITCH_ENTRIES = (
    0x004B07D6, 0x004B07E6, 0x004B07F0, 0x004B09A1, 0x004B09B4,
)
RENDER_SWITCH_TARGETS = (
    0x004B07E6, 0x004B07D6, 0x004B07F0, 0x004B09B4, 0x004B09A1,
)
MARKER = (
    "#if defined(__vita__) && "
    "defined(ISAAC_VITA_STABLE_30_PRESENTATION)\n"
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def switch_info(
    image: Image, root: int, end: int, entries: tuple[int, ...],
    site: int, targets: tuple[int, ...],
) -> dict[int, dict[str, object]]:
    actual_entries, edges, rejected, tables = G.discover_jump_tables(
        image, root, owner_end=end
    )
    expected = {site: targets}
    if actual_entries != entries or edges != expected or tables != expected or rejected:
        raise AssertionError(
            f"switch identity changed at 0x{root:08x}: "
            f"entries={actual_entries!r} edges={edges!r} rejected={rejected!r}"
        )
    return {
        root: {
            "entries": actual_entries,
            "edges": edges,
            "rejected": rejected,
            "tables": tables,
        }
    }


def translate(
    image: Image, pin: Image, root: int,
    switches: dict[int, dict[str, object]],
) -> dict[str, object]:
    result = G._translate_function(
        image, {"rva": root}, None, switches, pin_img=pin
    )
    if result.get("stub") is not None:
        raise AssertionError(
            f"stable30 owner 0x{root:08x} became a stub: {result['stub']}"
        )
    return result


def compiler() -> str:
    if os.environ.get("CC"):
        return os.environ["CC"]
    for candidate in ("clang", "cc", "gcc"):
        found = shutil.which(candidate)
        if found:
            return found
    if os.name == "nt":
        installed = Path(os.environ.get(
            "ProgramFiles", r"C:\Program Files"
        )) / "LLVM" / "bin" / "clang.exe"
        if installed.is_file():
            return str(installed)
    raise AssertionError("no GCC-compatible host compiler found")


def compile_manager_owner(text: str, image_base: int, stable30: bool) -> None:
    """Compile the actual fresh Manager body, not a reduced label fixture."""
    symbols = sorted(set(re.findall(
        r"\bsub_[0-9a-f]{8}(?=\(c\))", text
    )))
    declarations = "\n".join(
        f"void {symbol}(CPU *__restrict c);" for symbol in symbols
    )
    source = (
        "/* Fresh frozen-PE Manager owner compile closure. */\n"
        "#include <math.h>\n"
        "#include \"guest.h\"\n\n"
        f"{declarations}\n\n{text}"
    )
    with tempfile.TemporaryDirectory(prefix="isaac-stable30-manager-") as value:
        root = Path(value)
        unit = root / "manager.c"
        output = root / "manager.o"
        unit.write_text(source, encoding="utf-8", newline="\n")
        feature_defines = ["-D__vita__=1"]
        if stable30:
            feature_defines.append(
                "-DISAAC_VITA_STABLE_30_PRESENTATION=1"
            )
        command = [
            compiler(), "-std=gnu11", "-O2", "-Wall", "-Wextra",
            "-Wno-unused-label", "-Wno-unused-variable",
            "-Wno-parentheses-equality", *feature_defines,
            f"-DGUEST_IMAGE_BASE=0x{image_base:08x}U",
            f"-I{HERE / 'runtime'}", "-c", str(unit), "-o", str(output),
        ]
        completed = subprocess.run(
            command, check=False, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True,
        )
        if completed.returncode != 0 or not output.is_file():
            raise AssertionError(
                "fresh stable30 Manager owner did not compile:\n"
                f"command={command!r}\n{completed.stdout}"
            )


def blocks(text: str) -> list[str]:
    result = []
    position = 0
    while True:
        begin = text.find(MARKER, position)
        if begin < 0:
            return result
        end = text.find("#endif\n", begin)
        if end < 0:
            raise AssertionError("unterminated stable30 feature block")
        result.append(text[begin:end + len("#endif\n")])
        position = end + len("#endif\n")


def verify_feature_block(block: str) -> None:
    for forbidden in (
        "st8(", "st16(", "st32(", "st64(", "sub_002cdcf0(c);",
        "kage_pc_backend_present(",
    ):
        if forbidden in block:
            raise AssertionError(
                f"stable30 block acquired guest mutation/call: {forbidden}"
            )
    if "c->" in block and "c->ecx" not in block and "c->esi" not in block:
        raise AssertionError("stable30 block acquired an unexpected CPU field")


def verify_app(text: str) -> None:
    feature = blocks(text)
    if len(feature) != 1:
        raise AssertionError(f"Application has {len(feature)} stable30 blocks")
    verify_feature_block(feature[0])
    for marker in (
        "kage_pc_backend_stable30_bypass_limiter(void)",
        "if (kage_pc_backend_stable30_bypass_limiter())",
        "goto L_0048bd40;",
    ):
        if feature[0].count(marker) != 1:
            raise AssertionError(f"Application stable30 marker changed: {marker}")
    hook = text.index("if (kage_pc_backend_stable30_bypass_limiter())")
    limiter = text.index("/* 0048be3e  call 0x570040 */")
    backedge = text.index("/* 0048bf0b  jmp 0x48bd40 */")
    if not hook < limiter < backedge:
        raise AssertionError("stable30 no-op pacing moved outside original limiter")


def verify_manager(text: str) -> None:
    feature = blocks(text)
    if len(feature) != 1:
        raise AssertionError(f"Manager has {len(feature)} stable30 blocks")
    verify_feature_block(feature[0])
    if feature[0].count(
            "kage_pc_backend_stable30_manager_entry(c->ecx)") != 1 or \
            feature[0].count("goto L_004b0043;") != 1:
        raise AssertionError("Manager stable30 guarded non-full edge changed")
    if text.count("L_004b0043:") != 1:
        raise AssertionError("Manager stable30 guard label closure changed")
    hook = text.index("kage_pc_backend_stable30_manager_entry(c->ecx)")
    parity = text.index("/* 004b0027  mov eax, ecx */")
    guard_label = text.index("L_004b0043:")
    first_guard = text.index(
        "/* 004b0043  cmp byte ptr [edi + 0x4a26c], 0 */"
    )
    second_guard = text.index(
        "/* 004b004c  cmp byte ptr [edi + 0x4a26d], 0 */"
    )
    second_guard_edge = text.index("/* 004b0053  je 0x4b05e1 */")
    fast_increment = text.index(
        "/* 004b0059  lea eax, [ecx + 1] */"
    )
    game_call = text.index("sub_002cdcf0(c);")
    if not hook < parity < guard_label < first_guard < second_guard < \
            second_guard_edge < fast_increment < game_call:
        raise AssertionError(
            "Manager stable30 edge moved past frozen parity/native guards"
        )
    if text.count("sub_002cdcf0(c);") != 1:
        raise AssertionError("stable30 duplicated or removed Game::Update")


def verify_render(text: str) -> None:
    feature = blocks(text)
    if len(feature) != 1:
        raise AssertionError(f"Render has {len(feature)} stable30 blocks")
    verify_feature_block(feature[0])
    if feature[0].count(
            "kage_pc_backend_stable30_plan_render(") != 2 or \
            feature[0].count("c->esi + 0x0004a264U") != 1 or \
            feature[0].count("goto L_004b1089;") != 1:
        raise AssertionError("Render stable30 no-op edge changed")
    hook = text.index(
        "if (kage_pc_backend_stable30_plan_render("
    )
    parity = text.index(
        "/* 004b0614  test byte ptr [esi + 0x4a264], 1 */"
    )
    no_op = text.index("L_004b1089:")
    if not hook < parity < no_op:
        raise AssertionError("stable30 interpolation skip moved past parity/no-op")
    # Manager::Render owns three mutually exclusive state-specific Present
    # leaves.  The stable branch reaches the shared no-op epilogue before all
    # three; it must neither synthesize nor remove any of them.
    if text.count("if (!kage_pc_backend_present())") != 3:
        raise AssertionError("stable30 duplicated or removed a real Present leaf")


def expect_pin_failure(
    image: Image, pin: Image, switches: dict[int, dict[str, object]],
    root: int, rva: int,
) -> None:
    mutated = copy.copy(pin)
    memory = bytearray(pin.mem)
    memory[rva] ^= 1
    mutated.mem = bytes(memory)
    try:
        translate(image, mutated, root, switches)
    except RuntimeError:
        return
    raise AssertionError(f"mutated stable30 seam at 0x{rva:08x} was accepted")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=Path)
    arguments = parser.parse_args()
    path = arguments.pe.resolve()
    if path.stat().st_size != PE_SIZE or sha256(path) != PE_SHA256:
        raise AssertionError("stable30 codegen test received a different PE")

    for name, expected in {
        "APPLICATION_MAIN_RVA": APP_ROOT,
        "KAGE_PC_LIMITER_ENTRY_RVA": 0x0048BE3E,
        "KAGE_PC_LOOP_HEAD_RVA": 0x0048BD40,
        "VITA_FULLSPEED_MANAGER_ENTRY_OBSERVE_RVA": 0x004B0027,
        "VITA_STABLE30_MANAGER_GUARD_CHAIN_RVA": 0x004B0043,
        "VITA_FULLSPEED_MANAGER_FAST_INCREMENT_RVA": 0x004B0059,
        "VITA_FULLSPEED_RENDER_PARITY_OBSERVE_RVA": 0x004B0614,
        "VITA_FULLSPEED_MANAGER_COUNTER_OFFSET": 0x0004A264,
    }.items():
        if getattr(G, name) != expected:
            raise AssertionError(f"stable30 generator constant changed: {name}")
    if G.VITA_STABLE30_MANAGER_GUARD_CHAIN_MACHINE != bytes.fromhex(
            "80bf6ca2040000740d80bf6da20400000f8488050000"):
        raise AssertionError("stable30 native Manager guard bytes changed")

    G.EXE = str(path)
    pin = Image(str(path), DEFAULT_BASE)
    manager_switch = switch_info(
        pin, MANAGER_ROOT, MANAGER_END, MANAGER_SWITCH_ENTRIES,
        0x004B0291, MANAGER_SWITCH_TARGETS,
    )
    render_switch = switch_info(
        pin, RENDER_ROOT, RENDER_END, RENDER_SWITCH_ENTRIES,
        0x004B07CF, RENDER_SWITCH_TARGETS,
    )

    texts_by_base: dict[int, tuple[str, str, str]] = {}
    for base in (DEFAULT_BASE, VITA_IMAGE_BASE):
        image = Image(str(path), base)
        app = str(translate(image, pin, APP_ROOT, {})["text"])
        manager = str(
            translate(image, pin, MANAGER_ROOT, manager_switch)["text"]
        )
        render = str(
            translate(image, pin, RENDER_ROOT, render_switch)["text"]
        )
        verify_app(app)
        verify_manager(manager)
        verify_render(render)
        compile_manager_owner(manager, base, stable30=False)
        compile_manager_owner(manager, base, stable30=True)
        texts_by_base[base] = (app, manager, render)

    for index in range(3):
        default_blocks = blocks(texts_by_base[DEFAULT_BASE][index])
        vita_blocks = blocks(texts_by_base[VITA_IMAGE_BASE][index])
        if default_blocks != vita_blocks:
            raise AssertionError("stable30 feature block changed with image base")

    mutation_cases = (
        (APP_ROOT, {}, G.KAGE_PC_LIMITER_ENTRY_RVA),
        (APP_ROOT, {}, G.KAGE_PC_LIMITER_EXIT_RVA),
        (MANAGER_ROOT, manager_switch, G.VITA_FULLSPEED_MANAGER_ENTRY_MACHINE_RVA),
        (MANAGER_ROOT, manager_switch, 0x004B0043),
        (MANAGER_ROOT, manager_switch, 0x004B004A),
        (MANAGER_ROOT, manager_switch, 0x004B004C),
        (MANAGER_ROOT, manager_switch, 0x004B0053),
        (MANAGER_ROOT, manager_switch, G.VITA_FULLSPEED_MANAGER_FAST_INCREMENT_RVA),
        (MANAGER_ROOT, manager_switch, G.VITA_FULLSPEED_MANAGER_FINAL_INCREMENT_RVA),
        (RENDER_ROOT, render_switch, G.VITA_FULLSPEED_RENDER_PARITY_MACHINE_RVA),
    )
    for root, switches, rva in mutation_cases:
        expect_pin_failure(pin, pin, switches, root, rva)

    print(
        "Vita stable30 generator pin: PASS; exact J835 Manager parity/native "
        "4a26c/4a26d guards, Render parity/no-op and software-limiter seams; "
        "one Game::Update and three original mutually-exclusive Present "
        "leaves preserved; fresh Manager owner compiles OFF/ON with the "
        "forced guard label; no guest write or synthetic frame; "
        f"bases=2 seam_mutations={len(mutation_cases)} closed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
