#!/usr/bin/env python3
"""Frozen-PE/codegen and hostile-host oracle for RenderFrame borrowing."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
sys.path.insert(0, str(HERE))

import build_all as BA  # noqa: E402
import build_one as B  # noqa: E402
import gen_all as G  # noqa: E402
from gpr_locals import legacy_text  # noqa: E402
from image import DEFAULT_BASE, Image  # noqa: E402


VITA_BASE = 0x98000000
FENCE = (
    "#if defined(__vita__) && defined(ISAAC_VITA_RENDERFRAME_FASTPATH)"
)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _raw_owner(img: Image):
    translated = B.translate(
        img,
        G.VITA_RENDERFRAME_FASTPATH_ROOT_RVA,
        "sub_00003e50",
        None,
    )
    em, insns, order, body, unsupported, indirect, _members = translated
    assert unsupported == []
    assert indirect == []
    return em, insns, order, body


def _verify_generator(pe_path: Path) -> None:
    pin = Image(str(pe_path), DEFAULT_BASE)
    em, insns, order, body = _raw_owner(pin)
    specs = G.vita_renderframe_fastpath_for_body(
        pin,
        G.VITA_RENDERFRAME_FASTPATH_ROOT_RVA,
        em,
        insns,
        order,
        body,
    )
    assert tuple(specs) == G.VITA_RENDERFRAME_FASTPATH_SPECS

    sources = {}
    for base in (DEFAULT_BASE, VITA_BASE):
        emit = Image(str(pe_path), base)
        result = G._translate_function(
            emit,
            {"rva": G.VITA_RENDERFRAME_FASTPATH_ROOT_RVA},
            None,
            {},
            pin_img=pin,
        )
        assert result["stub"] is None
        assert result["insns"] == G.VITA_RENDERFRAME_FASTPATH_INSNS
        assert result["vita_renderframe_fastpath_sites"] == tuple(
            spec["site"] for spec in specs
        )
        source = legacy_text(result["text"])
        assert source.count("isaac_vita_renderframe_try_borrow(") == 5
        assert source.count("guest_stack_adjust_generated(c, 4U)") == 4
        assert source.count(FENCE) == 9
        assert source.count("sub_00003540(c);") == 9
        for site in G.VITA_RENDERFRAME_FASTPATH_RETAINED_RENDER_SITES:
            comment = "    /* %08x  call 0x3540 */" % site
            comment_at = source.index(comment)
            next_comment_at = source.index("    /* ", comment_at + len(comment))
            retained = source[comment_at:next_comment_at]
            assert FENCE not in retained
            assert retained.count("sub_00003540(c);") == 1
        for spec in specs:
            original = G._vita_renderframe_original_call_statement(spec)
            block = "\n".join(G.render_vita_renderframe_fastpath(
                spec, (original,), base
            ))
            comment = "    /* %08x  call 0x3540 */" % spec["site"]
            comment_at = source.index(comment)
            block_at = source.index(block, comment_at)
            next_at = source.index(
                "    /* %08x" % spec["next"], block_at
            )
            assert comment_at < block_at < next_at
            assert source.count(original) == 1
        sources[base] = source

    assert (
        "0x30608360U, 0x307fd62cU, 0x3056d8c0U" in
        sources[DEFAULT_BASE]
    )
    assert (
        "0x98608360U, 0x987fd62cU, 0x9856d8c0U" in
        sources[VITA_BASE]
    )

    # A generated regression test must regenerate both affected owners.  This
    # also proves the existing missing-layer guard remains independently owned.
    anm2 = G._translate_function(
        pin,
        {"rva": G.VITA_ANM2_MISSING_LAYER_GUARD_ROOT_RVA},
        None,
        {},
        pin_img=pin,
    )
    assert anm2["stub"] is None
    assert anm2["vita_anm2_missing_layer_guard_sites"] == tuple(
        spec["site"] for spec in G.VITA_ANM2_MISSING_LAYER_GUARD_SPECS
    )
    assert legacy_text(anm2["text"]).count(
        "#if defined(ISAAC_VITA_ANM2_MISSING_LAYER_GUARD)"
    ) == 3

    # Hostile stale-decoder mutations must all fail closed: owner/consumer,
    # getter/destructor/final-consumer bodies, observer callback/init/slot,
    # RTTI vftable, and a caller outside the owner.
    pin.mem = bytearray(pin.mem)
    mutations = (
        G.VITA_RENDERFRAME_FASTPATH_SPECS[0]["site"],
        0x00004367,
        G.VITA_RENDERFRAME_FASTPATH_GETTER_RVA,
        G.VITA_RENDERFRAME_FASTPATH_SMART_DTOR_RVA,
        G.VITA_RENDERFRAME_FASTPATH_FINAL_CONSUMER_RVA,
        G.VITA_RENDERFRAME_FASTPATH_RELEASE_CALLBACK_RVA,
        G.VITA_RENDERFRAME_FASTPATH_RELEASE_INIT_RVA,
        G.VITA_RENDERFRAME_FASTPATH_RELEASE_OBSERVER_RVA,
        G.VITA_RENDERFRAME_FASTPATH_COUNTER_VFTABLE_RVA,
        0x000051C6,
    )
    for rva in mutations:
        original = pin.mem[rva]
        pin.mem[rva] ^= 0x01
        try:
            G.vita_renderframe_fastpath_for_body(
                pin,
                G.VITA_RENDERFRAME_FASTPATH_ROOT_RVA,
                em,
                insns,
                order,
                body,
            )
        except RuntimeError as exc:
            assert "RenderFrame fast-path" in str(exc)
        else:
            raise AssertionError(
                "mutated RenderFrame contract passed at 0x%08x" % rva
            )
        finally:
            pin.mem[rva] = original


def _run(command: list[str], env: dict[str, str]) -> str:
    completed = subprocess.run(
        command,
        cwd=HERE,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    output = completed.stdout.decode("cp866", errors="replace")
    print(output, end="")
    if completed.returncode != 0:
        raise subprocess.CalledProcessError(completed.returncode, command)
    return output


def _verify_host_policy() -> None:
    env, compiler = BA.msvc_env()
    with tempfile.TemporaryDirectory(
            prefix="isaac-renderframe-fastpath-") as temporary:
        root = Path(temporary)
        oracle = root / "renderframe-fastpath-oracle.exe"
        _run([
            compiler, "/nologo", "/W4", "/WX", "/O2", "/std:c11",
            "/I", str(RUNTIME),
            str(RUNTIME / "host_vita_renderframe_fastpath_oracle.c"),
            "/Fe:" + str(oracle),
            "/Fo:" + str(root / "renderframe-fastpath-oracle.obj"),
            "/link", "/OPT:REF", "/INCREMENTAL:NO",
        ], env)
        output = _run([str(oracle)], env)
        assert "RenderFrame fast-path hostile snapshot oracle: PASS" in output

        # The production collector shares the policy header and must remain a
        # warning-clean C translation unit on the hostile 64-bit host.
        _run([
            compiler, "/nologo", "/c", "/W4", "/WX", "/O2", "/std:c11",
            "/DISAAC_VITA_RENDERFRAME_FASTPATH_ORACLE=1",
            "/wd4310", "/wd4311", "/wd4312", "/wd4996",
            "/I", str(RUNTIME),
            str(RUNTIME / "host_vita_renderframe_fastpath.c"),
            "/Fo:" + str(root / "renderframe-fastpath-runtime.obj"),
        ], env)


def _verify_build_switch() -> None:
    cmake = (HERE / "vita/CMakeLists.txt").read_text(encoding="utf-8")
    build_tool = (HERE.parent / "tools/build_vita.py").read_text(
        encoding="utf-8"
    )
    option = re.search(
        r"option\(ISAAC_VITA_RENDERFRAME_FASTPATH\s+.*?\s+(ON|OFF)\)",
        cmake,
        flags=re.S,
    )
    assert option is not None and option.group(1) == "OFF"
    assert "ISAAC_VITA_RENDERFRAME_FASTPATH requires the translated" in cmake
    assert cmake.count("ISAAC_VITA_RENDERFRAME_FASTPATH=1") == 1
    assert cmake.count("host_vita_renderframe_fastpath.c") == 1
    assert cmake.count("sub_00003e50") == 1
    owner_block = re.search(
        r"if\(ISAAC_VITA_RENDERFRAME_FASTPATH\)(.*?)endif\(\)\s+"
        r"if\(ISAAC_VITA_ANM2_SCRATCH\)",
        cmake,
        flags=re.S,
    )
    assert owner_block is not None
    assert "ISAAC_GENERATED_CANONICAL_C" in owner_block.group(1)
    assert "ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE" in owner_block.group(1)
    assert build_tool.count(
        '"-DISAAC_VITA_RENDERFRAME_FASTPATH=OFF"'
    ) == 2
    assert '"ISAAC_VITA_RENDERFRAME_FASTPATH": False' in build_tool


def run(pe_path: Path) -> None:
    pe_path = pe_path.resolve()
    assert pe_path.stat().st_size == G.VITA_RENDERFRAME_FASTPATH_PE_SIZE
    assert _sha256(pe_path) == G.VITA_RENDERFRAME_FASTPATH_PE_SHA256
    _verify_generator(pe_path)
    _verify_host_policy()
    _verify_build_switch()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", type=Path, required=True)
    args = parser.parse_args()
    run(args.pe)
    print("RenderFrame fast-path frozen/codegen/build oracle: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
