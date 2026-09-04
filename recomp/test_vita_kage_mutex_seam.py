#!/usr/bin/env python3
"""Prove the KAGE Mutex::Lock(-1)/Unlock native seam (ISAAC_VITA_KAGE_MUTEX_SEAM).

Modelled on test_vita_memset_thunk_fastpath.py:

  1. frozen PE pins (file size/sha, the two wrapper bodies byte for byte);
  2. gen_all renders both roots at both image bases with exactly one fenced
     seam statement each, placed between the coverage note and the first
     translated instruction, bracketed for the GPR locals, with the complete
     translated body (its authenticated sync-IAT sites) still behind it, and
     with a unit-packing size unchanged by the seam text;
  3. hostile authenticator cases (drifted instruction, missing sync-IAT site,
     unrelated root);
  4. the seam's copy of the critical-section object predicate equals the
     inline fast path's text (host_vita_sync_fastpath.h);
  5. CMake option default OFF, the SYNC_INLINE_FASTPATH requirement and the
     exactly-one-owner/one-seam-per-root census; the raw allocator gate's
     compile-scope check with hostile records;
  6. optional --generated-dir: corpus census (one owner, one seam per root,
     nothing elsewhere) and, with --baseline-generated-dir, the proof that
     the regenerated corpus differs from the baseline only by the seam lines
     of the owner unit;
  7. optional --cc: the host differential oracle
     (host_vita_kage_mutex_seam_oracle.c) against the ACTUAL translated
     bodies, both synthetic-stack modes, three seeds.  Needs a GNU-compatible
     compiler on a platform with the psp2 headers (--vitasdk-include) and
     mmap/VirtualAlloc below 4 GiB; the box recipe is
       --cc gcc --vitasdk-include $VITASDK/arm-vita-eabi/include
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
VITA = HERE / "vita"
sys.path.insert(0, str(VITA))

PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
LOCK_ROOT = 0x00562E00
LOCK_END = 0x00562EB4
LOCK_SHA256 = "e01c3b984ecf5a019fee83a92953e521c102871a07256bc38e2dd678149317f4"
UNLOCK_ROOT = 0x00562EC0
UNLOCK_END = 0x00562EE8
UNLOCK_SHA256 = "1a43a34d658f732916672c3b4f11b96f820fc523c32b008a9442641e86df86fb"
ROOTS = {
    LOCK_ROOT: ("sub_00562e00", "isaac_vita_kage_mutex_lock_try", 7968,
                "KAGE Mutex::Lock(-1) native critical-section seam",
                "/* 00562e00  push ebp */", 2),
    UNLOCK_ROOT: ("sub_00562ec0", "isaac_vita_kage_mutex_unlock_try", 7969,
                  "KAGE Mutex::Unlock native critical-section seam",
                  "/* 00562ec0  push esi */", 1),
}
# GUEST_IMPORT_CALL tokens per body in the corpus spelling (every IAT call
# site, including the fallback call behind each authenticated sync site).
IMPORT_SITES = {LOCK_ROOT: 6, UNLOCK_ROOT: 1}
FENCE = "#if defined(__vita__) && defined(ISAAC_VITA_KAGE_MUTEX_SEAM)"
OPTION = "ISAAC_VITA_KAGE_MUTEX_SEAM"
ORACLE_PASS = "Vita KAGE mutex seam oracle: PASS"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def must_fail(label: str, callback, needle: str, kinds=(Exception,)) -> None:
    try:
        callback()
    except kinds as exc:  # noqa: PERF203
        if needle not in str(exc):
            raise AssertionError(f"{label}: wrong failure: {exc}") from exc
    else:
        raise AssertionError(f"{label}: hostile case passed")


def run(command: list[str], cwd: Path | None = None) -> str:
    result = subprocess.run(
        command, check=False, cwd=cwd, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, errors="replace",
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


# ---------------------------------------------------------------- 1. PE pins

def verify_pe(pe_path: Path) -> None:
    require(pe_path.stat().st_size == PE_SIZE, "frozen PE size changed")
    require(digest(pe_path) == PE_SHA256, "frozen PE hash changed")
    from image import DEFAULT_BASE, Image  # noqa: E402

    # Unrelocated file bytes (Image applies the PE relocations for any other
    # base; gen_all's own relocated-body pins are checked in verify_codegen).
    image = Image(str(pe_path), Image(str(pe_path), DEFAULT_BASE).orig_base)
    require(image.orig_base == 0x400000, "PE ImageBase changed")
    lock = bytes(image.code_at(LOCK_ROOT, LOCK_END - LOCK_ROOT))
    unlock = bytes(image.code_at(UNLOCK_ROOT, UNLOCK_END - UNLOCK_ROOT))
    require(hashlib.sha256(lock).hexdigest() == LOCK_SHA256,
            "Mutex::Lock body bytes changed")
    require(hashlib.sha256(unlock).hexdigest() == UNLOCK_SHA256,
            "Mutex::Unlock body bytes changed")
    # The instructions the seam replays, as bytes at their pinned RVAs.
    pinned = {
        0x00562E08: bytes.fromhex("807b0400"),      # cmp byte [ebx+4], 0
        0x00562E20: bytes.fromhex("83ffff"),        # cmp edi, -1
        0x00562E25: bytes.fromhex("8b7308"),        # mov esi, [ebx+8]
        0x00562E29: bytes.fromhex("ff15fc60a000"),  # call [EnterCriticalSection]
        0x00562E2F: bytes.fromhex("807e1800"),      # cmp byte [esi+0x18], 0
        0x00562E4D: bytes.fromhex("c6461801"),      # mov byte [esi+0x18], 1
        0x00562E53: bytes.fromhex("b001"),          # mov al, 1
        0x00562E57: bytes.fromhex("c20400"),        # ret 4
        0x00562EC3: bytes.fromhex("807e0400"),      # cmp byte [esi+4], 0
        0x00562ED8: bytes.fromhex("8b4608"),        # mov eax, [esi+8]
        0x00562EDC: bytes.fromhex("c6401800"),      # mov byte [eax+0x18], 0
        0x00562EE0: bytes.fromhex("ff15f860a000"),  # call [LeaveCriticalSection]
        0x00562EE7: bytes.fromhex("c3"),            # ret
    }
    for rva, expected in pinned.items():
        require(bytes(image.code_at(rva, len(expected))) == expected,
                f"pinned instruction bytes changed at {rva:08x}")


# ------------------------------------------------------------- 2/3. codegen

def seam_lines(gen_all, spec) -> tuple[list[str], list[str]]:
    declaration = list(gen_all.render_vita_kage_mutex_seam_declaration(spec))
    seam = list(gen_all.render_vita_kage_mutex_seam(spec))
    # gpr_locals.bracket_seam_regions() wraps the c-> spelled statement.
    bracketed = seam[:2] + ["    GUEST_GPR_FLUSH(c);", seam[2],
                            "    GUEST_GPR_RELOAD(c);", seam[3]]
    return declaration, bracketed


def strip_seam(text: str, declaration: list[str], bracketed: list[str]) -> str:
    for block in (declaration, bracketed):
        joined = "\n".join(block) + "\n"
        require(text.count(joined) == 1, "seam block is not present exactly once")
        text = text.replace(joined, "")
    return text


def verify_codegen(pe_path: Path) -> dict[int, str]:
    os.environ["REPENTOGXM_PE"] = str(pe_path)
    sys.path.insert(0, str(HERE))
    import gen_all  # noqa: E402
    import gpr_locals  # noqa: E402
    from image import DEFAULT_BASE, Image  # noqa: E402

    require(set(gen_all.VITA_KAGE_MUTEX_SEAM_SPECS) == set(ROOTS),
            "seam roots changed")
    pin = Image(str(pe_path), DEFAULT_BASE)
    for root, end in ((LOCK_ROOT, LOCK_END), (UNLOCK_ROOT, UNLOCK_END)):
        owner = gen_all.VITA_REFCOUNT_SYNC_IAT_ROOT_SPECS[root]
        require(owner["end"] == end and
                hashlib.sha256(bytes(pin.code_at(root, end - root))).hexdigest()
                == owner["body_sha256"],
                f"relocated body pin changed at {root:08x}")
    vita_text: dict[int, str] = {}
    for base in (DEFAULT_BASE, 0x98000000):
        image = Image(str(pe_path), base)
        for root, (name, helper, coverage, marker, first, sites) in ROOTS.items():
            spec = gen_all.VITA_KAGE_MUTEX_SEAM_SPECS[root]
            require(spec["helper"] == helper and spec["coverage_id"] == coverage
                    and spec["marker"].startswith(marker), "seam spec drifted")
            result = gen_all._translate_function(
                image, {"rva": root}, None, {}, pin_img=pin)
            require(result["stub"] is None,
                    f"{name} became a stub at base {base:#x}")
            require(result["vita_kage_mutex_seam"] is True,
                    f"seam proof was not selected for {name} at base {base:#x}")
            text = result["text"]
            declaration, bracketed = seam_lines(gen_all, spec)
            require(text.count(marker) == 1, f"{name}: seam marker count changed")
            require(text.count(helper + "(") == 2,
                    f"{name}: helper declaration/call count changed")
            require(text.count(FENCE) == 2, f"{name}: seam fence count changed")
            # Exact placement: declaration after the register file, seam
            # ahead of the first translated instruction (the unit writer
            # inserts the coverage note between them; see verify_corpus).
            prologue = "\n".join([
                "    GUEST_FLAGS_DECL;",
                "    GUEST_GPR_DECL;",
                *declaration,
                *bracketed,
                f"    {first}",
            ]) + "\n"
            require(prologue in text, f"{name}: seam placement changed")
            require(bracketed[3] == f"    if ({helper}(c)) return;",
                    f"{name}: seam statement shape changed")
            # The complete translated body stays behind the seam.
            require(text.count("guest_try_direct_sync_import_call(") == sites,
                    f"{name}: authenticated sync-IAT sites changed")
            require("GUEST_GPR_FLUSH(c); return;" in text,
                    f"{name}: translated return disappeared")
            # The seam is the only textual change, and unit packing (measured
            # on legacy_text plus the worker's legacy_size_delta) is exactly
            # what the render without the seam table produces.
            plain = strip_seam(text, declaration, bracketed)
            saved_specs = gen_all.VITA_KAGE_MUTEX_SEAM_SPECS
            try:
                gen_all.VITA_KAGE_MUTEX_SEAM_SPECS = {}
                reference = gen_all._translate_function(
                    image, {"rva": root}, None, {}, pin_img=pin)
            finally:
                gen_all.VITA_KAGE_MUTEX_SEAM_SPECS = saved_specs
            require(reference["vita_kage_mutex_seam"] is False and
                    reference["text"] == plain,
                    f"{name}: the seam is not the only rendered change")
            require(gen_all.unit_packing_size(text) + result["legacy_size_delta"]
                    == gen_all.unit_packing_size(reference["text"]) +
                    reference["legacy_size_delta"],
                    f"{name}: unit packing size changed")
            require(gen_all.vita_kage_mutex_seam_packing_bytes(spec) ==
                    len(gpr_locals.legacy_text(text)) -
                    len(gpr_locals.legacy_text(plain)),
                    f"{name}: packing delta does not equal the seam text")
            if base == 0x98000000:
                vita_text[root] = text

    # The corpus spelling of the same bodies: gen_all main renders every
    # IAT site through GUEST_IMPORT_CALL(target, slot RVA, dense import ID)
    # (the slot-RVA order of the PE import directory, the order guest_table.c
    # and host_vita_import_id_map.inc freeze) and adds the coverage note.
    # legacy_text maps the tokens back, so apart from the note the corpus
    # body is exactly the plain render checked above.
    import iat_meta  # noqa: E402
    imports = iat_meta.read_imports(str(pe_path))
    import_ids = {slot: index for index, (slot, _name) in enumerate(imports)}
    require(import_ids.get(0x6060FC) == 61 and import_ids.get(0x6060F8) == 60,
            "sync import IDs moved; the seam's ISAAC_VITA_IMPORT_ID_SYNC_* "
            "census keys would drift")
    saved_ids = gen_all._WORKER_IMPORT_IDS
    gen_all._WORKER_IMPORT_IDS = import_ids
    try:
        image = Image(str(pe_path), 0x98000000)
        for root, (name, _helper, coverage, marker, _first, _sites) in ROOTS.items():
            result = gen_all._translate_function(
                image, {"rva": root}, None, {},
                function_coverage_ids={root: coverage}, pin_img=pin)
            text = result["text"]
            note = f"    guest_coverage_function({coverage}U);\n"
            require(result["vita_kage_mutex_seam"] is True and
                    text.count(marker) == 1 and text.count(note) == 1 and
                    text.count("GUEST_IMPORT_CALL(") == IMPORT_SITES[root] and
                    text.count("guest_call(c, _target)") ==
                    vita_text[root].count("guest_call(c, _target)") -
                    IMPORT_SITES[root],
                    f"{name}: corpus spelling changed (the tokens must replace "
                    "exactly the IAT-slot calls; the sync-site fallbacks and "
                    "register-indirect calls keep guest_call)")
            require(gpr_locals.legacy_text(text.replace(note, "", 1)) ==
                    gpr_locals.legacy_text(vita_text[root]),
                    f"{name}: corpus spelling differs from the plain render "
                    "beyond the import tokens and the coverage note")
            vita_text[root] = text
    finally:
        gen_all._WORKER_IMPORT_IDS = saved_ids

    # Hostile authenticator cases on the spec table itself.
    class Insn:
        def __init__(self, mnemonic, op_str):
            self.mnemonic = mnemonic
            self.op_str = op_str

    for root, (name, _helper, _coverage, _marker, _first, _sites) in ROOTS.items():
        spec = gen_all.VITA_KAGE_MUTEX_SEAM_SPECS[root]
        owner = gen_all.VITA_REFCOUNT_SYNC_IAT_ROOT_SPECS[root]
        insns = {rva: Insn(*want) for rva, want in spec["required"].items()}
        order = list(range(owner["insns"]))
        sites = tuple({"site": site} for site, _slot, _name in owner["sites"])
        require(gen_all.vita_kage_mutex_seam_for_body(
            pin, root, insns, order, sites) is spec, f"{name}: authenticator")
        require(gen_all.vita_kage_mutex_seam_for_body(
            pin, root + 0x10, insns, order, sites) is None,
            "unrelated root selected the seam")
        drifted = dict(insns)
        drifted[root] = Insn("push", "eax")
        must_fail(f"{name}: drifted instruction",
                  lambda: gen_all.vita_kage_mutex_seam_for_body(
                      pin, root, drifted, order, sites),
                  "ABI/algorithm changed")
        missing = dict(insns)
        del missing[max(missing)]
        must_fail(f"{name}: missing instruction",
                  lambda: gen_all.vita_kage_mutex_seam_for_body(
                      pin, root, missing, order, sites),
                  "ABI/algorithm changed")
        must_fail(f"{name}: sync-IAT site not selected",
                  lambda: gen_all.vita_kage_mutex_seam_for_body(
                      pin, root, insns, order, sites[:-1]),
                  "requires the authenticated sync-IAT sites")
        must_fail(f"{name}: CFG order length",
                  lambda: gen_all.vita_kage_mutex_seam_for_body(
                      pin, root, insns, order[:-1], sites),
                  "requires the authenticated sync-IAT sites")
    return vita_text


# ------------------------------------------- 4. object predicate identity

def predicate_conditions(text: str, function: str) -> list[str]:
    start = text.index(function)
    body = text[start:text.index("\n}\n", start)]
    conditions = re.findall(r"if \((.*?)\)\n\s+return 0U?;", body, re.DOTALL)
    return [" ".join(condition.split()) for condition in conditions]


def verify_predicate_copy() -> None:
    header = (RUNTIME / "host_vita_sync_fastpath.h").read_text(encoding="utf-8")
    seam = (RUNTIME / "host_vita_kage_mutex_seam.c").read_text(encoding="utf-8")
    inline = predicate_conditions(
        header, "static inline uint32_t isaac_vita_sync_inline_cs_address")
    copy = predicate_conditions(seam, "static inline int kage_mutex_cs_object")
    require(len(copy) == 4, "seam object predicate lost a compare")
    require(copy == inline[-4:],
            f"seam object predicate differs from the inline path: {copy} vs "
            f"{inline[-4:]}")
    require("isaac_vita_sync_inline_cs_address(" not in seam and
            "isaac_vita_sync_inline_enter(" not in seam and
            "isaac_vita_sync_inline_leave(" not in seam and
            "isaac_vita_sync_inline_return(" not in seam,
            "seam must not route through the stack-argument inline helpers")
    require("g_isaac_vita_sync_inline_owner" in seam and
            "exactly that of the inline critical-section fast path" in
            (RUNTIME / "host_vita_kage_mutex_seam.h").read_text(encoding="utf-8"),
            "seam thread-safety statement/latch check missing")


# --------------------------------------------------- 5. CMake + raw gate

def verify_cmake() -> None:
    cmake = (VITA / "CMakeLists.txt").read_text(encoding="utf-8")
    require(re.search(
        rf"option\({OPTION}\s+\"[^\"]+\"\s+OFF\)", cmake, re.DOTALL) is not None,
        "KAGE mutex seam is not default OFF")
    require(f"if({OPTION} AND NOT ISAAC_VITA_SYNC_INLINE_FASTPATH)" in cmake,
            "seam does not require ISAAC_VITA_SYNC_INLINE_FASTPATH")
    require(f"if({OPTION} AND ISAAC_VITA_SCAFFOLD_ONLY)" in cmake,
            "seam does not require the generated runtime")
    block = re.search(
        rf"if\({OPTION}\)\n(.*?)\n  endif\(\)\n  if\(ISAAC_VITA_[A-Z0-9_]+\)",
        cmake, re.DOTALL)
    require(block is not None, "seam CMake census block missing")
    body = block.group(1)
    for root in ("sub_00562e00", "sub_00562ec0"):
        require(
            f'REGEX "^void {root}\\\\(CPU \\\\*__restrict c\\\\)$"' in body,
            f"census does not match the {root} definition")
    require(body.count("LIMIT_COUNT 2") == 4, "census must detect duplicates")
    require('REGEX "KAGE Mutex::Lock\\\\(-1\\\\) native critical-section seam"'
            in body and
            'REGEX "KAGE Mutex::Unlock native critical-section seam"' in body,
            "census markers changed")
    require("NOT ISAAC_VITA_KAGE_MUTEX_SEAM_DEFINITIONS EQUAL 2 OR" in body and
            "NOT ISAAC_VITA_KAGE_MUTEX_SEAM_SEAMS EQUAL 2" in body,
            "census totals changed")
    require(cmake.count(f"{OPTION}=1)") == 1,
            "seam compile definition must be applied exactly once")
    scope = re.search(
        r"set_property\(SOURCE\s+\$\{ISAAC_VITA_KAGE_MUTEX_SEAM_OWNER\}\s+"
        r'"\$\{ISAAC_RUNTIME\}/host_vita_kage_mutex_seam\.c"\s+'
        r"APPEND PROPERTY COMPILE_DEFINITIONS\s+" + OPTION + r"=1\)", body)
    require(scope is not None, "seam definition scope changed")
    require('"${ISAAC_RUNTIME}/host_vita_kage_mutex_seam.c")' in body,
            "seam TU is not added to the runtime sources")
    for definition in ("ISAAC_VITA_SYNC_INLINE_FASTPATH=1",
                       "ISAAC_VITA_PHASE_PROFILE=1",
                       "ISAAC_VITA_PROFILE_IMPORT_KINDS=1",
                       "ISAAC_VITA_GUEST_SAMPLER=1",
                       "ISAAC_VITA_GUEST_DISPATCH_TABLE=1",
                       "GUEST_GENERATED_STACK_GUARD=0"):
        require(definition in body,
                f"seam TU does not mirror guest.c's {definition}")


def verify_raw_gate(root: Path) -> None:
    import vita_raw_allocator_gate as gate  # noqa: E402

    generated = root / "generated"
    generated.mkdir(parents=True)
    owner = generated / "guest_0166.c"
    other = generated / "guest_0001.c"
    seam_tu = root / "host_vita_kage_mutex_seam.c"
    owner_text = "\n".join([
        "void sub_00562e00(CPU *__restrict c)",
        "    /* KAGE Mutex::Lock(-1) native critical-section seam; exact "
        "translated fallback follows. */",
        "void sub_00562ec0(CPU *__restrict c)",
        "    /* KAGE Mutex::Unlock native critical-section seam; exact "
        "translated fallback follows. */",
    ]) + "\n"
    owner.write_text(owner_text, encoding="utf-8")
    other.write_text("void sub_00000000(CPU *__restrict c)\n", encoding="utf-8")
    seam_tu.write_text("", encoding="utf-8")

    def records(owner_defs, other_defs, seam_defs, undef=None):
        out = {
            owner: {"source": owner, "definitions": dict(owner_defs)},
            other: {"source": other, "definitions": dict(other_defs)},
            seam_tu: {"source": seam_tu, "definitions": dict(seam_defs)},
        }
        if undef:
            out[owner]["undefinitions"] = list(undef)
        return out

    off = {OPTION: "OFF"}
    on = {OPTION: "ON"}
    gate.verify_kage_mutex_seam_compile_scope(records({}, {}, {}), off)
    gate.verify_kage_mutex_seam_compile_scope(
        records({OPTION: ["1"]}, {}, {OPTION: ["1"]}), on)
    must_fail("definition while OFF",
              lambda: gate.verify_kage_mutex_seam_compile_scope(
                  records({OPTION: ["1"]}, {}, {OPTION: ["1"]}), off),
              "compile-definition scope changed", (gate.GateError,))
    must_fail("owner without definition",
              lambda: gate.verify_kage_mutex_seam_compile_scope(
                  records({}, {}, {OPTION: ["1"]}), on),
              "compile-definition scope changed", (gate.GateError,))
    must_fail("seam TU without definition",
              lambda: gate.verify_kage_mutex_seam_compile_scope(
                  records({OPTION: ["1"]}, {}, {}), on),
              "compile-definition scope changed", (gate.GateError,))
    must_fail("definition leaked to another unit",
              lambda: gate.verify_kage_mutex_seam_compile_scope(
                  records({OPTION: ["1"]}, {OPTION: ["1"]}, {OPTION: ["1"]}), on),
              "compile-definition scope changed", (gate.GateError,))
    must_fail("non-canonical value",
              lambda: gate.verify_kage_mutex_seam_compile_scope(
                  records({OPTION: ["0"]}, {}, {OPTION: ["1"]}), on),
              "non-canonical", (gate.GateError,))
    must_fail("policy undefined",
              lambda: gate.verify_kage_mutex_seam_compile_scope(
                  records({OPTION: ["1"]}, {}, {OPTION: ["1"]}, [OPTION]), on),
              "policy undefined", (gate.GateError,))
    owner.write_text(owner_text + owner_text.splitlines()[1] + "\n",
                     encoding="utf-8")
    must_fail("duplicate seam in the owner",
              lambda: gate.verify_kage_mutex_seam_compile_scope(
                  records({OPTION: ["1"]}, {}, {OPTION: ["1"]}), on),
              "missing or duplicate seam", (gate.GateError,))
    owner.write_text(owner_text, encoding="utf-8")
    other.write_text(owner_text.splitlines()[1] + "\n", encoding="utf-8")
    must_fail("seam escaped into another unit",
              lambda: gate.verify_kage_mutex_seam_compile_scope(
                  records({OPTION: ["1"]}, {}, {OPTION: ["1"]}), on),
              "missing or duplicate seam", (gate.GateError,))
    other.write_text("void sub_00000000(CPU *__restrict c)\n", encoding="utf-8")
    owner.write_text("\n".join(owner_text.splitlines()[:2]) + "\n",
                     encoding="utf-8")
    must_fail("second root missing",
              lambda: gate.verify_kage_mutex_seam_compile_scope(
                  records({OPTION: ["1"]}, {}, {OPTION: ["1"]}), on),
              "census changed", (gate.GateError,))
    owner.write_text(owner_text, encoding="utf-8")
    # The source-inventory side (seam TU only with the option, and only with
    # ISAAC_VITA_SYNC_INLINE_FASTPATH=ON) is covered by
    # vita/test_vita_raw_allocator_gate.py expected_source_tests.
    source = (VITA / "vita_raw_allocator_gate.py").read_text(encoding="utf-8")
    require('runtime.add("host_vita_kage_mutex_seam.c")' in source and
            f'f"{{KAGE_MUTEX_SEAM_CACHE_KEY}} requires "\n'
            '                "ISAAC_VITA_SYNC_INLINE_FASTPATH=ON"' in source,
            "raw gate source inventory lost the seam TU or its requirement")


# ---------------------------------------------------- 6. corpus census

def unit_files(directory: Path) -> list[Path]:
    return sorted(path for path in directory.glob("guest_[0-9][0-9][0-9][0-9].c"))


def verify_corpus(generated: Path, baseline: Path | None,
                  vita_text: dict[int, str]) -> str:
    sys.path.insert(0, str(HERE))
    import gen_all  # noqa: E402

    owners: list[Path] = []
    for path in unit_files(generated):
        text = path.read_text(encoding="utf-8")
        roots = sum(text.count(f"void {name}(CPU *__restrict c)\n")
                    for name, *_rest in ROOTS.values())
        markers = sum(text.count(marker) for _n, _h, _c, marker, *_r in ROOTS.values())
        helpers = sum(text.count(helper + "(") for _n, helper, *_r in ROOTS.values())
        if roots:
            require(roots == 2 and markers == 2 and helpers == 4 and
                    text.count(FENCE) == 4,
                    f"{path.name}: owner census roots={roots} markers={markers} "
                    f"helpers={helpers}")
            owners.append(path)
        else:
            require(markers == 0 and helpers == 0 and FENCE not in text,
                    f"{path.name}: seam escaped its owner")
    require(len(owners) == 1, f"expected one owner unit, found {owners}")
    owner = owners[0]
    owner_text = owner.read_text(encoding="utf-8")
    for root, text in vita_text.items():
        # The corpus-spelled render (header comment, coverage note, import
        # tokens, seam) is the owner's body, verbatim and once.
        require(owner_text.count(text) == 1,
                f"{ROOTS[root][0]}: corpus body differs from the fresh render")
    if baseline is not None:
        base_units = unit_files(baseline)
        new_units = unit_files(generated)
        require([p.name for p in base_units] == [p.name for p in new_units],
                "unit set changed")
        identical = 0
        other_seams = 0
        for base_path, new_path in zip(base_units, new_units):
            if new_path.name == owner.name:
                continue
            if base_path.read_bytes() == new_path.read_bytes():
                identical += 1
                continue
            # Other seams may be on in the regenerated corpus: the floor
            # thunk fast path (generation-time switch
            # GUEST_FLOOR_THUNK_FASTPATH=1, guest_0184.c) and the shader
            # attrib fast path (unconditional, guest_0167.c).  Their text is
            # fenced and must strip back to the baseline exactly, so nothing
            # but declared seams ever differs.
            unfenced = re.sub(
                r"#if defined\(__vita__\) && "
                r"defined\(ISAAC_VITA_(?:FLOOR_THUNK_FASTPATH|"
                r"SHADER_ATTRIB_FASTPATH)\)\n(?:.*\n)*?#endif\n",
                "", new_path.read_text(encoding="utf-8"))
            require(unfenced == base_path.read_text(encoding="utf-8"),
                    f"{new_path.name} differs from the baseline corpus")
            other_seams += 1
        stripped = owner_text
        for root in ROOTS:
            declaration, bracketed = seam_lines(
                gen_all, gen_all.VITA_KAGE_MUTEX_SEAM_SPECS[root])
            stripped = strip_seam(stripped, declaration, bracketed)
        require(stripped == (baseline / owner.name).read_text(encoding="utf-8"),
                f"{owner.name} differs from the baseline by more than the seam")
        print(f"corpus identity: {identical} units byte-identical to the "
              f"baseline; {owner.name} differs only by the "
              f"{sum(len(seam_lines(gen_all, s)[0]) + len(seam_lines(gen_all, s)[1]) for s in gen_all.VITA_KAGE_MUTEX_SEAM_SPECS.values())} seam lines; "
              f"{other_seams} unit(s) differ only by other fenced seams")
    return owner.name


# ------------------------------------------------------- 7. host oracle

def function_lines(text: str, name: str) -> list[str]:
    lines = text.splitlines()
    start = lines.index(f"void {name}(CPU *__restrict c)")
    if start and lines[start - 1].startswith(f"/* {name} "):
        start -= 1
    end = start
    while lines[end] != "}":
        end += 1
    return lines[start:end + 1]


def resolve_preprocessor(lines: list[str], defines: set[str]) -> list[str]:
    """Keep the `#if defined(__vita__)` branches (the corpus as compiled for
    the Vita); the seam fence itself is dropped: the oracle applies the seam
    and needs the plain translated body as its reference."""
    out: list[str] = []
    stack: list[tuple[bool, bool]] = []
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("#if "):
            expression = re.sub(r"defined\((\w+)\)",
                                lambda m: str(m.group(1) in defines),
                                stripped[4:])
            value = bool(eval(expression.replace("&&", " and ")  # noqa: S307
                              .replace("||", " or "), {"__builtins__": {}}))
            parent = all(taking for taking, _ in stack)
            stack.append((parent and value, value))
            continue
        if stripped == "#else":
            _taking, taken = stack.pop()
            parent = all(taking for taking, _ in stack)
            stack.append((parent and not taken, True))
            continue
        if stripped == "#endif":
            stack.pop()
            continue
        require(not stripped.startswith("#"), f"unexpected directive: {line}")
        if all(taking for taking, _ in stack):
            out.append(line)
    require(not stack, "unbalanced preprocessor block in a body")
    return out


def write_bodies(texts: dict[int, str], path: Path) -> None:
    out = ["/* generated by test_vita_kage_mutex_seam.py: the translated "
           "bodies with the Vita sync-IAT sites selected and the seam fence "
           "dropped */"]
    for root, text in sorted(texts.items()):
        body = resolve_preprocessor(function_lines(text, ROOTS[root][0]),
                                    {"__vita__"})
        require(not any(ROOTS[root][1] in line for line in body),
                "seam helper survived the fence drop")
        out.extend(body)
        out.append("")
    path.write_text("\n".join(out) + "\n", encoding="utf-8")


def run_oracle(cc: str, vitasdk_include: Path | None, bodies: dict[int, str],
               root: Path, steps: int) -> list[str]:
    include = root / "inc"
    include.mkdir(parents=True)
    write_bodies(bodies, include / "kage_mutex_seam_bodies.inc")
    common = [
        "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", str(RUNTIME), "-I", str(include),
        "-DGUEST_IMAGE_BASE=0x98000000u", "-DGUEST_STACK_REQUIRED=1",
        "-DISAAC_VITA_SYNC_INLINE_FASTPATH=1", f"-D{OPTION}=1",
        "-DISAAC_VITA_PHASE_PROFILE=1", "-DISAAC_VITA_GUEST_DISPATCH_TABLE=1",
        "-DISAAC_VITA_PROFILE_IMPORT_KINDS=1", "-DISAAC_VITA_GUEST_SAMPLER=1",
        "-DGUEST_GPR_LOCAL=1", "-DGUEST_FLAGS_LOCAL=1", "-DGUEST_COVERAGE_HOOKS=1",
    ]
    if vitasdk_include is not None:
        common += ["-idirafter", str(vitasdk_include)]
    if os.name != "nt":
        common.append("-no-pie")
    sources = [str(RUNTIME / name) for name in (
        "vita_sync_services.c", "host_vita_sync.c",
        "host_vita_kage_mutex_seam.c", "host_vita_kage_mutex_seam_oracle.c")]
    results = []
    for guard in (0, 1):
        exe = root / f"oracle-g{guard}{'.exe' if os.name == 'nt' else ''}"
        run([cc, *common, f"-DGUEST_GENERATED_STACK_GUARD={guard}", *sources,
             "-o", str(exe)])
        for seed in ("", "0x1234567", "0xdeadbeef"):
            command = [str(exe), str(steps)] + ([seed] if seed else [])
            output = run(command)
            line = [l for l in output.splitlines() if l.startswith(ORACLE_PASS)]
            require(len(line) == 1, f"oracle did not pass: {output}")
            require(f"stack_guard={guard} steps={steps}" in line[0],
                    "oracle ran a different configuration")
            results.append(line[0])
    return results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=Path)
    parser.add_argument("--generated-dir", type=Path)
    parser.add_argument("--baseline-generated-dir", type=Path)
    parser.add_argument("--cc", help="GNU-compatible host compiler for the oracle")
    parser.add_argument("--vitasdk-include", type=Path,
                        help="directory holding psp2/ (the oracle's sync mocks)")
    parser.add_argument("--steps", type=int, default=200000)
    arguments = parser.parse_args()

    verify_pe(arguments.pe)
    vita_text = verify_codegen(arguments.pe)
    verify_predicate_copy()
    verify_cmake()
    with tempfile.TemporaryDirectory(prefix="isaac-kage-mutex-") as value:
        root = Path(value)
        verify_raw_gate(root / "gate")
        owner = "<not checked>"
        if arguments.generated_dir is not None:
            owner = verify_corpus(arguments.generated_dir,
                                  arguments.baseline_generated_dir, vita_text)
        oracle = []
        if arguments.cc:
            oracle = run_oracle(arguments.cc, arguments.vitasdk_include,
                                vita_text, root / "oracle", arguments.steps)
            for line in oracle:
                print(line)
        else:
            print("note: --cc not given, host oracle skipped")
    print("Vita KAGE mutex seam: PASS; roots=2 seams=2 "
          f"owner={owner} bases=2 hostile-authenticator=10 hostile-gate=9 "
          f"oracle-runs={len(oracle)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
