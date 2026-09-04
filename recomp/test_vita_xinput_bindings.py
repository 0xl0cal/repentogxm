#!/usr/bin/env python3
"""Freeze the controller rows used by the Vita XInput policy."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

from test_vita_pacing_contract import rva_to_raw, sections


PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
TABLE_RVA = 0x007AAA20
ACTION_ROWS_OFFSET = 0x80
EXPECTED_ROWS = (
    (8, 8),    # bomb -> LB
    (9, 9),    # active item -> LT
    (10, 11),  # pill/card -> RB
    (11, 12),  # drop -> RT
    (12, 15),  # pause -> Start
    (13, 14),  # map -> Back
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=Path)
    args = parser.parse_args()

    data = args.pe.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != PE_SIZE or digest != PE_SHA256:
        raise SystemExit(
            f"XInput binding PE identity drifted: size={len(data)} "
            f"sha256={digest}"
        )
    header_size, table = sections(data)
    raw = rva_to_raw(TABLE_RVA + ACTION_ROWS_OFFSET, header_size, table)
    rows = tuple(
        struct.unpack_from("<II", data, raw + index * 8)
        for index in range(len(EXPECTED_ROWS))
    )
    if rows != EXPECTED_ROWS:
        raise SystemExit(
            "frozen XInput action/binding rows drifted: "
            + ", ".join(f"{action}->{binding}" for action, binding in rows)
        )

    print(
        "Frozen Vita XInput bindings: PASS "
        "(bomb=LB active=LT pill=RB drop=RT pause=Start map=Back)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
