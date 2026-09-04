#!/usr/bin/env python3
"""Frozen owner/codegen proof for Vita-rooted Resource::Open filenames."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import sys
from gpr_locals import legacy_text  # noqa: E402


HERE = Path(__file__).resolve().parent
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
ROOT = 0x00563120
VITA_BASE = 0x98000000
MARKER = "Vita title-root paths are absolute, despite not being X:/ paths."


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def policy_reference(path: str) -> bool:
    """Readable mirror of the emitted exact-root admission policy."""
    if not path.startswith("ux0:"):
        return False
    suffix = path[4:]
    if suffix.startswith("/"):
        suffix = suffix[1:]
    root = "data/isaacr001"
    return suffix == root or suffix.startswith(root + "/")


def verify_policy() -> None:
    accepted = (
        "ux0:data/isaacr001",
        "ux0:/data/isaacr001",
        "ux0:data/isaacr001/mods/836319872/main.lua",
        "ux0:/data/isaacr001/mods/external item descriptions_836319872/main.lua",
    )
    rejected = (
        "836319872/main.lua",
        "ux0:data",
        "ux0:data/isaacr0012/main.lua",
        "ux0:data/another-title/main.lua",
        "ux0:/data/isaacr0012/main.lua",
        "ux1:data/isaacr001/main.lua",
        "UX0:data/isaacr001/main.lua",
        r"ux0:data\isaacr001\main.lua",
        r"E:\SteamLibrary\main.lua",
    )
    require(all(policy_reference(path) for path in accepted),
            "approved Vita data-root spelling was rejected")
    require(not any(policy_reference(path) for path in rejected),
            "path outside the exact Vita title root was admitted")


def verify_codegen(pe_path: Path) -> None:
    os.environ["REPENTOGXM_PE"] = str(pe_path)
    sys.path.insert(0, str(HERE))
    import gen_all as G  # noqa: E402
    from image import DEFAULT_BASE, Image  # noqa: E402

    require(G.LUA_CALLBACK_NEUTRAL_TRANSLATION.get(
        "vita_rooted_file_probe") is False,
        "Vita rooted-file result field lacks its neutral callback default")
    ordinary_result_fields = {
        "rva", "public_name", "name", "manual", "stub", "text", "insns",
        "lines", "entry_gate",
    }
    pin = Image(str(pe_path), DEFAULT_BASE)
    for base in (DEFAULT_BASE, VITA_BASE):
        image = Image(str(pe_path), base)
        result = G._translate_function(
            image, {"rva": ROOT}, None, {}, pin_img=pin
        )
        require(result["stub"] is None,
                f"Resource::Open became a stub at base {base:#x}")
        require(result["vita_rooted_file_probe"] is True,
                f"Vita rooted-file seam disappeared at base {base:#x}")
        require(set(result) == ordinary_result_fields |
                set(G.LUA_CALLBACK_NEUTRAL_TRANSLATION),
                f"translation result fields escaped the callback contract "
                f"at base {base:#x}")
        source = legacy_text(result["text"])
        require(source.count(MARKER) == 1,
                f"Vita rooted-file block count changed at base {base:#x}")
        require(source.count("static const char _vita_root_tail[] =") == 1,
                f"Vita root policy literal count changed at base {base:#x}")
        require(source.count("                goto L_00563151;") == 1,
                f"direct-stdio edge count changed at base {base:#x}")

        length_guard = source.index(
            "if (((uint32_t)(c->edx) <= (uint32_t)(0x2U)))")
        fence = source.index("#if defined(__vita__)", length_guard)
        marker = source.index(MARKER, fence)
        original = source.index(
            "/* 00563140  cmp byte ptr [esi + 1], 0x3a */", marker
        )
        target = source.index("L_00563151:", original)
        require(length_guard < fence < marker < original < target,
                f"rooted-file fallback ordering changed at base {base:#x}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", required=True, type=Path)
    arguments = parser.parse_args()

    pe_path = arguments.pe.resolve()
    require(pe_path.stat().st_size == PE_SIZE, "frozen PE size changed")
    require(digest(pe_path) == PE_SHA256, "frozen PE hash changed")
    verify_policy()
    verify_codegen(pe_path)
    print("Vita rooted Resource::Open frozen owner/codegen: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
