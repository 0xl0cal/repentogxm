#!/usr/bin/env python3
"""Freeze the two logging-only OpenAL A005 observation sites."""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from image import DEFAULT_BASE, Image  # noqa: E402


PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
GET_ERROR_IAT = bytes.fromhex("ff154c636030")
SITES = {
    0x005BE811: bytes.fromhex(
        "ff150c63603083c40cff154c63603085c074105068208e76306a02"
        "e80efbf9ff83c40c"
    ),
    0x005BE8FF: bytes.fromhex(
        "ff156063603083c40cff154c63603085c074105068588d76306a02"
        "e820faf9ff83c40c"
    ),
}
STARTS = {
    0x005BE811: 0x005BE802,
    0x005BE8FF: 0x005BE8F0,
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_header() -> None:
    text = (HERE / "runtime" / "host_vita_audio.h").read_text(
        encoding="utf-8"
    )
    expected = {
        "ISAAC_VITA_AUDIO_A005_IS_PLAYING_RETURN": 0x005BE811,
        "ISAAC_VITA_AUDIO_A005_SET_VOLUME_RETURN": 0x005BE8FF,
    }
    for name, value in expected.items():
        match = re.search(rf"^#define\s+{name}\s+(0x[0-9a-fA-F]+)U$", text,
                          re.MULTILINE)
        if not match or int(match.group(1), 16) != value:
            raise AssertionError(f"{name} no longer matches the frozen PE")


def verify_pe(path: Path) -> None:
    if sha256(path) != PE_SHA256:
        raise AssertionError("A005 site test received a different frozen PE")
    image = Image(str(path), DEFAULT_BASE)
    for return_address, expected in SITES.items():
        start = STARTS[return_address]
        actual = image.code_at(start, len(expected))
        if actual != expected:
            raise AssertionError(
                f"logging-only A005 window changed at {start:08x}"
            )
        if image.code_at(return_address - len(GET_ERROR_IAT),
                         len(GET_ERROR_IAT)) != GET_ERROR_IAT:
            raise AssertionError(
                f"alGetError no longer returns at {return_address:08x}"
            )
        # test eax,eax; jz +0x10 skips only the push/push/push/log call block.
        if image.code_at(return_address, 4) != bytes.fromhex("85c07410"):
            raise AssertionError(
                f"A005 observer gained a non-logging control effect at "
                f"{return_address:08x}"
            )
    for expected in SITES.values():
        if image.mem.count(expected) != 1:
            raise AssertionError("frozen A005 observation window is not unique")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=Path)
    args = parser.parse_args()
    verify_header()
    verify_pe(args.pe.resolve())
    print(
        "Vita OpenAL A005 frozen sites: PASS; "
        "IsPlaying=005be811; SetVolume=005be8ff; logging-only=2"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
