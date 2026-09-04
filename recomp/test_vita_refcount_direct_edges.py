#!/usr/bin/env python3
"""Prove ImageBase refcount edges and their exact sync-IAT sites."""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
ROOT_EDGES = {
    0x00007790: ((0x000077C1, 0x00007AF0),),
    0x000078A0: ((0x000078C8, 0x00007B70),),
    0x00007AF0: (
        (0x00007AFC, 0x00562E00),
        (0x00007B18, 0x00562EC0),
        (0x00007B41, 0x00562EC0),
    ),
    0x00007B50: (
        (0x00007B5C, 0x00562E00),
        (0x00007B6B, 0x00562EC0),
    ),
    0x00007B70: (
        (0x00007B7C, 0x00562E00),
        (0x00007B8F, 0x00562EC0),
        (0x00007B96, 0x00562EC0),
        (0x00007B9C, 0x00007B50),
    ),
}
ROOT_SYNC_IAT = {
    0x00562E00: (
        (0x00562E29, 0x006060FC),
        (0x00562E8A, 0x006060F8),
    ),
    0x00562EC0: (
        (0x00562EE0, 0x006060F8),
    ),
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(command: list[str]) -> str:
    result = subprocess.run(
        command, check=False, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}): {command!r}\n{result.stdout}"
        )
    return result.stdout


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def compiler(explicit: str | None) -> str:
    if explicit:
        return explicit
    if os.environ.get("CC"):
        return os.environ["CC"]
    for candidate in ("clang", "cc", "gcc"):
        found = shutil.which(candidate)
        if found:
            return found
    if os.name == "nt":
        installed = Path(os.environ.get(
            "ProgramFiles", r"C:\Program Files"
        )) / "LLVM" / "bin" / "clang.exe"
        if installed.is_file():
            return str(installed)
    raise AssertionError("no GCC-compatible host compiler found; pass --cc")


def verify_codegen(pe_path: Path) -> None:
    os.environ["REPENTOGXM_PE"] = str(pe_path)
    sys.path.insert(0, str(HERE))
    import gen_all  # noqa: E402
    from image import DEFAULT_BASE, Image  # noqa: E402

    pin = Image(str(pe_path), DEFAULT_BASE)
    for root, expected in ROOT_EDGES.items():
        for base in (DEFAULT_BASE, 0x98000000):
            result = gen_all._translate_function(
                Image(str(pe_path), base), {"rva": root}, None, {},
                pin_img=pin,
            )
            require(result["stub"] is None,
                    f"refcount owner became a stub at {root:#x}/{base:#x}")
            require(result["vita_refcount_direct_edges"] == expected,
                    f"refcount edge census changed at {root:#x}/{base:#x}")
            text = result["text"]
            require(text.count("guest_direct_translated_target(") ==
                    len(expected),
                    f"direct target guards changed at {root:#x}/{base:#x}")
            require(text.count("GUEST_PHASE_PROFILE_NOTE_CALL();") ==
                    len(expected),
                    f"direct logical call census changed at {root:#x}")
            require(text.count(
                "guest_phase_profile_note_lookup_cache_hit();") ==
                    len(expected),
                    f"direct cache census changed at {root:#x}")
            for _site, target in expected:
                require(f"_target, 0x{target:08x}U" in text,
                        f"exact target missing at {root:#x}/{target:#x}")
                require(f"sub_{target:08x}(c);" in text,
                        f"direct owner missing at {root:#x}/{target:#x}")
    require(sum(len(edges) for edges in ROOT_EDGES.values()) == 11,
            "focused edge total changed")
    for root, expected in ROOT_SYNC_IAT.items():
        for base in (DEFAULT_BASE, 0x98000000):
            result = gen_all._translate_function(
                Image(str(pe_path), base), {"rva": root}, None, {},
                pin_img=pin,
            )
            require(result["stub"] is None,
                    f"sync wrapper became a stub at {root:#x}/{base:#x}")
            require(result["vita_refcount_sync_iat_sites"] == expected,
                    f"sync-IAT census changed at {root:#x}/{base:#x}")
            text = result["text"]
            require(text.count("guest_try_direct_sync_import_call(") ==
                    len(expected),
                    f"direct sync route count changed at {root:#x}")
            for site, slot in expected:
                require(f"c, _target, 0x{slot:08x}U" in text,
                        f"exact IAT slot missing at {site:#x}/{base:#x}")
                require(f"gpush_generated(c, 0x{site + 6:x}U);" in text,
                        f"return push changed at {site:#x}/{base:#x}")
            require("0x005eb2" not in text.lower(),
                    "static-init sync sites leaked into refcount wrapper")
    require(sum(len(sites) for sites in ROOT_SYNC_IAT.values()) == 3,
            "focused sync-IAT total changed")


def run_oracle(cc: str, image_base: int) -> str:
    with tempfile.TemporaryDirectory(prefix="isaac-refcount-direct-") as value:
        output = Path(value) / ("oracle.exe" if os.name == "nt" else "oracle")
        run([
            cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-DISAAC_VITA_PHASE_PROFILE=1",
            f"-DGUEST_IMAGE_BASE=0x{image_base:08x}U",
            "-I", str(RUNTIME),
            str(RUNTIME / "host_vita_refcount_direct_oracle.c"),
            "-o", str(output),
        ])
        return run([str(output)]).strip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=Path)
    parser.add_argument("--cc")
    arguments = parser.parse_args()

    require(arguments.pe.stat().st_size == PE_SIZE, "frozen PE size changed")
    require(digest(arguments.pe) == PE_SHA256, "frozen PE hash changed")
    verify_codegen(arguments.pe)
    cc = compiler(arguments.cc)
    for base in (0x30000000, 0x98000000):
        result = run_oracle(cc, base)
        require(result == (
            "Vita ReferenceCount direct-edge oracle: PASS; checks=150; "
            f"routes=25; base={base:08x}"
        ), "refcount direct-edge oracle result changed")
        print(result)
    print("Vita ReferenceCount direct edges: PASS; roots=5; sites=11; "
          "sync_wrappers=2; sync_iat_sites=3")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
