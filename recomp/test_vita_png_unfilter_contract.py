#!/usr/bin/env python3
"""Pin the proposed native PNG unfilter seam to the frozen PE/codegen."""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
from pathlib import Path

import capstone
import pefile

PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
IMAGE_BASE = 0x00400000

# Keep executable bytes, data table, and alignment padding distinct.  In
# particular, never describe the 944-byte root-to-root span as the function
# body: its final 28 bytes are a jump table followed by 0xcc padding.
FILTER_CODE = (0x005C6BF0, 0x005C6F84)
FILTER_CODE_SHA256 = "09b4b16da837b7e8e6f0add69704517d3f96990b642bc0d9e4c60d4df2bc1746"
FILTER_TABLE = (0x005C6F84, 0x005C6F98)
FILTER_TABLE_SHA256 = "073997e6a32211be2bf5a9e53a24c264c38ff33e73a0c22c6759a4ca5a37f7db"
FILTER_PADDING = (0x005C6F98, 0x005C6FA0)
FILTER_PADDING_BYTES = b"\xcc" * 8
FILTER_TARGETS = (
    0x005C6CB4,  # None: shared return
    0x005C6C0E,  # Sub
    0x005C6CBB,  # Up
    0x005C6D55,  # Average
    0x005C6ECF,  # Paeth
)

ROW_INFO_SETUP = (0x005B1788, 0x005B17FC)
ROW_INFO_SETUP_SHA256 = (
    "377f2cfd510d555fc9485f581df8da9cee81cdec9dfe1cd088a1fb07a8f69cd8"
)
ARGUMENT_SETUP = (0x005B17E5, 0x005B17FC)
ARGUMENT_SETUP_BYTES = bytes.fromhex(
    "0fb60150"  # filter = row_buf[0]; push filter
    "8b86e00000004050"  # push png_ptr->prev_row + 1
    "8d4101"  # row = row_buf + 1
    "8bce50"  # ECX = png_ptr; push row
    "e8f4530100"  # call RVA 0x005c6bf0
)
CALL_INSTRUCTION_RVA = 0x005B17F7
CALL_CONTINUATION = (0x005B17FC, 0x005B1818)
CALL_CONTINUATION_SHA256 = (
    "d091b33e1ab8daa79e993d60abc75aec2e684084419da057d626f82194038b41"
)
IMAGE_PNG_CALL = (0x005A1398, 0x005A13AD)
IMAGE_PNG_CALL_BYTES = bytes.fromhex(
    "8b16518bcbe85e010100"  # EDX=row_info, ECX=png_ptr, call sub_005b1500
    "83c4048d760483ef0175eb"  # caller cleanup and next-row loop
)

GENERATED_FUNCTIONS = {
    "sub_005b1500": (
        32483,
        "a406a399ca7b0e25e90546bcfcdb195e832ae333d95405c01b8112880877e430",
    ),
    "sub_005c6bf0": (
        37319,
        "5f4dc21d1d22f3db5bb1bb8ccad43c57fb4425e58ddc804351257f384662067d",
    ),
}

# The aggregate decoder profiler intentionally wraps the one proven call from
# sub_005b1500 to the unfilter helper.  Authenticate the complete generated
# window -- including its exact placement, arguments, guard, and paired end
# hook -- before restoring the uninstrumented form for the frozen semantic
# size/hash contract below.  This is deliberately a literal replacement, not
# a regex which could hide an unrelated generator edit.
ROW_PROFILE_BASELINE_WINDOW = b"""    /* 005b17f7  call 0x5c6bf0 */
    gpush_generated(c, 0x5b17fcU); GUEST_STACK_CALLSITE_BARRIER(); sub_005c6bf0(c);
"""
ROW_PROFILE_AUTHENTICATED_WINDOW = b"""    /* 005b17f7  call 0x5c6bf0 */
#if defined(__vita__) && defined(ISAAC_VITA_PNG_DECODE_PROFILE)
    {
        uint32_t _guest_vita_png_filter = UINT32_MAX;
        extern void kage_vita_png_profile_row_begin(
            uint32_t, uint32_t, uint32_t);
        if (guest_stack_contains(c, (uint32_t)(c->esp + 8U), 4U))
            _guest_vita_png_filter = ld32((uint32_t)(c->esp + 8U));
        kage_vita_png_profile_row_begin(
            ld32((uint32_t)(c->edx + 4U)),
            (uint32_t)ld8((uint32_t)(c->edx + 0xbU)),
            _guest_vita_png_filter);
    }
#endif
    gpush_generated(c, 0x5b17fcU); GUEST_STACK_CALLSITE_BARRIER(); sub_005c6bf0(c);
#if defined(__vita__) && defined(ISAAC_VITA_PNG_DECODE_PROFILE)
    extern void kage_vita_png_profile_row_end(void);
    kage_vita_png_profile_row_end();
#endif
"""
ROW_PROFILE_MARKERS = (
    b"ISAAC_VITA_PNG_DECODE_PROFILE",
    b"_guest_vita_png_filter",
    b"kage_vita_png_profile_row_begin",
    b"kage_vita_png_profile_row_end",
)

# Entry-only default-off A/B seam in the frozen row-unfilter target.  The
# exact block is removed only for comparison with the pre-experiment generated
# semantic hash; any modified, duplicated, or relocated form must fail.
NATIVE_ENTRY_AUTHENTICATED_WINDOW = b"""#if defined(__vita__) && defined(ISAAC_VITA_PNG_NATIVE_UNFILTER)
    extern int isaac_vita_png_unfilter_guest_try(CPU *__restrict);
    if (!g_guest_coverage_cases &&
            isaac_vita_png_unfilter_guest_try(c))
        return;
#endif
"""
NATIVE_ENTRY_MARKERS = (
    b"ISAAC_VITA_PNG_NATIVE_UNFILTER",
    b"isaac_vita_png_unfilter_guest_try",
)

# The combined whole-image cohort profiler also wraps the earlier
# png_read_row -> inflate call.  Keep this independent from the unfilter hook:
# both exact windows may coexist in sub_005b1500, but neither authorizes a
# partial, modified, duplicated, or relocated instrumentation block.
INFLATE_ROW_PROFILE_BASELINE_WINDOW = b"""    /* 005b1742  call 0x5c38a0 */
    gpush_generated(c, 0x5b1747U); GUEST_STACK_CALLSITE_BARRIER(); sub_005c38a0(c);
"""
INFLATE_ROW_PROFILE_AUTHENTICATED_WINDOW = b"""    /* 005b1742  call 0x5c38a0 */
#if defined(__vita__) && defined(ISAAC_VITA_PNG_DECODE_PROFILE)
    {
        uint32_t _guest_vita_png_zstream = c->ecx;
        uint32_t _guest_vita_png_zcapture =
            _guest_vita_png_zstream != 0U &&
            _guest_vita_png_zstream <= UINT32_MAX - 0x3fU;
        uint32_t _guest_vita_png_total_in = 0U;
        uint32_t _guest_vita_png_total_out = 0U;
        extern void kage_vita_png_profile_inflate_begin(
            uint32_t, uint32_t, uint32_t, uint32_t);
        if (_guest_vita_png_zcapture) {
            _guest_vita_png_total_in = ld32(
                _guest_vita_png_zstream + 0x8U);
            _guest_vita_png_total_out = ld32(
                _guest_vita_png_zstream + 0x14U);
        }
        kage_vita_png_profile_inflate_begin(
            0U, _guest_vita_png_zcapture,
            _guest_vita_png_total_in, _guest_vita_png_total_out);
#endif
    gpush_generated(c, 0x5b1747U); GUEST_STACK_CALLSITE_BARRIER(); sub_005c38a0(c);
#if defined(__vita__) && defined(ISAAC_VITA_PNG_DECODE_PROFILE)
        extern void kage_vita_png_profile_inflate_end(
            uint32_t, uint32_t, uint32_t);
        if (_guest_vita_png_zcapture) {
            _guest_vita_png_total_in = ld32(
                _guest_vita_png_zstream + 0x8U);
            _guest_vita_png_total_out = ld32(
                _guest_vita_png_zstream + 0x14U);
        }
        kage_vita_png_profile_inflate_end(
            _guest_vita_png_zcapture,
            _guest_vita_png_total_in, _guest_vita_png_total_out);
    }
#endif
"""
INFLATE_ROW_PROFILE_MARKERS = (
    b"_guest_vita_png_zstream",
    b"_guest_vita_png_zcapture",
    b"_guest_vita_png_total_in",
    b"_guest_vita_png_total_out",
    b"kage_vita_png_profile_inflate_begin",
    b"kage_vita_png_profile_inflate_end",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def decode(pe: pefile.PE, start: int, end: int) -> list[capstone.CsInsn]:
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    instructions = list(
        decoder.disasm(pe.get_data(start, end - start), IMAGE_BASE + start)
    )
    require(
        instructions
        and instructions[-1].address + instructions[-1].size == IMAGE_BASE + end,
        f"decode did not cover [0x{start:08x},0x{end:08x})",
    )
    return instructions


def instruction_map(pe: pefile.PE, start: int, end: int) -> dict[int, str]:
    return {
        instruction.address
        - IMAGE_BASE: f"{instruction.mnemonic} {instruction.op_str}".rstrip()
        for instruction in decode(pe, start, end)
    }


def expect_instructions(mapping: dict[int, str], expected: dict[int, str]) -> None:
    for rva, text in expected.items():
        actual = mapping.get(rva)
        require(
            actual == text,
            f"instruction drift at RVA 0x{rva:08x}: {actual!r} != {text!r}",
        )


def prove_pe(path: Path) -> None:
    payload = path.read_bytes()
    require(len(payload) == PE_SIZE, "frozen PE size changed")
    require(digest(payload) == PE_SHA256, "frozen PE SHA-256 changed")
    pe = pefile.PE(data=payload, fast_load=True)
    require(pe.OPTIONAL_HEADER.ImageBase == IMAGE_BASE, "PE ImageBase changed")

    for label, bounds, expected_hash in (
        ("filter executable code", FILTER_CODE, FILTER_CODE_SHA256),
        ("filter jump table", FILTER_TABLE, FILTER_TABLE_SHA256),
        ("row-info/call setup", ROW_INFO_SETUP, ROW_INFO_SETUP_SHA256),
        ("unfilter call continuation", CALL_CONTINUATION, CALL_CONTINUATION_SHA256),
    ):
        start, end = bounds
        actual = pe.get_data(start, end - start)
        require(digest(actual) == expected_hash, f"{label} bytes changed")
    require(
        pe.get_data(FILTER_PADDING[0], 8) == FILTER_PADDING_BYTES,
        "post-table 0xcc padding changed",
    )
    require(
        pe.get_data(ARGUMENT_SETUP[0], 23) == ARGUMENT_SETUP_BYTES,
        "unfilter argument setup/call bytes changed",
    )
    require(
        pe.get_data(IMAGE_PNG_CALL[0], 21) == IMAGE_PNG_CALL_BYTES,
        "ImagePng -> row pipeline call bytes changed",
    )

    table = pe.get_data(FILTER_TABLE[0], FILTER_TABLE[1] - FILTER_TABLE[0])
    absolute_targets = struct.unpack("<5I", table)
    require(
        tuple(value - IMAGE_BASE for value in absolute_targets) == FILTER_TARGETS,
        "filter 0..4 jump-table targets changed",
    )

    body = decode(pe, *FILTER_CODE)
    require(len(body) == 312, "filter region instruction census changed")
    require(
        body[-2].address - IMAGE_BASE == 0x005C6F7E
        and body[-2].mnemonic == "call"
        and body[-1].address - IMAGE_BASE == 0x005C6F83
        and body[-1].mnemonic == "nop",
        "311 semantic instructions plus terminal alignment nop changed",
    )
    expect_instructions(
        instruction_map(pe, *FILTER_CODE),
        {
            0x005C6BFE: "cmp edx, 4",
            0x005C6C01: "ja 0x9c6f79",
            0x005C6C07: "jmp dword ptr [edx*4 + 0x9c6f84]",
            0x005C6C0E: "movzx edi, byte ptr [eax + 0xb]",
            0x005C6C12: "mov ebx, dword ptr [eax + 4]",
            0x005C6CA6: "mov cl, byte ptr [esi + eax]",
            0x005C6CAC: "add byte ptr [eax - 1], cl",
            0x005C6CBB: "mov esi, dword ptr [eax + 4]",
            0x005C6D40: "mov al, byte ptr [edx + ecx]",
            0x005C6D46: "add byte ptr [ecx - 1], al",
            0x005C6D55: "movzx ecx, byte ptr [eax + 0xb]",
            0x005C6D59: "mov ebx, dword ptr [eax + 4]",
            0x005C6EBB: "add ecx, eax",
            0x005C6EBD: "shr ecx, 1",
            0x005C6EBF: "add byte ptr [edx + esi - 1], cl",
            0x005C6ECF: "movzx edx, byte ptr [eax + 0xb]",
            0x005C6ED3: "mov esi, dword ptr [eax + 4]",
            0x005C6F4F: "cmp ecx, edx",
            0x005C6F51: "jg 0x9c6f57",
            0x005C6F53: "cmp ecx, eax",
            0x005C6F55: "jle 0x9c6f60",
            0x005C6F5A: "cmp edx, eax",
            0x005C6F5C: "cmovle ebx, dword ptr [ebp - 0x10]",
            0x005C6F79: "mov edx, 0xb69c84",
            0x005C6F7E: "call 0x9c3420",
            0x005C6F83: "nop",
        },
    )

    caller_map = instruction_map(pe, *ROW_INFO_SETUP)
    expect_instructions(
        caller_map,
        {
            0x005B178F: "lea edx, [esi + 0xf8]",
            0x005B17CD: "mov dword ptr [esi + 0xf8], ecx",
            0x005B17DF: "mov dword ptr [esi + 0xfc], eax",
            0x005B17E5: "movzx eax, byte ptr [ecx]",
            0x005B17E8: "push eax",
            0x005B17F0: "push eax",
            0x005B17F1: "lea eax, [ecx + 1]",
            0x005B17F4: "mov ecx, esi",
            0x005B17F6: "push eax",
            CALL_INSTRUCTION_RVA: "call 0x9c6bf0",
        },
    )
    require(
        caller_map[0x005B178F] == "lea edx, [esi + 0xf8]"
        and not any(
            "edx" in text.split(",", 1)[0]
            for rva, text in caller_map.items()
            if 0x005B1795 <= rva < CALL_INSTRUCTION_RVA
        ),
        "row_info no longer reaches the private EDX ABI unchanged",
    )
    continuation_map = instruction_map(pe, *CALL_CONTINUATION)
    expect_instructions(
        continuation_map,
        {
            0x005B17FC: "mov eax, dword ptr [esi + 0xd0]",
            0x005B1802: "inc eax",
            0x005B1810: "call 0x9ec14c",
            0x005B1815: "add esp, 0x18",
        },
    )


def normalized_function(source: bytes, function_name: str) -> bytes:
    normalized = source.replace(b"\r\n", b"\n")
    marker = f"void {function_name}".encode("ascii")
    require(normalized.count(marker) == 1, f"{function_name} definition count changed")
    start = normalized.index(marker)
    next_function = re.search(rb"\n/\* sub_[0-9a-f]+  RVA ", normalized[start:])
    end = start + next_function.start() + 1 if next_function else len(normalized)
    return normalized[start:end]


def semantic_generated_function(function_name: str, function: bytes) -> bytes:
    """Remove only the exact, independently proven aggregate-profiler seams."""

    row_authenticated_count = function.count(ROW_PROFILE_AUTHENTICATED_WINDOW)
    inflate_authenticated_count = function.count(
        INFLATE_ROW_PROFILE_AUTHENTICATED_WINDOW
    )
    if function_name == "sub_005b1500":
        require(
            row_authenticated_count <= 1,
            "generated row function contains duplicate authenticated profile hooks",
        )
        require(
            inflate_authenticated_count <= 1,
            "generated row function contains duplicate authenticated inflate hooks",
        )
        if row_authenticated_count == 1:
            function = function.replace(
                ROW_PROFILE_AUTHENTICATED_WINDOW,
                ROW_PROFILE_BASELINE_WINDOW,
            )
        if inflate_authenticated_count == 1:
            function = function.replace(
                INFLATE_ROW_PROFILE_AUTHENTICATED_WINDOW,
                INFLATE_ROW_PROFILE_BASELINE_WINDOW,
            )
        require(
            function.count(ROW_PROFILE_BASELINE_WINDOW) == 1,
            "generated row function lost its exact unfilter call window",
        )
        require(
            function.count(INFLATE_ROW_PROFILE_BASELINE_WINDOW) == 1,
            "generated row function lost its exact inflate call window",
        )
    else:
        require(
            row_authenticated_count == 0,
            f"generated {function_name} unexpectedly contains the row profile hook",
        )
        require(
            inflate_authenticated_count == 0,
            f"generated {function_name} unexpectedly contains the inflate profile hook",
        )
    native_entry_count = function.count(NATIVE_ENTRY_AUTHENTICATED_WINDOW)
    if function_name == "sub_005c6bf0":
        require(
            native_entry_count == 1,
            "generated row target lost its exact native entry seam",
        )
        native_entry_offset = function.index(NATIVE_ENTRY_AUTHENTICATED_WINDOW)
        native_entry_prefix = function[:native_entry_offset]
        require(
            re.fullmatch(
                rb"void sub_005c6bf0\(CPU \*__restrict c\)\n\{\n"
                rb"(?:    guest_coverage_function\([0-9]+U\);\n)?",
                native_entry_prefix,
            ) is not None,
            "generated native seam moved away from covered function entry",
        )
        function = function.replace(NATIVE_ENTRY_AUTHENTICATED_WINDOW, b"")
    else:
        require(
            native_entry_count == 0,
            f"generated {function_name} unexpectedly contains native entry seam",
        )
    require(
        not any(
            marker in function
            for marker in (
                ROW_PROFILE_MARKERS + INFLATE_ROW_PROFILE_MARKERS +
                NATIVE_ENTRY_MARKERS
            )
        ),
        f"generated {function_name} contains an unauthenticated PNG seam edit",
    )
    return function


def prove_generated_function(
    function_name: str,
    function: bytes,
    expected_size: int,
    expected_hash: str,
) -> None:
    semantic = semantic_generated_function(function_name, function)
    require(
        len(semantic) == expected_size,
        f"generated {function_name} semantic size changed",
    )
    require(
        digest(semantic) == expected_hash,
        f"generated {function_name} semantic hash changed",
    )


def prove_profile_normalizer_hostiles() -> None:
    """Keep the permitted profiler seam narrower than the semantic contract."""

    prefix = b"void sub_005b1500(void)\n{\n"
    suffix = b"}\n"
    baseline = (
        prefix
        + INFLATE_ROW_PROFILE_BASELINE_WINDOW
        + ROW_PROFILE_BASELINE_WINDOW
        + suffix
    )
    row_profiled = (
        prefix
        + INFLATE_ROW_PROFILE_BASELINE_WINDOW
        + ROW_PROFILE_AUTHENTICATED_WINDOW
        + suffix
    )
    inflate_profiled = (
        prefix
        + INFLATE_ROW_PROFILE_AUTHENTICATED_WINDOW
        + ROW_PROFILE_BASELINE_WINDOW
        + suffix
    )
    combined_profiled = (
        prefix
        + INFLATE_ROW_PROFILE_AUTHENTICATED_WINDOW
        + ROW_PROFILE_AUTHENTICATED_WINDOW
        + suffix
    )
    expected_size = len(baseline)
    expected_hash = digest(baseline)
    for profiled in (baseline, row_profiled, inflate_profiled, combined_profiled):
        prove_generated_function(
            "sub_005b1500", profiled, expected_size, expected_hash
        )

    def before_function_end(payload: bytes, addition: bytes) -> bytes:
        require(payload.endswith(suffix), "hostile fixture lost function suffix")
        return payload[: -len(suffix)] + addition + suffix

    hostile_inside_row_hook = combined_profiled.replace(
        b"        uint32_t _guest_vita_png_filter = UINT32_MAX;\n",
        b"        uint32_t _guest_vita_png_filter = 0U;\n",
        1,
    )
    hostile_inside_inflate_hook = combined_profiled.replace(
        b"            0U, _guest_vita_png_zcapture,\n",
        b"            1U, _guest_vita_png_zcapture,\n",
        1,
    )
    hostile_duplicate_inflate_hook = before_function_end(
        combined_profiled, INFLATE_ROW_PROFILE_AUTHENTICATED_WINDOW
    )
    hostile_relocated_hooks = (
        prefix
        + ROW_PROFILE_AUTHENTICATED_WINDOW
        + INFLATE_ROW_PROFILE_AUTHENTICATED_WINDOW
        + suffix
    )
    hostile_after_hook = before_function_end(
        combined_profiled,
        b"    c->eax ^= 1U;  /* unexpected semantic edit */\n",
    )
    for label, hostile in (
        ("modified row profile hook", hostile_inside_row_hook),
        ("modified inflate profile hook", hostile_inside_inflate_hook),
        ("duplicate inflate profile hook", hostile_duplicate_inflate_hook),
        ("relocated profile hooks", hostile_relocated_hooks),
        ("extra generated edit", hostile_after_hook),
    ):
        try:
            prove_generated_function(
                "sub_005b1500", hostile, expected_size, expected_hash
            )
        except AssertionError:
            continue
        raise AssertionError(f"hostile self-test accepted {label}")

    row_prefix = b"void sub_005c6bf0(CPU *__restrict c)\n{\n"
    row_body = b"    c->eax = 1U;\n"
    row_end = b"}\n"
    native_row = (
        row_prefix + NATIVE_ENTRY_AUTHENTICATED_WINDOW + row_body + row_end
    )
    expected_row = row_prefix + row_body + row_end
    prove_generated_function(
        "sub_005c6bf0", native_row,
        len(expected_row), digest(expected_row),
    )
    for label, hostile in (
        (
            "modified native entry guard",
            native_row.replace(
                b"defined(ISAAC_VITA_PNG_NATIVE_UNFILTER)",
                b"ISAAC_VITA_PNG_NATIVE_UNFILTER",
                1,
            ),
        ),
        (
            "modified native handled branch",
            native_row.replace(b"        return;\n", b"        c->eax = 1U;\n", 1),
        ),
        (
            "removed native coverage fallback",
            native_row.replace(b"!g_guest_coverage_cases &&\n            ", b"", 1),
        ),
        (
            "duplicate native entry seam",
            row_prefix + NATIVE_ENTRY_AUTHENTICATED_WINDOW * 2 +
            row_body + row_end,
        ),
        (
            "relocated native entry seam",
            row_prefix + row_body + NATIVE_ENTRY_AUTHENTICATED_WINDOW + row_end,
        ),
    ):
        try:
            prove_generated_function(
                "sub_005c6bf0", hostile,
                len(expected_row), digest(expected_row),
            )
        except AssertionError:
            continue
        raise AssertionError(f"hostile self-test accepted {label}")


def prove_generated(generated_dir: Path) -> None:
    sources = sorted(generated_dir.glob("guest_[0-9][0-9][0-9][0-9].c"))
    require(sources, "generated guest corpus is empty")
    for function_name, (expected_size, expected_hash) in GENERATED_FUNCTIONS.items():
        marker = f"void {function_name}".encode("ascii")
        owners = [path for path in sources if marker in path.read_bytes()]
        require(
            len(owners) == 1, f"{function_name} maps to {len(owners)} generated units"
        )
        function = normalized_function(owners[0].read_bytes(), function_name)
        prove_generated_function(
            function_name,
            function,
            expected_size,
            expected_hash,
        )
    filter_source = normalized_function(
        next(
            path for path in sources if b"void sub_005c6bf0" in path.read_bytes()
        ).read_bytes(),
        "sub_005c6bf0",
    )
    for required in (
        NATIVE_ENTRY_AUTHENTICATED_WINDOW,
        b"/* 005c6c07  jmp dword ptr [edx*4 - 0x67a3907c] */",
        b"/* 005c6f7e  call 0x5c3420 */",
        b"sub_005c3420(c);",
    ):
        require(required in filter_source, f"generated filter source lost {required!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("pe", type=Path)
    parser.add_argument("generated_dir", type=Path)
    arguments = parser.parse_args()
    prove_profile_normalizer_hostiles()
    prove_pe(arguments.pe)
    prove_generated(arguments.generated_dir)
    print(
        "Vita PNG unfilter frozen contract: PASS; "
        "code=[005c6bf0,005c6f84)/916; table=[005c6f84,005c6f98)/20; "
        "call=005b17f7; filters=5; generated=2"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
