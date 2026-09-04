#!/usr/bin/env python3
"""Static owner/CMake gate for the default-OFF ANM2 scratch seam."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


def function_body(text: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", text, re.DOTALL)
    if not match:
        raise AssertionError(f"function absent: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    raise AssertionError(f"unterminated function: {name}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--heap", type=Path, required=True)
    parser.add_argument("--cmake", type=Path, required=True)
    arguments = parser.parse_args()

    heap = arguments.heap.read_text(encoding="utf-8")
    cmake = arguments.cmake.read_text(encoding="utf-8")

    conditional_include = (
        "#ifdef ISAAC_VITA_ANM2_SCRATCH\n"
        "#include \"host_vita_anm2_scratch.h\"\n"
        "#endif"
    )
    if conditional_include not in heap:
        raise AssertionError("scratch header is not default-OFF guarded")

    malloc = function_body(heap, "vita_heap_malloc")
    free = function_body(heap, "vita_heap_free")
    realloc = function_body(heap, "vita_heap_realloc")
    if "#ifdef ISAAC_VITA_ANM2_SCRATCH" not in malloc:
        raise AssertionError("malloc seam is not compile-time guarded")
    required_malloc = (
        "vita_heap_arg(c, 0U)",
        "vita_heap_arg(c, 2U)",
        "c->stack_floor",
        "c->stack_ceiling",
        "isaac_vita_anm2_scratch_malloc",
        "ISAAC_VITA_ANM2_SCRATCH_REJECTED",
        "ISAAC_VITA_ANM2_SCRATCH_HANDLED",
    )
    for marker in required_malloc:
        if marker not in malloc:
            raise AssertionError(f"malloc seam omits {marker}")
    if "isaac_vita_anm2_scratch_free" not in free or \
       "#ifdef ISAAC_VITA_ANM2_SCRATCH" not in free:
        raise AssertionError("free exact-base seam is absent or unguarded")
    if "isaac_vita_anm2_scratch" in realloc:
        raise AssertionError("realloc semantics were changed")

    option = re.search(
        r"option\(ISAAC_VITA_ANM2_SCRATCH\s+\n?"
        r"\s*\"[^\"]+\"\s+(ON|OFF)\)",
        cmake,
    )
    if not option or option.group(1) != "OFF":
        raise AssertionError("ISAAC_VITA_ANM2_SCRATCH is not default OFF")
    block = re.search(
        r"if\(ISAAC_VITA_ANM2_SCRATCH\)\n(.*?)\n\s*endif\(\)",
        cmake,
        re.DOTALL,
    )
    if not block:
        raise AssertionError("ANM2 CMake selection block is absent")
    selected = block.group(1)
    for marker in (
        "host_vita_anm2_scratch.c",
        "host_vita_heap.c",
        "APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_ANM2_SCRATCH=1",
    ):
        if marker not in selected:
            raise AssertionError(f"ANM2 CMake block omits {marker}")
    if "target_compile_definitions" in selected:
        raise AssertionError("ANM2 macro became target-wide")
    if cmake.count('"${ISAAC_RUNTIME}/host_vita_anm2_scratch.c"') != 1:
        raise AssertionError("ANM2 source selection is not singular")

    print(
        "ANM2 scratch integration gate: PASS; default=OFF; "
        "owner-stack=ESP+12; macro=host_vita_heap.c-only; realloc=unchanged"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
