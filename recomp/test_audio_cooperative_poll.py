#!/usr/bin/env python3
"""Frozen-PE/codegen proof for the cooperative Vita audio safe point."""

from __future__ import annotations

import argparse
import copy
import hashlib
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from image import DEFAULT_BASE, Image  # noqa: E402
import gen_all as G  # noqa: E402
from gpr_locals import legacy_text  # noqa: E402
from test_ogg_queue_error_cleanup import sha256  # noqa: E402


PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
ROOT_RVA = 0x003ABE30
SITE_RVA = 0x003ABE7F
CALL_RVA = 0x003ABEA7
TARGET_RVA = 0x003AAC50
WINDOW_SIZE = 0x2D
WINDOW_SHA256 = "fb74113431728cc4ef66781b8cdc50d1f11ee42ae2d3c341c019d57e0781d759"
EXPECTED_BLOCK = """    /* Vita audio owner-thread safe point; original follows. */
    {
        extern void kage_pc_backend_audio_cooperative_poll(CPU *);
        kage_pc_backend_audio_cooperative_poll(c);
    }
    /* 003abe7f  push edi */"""


def verify_pe(path: Path) -> None:
    if sha256(path) != PE_SHA256:
        raise AssertionError("cooperative audio test received a different PE")
    image = Image(str(path), DEFAULT_BASE)
    window = image.code_at(SITE_RVA, WINDOW_SIZE)
    if hashlib.sha256(window).hexdigest() != WINDOW_SHA256:
        raise AssertionError("Room::FixSpawnEntry activation window changed")
    call = image.code_at(CALL_RVA, 5)
    target = CALL_RVA + 5 + int.from_bytes(call[1:], "little", signed=True)
    if call != bytes.fromhex("e8a4edffff") or target != TARGET_RVA:
        raise AssertionError("Room::FixSpawnEntry activation call changed")


def render(image: Image, pin: Image) -> str:
    result = G._translate_function(
        image, {"rva": ROOT_RVA}, None, {}, pin_img=pin
    )
    if result.get("stub") is not None:
        raise AssertionError("cooperative poll owner became a stub")
    if result.get("vita_audio_cooperative_poll") is not True:
        raise AssertionError("cooperative poll seam was not selected")
    text = legacy_text(result["text"])
    if text.count(EXPECTED_BLOCK) != 1:
        raise AssertionError("cooperative poll is absent, duplicated or moved")
    if text.count("kage_pc_backend_audio_cooperative_poll(c);") != 1:
        raise AssertionError("cooperative poll call census changed")
    return text


def exercise_generator(path: Path) -> None:
    G.EXE = str(path)
    pin = Image(str(path), DEFAULT_BASE)
    default_text = render(pin, pin)
    vita_text = render(Image(str(path), 0x98000000), pin)
    if EXPECTED_BLOCK not in default_text or EXPECTED_BLOCK not in vita_text:
        raise AssertionError("safe point differs by guest image base")

    mutated = copy.copy(pin)
    memory = bytearray(pin.mem)
    memory[SITE_RVA] ^= 1
    mutated.mem = bytes(memory)
    try:
        render(mutated, mutated)
    except RuntimeError as exc:
        if "cooperative audio poll activation identity changed" not in str(exc):
            raise
    else:
        raise AssertionError("mutated activation window was accepted")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    args = parser.parse_args()
    path = args.pe.resolve()
    verify_pe(path)
    exercise_generator(path)
    print("Vita audio cooperative generator pin: PASS; root=003abe30; site=003abe7f; bases=2; mutation=closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
