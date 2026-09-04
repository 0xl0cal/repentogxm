#!/usr/bin/env python3
"""Prove the frozen floor thunk seam and run its focused inline oracle.

The seam is a generation-time switch (GUEST_FLOOR_THUNK_FASTPATH=1).  This
test renders the thunk root both ways: switched off it must be the exact
text of the default corpus (pass --generated to compare against a corpus
directory), switched on it must carry exactly one fenced seam and the same
unit packing size.  The ILP32 host oracle then executes the helper against a
real guest frame (handled, rejected and hostile cases) and pins its receipts.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
ROOT = 0x005EC3B2
IAT_RVA = 0x00606524
IMPORT_ID = 313
SWITCH = "GUEST_FLOOR_THUNK_FASTPATH"
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
MARKER = "Authenticated floor IAT fast dispatch"
ORACLE_RESULT = (
    "Vita floor thunk oracle: PASS; checks=%d; "
    "handled=15; rejected=6; hostile=6"
)
# Plain phase-profile build; with the per-import census array (the dispatch
# table / import-kinds owners' view); with the sampler's indirect-target word.
ORACLE_CHECKS = {
    (): 6135,
    ("-DISAAC_VITA_PROFILE_IMPORT_KINDS=1",): 11917,
    ("-DISAAC_VITA_GUEST_SAMPLER=1",): 6160,
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(command: list[str], env: dict[str, str] | None = None) -> str:
    result = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
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


def compiler(explicit: str | None) -> list[str]:
    """An ILP32 host compiler: the seam reads guest memory by 32-bit address."""
    if explicit:
        return shlex.split(explicit)
    if os.environ.get("CC"):
        return shlex.split(os.environ["CC"])
    candidates = []
    for name in ("clang", "gcc", "cc"):
        found = shutil.which(name)
        if found:
            candidates.append(found)
    if os.name == "nt":
        installed = Path(os.environ.get(
            "ProgramFiles", r"C:\Program Files"
        )) / "LLVM" / "bin" / "clang.exe"
        if installed.is_file():
            candidates.append(str(installed))
    for found in candidates:
        for flags in (["-m32"], []):
            cc = [found, *flags]
            if ilp32_ok(cc):
                return cc
    raise AssertionError(
        "no ILP32 host compiler found; pass --cc \"clang -m32\"")


def ilp32_ok(cc: list[str]) -> bool:
    with tempfile.TemporaryDirectory(prefix="isaac-floor-ilp32-") as value:
        probe = Path(value) / "probe.c"
        probe.write_text(
            "_Static_assert(sizeof(void *) == 4, \"ILP32\");\n"
            "int main(void) { return 0; }\n", encoding="utf-8")
        result = subprocess.run(
            [*cc, "-c", str(probe), "-o", str(Path(value) / "probe.o")],
            check=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True)
        return result.returncode == 0


def body_text(text: str, name: str) -> str:
    match = re.search(
        r"^/\* %s  RVA .*?\n(?:.*?\n)*?^\}\n" % re.escape(name),
        text, re.MULTILINE)
    require(match is not None, f"{name} body not found")
    return match.group(0)


def verify_codegen(pe_path: Path, generated: Path | None,
                   generated_on: Path | None = None) -> None:
    os.environ["REPENTOGXM_PE"] = str(pe_path)
    sys.path.insert(0, str(HERE))
    import gen_all  # noqa: E402
    import gpr_locals  # noqa: E402
    from image import DEFAULT_BASE, Image  # noqa: E402

    require(gen_all.VITA_FLOOR_THUNK_FASTPATH_ROOT_RVA == ROOT and
            gen_all.VITA_FLOOR_THUNK_FASTPATH_IAT_RVA == IAT_RVA and
            gen_all.VITA_FLOOR_THUNK_FASTPATH_IMPORT_ID == IMPORT_ID and
            gen_all.VITA_FLOOR_THUNK_FASTPATH_ENV == SWITCH,
            "floor thunk generator constants drifted")
    pin = Image(str(pe_path), DEFAULT_BASE)
    # The corpus spells IAT sites through GUEST_IMPORT_CALL/JMP only when the
    # emitter knows the dense slot table (gen_all passes it to its workers).
    from iat_meta import read_imports  # noqa: E402
    saved_import_ids = gen_all._WORKER_IMPORT_IDS
    gen_all._WORKER_IMPORT_IDS = {
        slot_rva: index
        for index, (slot_rva, _name) in enumerate(read_imports(str(pe_path)))
    }
    require(gen_all._WORKER_IMPORT_IDS.get(IAT_RVA) == IMPORT_ID,
            "frozen PE import table no longer binds row 313 to the floor slot")
    try:
        _verify_codegen_bodies(gen_all, gpr_locals, Image, DEFAULT_BASE,
                               pe_path, pin, generated, generated_on)
    finally:
        gen_all._WORKER_IMPORT_IDS = saved_import_ids
        os.environ.pop(SWITCH, None)


def _verify_codegen_bodies(gen_all, gpr_locals, Image, DEFAULT_BASE, pe_path,
                           pin, generated, generated_on=None) -> None:
    for base in (DEFAULT_BASE, 0x98000000):
        image = Image(str(pe_path), base)
        iat_word = "ld32((uint32_t)(0x%xU))" % ((base + IAT_RVA) & 0xFFFFFFFF)

        os.environ[SWITCH] = "0"
        require(not gen_all.vita_floor_thunk_fastpath_enabled(),
                "switch reads as enabled while off")
        off = gen_all._translate_function(
            image, {"rva": ROOT}, None, {}, pin_img=pin
        )
        require(off["stub"] is None,
                f"floor thunk became a stub at base {base:#x}")
        require(not off["vita_floor_thunk_fastpath"],
                f"floor seam selected with the switch off at base {base:#x}")
        require("floor_thunk" not in off["text"] and MARKER not in off["text"],
                "switched-off text mentions the floor seam")
        require("/* 005ec3b2  jmp dword ptr" in off["text"] and
                "GUEST_IMPORT_JMP(ld32(" in off["text"] and
                "0x%08xU, %dU);" % (IAT_RVA, IMPORT_ID) in off["text"],
                "translated floor GUEST_IMPORT_JMP spelling disappeared")

        os.environ[SWITCH] = "1"
        require(gen_all.vita_floor_thunk_fastpath_enabled(),
                "switch reads as disabled while on")
        on = gen_all._translate_function(
            image, {"rva": ROOT}, None, {}, pin_img=pin
        )
        require(on["stub"] is None,
                f"floor thunk became a stub at base {base:#x} (on)")
        require(on["vita_floor_thunk_fastpath"],
                f"floor proof was not selected at base {base:#x}")
        text = on["text"]
        require(text.count("isaac_vita_floor_thunk_direct_try(") == 2,
                "floor declaration/seam count changed")
        require(text.count(MARKER) == 1, "floor fast-path marker changed")
        require(text.count("defined(ISAAC_VITA_FLOOR_THUNK_FASTPATH)") == 2,
                "floor feature fence count changed")
        require("const uint32_t _target = %s;" % iat_word in text,
                "floor seam does not read the frozen IAT slot")
        require("if (isaac_vita_floor_thunk_direct_try(c, _target))" in text,
                "floor seam call changed")
        require("/* 005ec3b2  jmp dword ptr" in text and
                "GUEST_IMPORT_JMP(ld32(" in text,
                "translated floor fallback disappeared from the seam body")
        require("memset" not in text, "floor body carries the memset seam")
        require(text.index(MARKER) < text.index("/* 005ec3b2  jmp"),
                "floor seam is not ahead of the translated fallback")
        # The seam is fenced text only: dropping the fenced blocks gives the
        # switched-off body back exactly.
        stripped = re.sub(
            r"#if defined\(__vita__\) && "
            r"defined\(ISAAC_VITA_FLOOR_THUNK_FASTPATH\)\n(?:.*\n)*?#endif\n",
            "", text)
        require(stripped == off["text"],
                "switched-on body is not the switched-off body plus the "
                "fenced seam")
        # Unit packing: the seam bytes are taken back out of the budget.
        packing_off = (gen_all.unit_packing_size(off["text"]) +
                       off["legacy_size_delta"])
        packing_on = (gen_all.unit_packing_size(text) +
                      on["legacy_size_delta"])
        require(packing_on == packing_off,
                f"floor seam changed the unit packing size: "
                f"{packing_on} != {packing_off}")
        # The GPR bracket pass published the locals around the host call.
        legacy = gpr_locals.legacy_text(text)
        require("GUEST_GPR_FLUSH(c);\n        if (isaac_vita_floor_thunk"
                in text and "return;\n        GUEST_GPR_RELOAD(c);" in text,
                "floor seam is not bracketed for GPR locals")
        require(MARKER in legacy, "legacy text lost the seam marker")

        if generated is not None and base == 0x98000000:
            unit = generated / "guest_0184.c"
            require(unit.is_file(), f"{unit} is missing")
            corpus = body_text(unit.read_text(encoding="utf-8"),
                               "sub_005ec3b2")
            # The corpus body carries the coverage entry hook whose dense ID
            # only a whole-corpus generation assigns; everything else must
            # be the exact switched-off text.
            corpus, hooks = re.subn(
                r"^    guest_coverage_function\(\d+U\);\n", "", corpus,
                count=1, flags=re.MULTILINE)
            require(hooks == 1, "corpus body lost its coverage entry hook")
            require(corpus == off["text"],
                    "switched-off body differs from the corpus text")
        if generated_on is not None and base == 0x98000000:
            # A corpus generated with the switch on carries exactly the
            # switched-on body (plus the same dense coverage hook).
            unit = generated_on / "guest_0184.c"
            require(unit.is_file(), f"{unit} is missing")
            corpus_on = body_text(unit.read_text(encoding="utf-8"),
                                  "sub_005ec3b2")
            corpus_on, hooks = re.subn(
                r"^    guest_coverage_function\(\d+U\);\n", "", corpus_on,
                count=1, flags=re.MULTILINE)
            require(hooks == 1,
                    "switched-on corpus body lost its coverage entry hook")
            require(corpus_on == text,
                    "switched-on corpus body differs from the switched-on "
                    "rendering")
    os.environ.pop(SWITCH, None)


def verify_import_row() -> None:
    """Dense row 313 of the frozen contract binds the seam's constants."""
    rows = (RUNTIME / "host_vita_import_id_map.inc").read_text(
        encoding="utf-8").splitlines()
    row = [line for line in rows
           if line.startswith("ISAAC_VITA_IMPORT_ID_ROW(%dU," % IMPORT_ID)]
    require(row == [
        'ISAAC_VITA_IMPORT_ID_ROW(313U, 0x00606524U, '
        '"api-ms-win-crt-math-l1-1-0.dll!floor", ISAAC_VITA_IMPORT_MATH, 2U)'
    ], f"floor import row drifted: {row}")
    math_header = (RUNTIME / "host_vita_math.h").read_text(encoding="utf-8")
    require("#define ISAAC_VITA_MATH_FLOOR_IAT_RVA            0x00606524U"
            in math_header and
            "#define ISAAC_VITA_MATH_FLOOR_THUNK_RVA          0x005ec3b2U"
            in math_header and
            "#define ISAAC_VITA_MATH_FLOOR_CALL_COUNT         438U"
            in math_header, "math family floor constants drifted")
    helper = (RUNTIME / "host_vita_floor_thunk_direct.c").read_text(
        encoding="utf-8")
    require("ISAAC_FLOOR_IAT_RVA = 0x00606524U" in helper and
            "ISAAC_FLOOR_IMPORT_ID = 313U" in helper and
            "ISAAC_FLOOR_MATH_INDEX = 2U" in helper,
            "helper constants drifted from row 313")
    require("fpush(c, floor(ldd(esp + 4U)));" in helper and
            "c->esp = esp + 4U;" in helper and
            "guest_stack_contains(c, esp, ISAAC_FLOOR_FRAME_BYTES)" in helper,
            "helper no longer spells vita_math_floor")
    math_source = (RUNTIME / "host_vita_math.c").read_text(encoding="utf-8")
    require("fpush(c, floor(ldd(guest_stack_address(c, c->esp + 4U, 8U, 0U))));"
            in math_source, "vita_math_floor changed shape")


def run_oracle(cc: list[str]) -> list[str]:
    results = []
    with tempfile.TemporaryDirectory(prefix="isaac-floor-thunk-") as value:
        for index, (extra, checks) in enumerate(ORACLE_CHECKS.items()):
            output = Path(value) / (
                f"oracle{index}.exe" if os.name == "nt" else f"oracle{index}")
            run([
                *cc,
                "-std=c11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-DISAAC_VITA_FLOOR_THUNK_FASTPATH=1",
                "-DISAAC_VITA_PHASE_PROFILE=1",
                *extra,
                "-I",
                str(RUNTIME),
                str(RUNTIME / "host_vita_floor_thunk_direct.c"),
                str(RUNTIME / "host_vita_floor_thunk_direct_oracle.c"),
                "-o",
                str(output),
            ])
            result = run([str(output)]).strip()
            require(result == ORACLE_RESULT % checks,
                    f"floor thunk oracle result changed ({extra}): {result}")
            results.append(result)
    return results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=Path)
    parser.add_argument("--generated", type=Path,
                        help="default corpus directory (guest_0184.c must "
                             "carry the switched-off body)")
    parser.add_argument("--generated-on", type=Path,
                        help="switched-on corpus directory (guest_0184.c must "
                             "carry the fenced seam)")
    parser.add_argument("--cc", help='ILP32 host compiler, e.g. "clang -m32"')
    arguments = parser.parse_args()

    require(arguments.pe.stat().st_size == PE_SIZE, "frozen PE size changed")
    require(digest(arguments.pe) == PE_SHA256, "frozen PE hash changed")
    verify_import_row()
    verify_codegen(arguments.pe, arguments.generated, arguments.generated_on)
    cc = compiler(arguments.cc)
    require(ilp32_ok(cc), f"{' '.join(cc)} is not an ILP32 compiler")
    for line in run_oracle(cc):
        print(line)
    print("Vita floor thunk fast path: PASS; direct-call sites=438; "
          "switch=%s; owner=guest_0184.c" % SWITCH)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
