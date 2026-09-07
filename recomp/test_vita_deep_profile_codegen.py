#!/usr/bin/env python3
"""Fresh transition-scope emission and original call-chain checks.

This exercises the real emitter, not a checked-in generated corpus. Runtime
timer/cleanup/longjmp behavior belongs to the deep-profile runtime oracle.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import gen_all as G  # noqa: E402
from image import DEFAULT_BASE, Image  # noqa: E402

PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
# Copied from the existing generated original guest call chain. The test's
# independent list catches accidentally dropped or miscategorized entry roots.
SCOPES = {
    0x00007160: "KVD_ANM2_LOAD",
    0x000073A0: "KVD_ANM2_GRAPHICS",
    0x00520160: "KVD_ROOM_STATE_RESET",
    0x00314FC0: "KVD_ROOM_SNAPSHOT",
    0x003EBFD0: "KVD_ROOM_TRANSITION",
    0x003EB2F0: "KVD_ROOM_SWITCH",
    0x002D0BC0: "KVD_GAME_CHANGE_ROOM",
    0x0030CFD0: "KVD_LEVEL_CHANGE_ROOM",
    0x0030BA40: "KVD_ROOM_SETUP",
    0x003B0E80: "KVD_ROOM_INIT",
    0x003B1A90: "KVD_ROOM_INIT",
    0x003B26D3: "KVD_ROOM_INIT",
    0x003B2B25: "KVD_ROOM_INIT",
    0x003B548F: "KVD_ROOM_INIT",
    0x003B5499: "KVD_ROOM_INIT",
    0x003B57EA: "KVD_ROOM_INIT",
    0x003A9D70: "KVD_ROOM_CLEANUP",
    0x003AE2D0: "KVD_ROOM_SAVE",
    0x003CA670: "KVD_ROOM_PRERENDER",
    0x003CAD40: "KVD_ROOM_RENDER",
}
CALLS = (
    (0x003B11CD, 0x00007160),
    (0x003B13F0, 0x00007160),
    (0x00007193, 0x00008BE0),
    (0x00007199, 0x00008FA0),
    (0x000071A7, 0x000073A0),
    (0x00007414, 0x004B3340),
    (0x00008D7C, 0x00009B40),
    (0x004B0311, 0x002CDCF0),
    (0x002CE0FA, 0x003EBFD0),
    (0x002CE494, 0x003EBFD0),
    (0x003EC5D7, 0x003EB2F0),
    (0x003EC5F2, 0x003EB2F0),
    (0x003EC6B4, 0x003EB2F0),
    (0x003EB53F, 0x002D0BC0),
    (0x002D0C3C, 0x0030CFD0),
    (0x0030D007, 0x003AE2D0),
    (0x0030D8F9, 0x0030BA40),
    (0x0030BF2A, 0x003B0E80),
    (0x003B110C, 0x0055E330),
    (0x003B1127, 0x003A9D70),
    (0x0030CA98, 0x00401A50),
    (0x003EBAA1, 0x003CA670),
    (0x003EBEAC, 0x003CAD40),
    (0x003EC54A, 0x003CA670),
    (0x003EC585, 0x003CAD40),
    (0x003EC58F, 0x00560EB0),
)
GUARD = ("#if defined(__vita__) && defined(ISAAC_VITA_DEEP_PROFILE) && "
         "ISAAC_VITA_DEEP_PROFILE\n")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def translate(image: Image, pin: Image, root: int, switch_info: dict) -> dict:
    result = G._translate_function(image, {"rva": root}, None, switch_info, pin_img=pin)
    require(result.get("stub") is None,
            f"deep-profile root {root:08x} is a stub: {result.get('stub')}")
    return result


def check_scope_pins(pin: Image, pins: dict, identity: str) -> int:
    """Exercise actual frozen-body and CFG refusals without changing PE files."""
    failures = 0
    for root, (end, _last, _count, _ret, _digest) in pins.items():
        _, insns, order, _, unsupported, _, _ = G.B.translate(
            pin, root, f"sub_{root:08x}", None)
        require(not unsupported, f"{identity} pin control is unsupported: {root:08x}")
        require(G.vita_deep_profile_prologue(pin, root, insns, order, (), ()),
                f"{identity} pin control did not select {root:08x}")

        def rejected(candidate_order=order, entries=(), setjmps=()):
            nonlocal failures
            try:
                G.vita_deep_profile_prologue(pin, root, insns, candidate_order,
                                            entries, setjmps)
            except RuntimeError as error:
                require(identity + " deep-scope identity" in str(error),
                        f"wrong negative-test error: {error}")
                failures += 1
            else:
                raise AssertionError(f"changed {identity} scope admitted: {root:08x}")

        rejected(order[:-1])
        rejected(entries=(root + 1,))
        rejected(setjmps=(root,))
        original_image = G.Image
        raw = Image(pin.path, pin.orig_base)
        for address in (root, end - 1):
            mutated = copy.copy(raw)
            payload = bytearray(raw.mem)
            payload[address] ^= 1
            mutated.mem = bytes(payload)
            try:
                G.Image = lambda path, base: mutated
                rejected()
            finally:
                G.Image = original_image
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    parser.add_argument("--switch-cache", type=Path, required=True,
                        help="Existing whole-corpus proved switch metadata; bodies regenerate fresh")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    payload = args.pe.read_bytes()
    require(len(payload) == 8650240 and
            hashlib.sha256(payload).hexdigest() == PE_SHA256,
            "deep-profile test requires the frozen original PE")
    require(G.VITA_DEEP_PROFILE_ROOT_SCOPES == SCOPES,
            "transition root/category membership changed")
    require(len(SCOPES) == 20 and len(set(SCOPES.values())) == 14,
            "transition category census changed")
    require(set(root for root, category in SCOPES.items()
                if category == "KVD_ROOM_INIT") == G.LUA_PC_ROOM_INIT_BYPASS_ROOTS,
            "Room::Init shared-tail membership changed")
    G.EXE = str(args.pe.resolve())
    pin = Image(G.EXE, DEFAULT_BASE)
    switch_wrapper = json.loads(args.switch_cache.read_text(encoding="utf-8"))
    switch_payload = switch_wrapper["payload"]
    require(switch_wrapper["schema"] == G.SWITCH_CACHE_SCHEMA and
            hashlib.sha256(G._canonical_json_bytes(switch_payload)).hexdigest() ==
            switch_wrapper["payload_sha256"], "switch metadata checksum/schema differs")
    # Reuse the serializer's structural checks; the selected tables' actual PE
    # bytes/relocations are verified by _translate_function's normal path.
    cached_roots = frozenset(row["root"] for row in switch_payload["switch_info"])
    switch_info = G.unpack_switch_analysis(switch_payload, cached_roots)[-1]
    for site, target in CALLS:
        code = pin.code_at(site, 5)
        actual = site + 5 + int.from_bytes(code[1:], "little", signed=True)
        require(code[0] == 0xE8 and actual == target,
                f"original transition call changed: {site:08x}->{target:08x}")
    pin_refusals = check_scope_pins(pin, G.VITA_DEEP_ANM2_PROFILE_PINS, "ANM2 Load/Graphics")
    require(pin_refusals == 10, "ANM2 pin-negative census changed")
    state_refusals = check_scope_pins(pin, G.VITA_DEEP_ROOM_STATE_PROFILE_PINS, "Room reset/snapshot")
    require(state_refusals == 10, "room-state pin-negative census changed")

    records = []
    original_scopes = G.VITA_DEEP_PROFILE_ROOT_SCOPES
    # Both supported guest image bases must get the same categories. Disabling
    # the new selector is a fresh reference generation, not frozen old C.
    for base in (DEFAULT_BASE, 0x98000000):
        image = pin if base == DEFAULT_BASE else Image(G.EXE, base)
        for root, category in sorted(SCOPES.items()):
            candidate = translate(image, pin, root, switch_info)
            try:
                G.VITA_DEEP_PROFILE_ROOT_SCOPES = {}
                reference = translate(image, pin, root, switch_info)
            finally:
                G.VITA_DEEP_PROFILE_ROOT_SCOPES = original_scopes
            text = candidate["text"]
            # The existing GPR seam pass may add guarded FLUSH/RELOAD lines;
            # remove the complete conditional, not just our source template.
            matches = list(re.finditer(re.escape(GUARD) + r".*?^#endif\n",
                                       text, re.M | re.S))
            require(len(matches) == 1 and
                    text.count("KAGE_VITA_DEEP_SCOPE(") == 1,
                    f"scope absent/duplicated for {root:08x}")
            seam = matches[0].group(0)
            require(seam.count(f"KAGE_VITA_DEEP_SCOPE({category});") == 1,
                    f"wrong scope category for {root:08x}")
            require(text.replace(seam, "", 1) == reference["text"],
                    f"OFF text changed for {root:08x}")
            before = text[:text.index(seam)]
            require(before.count("{") == 1 and before.count("}") == 0 and
                    "/* %08x  " % root not in before,
                    f"scope not at function start for {root:08x}")
            require(G.unit_packing_size(text) + candidate["legacy_size_delta"] ==
                    G.unit_packing_size(reference["text"]) + reference["legacy_size_delta"],
                    f"scope changes generated TU packing for {root:08x}")
            require(text.count("return;") == reference["text"].count("return;"),
                    f"ordinary return census changed for {root:08x}")
            include = G.vita_deep_profile_unit_include([text, text])
            require(include == GUARD + '#include "kage_vita_deep_profile.h"\n#endif\n\n',
                    "affected unit include missing, unguarded or duplicated")
            require(G.vita_deep_profile_unit_include([reference["text"]]) == "",
                    "unaffected generated unit gained a header")
            records.append({"base": f"{base:08x}", "root": f"{root:08x}",
                            "category": category, "insns": candidate["insns"],
                            "returns": text.count("return;"),
                            "fresh_sha256": hashlib.sha256(text.encode()).hexdigest(),
                            "off_text_identical": True, "packing_identical": True})
        print(f"deep-profile fresh emitter base={base:08x}: 20 roots PASS", flush=True)

    # A hot ordinary update and the existing ANM2 parser are owned by their
    # existing runtime observers, not accidentally added to this coarse set.
    for root in (0x002CDCF0, 0x00009B40):
        result = translate(pin, pin, root, switch_info)
        require("KAGE_VITA_DEEP_SCOPE(" not in result["text"],
                f"unselected root {root:08x} received a coarse scope")
    require(G.vita_deep_profile_unit_include([]) == "", "empty unit gained a header")
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / "deep-codegen.json").write_text(
            json.dumps({"pe_sha256": PE_SHA256, "direct_calls": len(CALLS),
                        "anm2_pin_refusals": pin_refusals,
                        "room_state_pin_refusals": state_refusals,
                        "switch_payload_sha256": switch_wrapper["payload_sha256"],
                        "records": records}, indent=2) + "\n", encoding="utf-8")
    print("Deep profile codegen PASS: 26 original calls; 40 fresh scoped bodies; "
          "two fresh unselected controls; 10 ANM2 + 10 room-state pin refusals; OFF identity; stable unit packing")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
