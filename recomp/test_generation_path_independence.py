#!/usr/bin/env python3
"""Two-root regression for portable discovery and generation identities."""

from __future__ import annotations

import json
import os
import sys
import tempfile
from pathlib import Path


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import bounds as Bounds  # noqa: E402
import build_contract as BC  # noqa: E402
import funcs as Funcs  # noqa: E402
import gen_all as Gen  # noqa: E402


PE_BYTES = b"MZ\x00portable synthetic recipe input\r\n"
GENERATOR_SOURCE_NAMES = (
    "gen_all.py",
    "build_one.py",
    "emit.py",
    "trans.py",
    "image.py",
    "iat_meta.py",
    "sweep2.py",
    "build_contract.py",
)
RUNTIME_INPUTS = {
    "runtime/manual_kage.h": "MANUAL_KAGE_HEADER",
    "runtime/manual_portable.h": "MANUAL_PORTABLE_HEADER",
    "runtime/manual_kage.c": "MANUAL_KAGE_SOURCE",
    "runtime/manual_portable.c": "MANUAL_PORTABLE_SOURCE",
    "runtime/kage_vita_fx_rollback.h": "VITA_FXLAYERS_ROLLBACK_HEADER",
}
SECTIONS = (
    {
        "name": ".text",
        "rva": 0x1000,
        "vsize": 0x40,
        "exec": True,
        "data": b"physical PE bytes must not enter the document",
    },
    {
        "name": ".rdata",
        "rva": 0x2000,
        "vsize": 0x20,
        "exec": False,
        "data": b"nor may section backing bytes",
    },
)
FUNCTIONS = (
    {"rva": 0x1000, "end": 0x1010, "size": 0x10},
    {"rva": 0x1020, "end": 0x1040, "size": 0x20},
)
BOUNDS_DOCUMENT = {
    "called": [0x1000],
    "jumped": [0x1020],
    "ptrs": [0x1000],
    "data_like": [],
    "evidence": {"CALLED": 1, "VTABLE": 0, "JUMPED": 1, "ORPHAN": 0},
}


def canonical_bytes(value):
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
        allow_nan=False,
    ).encode("utf-8")


def fixture_sources():
    sources = {
        "source/recomp/funcs.py": Path(Funcs.__file__).read_bytes(),
        "source/recomp/msvc_rtti.py": Path(Funcs.RTTI.__file__).read_bytes(),
        "source/recomp/bounds.py": Path(Bounds.__file__).read_bytes(),
    }
    for name in GENERATOR_SOURCE_NAMES:
        sources["source/recomp/" + name] = (HERE / name).read_bytes()
    for logical, attribute in RUNTIME_INPUTS.items():
        sources[logical] = Path(getattr(Gen, attribute)).read_bytes()
    return sources


def create_fixture(root, sources):
    paths = {}
    for logical, payload in sources.items():
        path = root.joinpath(*logical.split("/"))
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        paths[logical] = path
    pe = root.joinpath(*Funcs.PE_LOGICAL_NAME.split("/"))
    pe.parent.mkdir(parents=True, exist_ok=True)
    pe.write_bytes(PE_BYTES)
    paths[Funcs.PE_LOGICAL_NAME] = pe
    (root / "analysis output").mkdir()
    return paths


def bind_fixture(paths):
    pe = str(paths[Funcs.PE_LOGICAL_NAME])
    Funcs.EXE = pe
    Funcs.__file__ = str(paths["source/recomp/funcs.py"])
    Funcs.RTTI.__file__ = str(paths["source/recomp/msvc_rtti.py"])
    BC.__file__ = str(paths["source/recomp/build_contract.py"])

    Bounds.EXE = pe
    Bounds.__file__ = str(paths["source/recomp/bounds.py"])

    Gen.EXE = pe
    Gen.__file__ = str(paths["source/recomp/gen_all.py"])
    for logical, attribute in RUNTIME_INPUTS.items():
        setattr(Gen, attribute, str(paths[logical]))


def run_fixture(root, paths):
    bind_fixture(paths)
    analysis = root / "analysis output"

    functions_recipe_id, functions_recipe = Funcs.contract_recipe()
    assert functions_recipe["inputs"][Funcs.PE_LOGICAL_NAME] == BC.sha256_file(
        paths[Funcs.PE_LOGICAL_NAME]
    )
    document = Funcs.functions_document(
        0x400000,
        0x3000,
        0x1000,
        0x1000,
        0x1040,
        SECTIONS,
        FUNCTIONS,
    )
    assert document["exe"] == Funcs.PE_LOGICAL_NAME
    assert not os.path.isabs(document["exe"])
    functions_bytes = json.dumps(document).encode("utf-8")
    assert str(root).encode("utf-8") not in functions_bytes
    assert str(paths[Funcs.PE_LOGICAL_NAME]).encode("utf-8") not in functions_bytes
    functions_path = analysis / "functions.json"
    functions_path.write_bytes(functions_bytes)
    functions_outputs = BC.snapshot(analysis, ["functions.json"])
    functions_manifest = BC.make_manifest(
        "functions",
        functions_recipe_id,
        functions_recipe,
        functions_outputs,
    )

    bounds_path = analysis / "bounds.json"
    bounds_path.write_text(json.dumps(BOUNDS_DOCUMENT), encoding="utf-8")
    bounds_recipe_id, bounds_recipe = Bounds.contract_recipe(
        functions_manifest["output_set_id"]
    )
    bounds_manifest = BC.make_manifest(
        "bounds",
        bounds_recipe_id,
        bounds_recipe,
        BC.snapshot(analysis, ["bounds.json"]),
    )
    generation_recipe_id, generation_recipe = Gen.contract_recipe(
        functions_manifest,
        bounds_manifest,
    )
    for recipe in (functions_recipe, bounds_recipe, generation_recipe):
        encoded = canonical_bytes(recipe)
        assert str(root).encode("utf-8") not in encoded
        assert str(paths[Funcs.PE_LOGICAL_NAME]).encode("utf-8") not in encoded

    return {
        "functions_bytes": functions_bytes,
        "functions_output_set": functions_manifest["output_set_id"],
        "functions_recipe_id": functions_recipe_id,
        "bounds_recipe_id": bounds_recipe_id,
        "generation_recipe_id": generation_recipe_id,
    }


def main():
    original = {
        "Funcs.EXE": Funcs.EXE,
        "Funcs.__file__": Funcs.__file__,
        "Funcs.RTTI.__file__": Funcs.RTTI.__file__,
        "BC.__file__": BC.__file__,
        "Bounds.EXE": Bounds.EXE,
        "Bounds.__file__": Bounds.__file__,
        "Gen.EXE": Gen.EXE,
        "Gen.__file__": Gen.__file__,
        **{
            "Gen." + attribute: getattr(Gen, attribute)
            for attribute in RUNTIME_INPUTS.values()
        },
    }
    try:
        sources = fixture_sources()
        with tempfile.TemporaryDirectory(
            prefix="isaac-generation-path-contract-"
        ) as temporary:
            parent = Path(temporary)
            first_root = parent / "short root [a].+$"
            second_root = parent / "a much longer root (b){2}^$ + spaces"
            assert len(str(first_root)) != len(str(second_root))
            assert any(character in str(first_root) for character in "[].+$")
            assert any(character in str(second_root) for character in "(){}^$")
            first_paths = create_fixture(first_root, sources)
            second_paths = create_fixture(second_root, sources)

            first = run_fixture(first_root, first_paths)
            second = run_fixture(second_root, second_paths)
            assert first == second

            pe = second_paths[Funcs.PE_LOGICAL_NAME]
            pe.write_bytes(PE_BYTES + b"mutated")
            pe_mutated = run_fixture(second_root, second_paths)
            assert pe_mutated["functions_bytes"] == first["functions_bytes"]
            assert pe_mutated["functions_output_set"] == first[
                "functions_output_set"
            ]
            for key in (
                "functions_recipe_id",
                "bounds_recipe_id",
                "generation_recipe_id",
            ):
                assert pe_mutated[key] != first[key]
            pe.write_bytes(PE_BYTES)

            funcs_source = second_paths["source/recomp/funcs.py"]
            funcs_payload = funcs_source.read_bytes()
            funcs_source.write_bytes(funcs_payload + b"\n# mutation fixture\n")
            funcs_mutated = run_fixture(second_root, second_paths)
            assert funcs_mutated["functions_recipe_id"] != first[
                "functions_recipe_id"
            ]
            assert funcs_mutated["functions_bytes"] == first["functions_bytes"]
            assert funcs_mutated["functions_output_set"] == first[
                "functions_output_set"
            ]
            assert funcs_mutated["bounds_recipe_id"] == first["bounds_recipe_id"]
            assert funcs_mutated["generation_recipe_id"] == first[
                "generation_recipe_id"
            ]
            funcs_source.write_bytes(funcs_payload)

            generator_source = second_paths["source/recomp/gen_all.py"]
            generator_payload = generator_source.read_bytes()
            generator_source.write_bytes(
                generator_payload + b"\n# mutation fixture\n"
            )
            generator_mutated = run_fixture(second_root, second_paths)
            assert generator_mutated["functions_recipe_id"] == first[
                "functions_recipe_id"
            ]
            assert generator_mutated["bounds_recipe_id"] == first[
                "bounds_recipe_id"
            ]
            assert generator_mutated["generation_recipe_id"] != first[
                "generation_recipe_id"
            ]

        print(
            "generation path-independence: two roots/portable output/"
            "PE+source mutation contracts PASS"
        )
        return 0
    finally:
        Funcs.EXE = original["Funcs.EXE"]
        Funcs.__file__ = original["Funcs.__file__"]
        Funcs.RTTI.__file__ = original["Funcs.RTTI.__file__"]
        BC.__file__ = original["BC.__file__"]
        Bounds.EXE = original["Bounds.EXE"]
        Bounds.__file__ = original["Bounds.__file__"]
        Gen.EXE = original["Gen.EXE"]
        Gen.__file__ = original["Gen.__file__"]
        for attribute in RUNTIME_INPUTS.values():
            setattr(Gen, attribute, original["Gen." + attribute])


if __name__ == "__main__":
    raise SystemExit(main())
