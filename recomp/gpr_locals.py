"""General registers as C locals: the spelling map and the seam-sync pass.

The emitter names guest registers through tokens (`GR(eax)`, `GPUSH(v)`,
`GPOP()`, `GESP_SET(v)`, `GESP_ADJ(n)`, `GSTACK_ADDR(a, n)`) and marks every
boundary it creates itself -- translated calls, guest_call (including its
IAT-site spelling `GUEST_IMPORT_CALL/GUEST_IMPORT_JMP`), guest_fault,
guest_int3, guest_cpuid/xgetbv, return -- with `GUEST_GPR_FLUSH(c)` before and
`GUEST_GPR_RELOAD(c)` after.  guest.h maps the tokens either to the exact
`c->reg` / `_generated` helper text generated code always had
(GUEST_GPR_LOCAL=0) or to eight per-body locals (GUEST_GPR_LOCAL=1).  Two
more emitter spellings ride on the same normalisers: the dual-spelled
inlined leaf site (`#if GUEST_LEAF_INLINE`, leaf_inline.py) and the SSE
lowering tokens `GUEST_XMM_*` (sse_lower.py).

Two things need to know about that spelling outside emit.py:

  * gen_all's pins compare emitted statements against strings frozen in the
    old spelling.  `legacy_statements()` maps emitter output back exactly, so
    a pin stays a pin and also stays usable as seam text.

  * gen_all's host seams are written in the old spelling on purpose: inside a
    seam the CPU struct is the register file.  `bracket_seam_regions()` finds
    every such line in a rendered body and encloses it -- grown to the
    smallest brace- and preprocessor-balanced sequence of complete statements
    around it -- in FLUSH ... RELOAD, rewriting any `goto` that leaves the
    region so the locals are current on arrival.  A line it cannot enclose
    that way is an error: the seam has to be restructured, never silently
    half-synchronised.
"""
from __future__ import annotations

import re

import sse_lower

GPRS = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")

_GR_RE = re.compile(r"\bGR\((eax|ecx|edx|ebx|esp|ebp|esi|edi)\)")
_TOKEN_SIMPLE = (
    ("GPUSH(", "gpush_generated(c, "),
    ("GPOP()", "gpop_generated(c)"),
    ("GESP_SET(", "guest_stack_set_generated(c, "),
    ("GESP_ADJ(", "guest_stack_adjust_generated(c, "),
    ("GSTACK_ADDR(", "guest_stack_address_generated(c, "),
)


_IMPORT_TOKEN_RE = re.compile(r"\bGUEST_IMPORT_(CALL|JMP)\(")


def _import_token_arguments(text: str, start: int):
    """`text[start:]` follows `GUEST_IMPORT_xxx(`: return the top-level
    comma-separated arguments and the index just past the closing paren."""
    depth = 0
    cursor = start
    arguments = []
    for index in range(start, len(text)):
        char = text[index]
        if char == "(":
            depth += 1
        elif char == ")":
            if depth == 0:
                arguments.append(text[cursor:index].strip())
                return arguments, index + 1
            depth -= 1
        elif char == "," and depth == 0:
            arguments.append(text[cursor:index].strip())
            cursor = index + 1
    raise ValueError("unterminated GUEST_IMPORT token: %r" % text[start:])


def legacy_import_tokens(text: str) -> str:
    """`GUEST_IMPORT_CALL(target, slot, id)` -> `guest_call(c, target)` and
    `GUEST_IMPORT_JMP(target, slot, id)` -> the `GUEST_FLAGS_FLUSH(c);
    guest_call(c, target)` an unresolved indirect JMP always spelled: the
    IAT direct path (guest.h) is transparent to every pin and to packing."""
    if "GUEST_IMPORT_" not in text:
        return text
    pieces = []
    position = 0
    for match in _IMPORT_TOKEN_RE.finditer(text):
        if match.start() < position:
            continue
        arguments, end = _import_token_arguments(text, match.end())
        if len(arguments) != 3:
            raise ValueError("GUEST_IMPORT token needs three arguments: %r"
                             % text[match.start():end])
        pieces.append(text[position:match.start()])
        if match.group(1) == "JMP":
            pieces.append("GUEST_FLAGS_FLUSH(c); guest_call(c, %s)"
                          % arguments[0])
        else:
            pieces.append("guest_call(c, %s)" % arguments[0])
        position = end
    pieces.append(text[position:])
    return "".join(pieces)


# A direct call whose tiny leaf callee was copied in place (leaf_inline.py) is
# one statement carrying both spellings behind `#if GUEST_LEAF_INLINE`; the
# `#else` branch is the exact pre-inlining call, so the block collapses to it.
_LEAF_INLINE_SITE_RE = re.compile(
    r"(GPUSH\(0x[0-9a-fA-F]+U\); GUEST_STACK_CALLSITE_BARRIER\(\);)\n"
    r"[ \t]*#if GUEST_LEAF_INLINE\n"
    r"(?:(?![ \t]*#)[^\n]*\n)*?"
    r"[ \t]*#else\n"
    r"[ \t]*(GUEST_GPR_FLUSH\(c\); sub_[0-9a-f]{8}\(c\); GUEST_GPR_RELOAD\(c\);)\n"
    r"[ \t]*#endif")


def collapse_leaf_inline(text: str) -> str:
    """Every dual-spelled inlined leaf site -> its single-statement call."""
    if "#if GUEST_LEAF_INLINE" not in text:
        return text
    return _LEAF_INLINE_SITE_RE.sub(r"\1 \2", text)


def legacy_spelling(text: str) -> str:
    """Emitter output -> the exact text the emitter produced before tokens.

    Inline sync macros are removed (they were not there), the register token
    becomes the CPU field, the stack tokens become their helper calls and the
    IAT import tokens become the guest_call they guard and the SSE lowering
    tokens become the lane loops / union copies / lazy comis producer and
    cc_* consumer they stand for (sse_lower.legacy_sse_tokens).  Seam-owned
    text never contains tokens, so this is the identity on it.

    Order matters only where one spelling nests another: an inlined leaf copy
    may itself carry SSE tokens and GPR tokens, so the dual-spelled site
    collapses to its single call first; the IAT and SSE tokens are
    independent of each other and of the register/stack tokens they may
    carry as arguments (`GUEST_XMM_LDX(0, GR(eax) + 16U)`), which are mapped
    last.
    """
    text = collapse_leaf_inline(text)
    text = text.replace("GUEST_GPR_FLUSH(c); ", "")
    text = text.replace(" GUEST_GPR_RELOAD(c);", "")
    text = legacy_import_tokens(text)
    text = sse_lower.legacy_sse_tokens(text)
    text = _GR_RE.sub(r"c->\1", text)
    for token, legacy in _TOKEN_SIMPLE:
        text = text.replace(token, legacy)
    return text


def legacy_statements(statements):
    return tuple(legacy_spelling(statement) for statement in statements)


_STANDALONE_SYNC_RE = re.compile(
    r"^[ \t]*GUEST_GPR_(?:FLUSH|RELOAD|DECL)\(?c?\)?;[ \t]*\n", re.M)
_SAFE_GOTO_RE = re.compile(r"\{ GUEST_GPR_RELOAD\(c\); (goto L_[0-9A-Za-z_]+;) \}")


def legacy_text(text: str) -> str:
    """A whole rendered body/unit -> what gen_all rendered before the GPR
    locals: the declaration line, the seam brackets and the goto reloads are
    removed and every statement is spelled the old way.  For frozen-shape
    tests that pin seam text; the sync lines themselves are the contract of
    test_flags_local_corpus_contract.py."""
    text = _STANDALONE_SYNC_RE.sub("", text)
    text = _SAFE_GOTO_RE.sub(r"\1", text)
    return legacy_spelling(text)


# ------------------------------------------------------------------------
# Line classification for the seam pass.

# Helpers that never read or write a general register (guest memory, the
# flag state, x87, coverage, the TIB base, libm) plus the tokens and C
# keywords that look like calls.  Everything else that is called from a seam
# may see the CPU struct and needs the bracket.
PURE_CALLS = frozenset((
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
    # SSE lowering tokens (guest.h): xmm lanes and guest memory only.
    *sse_lower.PURE_NAMES,
))
# Boundary calls the emitter itself synchronises inline (GUEST_IMPORT_CALL/
# GUEST_IMPORT_JMP are the guarded guest_call of an IAT site, guest.h).
BOUNDARY_CALLS_RE = re.compile(
    r"\b(?:sub_[0-9a-f]{8}|guest_call|guest_fault|guest_int3|guest_cpuid|"
    r"guest_xgetbv|GUEST_IMPORT_CALL|GUEST_IMPORT_JMP)\s*\(")
TOKEN_RE = re.compile(
    r"\b(?:GR|GPUSH|GPOP|GESP_SET|GESP_ADJ|GSTACK_ADDR)\(|"
    r"\bGUEST_GPR_(?:FLUSH|RELOAD|DECL)\b|\bGUEST_FLAGS_(?:FLUSH|LOAD|DECL)\b|"
    r"\bGUEST_FL\b")
LEGACY_GPR_RE = re.compile(
    r"\bc->(?:eax|ecx|edx|ebx|esp|ebp|esi|edi|r\[)")
LEGACY_STACK_RE = re.compile(
    r"\b(?:gpush_generated|gpop_generated|guest_stack_set_generated|"
    r"guest_stack_adjust_generated|guest_stack_address_generated|gpush_at|"
    r"gpop_at|guest_stack_address|guest_stack_set|guest_stack_adjust|gpush|"
    r"gpop)\s*\(")
CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
CC_FL_RE = re.compile(r"^(?:cc|fl)_[a-z]+\Z")
LABEL_RE = re.compile(r"^\s*(?:L_[0-9A-Za-z_]+\s*:|case\b|default\s*:)")
GOTO_RE = re.compile(r"\bgoto (L_[0-9A-Za-z_]+);")
RETURN_RE = re.compile(r"\breturn;")
RETURN_OK_RE = re.compile(r"if \(!GESP_(?:SET|ADJ)\(")
_STRING_RE = re.compile(r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)\'')
_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)

FLUSH_STATEMENT = "GUEST_GPR_FLUSH(c);"
RELOAD_STATEMENT = "GUEST_GPR_RELOAD(c);"


def code_only(line: str) -> str:
    """The line without string/char literals and without inline comments."""
    line = _COMMENT_RE.sub(" ", line)
    if "/*" in line:
        line = line[:line.index("/*")]
    return _STRING_RE.sub('""', line)


def foreign_calls(code: str):
    """Identifiers called on this (literal-free) line that are not known to
    leave the general registers alone and are not emitter boundaries."""
    out = []
    for name in CALL_RE.findall(code):
        if name in PURE_CALLS or CC_FL_RE.match(name):
            continue
        if BOUNDARY_CALLS_RE.match(name + "("):
            continue
        out.append(name)
    return out


def is_hard(code: str) -> bool:
    """Memory-mode content: the CPU struct is the register file here."""
    if LEGACY_GPR_RE.search(code) or LEGACY_STACK_RE.search(code):
        return True
    stripped = code.strip()
    if stripped.startswith("extern ") or stripped.startswith("static "):
        return False                       # a declaration, not a call
    if foreign_calls(code):
        return True
    if BOUNDARY_CALLS_RE.search(code) and "GUEST_GPR_FLUSH(c)" not in code:
        return True
    if (RETURN_RE.search(code) and "GUEST_GPR_FLUSH(c)" not in code and
            not RETURN_OK_RE.search(code)):
        return True                        # a return must publish the locals
    return False


class SeamSyncError(RuntimeError):
    pass


def _pp_kind(stripped: str):
    if not stripped.startswith("#"):
        return None
    words = stripped[1:].split()
    word = words[0] if words else ""
    if word in ("if", "ifdef", "ifndef"):
        return "open"
    if word in ("else", "elif"):
        return "alt"
    if word == "endif":
        return "close"
    return "other"


def classify(lines, name="?"):
    """Per line: literal-free code, kind and preprocessor kind.

    kind is one of "pp", "label", "token" (an emitter statement, spelled
    through the tokens), "hard" (memory-mode seam content) or "pure"."""
    codes, kinds, pp_kinds = [], [], []
    in_comment = False
    for line in lines:
        text = line
        if in_comment:
            if "*/" in text:
                text = text[text.index("*/") + 2:]
                in_comment = False
            else:
                codes.append("")
                kinds.append("pure")
                pp_kinds.append(None)
                continue
        code = code_only(text)
        if "/*" in _COMMENT_RE.sub(" ", text):
            in_comment = True
        pk = _pp_kind(line.strip())
        codes.append(code)
        pp_kinds.append(pk)
        if pk is not None:
            kinds.append("pp")
        elif LABEL_RE.match(code):
            kinds.append("label")
        elif TOKEN_RE.search(code):
            if is_hard(code):
                raise SeamSyncError(
                    "%s: mixed register spellings on one line: %r"
                    % (name, line.strip()))
            kinds.append("token")
        elif is_hard(code):
            kinds.append("hard")
        else:
            kinds.append("pure")
    return codes, kinds, pp_kinds


def bracket_seam_regions(lines, name="?"):
    """Enclose every memory-mode line of a rendered body in FLUSH ... RELOAD.

    `lines` are the statements between the body's braces (labels, comments
    and preprocessor lines included).  Returns a new list.
    """
    n = len(lines)
    codes, kinds, pp_kinds = classify(lines, name)
    BACK, FWD, FATAL = "back", "fwd", "fatal"

    def brace_delta(code):
        return code.count("{") - code.count("}")

    def region_ok(a, b):
        """Brace- and preprocessor-balanced sequence of complete statements
        from a to b inclusive; returns (reason, direction to grow) or None."""
        if kinds[a] == "pp":
            return "starts on a preprocessor line", BACK
        if kinds[b] == "pp":
            return "ends on a preprocessor line", FWD
        depth = 0
        pp_depth = 0
        for k in range(a, b + 1):
            if kinds[k] in ("label", "token"):
                return "run crosses a label or an emitter statement", FATAL
            pk = pp_kinds[k]
            if pk == "open":
                pp_depth += 1
            elif pk == "close":
                pp_depth -= 1
                if pp_depth < 0:
                    return "run leaves its preprocessor block", BACK
            elif pk == "alt" and pp_depth == 0:
                return "run crosses #else/#elif of its own block", FATAL
            depth += brace_delta(codes[k])
            if depth < 0:
                return "closes a brace opened before the run", BACK
        if pp_depth != 0:
            return "unterminated nested preprocessor block", FWD
        if depth != 0:
            return "opens a brace it does not close", FWD
        last = codes[b].strip()
        if not (last.endswith(";") or last.endswith("}")):
            return "last line is not a complete statement: %r" % last, FWD
        first = codes[a].strip()
        if first.startswith("else") or first.startswith("}"):
            return "first line continues a previous statement: %r" % first, BACK
        # The statement before the region must be complete: a line ending in
        # `=`, `(`, `,`, `)` (an if/while header), `else`, `do` or any other
        # operator continues into the region's first line (`const int x =`
        # followed by the call was one real case).  Comment-only lines are
        # skipped when looking for it.
        k = a - 1
        while k >= 0 and kinds[k] not in ("pp", "label") and not codes[k].strip():
            k -= 1
        if k >= 0 and kinds[k] not in ("pp", "label"):
            prev = codes[k].strip()
            if not (prev.endswith(";") or prev.endswith("{") or
                    prev.endswith("}")):
                return "preceded by an unfinished statement: %r" % prev, BACK
        k = b + 1
        while k < n and kinds[k] not in ("pp", "label") and not codes[k].strip():
            k += 1
        if k < n and kinds[k] not in ("pp", "label"):
            nxt = codes[k].strip()
            if nxt.startswith("else"):
                return "followed by an else: %r" % nxt, FWD
        return None

    out = []
    i = 0
    last_region_end = -2
    while i < n:
        if kinds[i] != "hard":
            out.append(lines[i])
            i += 1
            continue
        # The segment is bounded by label/emitter lines on both sides; the
        # region starts as this one line and grows only as far as the shape
        # demands, so seams in different preprocessor blocks stay apart.
        seg_first = i
        while seg_first - 1 >= 0 and kinds[seg_first - 1] not in ("label", "token"):
            seg_first -= 1
        seg_end = i
        while seg_end + 1 < n and kinds[seg_end + 1] not in ("label", "token"):
            seg_end += 1
        a = b = i
        verdict = region_ok(a, b)
        while verdict is not None:
            reason, direction = verdict
            if direction == BACK and a - 1 >= seg_first:
                a -= 1
            elif direction == FWD and b + 1 <= seg_end:
                b += 1
            else:
                raise SeamSyncError(
                    "%s: cannot bracket host seam (%s):\n%s"
                    % (name, reason,
                       "\n".join(lines[max(a - 2, 0):min(b + 3, n)])))
            verdict = region_ok(a, b)
        # Lines between a and i were already copied out; take them back.
        if i > a:
            del out[len(out) - (i - a):]
        indent = lines[a][:len(lines[a]) - len(lines[a].lstrip())]
        if (a == last_region_end + 1 and out and
                out[-1].strip() == RELOAD_STATEMENT):
            out.pop()                      # adjacent regions: one bracket
        else:
            out.append(indent + FLUSH_STATEMENT)
        for k in range(a, b + 1):
            line = lines[k]
            if kinds[k] != "pp" and GOTO_RE.search(codes[k]):
                line = GOTO_RE.sub(r"{ GUEST_GPR_RELOAD(c); goto \1; }", line)
            out.append(line)
        out.append(indent + RELOAD_STATEMENT)
        last_region_end = b
        i = b + 1
    return out
