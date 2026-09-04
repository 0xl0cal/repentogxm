#!/usr/bin/env python3
"""Frozen-PE host/codegen oracle for the original Vita effects profile."""

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
    "defined(ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE)"
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
        G.VITA_ORIGINAL_EFFECTS_PROFILE_ROOT_RVA,
        "sub_0047fbe0",
        None,
    )
    em, insns, order, body, unsupported, indirect, _members = result
    assert unsupported == []
    assert indirect == []
    return em, insns, order, body


def _verify_generator(pe_path: Path) -> None:
    pin = Image(str(pe_path), DEFAULT_BASE)
    em, insns, order, body = _raw_translation(pin)
    assert G.vita_original_effects_profile_for_body(
        pin,
        G.VITA_ORIGINAL_EFFECTS_PROFILE_ROOT_RVA,
        em,
        insns,
        order,
        body,
    )

    expected_block = "\n".join(
        G.render_vita_original_effects_profile()
    )
    blocks = {}
    for base in (DEFAULT_BASE, VITA_BASE):
        emit = Image(str(pe_path), base)
        result = G._translate_function(
            emit,
            {"rva": G.VITA_ORIGINAL_EFFECTS_PROFILE_ROOT_RVA},
            None,
            {},
            pin_img=pin,
        )
        assert result["stub"] is None
        assert result["vita_original_effects_profile"] is True
        source = legacy_text(result["text"])
        # One block after the last default store (reached on every path) and
        # one after the last GetBool store (reached only with config.ini).
        assert source.count(FENCE) == 2
        assert source.count(expected_block) == 2

        default_comment = (
            "    /* %08x" % G.VITA_ORIGINAL_EFFECTS_PROFILE_DEFAULT_SITE_RVA
        )
        default_next_comment = "    /* 0047fc2b"
        default_at = source.index(default_comment)
        default_store_at = source.index(
            "    " + G.VITA_ORIGINAL_EFFECTS_PROFILE_DEFAULT_SITE_STATEMENT,
            default_at,
        )
        default_block_at = source.index(expected_block, default_store_at)
        default_next_at = source.index(default_next_comment, default_block_at)
        assert default_at < default_store_at < default_block_at < default_next_at

        site_comment = "    /* %08x" % G.VITA_ORIGINAL_EFFECTS_PROFILE_SITE_RVA
        next_comment = "    /* 0047fe37"
        site_at = source.index(site_comment)
        original_at = source.index(
            "    " + G.VITA_ORIGINAL_EFFECTS_PROFILE_SITE_STATEMENT,
            site_at,
        )
        block_at = source.index(expected_block, original_at)
        next_at = source.index(next_comment, block_at)
        assert site_at < original_at < block_at < next_at
        assert default_block_at < site_at
        blocks[base] = expected_block
    assert blocks[DEFAULT_BASE] == blocks[VITA_BASE]

    # Machine, owner and source-name drift must each fail closed even if a
    # caller accidentally reuses the original decoded instruction objects.
    pin.mem = bytearray(pin.mem)
    mutations = (
        G.VITA_ORIGINAL_EFFECTS_PROFILE_SITE_RVA,
        G.VITA_ORIGINAL_EFFECTS_PROFILE_DEFAULT_SITE_RVA,
        G.VITA_ORIGINAL_EFFECTS_PROFILE_EARLY_EXIT_RVA,
        G.VITA_ORIGINAL_EFFECTS_PROFILE_ROOT_RVA,
        G.VITA_ORIGINAL_EFFECTS_PROFILE_STRING_RVAS[
            G.VITA_ORIGINAL_EFFECTS_PROFILE_FILTER_OFFSET
        ][0],
        G.VITA_ORIGINAL_EFFECTS_PROFILE_STRING_RVAS[
            G.VITA_ORIGINAL_EFFECTS_PROFILE_WATER_OFFSET
        ][0],
        G.VITA_ORIGINAL_EFFECTS_PROFILE_STRING_RVAS[
            G.VITA_ORIGINAL_EFFECTS_PROFILE_COLOR_MODIFIER_OFFSET
        ][0],
    )
    for rva in mutations:
        original = pin.mem[rva]
        pin.mem[rva] ^= 0x01
        try:
            G.vita_original_effects_profile_for_body(
                pin,
                G.VITA_ORIGINAL_EFFECTS_PROFILE_ROOT_RVA,
                em,
                insns,
                order,
                body,
            )
        except RuntimeError as exc:
            assert "original-Vita" in str(exc)
        else:
            raise AssertionError(
                "mutated original-Vita effects owner passed at 0x%08x" % rva
            )
        finally:
            pin.mem[rva] = original


def _verify_host_oracle() -> None:
    before = bytearray(range(0x90))
    before[0x7C] = 1  # Color correction differs across Vita revisions.
    before[0x7D] = 1  # Lighting remains enabled on both revisions.
    before[0x7E] = 1  # Shockwave: PC default, disabled on Vita.
    before[0x7F] = 1  # Caustics: PC default, disabled on Vita.
    before[0x80] = 1  # Filter: PC default, disabled on Vita.
    before[0x81] = 1  # Pixelation differs across Vita revisions.
    before[0x83] = 1  # Repentance water surface has no Rebirth equivalent.
    before[0x84] = 1  # Loader's copied water-surface latch.
    before[0x85] = 1  # Repentance whole-room ColorModifier surface.
    after = G.vita_original_effects_profile_host_oracle(before)

    changed = {
        index for index, (old, new) in enumerate(zip(before, after))
        if old != new
    }
    assert changed == set(G.VITA_ORIGINAL_EFFECTS_PROFILE_FIELDS)
    assert after[0x7C] == 1
    assert after[0x7D] == 1
    assert after[0x7E] == 0
    assert after[0x7F] == 0
    assert after[0x80] == 0
    assert after[0x81] == 1
    assert after[0x83] == 0
    assert after[0x84] == 0
    assert after[0x85] == 0

    try:
        G.vita_original_effects_profile_host_oracle(b"short")
    except ValueError:
        pass
    else:
        raise AssertionError("short OptionsConfig image was accepted")


def _verify_build_switch() -> None:
    cmake = (HERE / "vita/CMakeLists.txt").read_text(encoding="utf-8")
    build_tool = (HERE.parent / "tools/build_vita.py").read_text(
        encoding="utf-8"
    )
    option = re.search(
        r"option\(ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE\s+.*?\s+(ON|OFF)\)",
        cmake,
        flags=re.S,
    )
    assert option is not None and option.group(1) == "OFF"
    assert "ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE requires the translated" in cmake
    assert cmake.count("ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE=1") == 1
    assert cmake.count("sub_0047fbe0") == 1
    assert build_tool.count(
        '"-DISAAC_VITA_ORIGINAL_EFFECTS_PROFILE=OFF"'
    ) == 2
    assert '"ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE": False' in build_tool


def run(pe_path: Path) -> None:
    pe_path = pe_path.resolve()
    assert pe_path.stat().st_size == G.VITA_ORIGINAL_EFFECTS_PROFILE_PE_SIZE
    assert _sha256(pe_path) == G.VITA_ORIGINAL_EFFECTS_PROFILE_PE_SHA256
    _verify_generator(pe_path)
    _verify_host_oracle()
    _verify_build_switch()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", type=Path, required=True)
    args = parser.parse_args()
    run(args.pe)
    print("original Vita effects host/codegen oracle: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
