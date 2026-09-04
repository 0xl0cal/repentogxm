#!/usr/bin/env python3
"""Frozen-PE codegen oracle for the selective Vita pill-Bloom bypass."""

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
    "defined(ISAAC_VITA_PILL_BLOOM_BYPASS)"
)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _raw_translation(img: Image):
    result = B.translate(
        img,
        G.VITA_PILL_BLOOM_BYPASS_ROOT_RVA,
        "sub_002ce770",
        None,
    )
    em, insns, order, body, unsupported, indirect, _members = result
    assert unsupported == []
    assert indirect == []
    return em, insns, order, body


def _verify_rendered_site(source: str, site: int, next_site: int) -> None:
    original = {
        G.VITA_PILL_BLOOM_BYPASS_CAPTURE_SITE_RVA:
            G.VITA_PILL_BLOOM_BYPASS_CAPTURE_STATEMENT,
        G.VITA_PILL_BLOOM_BYPASS_COMPOSITE_SITE_RVA:
            G.VITA_PILL_BLOOM_BYPASS_COMPOSITE_STATEMENT,
    }[site]
    block = "\n".join(G.render_vita_pill_bloom_bypass_site(site))
    comment_at = source.index("    /* %08x" % site)
    original_at = source.index("    " + original, comment_at)
    block_at = source.index(block, original_at)
    next_at = source.index("    /* %08x" % next_site, block_at)
    assert comment_at < original_at < block_at < next_at
    assert source.count(block) == 1


def _verify_generator(pe_path: Path) -> None:
    pin = Image(str(pe_path), DEFAULT_BASE)
    em, insns, order, body = _raw_translation(pin)
    assert G.vita_pill_bloom_bypass_for_body(
        pin,
        G.VITA_PILL_BLOOM_BYPASS_ROOT_RVA,
        em,
        insns,
        order,
        body,
    )

    local = "\n".join(G.render_vita_pill_bloom_bypass_local())
    rendered_by_base = {}
    for base in (DEFAULT_BASE, VITA_BASE):
        emit = Image(str(pe_path), base)
        result = G._translate_function(
            emit,
            {"rva": G.VITA_PILL_BLOOM_BYPASS_ROOT_RVA},
            None,
            {},
            pin_img=pin,
        )
        assert result["stub"] is None
        assert result["vita_pill_bloom_bypass"] is True
        source = legacy_text(result["text"])
        assert source.count(FENCE) == 3
        assert source.count(local) == 1
        assert source.index(local) < source.index("    /* 002ce770")
        _verify_rendered_site(
            source, G.VITA_PILL_BLOOM_BYPASS_CAPTURE_SITE_RVA, 0x002CEA8C
        )
        _verify_rendered_site(
            source, G.VITA_PILL_BLOOM_BYPASS_COMPOSITE_SITE_RVA, 0x002CF367
        )
        assert source.count(
            "kage_vita_phase_profile_note_pill_bloom_capture_skip();"
        ) == 1
        assert source.count(
            "kage_vita_phase_profile_note_pill_bloom_composite_skip();"
        ) == 1
        assert local.count("defined(ISAAC_VITA_PHASE_RECEIPTS)") == 1
        for site in (
            G.VITA_PILL_BLOOM_BYPASS_CAPTURE_SITE_RVA,
            G.VITA_PILL_BLOOM_BYPASS_COMPOSITE_SITE_RVA,
        ):
            block = "\n".join(G.render_vita_pill_bloom_bypass_site(site))
            assert block.count("defined(ISAAC_VITA_PHASE_RECEIPTS)") == 1
        assert source.count("goto L_002ceaad;") >= 2
        assert source.count("goto L_002cf5a1;") >= 3
        rendered_by_base[base] = (
            local,
            "\n".join(G.render_vita_pill_bloom_bypass_site(
                G.VITA_PILL_BLOOM_BYPASS_CAPTURE_SITE_RVA
            )),
            "\n".join(G.render_vita_pill_bloom_bypass_site(
                G.VITA_PILL_BLOOM_BYPASS_COMPOSITE_SITE_RVA
            )),
        )
    assert rendered_by_base[DEFAULT_BASE] == rendered_by_base[VITA_BASE]

    # Each independent proof dimension must fail closed even if stale decoded
    # instruction and C-emission objects are accidentally reused by a caller.
    pin.mem = bytearray(pin.mem)
    mutations = (
        G.VITA_PILL_BLOOM_BYPASS_CAPTURE_SITE_RVA,
        G.VITA_PILL_BLOOM_BYPASS_COMPOSITE_SITE_RVA,
        G.VITA_PILL_BLOOM_BYPASS_TRIGGER_WINDOW_RVA,
        G.VITA_PILL_BLOOM_BYPASS_CREATE_WINDOW_RVA,
        G.VITA_PILL_BLOOM_BYPASS_STRING_RVA,
    )
    for rva in mutations:
        original = pin.mem[rva]
        pin.mem[rva] ^= 0x01
        try:
            G.vita_pill_bloom_bypass_for_body(
                pin,
                G.VITA_PILL_BLOOM_BYPASS_ROOT_RVA,
                em,
                insns,
                order,
                body,
            )
        except RuntimeError as exc:
            assert "pill-Bloom" in str(exc)
        else:
            raise AssertionError(
                "mutated pill-Bloom proof passed at 0x%08x" % rva
            )
        finally:
            pin.mem[rva] = original


def _verify_host_oracle() -> None:
    oracle = G.vita_pill_bloom_bypass_host_oracle
    assert oracle(0, True, True) == {
        "capture_pushes": 0, "composite_pops": 0,
        "capture_skips": 0, "composite_skips": 0,
    }
    assert oracle(30, False, True) == {
        "capture_pushes": 0, "composite_pops": 0,
        "capture_skips": 0, "composite_skips": 0,
    }
    assert oracle(30, True, False) == {
        "capture_pushes": 1, "composite_pops": 1,
        "capture_skips": 0, "composite_skips": 0,
    }
    assert oracle(30, True, True) == {
        "capture_pushes": 0, "composite_pops": 0,
        "capture_skips": 1, "composite_skips": 1,
    }


def _verify_build_switch() -> None:
    cmake = (HERE / "vita/CMakeLists.txt").read_text(encoding="utf-8")
    build_tool = (HERE.parent / "tools/build_vita.py").read_text(
        encoding="utf-8"
    )
    option = re.search(
        r"option\(ISAAC_VITA_PILL_BLOOM_BYPASS\s+.*?\s+(ON|OFF)\)",
        cmake,
        flags=re.S,
    )
    assert option is not None and option.group(1) == "OFF"
    assert "requires the translated KAGE runtime" in cmake[option.end():]
    assert "ISAAC_VITA_PILL_BLOOM_BYPASS requires ISAAC_VITA_PHASE_PROFILE" \
        not in cmake[option.end():]
    assert cmake.count("ISAAC_VITA_PILL_BLOOM_BYPASS=1") == 2
    # Game::Render is independently selected by this bypass and the default-off
    # Utero world-seam diagnostic; each owner census remains exact.
    assert cmake.count("sub_002ce770") == 2
    assert build_tool.count('"-DISAAC_VITA_PILL_BLOOM_BYPASS=OFF"') == 2
    assert '"ISAAC_VITA_PILL_BLOOM_BYPASS": False' in build_tool


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
    print("selective Vita pill-Bloom host/codegen oracle: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
