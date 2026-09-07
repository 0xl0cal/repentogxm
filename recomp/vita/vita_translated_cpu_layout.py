#!/usr/bin/env python3
"""Emit the forced-include layout header for ISAAC_VITA_TRANSLATED_CPU_HOT_LAYOUT.

The header redeclares every hot translated function with
``__attribute__((hot, section(".text.sorted.NNNNNN")))`` in hot-set order.
GCC applies attributes from a prior declaration to the definition, so the
generated corpus is untouched; the default GNU ld script places
``*(SORT(.text.sorted.*))`` after ``.text.hot`` and before the ordinary
``.text.*`` input sections, sorted by name, which yields exactly the requested
order without a custom linker script.  ``--cold-rest`` additionally marks every
other function in guest_funcs.h ``__attribute__((cold))``: size-optimised and
grouped into ``.text.unlikely.*`` ahead of the hot region.
"""
from __future__ import annotations

import argparse
import re
import sys

DECL_RE = re.compile(r"^void (sub_[0-9a-f]{8})\(CPU \*__restrict c\);$")
NAME_RE = re.compile(r"^sub_[0-9a-f]{8}$")


def read_hot_set(path: str) -> list[str]:
    result: list[str] = []
    seen: set[str] = set()
    with open(path, encoding="utf-8") as handle:
        for number, raw in enumerate(handle, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if not NAME_RE.match(line):
                raise ValueError(
                    f"{path}:{number}: not a translated function name: {line!r}")
            if line in seen:
                raise ValueError(f"{path}:{number}: duplicate hot function {line}")
            seen.add(line)
            result.append(line)
    return result


def read_declared(path: str) -> list[str]:
    result: list[str] = []
    with open(path, encoding="utf-8") as handle:
        for raw in handle:
            match = DECL_RE.match(raw.rstrip("\n"))
            if match:
                result.append(match.group(1))
    if not result:
        raise ValueError(f"{path}: no translated function declarations found")
    return result


def render(hot: list[str], declared: list[str], cold_rest: bool) -> str:
    declared_set = set(declared)
    lines = [
        "/* GENERATED at configure time by vita_translated_cpu_layout.py; "
        "do not edit.",
        " * Attributes on these declarations bind to the generated definitions. */",
        "#ifndef ISAAC_VITA_TRANSLATED_CPU_LAYOUT_H",
        "#define ISAAC_VITA_TRANSLATED_CPU_LAYOUT_H",
        '#include "guest.h"',
    ]
    placed = 0
    for function in hot:
        if function not in declared_set:
            continue                     # stale entry: harmless, skip
        lines.append(
            f"void {function}(CPU *__restrict c) "
            f'__attribute__((hot, section(".text.sorted.{placed:06d}")));')
        placed += 1
    if cold_rest:
        hot_set = set(hot)
        for function in declared:
            if function not in hot_set:
                lines.append(
                    f"void {function}(CPU *__restrict c) __attribute__((cold));")
    lines.append(f"#define ISAAC_VITA_TRANSLATED_CPU_LAYOUT_HOT_COUNT {placed}U")
    lines.append("#endif")
    return "\n".join(lines) + "\n"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--hot-set", required=True)
    parser.add_argument("--funcs", required=True, help="generated guest_funcs.h")
    parser.add_argument("--output", required=True)
    parser.add_argument("--cold-rest", action="store_true")
    args = parser.parse_args(argv)
    try:
        hot = read_hot_set(args.hot_set)
        declared = read_declared(args.funcs)
    except (OSError, ValueError) as exc:
        print(f"vita_translated_cpu_layout: {exc}", file=sys.stderr)
        return 1
    if not hot:
        print("vita_translated_cpu_layout: hot set is empty", file=sys.stderr)
        return 1
    text = render(hot, declared, args.cold_rest)
    # Every generated unit force-includes this header.  Preserve its mtime
    # across identical configurations so Ninja keeps the existing objects.
    try:
        with open(args.output, "rb") as existing:
            unchanged = existing.read() == text.encode("utf-8")
    except FileNotFoundError:
        unchanged = False
    if not unchanged:
        with open(args.output, "w", encoding="utf-8", newline="\n") as out:
            out.write(text)
    known = sum(1 for f in hot if f in set(declared))
    print(f"hot={known}/{len(hot)} declared={len(declared)} "
          f"cold_rest={int(args.cold_rest)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
