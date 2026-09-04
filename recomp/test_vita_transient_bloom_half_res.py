#!/usr/bin/env python3
"""Frozen-PE oracle for the half-resolution transient-Bloom surface."""

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
FENCE = (
    "#if defined(__vita__) && "
    "defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)"
)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _raw_translation(img: Image, root: int):
    result = B.translate(img, root, "bloom_half_probe", None)
    em, insns, order, body, unsupported, indirect, _members = result
    assert unsupported == []
    assert indirect == []
    return em, insns, order, body


def _verify_block_after_site(
        source: str, site: int, next_site: int, block: str) -> None:
    comment_at = source.index("    /* %08x" % site)
    block_at = source.index(block, comment_at)
    next_at = source.index("    /* %08x" % next_site, block_at)
    assert comment_at < block_at < next_at
    assert source.count(block) == 1


def _verify_generator(pe_path: Path) -> None:
    pin = Image(str(pe_path), DEFAULT_BASE)
    raw = {}
    for root in sorted(G.VITA_TRANSIENT_BLOOM_HALF_RES_ROOTS):
        raw[root] = _raw_translation(pin, root)
        em, insns, order, body = raw[root]
        expected = (
            "create"
            if root == G.VITA_TRANSIENT_BLOOM_HALF_RES_CREATE_ROOT_RVA
            else "render"
        )
        assert G.vita_transient_bloom_half_res_for_body(
            pin, root, em, insns, order, body
        ) == expected

    create_block = "\n".join(
        G.render_vita_transient_bloom_half_res_create()
    )
    render_blocks = {
        site: "\n".join(G.render_vita_transient_bloom_half_res_render(site))
        for site in G.VITA_TRANSIENT_BLOOM_HALF_RES_RENDER_SITES
    }
    rendered_by_base = {}
    for base in (DEFAULT_BASE, VITA_BASE):
        emit = Image(str(pe_path), base)
        for root in sorted(G.VITA_TRANSIENT_BLOOM_HALF_RES_ROOTS):
            result = G._translate_function(
                emit, {"rva": root}, None, {}, pin_img=pin
            )
            assert result["stub"] is None
            assert result["vita_transient_bloom_half_res"] is True
            source = legacy_text(result["text"])
            if root == G.VITA_TRANSIENT_BLOOM_HALF_RES_CREATE_ROOT_RVA:
                em, insns, order, body = raw[root]
                width_index = order.index(
                    G.VITA_TRANSIENT_BLOOM_HALF_RES_WIDTH_SITE_RVA
                )
                next_site = order[width_index + 1]
                _verify_block_after_site(
                    source,
                    G.VITA_TRANSIENT_BLOOM_HALF_RES_WIDTH_SITE_RVA,
                    next_site,
                    create_block,
                )
                assert source.count(create_block) == 1
                assert source.count(FENCE) == 1
                initial = create_block.index(
                    "_guest_vita_bloom_width == 480U"
                )
                recreated = create_block.index(
                    "_guest_vita_bloom_width == 960U"
                )
                mutation = create_block.index(
                    "_guest_vita_bloom_width / 2U"
                )
                assert initial < recreated < mutation
                assert "_guest_vita_bloom_height == 270U" in create_block
                assert "_guest_vita_bloom_height == 540U" in create_block
                assert "_guest_vita_bloom_width / 2U" in create_block
                assert "_guest_vita_bloom_height / 2U" in create_block
                assert create_block.count(
                    "defined(ISAAC_VITA_PHASE_RECEIPTS)"
                ) == 4
                assert (
                    "kage_vita_phase_profile_note_bloom_half_create(1U);"
                    in create_block[initial:recreated]
                )
                assert (
                    "kage_vita_phase_profile_note_bloom_half_create(2U);"
                    in create_block[recreated:]
                )
                # The height push is untouched; both arguments move only
                # after the width push has made the pair adjacent on stack.
                height_comment = "    /* %08x" % (
                    G.VITA_TRANSIENT_BLOOM_HALF_RES_HEIGHT_SITE_RVA
                )
                width_comment = "    /* %08x" % (
                    G.VITA_TRANSIENT_BLOOM_HALF_RES_WIDTH_SITE_RVA
                )
                assert source.index(height_comment) < source.index(width_comment)
                assert source.index(create_block) > source.index(width_comment)
                rendered_by_base[(root, base)] = create_block
            else:
                em, insns, order, body = raw[root]
                for site, block in render_blocks.items():
                    site_index = order.index(site)
                    _verify_block_after_site(
                        source, site, order[site_index + 1], block
                    )
                assert source.count(FENCE) == 2
                rendered_by_base[(root, base)] = tuple(
                    render_blocks[site]
                    for site in sorted(render_blocks)
                )
    for root in G.VITA_TRANSIENT_BLOOM_HALF_RES_ROOTS:
        assert (rendered_by_base[(root, DEFAULT_BASE)] ==
                rendered_by_base[(root, VITA_BASE)])

    # Full owner and lifecycle mutations must all reject even if a caller
    # accidentally reuses stale decoded instructions and emitted C.
    pin.mem = bytearray(pin.mem)
    create_root = G.VITA_TRANSIENT_BLOOM_HALF_RES_CREATE_ROOT_RVA
    create_raw = raw[create_root]
    mutations = [
        G.VITA_TRANSIENT_BLOOM_HALF_RES_WIDTH_SITE_RVA,
        G.VITA_TRANSIENT_BLOOM_HALF_RES_HEIGHT_SITE_RVA,
        G.VITA_TRANSIENT_BLOOM_HALF_RES_CREATE_CALL_SITES[0],
        0x007682AC,
        0x007AA92C,
        G.VITA_PILL_BLOOM_BYPASS_STRING_RVA,
    ]
    mutations.extend(
        window[0] for window in (
            G.VITA_TRANSIENT_BLOOM_HALF_RES_ACTIVATE_WINDOW,
            G.VITA_TRANSIENT_BLOOM_HALF_RES_PROJECTION_WINDOW,
            G.VITA_TRANSIENT_BLOOM_HALF_RES_SURFACE_WINDOW,
            G.VITA_TRANSIENT_BLOOM_HALF_RES_UV_WINDOW,
            G.VITA_TRANSIENT_BLOOM_HALF_RES_DRAW_DISPATCH_WINDOW,
            G.VITA_TRANSIENT_BLOOM_HALF_RES_QUAD_WINDOW,
            G.VITA_TRANSIENT_BLOOM_HALF_RES_COMPOSITE_WINDOW,
            G.VITA_TRANSIENT_BLOOM_HALF_RES_GLOBALS_WINDOW,
            G.VITA_TRANSIENT_BLOOM_HALF_RES_RESIZE_WINDOW,
            G.VITA_TRANSIENT_BLOOM_HALF_RES_RECREATE_WINDOW,
        )
    )
    for rva in mutations:
        original = pin.mem[rva]
        pin.mem[rva] ^= 0x01
        try:
            G.vita_transient_bloom_half_res_for_body(
                pin, create_root, *create_raw
            )
        except RuntimeError as exc:
            assert "Bloom" in str(exc)
        else:
            raise AssertionError(
                "mutated transient-Bloom proof passed at 0x%08x" % rva
            )
        finally:
            pin.mem[rva] = original


def _verify_host_oracle() -> None:
    oracle = G.vita_transient_bloom_half_res_host_oracle
    initial = oracle(480, 270, align8=True)
    assert initial == {
        "receipt": "initial",
        "applied": False,
        "requested": (480, 270),
        "backing": (480, 272),
        "viewport": (480, 270),
        "projection": (480, 270),
        "composite": (480, 270),
        "uv_extent": (1.0, 270 / 272),
        "texel_step": (1 / 480, 1 / 272),
    }
    assert oracle(480, 270, align8=False)["backing"] == (512, 512)
    logical = oracle(960, 540, align8=True)
    assert logical == {
        "receipt": "recreated",
        "applied": True,
        "requested": (480, 270),
        "backing": (480, 272),
        "viewport": (480, 270),
        "projection": (480, 270),
        "composite": (480, 270),
        "uv_extent": (1.0, 270 / 272),
        "texel_step": (1 / 480, 1 / 272),
    }
    rejected = oracle(481, 270, align8=True)
    assert rejected["receipt"] == "rejected"
    assert rejected["applied"] is False
    assert rejected["requested"] == (481, 270)
    assert rejected["viewport"] == (481, 270)
    assert rejected["projection"] == (480, 270)
    assert rejected["composite"] == (480, 270)
    # The 960x544 scanout backing is physical padding, never a logical
    # creation request; accepting it would crop four real rows after halving.
    physical = oracle(960, 544, align8=True)
    assert physical["receipt"] == "rejected"
    assert physical["applied"] is False
    assert physical["requested"] == (960, 544)


def _verify_build_switch() -> None:
    cmake = (HERE / "vita/CMakeLists.txt").read_text(encoding="utf-8")
    build_tool = (HERE.parent / "tools/build_vita.py").read_text(
        encoding="utf-8"
    )
    header = (HERE / "runtime/kage_vita_phase_profile.h").read_text(
        encoding="utf-8"
    )
    runtime = (HERE / "runtime/kage_vita_phase_profile.c").read_text(
        encoding="utf-8"
    )
    manual_kage = (HERE / "runtime/manual_kage_vita.c").read_text(
        encoding="utf-8"
    )
    backend = (HERE / "runtime/kage_vita_backend.c").read_text(
        encoding="utf-8"
    )
    option = re.search(
        r"option\(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES\s+.*?\s+(ON|OFF)\)",
        cmake,
        flags=re.S,
    )
    assert option is not None and option.group(1) == "OFF"
    policy = cmake[option.end():]
    assert "requires the translated KAGE runtime" in policy
    assert "ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES requires " \
        "ISAAC_VITA_PHASE_PROFILE" not in policy
    assert "requires ISAAC_VITA_TEXTURE_ALIGN8_POLICY=ON" in policy
    assert "mutually exclusive performance modes" in policy
    assert cmake.count("ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES=1") == 2
    assert '"^void sub_002c(7e70|e770)' in cmake
    assert build_tool.count(
        '"-DISAAC_VITA_TRANSIENT_BLOOM_HALF_RES=OFF"'
    ) == 2
    assert '"ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES": False' in build_tool
    assert "Bloom bypass and half-resolution modes are mutually exclusive" \
        in header
    assert "ph120.h" in runtime
    assert "half(create i,a,r)=" in runtime
    assert "bloom(c,o)=" in runtime
    assert "#define KAGE_DEFAULT_WIDTH       960u" in manual_kage
    assert "#define KAGE_DEFAULT_HEIGHT      540u" in manual_kage
    assert "width = kage_vita_backend_width();" in manual_kage
    assert "height = kage_vita_backend_height();" in manual_kage
    assert "#define KAGE_VITA_LOGICAL_WIDTH   960u" in backend
    assert "#define KAGE_VITA_LOGICAL_HEIGHT  540u" in backend


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
    print("Vita transient-Bloom half-resolution host/codegen oracle: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
