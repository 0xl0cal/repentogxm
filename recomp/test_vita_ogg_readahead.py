#!/usr/bin/env python3
"""Structural proof of ISAAC_VITA_OGG_READAHEAD (host_vita_ogg_readahead.c).

1. Build gate: the option defaults OFF, the three cache strings default to
   4 chunks / 20000 us / priority 168 and are range-checked, the knob refuses
   to configure without NATIVE_VORBIS, ARCHIVE_FILE_CACHE and
   CRT_DESCRIPTOR_RECOVER, and the source, its defines and the three
   --wrap options are reachable from the knob block only.
2. OFF identity at the source level: in host_vita_crt.c and
   host_vita_native_vorbis.c every read-ahead line sits inside an
   `#if defined(ISAAC_VITA_OGG_READAHEAD)` block that has no #else/#elif, so
   with the knob OFF the preprocessor sees exactly the file without those
   blocks.  With --base REV the stripped files must also equal REV's copies
   byte for byte (the object-level proof is a cmp gate on an ARM build).
3. Registries: the raw allocator gate registers the new unit under the knob,
   its test fixture lists the knob OFF, and the README documents it.

The behaviour proofs (byte traces, PCM, faults, races, mutants) are in
recomp/vita/test_ogg_readahead.sh.

usage: test_vita_ogg_readahead.py [--base REV]
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
RUNTIME = HERE / "runtime"
VITA = HERE / "vita"
KNOB = "ISAAC_VITA_OGG_READAHEAD"
GUARD = f"#if defined({KNOB})"
HOOKED = ("host_vita_crt.c", "host_vita_native_vorbis.c")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def verify_build_gate() -> None:
    cmake = (VITA / "CMakeLists.txt").read_text(encoding="utf-8")
    require(re.search(rf"option\({KNOB}\s+\"[^\"]*\"\s+OFF\)", cmake)
            is not None, "option missing or not default OFF")
    for name, default in (("CHUNKS", "4"), ("WAIT_US", "20000"),
                          ("PRIORITY", "168")):
        require(re.search(rf"set\({KNOB}_{name} \"{default}\" CACHE STRING",
                          cmake) is not None,
                f"{KNOB}_{name} default {default} missing")
        require(f"{KNOB}_{name} must be" in cmake,
                f"{KNOB}_{name} is not range-checked")
    require(f"{KNOB} requires ISAAC_VITA_NATIVE_VORBIS=ON, "
            "ISAAC_VITA_ARCHIVE_FILE_CACHE=ON and "
            "ISAAC_VITA_CRT_DESCRIPTOR_RECOVER=ON" in cmake,
            "prerequisite gate missing")
    block = re.search(rf"\n        if\({KNOB}\)\n(.*?)\n        endif\(\)\n",
                      cmake, re.S)
    require(block is not None, "knob block missing")
    body = block.group(1)
    for needle in ('"${ISAAC_RUNTIME}/host_vita_ogg_readahead.c"',
                   f"{KNOB}=1",
                   f"ISAAC_OGG_RA_CHUNKS=${{{KNOB}_CHUNKS}}U",
                   f"ISAAC_OGG_RA_WAIT_US=${{{KNOB}_WAIT_US}}U",
                   f"ISAAC_OGG_RA_PRIORITY=${{{KNOB}_PRIORITY}}",
                   '"${ISAAC_RUNTIME}/host_vita_crt.c"',
                   '"${ISAAC_RUNTIME}/host_vita_native_vorbis.c"',
                   "-Wl,--wrap=_read_r", "-Wl,--wrap=_lseek_r",
                   "-Wl,--wrap=_close_r"):
        require(needle in body, f"knob block lacks {needle}")
    require(cmake.count("host_vita_ogg_readahead.c") == 2 and
            cmake.count("--wrap=_read_r") == 1 and
            cmake.count("--wrap=_lseek_r") == 1 and
            cmake.count("--wrap=_close_r") == 1 and
            cmake.count(f"{KNOB}=1") == 2,
            "the module, its defines and the wraps must be reachable from "
            "the knob block only")
    # The knob block sits inside the NATIVE_VORBIS block (its prerequisite).
    vorbis = cmake.find("      if(ISAAC_VITA_NATIVE_VORBIS)\n")
    require(0 <= vorbis < block.start(),
            "knob block must be nested in the NATIVE_VORBIS block")


def strip_blocks(text: str, path: str) -> tuple[str, int]:
    """Remove every `#if defined(KNOB)` ... matching `#endif` block."""
    out: list[str] = []
    depth = 0           # nesting inside a knob block
    blocks = 0
    for line in text.splitlines(keepends=True):
        directive = line.lstrip()
        if depth == 0:
            if directive.startswith(GUARD) and \
                    directive[len(GUARD):].strip() == "":
                depth = 1
                blocks += 1
                continue
            require("OGG_READAHEAD" not in line and "ogg_ra" not in line,
                    f"{path}: read-ahead reference outside a guarded block: "
                    f"{line.strip()}")
            out.append(line)
            continue
        if re.match(r"#\s*if", directive):
            depth += 1
        elif re.match(r"#\s*(else|elif)", directive) and depth == 1:
            raise SystemExit(f"FAIL: {path}: #else/#elif in a {KNOB} block")
        elif re.match(r"#\s*endif", directive):
            depth -= 1
    require(depth == 0, f"{path}: unterminated {KNOB} block")
    return "".join(out), blocks


def git_show(rev: str, path: Path) -> str:
    rel = path.relative_to(REPO).as_posix()
    result = subprocess.run(["git", "-C", str(REPO), "show", f"{rev}:{rel}"],
                            capture_output=True, check=False)
    require(result.returncode == 0,
            f"git show {rev}:{rel} failed: {result.stderr.decode().strip()}")
    return result.stdout.decode("utf-8")


def verify_sources(base: str | None) -> list[str]:
    lines = []
    for name in HOOKED:
        path = RUNTIME / name
        text = path.read_bytes().decode("utf-8")
        stripped, blocks = strip_blocks(text, name)
        require(blocks > 0, f"{name}: no {KNOB} block")
        verdict = "insertion-only"
        if base:
            require(stripped == git_show(base, path),
                    f"{name}: OFF text differs from {base}")
            verdict += f", OFF text == {base}"
        lines.append(f"{name}: {blocks} guarded blocks, {verdict}")
    return lines


def verify_registries() -> None:
    gate = (VITA / "vita_raw_allocator_gate.py").read_text(encoding="utf-8")
    require(re.search(rf'if _feature\(cache, "{KNOB}"\):\s*\n\s*'
                      r'runtime\.add\("host_vita_ogg_readahead\.c"\)', gate)
            is not None, "raw allocator gate does not register the unit")
    gate_test = (VITA / "test_vita_raw_allocator_gate.py").read_text(
        encoding="utf-8")
    require(f'"{KNOB}": "OFF"' in gate_test,
            "raw allocator gate fixture does not list the knob OFF")
    require('{"host_vita_ogg_readahead.c"}' in gate_test,
            "raw allocator gate test has no inventory case for the unit")
    readme = (VITA / "README.md").read_text(encoding="utf-8")
    require(readme.count(f"`{KNOB}`") >= 2,
            "README must list the knob and document it")
    require("test_ogg_readahead.sh" in readme,
            "README must name the proof script")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", help="revision the OFF text must equal")
    arguments = parser.parse_args()
    verify_build_gate()
    lines = verify_sources(arguments.base)
    verify_registries()
    for line in lines:
        print(line)
    print(f"Vita OGG read-ahead structure: PASS (build gate, {len(lines)} "
          "hooked units, registries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
