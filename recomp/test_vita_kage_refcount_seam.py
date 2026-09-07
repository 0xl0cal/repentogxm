#!/usr/bin/env python3
"""Prove the KAGE ReferenceCount AddRef / weak-lock / Release native seam
(ISAAC_VITA_KAGE_REFCOUNT_SEAM).

Modelled on test_vita_kage_mutex_seam.py:

  1. frozen PE pins (file size/sha, the three helper bodies byte for byte,
     the instructions the seam replays or refuses);
  2. gen_all renders the three roots at both image bases with exactly one
     fenced seam statement each (the refcount fence, not the mutex fence),
     placed between the coverage note and the first translated instruction,
     bracketed for the GPR locals, with the complete translated body (its
     authenticated direct edges) still behind it, and with a unit-packing
     size unchanged by the seam text; the mutex wrappers keep their own seam;
  3. hostile authenticator cases (drifted instruction, missing instruction,
     missing direct edge, CFG order, unrelated root);
  4. the seam's six predicate copies equal the mutex seam's text and the
     object predicate equals the inline fast path's; the TU refuses
     GUEST_FLAGS_LOCAL=0; the VERIFY run-scope layout mirrors guest.c; the
     receipt formats the oracle asserts are the TU's literals;
  5. CMake option defaults OFF, the five fatals, the three-root census in one
     owner, the TU's mirrored defines, and the scope of the census owner
     definitions (ISAAC_VITA_PHASE_PROFILE / ISAAC_VITA_GUEST_DISPATCH_TABLE
     reach runtime TUs only, never a generated unit -- the reason the seam
     replays no direct-edge census); the raw allocator gate's compile-scope
     check with hostile records;
  6. optional --generated-dir: corpus census (one owner with three roots and
     three seams, nothing elsewhere) and, with --baseline-generated-dir, the
     proof that the regenerated corpus differs from the baseline only by the
     seam lines of the owner unit (other fenced seams stripped);
  7. optional --cc: the host differential oracle
     (host_vita_kage_refcount_seam_oracle.c) against the ACTUAL translated
     bodies plus the two mutex wrappers with their seam -- compiled as a
     translation unit of their own with the owner unit's define set (no
     ISAAC_VITA_PHASE_PROFILE / GUEST_DISPATCH_TABLE, GUEST_COVERAGE_HOOKS=0,
     as CMake compiles guest_0000.c) -- both synthetic-stack modes x both
     GPR spellings, the VERIFY leg, three seeds.  Needs a
     GNU-compatible compiler (clang -m32 on Windows, gcc on the box) with the
     psp2 headers (--vitasdk-include) and mmap/VirtualAlloc below 4 GiB.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shlex
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
RELEASE_ROOT = 0x00007AF0
ADDREF_ROOT = 0x00007B50
WEAKLOCK_ROOT = 0x00007B70
LOCK_ROOT = 0x00562E00
UNLOCK_ROOT = 0x00562EC0
ROOTS = {
    RELEASE_ROOT: {
        "name": "sub_00007af0", "end": 0x00007B49, "insns": 37,
        "sha256": "a8418cba05d279b8c2815bd4f2cea42b0a83ef87d7202944b867ab09616fede2",
        "helper": "isaac_vita_kage_refcount_release_try", "coverage": 259,
        "marker": "KAGE ReferenceCount::Release native seam",
        "first": "/* 00007af0  push esi */",
        "edges": ((0x00007AFC, LOCK_ROOT), (0x00007B18, UNLOCK_ROOT),
                  (0x00007B41, UNLOCK_ROOT)),
    },
    ADDREF_ROOT: {
        "name": "sub_00007b50", "end": 0x00007B6E, "insns": 13,
        "sha256": "de9f62b6e3714fb24142eb04f39293b30e655c5a72f5f447cba8b0f35824e2da",
        "helper": "isaac_vita_kage_refcount_addref_try", "coverage": 260,
        "marker": "KAGE ReferenceCount::AddRef native seam",
        "first": "/* 00007b50  push esi */",
        "edges": ((0x00007B5C, LOCK_ROOT), (0x00007B6B, UNLOCK_ROOT)),
    },
    WEAKLOCK_ROOT: {
        "name": "sub_00007b70", "end": 0x00007BA4, "insns": 25,
        "sha256": "cae50e6a76c558eb80735da42e211a5f8b42566571c61333523723f70745c7f0",
        "helper": "isaac_vita_kage_refcount_weaklock_try", "coverage": 261,
        "marker": "KAGE ReferenceCount weak-lock native seam",
        "first": "/* 00007b70  push esi */",
        "edges": ((0x00007B7C, LOCK_ROOT), (0x00007B8F, UNLOCK_ROOT),
                  (0x00007B96, UNLOCK_ROOT), (0x00007B9C, ADDREF_ROOT)),
    },
}
MUTEX_ROOTS = {LOCK_ROOT: ("sub_00562e00", 7968), UNLOCK_ROOT: ("sub_00562ec0", 7969)}
FENCE = "#if defined(__vita__) && defined(ISAAC_VITA_KAGE_REFCOUNT_SEAM)"
MUTEX_FENCE = "#if defined(__vita__) && defined(ISAAC_VITA_KAGE_MUTEX_SEAM)"
OPTION = "ISAAC_VITA_KAGE_REFCOUNT_SEAM"
VERIFY_OPTION = "ISAAC_VITA_KAGE_REFCOUNT_SEAM_VERIFY"
MUTEX_OPTION = "ISAAC_VITA_KAGE_MUTEX_SEAM"
SEAM_TU = RUNTIME / "host_vita_kage_refcount_seam.c"
SEAM_HEADER = RUNTIME / "host_vita_kage_refcount_seam.h"
MUTEX_TU = RUNTIME / "host_vita_kage_mutex_seam.c"
ORACLE_TU = RUNTIME / "host_vita_kage_refcount_seam_oracle.c"
ORACLE_PASS = "Vita KAGE refcount seam oracle: PASS"
# The corpus as compiled for the Vita production build: a generated unit
# never carries ISAAC_VITA_PHASE_PROFILE (verify_cmake pins the scope), so
# the fenced predicted-edge census hook (guest_phase_profile_note_lookup_
# cache_hit) is compiled OUT of the bodies and their NOTE_CALL / NOTE_LOOKUP
# macros are no-ops -- the bodies TU is compiled without the define too.
CORPUS_DEFINES = {"__vita__"}
# guest_0000.c / guest_0166.c as recomp/vita/CMakeLists.txt compiles them,
# minus GUEST_IMPORT_DIRECT (the wrappers' non-sync import sites lie on
# rejection-only paths and must escape through guest_call here) and the
# SSE/fs-base knobs these bodies never touch; plus the target-wide
# ISAAC_VITA_PROFILE_FUNCTION_ENTRIES of a counting build.
OWNER_UNIT_DEFINES = (
    "-DGUEST_IMAGE_BASE=0x98000000u", "-DGUEST_STACK_REQUIRED=1",
    "-DGUEST_FLAGS_LOCAL=1", "-DGUEST_COVERAGE_HOOKS=0",
    "-DISAAC_VITA_PROFILE_FUNCTION_ENTRIES=1",
)
PREDICATE_COPIES = (
    "static inline int kage_mutex_cs_object(",
    "static inline int kage_mutex_frame_inside_stack(",
    "static inline int kage_mutex_ranges_overlap(",
    "static inline void kage_mutex_note_low_water(",
    "static inline void kage_mutex_note_import(",
    "static inline int kage_mutex_sync_slot_ready(",
)
REASON_NAMES = ("handled", "latch", "esp", "frame", "slot", "init", "vt", "cs",
                "alias", "owner", "busy", "zero", "last")
FN_NAMES = ("AddRef", "weak-lock", "Release")


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


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


# ---------------------------------------------------------------- 1. PE pins

def verify_pe(pe_path: Path) -> None:
    require(pe_path.stat().st_size == PE_SIZE, "frozen PE size changed")
    require(digest(pe_path) == PE_SHA256, "frozen PE hash changed")
    from image import DEFAULT_BASE, Image  # noqa: E402

    image = Image(str(pe_path), Image(str(pe_path), DEFAULT_BASE).orig_base)
    require(image.orig_base == 0x400000, "PE ImageBase changed")
    for root, spec in ROOTS.items():
        body = bytes(image.code_at(root, spec["end"] - root))
        require(hashlib.sha256(body).hexdigest() == spec["sha256"],
                f"{spec['name']} body bytes changed")
    # The instructions the seam replays (frame pushes, the count update, the
    # returns) and the ones it refuses (the destroy path), as bytes.
    pinned = {
        0x00007AF4: "6aff",            # push -1 (Lock's timeout)
        0x00007AFC: "ff500c",          # call [eax+0xc]  Mutex::Lock
        0x00007AFF: "0fb74604",        # movzx eax, word [esi+4]
        0x00007B09: "66894604",        # mov word [esi+4], ax
        0x00007B10: "7529",            # jne 0x7b3b (count != 0 after dec)
        0x00007B18: "ff5010",          # call [eax+0x10] Unlock (destroy path)
        0x00007B26: "ff5060",          # call [eax+0x60] T destructor
        0x00007B36: "8b4014",          # mov eax, [eax+0x14] ReleaseWeak
        0x00007B39: "ffe0",            # jmp eax
        0x00007B41: "ff5010",          # call [eax+0x10] Unlock (common tail)
        0x00007B45: "b001",            # mov al, 1
        0x00007B48: "c3",              # ret
        0x00007B54: "6aff",            # push -1
        0x00007B5C: "ff500c",          # call [eax+0xc]  Lock
        0x00007B5F: "66ff4604",        # inc word [esi+4]
        0x00007B6B: "ff6010",          # jmp [eax+0x10] Unlock (tail)
        0x00007B74: "6aff",            # push -1
        0x00007B7C: "ff500c",          # call [eax+0xc]  Lock
        0x00007B85: "83c010",          # add eax, 0x10
        0x00007B88: "66837f0400",      # cmp word [edi+4], 0
        0x00007B8D: "7507",            # jne 0x7b96 (alive)
        0x00007B8F: "ff10",            # call [eax] Unlock (dead)
        0x00007B92: "32c0",            # xor al, al
        0x00007B96: "ff10",            # call [eax] Unlock (alive)
        0x00007B9C: "ff5008",          # call [eax+8] AddRef
        0x00007BA0: "b001",            # mov al, 1
        0x00007BA3: "c3",              # ret
    }
    for rva, expected in pinned.items():
        require(bytes(image.code_at(rva, len(expected) // 2)).hex() == expected,
                f"pinned instruction bytes changed at {rva:08x}")


# ------------------------------------------------------------- 2/3. codegen

def seam_lines(gen_all, spec) -> tuple[list[str], list[str]]:
    declaration = list(gen_all.render_vita_kage_mutex_seam_declaration(spec))
    seam = list(gen_all.render_vita_kage_mutex_seam(spec))
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

    require(set(gen_all.VITA_KAGE_REFCOUNT_SEAM_SPECS) == set(ROOTS),
            "seam roots changed")
    require(gen_all.VITA_KAGE_REFCOUNT_SEAM_FENCE == FENCE, "seam fence changed")
    pin = Image(str(pe_path), DEFAULT_BASE)
    for root, spec in ROOTS.items():
        owner = gen_all.VITA_REFCOUNT_DIRECT_EDGE_ROOT_SPECS[root]
        require(owner["end"] == spec["end"] and owner["insns"] == spec["insns"] and
                owner["body_sha256"] == spec["sha256"] and
                hashlib.sha256(bytes(pin.code_at(root, spec["end"] - root)))
                .hexdigest() == spec["sha256"],
                f"direct-edge owner pin changed at {root:08x}")
        require(tuple((site, target) for site, target, *_r in owner["edges"])
                == spec["edges"], f"direct edges changed at {root:08x}")
        seam = gen_all.VITA_KAGE_REFCOUNT_SEAM_SPECS[root]
        require(seam["helper"] == spec["helper"] and
                seam["coverage_id"] == spec["coverage"] and
                seam["marker"].startswith(spec["marker"]) and
                seam["fence"] == FENCE and
                len(seam["required"]) == spec["insns"] and
                min(seam["required"]) == root and
                max(seam["required"]) < spec["end"],
                f"seam spec drifted at {root:08x}")
    vita_text: dict[int, str] = {}
    for base in (DEFAULT_BASE, 0x98000000):
        image = Image(str(pe_path), base)
        for root, spec in ROOTS.items():
            name, helper, marker = spec["name"], spec["helper"], spec["marker"]
            seam = gen_all.VITA_KAGE_REFCOUNT_SEAM_SPECS[root]
            result = gen_all._translate_function(
                image, {"rva": root}, None, {}, pin_img=pin)
            require(result["stub"] is None, f"{name} became a stub at {base:#x}")
            require(result["vita_kage_refcount_seam"] is True and
                    result["vita_kage_mutex_seam"] is False,
                    f"seam proof selection wrong for {name} at base {base:#x}")
            require(tuple(result["vita_refcount_direct_edges"]) == spec["edges"],
                    f"{name}: authenticated direct edges changed")
            text = result["text"]
            declaration, bracketed = seam_lines(gen_all, seam)
            require(declaration[0] == FENCE and bracketed[0] == FENCE,
                    f"{name}: seam rendered under the wrong fence")
            require(text.count(marker) == 1, f"{name}: seam marker count changed")
            require(text.count(helper + "(") == 2,
                    f"{name}: helper declaration/call count changed")
            require(text.count(FENCE) == 2 and MUTEX_FENCE not in text,
                    f"{name}: seam fence count changed")
            prologue = "\n".join([
                "    GUEST_FLAGS_DECL;",
                "    GUEST_GPR_DECL;",
                *declaration,
                *bracketed,
                f"    {spec['first']}",
            ]) + "\n"
            require(prologue in text, f"{name}: seam placement changed")
            require(bracketed[3] == f"    if ({helper}(c)) return;",
                    f"{name}: seam statement shape changed")
            # The complete translated body stays behind the seam: every
            # direct edge is rendered as the predicted call of its target.
            for _site, target in spec["edges"]:
                require(f"sub_{target:08x}(c);" in text,
                        f"{name}: direct edge to {target:08x} not rendered")
            # AddRef returns through its predicted tail jmp to Unlock, the
            # other two through `ret`.
            tail = ("          sub_00562ec0(c);\n      } else\n"
                    "          guest_call(c, _target);\n      return;\n"
                    if root == ADDREF_ROOT else "GUEST_GPR_FLUSH(c); return;")
            require(tail in text, f"{name}: translated return disappeared")
            plain = strip_seam(text, declaration, bracketed)
            saved_specs = gen_all.VITA_KAGE_REFCOUNT_SEAM_SPECS
            try:
                gen_all.VITA_KAGE_REFCOUNT_SEAM_SPECS = {}
                reference = gen_all._translate_function(
                    image, {"rva": root}, None, {}, pin_img=pin)
            finally:
                gen_all.VITA_KAGE_REFCOUNT_SEAM_SPECS = saved_specs
            require(reference["vita_kage_refcount_seam"] is False and
                    reference["text"] == plain,
                    f"{name}: the seam is not the only rendered change")
            require(gen_all.unit_packing_size(text) + result["legacy_size_delta"]
                    == gen_all.unit_packing_size(reference["text"]) +
                    reference["legacy_size_delta"],
                    f"{name}: unit packing size changed")
            require(gen_all.vita_kage_mutex_seam_packing_bytes(seam) ==
                    len(gpr_locals.legacy_text(text)) -
                    len(gpr_locals.legacy_text(plain)),
                    f"{name}: packing delta does not equal the seam text")
            if base == 0x98000000:
                vita_text[root] = text
        # The mutex wrappers keep their own seam, untouched by this one.
        for root, (name, _coverage) in MUTEX_ROOTS.items():
            result = gen_all._translate_function(
                image, {"rva": root}, None, {}, pin_img=pin)
            require(result["vita_kage_mutex_seam"] is True and
                    result["vita_kage_refcount_seam"] is False and
                    FENCE not in result["text"] and
                    result["text"].count(MUTEX_FENCE) == 2,
                    f"{name}: mutex seam changed at base {base:#x}")

    # The corpus spelling (coverage note, dense import IDs): the refcount
    # bodies have no IAT site, so apart from the note they are the render
    # above; the mutex wrappers are rendered the same way for the oracle.
    import iat_meta  # noqa: E402
    imports = iat_meta.read_imports(str(pe_path))
    import_ids = {slot: index for index, (slot, _name) in enumerate(imports)}
    require(import_ids.get(0x6060FC) == 61 and import_ids.get(0x6060F8) == 60,
            "sync import IDs moved")
    saved_ids = gen_all._WORKER_IMPORT_IDS
    gen_all._WORKER_IMPORT_IDS = import_ids
    corpus: dict[int, str] = {}
    try:
        image = Image(str(pe_path), 0x98000000)
        for root, spec in ROOTS.items():
            result = gen_all._translate_function(
                image, {"rva": root}, None, {},
                function_coverage_ids={root: spec["coverage"]}, pin_img=pin)
            text = result["text"]
            note = f"    guest_coverage_function({spec['coverage']}U);\n"
            require(result["vita_kage_refcount_seam"] is True and
                    text.count(spec["marker"]) == 1 and text.count(note) == 1 and
                    "GUEST_IMPORT_CALL(" not in text and
                    "guest_try_direct_sync_import_call(" not in text,
                    f"{spec['name']}: corpus spelling changed")
            require(gpr_locals.legacy_text(text.replace(note, "", 1)) ==
                    gpr_locals.legacy_text(vita_text[root]),
                    f"{spec['name']}: corpus spelling differs from the plain "
                    "render beyond the coverage note")
            # The note precedes the seam: the seam's replayed
            # guest_coverage_function(id) for the elided bodies and the
            # oracle's route decomposition assume exactly this order.
            require(text.index(note) < text.index(spec["marker"]),
                    f"{spec['name']}: coverage note is not ahead of the seam")
            corpus[root] = text
        for root, (name, coverage) in MUTEX_ROOTS.items():
            result = gen_all._translate_function(
                image, {"rva": root}, None, {},
                function_coverage_ids={root: coverage}, pin_img=pin)
            require(result["vita_kage_mutex_seam"] is True and
                    result["text"].count(
                        f"    guest_coverage_function({coverage}U);\n") == 1,
                    f"{name}: corpus spelling changed")
            corpus[root] = result["text"]
    finally:
        gen_all._WORKER_IMPORT_IDS = saved_ids

    # Hostile authenticator cases on the spec table itself.
    class Insn:
        def __init__(self, mnemonic, op_str):
            self.mnemonic = mnemonic
            self.op_str = op_str

    for root, spec in ROOTS.items():
        name = spec["name"]
        seam = gen_all.VITA_KAGE_REFCOUNT_SEAM_SPECS[root]
        owner = gen_all.VITA_REFCOUNT_DIRECT_EDGE_ROOT_SPECS[root]
        insns = {rva: Insn(*want) for rva, want in seam["required"].items()}
        order = list(range(owner["insns"]))
        edges = tuple({"site": site} for site, *_rest in owner["edges"])
        require(gen_all.vita_kage_refcount_seam_for_body(
            pin, root, insns, order, edges) is seam, f"{name}: authenticator")
        require(gen_all.vita_kage_refcount_seam_for_body(
            pin, root + 0x10, insns, order, edges) is None,
            "unrelated root selected the seam")
        drifted = dict(insns)
        drifted[root] = Insn("push", "eax")
        must_fail(f"{name}: drifted instruction",
                  lambda: gen_all.vita_kage_refcount_seam_for_body(
                      pin, root, drifted, order, edges),
                  "ABI/algorithm changed")
        # The destroy path (Release) / the alive tail are pinned too.
        missing = dict(insns)
        del missing[max(missing)]
        must_fail(f"{name}: missing instruction",
                  lambda: gen_all.vita_kage_refcount_seam_for_body(
                      pin, root, missing, order, edges),
                  "ABI/algorithm changed")
        must_fail(f"{name}: direct edge not selected",
                  lambda: gen_all.vita_kage_refcount_seam_for_body(
                      pin, root, insns, order, edges[:-1]),
                  "requires the authenticated direct edges")
        must_fail(f"{name}: CFG order length",
                  lambda: gen_all.vita_kage_refcount_seam_for_body(
                      pin, root, insns, order[:-1], edges),
                  "requires the authenticated direct edges")
    return corpus


# ---------------------------------------- 4. predicate copies + TU pins

def predicate_conditions(text: str, function: str) -> list[str]:
    start = text.index(function)
    body = text[start:text.index("\n}\n", start)]
    conditions = re.findall(r"if \((.*?)\)\n\s+return 0U?;", body, re.DOTALL)
    return [" ".join(condition.split()) for condition in conditions]


def function_text(text: str, signature: str) -> str:
    start = text.index(signature)
    return text[start:text.index("\n}\n", start) + 3]


def string_literal(text: str, anchor: str) -> str:
    """The concatenated C string literal that starts at `anchor`."""
    start = text.index(anchor)
    # Adjacent pieces may be separated by whitespace or a macro's
    # backslash-newline continuation (the oracle's #define).
    pieces = re.match(r'(?:(?:\s|\\\r?\n)*"(?:[^"\\]|\\.)*")+', text[start:])
    require(pieces is not None, f"no literal at {anchor!r}")
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', pieces.group(0)))


def verify_seam_sources() -> None:
    seam = read(SEAM_TU)
    mutex = read(MUTEX_TU)
    header = read(RUNTIME / "host_vita_sync_fastpath.h")
    oracle = read(ORACLE_TU)
    for signature in PREDICATE_COPIES:
        require(function_text(seam, signature) == function_text(mutex, signature),
                f"seam predicate copy differs from the mutex seam: {signature}")
    require(seam.count("static inline int kage_mutex_") +
            seam.count("static inline void kage_mutex_") == len(PREDICATE_COPIES),
            "seam gained or lost a mutex predicate copy")
    inline = predicate_conditions(
        header, "static inline uint32_t isaac_vita_sync_inline_cs_address")
    copy = predicate_conditions(seam, "static inline int kage_mutex_cs_object")
    require(len(copy) == 4 and copy == inline[-4:],
            f"object predicate differs from the inline path: {copy} vs {inline[-4:]}")
    for banned in ("isaac_vita_sync_inline_cs_address(",
                   "isaac_vita_sync_inline_enter(", "isaac_vita_sync_inline_leave(",
                   "isaac_vita_sync_inline_return(", "isaac_vita_kage_mutex_lock_try(",
                   "isaac_vita_kage_mutex_unlock_try(", "ISAAC_VITA_KAGE_MUTEX_SEAM"):
        require(banned not in seam, f"seam must not route through {banned}")
    require("g_isaac_vita_sync_inline_owner" in seam and
            "exactly that of the inline critical-section fast path" in read(SEAM_HEADER),
            "seam thread-safety statement/latch check missing")
    # Census identity: a generated unit never carries ISAAC_VITA_PHASE_PROFILE
    # (verify_cmake), so the direct edges the seam elides record nothing on
    # the translated route and the seam must not replay them; it replays
    # only the guest.c-owned import census (the verbatim
    # kage_mutex_note_import copy: the one NOTE_CALL in the TU) and the
    # three elided bodies' entry hooks.
    require("guest_phase_profile_note_lookup_cache_hit" not in seam and
            "GUEST_PHASE_PROFILE_NOTE_LOOKUP" not in seam and
            "kage_refcount_note_edge" not in seam and
            seam.count("GUEST_PHASE_PROFILE_NOTE_CALL()") == 1 and
            seam.count("guest_coverage_function(RC_COVERAGE_") == 3,
            "seam replays a census the translated route does not record")
    # The TU refuses the corpus it is not exact for.
    require("#if !defined(GUEST_FLAGS_LOCAL) || !GUEST_FLAGS_LOCAL\n" in seam and
            "#error The KAGE refcount seam requires the GUEST_FLAGS_LOCAL=1 corpus"
            in seam, "GUEST_FLAGS_LOCAL refusal missing")
    # The only store into the flag state is the VERIFY snapshot restore
    # (entry value, before the rerun); the handled paths write nothing.
    code = re.sub(r"/\*.*?\*/", "", seam, flags=re.DOTALL)
    flag_stores = [m.start() for m in re.finditer(r"c->fl(\.\w+)?\s*=[^=]", code)]
    restore = function_text(code, "static void kage_refcount_snapshot_restore(")
    require(len(flag_stores) == 1 and
            code.index(restore) < flag_stores[0] < code.index(restore) + len(restore),
            "seam writes the flag state outside the VERIFY snapshot restore")
    require("SET_FLAGS(" not in code and "GUEST_FLAGS_FLUSH" not in code and
            "GUEST_FL->" not in code, "seam computes or flushes flags")
    # VERIFY's private run scope mirrors guest.c's private layout.
    guest = read(RUNTIME / "guest.c")
    require("typedef struct guest_run_scope {\n    jmp_buf env;\n} guest_run_scope;\n"
            in guest, "guest.c run scope layout changed")
    require("typedef struct kage_refcount_run_scope {\n    jmp_buf env;\n"
            "} kage_refcount_run_scope;\n" in seam,
            "seam run scope layout does not mirror guest.c")
    # Frame constants the oracle's tables and acceptance predicate assume.
    require("RC_FRAME_BYTES = 40U," in seam and
            "RC_FRAME_BYTES_WEAKLOCK = 52U," in seam and
            "RC_STATS_FIRST = 1000U," in seam and
            "RC_STATS_PERIOD_MASK = 0xFFFFFU," in seam and
            "RC_VERIFY_LOG_FIRST = 64U," in seam and
            "RC_VERIFY_LOG_EVERY = 4096U" in seam,
            "seam frame/stats/verify constants changed")
    # VERIFY receipts are throttled: MATCH only on loud runs, MISMATCH on
    # loud runs or the first RC_VERIFY_LOG_FIRST (the oracle counts them).
    require("        if (loud)\n            isaac_vita_log(\"[isaac-kage] refcount seam "
            "VERIFY: fn=%s n=%u \"\n                           \"result=MATCH" in seam and
            "    if (loud || s_stats.verify_mismatches < RC_VERIFY_LOG_FIRST)\n"
            "        isaac_vita_log(\"[isaac-kage] refcount seam VERIFY: fn=%s n=%u \"\n"
            "                       \"result=MISMATCH" in seam and
            "kage_refcount_verify_compare(fn, &s_verify_seam, &s_verify_entry,\n"
            "                                      loud))" in seam,
            "VERIFY receipt throttle changed")
    # Receipt texts: the reason/fn tables and the three formats the oracle
    # parses are the TU's literals.
    require(re.search(r"s_fn_names\[ISAAC_KRS_FN_COUNT\] = \{\s*" +
                      ",\s*".join(f'"{n}"' for n in FN_NAMES) + r"\s*\}", seam),
            "fn names changed")
    require(re.search(r"s_reason_names\[ISAAC_KRS_REASON_COUNT\] = \{\s*" +
                      ",\s*".join(f'"{n}"' for n in REASON_NAMES) + r"\s*\}", seam),
            "reason names changed")
    require(re.search(r"s_reason_names\[ISAAC_KRS_REASON_COUNT\] = \{\s*" +
                      ",\s*".join(f'"{n}"' for n in REASON_NAMES) + r"\s*\}", oracle),
            "oracle reason table differs")
    stats_format = string_literal(seam, '"[isaac-kage] refcount seam: addref=')
    oracle_format = string_literal(oracle, '"[isaac-kage] refcount seam: addref=')
    require(stats_format == oracle_format, "oracle stats format differs from the TU")
    require(stats_format.count("%u") == 24 and
            "rejected(" + ",".join(REASON_NAMES[1:]) + ")=" in stats_format,
            "stats format changed")
    banner = string_literal(seam, '"[isaac-kage] refcount seam: roots=')
    require(banner == "[isaac-kage] refcount seam: roots=7af0,7b50,7b70 lock=%08x "
            "unlock=%08x addref=%08x slots=%08x,%08x frame=%u/%u verify=%d build=%s",
            "banner format changed")
    fallback = string_literal(seam, '"[isaac-kage] refcount seam fallback: fn=')
    require(fallback == "[isaac-kage] refcount seam fallback: fn=%s reason=%s "
            "call=%u (translated body used)", "fallback format changed")
    require('"verify skipped (fault) what=%s addr=%08x skipped=%u"' in seam and
            '"result=MATCH mismatches=%u"' in seam and
            '"result=MISMATCH field=%s+%u want=%08x got=%08x "' in seam,
            "VERIFY receipt formats changed")
    require('#define ISAAC_VITA_KAGE_REFCOUNT_SEAM_BUILD_ID "refcount-seam:unstamped"'
            in seam, "default build id changed")
    # The oracle reuses the mutex oracle's guest.c route copy verbatim.
    mutex_oracle = read(RUNTIME / "host_vita_kage_mutex_seam_oracle.c")
    route = "int guest_try_direct_sync_import_call(\n"
    require(function_text(oracle, route) == function_text(mutex_oracle, route),
            "oracle's guest_try_direct_sync_import_call copy differs")
    require("longjmp(scope->env, 1);" in function_text(oracle, "void guest_call(") and
            '#include "kage_refcount_seam_bodies.inc"' not in oracle and
            "kage_refcount_seam_bodies.c" in oracle and
            "unsigned char *g_guest_coverage_functions = NULL;" in oracle and
            "CHECK(s_lookup_hits == 0U);" in oracle and
            "!SAME(lookup_hits)" in oracle,
            "oracle guest_call mock / bodies TU / census arrangement changed")


# --------------------------------------------------- 5. CMake + raw gate

def verify_cmake() -> None:
    cmake = read(VITA / "CMakeLists.txt")
    for option in (OPTION, VERIFY_OPTION):
        require(re.search(rf"option\({option}\s+\"[^\"]+\"\s+OFF\)", cmake,
                          re.DOTALL) is not None, f"{option} is not default OFF")
    for fatal in (f"if({OPTION} AND ISAAC_VITA_SCAFFOLD_ONLY)",
                  f"if({OPTION} AND NOT {MUTEX_OPTION})",
                  f"if({VERIFY_OPTION} AND NOT {OPTION})",
                  f"if({OPTION} AND NOT ISAAC_VITA_SYNC_INLINE_FASTPATH)",
                  f"if({OPTION} AND\n   NOT (ISAAC_VITA_TRANSLATED_CPU AND "
                  "ISAAC_VITA_TRANSLATED_CPU_FLAGS_LOCAL))"):
        require(fatal in cmake, f"fatal missing: {fatal}")
    block = re.search(
        rf"  if\({OPTION}\)\n(.*?)\n  endif\(\)\n  if\(ISAAC_VITA_FLOOR_THUNK_FASTPATH\)",
        cmake, re.DOTALL)
    require(block is not None, "seam CMake census block missing or moved")
    body = block.group(1)
    for spec in ROOTS.values():
        require(f'REGEX "^void {spec["name"]}\\\\(CPU \\\\*__restrict c\\\\)$"' in body,
                f"census does not match the {spec['name']} definition")
        require(f'REGEX "{spec["marker"]}"' in body, "census markers changed")
    require(body.count("LIMIT_COUNT 2") == 6, "census must detect duplicates")
    require("NOT ISAAC_VITA_KAGE_REFCOUNT_SEAM_OWNER_COUNT EQUAL 1 OR" in body and
            "NOT ISAAC_VITA_KAGE_REFCOUNT_SEAM_DEFINITIONS EQUAL 3 OR" in body and
            "NOT ISAAC_VITA_KAGE_REFCOUNT_SEAM_SEAMS EQUAL 3" in body,
            "census totals changed")
    require('"KAGE refcount owner has a missing or duplicate seam: "' in body and
            '"KAGE refcount seam escaped its frozen owner: "' in body,
            "census fatals changed")
    require(cmake.count(f"{OPTION}=1)") == 1 and cmake.count(f"{MUTEX_OPTION}=1)") == 1,
            "seam compile definitions must each be applied exactly once")
    scope = re.search(
        r"set_property\(SOURCE\s+\$\{ISAAC_VITA_KAGE_REFCOUNT_SEAM_OWNER\}\s+"
        r'"\$\{ISAAC_RUNTIME\}/host_vita_kage_refcount_seam\.c"\s+'
        r"APPEND PROPERTY COMPILE_DEFINITIONS\s+" + OPTION + r"=1\)", body)
    require(scope is not None, "seam definition scope changed")
    require('"${ISAAC_RUNTIME}/host_vita_kage_refcount_seam.c")' in body,
            "seam TU is not added to the runtime sources")
    for definition in ("ISAAC_VITA_SYNC_INLINE_FASTPATH=1",
                       "GUEST_FLAGS_LOCAL=1",
                       'ISAAC_VITA_KAGE_REFCOUNT_SEAM_BUILD_ID=\\"${ISAAC_VITA_GUEST_LINK_ID}\\"',
                       "ISAAC_VITA_PHASE_PROFILE=1",
                       "ISAAC_VITA_PROFILE_IMPORT_KINDS=1",
                       "ISAAC_VITA_GUEST_SAMPLER=1",
                       "ISAAC_VITA_GUEST_DISPATCH_TABLE=1",
                       "GUEST_GENERATED_STACK_GUARD=0",
                       f"{VERIFY_OPTION}=1"):
        require(definition in body, f"seam TU does not receive {definition}")
    require(body.count("host_vita_kage_refcount_seam.c") >= 8 and
            "host_vita_kage_mutex_seam.c" not in body,
            "seam TU defines leak to another TU")
    require('"Isaac Vita KAGE refcount: native AddRef/weak-lock/Release seam in "'
            in body, "status line changed")
    # The census owner definitions never reach a generated unit: every
    # set_property applying them names "${ISAAC_RUNTIME}/..." files only and
    # no target-wide definition exists.  The seam replays no direct-edge
    # census because of exactly this scope (in guest_0000.c the edges'
    # NOTE_CALL / NOTE_LOOKUP are no-ops and the lookup-cache hook is fenced
    # out); a generated unit gaining either define would need the seam to
    # replay the edges again and the oracle's bodies TU to follow.
    scoped = 0
    for statement in set_property_statements(cmake):
        if "ISAAC_VITA_PHASE_PROFILE=1" not in statement and \
                "ISAAC_VITA_GUEST_DISPATCH_TABLE=1" not in statement:
            continue
        scoped += 1
        require(statement.startswith("set_property(SOURCE") and
                "APPEND PROPERTY" in statement,
                f"census owner definition applied outside SOURCE scope: {statement}")
        sources = statement[len("set_property(SOURCE"):
                            statement.index("APPEND PROPERTY")].split()
        require(sources and all(
            re.fullmatch(r'"\$\{ISAAC_RUNTIME\}/[a-z0-9_]+\.c"', source)
            for source in sources),
            f"census owner definition reaches a non-runtime source: {statement}")
    require(scoped >= 10, f"census owner definition sites changed: {scoped}")
    require(re.search(r"(target_compile_definitions|add_compile_definitions|"
                      r"add_definitions)\([^)]*ISAAC_VITA_(PHASE_PROFILE|"
                      r"GUEST_DISPATCH_TABLE)(=|\s|\))", cmake) is None,
            "census owner definition became target-wide")
    require("if(ISAAC_VITA_TRANSLATED_CPU_COVERAGE_HOOKS_OFF)\n"
            "      # guest.c keeps the hooks (PC coverage transport); on Vita every pointer\n"
            "      # stays NULL, so the generated entry hook is a dead load + branch.\n"
            "      set_property(SOURCE ${ISAAC_GENERATED_C} APPEND PROPERTY\n"
            "        COMPILE_DEFINITIONS GUEST_COVERAGE_HOOKS=0)\n" in cmake,
            "generated units' coverage-hooks define changed")


def set_property_statements(cmake: str) -> list[str]:
    """Every `set_property(...)` statement of the file, parentheses balanced."""
    out: list[str] = []
    start = 0
    while True:
        index = cmake.find("set_property(", start)
        if index < 0:
            return out
        depth = 0
        end = index
        while True:
            if cmake[end] == "(":
                depth += 1
            elif cmake[end] == ")":
                depth -= 1
                if depth == 0:
                    break
            end += 1
        out.append(cmake[index:end + 1])
        start = end + 1


def verify_raw_gate(root: Path) -> None:
    import vita_raw_allocator_gate as gate  # noqa: E402

    generated = root / "generated"
    generated.mkdir(parents=True)
    owner = generated / "guest_0000.c"
    other = generated / "guest_0001.c"
    seam_tu = root / "host_vita_kage_refcount_seam.c"
    owner_lines = []
    for spec in ROOTS.values():
        owner_lines.append(f"void {spec['name']}(CPU *__restrict c)")
        owner_lines.append(f"    /* {spec['marker']}; exact translated fallback "
                           "follows. */")
    owner_text = "\n".join(owner_lines) + "\n"
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

    check = gate.verify_kage_refcount_seam_compile_scope
    off = {OPTION: "OFF"}
    on = {OPTION: "ON"}
    check(records({}, {}, {}), off)
    check(records({OPTION: ["1"]}, {}, {OPTION: ["1"]}), on)
    must_fail("definition while OFF",
              lambda: check(records({OPTION: ["1"]}, {}, {OPTION: ["1"]}), off),
              "compile-definition scope changed", (gate.GateError,))
    must_fail("owner without definition",
              lambda: check(records({}, {}, {OPTION: ["1"]}), on),
              "compile-definition scope changed", (gate.GateError,))
    must_fail("seam TU without definition",
              lambda: check(records({OPTION: ["1"]}, {}, {}), on),
              "compile-definition scope changed", (gate.GateError,))
    must_fail("definition leaked to another unit",
              lambda: check(records({OPTION: ["1"]}, {OPTION: ["1"]},
                                    {OPTION: ["1"]}), on),
              "compile-definition scope changed", (gate.GateError,))
    must_fail("non-canonical value",
              lambda: check(records({OPTION: ["0"]}, {}, {OPTION: ["1"]}), on),
              "non-canonical", (gate.GateError,))
    must_fail("policy undefined",
              lambda: check(records({OPTION: ["1"]}, {}, {OPTION: ["1"]}, [OPTION]),
                            on),
              "policy undefined", (gate.GateError,))
    owner.write_text(owner_text + owner_lines[1] + "\n", encoding="utf-8")
    must_fail("duplicate seam in the owner",
              lambda: check(records({OPTION: ["1"]}, {}, {OPTION: ["1"]}), on),
              "missing or duplicate seam", (gate.GateError,))
    owner.write_text(owner_text, encoding="utf-8")
    other.write_text(owner_lines[3] + "\n", encoding="utf-8")
    must_fail("seam escaped into another unit",
              lambda: check(records({OPTION: ["1"]}, {}, {OPTION: ["1"]}), on),
              "missing or duplicate seam", (gate.GateError,))
    other.write_text("void sub_00000000(CPU *__restrict c)\n", encoding="utf-8")
    owner.write_text("\n".join(owner_lines[:4]) + "\n", encoding="utf-8")
    must_fail("third root missing",
              lambda: check(records({OPTION: ["1"]}, {}, {OPTION: ["1"]}), on),
              "census changed", (gate.GateError,))
    owner.write_text(owner_text, encoding="utf-8")
    other.write_text("\n".join(owner_lines[4:]) + "\n", encoding="utf-8")
    owner.write_text("\n".join(owner_lines[:4]) + "\n", encoding="utf-8")
    must_fail("roots split over two owners",
              lambda: check(records({OPTION: ["1"]}, {OPTION: ["1"]},
                                    {OPTION: ["1"]}), on),
              "census changed", (gate.GateError,))
    source = read(VITA / "vita_raw_allocator_gate.py")
    require('runtime.add("host_vita_kage_refcount_seam.c")' in source and
            'f"{KAGE_REFCOUNT_SEAM_CACHE_KEY} requires "\n'
            '                f"{KAGE_MUTEX_SEAM_CACHE_KEY}=ON"' in source,
            "raw gate source inventory lost the seam TU or its requirement")
    require('        for key in ("ISAAC_VITA_TRANSLATED_CPU",\n'
            '                    "ISAAC_VITA_TRANSLATED_CPU_FLAGS_LOCAL"):\n'
            '            if key in cache and not _feature(cache, key):' in source and
            'f"{KAGE_REFCOUNT_SEAM_CACHE_KEY} requires {key}=ON"' in source,
            "raw gate lost the translated-cpu / flags-local re-check")
    require("    verify_kage_mutex_seam_compile_scope(records, cache)\n"
            "    verify_kage_refcount_seam_compile_scope(records, cache)\n" in source,
            "raw gate does not run the refcount scope check after the mutex one")


# ---------------------------------------------------- 6. corpus census

def unit_files(directory: Path) -> list[Path]:
    return sorted(path for path in directory.glob("guest_[0-9][0-9][0-9][0-9].c"))


def verify_corpus(generated: Path, baseline: Path | None,
                  corpus: dict[int, str]) -> str:
    sys.path.insert(0, str(HERE))
    import gen_all  # noqa: E402

    owners: list[Path] = []
    for path in unit_files(generated):
        text = read(path)
        roots = sum(text.count(f"void {spec['name']}(CPU *__restrict c)\n")
                    for spec in ROOTS.values())
        markers = sum(text.count(spec["marker"]) for spec in ROOTS.values())
        helpers = sum(text.count(spec["helper"] + "(") for spec in ROOTS.values())
        if roots:
            require(roots == 3 and markers == 3 and helpers == 6 and
                    text.count(FENCE) == 6,
                    f"{path.name}: owner census roots={roots} markers={markers} "
                    f"helpers={helpers} fences={text.count(FENCE)}")
            owners.append(path)
        else:
            require(markers == 0 and helpers == 0 and FENCE not in text,
                    f"{path.name}: seam escaped its owner")
    require(len(owners) == 1, f"expected one owner unit, found {owners}")
    owner = owners[0]
    owner_text = read(owner)
    for root in ROOTS:
        require(owner_text.count(corpus[root]) == 1,
                f"{ROOTS[root]['name']}: corpus body differs from the fresh render")
    mutex_owner = [path for path in unit_files(generated)
                   if MUTEX_FENCE in read(path)]
    require(len(mutex_owner) == 1 and mutex_owner[0] != owner and
            all(read(mutex_owner[0]).count(corpus[root]) == 1 for root in MUTEX_ROOTS),
            "mutex wrappers' corpus bodies differ from the fresh render")
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
            unfenced = re.sub(
                r"#if defined\(__vita__\) && "
                r"defined\(ISAAC_VITA_(?:FLOOR_THUNK_FASTPATH|"
                r"SHADER_ATTRIB_FASTPATH|KAGE_MUTEX_SEAM)\)\n"
                r"(?:.*\n)*?#endif\n",
                "", read(new_path))
            require(unfenced == read(base_path),
                    f"{new_path.name} differs from the baseline corpus")
            other_seams += 1
        stripped = owner_text
        lines = 0
        for root in ROOTS:
            declaration, bracketed = seam_lines(
                gen_all, gen_all.VITA_KAGE_REFCOUNT_SEAM_SPECS[root])
            stripped = strip_seam(stripped, declaration, bracketed)
            lines += len(declaration) + len(bracketed)
        require(stripped == read(baseline / owner.name),
                f"{owner.name} differs from the baseline by more than the seam")
        print(f"corpus identity: {identical} units byte-identical to the "
              f"baseline; {owner.name} differs only by the {lines} seam lines; "
              f"{other_seams} unit(s) differ only by other fenced seams")
    return owner.name


# ------------------------------------------------------- 7. host oracle

def function_lines(text: str, name: str) -> list[str]:
    lines = text.splitlines()
    start = lines.index(f"void {name}(CPU *__restrict c)")
    end = start
    while lines[end] != "}":
        end += 1
    return lines[start:end + 1]


def resolve_preprocessor(lines: list[str], defines: set[str]) -> list[str]:
    """Keep the `#if defined(...)` branches selected by `defines` (the corpus
    as compiled for the Vita with the given seams on)."""
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


def rename(lines: list[str], name: str, suffix: str) -> list[str]:
    head = f"void {name}(CPU *__restrict c)"
    require(lines[0] == head, "body does not start with its definition")
    return [f"void {name}{suffix}(CPU *__restrict c)"] + lines[1:]


def replace_once(lines: list[str], old: str, new: str, label: str) -> list[str]:
    hits = [i for i, line in enumerate(lines) if old in line]
    require(len(hits) == 1, f"{label}: expected exactly one {old!r}, found {len(hits)}")
    out = list(lines)
    out[hits[0]] = out[hits[0]].replace(old, new)
    return out


def write_bodies(corpus: dict[int, str], path: Path) -> None:
    """Three spellings per refcount root (see the oracle's header comment):
    sub_X (exact unit text, seam kept), sub_X_plain (seam dropped: the
    production body without the knob), sub_X_rest (plain minus the entry
    hook: what follows the seam statement)."""
    out = ["/* generated by test_vita_kage_refcount_seam.py from the fresh "
           "gen_all render (corpus spelling, base 0x98000000) */"]
    for root, (name, _coverage) in sorted(MUTEX_ROOTS.items()):
        body = resolve_preprocessor(function_lines(corpus[root], name),
                                    CORPUS_DEFINES | {MUTEX_OPTION})
        require(sum("isaac_vita_kage_mutex_" in line for line in body) == 2,
                f"{name}: mutex seam not selected")
        out.extend(body)
        out.append("")
    nested = f"{ROOTS[ADDREF_ROOT]['name']}(c);"
    for root, spec in sorted(ROOTS.items()):
        name, helper = spec["name"], spec["helper"]
        note = f"guest_coverage_function({spec['coverage']}U);"
        unit = resolve_preprocessor(function_lines(corpus[root], name),
                                    CORPUS_DEFINES | {OPTION})
        require(sum(helper in line for line in unit) == 2 and
                sum(note in line for line in unit) == 1,
                f"{name}: unit text lost the seam or the entry hook")
        plain = resolve_preprocessor(function_lines(corpus[root], name),
                                     CORPUS_DEFINES)
        require(not any(helper in line for line in plain),
                f"{name}: seam helper survived the fence drop")
        # The owner unit's spelling of the direct edges: the census macros
        # stay as text (the bodies TU compiles them to nothing, exactly like
        # guest_0000.c) and the fenced lookup-cache hook is gone.
        require(sum("GUEST_PHASE_PROFILE_NOTE_CALL();" in line for line in plain) ==
                len(spec["edges"]) and
                sum("GUEST_PHASE_PROFILE_NOTE_LOOKUP();" in line for line in plain) ==
                len(spec["edges"]),
                f"{name}: direct-edge census macro count changed")
        hits = [i for i, line in enumerate(plain) if line == f"    {note}"]
        require(len(hits) == 1, f"{name}: entry hook count changed")
        rest = plain[:hits[0]] + plain[hits[0] + 1:]
        if root == WEAKLOCK_ROOT:
            plain = replace_once(plain, nested, f"{ROOTS[ADDREF_ROOT]['name']}_plain(c);",
                                 name)
            require(sum(nested in line for line in rest) == 1,
                    f"{name}: nested AddRef call count changed")
        out.extend(unit)
        out.append("")
        out.extend(rename(plain, name, "_plain"))
        out.append("")
        out.extend(rename(rest, name, "_rest"))
        out.append("")
    require(not any("guest_phase_profile_note_lookup_cache_hit" in line
                    for line in out),
            "a body kept the fenced lookup-cache hook (generated units are "
            "compiled without ISAAC_VITA_PHASE_PROFILE)")
    path.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")


def write_bodies_tu(path: Path) -> None:
    """The reference bodies as a translation unit of their own, compiled with
    OWNER_UNIT_DEFINES (run_oracle): what guest_0000.c / guest_0166.c receive
    from CMake, so the bodies' census macros mean what they mean on the Vita."""
    names = [name for name, _coverage in MUTEX_ROOTS.values()]
    names += [spec["name"] + suffix for spec in ROOTS.values()
              for suffix in ("", "_plain", "_rest")]
    lines = [
        "/* generated by test_vita_kage_refcount_seam.py: the reference bodies as",
        " * a translation unit of their own, compiled like a generated unit (no",
        " * ISAAC_VITA_PHASE_PROFILE, no ISAAC_VITA_GUEST_DISPATCH_TABLE,",
        " * GUEST_COVERAGE_HOOKS=0) and linked with the oracle. */",
        '#include "guest.h"',
        "#if defined(ISAAC_VITA_PHASE_PROFILE) || defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)",
        "#error the reference bodies must compile like a generated unit",
        "#endif",
        "#if GUEST_COVERAGE_HOOKS",
        "#error the reference bodies must compile with GUEST_COVERAGE_HOOKS=0",
        "#endif",
        "void sub_0055e330(CPU *__restrict c);",
    ]
    lines += [f"void {name}(CPU *__restrict c);" for name in sorted(names)]
    lines.append('#include "kage_refcount_seam_bodies.inc"')
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def run_oracle(cc: str, vitasdk_include: Path | None, corpus: dict[int, str],
               root: Path, steps: int) -> list[str]:
    include = root / "inc"
    include.mkdir(parents=True)
    write_bodies(corpus, include / "kage_refcount_seam_bodies.inc")
    bodies_tu = include / "kage_refcount_seam_bodies.c"
    write_bodies_tu(bodies_tu)
    compiler = shlex.split(cc)
    strict = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
              "-I", str(RUNTIME), "-I", str(include)]
    if vitasdk_include is not None:
        strict += ["-idirafter", str(vitasdk_include)]
    # The census owners (the oracle, the seam TU, the mutex seam TU, the
    # endpoint): every profile / dispatch / sampler definition, exactly as
    # guest.c and the seam TUs receive them in a counting build.
    common = strict + [
        "-DGUEST_IMAGE_BASE=0x98000000u", "-DGUEST_STACK_REQUIRED=1",
        "-DISAAC_VITA_SYNC_INLINE_FASTPATH=1", f"-D{MUTEX_OPTION}=1",
        f"-D{OPTION}=1",
        "-DISAAC_VITA_PHASE_PROFILE=1", "-DISAAC_VITA_GUEST_DISPATCH_TABLE=1",
        "-DISAAC_VITA_PROFILE_IMPORT_KINDS=1", "-DISAAC_VITA_GUEST_SAMPLER=1",
        "-DISAAC_VITA_PROFILE_FUNCTION_ENTRIES=1",
        "-DGUEST_FLAGS_LOCAL=1", "-DGUEST_COVERAGE_HOOKS=1",
    ]
    # The reference bodies: the owner unit's define set (both seam options,
    # like guest_0000.c and guest_0166.c; no census owner definition).
    owner = strict + [*OWNER_UNIT_DEFINES, f"-D{MUTEX_OPTION}=1", f"-D{OPTION}=1"]
    link: list[str] = []
    if os.name == "nt":
        # The IAT page is mapped at its guest address (0x98606000): the
        # 32-bit image must be large-address aware.  The UCRT's strcpy/sscanf
        # deprecation is noise here.  (clang -m32 against the MSVC UCRT; the
        # psp2 headers' _MSC_VER branch spells SCE_DEPRECATED as a
        # __declspec on enumerators, which clang rejects: point
        # --vitasdk-include at a copy whose psp2common/types.h takes the GNU
        # branch under __clang__.)
        link = ["-Wl,/LARGEADDRESSAWARE"]
        common.append("-D_CRT_SECURE_NO_WARNINGS")
    else:
        link = ["-no-pie"]
    sources = [str(RUNTIME / name) for name in (
        "vita_sync_services.c", "host_vita_sync.c",
        "host_vita_kage_mutex_seam.c", "host_vita_kage_refcount_seam.c",
        "host_vita_kage_refcount_seam_oracle.c")]
    legs = [(guard, gpr, 0) for guard in (0, 1) for gpr in (1, 0)]
    legs += [(0, 1, 1), (1, 1, 1)]
    results = []
    for guard, gpr, verify in legs:
        exe = root / f"oracle-g{guard}-r{gpr}-v{verify}{'.exe' if os.name == 'nt' else ''}"
        unit = [f"-DGUEST_GENERATED_STACK_GUARD={guard}", f"-DGUEST_GPR_LOCAL={gpr}"]
        defines = list(unit)
        if verify:
            defines.append(f"-D{VERIFY_OPTION}=1")
        bodies = root / f"bodies-g{guard}-r{gpr}{'.obj' if os.name == 'nt' else '.o'}"
        run([*compiler, *owner, *unit, "-c", str(bodies_tu), "-o", str(bodies)])
        run([*compiler, *common, *defines, *link, *sources, str(bodies),
             "-o", str(exe)])
        for seed in ("", "0x1234567", "0xdeadbeef"):
            command = [str(exe), str(steps)] + ([seed] if seed else [])
            output = run(command)
            line = [l for l in output.splitlines() if l.startswith(ORACLE_PASS)]
            require(len(line) == 1, f"oracle did not pass: {output}")
            require(f"(stack_guard={guard} gpr_local={gpr} verify={verify} "
                    f"steps={steps} " in line[0],
                    "oracle ran a different configuration")
            results.append(line[0])
    return results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=Path)
    parser.add_argument("--generated-dir", type=Path)
    parser.add_argument("--baseline-generated-dir", type=Path)
    parser.add_argument("--cc", help="GNU-compatible host compiler for the oracle "
                        "(a command line, e.g. \"clang -m32\")")
    parser.add_argument("--vitasdk-include", type=Path,
                        help="directory holding psp2/ (the oracle's sync mocks)")
    parser.add_argument("--steps", type=int, default=100000)
    arguments = parser.parse_args()

    verify_pe(arguments.pe)
    corpus = verify_codegen(arguments.pe)
    verify_seam_sources()
    verify_cmake()
    with tempfile.TemporaryDirectory(prefix="isaac-kage-refcount-") as value:
        root = Path(value)
        verify_raw_gate(root / "gate")
        owner = "<not checked>"
        if arguments.generated_dir is not None:
            owner = verify_corpus(arguments.generated_dir,
                                  arguments.baseline_generated_dir, corpus)
        oracle = []
        if arguments.cc:
            oracle = run_oracle(arguments.cc, arguments.vitasdk_include,
                                corpus, root / "oracle", arguments.steps)
            for line in oracle:
                print(line)
        else:
            print("note: --cc not given, host oracle skipped")
    print("Vita KAGE refcount seam: PASS; roots=3 seams=3 "
          f"owner={owner} bases=2 hostile-authenticator=15 hostile-gate=10 "
          f"predicate-copies={len(PREDICATE_COPIES)} oracle-runs={len(oracle)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
