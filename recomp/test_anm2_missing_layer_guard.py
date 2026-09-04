#!/usr/bin/env python3
"""Focused host/codegen oracle for the frozen ANM2 missing-layer guard."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import build_one as B  # noqa: E402
import gen_all as G  # noqa: E402
from gpr_locals import legacy_text  # noqa: E402
from image import DEFAULT_BASE, Image  # noqa: E402


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _translated_owner(img: Image):
    known = {
        G.VITA_ANM2_MISSING_LAYER_GUARD_ROOT_RVA,
        0x00003E50,
    }
    translated = B.translate(
        img,
        G.VITA_ANM2_MISSING_LAYER_GUARD_ROOT_RVA,
        "sub_00005250",
        known,
    )
    em, insns, order, body, unsupported, indirect, _members = translated
    assert unsupported == []
    assert indirect == []
    return known, em, insns, order, body


def run(pe_path: Path) -> None:
    pe_path = pe_path.resolve()
    assert pe_path.stat().st_size == G.VITA_ANM2_MISSING_LAYER_GUARD_PE_SIZE
    assert _sha256(pe_path) == G.VITA_ANM2_MISSING_LAYER_GUARD_PE_SHA256

    img = Image(str(pe_path), DEFAULT_BASE)
    known, em, insns, order, body = _translated_owner(img)
    specs = G.vita_anm2_missing_layer_guard_for_body(
        img,
        G.VITA_ANM2_MISSING_LAYER_GUARD_ROOT_RVA,
        em,
        insns,
        order,
        body,
    )
    assert tuple(specs) == G.VITA_ANM2_MISSING_LAYER_GUARD_SPECS

    result = G._translate_function(
        img,
        {"rva": G.VITA_ANM2_MISSING_LAYER_GUARD_ROOT_RVA},
        known,
        {},
    )
    assert result["stub"] is None
    assert result["vita_anm2_missing_layer_guard_sites"] == tuple(
        spec["site"] for spec in specs
    )
    source = legacy_text(result["text"])
    fence = "#if defined(ISAAC_VITA_ANM2_MISSING_LAYER_GUARD)"
    assert source.count(fence) == 3
    assert source.count("if ((int32_t)c->ecx < 0) {") == 3
    assert source.count("guest_stack_adjust_generated(c, 4U)") == 1

    initial_esp = 0x83303C34
    for spec in specs:
        site = spec["site"]
        emitted_guard = "\n".join(
            G.render_vita_anm2_missing_layer_guard(spec)
        )
        load_comment = (
            "    /* %08x  mov ecx, dword ptr "
            "[edx + edi*4 + 0x38] */" % site
        )
        load_at = source.index(load_comment)
        load_statement_at = source.index(
            "    " + G.VITA_ANM2_MISSING_LAYER_GUARD_LOAD_STATEMENT,
            load_at,
        )
        guard_at = source.index(emitted_guard, load_statement_at)
        next_comment_at = source.index(
            "    /* %08x" % spec["next"], guard_at
        )
        assert load_at < load_statement_at < guard_at < next_comment_at

        # Enabled guard: signed-negative map sentinels skip this render path.
        # The shared third path alone unwinds its already-pushed EAX word.
        for missing in (-1, 0x80000000, 0xFFFFFFFF):
            target, final_esp = G.vita_anm2_missing_layer_guard_host_oracle(
                spec, missing, initial_esp
            )
            assert target == spec["skip"]
            assert final_esp == (
                initial_esp + spec["pending_stack_bytes"]
            ) & 0xFFFFFFFF

        # Every valid signed layer order falls through without touching ESP;
        # the original array loads/call immediately following the fence remain
        # the generated implementation.
        for valid in (0, 1, 17, 0x7FFFFFFF):
            target, final_esp = G.vita_anm2_missing_layer_guard_host_oracle(
                spec, valid, initial_esp
            )
            assert target is None
            assert final_esp == initial_esp

    assert tuple(spec["pending_stack_bytes"] for spec in specs) == (0, 0, 4)

    # One-byte code drift must fail at the owner/signature proof even if the
    # caller accidentally supplies the old decoded instruction objects.
    mutation_rva = specs[0]["site"]
    img.mem = bytearray(img.mem)
    original = img.mem[mutation_rva]
    img.mem[mutation_rva] ^= 0x01
    try:
        G.vita_anm2_missing_layer_guard_for_body(
            img,
            G.VITA_ANM2_MISSING_LAYER_GUARD_ROOT_RVA,
            em,
            insns,
            order,
            body,
        )
    except RuntimeError as exc:
        assert "ANM2 missing-layer" in str(exc)
    else:
        raise AssertionError("mutated ANM2 owner passed its fail-closed proof")
    finally:
        img.mem[mutation_rva] = original


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", type=Path, required=True)
    args = parser.parse_args()
    run(args.pe)
    print("ANM2 missing-layer host/codegen oracle: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
