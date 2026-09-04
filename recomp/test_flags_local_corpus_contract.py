#!/usr/bin/env python3
"""Corpus-level contract for the flag-state locals, the general-register
locals and the xmm zero-fill elision.

Runs against a generated Vita corpus (`--generated-dir`) and checks, from the
emitted text alone and independently of the emitter's own analysis:

flag state
  * every translated body opens with `GUEST_FLAGS_DECL;`;
  * no generated statement addresses the flag state through the CPU any more
    (`SET_FLAGS(c,`, `cc_*(c)`, `fl_*(c)`, `c->f_*`): all of it goes through
    GUEST_FL, so one compile definition decides where the state lives;
  * every unresolved indirect JMP (`guest_call(c, X); return;`, the only place
    flags cross a function boundary architecturally) flushes the locals first;
  * a `jmp dword ptr [IAT slot]` (`GUEST_IMPORT_JMP(...)`) is a transfer to a
    host import, which never reads the x86 flags -- exactly as at every
    `call [IAT]` site -- so it must NOT flush (there is nothing to publish:
    the emitter treats it as no flag consumer), and a body that consists of
    that one instruction must not load the flag state at entry either.

general registers (GUEST_GPR_LOCAL)
  The body is read as a two-state machine.  In the locals state (the default)
  the eight registers live in `_eax.._edi`: no statement may spell `c->eax`,
  `c->r[..]` or a synthetic-stack helper directly, may call anything outside
  the emitter's pure helper set, or may `return` -- unless the same statement
  publishes the locals first with `GUEST_GPR_FLUSH(c);` and, when control
  comes back (a call that returns), takes them back with
  `GUEST_GPR_RELOAD(c);`.  A standalone `GUEST_GPR_FLUSH(c);` line opens the
  memory state (a host seam written against the CPU struct): inside it no
  register token may appear, no label may be entered, every `goto` out of it
  must reload first, and the matching standalone `GUEST_GPR_RELOAD(c);` must
  close it at the same brace depth inside the same preprocessor block.
  * every body opens with `GUEST_FLAGS_DECL; GUEST_GPR_DECL;`.

xmm zero-fill elision
  Reported as statistics (loads emitted without the `(xmm_t){0}` fill versus
  fills left); the decision itself is a value-flow analysis over the block
  graph and is gated by test_xmm_zero_elision.py.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

BODY_RE = re.compile(
    r"^void ((?:sub|guest_original)_[0-9a-f]{8})\(CPU \*__restrict c\)\n\{\n(.*?)^\}\n",
    re.M | re.S)
XMM_RE = re.compile(r"c->x\[([0-7])\]")
SCALAR_ACCESSORS = (".f[0]", ".d[0]", ".u32[0]", ".i32[0]")
ELIDED_LOAD_RE = re.compile(
    r"^\s*c->x\[([0-7])\]\.(f|d)\[0\] = (ldf|ldd)\(", re.M)
D0_WRITE_RE = re.compile(r"c->x\[([0-7])\]\.d\[0\] = ")
FORBIDDEN_FLAG_TEXT = (
    "SET_FLAGS(c,", "c->f_a", "c->f_b", "c->f_r", "c->f_op", "c->f_sz",
    "c->f_cf", "c->f_of", "c->f_zf", "c->f_sf", "c->f_pf",
)
FLAG_CALL_RE = re.compile(r"\b(cc|fl)_[a-z]+\(c\)")
TAIL_RE = re.compile(r"^(.*)guest_call\(c, .*\); return;", re.M)

# --- general registers ----------------------------------------------------
# Deliberately re-stated here rather than imported from gpr_locals.py: the
# contract must fail if the generator's own notion of "pure" drifts.
GPR_PURE_CALLS = frozenset((
    "ld8", "ld16", "ld32", "ld64", "ldf", "ldd", "ldx",
    "st8", "st16", "st32", "st64", "stf", "std_", "stx", "st80d",
    "SET_FLAGS", "fpush", "fpop", "fst", "fcmp_set", "guest_fs_base",
    "GUEST_STACK_CALLSITE_BARRIER", "guest_coverage_function",
    "guest_coverage_case", "sqrt", "sqrtf", "fabs", "nearbyint", "UINT64_C",
    "guest_atomic_cmpxchg64", "guest_direct_translated_target",
    "__sync_lock_test_and_set", "__sync_lock_release", "sizeof",
    "GUEST_FLAGS_FLUSH", "GUEST_FLAGS_LOAD", "GUEST_FLAGS_DECL",
    "GUEST_GPR_FLUSH", "GUEST_GPR_RELOAD", "GUEST_GPR_DECL",
    "GR", "GPUSH", "GPOP", "GESP_SET", "GESP_ADJ", "GSTACK_ADDR",
    "if", "while", "for", "switch", "return", "do", "else", "case",
    "defined", "goto",
    # SSE lowering tokens (guest.h, recomp/sse_lower.py): xmm lanes and
    # guest memory only, no general register is read or written.
    "GUEST_XMM_MOV", "GUEST_XMM_MOV0", "GUEST_XMM_LDX", "GUEST_XMM_STX",
    "GUEST_XMM_LANEOP_R", "GUEST_XMM_LANEOP_M",
    "GUEST_XMM_LANEOP0_R", "GUEST_XMM_LANEOP0_M",
    "GUEST_XMM_PANDN_R", "GUEST_XMM_PANDN_M",
    "GUEST_XMM_CVTDQ2PS_R", "GUEST_XMM_CVTDQ2PS_M",
    "GUEST_XMM_CVTDQ2PS0_R", "GUEST_XMM_CVTDQ2PS0_M",
    "GUEST_XMM_PSRLDQ", "GUEST_XMM_PSRA_IMM", "GUEST_XMM_PSHL_IMM",
    "GUEST_XMM_PSRA_REG", "GUEST_XMM_PSHL_REG",
    "GUEST_XMM_PSHUFD_R", "GUEST_XMM_PSHUFD_M",
    "GUEST_XMM_SHUFPS_R", "GUEST_XMM_SHUFPS_M",
    "GUEST_XMM_UNPCK_R", "GUEST_XMM_UNPCK_M",
    "GUEST_XMM_PSHUFW_R", "GUEST_XMM_PSHUFW_M",
    "GUEST_XMM_PUNPCKLDQ_R", "GUEST_XMM_PUNPCKLDQ_M",
    "GUEST_XMM_COMIS_PAIRED", "GUEST_XMM_FCC",
))
GPR_BOUNDARY_RE = re.compile(
    r"\b(?:sub_[0-9a-f]{8}|guest_call|guest_fault|guest_int3|guest_cpuid|"
    r"guest_xgetbv|GUEST_IMPORT_CALL|GUEST_IMPORT_JMP)\s*\(")
IAT_JMP_RE = re.compile(r"^(.*)GUEST_IMPORT_JMP\(.*\); return;", re.M)
GPR_TOKEN_RE = re.compile(
    r"\b(?:GR|GPUSH|GPOP|GESP_SET|GESP_ADJ|GSTACK_ADDR)\(")
GPR_LEGACY_RE = re.compile(
    r"\bc->(?:eax|ecx|edx|ebx|esp|ebp|esi|edi|r\[)|"
    r"\b(?:gpush_generated|gpop_generated|guest_stack_set_generated|"
    r"guest_stack_adjust_generated|guest_stack_address_generated|gpush_at|"
    r"gpop_at|guest_stack_address|guest_stack_set|guest_stack_adjust|gpush|"
    r"gpop)\s*\(")
GPR_CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
GPR_CC_FL_RE = re.compile(r"^(?:cc|fl)_[a-z]+\Z")
GPR_LABEL_RE = re.compile(r"^\s*(?:L_[0-9A-Za-z_]+\s*:|case\b|default\s*:)")
GPR_GOTO_RE = re.compile(r"\bgoto (L_[0-9A-Za-z_]+);")
GPR_SAFE_GOTO_RE = re.compile(
    r"\{ GUEST_GPR_RELOAD\(c\); goto L_[0-9A-Za-z_]+; \}")
GPR_RETURN_OK_RE = re.compile(r"if \(!GESP_(?:SET|ADJ)\(")
_STRING_RE = re.compile(r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)\'')
_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
FLUSH_LINE = "GUEST_GPR_FLUSH(c);"
RELOAD_LINE = "GUEST_GPR_RELOAD(c);"


def fail(message: str) -> None:
    raise SystemExit(message)


def check_flags(name: str, body: str, stats: dict, problems: list) -> None:
    first = body.split("\n", 1)[0].strip()
    if first != "GUEST_FLAGS_DECL;":
        problems.append(f"{name}: body does not open with GUEST_FLAGS_DECL; "
                        f"({first!r})")
    for text in FORBIDDEN_FLAG_TEXT:
        if text in body:
            problems.append(f"{name}: flag state addressed through CPU: {text}")
            break
    if FLAG_CALL_RE.search(body):
        problems.append(f"{name}: flag helper called with the CPU: "
                        f"{FLAG_CALL_RE.search(body).group(0)}")
    for match in TAIL_RE.finditer(body):
        prefix = match.group(1)
        stats["tails"] += 1
        if "GUEST_FLAGS_FLUSH(c);" not in prefix:
            problems.append(
                f"{name}: indirect tail without flag flush: "
                f"{match.group(0).strip()[:100]}")
    iat_jmps = list(IAT_JMP_RE.finditer(body))
    for match in iat_jmps:
        stats["iat_jmps"] += 1
        if "GUEST_FLAGS_FLUSH(c);" in match.group(1):
            problems.append(
                f"{name}: IAT jmp publishes flags to a host import: "
                f"{match.group(0).strip()[:100]}")
    if iat_jmps:
        comments = re.findall(r"^\s*/\* [0-9a-f]{8}  ", body, re.M)
        if len(comments) == 1:
            stats["iat_jmp_thunks"] += 1
            if "GUEST_FLAGS_LOAD(c);" in body:
                problems.append(
                    f"{name}: one-instruction IAT jmp thunk loads flags")
    stats["loads"] += body.count("GUEST_FLAGS_LOAD(c);")
    stats["set_flags"] += body.count("SET_FLAGS(GUEST_FL,")
    stats["consumers"] += len(re.findall(r"\b(cc|fl)_[a-z]+\(GUEST_FL\)", body))


def _code_only(line: str) -> str:
    line = _COMMENT_RE.sub(" ", line)
    if "/*" in line:
        line = line[:line.index("/*")]
    return _STRING_RE.sub('""', line)


def _foreign_calls(code: str):
    out = []
    if code.strip().startswith(("extern ", "static ")):
        return out                         # a declaration, not a call
    for called in GPR_CALL_RE.findall(code):
        if called in GPR_PURE_CALLS or GPR_CC_FL_RE.match(called):
            continue
        if GPR_BOUNDARY_RE.match(called + "("):
            continue
        out.append(called)
    return out


def check_gpr(name: str, body: str, stats: dict, problems: list) -> None:
    lines = body.split("\n")
    head = [line.strip() for line in lines[:2]]
    if head != ["GUEST_FLAGS_DECL;", "GUEST_GPR_DECL;"]:
        problems.append(f"{name}: body does not open with the flag and GPR "
                        f"declarations ({head!r})")
    bad = []

    def problem(index, why):
        bad.append(f"{name}:{index + 1}: {why}: {lines[index].strip()[:110]}")

    mode = "locals"
    depth = 0
    pp_depth = 0
    region_depth = region_pp = None
    in_comment = False
    for index, raw in enumerate(lines):
        text = raw
        if in_comment:
            if "*/" not in text:
                continue
            text = text[text.index("*/") + 2:]
            in_comment = False
        code = _code_only(text)
        if "/*" in _COMMENT_RE.sub(" ", text):
            in_comment = True
        stripped = code.strip()
        if raw.strip().startswith("#"):
            word = raw.strip()[1:].split(None, 1)[0] if raw.strip()[1:].split() else ""
            if word in ("if", "ifdef", "ifndef"):
                pp_depth += 1
            elif word == "endif":
                pp_depth -= 1
                if mode == "memory" and pp_depth < region_pp:
                    problem(index, "seam region leaves its preprocessor block")
            elif word in ("else", "elif"):
                if mode == "memory" and pp_depth == region_pp:
                    problem(index, "seam region crosses #else of its block")
            continue
        if not stripped:
            continue
        if stripped == FLUSH_LINE:
            if mode == "memory":
                problem(index, "nested seam region")
            # A region may only open between complete statements.
            k = index - 1
            while k >= 0 and not _code_only(lines[k]).strip():
                k -= 1
            prev = _code_only(lines[k]).strip() if k >= 0 else ";"
            if not (prev.endswith((";", "{", "}", ":")) or
                    lines[k].strip().startswith("#")):
                problem(index, "seam region opens inside a statement: %r" % prev)
            mode = "memory"
            region_depth, region_pp = depth, pp_depth
            stats["seam_regions"] += 1
            continue
        if stripped == RELOAD_LINE:
            if mode == "locals":
                problem(index, "reload without an open seam region")
            elif depth != region_depth:
                problem(index, "seam region closes at another brace depth")
            mode = "locals"
            continue
        is_label = bool(GPR_LABEL_RE.match(code))
        if mode == "memory":
            bare = GPR_SAFE_GOTO_RE.sub("goto;", code)
            if GPR_TOKEN_RE.search(bare) or "GUEST_GPR_" in bare:
                problem(index, "register token inside a seam region")
            if is_label:
                problem(index, "label inside a seam region")
            for match in GPR_GOTO_RE.finditer(code):
                start = code.rfind("{", 0, match.start())
                end = code.find("}", match.end())
                if (start < 0 or end < 0 or
                        not GPR_SAFE_GOTO_RE.search(code[start:end + 1])):
                    problem(index, "goto out of a seam region without reload")
            stats["seam_lines"] += 1
        else:
            if GPR_LEGACY_RE.search(code):
                problem(index, "register addressed through the CPU in locals state")
            foreign = _foreign_calls(code)
            if foreign:
                problem(index, "host call outside a seam region: %s" % foreign[0])
            has_flush = "GUEST_GPR_FLUSH(c);" in code
            has_reload = "GUEST_GPR_RELOAD(c);" in code
            returns = "return;" in code
            if GPR_BOUNDARY_RE.search(code):
                if not has_flush:
                    problem(index, "boundary call without a preceding flush")
                elif not returns and not has_reload:
                    problem(index, "returning boundary call without a reload")
                stats["boundary_calls"] += 1
                if returns:
                    stats["tail_transfers"] += 1
            if returns and not has_flush and not GPR_RETURN_OK_RE.search(code):
                problem(index, "return without a flush")
            if has_flush:
                stats["inline_flushes"] += 1
        depth += code.count("{") - code.count("}")
    if mode != "locals":
        bad.append(f"{name}: body ends inside a seam region")
    if bad:
        stats["gpr_bodies_failed"] += 1
        problems.extend(bad[:3])
    stats["flushes"] += body.count("GUEST_GPR_FLUSH(c);")
    stats["reloads"] += body.count("GUEST_GPR_RELOAD(c);")


def check_xmm(name: str, body: str, stats: dict, problems: list) -> None:
    """Statistics only: the elision decision follows the value through the
    block graph (emit.xmm_zero_fill_elidable, gated by
    test_xmm_zero_elision.py), so a per-register text rule cannot re-derive
    it.  What can be checked textually: an elided load is a plain scalar
    store into lane 0 and nothing else on that line."""
    for match in ELIDED_LOAD_RE.finditer(body):
        stats["elided_sites"] += 1
        line = body[match.start():body.index("\n", match.end())]
        if "(xmm_t)" in line or line.count("c->x[") != 1:
            problems.append(f"{name}: unexpected elided load shape: {line!r}")
    stats["zero_fills"] += body.count("(xmm_t){0}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--generated-dir", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    units = sorted(arguments.generated_dir.glob("guest_[0-9][0-9][0-9][0-9].c"))
    if not units:
        fail(f"no generated units under {arguments.generated_dir}")
    stats = {"functions": 0, "tails": 0, "loads": 0, "set_flags": 0,
             "iat_jmps": 0, "iat_jmp_thunks": 0,
             "consumers": 0, "elided_sites": 0, "zero_fills": 0,
             "flushes": 0, "reloads": 0, "inline_flushes": 0,
             "boundary_calls": 0, "tail_transfers": 0, "seam_regions": 0,
             "seam_lines": 0, "gpr_bodies_failed": 0}
    problems: list[str] = []
    for path in units:
        source = path.read_bytes().replace(b"\r\n", b"\n").decode("utf-8")
        for match in BODY_RE.finditer(source):
            name, body = match.group(1), match.group(2)
            stats["functions"] += 1
            check_flags(name, body, stats, problems)
            check_gpr(name, body, stats, problems)
            check_xmm(name, body, stats, problems)
    for line in problems[:40]:
        print("FAIL", line, file=sys.stderr)
    print("flags-local corpus contract: %d units, %d functions, "
          "%d SET_FLAGS, %d cc_/fl_ consumers, %d indirect tails flushed, "
          "%d IAT jmps unflushed (%d one-instruction thunks without load), "
          "%d entry loads; xmm loads without zero fill: %d, zero fills left: %d"
          % (len(units), stats["functions"], stats["set_flags"],
             stats["consumers"], stats["tails"], stats["iat_jmps"],
             stats["iat_jmp_thunks"], stats["loads"],
             stats["elided_sites"], stats["zero_fills"]))
    print("gpr-local corpus contract: %d GUEST_GPR_FLUSH, %d GUEST_GPR_RELOAD; "
          "%d boundary calls (%d tail transfers), %d inline flushes, "
          "%d host seam regions (%d lines); bodies failing: %d"
          % (stats["flushes"], stats["reloads"], stats["boundary_calls"],
             stats["tail_transfers"], stats["inline_flushes"],
             stats["seam_regions"], stats["seam_lines"],
             stats["gpr_bodies_failed"]))
    if problems:
        fail("flags/gpr-local corpus contract: %d violations" % len(problems))
    print("flags-local corpus contract: PASS")
    print("gpr-local corpus contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
