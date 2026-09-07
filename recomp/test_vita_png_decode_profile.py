#!/usr/bin/env python3
"""Frozen-PE owner/seam oracle for the aggregate Vita PNG profiler."""

from __future__ import annotations

import argparse
import gc
import hashlib
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace

import pefile


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import gen_all as G  # noqa: E402
from gpr_locals import legacy_text  # noqa: E402
from image import DEFAULT_BASE, Image  # noqa: E402


VITA_BASE = 0x98000000


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def switch_info() -> dict[int, dict[str, object]]:
    targets = G.VITA_PNG_DECODE_PROFILE_FILTER_TARGETS
    site = G.VITA_PNG_DECODE_PROFILE_TABLE_SITE_RVA
    return {
        0x005B1500: {
            "entries": G.VITA_PNG_DECODE_PROFILE_READ_TARGETS,
            "edges": {
                G.VITA_PNG_DECODE_PROFILE_READ_TABLE_SITE_RVA:
                    G.VITA_PNG_DECODE_PROFILE_READ_TARGETS
            },
            "tables": {
                G.VITA_PNG_DECODE_PROFILE_READ_TABLE_SITE_RVA:
                    G.VITA_PNG_DECODE_PROFILE_READ_TARGETS
            },
        },
        G.VITA_PNG_DECODE_PROFILE_ROW_RVA: {
            "entries": tuple(sorted(targets)),
            "edges": {site: targets},
            "tables": {site: targets},
        },
        0x005D6D80: {
            "entries": tuple(sorted(
                G.VITA_PNG_INFLATE_PROFILE_CODES_TARGETS)),
            "edges": {
                G.VITA_PNG_INFLATE_PROFILE_CODES_TABLE_SITE_RVA:
                    G.VITA_PNG_INFLATE_PROFILE_CODES_TARGETS
            },
            "tables": {
                G.VITA_PNG_INFLATE_PROFILE_CODES_TABLE_SITE_RVA:
                    G.VITA_PNG_INFLATE_PROFILE_CODES_TARGETS
            },
        },
    }


def verify_raw_partition(pe_path: Path) -> None:
    probe = Image(str(pe_path), DEFAULT_BASE)
    raw = Image(str(pe_path), probe.orig_base)
    body = raw.code_at(
        G.VITA_PNG_DECODE_PROFILE_ROW_RVA,
        G.VITA_PNG_DECODE_PROFILE_TABLE_RVA -
        G.VITA_PNG_DECODE_PROFILE_ROW_RVA,
    )
    table = raw.code_at(
        G.VITA_PNG_DECODE_PROFILE_TABLE_RVA,
        len(G.VITA_PNG_DECODE_PROFILE_TABLE_RAW),
    )
    padding = raw.code_at(
        G.VITA_PNG_DECODE_PROFILE_PADDING_RVA,
        len(G.VITA_PNG_DECODE_PROFILE_PADDING),
    )
    extent = raw.code_at(
        G.VITA_PNG_DECODE_PROFILE_ROW_RVA,
        G.VITA_PNG_DECODE_PROFILE_ROW_EXTENT_END_RVA -
        G.VITA_PNG_DECODE_PROFILE_ROW_RVA,
    )
    assert len(body) == 916
    assert hashlib.sha256(body).hexdigest() == \
        G.VITA_PNG_DECODE_PROFILE_OWNERS[
            G.VITA_PNG_DECODE_PROFILE_ROW_RVA]["body_sha256"]
    assert table == G.VITA_PNG_DECODE_PROFILE_TABLE_RAW
    assert hashlib.sha256(table).hexdigest() == \
        G.VITA_PNG_DECODE_PROFILE_TABLE_SHA256
    assert padding == b"\xcc" * 8 == G.VITA_PNG_DECODE_PROFILE_PADDING
    assert len(extent) == 944
    assert hashlib.sha256(extent).hexdigest() == \
        G.VITA_PNG_DECODE_PROFILE_ROW_EXTENT_SHA256
    assert extent == body + table + padding
    read_table = raw.code_at(
        G.VITA_PNG_DECODE_PROFILE_READ_TABLE_RVA,
        len(G.VITA_PNG_DECODE_PROFILE_READ_TABLE_RAW),
    )
    assert read_table == G.VITA_PNG_DECODE_PROFILE_READ_TABLE_RAW
    assert hashlib.sha256(read_table).hexdigest() == \
        G.VITA_PNG_DECODE_PROFILE_READ_TABLE_SHA256
    codes_table = raw.code_at(
        G.VITA_PNG_INFLATE_PROFILE_CODES_TABLE_RVA,
        len(G.VITA_PNG_INFLATE_PROFILE_CODES_TABLE_RAW),
    )
    assert codes_table == G.VITA_PNG_INFLATE_PROFILE_CODES_TABLE_RAW
    assert hashlib.sha256(codes_table).hexdigest() == \
        G.VITA_PNG_INFLATE_PROFILE_CODES_TABLE_SHA256
    assert G._vita_png_decode_profile_rel32_xrefs(
        raw, G.VITA_PNG_INFLATE_PROFILE_INFLATE_RVA
    ) == G.VITA_PNG_INFLATE_PROFILE_INFLATE_XREFS
    assert G._vita_png_decode_profile_rel32_xrefs(
        raw, G.VITA_PNG_INFLATE_PROFILE_FAST_RVA
    ) == G.VITA_PNG_INFLATE_PROFILE_FAST_XREFS


def verify_generator(pe_path: Path) -> None:
    old_exe = G.EXE
    G.EXE = str(pe_path)
    try:
        capture_limit = (
            0xFFFFFFFF -
            G.VITA_PNG_INFLATE_PROFILE_STREAM_PROVEN_LAST_OFFSET
        )
        assert not G.vita_png_inflate_capture_range_nonwrapping(0)
        assert G.vita_png_inflate_capture_range_nonwrapping(1)
        assert G.vita_png_inflate_capture_range_nonwrapping(capture_limit)
        assert not G.vita_png_inflate_capture_range_nonwrapping(
            capture_limit + 1
        )
        assert not G.vita_png_inflate_capture_range_nonwrapping(0xFFFFFFFF)
        assert not G.vita_png_inflate_capture_range_nonwrapping(True)
        pin = Image(str(pe_path), DEFAULT_BASE)
        switches = switch_info()
        for base in (DEFAULT_BASE, VITA_BASE):
            image = Image(str(pe_path), base)
            for root, spec in sorted(G.VITA_PNG_DECODE_PROFILE_OWNERS.items()):
                result = G._translate_function(
                    image, {"rva": root}, None, switches, pin_img=pin,
                )
                assert result["stub"] is None
                assert result["vita_png_decode_profile"] == spec["kind"]
                source = legacy_text(result["text"])
                if "site" not in spec:
                    assert "kage_vita_png_profile_" not in source
                    if spec["kind"] == "row_target":
                        hook = "isaac_vita_png_unfilter_guest_try(c)"
                        assert source.count(hook) == 1
                        assert source.count(
                            "defined(ISAAC_VITA_PNG_NATIVE_UNFILTER)"
                        ) == 1
                        assert source.count("!g_guest_coverage_cases") == 1
                        assert source.index(hook) < source.index(
                            "/* 005c6bf0  push ebp */"
                        )
                        covered = G._translate_function(
                            image, {"rva": root}, None, switches,
                            function_coverage_ids={root: 31337}, pin_img=pin,
                        )
                        covered_source = legacy_text(covered["text"])
                        coverage = "guest_coverage_function(31337U);"
                        assert covered_source.count(coverage) == 1
                        assert (covered_source.index(coverage) <
                                covered_source.index(hook) <
                                covered_source.index(
                                    "/* 005c6bf0  push ebp */"))
                    else:
                        assert "ISAAC_VITA_PNG_NATIVE_UNFILTER" not in source
                    continue
                original = "    " + spec["statement"]
                assert source.count(original) == 1
                begin = (
                    "kage_vita_png_profile_image_begin();"
                    if spec["kind"] == "image" else
                    "        kage_vita_png_profile_row_begin("
                )
                end = (
                    "kage_vita_png_profile_image_end();"
                    if spec["kind"] == "image" else
                    "kage_vita_png_profile_row_end();"
                )
                begin_at = source.index(begin)
                original_at = source.index(original, begin_at)
                end_at = source.index(end, original_at)
                assert begin_at < original_at < end_at
                assert source.count(begin) == 1
                assert source.count(end) == 1
                if spec["kind"] == "row":
                    assert "c->esp + 8U" in source
                    assert "c->edx + 4U" in source
                    assert "c->edx + 0xbU" in source
                    assert "sceKernelGetProcessTimeWide" not in source
                    assert "isaac_vita_log" not in source
            for root, spec in sorted(
                    G.VITA_PNG_INFLATE_PROFILE_OWNERS.items()):
                result = G._translate_function(
                    image, {"rva": root}, None, switches, pin_img=pin,
                )
                assert result["stub"] is None
                assert result["vita_png_inflate_profile"] == spec["kind"]
                source = legacy_text(result["text"])
                original = "    " + spec["statement"]
                assert source.count(original) == 1
                if spec["kind"] == "fast":
                    begin = "kage_vita_png_profile_fast_begin();"
                    end = "kage_vita_png_profile_fast_end();"
                    assert "_guest_vita_png_zstream" not in source
                else:
                    begin = "        kage_vita_png_profile_inflate_begin("
                    end = "        kage_vita_png_profile_inflate_end("
                    assert source.count("_guest_vita_png_zstream = c->ecx") == 1
                    assert source.count(
                        "_guest_vita_png_zstream + 0x8U") == 2
                    assert source.count(
                        "_guest_vita_png_zstream + 0x14U") == 2
                    assert source.count("if (_guest_vita_png_zcapture)") == 2
                    assert source.count(
                        "_guest_vita_png_zstream <= UINT32_MAX - 0x3fU"
                    ) == 1
                    assert f"            {spec['class']}U, " in source
                begin_at = source.index(begin)
                original_at = source.index(original, begin_at)
                end_at = source.index(end, original_at)
                assert begin_at < original_at < end_at
                assert source.count(begin) == 1
                assert source.count(end) == 1
                assert "sceKernelGetProcessTimeWide" not in source
                assert "isaac_vita_log" not in source
    finally:
        G.EXE = old_exe


def verify_hostile_inputs(pe_path: Path) -> None:
    mutations = (
        0x005A0C79,                         # outer CALL
        G.VITA_PNG_DECODE_PROFILE_ROW_WINDOW_RVA,
        G.VITA_PNG_DECODE_PROFILE_ROW_RVA,
        G.VITA_PNG_DECODE_PROFILE_TABLE_RVA,
        G.VITA_PNG_DECODE_PROFILE_PADDING_RVA,
        0x005B1742,                         # row inflate CALL
        0x005C60DF,                         # ancillary ECX/CALL window
        0x005C71C4,                         # finish ECX/CALL window
        0x005D6DF6,                         # complete fast-call ABI window
        G.VITA_PNG_INFLATE_PROFILE_CODES_TABLE_RVA,
    )
    pe = pefile.PE(str(pe_path), fast_load=True)
    offsets = {rva: pe.get_offset_from_rva(rva) for rva in mutations}
    pe.close()
    original = pe_path.read_bytes()
    pin = Image(str(pe_path), DEFAULT_BASE)
    spec = G.VITA_PNG_DECODE_PROFILE_OWNERS[0x005A0C50]
    translated = G._translate_function(
        pin, {"rva": 0x005A0C50}, None, switch_info(), pin_img=pin,
    )
    assert translated["stub"] is None and spec["kind"] == "image"
    with tempfile.TemporaryDirectory(prefix="isaac-png-pin-") as value:
        for rva in mutations:
            payload = bytearray(original)
            offset = offsets[rva]
            payload[offset] ^= 1
            mutated_path = Path(value) / f"mutated-{rva:08x}.exe"
            mutated_path.write_bytes(payload)
            mutated = Image(str(mutated_path), DEFAULT_BASE)
            try:
                # Reuse the original semantic decode deliberately.  The
                # independent physical-PE pin must still reject each region.
                G.vita_png_decode_profile_for_body(
                    mutated, 0x005A0C50, {}, (0x005A0C50,), [], (), {},
                )
            except RuntimeError as exc:
                assert "PNG decode" in str(exc)
            else:
                raise AssertionError(f"mutated PE passed at RVA {rva:08x}")
            finally:
                del mutated
                gc.collect()

    # Membership failures must be loud before any renderer can emit a partial
    # profile.  These probes use the real frozen PE but deliberately hostile
    # reached-site sets.
    try:
        G.vita_png_inflate_profile_for_body(
            pin, 0x005A0C50, {},
            (0x005A0C50, 0x005B1742), [], (), {},
        )
    except RuntimeError as exc:
        assert "unexpected PNG inflate profile membership" in str(exc)
    else:
        raise AssertionError("foreign inflate seam membership passed")
    try:
        G.vita_png_inflate_profile_for_body(
            pin, 0x005B1500, {}, (0x005B1500,), [], (), {},
        )
    except RuntimeError as exc:
        assert "partial PNG inflate profile membership" in str(exc)
    else:
        raise AssertionError("missing inflate seam membership passed")

    # Re-run two complete translated owners and perturb only the semantic
    # inputs handed to the validator.  This reaches the independent emission,
    # ABI, and jump-table guards beyond the whole-file identity gate.
    switches = switch_info()

    def components(root: int):
        info = switches.get(root, {})
        _public, name = G.function_symbols(root)
        translated = G.B.translate(
            pin, root, name, None,
            info.get("entries", ()), info.get("edges", {}),
        )
        assert not translated[4]
        return translated

    _em, insns, order, body, _unsup, _indirect, _members = components(
        0x005B1500
    )
    hostile_body = list(body)
    for index, (instruction, statements) in enumerate(hostile_body):
        if instruction.address == 0x005B1742:
            hostile_body[index] = (instruction, ("sub_005c38a0(c);",))
            break
    else:
        raise AssertionError("row inflate statement absent from host oracle")
    try:
        G.vita_png_inflate_profile_for_body(
            pin, 0x005B1500, insns, order, hostile_body,
            switches[0x005B1500]["entries"],
            switches[0x005B1500]["edges"],
        )
    except RuntimeError as exc:
        assert "owner/CALL/ABI identity changed" in str(exc)
    else:
        raise AssertionError("hostile inflate emission passed")

    hostile_insns = dict(insns)
    original_abi = hostile_insns[0x005B1740]
    hostile_insns[0x005B1740] = SimpleNamespace(
        mnemonic="mov", op_str="ecx, edi", bytes=original_abi.bytes,
    )
    try:
        G.vita_png_inflate_profile_for_body(
            pin, 0x005B1500, hostile_insns, order, body,
            switches[0x005B1500]["entries"],
            switches[0x005B1500]["edges"],
        )
    except RuntimeError as exc:
        assert "owner/CALL/ABI identity changed" in str(exc)
    else:
        raise AssertionError("hostile inflate ECX ABI passed")

    _em, insns, order, body, _unsup, _indirect, _members = components(
        0x005D6D80
    )
    try:
        G.vita_png_inflate_profile_for_body(
            pin, 0x005D6D80, insns, order, body, (),
            switches[0x005D6D80]["edges"],
        )
    except RuntimeError as exc:
        assert "table/CFG proof changed" in str(exc)
    else:
        raise AssertionError("missing inflate_codes table entries passed")


def verify_build_switches() -> None:
    cmake = (HERE / "vita/CMakeLists.txt").read_text(encoding="utf-8")
    build = (HERE.parent / "tools/build_vita.py").read_text(encoding="utf-8")
    raw_gate = (HERE / "vita/vita_raw_allocator_gate.py").read_text(
        encoding="utf-8"
    )
    name = "ISAAC_VITA_PNG_DECODE_PROFILE"
    assert f"option({name}" in cmake
    option_tail = cmake.split(f"option({name}", 1)[1].split(")", 1)[0]
    assert option_tail.rstrip().endswith("OFF")
    assert "kage_vita_png_decode_profile.c" in cmake
    profile_header = (HERE / "runtime/kage_vita_png_decode_profile.h").read_text(
        encoding="utf-8"
    )
    assert "observed upper bounds" in profile_header
    assert "One whole image per 32-image block is selected" in profile_header
    assert "may be reported as pure PNG, DEFLATE, or unfilter CPU" in \
        profile_header
    assert "inflate includes inflate_fast" in profile_header
    assert "never subtracted" in profile_header
    assert (
        'ISAAC_VITA_PNG_DECODE_PROFILE_BUILD_ID=\\"'
        '${ISAAC_VITA_GUEST_LINK_ID}\\"'
    ) in cmake
    # Count the profiler's owner list, not independent native PNG/inflate
    # options, comments and linker wrappers elsewhere in the same project.
    profile_start = cmake.index(
        "  if(ISAAC_VITA_PNG_DECODE_PROFILE OR ISAAC_VITA_PNG_WINDOW_PROFILE)\n"
        "    # gen_all proves eight unique frozen owners"
    )
    profile_end = cmake.index(
        "  if(ISAAC_VITA_ANM2_MISSING_LAYER_GUARD)", profile_start
    )
    profile_scope = cmake[profile_start:profile_end]
    for root in (
            "sub_005a0c50", "sub_005a0cd0", "sub_005b1500",
            "sub_005c5fb0", "sub_005c6fa0", "sub_005d6d80"):
        assert profile_scope.count(root) == 1, root
        if root == "sub_005b1500":
            # ISAAC_VITA_NATIVE_PNG wraps the same png_read_row seam at link
            # time (GNU ld --wrap); that one line is the only other mention.
            wrap = "-Wl,--wrap=sub_005b1500"
            assert cmake.count(wrap) == 1
            native = cmake.split("if(ISAAC_VITA_NATIVE_PNG)", 1)[1]
            assert wrap in native.split("else()", 1)[0]
    assert f'"{name}": png_decode_profile' in build
    assert "--png-decode-profile" in build
    assert "kage_vita_png_decode_profile.c" in raw_gate


def run(pe_path: Path) -> None:
    pe_path = pe_path.resolve()
    assert pe_path.stat().st_size == G.VITA_PNG_DECODE_PROFILE_PE_SIZE
    assert sha256(pe_path) == G.VITA_PNG_DECODE_PROFILE_PE_SHA256
    verify_raw_partition(pe_path)
    verify_generator(pe_path)
    verify_hostile_inputs(pe_path)
    verify_build_switches()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", type=Path, required=True)
    args = parser.parse_args()
    run(args.pe)
    print("Vita PNG decode profile frozen owner/seam oracle: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
