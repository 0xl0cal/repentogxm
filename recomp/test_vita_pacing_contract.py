#!/usr/bin/env python3
"""Freeze the two Repentance loops whose pacing depends on KAGE actual VSync."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"

# Exact byte windows from the frozen PE.  Together they pin both actual-VSync
# branches, the 1/60 target, the millisecond sleep conversion, and the final
# monotonic spin in each limiter.
WINDOWS = {
    (0x00476B6D, 0x00476B7A): "803d0d7bbc00000f850c010000",
    (0x00476BDA, 0x00476C10): (
        "f20f5c8580fafefff20f100d58a6b60033c9f20f5cc8"
        "f20f590d90aeb600f20f2cc1f7d085c00f4fc885c97409"
        "51ff153861a0006690"
    ),
    (0x00476C70, 0x00476C86):
        "f20f5c8580fafefff20f100d58a6b600660f2fc8778a",
    (0x0048BE32, 0x0048BE3E): "381d0d7bbc000f8502ffffff",
    (0x0048BE3E, 0x0048BE6B): (
        "e8fd410e00f20f5c85c4fbfffff20f100d58a6b600"
        "f20f5cc8f20f590d90aeb600f20f2cc1f7d08985d4fbffff"
    ),
    (0x0048BEA3, 0x0048BEB1): "ffb5d4fbffffff153861a000eb3f",
    (0x0048BEF0, 0x0048BF10): (
        "e84b410e00f20f5c85c4fbfffff20f100d58a6b600"
        "660f2fc877e5e930feffff"
    ),
}


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def sections(data: bytes) -> tuple[int, list[tuple[int, int, int, int]]]:
    if data[:2] != b"MZ":
        raise SystemExit("pacing PE lost its DOS signature")
    pe = u32(data, 0x3C)
    if data[pe : pe + 4] != b"PE\0\0":
        raise SystemExit("pacing PE lost its NT signature")
    if u16(data, pe + 4) != 0x014C or u16(data, pe + 24) != 0x010B:
        raise SystemExit("pacing PE is not the frozen i386/PE32 image")
    count = u16(data, pe + 6)
    optional_size = u16(data, pe + 20)
    optional = pe + 24
    if u32(data, optional + 28) != 0x00400000:
        raise SystemExit("pacing PE image base drifted")
    header_size = u32(data, optional + 60)
    table = optional + optional_size
    result = []
    for index in range(count):
        entry = table + index * 40
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", data, entry + 8
        )
        result.append((virtual_address, virtual_size, raw_offset, raw_size))
    return header_size, result


def rva_to_raw(
    rva: int, header_size: int, table: list[tuple[int, int, int, int]]
) -> int:
    if rva < header_size:
        return rva
    for virtual_address, virtual_size, raw_offset, raw_size in table:
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            return raw_offset + rva - virtual_address
    raise SystemExit(f"pacing RVA 0x{rva:08x} is outside the frozen PE")


def relative_target(data: bytes, raw: int, next_rva: int) -> int:
    return next_rva + struct.unpack_from("<i", data, raw)[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=Path)
    args = parser.parse_args()

    data = args.pe.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != PE_SIZE or digest != PE_SHA256:
        raise SystemExit(
            f"pacing PE identity drifted: size={len(data)} sha256={digest}"
        )
    header_size, table = sections(data)

    for (start, end), expected_hex in WINDOWS.items():
        raw = rva_to_raw(start, header_size, table)
        actual = data[raw : raw + end - start]
        expected = bytes.fromhex(expected_hex)
        if actual != expected:
            raise SystemExit(
                f"pacing window 0x{start:08x}..0x{end:08x} drifted: "
                f"{actual.hex()}"
            )

    frontend = rva_to_raw(0x00476B6D, header_size, table)
    gameplay = rva_to_raw(0x0048BE32, header_size, table)
    if u32(data, frontend + 2) != 0x00BC7B0D:
        raise SystemExit("frontend VSync branch no longer reads 0x00bc7b0d")
    if relative_target(data, frontend + 9, 0x00476B7A) != 0x00476C86:
        raise SystemExit("frontend VSync branch no longer skips its limiter")
    if u32(data, gameplay + 2) != 0x00BC7B0D:
        raise SystemExit("gameplay VSync branch no longer reads 0x00bc7b0d")
    if relative_target(data, gameplay + 8, 0x0048BE3E) != 0x0048BD40:
        raise SystemExit("gameplay VSync branch no longer loops past its limiter")

    frame_raw = rva_to_raw(0x0076A658, header_size, table)
    scale_raw = rva_to_raw(0x0076AE90, header_size, table)
    if data[frame_raw : frame_raw + 8] != bytes.fromhex("111111111111913f"):
        raise SystemExit("frozen 1/60 pacing constant drifted")
    if data[scale_raw : scale_raw + 8] != bytes.fromhex("0000000000408fc0"):
        raise SystemExit("frozen -1000 sleep conversion constant drifted")

    print(
        "Frozen Vita pacing contract: PASS "
        f"(size={len(data)} sha256={digest}; frontend+gameplay limiters exact)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
