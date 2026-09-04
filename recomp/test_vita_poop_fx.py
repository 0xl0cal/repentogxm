#!/usr/bin/env python3
"""Frozen-J835 oracle for PoopFx attribution and one-cloud codegen."""

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


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _raw_translation(img: Image, root: int):
    result = B.translate(img, root, "vita_poop_fx_probe")
    em, insns, order, body, unsupported, indirect, _members = result
    assert unsupported == []
    assert indirect == []
    return em, insns, order, body


def _verify_generator(pe_path: Path) -> None:
    pin = Image(str(pe_path), DEFAULT_BASE)
    raw = {}
    for root in sorted(G.VITA_POOP_FX_ROOTS):
        em, insns, order, body = _raw_translation(pin, root)
        kind = G.vita_poop_fx_for_body(
            pin, root, em, insns, order, body
        )
        assert kind == G.VITA_POOP_FX_OWNERS[root]["kind"]
        raw[root] = (em, insns, order, body)
        for base in (DEFAULT_BASE, VITA_BASE):
            result = G._translate_function(
                Image(str(pe_path), base), {"rva": root}, None, {},
                pin_img=pin,
            )
            assert result["stub"] is None
            assert result["vita_poop_fx"] is True
            source = legacy_text(result["text"])
            if kind == "add":
                block = "\n".join(G.render_vita_poop_fx_site(
                    kind, G.VITA_POOP_FX_ADD_CAPTURE_SITE_RVA
                ))
                comment_at = source.index("    /* 004edaad  ")
                original_at = source.index(
                    "    " + G.VITA_POOP_FX_ADD_CAPTURE_STATEMENT,
                    comment_at,
                )
                block_at = source.index(block, original_at)
                next_at = source.index("    /* 004edab0  ", block_at)
                assert comment_at < original_at < block_at < next_at
                assert source.count(block) == 1
                assert "note_poop_fx_add(c->ebp)" in block
                assert "defined(ISAAC_VITA_PHASE_RECEIPTS)" in block
            else:
                active = "\n".join(G.render_vita_poop_fx_site(
                    kind, G.VITA_POOP_FX_RENDER_ACTIVE_SITE_RVA
                ))
                cap = "\n".join(G.render_vita_poop_fx_site(
                    kind, G.VITA_POOP_FX_RENDER_CAP_SITE_RVA
                ))
                active_comment = source.index("    /* 004ec566  ")
                active_original = source.index(
                    "    " + G.VITA_POOP_FX_RENDER_ACTIVE_STATEMENT,
                    active_comment,
                )
                active_at = source.index(active, active_original)
                cap_comment = source.index("    /* 004ec785  ")
                cap_at = source.index(cap, cap_comment)
                branch_comment = source.index("    /* 004ec788  ", cap_at)
                assert active_comment < active_original < active_at
                assert cap_comment < cap_at < branch_comment
                assert source.count(active) == 1
                assert source.count(cap) == 1
                assert "c->edi == 1U" in cap
                assert " + 0x50U" in cap
                assert "note_poop_fx_cap(2U)" in cap
                assert "defined(ISAAC_VITA_PHASE_RECEIPTS)" in active
                assert "defined(ISAAC_VITA_PHASE_RECEIPTS)" in cap
                assert "goto L_004ec78e;" in cap
                assert "st32(" not in active and "st32(" not in cap

    # Reuse stale decode/emission objects deliberately: every independent raw
    # ownership oracle must still notice drift in the PE itself.
    pin.mem = bytearray(pin.mem)
    mutation_cases = (
        (G.VITA_POOP_FX_RENDER_ROOT_RVA,
         G.VITA_POOP_FX_RENDER_CAP_SITE_RVA),
        (G.VITA_POOP_FX_ADD_ROOT_RVA,
         G.VITA_POOP_FX_ADD_CAPTURE_SITE_RVA),
        (G.VITA_POOP_FX_ADD_ROOT_RVA,
         G.VITA_POOP_FX_DIRECT_CALL_SITES[0]),
        (G.VITA_POOP_FX_ADD_ROOT_RVA,
         G.VITA_POOP_FX_ASSET_STRING_RVA),
    )
    for root, rva in mutation_cases:
        em, insns, order, body = raw[root]
        original = pin.mem[rva]
        pin.mem[rva] ^= 1
        try:
            G.vita_poop_fx_for_body(pin, root, em, insns, order, body)
        except RuntimeError as exc:
            assert "PoopFx" in str(exc)
        else:
            raise AssertionError(
                "mutated PoopFx proof passed at 0x%08x" % rva
            )
        finally:
            pin.mem[rva] = original


def _verify_host_oracle() -> None:
    other = next(
        value for value in G.VITA_POOP_FX_DIRECT_RETURN_RVAS
        if value not in {
            G.VITA_POOP_FX_PILL_RETURN_RVA,
            G.VITA_POOP_FX_BLACK_CORRELATED_RETURN_RVA,
        }
    )
    result = G.vita_poop_fx_profile_host_oracle(
        (
            G.VITA_POOP_FX_PILL_RETURN_RVA,
            G.VITA_POOP_FX_BLACK_CORRELATED_RETURN_RVA,
            other,
            0xDEADC0DE,
        ),
        ((180, 3, 1), (179, 3, 1), (200, 4, 1)),
        single_cloud=True,
    )
    assert result == {
        "add_pill": 1, "add_black": 1,
        "add_other": 1, "add_unknown": 1,
        "active": 3, "clouds": 10,
        "countdown_min": 179, "countdown_max": 200,
        "countdown_last": 200,
        "cap_frames": 2, "cap_skipped": 4, "bad": 2,
    }


def _verify_build_switch() -> None:
    cmake = (HERE / "vita/CMakeLists.txt").read_text(encoding="utf-8")
    build_tool = (HERE.parent / "tools/build_vita.py").read_text(
        encoding="utf-8"
    )
    for name in (
        "ISAAC_VITA_POOP_FX_PROFILE",
        "ISAAC_VITA_POOP_FX_SINGLE_CLOUD",
    ):
        option = re.search(
            rf"option\({name}\s+.*?\s+(ON|OFF)\)", cmake, flags=re.S
        )
        assert option is not None and option.group(1) == "OFF"
        assert build_tool.count(f'"-D{name}=OFF"') == 2
        assert f'"{name}": False' in build_tool
    assert "ISAAC_VITA_POOP_FX_PROFILE requires ISAAC_VITA_PHASE_PROFILE" \
        not in cmake
    assert "requires ISAAC_VITA_POOP_FX_PROFILE=ON" in cmake
    assert cmake.count("ISAAC_VITA_POOP_FX_PROFILE=1") == 2
    assert cmake.count("ISAAC_VITA_POOP_FX_SINGLE_CLOUD=1") == 1
    assert '"^void sub_004e(c3b0|daa0)' in cmake


def run(pe_path: Path) -> None:
    pe_path = pe_path.resolve()
    assert pe_path.stat().st_size == G.VITA_POOP_FX_PE_SIZE
    assert _sha256(pe_path) == G.VITA_POOP_FX_PE_SHA256
    _verify_generator(pe_path)
    _verify_host_oracle()
    _verify_build_switch()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", type=Path, required=True)
    args = parser.parse_args()
    run(args.pe)
    print("Vita PoopFx profile/single-cloud codegen oracle: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
