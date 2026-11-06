#!/usr/bin/env python3
"""Corpus-wide proof for GUEST_GPR_FLUSH_MASK (recomp/gpr_flush_mask.py).

The masks are exact when, at every GUEST_GPR_FLUSH_MASK(c, m), each register
the mask skips satisfies `c->reg == _reg`.  That splits into two facts:

  L  the local still holds the value it had at the last synchronisation
     (GUEST_GPR_DECL / GUEST_GPR_RELOAD / a FLUSH) on every path to the site;
  C  nothing but those synchronisations wrote the CPU field since then
     (host code naming `c`, a translated callee, guest_call, an import ...).

gpr_flush_mask.py derives both from the rendered text with its own parser.
This test re-derives them independently on a generated corpus:

1. Text (`--generated-dir`, optionally `--off-generated-dir`):
   * every unit, masks mapped back to FLUSH, equals the unmasked corpus
     (only FLUSH statements changed, unit membership unchanged);
   * guest_0144.c (the pinned direct-default owner) carries no mask;
   * census of every masked body: no direct `_reg` local spelling, no
     `&GR(`, every `goto` target is a label of the same body, every
     boundary call (translated callee, guest_call, import, int3, cpuid,
     xgetbv) is preceded by a publication and followed by RELOAD or return
     (fact C for boundaries), and the called identifiers outside the
     emitter vocabulary are listed (fact C for host code: each one makes
     the analysis keep full FLUSHes until a RELOAD; the list is the review
     surface).
2. GCC proof of fact L (`--cc`, Linux gcc): every masked unit is compiled
   through a check prelude that gives each register a shadow local `_s_reg`,
   assigned at every synchronisation exactly where the CPU field is, and
   turns every skipped store of a FLUSH_MASK into
       if (_s_reg != _reg) gpr_mask_violation_<reg>_<line>();
   The implicit callee is never defined: GCC leaves the call in the object
   unless its own value numbering proves `_s_reg == _reg` on every path to
   the site -- with the real macro expansions, the real control flow
   (gotos, switches, loops, preprocessor branches as configured) and every
   real write to the local.  `nm -u` lists the sites it could not prove;
   the proof passes when the list is empty.  Configurations: production
   (GUEST_LEAF_INLINE=1), GUEST_LEAF_INLINE=0 (the #else spellings) and
   vita-seams (__vita__ plus every ISAAC_VITA_* name the corpus tests, so
   every gen_all seam block compiles in), all with the production
   generated-unit defines.
   GCC proof of fact C (configurations hostflag, hostflag-vita-seams): the
   same compiles with every use of `c` in a masked body that could write a
   GPR field outside a synchronisation -- `c->eax..edi/esp/r`, `c` handed
   to anything but the sync macros and the helpers listed in
   HOST_SAFE_CALLEES (read in guest.h/guest.c), and every invocation of a
   macro that passes `c` without spelling it (HIDDEN_C_MACROS) -- rewritten
   to set a flag local that FLUSH/RELOAD clear; a masked FLUSH reachable
   with the flag set is reported the same way.  The hostflag configurations
   keep every fact-L check, so they subsume production / vita-seams.  The
   classification is this test's own, not gpr_flush_mask.py's.
   Expansion census (with --cc, before the proof): up to 16 distinct real
   invocations of every identifier called in masked bodies are expanded
   with gcc -E in the production and vita-seams configurations; no
   expansion may name a GPR field of `c` that its invocation does not spell
   (sync macros excepted), and the identifiers whose expansion passes a `c`
   the invocation does not spell must be exactly HIDDEN_C_MACROS.  Two
   synthetic macros are positive controls.
3. Mutations (positive controls; the proof must catch each): in sampled
   units, every mask forced to 0x00, and one dirty bit dropped per masked
   site.  A dropped bit that GCC still proves equal is a register whose
   value is unchanged although the analysis saw a write (e.g. a balanced
   push/pop); those are counted, not failed.  For fact C: the RELOAD after
   every translated call dropped, and separately after every
   GUEST_IMPORT_CALL/JMP (some masked site must be flagged in each mutated
   unit), and a host CPU access or helper call inserted before every 7th
   masked site, with a reachability marker beside it: every probe whose
   marker survives must be flagged (GCC deletes a site it proves unreachable,
   such as a divide-by-zero branch after `mov ecx, 10`, and a probe in the
   `#else` arm of `#if GUEST_LEAF_INLINE` is not compiled in production; the
   leaf-inline-off proof covers those sites).
4. OFF identity (`--off-generated-dir`): each masked unit compiled with
   GUEST_GPR_FLUSH_MASKED=0 is object-identical to the unmasked unit, and
   with GUEST_GPR_FLUSH_MASKED=1 differs only where masks are partial; the
   x86-64 instruction and register-store counts of both are reported as a
   host-side proxy (`--counts-report`).
5. Wiring (always): CMake option default OFF, prerequisite, corpus probe and
   the one compile definition; guest.h default, and the bit -> register map
   of its GUEST_GPR_FLUSH_MASKED=1 macro (the GCC proof replaces that macro,
   so its Intel order is checked here); release pin OFF; gen_all recipe and
   the guest_0144.c exemption; README section.

usage: test_gpr_flush_mask_proof.py --generated-dir MASKED
           [--off-generated-dir OFF] [--cc gcc] [--jobs N] [--units N]
           [--only UNITS] [--configs LIST] [--sample-mutations N]
           [--skip-mutations] [--report FILE] [--unproven-report FILE]
           [--counts-report FILE]
Needs Linux gcc for --cc (WSL works); without --cc only the text checks run.
The whole corpus is hours of GCC on 30 jobs (some units take > 6 minutes per
configuration); hostflag,hostflag-vita-seams,leaf-inline-off covers every
check of the five configurations.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import concurrent.futures
import hashlib
import os
import pathlib
import random
import re
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
sys.path.insert(0, os.fspath(HERE))

import gpr_flush_mask as M                                   # noqa: E402

GPRS = M.GPRS
UNIT_RE = re.compile(r"guest_\d{4}\.c\Z")
FUNC_RE = re.compile(
    r"^void ((?:sub|guest_original)_[0-9a-f]{8})\(CPU \*__restrict c\)\n\{\n", re.M)
MASK_RE = re.compile(r"GUEST_GPR_FLUSH_MASK\(c, 0x([0-9a-f]{2})U\);")
FLUSH_TEXT = "GUEST_GPR_FLUSH(c);"
EXEMPT_UNIT = "guest_0144.c"

# Generated-unit defines of the production configuration
# (recomp/vita/CMakeLists.txt ~9640-9760 under the production cache settings:
# GUEST_STACK_GUARD OFF, COVERAGE_HOOKS_OFF, FS_BASE_INLINE, FLAGS_LOCAL,
# GPR_LOCAL, IAT_DIRECT, SSE_LOWER ON; LEAF_INLINE ON = header default).
PRODUCTION_DEFINES = [
    "-DGUEST_IMAGE_BASE=0x98000000u", "-DGUEST_STACK_REQUIRED=1",
    "-DGUEST_GENERATED_STACK_GUARD=0", "-DGUEST_COVERAGE_HOOKS=0",
    "-DGUEST_FS_BASE_INLINE=1", "-DGUEST_FLAGS_LOCAL=1", "-DGUEST_GPR_LOCAL=1",
    "-DGUEST_IMPORT_DIRECT=1", "-DGUEST_SSE_LOWER=1",
    "-DISAAC_VITA_PHASE_PROFILE=1", "-DISAAC_VITA_FLOOR_THUNK_FASTPATH=1",
]
CONFIGS = {
    "production": ["-DGUEST_LEAF_INLINE=1"],
    "leaf-inline-off": ["-DGUEST_LEAF_INLINE=0"],
    # "vita-seams" is filled in by seam_config(): __vita__ plus every
    # ISAAC_VITA_* name the corpus tests in a preprocessor line, all =1, so
    # every gen_all seam block compiles in (most are `#if defined(__vita__)
    # && defined(ISAAC_VITA_X)` and absent from the two configs above).
    "vita-seams": None,
    # Fact C (see host_instrument): the same two spellings with every host
    # use of `c` in a masked body rewritten to set the prelude's _gpr_host.
    "hostflag": ["-DGUEST_LEAF_INLINE=1"],
    "hostflag-vita-seams": None,
}
PP_KNOB_RE = re.compile(r"^[ \t]*#[ \t]*(?:if|elif|ifdef|ifndef)\b[^\n]*", re.M)


# ISAAC_VITA_* names a seam block uses as a value rather than tests in a
# preprocessor line; CMake gives each its value (recomp/vita/CMakeLists.txt:
# ISAAC_VITA_ARCHIVE_VALIDATION_SKIP_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\").
# Any other such name fails seam_config() loudly instead of the compile.
SEAM_VALUE_DEFINES = {
    "ISAAC_VITA_ARCHIVE_VALIDATION_SKIP_BUILD_ID": '"gpr-mask-proof"',
}
VITA_NAME_RE = re.compile(r"\bISAAC_VITA_[A-Z0-9_]+\b")


def seam_config(gen, units):
    names, values = set(), set()
    for unit in units:
        text = read(os.path.join(gen, unit))
        for line in PP_KNOB_RE.findall(text):
            names.update(VITA_NAME_RE.findall(line))
        values.update(VITA_NAME_RE.findall(PP_KNOB_RE.sub("", text)))
    values -= names
    unknown = sorted(values - set(SEAM_VALUE_DEFINES))
    check(not unknown,
          "vita-seams: ISAAC_VITA_* value names without a proof value: %r" % unknown)
    return (["-D__vita__", "-DGUEST_LEAF_INLINE=1"] +
            ["-D%s=1" % n for n in sorted(names)] +
            ["-D%s=%s" % (n, SEAM_VALUE_DEFINES[n]) for n in sorted(values)
             if n in SEAM_VALUE_DEFINES])

_REGS = ", ".join
CHECK_PRELUDE = (r'''/* test_gpr_flush_mask_proof.py check prelude (never built for the Vita). */
#include <math.h>
#include "guest.h"
#include "guest_funcs.h"
#if !GUEST_GPR_LOCAL
#error the GPR flush mask proof needs GUEST_GPR_LOCAL=1
#endif
#undef GUEST_GPR_DECL
#undef GUEST_GPR_FLUSH
#undef GUEST_GPR_RELOAD
#undef GUEST_GPR_FLUSH_MASK
#if GUEST_GENERATED_STACK_GUARD
#define GPR_PROOF_GUARD_LOCALS , _gsp_value GUEST_GPR_UNUSED = 0U; \
    int GUEST_GPR_UNUSED _gsp_ok = 0
#else
#define GPR_PROOF_GUARD_LOCALS
#endif
/* Same loads as guest.h GUEST_GPR_DECL, plus the shadows (= the CPU field
 * as far as this body's own synchronisations go). */
#define GUEST_GPR_DECL \
    uint32_t GUEST_GPR_UNUSED %(decl)s GPR_PROOF_GUARD_LOCALS; \
    uint32_t GUEST_GPR_UNUSED %(sdecl)s; \
    int GUEST_GPR_UNUSED _gpr_host = 0
#define GUEST_GPR_FLUSH(c) ((void)(%(flush)s, %(sset)s, _gpr_host = 0))
#define GUEST_GPR_RELOAD(c) ((void)(%(reload)s, %(sset)s, _gpr_host = 0))
#define GPR_PROOF_CAT(r, l) gpr_mask_violation_##r##_##l
#define GPR_PROOF_SYM(r, l) GPR_PROOF_CAT(r, l)
/* Fact L: each skipped register's local against its value at the last sync. */
#define GPR_PROOF_CHECK(m, bit, r) \
    (((m) & (bit)) ? (void)0 : \
     (_s_##r != _##r ? (void)GPR_PROOF_SYM(r, __LINE__)() : (void)0))
/* Fact C: the hostflag configurations rewrite every text use of `c` in a
 * masked body that can write a GPR field outside a synchronisation to
 * GPR_PROOF_HOSTP(c) (host_instrument in the test); a FLUSH or RELOAD clears
 * the flag, and a masked FLUSH reached with it set on any path is a
 * violation.  Without the rewrite _gpr_host is the constant 0. */
#define GPR_PROOF_HOSTP(c) (_gpr_host = 1, (c))
#define GPR_PROOF_HOST_MACRO(m, ...) (_gpr_host = 1, m(__VA_ARGS__))
#define GPR_PROOF_HOST_CHECK() \
    (_gpr_host ? (void)GPR_PROOF_SYM(host, __LINE__)() : (void)0)
/* Mutation control only: an undefined call GCC keeps unless the code that
 * holds it is unreachable or not compiled, so it names the probes that can
 * be reached at all. */
#define GPR_PROOF_REACH() ((void)GPR_PROOF_SYM(reach, __LINE__)())
/* The checks, then the full publication: the program is the unmasked one. */
#define GUEST_GPR_FLUSH_MASK(c, m) \
    ((void)(%(checks)s, GPR_PROOF_HOST_CHECK(), GUEST_GPR_FLUSH(c)))
''' % {
    "decl": _REGS("_%s = c->%s" % (r, r) for r in GPRS),
    "sdecl": _REGS("_s_%s = _%s" % (r, r) for r in GPRS),
    "flush": _REGS("(c)->%s = _%s" % (r, r) for r in GPRS),
    "reload": _REGS("_%s = (c)->%s" % (r, r) for r in GPRS),
    "sset": _REGS("_s_%s = _%s" % (r, r) for r in GPRS),
    "checks": _REGS("GPR_PROOF_CHECK(m, 0x%02xU, %s)" % (1 << i, r)
                    for i, r in enumerate(GPRS)),
})

FAILURES = []


def check(cond, what):
    if not cond:
        FAILURES.append(what)
        print("FAIL:", what, flush=True)


def read(path):
    with open(path, "r", encoding="utf-8", newline="") as fh:
        return fh.read()


def units_of(directory):
    return sorted(n for n in os.listdir(directory) if UNIT_RE.match(n))


def bodies(text):
    """[(name, body_start, body_end)] of the corpus functions of one unit."""
    out = []
    for m in FUNC_RE.finditer(text):
        end = text.find("\n}\n", m.end())
        out.append((m.group(1), m.end(), end + 1))
    return out


# ------------------------------------------------------------ text census ---
VOCABULARY = (M._VALUE_CALLS | M._CPU_NEUTRAL_CALLS | M._BOUNDARY_CALLS |
              M._STACK_WRITERS | M._SYNC | {"GUEST_GPR_FLUSH_MASK"})
CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
LOCAL_RE = re.compile(r"\b_(?:eax|ecx|edx|ebx|esp|ebp|esi|edi|gsp_value|gsp_ok)\b")
ADDR_GR_RE = re.compile(r"&\s*\(?\s*GR\s*\(")
GOTO_RE = re.compile(r"\bgoto\s+([A-Za-z_]\w*)\s*;")
LABEL_RE = re.compile(r"(?:^|[;{}]|\n)\s*([A-Za-z_]\w*)\s*:(?!:)")
BOUNDARY_RE = re.compile(
    r"\b(?:(sub_[0-9a-f]{8}|guest_original_[0-9a-f]{8})\(c\)|"
    r"(guest_call|guest_int3)\(c,|(guest_cpuid|guest_xgetbv)\(c\)|"
    r"(GUEST_IMPORT_CALL|GUEST_IMPORT_JMP)\()")
COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)


def strip_comments(text):
    return COMMENT_RE.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def statement_end(text, pos):
    """Index just past the `;` closing the statement that starts at pos."""
    depth = 0
    for k in range(pos, len(text)):
        ch = text[k]
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        elif ch == ";" and depth == 0:
            return k + 1
    return -1


def census_body(name, body, census):
    """Fact C at the text level for one masked body."""
    text = strip_comments(body)
    for m in LOCAL_RE.finditer(text):
        census["direct_local"].append((name, m.group(0)))
    for m in ADDR_GR_RE.finditer(text):
        # unary `&` only: binary AND has an operand (name, number, `)`, `]`)
        # before it
        prev = text[:m.start()].rstrip()[-1:]
        if prev and (prev.isalnum() or prev in "_)]&"):
            continue
        census["addr_gr"].append((name, text[m.start():m.start() + 20]))
    labels = {m.group(1) for m in LABEL_RE.finditer(text)} - {"default", "case"}
    for m in GOTO_RE.finditer(text):
        if m.group(1) not in labels:
            census["goto_unknown_label"].append((name, m.group(1)))
        elif not M._LABEL_RE.match(m.group(1)):
            census["goto_non_L_label"].append((name, m.group(1)))
    for m in CALL_RE.finditer(text):
        ident = m.group(1)
        if (ident in VOCABULARY or M._SUB_RE.match(ident) or
                ident.startswith("GUEST_XMM_") or re.match(r"^(cc|fl)_[a-z]+\Z", ident)):
            continue
        census["host_calls"][ident] += 1
    for m in re.finditer(r"\bc->(\w+)", text):
        if m.group(1) in M._CPU_GPR_FIELD or m.group(1) not in M._CPU_OTHER_FIELDS:
            census["host_fields"][m.group(1)] += 1
    for m in BOUNDARY_RE.finditer(text):
        callee = next(g for g in m.groups() if g)
        end = statement_end(text, m.start())
        after = text[end:end + 80].lstrip()
        before = text[max(0, m.start() - 200):m.start()].rstrip()
        followed = after.startswith("GUEST_GPR_RELOAD(c);") or after.startswith("return;")
        preceded = bool(re.search(
            r"(GUEST_GPR_FLUSH\(c\);|GUEST_GPR_FLUSH_MASK\(c, 0x[0-9a-f]{2}U\);)"
            r"(\s*GUEST_FLAGS_FLUSH\(c\);)?\Z", before))
        key = callee if not M._SUB_RE.match(callee) else "sub_*"
        census["boundary"][(key, preceded, followed)] += 1
        if not (preceded and followed):
            census["boundary_bad"].append((name, callee, before[-60:], after[:40]))


def text_checks(gen, off, report):
    census = {"direct_local": [], "addr_gr": [], "goto_unknown_label": [],
              "goto_non_L_label": [], "host_calls": collections.Counter(),
              "host_fields": collections.Counter(), "boundary": collections.Counter(),
              "boundary_bad": []}
    totals = collections.Counter()
    per_function = {}
    for unit in units_of(gen):
        text = read(os.path.join(gen, unit))
        masks = MASK_RE.findall(text)
        if off is not None:
            ref = read(os.path.join(off, unit))
            check(M.legacy_text(text) == ref,
                  "%s: masks mapped back to FLUSH differ from the unmasked unit" % unit)
        if unit == EXEMPT_UNIT:
            check(not masks, "%s carries %d masks" % (unit, len(masks)))
        totals["units"] += 1
        totals["masks"] += len(masks)
        totals["flushes_full"] += text.count(FLUSH_TEXT)
        for name, start, end in bodies(text):
            body = text[start:end]
            ms = [int(x, 16) for x in MASK_RE.findall(body)]
            full = body.count(FLUSH_TEXT)
            per_function[name] = (unit, len(ms), full,
                                  sum(8 - bin(x).count("1") for x in ms))
            totals["bodies"] += 1
            if ms:
                totals["masked_bodies"] += 1
                census_body(name, body, census)
    totals["stores_removed"] = sum(v[3] for v in per_function.values())
    print("text: %d units, %d bodies (%d masked), %d masked FLUSH sites, %d full FLUSH "
          "sites left, %d static stores removed" % (
              totals["units"], totals["bodies"], totals["masked_bodies"], totals["masks"],
              totals["flushes_full"], totals["stores_removed"]))
    for key in ("direct_local", "addr_gr", "goto_unknown_label", "goto_non_L_label",
                "boundary_bad"):
        print("census %-20s %d %s" % (key, len(census[key]), census[key][:4]))
        check(not census[key], "census %s: %r" % (key, census[key][:8]))
    print("census boundary (callee, preceded by a publication, followed by RELOAD/return):")
    for (callee, pre, fol), n in sorted(census["boundary"].items()):
        print("    %-20s pre=%d fol=%d %7d" % (callee, pre, fol, n))
    print("census host calls in masked bodies (keep FLUSHes full until RELOAD): %d "
          "distinct, %d sites" % (len(census["host_calls"]), sum(census["host_calls"].values())))
    for ident, n in census["host_calls"].most_common():
        print("    %-48s %6d" % (ident, n))
    print("census host-classified c-> fields in masked bodies: %r" % dict(census["host_fields"]))
    if report:
        with open(report, "w", encoding="utf-8") as fh:
            fh.write("function\tunit\tmasked\tfull\tstores_removed\n")
            for name in sorted(per_function):
                unit, nm, nf, sr = per_function[name]
                fh.write("%s\t%s\t%d\t%d\t%d\n" % (name, unit, nm, nf, sr))
    return per_function


# --------------------------------------------------------------- wiring ---
def wiring_checks():
    """The knob is wired like its siblings: CMake option default OFF with its
    prerequisite and corpus probe, the one compile definition, the header
    default, the release pin, the generation recipe, the README section."""
    root = HERE.parent
    cmake = read(os.fspath(HERE / "vita" / "CMakeLists.txt"))
    knob = "ISAAC_VITA_TRANSLATED_CPU_GPR_FLUSH_MASK"
    check(re.search(r"option\(%s\n\s+\"[^\"]*\" OFF\)" % knob, cmake) is not None,
          "CMake option missing or not default OFF")
    block = re.search(r"\n    if\(%s\)\n(.*?)\n    endif\(\)\n    if\(" % knob, cmake, re.S)
    check(block is not None, "CMake knob block missing")
    if block:
        body = block.group(1)
        for needle in ("requires ISAAC_VITA_TRANSLATED_CPU_GPR_LOCAL=ON",
                       '"${ISAAC_GENERATED_DIR}/guest_0000.c"',
                       'REGEX "GUEST_GPR_FLUSH_MASK\\\\(c, 0x"',
                       "COMPILE_DEFINITIONS GUEST_GPR_FLUSH_MASKED=1"):
            check(needle in body, "CMake knob block lacks %s" % needle)
    check(cmake.count("COMPILE_DEFINITIONS GUEST_GPR_FLUSH_MASKED=1") == 1,
          "GUEST_GPR_FLUSH_MASKED=1 must be defined from the knob block only")
    header = read(os.fspath(RUNTIME / "guest.h"))
    check("#ifndef GUEST_GPR_FLUSH_MASKED\n#define GUEST_GPR_FLUSH_MASKED 0\n#endif" in header,
          "guest.h must default GUEST_GPR_FLUSH_MASKED to 0")
    # The GCC proof replaces GUEST_GPR_FLUSH_MASK with its own checks, so the
    # bit -> register map of the real masked macro is checked here: Intel
    # order, as the analysis (and the prelude, through M.GPRS) assume.
    intel = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")
    check(tuple(M.GPRS) == intel, "gpr_flush_mask.GPRS is not Intel order: %r" % (M.GPRS,))
    real = re.search(r"#if GUEST_GPR_FLUSH_MASKED\n#define GUEST_GPR_FLUSH_MASK\(c, m\) \\\n"
                     r"(.*?)\n#else\n", header, re.S)
    pairs = re.findall(r"\(m\) & 0x([0-9A-Fa-f]{2})U \? \(void\)\(\(c\)->(\w+) = _(\w+)\)",
                       real.group(1) if real else "")
    check(sorted((int(b, 16), f, l) for b, f, l in pairs) ==
          [(1 << i, r, r) for i, r in enumerate(intel)],
          "guest.h GUEST_GPR_FLUSH_MASKED=1 macro does not store bit i -> Intel register i: %r"
          % pairs)
    release = read(os.fspath(root / "release" / "v0.1.1-alpha.cmake"))
    check(release.count('set(%s OFF CACHE BOOL "")' % knob) == 1,
          "release recipe must pin %s OFF" % knob)
    gen_all = read(os.fspath(HERE / "gen_all.py"))
    check('"gpr_flush_mask.py"' in gen_all and '"gpr_flush_mask": ' in gen_all,
          "gen_all recipe must record gpr_flush_mask.py and the switch")
    check('os.path.basename(path) != "guest_0144.c"' in gen_all,
          "gen_all must exempt the pinned direct-default owner guest_0144.c")
    readme = read(os.fspath(HERE / "vita" / "README.md"))
    check(knob in readme and "GUEST_GPR_FLUSH_MASK=1" in readme,
          "recomp/vita/README.md must document the knob and its generation switch")
    print("wiring: CMake option/prerequisite/probe/definition, guest.h default, release pin "
          "OFF, gen_all recipe + guest_0144 exemption, README: %s" % (
              "PASS" if not FAILURES else "FAIL"))


# ------------------------------------------------- fact C instrumentation ---
# Callees that receive `c` in masked bodies and cannot write a GPR field
# (read in guest.h/guest.c for this test, not taken from gpr_flush_mask.py):
# the sync macros themselves; fpush/fpop/fst/fcmp_set touch st, st_top and
# fsw only; GUEST_FLAGS_FLUSH/LOAD copy the flags union; guest_fs_base
# refreshes the synthetic TIB and never names a CPU field; guest_fault does
# not return.  Everything else that receives `c` -- translated callees,
# guest_call, int3, cpuid, xgetbv, every gen_all seam helper -- and every
# c->eax..edi/esp/r reference counts as a possible GPR write.
HOST_SAFE_CALLEES = frozenset((
    "GUEST_GPR_FLUSH", "GUEST_GPR_RELOAD", "GUEST_GPR_FLUSH_MASK",
    "fpush", "fpop", "fst", "fcmp_set", "GUEST_FLAGS_FLUSH", "GUEST_FLAGS_LOAD",
    "guest_fs_base", "guest_fault",
))
GPR_FIELDS = frozenset(GPRS) | {"r"}
C_TOKEN_RE = re.compile(r"\(|\)|(?<![\w.>])c\b")
HOSTP = "GPR_PROOF_HOSTP(c)"
# Macros whose production expansion hands `c` to a function although their
# invocation does not spell it (guest.h: GUEST_IMPORT_CALL/JMP ->
# guest_import_call(c, ...) or guest_call(c, ...)).  expansion_census()
# re-derives this set from gcc -E on the corpus and fails when it changes.
HIDDEN_C_MACROS = frozenset(("GUEST_IMPORT_CALL", "GUEST_IMPORT_JMP"))
HIDDEN_C_RE = re.compile(r"\b(%s)\s*\(" % "|".join(sorted(HIDDEN_C_MACROS)))


def host_instrument(text, stats=None):
    """Masked bodies of one unit with every possible host GPR write of `c`
    wrapped in GPR_PROOF_HOSTP, and every HIDDEN_C_MACROS invocation in
    GPR_PROOF_HOST_MACRO (same lines, so __LINE__ still names the site).
    A `c` that is neither `c->field` nor a call argument fails the test.

    Only code after the body's GUEST_GPR_DECL is instrumented.  A seam may
    run before it (guest_0173.c sub_0059c690: `if (... &&
    isaac_vita_wav_buffered_rewind_try(c)) return;` ahead of the DECLs);
    _gpr_host does not exist there yet, and it need not: GUEST_GPR_DECL is
    the first sync and reads every field, so a host write before it reaches
    the locals and the shadows in both builds alike.  That holds only if the
    pre-DECL code cannot jump past the DECL, which is checked."""
    edits = []
    for name, start, end in bodies(text):
        body = text[start:end]
        if not MASK_RE.search(body):
            continue
        code = strip_comments(body)
        decl = re.search(r"\bGUEST_GPR_DECL\b", code)
        check(decl is not None, "%s: masked body without GUEST_GPR_DECL" % name)
        first = decl.end() if decl else 0
        pre = code[:decl.start()] if decl else ""
        check(not re.search(r"\bgoto\b|\bswitch\b|\bcase\b", pre) and not LABEL_RE.search(
            re.sub(r"^\s*\{", "", pre)), "%s: control flow before GUEST_GPR_DECL could skip it" % name)
        if stats is not None and re.search(r"(?<![\w.>])c\b", pre):
            stats["pre-decl c uses"] += 1
        for h in HIDDEN_C_RE.finditer(code, first):
            edits.append((start + h.start(), h.end() - h.start(),
                          "GPR_PROOF_HOST_MACRO(%s, " % h.group(1)))
            if stats is not None:
                stats["macro:" + h.group(1)] += 1
        stack = []
        for t in C_TOKEN_RE.finditer(code, first):
            tok = t.group(0)
            if tok == "(":
                stack.append(t.start())
                continue
            if tok == ")":
                if stack:
                    stack.pop()
                continue
            rest = code[t.end():t.end() + 48]
            field = re.match(r"\s*->\s*(\w+)", rest)
            if field:
                if field.group(1) in GPR_FIELDS:
                    edits.append((start + t.start(), 1, HOSTP))
                    if stats is not None:
                        stats["field:" + field.group(1)] += 1
                continue
            if re.match(r"\s*[,)]", rest) and stack:
                k = stack[-1]
                n = re.search(r"(\w+)\s*\Z", code[max(0, k - 96):k])
                callee = n.group(1) if n else None
                if callee not in HOST_SAFE_CALLEES:
                    edits.append((start + t.start(), 1, HOSTP))
                    if stats is not None:
                        key = "sub_*" if callee and M._SUB_RE.match(callee) else callee
                        stats["call:%s" % key] += 1
                continue
            check(False, "%s: unclassified use of c: %r" % (name, code[t.start():t.start() + 40]))
    out, pos = [], 0
    for off, length, new in sorted(edits):
        out.append(text[pos:off])
        out.append(new)
        pos = off + length
    out.append(text[pos:])
    return "".join(out)


SYNC_MACROS = frozenset(("GUEST_GPR_DECL", "GUEST_GPR_FLUSH", "GUEST_GPR_RELOAD",
                         "GUEST_GPR_FLUSH_MASK"))
NOT_CALLS = frozenset(("if", "while", "for", "switch", "return", "sizeof", "defined",
                       "endif", "__attribute__", "cleanup"))
GPR_FIELD_RE = re.compile(r"(?<![\w.>])c\s*\)?\s*->\s*(?:%s)\b" % "|".join(sorted(GPR_FIELDS)))
BARE_C_RE = re.compile(r"(?<![\w.>])c\b(?!\s*\)?\s*->)")


def _call_text(code, start, open_paren, limit=4096):
    depth = 0
    for i in range(open_paren, min(len(code), open_paren + limit)):
        if code[i] == "(":
            depth += 1
        elif code[i] == ")":
            depth -= 1
            if not depth:
                return code[start:i + 1]
    return None


def expansion_census(cc, gen, units, per_ident=16):
    """host_instrument reads the unexpanded text.  Expand real invocations of
    every identifier called in masked bodies (up to `per_ident` distinct
    spellings each) with gcc -E in the production and vita-seams
    configurations, and require that (a) no expansion names a GPR field of
    `c` beyond what its invocation spells, except the sync macros, and
    (b) the identifiers whose expansion passes `c` that the invocation does
    not spell are exactly HIDDEN_C_MACROS."""
    samples = collections.defaultdict(list)
    seen = collections.Counter()
    for unit in units:
        text = read(os.path.join(gen, unit))
        for name, start, end in bodies(text):
            body = text[start:end]
            if not MASK_RE.search(body):
                continue
            code = strip_comments(body)          # starts after the body's `{`
            for m in CALL_RE.finditer(code):
                ident = m.group(1)
                if ident in NOT_CALLS or M._SUB_RE.match(ident) or ident.startswith("guest_original_"):
                    continue
                have = samples[ident]
                seen[ident] += 1
                # distinct spellings per identifier; an identifier stops being
                # sampled once it is full or 4096 calls brought nothing new
                if len(have) >= per_ident or (seen[ident] > 4096 and have):
                    continue
                call = _call_text(code, m.start(), m.end() - 1)
                if call is None:
                    continue
                call = " ".join(call.split())
                if call not in have:
                    have.append(call)
    names = sorted(samples)
    hidden_all, gpr_bad = set(), []
    for tag, defines in (("production", CONFIGS["production"]),
                         ("vita-seams", CONFIGS["vita-seams"])):
        # Positive controls: one macro hiding `c`, one naming a GPR field.
        lines = ['#include <math.h>', '#include "guest.h"', '#include "guest_funcs.h"',
                 "#define GPR_PROOF_CONTROL_HIDDEN() guest_call(c, 0U)",
                 "#define GPR_PROOF_CONTROL_FIELD() ((c)->ebx = 0U)",
                 "void gpr_proof_expansion_probe(CPU *__restrict c) {",
                 "GUEST_FLAGS_DECL; GUEST_GPR_DECL;"]
        keys = [("GPR_PROOF_CONTROL_HIDDEN", "GPR_PROOF_CONTROL_HIDDEN()"),
                ("GPR_PROOF_CONTROL_FIELD", "GPR_PROOF_CONTROL_FIELD()")]
        for k, (_, call) in enumerate(keys):
            lines.append("gpr_proof_begin_%d; %s; gpr_proof_end;" % (k, call))
        for ident in names:
            for k, call in enumerate(samples[ident]):
                keys.append((ident, call))
                lines.append("gpr_proof_begin_%d; %s; gpr_proof_end;" % (len(keys) - 1, call))
        lines.append("}")
        with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False,
                                         encoding="ascii", newline="\n") as fh:
            fh.write("\n".join(lines) + "\n")
            probe = fh.name
        pp = subprocess.run([cc, "-std=gnu11", "-E", "-P", *PRODUCTION_DEFINES, *defines,
                             "-DGUEST_GPR_FLUSH_MASKED=1", "-I", os.fspath(RUNTIME),
                             "-I", os.fspath(gen), probe],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        os.unlink(probe)
        if pp.returncode:
            check(False, "expansion census [%s]: gcc -E failed:\n%s" % (tag, pp.stdout[-2000:]))
            continue
        expanded = dict((int(m.group(1)), m.group(2)) for m in re.finditer(
            r"gpr_proof_begin_(\d+)\s*;(.*?)gpr_proof_end", pp.stdout, re.S))
        check(len(expanded) == len(keys), "expansion census [%s]: %d of %d probes found" % (
            tag, len(expanded), len(keys)))
        for k, (ident, call) in enumerate(keys):
            exp = expanded.get(k, "")
            if ident not in SYNC_MACROS and \
                    len(GPR_FIELD_RE.findall(exp)) > len(GPR_FIELD_RE.findall(call)):
                gpr_bad.append((tag, ident, call[:80], " ".join(exp.split())[:160]))
            if ident not in SYNC_MACROS and \
                    len(BARE_C_RE.findall(exp)) > len(BARE_C_RE.findall(call)):
                hidden_all.add(ident)
    check({"GPR_PROOF_CONTROL_HIDDEN"} <= hidden_all and
          any(row[1] == "GPR_PROOF_CONTROL_FIELD" for row in gpr_bad),
          "expansion census: the positive controls were not detected")
    hidden_all.discard("GPR_PROOF_CONTROL_HIDDEN")
    hidden_all.discard("GPR_PROOF_CONTROL_FIELD")
    gpr_bad = [row for row in gpr_bad if not row[1].startswith("GPR_PROOF_CONTROL_")]
    present = HIDDEN_C_MACROS & set(names)
    check(not gpr_bad, "expansion census: GPR fields named only by an expansion: %r" % gpr_bad[:6])
    check(hidden_all == present,
          "expansion census: identifiers hiding `c` %r != HIDDEN_C_MACROS present %r" % (
              sorted(hidden_all), sorted(present)))
    print("expansion census: %d identifiers, %d invocations expanded (production + vita-seams); "
          "GPR fields only in sync macros: %s; macros hiding c: %s" % (
              len(names), sum(len(v) for v in samples.values()),
              "yes" if not gpr_bad else "NO", sorted(hidden_all)), flush=True)


# -------------------------------------------------------------- GCC proof ---
def compile_unit(cc, gen, unit_text, unit_name, defines, work, tag):
    """Compile one unit through the check prelude; {(reg, line)} unproven."""
    if tag.startswith("hostflag"):
        unit_text = host_instrument(unit_text)
    src = work / ("%s.%s.c" % (unit_name, tag))
    body = work / ("%s.%s.body.c" % (unit_name, tag))
    body.write_text(unit_text, encoding="utf-8", newline="\n")
    src.write_text(CHECK_PRELUDE + '#include "%s"\n' % body.name,
                   encoding="ascii", newline="\n")
    obj = work / ("%s.%s.o" % (unit_name, tag))
    cmd = [cc, "-std=gnu11", "-O2", "-w", "-c", "-fno-strict-aliasing",
           *PRODUCTION_DEFINES, *defines, "-I", os.fspath(RUNTIME), "-I", os.fspath(gen),
           "-I", os.fspath(work), os.fspath(src), "-o", os.fspath(obj)]
    built = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if built.returncode:
        raise RuntimeError("compile failed %s %s:\n%s" % (unit_name, tag, built.stdout[-3000:]))
    nm = subprocess.run(["nm", "-u", os.fspath(obj)], stdout=subprocess.PIPE, text=True,
                        check=True).stdout
    out = set()
    for m in re.finditer(r"\bgpr_mask_violation_([a-z]+)_(\d+)\b", nm):
        out.add((m.group(1), int(m.group(2))))
    for p in (src, body, obj):
        p.unlink()
    return out


class Lines:
    """1-based line numbers of offsets in one text (bisect over newlines)."""

    def __init__(self, text):
        self.nl = [m.start() for m in re.finditer("\n", text)]

    def __call__(self, pos):
        return bisect.bisect_left(self.nl, pos) + 1


def site_lines(text):
    """{line: [(mask, function)]} of the masked sites of one unit."""
    line_of = Lines(text)
    spans = bodies(text)
    starts = [s for _, s, _ in spans]
    out = collections.defaultdict(list)
    for m in MASK_RE.finditer(text):
        k = bisect.bisect_right(starts, m.start()) - 1
        name = spans[k][0] if k >= 0 else None
        out[line_of(m.start())].append((int(m.group(1), 16), name))
    return out


def prove(cc, gen, jobs, units, configs, unproven_out=None):
    work = pathlib.Path(tempfile.mkdtemp(prefix="gpr-mask-proof-"))
    jobs_list = []
    for unit in units:
        text = read(os.path.join(gen, unit))
        if not MASK_RE.search(text):
            continue
        for tag in configs:
            jobs_list.append((unit, tag, text))
    unproven = collections.defaultdict(set)
    sites = 0
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        futs = {pool.submit(compile_unit, cc, gen, text, unit, CONFIGS[tag], work, tag):
                (unit, tag, text) for unit, tag, text in jobs_list}
        for fut in concurrent.futures.as_completed(futs):
            unit, tag, text = futs[fut]
            try:
                got = fut.result()
            except RuntimeError as exc:
                # One unit that does not build must not discard the others.
                check(False, str(exc)[:3000])
                print("    COMPILE FAILED %s [%s]" % (unit, tag), flush=True)
                continue
            if tag == configs[0]:
                sites += len(MASK_RE.findall(text))
            if got:
                lines = site_lines(text)
                for reg, line in sorted(got):
                    unproven[(unit, tag)].add((reg, line, tuple(lines.get(line, ()))))
    try:
        work.rmdir()
    except OSError:
        pass
    n_bad = 0
    for tag in configs:
        rows = sum(len(v) for k, v in unproven.items() if k[1] == tag)
        print("gcc proof [%s]: %d masked sites, %d unproven (reg, site)" % (
            tag, sites, rows), flush=True)
        n_bad += rows
        check(rows == 0, "gcc could not prove %d skipped stores [%s]" % (rows, tag))
    print("gcc proof: %d unit compiles (%s), %d masked sites, %d unproven (reg, site)" % (
        len(jobs_list), "+".join(configs), sites, n_bad), flush=True)
    for key, rows in sorted(unproven.items()):
        for row in sorted(rows)[:10]:
            print("    UNPROVEN %s [%s] %s line %d %r" % (key[0], key[1], row[0], row[1], row[2]))
    if unproven_out:
        with open(unproven_out, "w", encoding="utf-8") as fh:
            fh.write("\t".join(("unit", "config", "reg", "line", "mask", "function")) + "\n")
            for key, rows in sorted(unproven.items()):
                for reg, line, where in sorted(rows):
                    for mask, fn in where or ((None, None),):
                        fh.write("%s\t%s\t%s\t%d\t%s\t%s\n" % (
                            key[0], key[1], reg, line,
                            "" if mask is None else "0x%02x" % mask, fn))
    return n_bad


def mutate(cc, gen, units, jobs, seed):
    """Positive controls on sampled units."""
    rng = random.Random(seed)
    work = pathlib.Path(tempfile.mkdtemp(prefix="gpr-mask-mut-"))
    tasks = []
    for unit in units:
        text = read(os.path.join(gen, unit))
        sites = list(MASK_RE.finditer(text))
        dirty = [m for m in sites if int(m.group(1), 16)]
        if not dirty:
            continue
        zero = MASK_RE.sub("GUEST_GPR_FLUSH_MASK(c, 0x00U);", text)
        tasks.append(("zero", unit, zero, None))
        # one dropped dirty bit per site, sites on distinct lines only
        pieces, pos, expect, seen_lines = [], 0, set(), set()
        line_of = Lines(text)
        for m in dirty:
            line = line_of(m.start())
            if line in seen_lines:
                continue
            seen_lines.add(line)
            mask = int(m.group(1), 16)
            bits = [i for i in range(8) if mask >> i & 1]
            bit = rng.choice(bits)
            pieces.append(text[pos:m.start()])
            pieces.append("GUEST_GPR_FLUSH_MASK(c, 0x%02xU);" % (mask & ~(1 << bit)))
            pos = m.end()
            expect.add((GPRS[bit], line))
        pieces.append(text[pos:])
        tasks.append(("dropbit", unit, "".join(pieces), expect))
        # fact C controls (hostflag): a translated call whose RELOAD is
        # dropped, and a host CPU access / helper call placed right before
        # a masked FLUSH; each must be reported at the masked site after it.
        noreload = re.sub(r"\b(sub_[0-9a-f]{8}\(c\);) GUEST_GPR_RELOAD\(c\);", r"\1", text)
        if noreload != text:
            tasks.append(("hostflag-noreload", unit, noreload, None))
        # the same through a macro that hides `c` (HIDDEN_C_MACROS)
        noreload = re.sub(r"\b(GUEST_IMPORT_(?:CALL|JMP)\([^;\n]*\);) GUEST_GPR_RELOAD\(c\);",
                          r"\1", text)
        if noreload != text:
            tasks.append(("hostflag-noreload-import", unit, noreload, None))
        pieces, pos, expect = [], 0, set()
        for k, m in enumerate(sites):
            if k % 7:
                continue
            probe = "(void)c->ebx; " if k % 14 == 0 else "gpr_proof_probe(c); "
            pieces.append(text[pos:m.start()])
            pieces.append(probe + "GPR_PROOF_REACH(); " + m.group(0))
            pos = m.end()
            expect.add(("host", line_of(m.start())))
        pieces.append(text[pos:])
        tasks.append(("hostflag-insert", unit, "".join(pieces), expect))
    caught = collections.Counter()
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        futs = {pool.submit(compile_unit, cc, gen, t, u, CONFIGS["production"], work, kind):
                (kind, u, expect) for kind, u, t, expect in tasks}
        for fut in concurrent.futures.as_completed(futs):
            kind, unit, expect = futs[fut]
            got = fut.result()
            if kind == "zero":
                caught["zero_units"] += 1
                caught["zero_flagged"] += len(got)
                check(bool(got), "mutation all-masks-0x00 survived in %s" % unit)
            elif kind in ("hostflag-noreload", "hostflag-noreload-import"):
                host = {g for g in got if g[0] == "host"}
                key = "noreload" if kind == "hostflag-noreload" else "noreload_import"
                caught[key + "_units"] += 1
                caught[key + "_flagged"] += len(host)
                check(bool(host), "mutation %s survived in %s" % (kind, unit))
            elif kind == "hostflag-insert":
                # Every inserted probe whose GPR_PROOF_REACH marker survives
                # (code GCC cannot prove unreachable, in a preprocessor arm
                # this configuration compiles) must be reported at its site.
                # Seen unreached: the `#else` arm of `#if GUEST_LEAF_INLINE`
                # (not compiled in production), and divide-by-zero branches
                # after `mov ecx, <constant>` (folded away).
                reached = {("host", line) for reg, line in got if reg == "reach"}
                host = {g for g in got if g[0] == "host"}
                caught["insert_units"] += 1
                caught["insert_sites"] += len(expect)
                caught["insert_reached"] += len(reached)
                caught["insert_caught"] += len(reached & host)
                check(reached <= expect, "%s: reach markers off the probe lines" % unit)
                check(bool(reached), "%s: no inserted probe is reachable" % unit)
                check(reached <= host, "reachable inserted host accesses not caught in %s: "
                      "%d/%d, lines %r" % (unit, len(reached & host), len(reached),
                                           sorted(l for _, l in reached - host)[:10]))
            else:
                hit = expect & got
                caught["drop_sites"] += len(expect)
                caught["drop_caught"] += len(hit)
                for reg, _ in expect - got:
                    caught["drop_value_equal_" + reg] += 1
    try:
        work.rmdir()
    except OSError:
        pass
    print("mutations: all-masks-0x00 in %d units -> %d flagged (reg, line); one dirty bit "
          "dropped at %d sites -> %d caught" % (
              caught["zero_units"], caught["zero_flagged"], caught["drop_sites"],
              caught["drop_caught"]))
    eq = {k[len("drop_value_equal_"):]: v for k, v in caught.items()
          if k.startswith("drop_value_equal_")}
    print("    dropped bits GCC still proves value-equal (written but unchanged): %r" % eq)
    print("mutations (fact C, hostflag): RELOAD dropped after translated calls in %d units "
          "-> %d masked sites flagged; after GUEST_IMPORT_CALL/JMP in %d units -> %d "
          "flagged; host access inserted before %d masked sites in %d units -> %d reachable, "
          "%d of them caught" % (
              caught["noreload_units"], caught["noreload_flagged"],
              caught["noreload_import_units"], caught["noreload_import_flagged"],
              caught["insert_sites"], caught["insert_units"], caught["insert_reached"],
              caught["insert_caught"]))
    check(caught["noreload_import_units"] > 0,
          "no sampled unit has a GUEST_IMPORT_CALL/JMP RELOAD to drop: control did not run")
    check(not caught["drop_sites"] or caught["drop_caught"] * 2 > caught["drop_sites"],
          "dropped-bit mutations mostly survived: the proof is not sensitive")
    return caught


# ----------------------------------------------------------- OFF identity ---
INSN_RE = re.compile(r"^\s+[0-9a-f]+:\t", re.M)
STORE_RE = re.compile(r"^\s+[0-9a-f]+:\tmov[lq]?\s+%[a-z0-9]+,(?:0x[0-9a-f]+)?\(%[a-z0-9]+\)",
                      re.M)


def object_counts(obj):
    """(instructions, register-to-memory movs) of an x86-64 object (objdump -d).
    A host-side proxy only: the Vita build is ARM."""
    dump = subprocess.run(["objdump", "-d", "--no-show-raw-insn", os.fspath(obj)],
                          stdout=subprocess.PIPE, text=True, check=True).stdout
    return len(INSN_RE.findall(dump)), len(STORE_RE.findall(dump))


def off_identity(cc, gen, off, units, jobs, counts_out=None):
    work = pathlib.Path(tempfile.mkdtemp(prefix="gpr-mask-off-"))

    def build(src_dir, unit, defines, tag, counts=False):
        obj = work / ("%s.%s.o" % (unit, tag))
        cmd = [cc, "-std=gnu11", "-O2", "-w", "-c", "-fno-strict-aliasing",
               *PRODUCTION_DEFINES, "-DGUEST_LEAF_INLINE=1", *defines,
               "-I", os.fspath(RUNTIME), "-I", os.fspath(src_dir),
               os.path.join(src_dir, unit), "-o", os.fspath(obj)]
        subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        data = obj.read_bytes()
        n = object_counts(obj) if counts else None
        obj.unlink()
        # the object embeds no path (no -g); compare the bytes
        return hashlib.sha256(data).hexdigest(), n

    def one(unit):
        base, nb = build(off, unit, [], "base", True)
        masked0, _ = build(gen, unit, ["-DGUEST_GPR_FLUSH_MASKED=0"], "m0")
        masked1, n1 = build(gen, unit, ["-DGUEST_GPR_FLUSH_MASKED=1"], "m1", True)
        return unit, base, masked0, masked1, nb, n1

    same0 = diff1 = 0
    totals = collections.Counter()
    rows = []
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for unit, base, m0, m1, nb, n1 in pool.map(one, units):
            totals["insn_base"] += nb[0]
            totals["insn_m1"] += n1[0]
            totals["store_base"] += nb[1]
            totals["store_m1"] += n1[1]
            rows.append((unit, nb[0], n1[0], nb[1], n1[1]))
            has = bool(MASK_RE.search(read(os.path.join(gen, unit))))
            check(base == m0, "%s: masked corpus with GUEST_GPR_FLUSH_MASKED=0 differs" % unit)
            same0 += base == m0
            diff1 += m1 != base
            if not has:
                check(m1 == base, "%s: no masks but MASKED=1 object differs" % unit)
    try:
        work.rmdir()
    except OSError:
        pass
    print("off identity: %d/%d units object-identical with GUEST_GPR_FLUSH_MASKED=0; "
          "%d differ with =1" % (same0, len(units), diff1))
    print("x86-64 gcc -O2 proxy (not ARM): instructions %d -> %d (%+d), reg->mem movs "
          "%d -> %d (%+d)" % (totals["insn_base"], totals["insn_m1"],
                              totals["insn_m1"] - totals["insn_base"],
                              totals["store_base"], totals["store_m1"],
                              totals["store_m1"] - totals["store_base"]))
    if counts_out:
        with open(counts_out, "w", encoding="utf-8") as fh:
            fh.write("\t".join(("unit", "insn_base", "insn_masked", "store_base",
                                "store_masked")) + "\n")
            for row in sorted(rows):
                fh.write("%s\t%d\t%d\t%d\t%d\n" % row)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--generated-dir", required=True)
    ap.add_argument("--off-generated-dir")
    ap.add_argument("--cc")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--units", type=int, default=0, help="limit to the first N units")
    ap.add_argument("--only", help="comma-separated unit names (guest_0072.c,...)")
    ap.add_argument("--skip-text", action="store_true", help="skip the text checks")
    # hostflag / hostflag-vita-seams keep every fact-L check of production /
    # vita-seams, so the default runs those two plus the #else spelling.
    ap.add_argument("--configs", default="hostflag,hostflag-vita-seams,leaf-inline-off",
                    help="comma-separated GCC proof configurations ('' = no proof)")
    ap.add_argument("--sample-mutations", type=int, default=12)
    ap.add_argument("--off-identity-units", type=int, default=0,
                    help="OFF identity on the first N units (0 = all)")
    ap.add_argument("--seed", type=int, default=20261024)
    ap.add_argument("--report", help="per-function TSV of masked/full/stores removed")
    ap.add_argument("--unproven-report", help="TSV of every site GCC could not prove")
    ap.add_argument("--counts-report", help="per-unit TSV of x86-64 object counts, "
                    "unmasked vs GUEST_GPR_FLUSH_MASKED=1")
    ap.add_argument("--skip-mutations", action="store_true")
    ap.add_argument("--proof-only", action="store_true",
                    help="stop after the GCC proof (no mutations, no OFF identity)")
    args = ap.parse_args()
    wiring_checks()
    gen = args.generated_dir
    units = units_of(gen)
    if args.only:
        units = [u for u in units if u in args.only.split(",")]
    if args.units:
        units = units[:args.units]
    if not args.skip_text:
        text_checks(gen, args.off_generated_dir, args.report)
    CONFIGS["vita-seams"] = seam_config(gen, units_of(gen))
    CONFIGS["hostflag-vita-seams"] = CONFIGS["vita-seams"]
    print("vita-seams config: %d ISAAC_VITA_* names defined" % (len(CONFIGS["vita-seams"]) - 2))
    if args.cc:
        expansion_census(args.cc, gen, units)
        if args.configs:
            prove(args.cc, gen, args.jobs, units, args.configs.split(","),
                  args.unproven_report)
        if args.proof_only:
            print("gpr flush mask proof: %s (proof only)" % ("PASS" if not FAILURES else "FAIL"))
            return 1 if FAILURES else 0
        masked_units = [u for u in units if MASK_RE.search(read(os.path.join(gen, u)))]
        rng = random.Random(args.seed)
        sample = sorted(rng.sample(masked_units, min(args.sample_mutations, len(masked_units))))
        if not args.skip_mutations:
            mutate(args.cc, gen, sample, args.jobs, args.seed)
        if args.off_generated_dir:
            ids = units if not args.off_identity_units else units[:args.off_identity_units]
            off_identity(args.cc, gen, args.off_generated_dir, ids, args.jobs,
                         args.counts_report)
    if FAILURES:
        print("gpr flush mask proof: %d failure(s)" % len(FAILURES))
        return 1
    print("gpr flush mask proof: PASS%s" % ("" if args.cc else " (text only, no --cc)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
