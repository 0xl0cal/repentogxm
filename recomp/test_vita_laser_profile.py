#!/usr/bin/env python3
"""Frozen-PE codegen oracle for the read-only Vita laser census."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import sys


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import build_one as B  # noqa: E402
import gen_all as G  # noqa: E402
from gpr_locals import legacy_text  # noqa: E402
from image import DEFAULT_BASE, Image  # noqa: E402


VITA_BASE = 0x98000000
FENCE = "#if defined(__vita__) && defined(ISAAC_VITA_LASER_PROFILE)"


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _switch_info(img: Image, root: int) -> dict:
    spec = G.VITA_LASER_PROFILE_OWNERS[root]
    entries, edges, rejected, tables = G.discover_jump_tables(
        img, root, owner_end=spec["end"]
    )
    assert entries == spec["switch_entries"]
    assert edges == spec["switch_edges"]
    assert tables == spec["switch_edges"]
    assert rejected == {}
    return {
        root: {
            "entries": entries,
            "edges": edges,
            "rejected": rejected,
            "tables": tables,
        }
    }


def _raw_translation(img: Image, root: int, switch_info: dict):
    info = switch_info[root]
    result = B.translate(
        img, root, "vita_laser_profile_probe", None,
        info["entries"], info["edges"],
    )
    em, insns, order, body, unsupported, indirect, _members = result
    assert unsupported == []
    assert tuple(indirect) == tuple(info["edges"])
    return em, insns, order, body


def _verify_generator(pe_path: Path) -> None:
    pin = Image(str(pe_path), DEFAULT_BASE)
    switch_by_root = {
        root: _switch_info(pin, root) for root in G.VITA_LASER_PROFILE_ROOTS
    }
    raw_by_root = {}
    rendered_by_base = {}
    for root in sorted(G.VITA_LASER_PROFILE_ROOTS):
        info = switch_by_root[root]
        em, insns, order, body = _raw_translation(pin, root, info)
        spec = G.vita_laser_profile_for_body(
            pin, root, em, insns, order, body,
            info[root]["entries"], info[root]["edges"],
        )
        assert spec is G.VITA_LASER_PROFILE_OWNERS[root]
        raw_by_root[root] = (em, insns, order, body)

        begin_block = "\n".join(G.render_vita_laser_profile_begin(spec))
        end_block = "\n".join(G.render_vita_laser_profile_end(spec))
        for base in (DEFAULT_BASE, VITA_BASE):
            emit = Image(str(pe_path), base)
            result = G._translate_function(
                emit, {"rva": root}, None, info, pin_img=pin
            )
            assert result["stub"] is None
            assert result["vita_laser_profile"] is True
            source = legacy_text(result["text"])
            comment_at = source.index("    /* %08x" % spec["site"])
            original_at = source.index(
                "    " + spec["statement"], comment_at
            )
            block_at = source.index(begin_block, original_at)
            next_at = source.index(
                "    /* %08x" % spec["window_end"], block_at
            )
            assert comment_at < original_at < block_at < next_at
            epilogue_label_at = source.index(
                "L_%08x:" % spec["epilogue_site"]
            )
            end_block_at = source.index(end_block, epilogue_label_at)
            epilogue_comment_at = source.index(
                "    /* %08x" % spec["epilogue_site"], end_block_at
            )
            assert epilogue_label_at < end_block_at < epilogue_comment_at
            assert source.count(begin_block) == 1
            assert source.count(end_block) == 1
            assert source.count(FENCE) == 2
            assert begin_block.count(
                "kage_vita_phase_profile_laser_begin(") == 2
            assert end_block.count(
                "kage_vita_phase_profile_laser_end(") == 2
            assert (FENCE + "\n    ;\n    extern void "
                    "kage_vita_phase_profile_laser_end(") in end_block
            assert "c->ebp," in begin_block
            assert "c->ebp," in end_block
            assert " + 0x34U" in begin_block
            assert " + 0x2cU" in begin_block
            assert " + 0x30U" in begin_block
            assert " + 0x3adU" in begin_block
            assert "st8(" not in begin_block and "st32(" not in begin_block
            assert "st8(" not in end_block and "st32(" not in end_block
            assert "goto " not in begin_block and "goto " not in end_block
            rendered_by_base[(root, base)] = (begin_block, end_block)
        assert (rendered_by_base[(root, DEFAULT_BASE)] ==
                rendered_by_base[(root, VITA_BASE)])

    # Mutating either active-path proof must fail even when stale decode and
    # emitted-C objects are deliberately reused.
    pin.mem = bytearray(pin.mem)
    for root in sorted(G.VITA_LASER_PROFILE_ROOTS):
        spec = G.VITA_LASER_PROFILE_OWNERS[root]
        em, insns, order, body = raw_by_root[root]
        info = switch_by_root[root][root]
        for rva in (spec["site"], spec["epilogue_site"], root):
            original = pin.mem[rva]
            pin.mem[rva] ^= 0x01
            try:
                G.vita_laser_profile_for_body(
                    pin, root, em, insns, order, body,
                    info["entries"], info["edges"],
                )
            except RuntimeError as exc:
                assert "laser" in str(exc)
            else:
                raise AssertionError(
                    "mutated laser proof passed at 0x%08x" % rva
                )
            finally:
                pin.mem[rva] = original


def _verify_host_oracle() -> None:
    result = G.vita_laser_profile_host_oracle((
        (1, 0, 0, 0, 0),
        (39, 1, 1, 1, 0),
        (39, 1, 1, 1, 1),
        (1000, 9, 4, 2, 0),
        (39, 1, 1, 0x80, 0),
        (39, 1, 1, 0, 1),
    ))
    assert result == {
        "render": 4, "shadow": 2,
        "sample_zero": 1, "sample_positive": 2,
        "source_1": 1, "source_10_999": 2, "source_other": 1,
        "variant_0": 1, "variant_1": 2, "variant_2": 0,
        "variant_other": 1, "subtype_1_3": 2, "subtype_other": 2,
        "bad": 2,
    }


def _verify_build_switch() -> None:
    cmake = (HERE / "vita/CMakeLists.txt").read_text(encoding="utf-8")
    build_tool = (HERE.parent / "tools/build_vita.py").read_text(
        encoding="utf-8"
    )
    option = re.search(
        r"option\(ISAAC_VITA_LASER_PROFILE\s+.*?\s+(ON|OFF)\)",
        cmake,
        flags=re.S,
    )
    assert option is not None and option.group(1) == "OFF"
    assert "requires the translated KAGE runtime" in cmake[option.end():]
    assert "requires ISAAC_VITA_PHASE_PROFILE=ON" in cmake[option.end():]
    assert cmake.count("ISAAC_VITA_LASER_PROFILE=1") == 2
    assert '"^void sub_004d(1330|5090)' in cmake
    assert build_tool.count('"-DISAAC_VITA_LASER_PROFILE=OFF"') == 2
    assert '"ISAAC_VITA_LASER_PROFILE": False' in build_tool


def run(pe_path: Path) -> None:
    pe_path = pe_path.resolve()
    assert pe_path.stat().st_size == G.VITA_PILL_BLOOM_BYPASS_PE_SIZE
    assert _sha256(pe_path) == G.VITA_PILL_BLOOM_BYPASS_PE_SHA256
    _verify_generator(pe_path)
    _verify_host_oracle()
    _verify_build_switch()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", type=Path, required=True)
    args = parser.parse_args()
    run(args.pe)
    print("Vita laser Render/Shadow codegen oracle: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
