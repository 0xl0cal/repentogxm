#!/usr/bin/env python3
"""Frozen-PE host/codegen oracle for the opt-in vanilla/KAGE bundle."""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import build_one as B  # noqa: E402
import gen_all as G  # noqa: E402
from gpr_locals import legacy_text  # noqa: E402
from image import DEFAULT_BASE, Image  # noqa: E402


VITA_BASE = 0x98000000
FENCE = (
    "#if defined(__vita__) && "
    "defined(ISAAC_VITA_VANILLA_KAGE_BUNDLE)"
)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _tear_switch_info(img: Image, root: int) -> dict:
    targets = G._validate_entity_tear_update_owner_identity(img)
    sites = tuple(
        site for site in
        dict(G.ENTITY_TEAR_UPDATE_SWITCH_OWNER["root_indirect_order"])[root]
        if site in targets
    )
    entries = tuple(sorted({target for site in sites for target in targets[site]}))
    return {
        root: {
            "entries": entries,
            "edges": {
                site: tuple(dict.fromkeys(targets[site])) for site in sites
            },
            "tables": {site: targets[site] for site in sites},
        }
    }


def _raw_translation(img: Image, root: int, switch_info: dict):
    info = switch_info.get(root, {})
    result = B.translate(
        img,
        root,
        "vanilla_kage_bundle_probe",
        None,
        info.get("entries", ()),
        info.get("edges", {}),
    )
    em, insns, order, body, unsupported, indirect, _members = result
    assert unsupported == []
    if root in G.VITA_VANILLA_KAGE_GODHEAD_ROOTS:
        expected = dict(G.ENTITY_TEAR_UPDATE_SWITCH_OWNER["root_indirect_order"])[root]
        assert tuple(indirect) == expected
    else:
        assert indirect == []
    return em, insns, order, body


def _verify_rendered_site(source: str, spec: dict) -> None:
    site = spec["site"]
    block = "\n".join(G.render_vita_vanilla_kage_bundle(
        spec,
        ({
            "godhead_partition": G.VITA_VANILLA_KAGE_GODHEAD_STATEMENT,
            "compatible_stride": G.VITA_VANILLA_KAGE_STRIDE_STATEMENT,
            "same_image_invalidation":
                G.VITA_VANILLA_KAGE_INVALIDATE_STATEMENT,
        }[spec["kind"]],),
    ))
    comment = "    /* %08x" % site
    comment_at = source.index(comment)
    block_at = source.index(block, comment_at)
    next_comment_at = source.index("    /* ", block_at + len(block))
    assert comment_at < block_at < next_comment_at
    assert source.count(block) == 1

    if spec["kind"] == "godhead_partition":
        assert "gpush_generated(c, 0x8U);" in block
        assert G.VITA_VANILLA_KAGE_GODHEAD_STATEMENT in block
    elif spec["kind"] == "compatible_stride":
        required = (
            "const uint32_t _image = (uint32_t)(c->esi - 0x50U);",
            "(uint32_t)(_image + 0x3aU)",
            "ld32((uint32_t)(_batch + 0x28U))",
            "st32(_back_slot, _batch);",
            "goto L_00560567;",
            G.VITA_VANILLA_KAGE_STRIDE_STATEMENT,
        )
        assert all(item in block for item in required)
    else:
        assert "if (c->eax != c->edi)" in block
        assert block.count(G.VITA_VANILLA_KAGE_INVALIDATE_STATEMENT) == 2


def _verify_generator(pe_path: Path) -> None:
    pin = Image(str(pe_path), DEFAULT_BASE)
    switch_info_by_root = {
        root: _tear_switch_info(pin, root)
        for root in G.VITA_VANILLA_KAGE_GODHEAD_ROOTS
    }
    expected_memberships = {
        spec["site"]: set(spec["roots"])
        for spec in G.VITA_VANILLA_KAGE_BUNDLE_SPECS
    }

    blocks_by_base = {}
    for base in (DEFAULT_BASE, VITA_BASE):
        emit = Image(str(pe_path), base)
        actual_memberships = {
            site: set() for site in G.VITA_VANILLA_KAGE_BUNDLE_SITES
        }
        rendered = {}
        for root in sorted(G.VITA_VANILLA_KAGE_BUNDLE_ROOTS):
            switch_info = switch_info_by_root.get(root, {})
            result = G._translate_function(
                emit, {"rva": root}, None, switch_info, pin_img=pin
            )
            assert result["stub"] is None
            assert legacy_text(result["text"]).count(FENCE) == 1
            for site in result["vita_vanilla_kage_bundle_sites"]:
                actual_memberships[site].add(root)
                spec = G.VITA_VANILLA_KAGE_BUNDLE_SPECS_BY_SITE[site]
                _verify_rendered_site(legacy_text(result["text"]), spec)
                rendered[(root, site)] = "\n".join(
                    G.render_vita_vanilla_kage_bundle(
                        spec,
                        ({
                            "godhead_partition":
                                G.VITA_VANILLA_KAGE_GODHEAD_STATEMENT,
                            "compatible_stride":
                                G.VITA_VANILLA_KAGE_STRIDE_STATEMENT,
                            "same_image_invalidation":
                                G.VITA_VANILLA_KAGE_INVALIDATE_STATEMENT,
                        }[spec["kind"]],),
                    )
                )
        assert actual_memberships == expected_memberships
        blocks_by_base[base] = rendered
    assert blocks_by_base[DEFAULT_BASE] == blocks_by_base[VITA_BASE]

    # Each exact machine seam must reject one-byte drift even when a caller
    # accidentally reuses the old decoded instruction and emission objects.
    representative_roots = {
        G.VITA_VANILLA_KAGE_GODHEAD_SITE_RVA: 0x00242990,
        G.VITA_VANILLA_KAGE_STRIDE_SITE_RVA:
            G.VITA_VANILLA_KAGE_STRIDE_ROOT_RVA,
        G.VITA_VANILLA_KAGE_INVALIDATE_SITE_RVA:
            G.VITA_VANILLA_KAGE_INVALIDATE_ROOT_RVA,
    }
    pin.mem = bytearray(pin.mem)
    for site, root in representative_roots.items():
        switch_info = switch_info_by_root.get(root, {})
        em, insns, order, body = _raw_translation(pin, root, switch_info)
        original = pin.mem[site]
        pin.mem[site] ^= 0x01
        try:
            G.vita_vanilla_kage_bundle_for_body(
                pin, root, em, insns, order, body
            )
        except RuntimeError as exc:
            assert any(word in str(exc) for word in (
                "vanilla/KAGE", "Godhead", "KAGE", "Entity_Tear"
            ))
        else:
            raise AssertionError(
                "mutated vanilla/KAGE seam passed at 0x%08x" % site
            )
        finally:
            pin.mem[site] = original


def _verify_host_oracles() -> None:
    oracle = G.vita_vanilla_kage_stride_host_oracle
    assert oracle(24, ()) == (False, ())
    assert oracle(24, (16, 24)) == (True, (16, 24))
    assert oracle(24, (16, 24, 32)) == (True, (16, 32, 24))
    assert oracle(24, (24, 16, 24, 32)) == (True, (24, 16, 32, 24))
    assert oracle(24, (16, 32, 48)) == (False, (16, 32, 48))
    assert oracle(0x10018, (16, 24, 32)) == (True, (16, 32, 24))
    assert oracle(24, (0x10018, 32)) == (False, (0x10018, 32))

    invalidate = G.vita_vanilla_kage_invalidation_host_oracle
    assert invalidate(0, 0x1000) is False
    assert invalidate(0x1000, 0x1000) is False
    assert invalidate(0x1000, 0x2000) is True
    assert G.VITA_VANILLA_KAGE_GODHEAD_PARTITION == 8


def _verify_build_switch() -> None:
    cmake = (HERE / "vita/CMakeLists.txt").read_text(encoding="utf-8")
    option = re.search(
        r"option\(ISAAC_VITA_VANILLA_KAGE_BUNDLE\s+.*?\s+(ON|OFF)\)",
        cmake,
        flags=re.S,
    )
    assert option is not None and option.group(1) == "OFF"
    raster = re.search(
        r"option\(ISAAC_VITA_DISPLAY_RASTER_720\s+.*?\s+(ON|OFF)\)",
        cmake,
        flags=re.S,
    )
    assert raster is not None and raster.group(1) == "OFF"
    assert "NOT ISAAC_VITA_KAGE" in cmake[option.end():]
    assert cmake.count("ISAAC_VITA_VANILLA_KAGE_BUNDLE=1") == 1
    for root in G.VITA_VANILLA_KAGE_BUNDLE_ROOTS:
        assert cmake.count("sub_%08x" % root) == 1


def run(pe_path: Path) -> None:
    pe_path = pe_path.resolve()
    assert pe_path.stat().st_size == G.VITA_VANILLA_KAGE_BUNDLE_PE_SIZE
    assert _sha256(pe_path) == G.VITA_VANILLA_KAGE_BUNDLE_PE_SHA256
    _verify_generator(pe_path)
    _verify_host_oracles()
    _verify_build_switch()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", type=Path, required=True)
    args = parser.parse_args()
    run(args.pe)
    print(
        "vanilla/KAGE bundle host/codegen oracle: PASS; "
        "sites=3 roots=6 bases=2 mutations=3 default-OFF=PASS"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
