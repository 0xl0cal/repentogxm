#!/usr/bin/env python3
"""Host-only poison tests for the path-independent linker-map contract."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import tempfile

import linker_map_contract as contract


RECIPE = "1f" * 32


def _prefix(path: Path) -> bytes:
    return os.fsencode(path.resolve().as_posix())


def _map(
    source: Path,
    generated: Path,
    vitasdk: Path,
    *,
    recipe: str = RECIPE,
    end: bytes = b"0x0000000082ad1560",
) -> bytes:
    recipe_bytes = recipe.encode("ascii")
    return b"".join((
        b"Archive member included to satisfy reference by file (symbol)\r\n",
        _prefix(source), b"/recomp/runtime/guest.c.obj (guest_image_load)\r\n",
        b" .text.host CMakeFiles/isaac_first_arm_fault.dir", _prefix(source),
        b"/recomp/runtime/host_vita_heap.c.obj\r\n",
        b" .text.guest CMakeFiles/isaac_first_arm_fault.dir",
        _prefix(generated), b"/guest_0001.c.obj\r\n",
        b"LOAD ", _prefix(vitasdk), b"/lib/libc.a\r\n",
        b" .text.guest_generation_id\r\n",
        b"                0x0000000081000010 guest_generation_id\r\n",
        b" .rodata.guest_generation_", recipe_bytes, b"\r\n",
        b"                0x0000000081000000 guest_generation_",
        recipe_bytes, b"\r\n",
        b"                ", end, b"                _end = .\r\n",
    ))


def _canonical(
    payload: bytes, source: Path, generated: Path, vitasdk: Path,
    recipe: str = RECIPE,
) -> contract.CanonicalLinkerMap:
    return contract.canonicalize(
        payload,
        source_root=source,
        generated_root=generated,
        vitasdk_root=vitasdk,
        generation_recipe_id=recipe,
    )


def _reject(label: str, expected: str, action: object) -> None:
    try:
        assert callable(action)
        action()
    except contract.LinkerMapContractError as exc:
        if expected not in str(exc):
            raise AssertionError(
                f"{label}: wrong failure: {exc!s}"
            ) from exc
    else:
        raise AssertionError(f"{label}: hostile map was accepted")


def main() -> int:
    if not __debug__:
        raise RuntimeError("linker-map poison tests require assertions")
    with tempfile.TemporaryDirectory(
        prefix="isaac-map-contract [hostile]+$ "
    ) as temporary:
        base = Path(temporary)
        source_a = base / "A source [x]+$"
        generated_a = source_a / "nested generated (one)"
        vitasdk_a = base / "A SDK {softfp}"
        source_b = base / "much-longer B source tree"
        generated_b = base / "B generated independent"
        vitasdk_b = source_b / "nested SDK with spaces"
        for path in (
            generated_a, vitasdk_a, source_b, generated_b, vitasdk_b,
        ):
            path.mkdir(parents=True, exist_ok=True)

        raw_a = _map(source_a, generated_a, vitasdk_a)
        raw_b = _map(source_b, generated_b, vitasdk_b)
        assert raw_a != raw_b
        canonical_a = _canonical(raw_a, source_a, generated_a, vitasdk_a)
        canonical_b = _canonical(raw_b, source_b, generated_b, vitasdk_b)
        assert canonical_a.payload == canonical_b.payload
        assert canonical_a.record == canonical_b.record
        assert canonical_a.record["replacements"] == {
            "source_root": 2,
            "generated_root": 1,
            "vitasdk_root": 1,
            "guest_generation": 2,
        }
        assert canonical_a.payload.count(b"\r\n") == raw_a.count(b"\r\n")
        assert canonical_a.payload.count(b"guest_generation_id") == 2
        assert canonical_a.record["end"] == "0x82ad1560"
        assert canonical_a.record["sha256"] == hashlib.sha256(
            canonical_a.payload
        ).hexdigest()
        new_recipe = "2d" * 32
        canonical_new_recipe = _canonical(
            _map(source_a, generated_a, vitasdk_a, recipe=new_recipe),
            source_a, generated_a, vitasdk_a, new_recipe,
        )
        assert canonical_new_recipe.payload == canonical_a.payload

        semantic_mutations = {
            "object suffix": raw_a.replace(b"guest.c.obj", b"guest2.c.obj", 1),
            "section": raw_a.replace(b".text.host", b".text.host2", 1),
            "address": raw_a.replace(b"0x0000000081000000", b"0x0000000081000004", 1),
            "symbol": raw_a.replace(b"guest_image_load", b"guest_image_load2", 1),
            "SDK suffix": raw_a.replace(b"/lib/libc.a", b"/lib/libc2.a", 1),
            "end": _map(
                source_a, generated_a, vitasdk_a,
                end=b"0x0000000082ad1570",
            ),
        }
        for label, mutated in semantic_mutations.items():
            result = _canonical(mutated, source_a, generated_a, vitasdk_a)
            assert result.record["sha256"] != canonical_a.record["sha256"], label

        foreign = "2e" * 32
        _reject(
            "foreign generation", "foreign guest_generation",
            lambda: _canonical(
                _map(source_a, generated_a, vitasdk_a, recipe=foreign),
                source_a, generated_a, vitasdk_a,
            ),
        )
        malformed_generation = raw_a.replace(
            RECIPE.encode("ascii"), b"g" + RECIPE.encode("ascii")[1:], 1
        )
        _reject(
            "malformed generation", "malformed guest_generation",
            lambda: _canonical(
                malformed_generation, source_a, generated_a, vitasdk_a
            ),
        )
        _reject(
            "bad expected generation", "recipe_id is not lowercase SHA-256",
            lambda: _canonical(
                raw_a, source_a, generated_a, vitasdk_a, RECIPE.upper()
            ),
        )
        one_generation = raw_a.replace(
            b"guest_generation_" + RECIPE.encode("ascii"),
            b"guest_generation_id",
            1,
        )
        _reject(
            "missing generation occurrence", "occurrence count changed",
            lambda: _canonical(
                one_generation, source_a, generated_a, vitasdk_a
            ),
        )
        three_generations = (
            raw_a + b"guest_generation_" + RECIPE.encode("ascii") + b"\r\n"
        )
        _reject(
            "extra generation occurrence", "occurrence count changed",
            lambda: _canonical(
                three_generations, source_a, generated_a, vitasdk_a
            ),
        )

        for label, root_bytes in (
            ("source_root", _prefix(source_a)),
            ("generated_root", _prefix(generated_a)),
            ("vitasdk_root", _prefix(vitasdk_a)),
        ):
            if label == "source_root":
                omitted = raw_a.replace(
                    root_bytes + b"/recomp/", b"/omitted/root/recomp/"
                )
            else:
                omitted = raw_a.replace(
                    root_bytes + b"/", b"/omitted/root/"
                )
            _reject(
                f"missing {label}", f"missing required {label} prefix",
                lambda value=omitted: _canonical(
                    value, source_a, generated_a, vitasdk_a
                ),
            )

        near_prefix = raw_a + _prefix(source_a) + b"2/foreign.o\r\n"
        _reject(
            "near prefix", "retains unsafe source_root root occurrence",
            lambda: _canonical(
                near_prefix, source_a, generated_a, vitasdk_a
            ),
        )
        nonpath_root = (
            raw_a + b"semantic_" + _prefix(source_a) + b"/hidden\r\n"
        )
        _reject(
            "nonpath root", "outside a filename boundary",
            lambda: _canonical(
                nonpath_root, source_a, generated_a, vitasdk_a
            ),
        )
        embedded_generation = (
            raw_a + b"myguest_generation_" + RECIPE.encode("ascii") + b"\r\n"
        )
        _reject(
            "embedded generation", "inside another identifier",
            lambda: _canonical(
                embedded_generation, source_a, generated_a, vitasdk_a
            ),
        )
        token_collision = raw_a + b"@ISAAC_SOURCE_ROOT@/foreign.o\r\n"
        _reject(
            "token collision", "already contains canonical token",
            lambda: _canonical(
                token_collision, source_a, generated_a, vitasdk_a
            ),
        )
        _reject(
            "duplicate roots", "roots are duplicated",
            lambda: _canonical(raw_a, source_a, source_a, vitasdk_a),
        )

        _reject(
            "missing end", "missing '_end = .'",
            lambda: _canonical(
                raw_a.replace(b"_end = .", b"end omitted"),
                source_a, generated_a, vitasdk_a,
            ),
        )
        _reject(
            "duplicate end", "duplicate '_end = .'",
            lambda: _canonical(
                raw_a + b"0x82ad1560 _end = .\n",
                source_a, generated_a, vitasdk_a,
            ),
        )
        _reject(
            "malformed end", "malformed '_end = .'",
            lambda: _canonical(
                raw_a.replace(
                    b"0x0000000082ad1560                _end = .",
                    b"not-hex _end = .",
                ),
                source_a, generated_a, vitasdk_a,
            ),
        )
        _reject(
            "out of range end", "outside uint32",
            lambda: _canonical(
                raw_a.replace(
                    b"0x0000000082ad1560", b"0x0000000100000000"
                ),
                source_a, generated_a, vitasdk_a,
            ),
        )

    print("linker-map contract poison tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
