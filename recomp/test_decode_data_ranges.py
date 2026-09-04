#!/usr/bin/env python3
"""Fail-closed regression for recursive decode around proved embedded data."""

from __future__ import annotations

import trans as T


BASE = 0x1000


def context(raw: bytes) -> T.Ctx:
    ctx = T.Ctx("decode-data-range-fixture", 0, BASE, BASE + len(raw), raw)
    # Production uses image.Image, whose SizeOfImage-backed `size` bounds the
    # relocation-proved table ranges.  Keep the tiny fixture equally bounded.
    ctx.size = BASE + len(raw)
    return ctx


def expect(exception, fragment: str, call) -> None:
    try:
        call()
    except exception as exc:
        if fragment not in str(exc):
            raise AssertionError(
                "wrong failure for %r: %s" % (fragment, exc)) from exc
    else:
        raise AssertionError("missing failure containing %r" % fragment)


def main() -> int:
    # Ordinary fallthrough stops at the first byte of a proved table.  Bytes
    # after the table are deliberately not rediscovered without a real edge.
    sequential = context(bytes.fromhex("90 90 11 22 33 44 c3"))
    insns, order, indirect = T.decode(
        sequential, BASE, data_ranges=((BASE + 2, BASE + 6),))
    if tuple(order) != (BASE, BASE + 1) or set(insns) != set(order):
        raise AssertionError("fallthrough crossed proved data: %r" % order)
    if indirect:
        raise AssertionError("fixture invented an indirect edge")

    data_range = ((BASE + 4, BASE + 8),)
    direct = context(bytes.fromhex("eb 02 cc cc 11 22 33 44 c3"))
    expect(
        T.Unsupported,
        "control target enters proved data range",
        lambda: T.decode(direct, BASE, data_ranges=data_range),
    )
    conditional = context(bytes.fromhex("74 02 c3 cc 11 22 33 44 c3"))
    expect(
        T.Unsupported,
        "control target enters proved data range",
        lambda: T.decode(conditional, BASE, data_ranges=data_range),
    )

    seeded = context(bytes.fromhex("c3 cc cc cc 11 22 33 44 c3"))
    expect(
        T.Unsupported,
        "decode seed lies in proved data range",
        lambda: T.decode(
            seeded,
            BASE,
            extra_starts=(BASE + 5,),
            data_ranges=data_range,
        ),
    )

    # An instruction beginning before a table must not consume its bytes.
    overlap = context(bytes.fromhex("b8 01 00 00 00 c3"))
    expect(
        T.Unsupported,
        "instruction overlaps proved data range",
        lambda: T.decode(
            overlap, BASE, data_ranges=((BASE + 3, BASE + 5),)),
    )

    expect(
        ValueError,
        "decode data ranges overlap",
        lambda: T.decode(
            seeded,
            BASE,
            data_ranges=((BASE + 2, BASE + 6),
                         (BASE + 5, BASE + 8)),
        ),
    )
    expect(
        ValueError,
        "decode data range must be an integer pair",
        lambda: T.decode(seeded, BASE, data_ranges=((BASE + 2, "bad"),)),
    )
    expect(
        ValueError,
        "decode data range is outside the image",
        lambda: T.decode(
            seeded, BASE, data_ranges=((BASE + 2, seeded.size + 1),)),
    )

    print("proved data-range decode regression: PASS; cases=8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
