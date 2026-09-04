#!/usr/bin/env python3
"""Frozen-J835 proof for the Utero II world-only render receipt seams."""

from __future__ import annotations

import argparse
import copy
import hashlib
import re
from pathlib import Path

import pefile

import build_one as B
import gen_all as G
from gpr_locals import legacy_text  # noqa: E402
from image import DEFAULT_BASE, Image

PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
VITA_IMAGE_BASE = 0x98000000
ROOT = 0x002CE770
END = 0x002D0375
INSNS = 1494
LUA_ROOT = 0x003FFE10
LUA_HELPER = 0x0040EEF0
SITES = (0x002CEC24, 0x002CEC7C, 0x002CEF8E)
EXPECTED_IMPORTS = {
    0x00606170: ("Lua5.3.3r.dll", "lua_settop"),
    0x0060623C: ("Lua5.3.3r.dll", "lua_pcallk"),
    0x00606240: ("Lua5.3.3r.dll", "lua_pushinteger"),
    0x00606248: ("Lua5.3.3r.dll", "luaL_unref"),
    0x0060624C: ("Lua5.3.3r.dll", "lua_rawgeti"),
    0x00606254: ("Lua5.3.3r.dll", "luaL_ref"),
    0x00606258: ("Lua5.3.3r.dll", "lua_pushvalue"),
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def raw_translation(image: Image) -> tuple[object, ...]:
    result = B.translate(image, ROOT, "sub_002ce770", None)
    em, insns, order, body, unsupported, indirect, _members = result
    if unsupported or indirect:
        raise AssertionError(
            f"Game::Render raw translation changed: {unsupported!r} {indirect!r}"
        )
    return em, insns, order, body


def translate(image: Image, pin: Image) -> dict[str, object]:
    result = G._translate_function(
        image, {"rva": ROOT}, None, {}, pin_img=pin
    )
    if result.get("stub") is not None:
        raise AssertionError(f"Game::Render became a stub: {result['stub']}")
    return result


def guarded_blocks(text: str) -> tuple[str, ...]:
    blocks = tuple(re.findall(
        r"#if defined\(__vita__\) && defined\(ISAAC_VITA_WORLD_SEAM_DIAG\)\n"
        r".*?#endif\n",
        text,
        flags=re.DOTALL,
    ))
    if len(blocks) != 4:
        raise AssertionError(f"world-seam guard census changed: {len(blocks)}")
    return blocks


def verify_translation(result: dict[str, object]) -> tuple[str, ...]:
    if result.get("vita_world_seam_diag") is not True or \
            result.get("insns") != INSNS:
        raise AssertionError("Game::Render world-seam selection changed")
    text = str(legacy_text(result["text"]))
    blocks = guarded_blocks(text)
    for block in blocks:
        statement = block.find(
            "    (void)0; /* A generated label must precede a C statement. */"
        )
        declaration = block.find("    extern void kage_vita_world_seam_diag_")
        if statement < 0 or declaration < 0 or statement > declaration:
            raise AssertionError(
                "world-seam declaration is not protected from a generated label"
            )
        for forbidden in (
            "st8(", "st16(", "st32(", "st64(", "glReadPixels",
            "glGet", "sceKernel", "sleep", "delay",
        ):
            if forbidden in block:
                raise AssertionError(
                    f"world-seam generated block acquired forbidden work: {forbidden}"
                )
        if re.search(r"c->[A-Za-z0-9_]+\s*=", block):
            raise AssertionError("world-seam generated block writes CPU state")

    begin_comment = text.index("/* 002cec24  mov ecx, ")
    begin_call = text.index("    kage_vita_world_seam_diag_begin(", begin_comment)
    begin_instruction = text.index("    c->ecx =", begin_call)
    if not begin_comment < begin_call < begin_instruction:
        raise AssertionError("begin receipt no longer precedes Manager::Clear")

    room_comment = text.index("/* 002cec7c  call 0x3ffe10 */")
    room_call = text.index(
        "    kage_vita_world_seam_diag_post_room();", room_comment
    )
    lua_original = text.index(
        "sub_003ffe10(c);", room_call
    )
    lua_call = text.index(
        "    kage_vita_world_seam_diag_post_lua();", lua_original
    )
    after_lua = text.index("/* 002cec81", lua_call)
    if not room_comment < room_call < lua_original < lua_call < after_lua:
        raise AssertionError("Room/Lua receipt ordering changed")

    late_comment = text.index("/* 002cef8e  call 0x20720 */")
    late_original = text.index("sub_00020720(c);", late_comment)
    pre_hud = text.index(
        "    kage_vita_world_seam_diag_pre_hud();", late_original
    )
    hud_setup = text.index("/* 002cef93", pre_hud)
    hud_call = text.index("/* 002cef99  call 0x504610 */", hud_setup)
    if not late_comment < late_original < pre_hud < hud_setup < hud_call:
        raise AssertionError("late/pre-HUD receipt ordering changed")
    return blocks


def verify_imports(path: Path) -> None:
    pe = pefile.PE(str(path), fast_load=False)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    observed = {
        item.address - image_base: (
            descriptor.dll.decode("ascii"),
            item.name.decode("ascii") if item.name is not None else "",
        )
        for descriptor in pe.DIRECTORY_ENTRY_IMPORT
        for item in descriptor.imports
    }
    actual = {rva: observed.get(rva) for rva in EXPECTED_IMPORTS}
    if actual != EXPECTED_IMPORTS:
        raise AssertionError(f"frozen Lua IAT identity changed: {actual!r}")


def expect_pin_failure(
    image: Image, raw: tuple[object, ...], rva: int
) -> None:
    em, insns, order, body = raw
    mutated = copy.copy(image)
    memory = bytearray(image.mem)
    memory[rva] ^= 1
    mutated.mem = bytes(memory)
    try:
        G.vita_world_seam_diag_for_body(
            mutated, ROOT, em, insns, order, body
        )
    except (RuntimeError, G.T.Unsupported) as exc:
        if (not isinstance(exc, G.T.Unsupported) and
                "Game/Room/Lua/overlay/HUD proof changed" not in str(exc)):
            raise AssertionError(
                f"wrong world-seam failure for 0x{rva:08x}: {exc}"
            ) from exc
    else:
        raise AssertionError(f"mutated world-seam pin passed at 0x{rva:08x}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    arguments = parser.parse_args()
    path = arguments.pe.resolve()
    if path.stat().st_size != PE_SIZE or sha256(path) != PE_SHA256:
        raise AssertionError("world-seam codegen test received a different PE")

    expected_constants = {
        "VITA_WORLD_SEAM_DIAG_PE_SIZE": PE_SIZE,
        "VITA_WORLD_SEAM_DIAG_PE_SHA256": PE_SHA256,
        "VITA_WORLD_SEAM_DIAG_ROOT_RVA": ROOT,
        "VITA_WORLD_SEAM_DIAG_END_RVA": END,
        "VITA_WORLD_SEAM_DIAG_INSNS": INSNS,
        "VITA_WORLD_SEAM_DIAG_LUA_ROOT_RVA": LUA_ROOT,
        "VITA_WORLD_SEAM_DIAG_LUA_CALL_HELPER_RVA": LUA_HELPER,
        "VITA_WORLD_SEAM_DIAG_SITES": frozenset(SITES),
        "VITA_WORLD_SEAM_DIAG_LUA_IAT_RVAS": tuple(EXPECTED_IMPORTS),
    }
    for name, expected in expected_constants.items():
        if getattr(G, name) != expected:
            raise AssertionError(f"generator world-seam constant changed: {name}")
    if G.LUA_CALLBACK_NEUTRAL_TRANSLATION.get(
            "vita_world_seam_diag") is not False:
        raise AssertionError("Lua supplemental schema lacks neutral world-seam field")
    verify_imports(path)

    G.EXE = str(path)
    pin = Image(str(path), DEFAULT_BASE)
    raw = raw_translation(pin)
    if not G.vita_world_seam_diag_for_body(pin, ROOT, *raw):
        raise AssertionError("raw frozen Game::Render did not select world seam")

    default_blocks = verify_translation(translate(pin, pin))
    vita_image = Image(str(path), VITA_IMAGE_BASE)
    vita_blocks = verify_translation(translate(vita_image, pin))
    if default_blocks != vita_blocks:
        raise AssertionError("world-seam blocks changed with guest image base")

    unrelated = G._translate_function(
        pin, {"rva": 0x0048BC50}, None, {}, pin_img=pin
    )
    if unrelated.get("vita_world_seam_diag") is not False or \
            "ISAAC_VITA_WORLD_SEAM_DIAG" in str(unrelated.get("text")):
        raise AssertionError("world-seam receipt escaped Game::Render")

    callback = G._translate_function(
        pin, {"rva": 0x003F9370}, None, {}, pin_img=pin
    )
    G.validate_lua_callback_translation(
        {"rva": 0x003F9370, "instruction_count": callback["insns"]},
        callback,
    )
    if callback.get("vita_world_seam_diag") is not False:
        raise AssertionError("Lua supplemental callback inherited world seam")

    for mutation in (
        0x002CE77E,  # incoming Game* identity
        0x002CEC24,  # before Manager::Clear
        0x002CEC6E,  # ordinary Room::Render
        0x002CEC7C,  # post-Room Lua wrapper call
        0x002CED89,  # optional late Surface draw
        0x002CEF8E,  # final pre-HUD helper
        0x002CEF99,  # HUD::Render boundary
        0x003FFE6B,  # Lua pcall helper dispatch
        0x003FFE7B,  # luaL_unref edge
        0x0040EF3B,  # exact lua_pcallk IAT call
    ):
        expect_pin_failure(pin, raw, mutation)

    print(
        "Vita world-seam codegen: PASS; exact Game/Room/Lua/overlay/HUD; "
        "four read-only joins; bases=2; Lua IAT=7; mutations=10 closed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
