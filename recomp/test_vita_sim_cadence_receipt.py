#!/usr/bin/env python3
"""Frozen-J835 codegen proof for the low-overhead Game::Update receipt."""

from __future__ import annotations

import argparse
import copy
import hashlib
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import gen_all as G  # noqa: E402
from image import DEFAULT_BASE, Image  # noqa: E402

PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
VITA_IMAGE_BASE = 0x98000000
GAME_UPDATE_ROOT = 0x002CDCF0
GAME_UPDATE_END = 0x002CE750
GAME_FRAME_OFFSET = 0x001A30DC
GAME_UPDATE_INSNS = 640
GAME_UPDATE_BODY_SHA256 = (
    "462443857f41eb376a22f03c4e4e211e76c67c5898b7f6fbc9146be36815ac5d"
)
GAME_UPDATE_ORDER_SHA256 = (
    "73af861b9cf58481667185a2a08a0c17f84048712b5cf87324802a05fd5c0eb2"
)
GAME_UPDATE_PREFIX = bytes.fromhex(
    "558bec83e4f083ec38a1b4a37a3033c48944243456578bf9"
)
MANAGER_CALL = 0x004B0311
MANAGER_CALL_BYTES = bytes.fromhex("e8dad9e1ff")
LUA_SUPPLEMENTAL_CALLBACK = 0x003F9370
SWITCH_ENTRIES = (
    0x002CDEE3, 0x002CDF18, 0x002CDF37,
    0x002CDF56, 0x002CDFA9, 0x002CE022,
)
SWITCH_TARGETS = (
    0x002CDF18, 0x002CDEE3, 0x002CDF37,
    0x002CDF56, 0x002CDFA9, 0x002CE022,
)
RETURNS = (
    0x002CE074, 0x002CE099, 0x002CE0BE, 0x002CE0EA,
    0x002CE121, 0x002CE167, 0x002CE1C7, 0x002CE362,
    0x002CE74F,
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def switch_info(image: Image) -> dict[int, dict[str, object]]:
    entries, edges, rejected, tables = G.discover_jump_tables(
        image, GAME_UPDATE_ROOT, owner_end=GAME_UPDATE_END
    )
    expected = {0x002CDEDC: SWITCH_TARGETS}
    if entries != SWITCH_ENTRIES or edges != expected or \
            tables != expected or rejected:
        raise AssertionError(
            "Game::Update switch proof changed: "
            f"entries={entries!r} edges={edges!r} rejected={rejected!r}"
        )
    return {
        GAME_UPDATE_ROOT: {
            "entries": entries,
            "edges": edges,
            "rejected": rejected,
            "tables": tables,
        }
    }


def translate(
    image: Image, pin: Image, switches: dict[int, dict[str, object]]
) -> dict[str, object]:
    result = G._translate_function(
        image, {"rva": GAME_UPDATE_ROOT}, None, switches, pin_img=pin
    )
    if result.get("stub") is not None:
        raise AssertionError(f"Game::Update became a stub: {result['stub']}")
    return result


def verify(result: dict[str, object]) -> str:
    if result.get("vita_sim_cadence_receipt") is not True:
        raise AssertionError("Game::Update receipt seam was not selected")
    if result.get("insns") != GAME_UPDATE_INSNS or \
            result.get("switch_entries") != len(SWITCH_ENTRIES) or \
            result.get("switch_sites") != 1:
        raise AssertionError("Game::Update translated shape changed")
    text = str(result["text"])
    marker = (
        "#if defined(__vita__) && "
        "defined(ISAAC_VITA_SIM_CADENCE_RECEIPT)\n"
    )
    if text.count(marker) != 1 or \
            text.count("kage_vita_sim_cadence_note_game_update(") != 2:
        raise AssertionError("receipt block is absent or duplicated")
    begin = text.index(marker)
    end = text.index("#endif\n", begin) + len("#endif\n")
    block = text[begin:end]
    first_instruction = text.index("/* 002cdcf0  push ebp */")
    if not begin < end < first_instruction:
        raise AssertionError("receipt no longer samples incoming Game* at entry")
    if "c->ecx, ld32((uint32_t)(c->ecx + 0x001a30dcU))" not in block or \
            "One read-only sample per real Game::Update invocation" not in block:
        raise AssertionError("receipt Game*/frame observation changed")
    for forbidden in (
        "st8(", "st16(", "st32(", "st64(", "sceKernel", "Printf",
        "scheduler", "delay", "sleep",
    ):
        if forbidden in block:
            raise AssertionError(f"receipt block acquired forbidden work: {forbidden}")
    if re.search(r"c->[a-zA-Z0-9_]+\s*=", block):
        raise AssertionError("receipt block writes guest CPU state")
    if text.count("/* 002ce598  mov esi, dword ptr [edi + 0x1a30dc] */") != 1:
        raise AssertionError("original Game frame increment is absent")
    for site in RETURNS:
        if text.count(f"/* {site:08x}  ret  */") != 1:
            raise AssertionError(f"Game::Update return changed at 0x{site:08x}")
    return text


def expect_pin_failure(
    image: Image, switches: dict[int, dict[str, object]], rva: int
) -> None:
    mutated = copy.copy(image)
    memory = bytearray(image.mem)
    memory[rva] ^= 1
    mutated.mem = bytes(memory)
    try:
        translate(image, mutated, switches)
    except (RuntimeError, AssertionError) as exc:
        if "simulation cadence Game::Update body/caller identity changed" not in \
                str(exc) and "became a stub" not in str(exc):
            raise AssertionError(
                f"wrong failure for mutation 0x{rva:08x}: {exc}"
            ) from exc
    else:
        raise AssertionError(
            f"mutated receipt pin at 0x{rva:08x} was accepted"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    arguments = parser.parse_args()
    path = arguments.pe.resolve()
    if path.stat().st_size != PE_SIZE or sha256(path) != PE_SHA256:
        raise AssertionError("receipt codegen test received a different PE")

    expected_constants = {
        "VITA_SIM_CADENCE_GAME_UPDATE_ROOT_RVA": GAME_UPDATE_ROOT,
        "VITA_SIM_CADENCE_GAME_UPDATE_END_RVA": GAME_UPDATE_END,
        "VITA_SIM_CADENCE_GAME_FRAME_OFFSET": GAME_FRAME_OFFSET,
        "VITA_SIM_CADENCE_GAME_UPDATE_INSNS": GAME_UPDATE_INSNS,
        "VITA_SIM_CADENCE_GAME_UPDATE_BODY_SHA256": GAME_UPDATE_BODY_SHA256,
        "VITA_SIM_CADENCE_GAME_UPDATE_ORDER_SHA256": GAME_UPDATE_ORDER_SHA256,
        "VITA_SIM_CADENCE_GAME_UPDATE_PREFIX_MACHINE": GAME_UPDATE_PREFIX,
        "VITA_SIM_CADENCE_MANAGER_CALL_RVA": MANAGER_CALL,
        "VITA_SIM_CADENCE_MANAGER_CALL_MACHINE": MANAGER_CALL_BYTES,
    }
    for name, expected in expected_constants.items():
        if getattr(G, name) != expected:
            raise AssertionError(f"generator receipt constant changed: {name}")
    if G.LUA_CALLBACK_NEUTRAL_TRANSLATION.get(
            "vita_sim_cadence_receipt") is not False:
        raise AssertionError(
            "Lua supplemental callback translation lacks neutral cadence field"
        )

    G.EXE = str(path)
    pin = Image(str(path), DEFAULT_BASE)
    call = pin.code_at(MANAGER_CALL, len(MANAGER_CALL_BYTES))
    target = (
        MANAGER_CALL + len(call) +
        int.from_bytes(call[1:], "little", signed=True)
    ) & 0xFFFFFFFF
    if call != MANAGER_CALL_BYTES or call[:1] != b"\xe8" or \
            target != GAME_UPDATE_ROOT:
        raise AssertionError("real Manager -> Game::Update call changed")
    switches = switch_info(pin)

    default_text = verify(translate(pin, pin, switches))
    vita_image = Image(str(path), VITA_IMAGE_BASE)
    vita_text = verify(translate(vita_image, pin, switches))
    receipt_block = re.compile(
        r"#if defined\(__vita__\).*?ISAAC_VITA_SIM_CADENCE_RECEIPT\).*?"
        r"#endif\n",
        re.DOTALL,
    )
    if receipt_block.search(default_text).group(0) != \
            receipt_block.search(vita_text).group(0):
        raise AssertionError("receipt block changed with guest image base")

    # The ordinary application root must not inherit the Game receipt marker.
    application = G._translate_function(
        pin, {"rva": 0x0048BC50}, None, {}, pin_img=pin
    )
    if application.get("vita_sim_cadence_receipt") is not False or \
            "ISAAC_VITA_SIM_CADENCE_RECEIPT" in str(application.get("text")):
        raise AssertionError("receipt escaped its exact Game::Update owner")

    # This is the exact supplemental callback that exposed the previous
    # result-schema omission.  Keep a direct translation check here so adding
    # the receipt field can never make a full Lua-enabled generation fail late.
    callback = G._translate_function(
        pin, {"rva": LUA_SUPPLEMENTAL_CALLBACK}, None, {}, pin_img=pin
    )
    callback_fn = {
        "rva": LUA_SUPPLEMENTAL_CALLBACK,
        "instruction_count": callback["insns"],
    }
    G.validate_lua_callback_translation(callback_fn, callback)
    if callback.get("vita_sim_cadence_receipt") is not False:
        raise AssertionError("Lua supplemental callback inherited cadence seam")

    for mutation in (
        0x002CDCF3,  # pinned entry prologue
        0x002CDD06,  # mov edi,ecx proves the incoming this pointer
        0x002CE598,  # original frame increment
        RETURNS[-1],
        MANAGER_CALL,
    ):
        expect_pin_failure(pin, switches, mutation)

    print(
        "Vita sim cadence codegen: PASS; exact Game::Update body/caller; "
        "one entry note; read-only Game*/frame sample; bases=2; "
        "mutations=5 closed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
