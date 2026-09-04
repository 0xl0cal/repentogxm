#!/usr/bin/env python3
"""Frozen-PE/codegen proof for the seven aggregate phase-profile seams."""

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


PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
VITA_IMAGE_BASE = 0x98000000
EXPECTED = (
    (0x0048BD40, "kage_pc_backend_note_loop_head"),
    (0x0048BDF0, "kage_pc_backend_note_service_entry"),
    (0x0048BDF5, "kage_pc_backend_note_update_entry"),
    (0x0048BDFA, "kage_pc_backend_note_render_entry"),
    (0x0048BDFF, "kage_pc_backend_note_render_return"),
    (0x0048BE3E, "kage_pc_backend_note_limiter_entry"),
    (0x0048BF0B, "kage_pc_backend_note_limiter_exit"),
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def render(image: Image, pin: Image) -> str:
    result = G._translate_function(
        image, {"rva": G.APPLICATION_MAIN_RVA}, None, {}, pin_img=pin
    )
    if result.get("stub") is not None:
        raise AssertionError("application main became a generated stub")
    expected_sites = tuple(site for site, unused_name in EXPECTED)
    if result.get("kage_pc_loop_stages") != expected_sites:
        raise AssertionError(
            "phase-profile seam census/order changed: "
            f"{result.get('kage_pc_loop_stages')!r} != {expected_sites!r}"
        )
    text = result["text"]
    prior = -1
    for site, function in EXPECTED:
        call = f"            {function}();"
        if text.count(call) != 1:
            raise AssertionError(
                f"profile hook {function} is absent or duplicated"
            )
        position = text.index(call)
        if position <= prior:
            raise AssertionError("profile hooks are not emitted in machine order")
        original = text.find(f"/* {site:08x}  ", position)
        if original < position:
            raise AssertionError(
                f"profile hook {function} no longer precedes its instruction"
            )
        prior = position
    return text


def expect_pin_failure(pin: Image, rva: int, needle: str) -> None:
    mutated = copy.copy(pin)
    memory = bytearray(pin.mem)
    memory[rva] ^= 1
    mutated.mem = bytes(memory)
    try:
        render(mutated, mutated)
    except RuntimeError as exc:
        if needle not in str(exc):
            raise
    else:
        raise AssertionError(
            f"mutated profile seam at 0x{rva:08x} was accepted"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    arguments = parser.parse_args()
    path = arguments.pe.resolve()
    if path.stat().st_size != PE_SIZE or sha256(path) != PE_SHA256:
        raise AssertionError("phase-profile codegen test received a different PE")

    G.EXE = str(path)
    pin = Image(str(path), DEFAULT_BASE)
    pc = render(pin, pin)
    vita = render(Image(str(path), VITA_IMAGE_BASE), pin)
    for unused_site, function in EXPECTED:
        if f"            {function}();" not in pc or \
                f"            {function}();" not in vita:
            raise AssertionError(f"profile hook {function} differs by image base")
    if G.PC_CLOCK_ROOT_RVA != G.KAGE_PC_LIMITER_ENTRY_TARGET_RVA:
        raise AssertionError("limiter profile seam and backend clock root diverged")

    expect_pin_failure(
        pin, G.KAGE_PC_SERVICE_ENTRY_RVA,
        "Update/Render/return machine window changed",
    )
    expect_pin_failure(
        pin, G.KAGE_PC_LIMITER_ENTRY_RVA,
        "software limiter entry machine window changed",
    )
    expect_pin_failure(
        pin, G.KAGE_PC_LIMITER_EXIT_RVA,
        "terminal backedge machine window changed",
    )
    print(
        "Vita phase-profile generator pin: PASS; "
        "seven seams; bases=2; service/limiter mutations=closed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
