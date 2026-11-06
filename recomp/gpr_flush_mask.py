"""Dirty-register publication masks for GPR-locals bodies (GUEST_GPR_FLUSH_MASK).

Why.  Under GUEST_GPR_LOCAL=1 every boundary publishes the eight register
locals with GUEST_GPR_FLUSH(c) (eight stores).  GCC drops a store only when it
can prove the CPU field still holds the local, and it almost never can: the
first guest store (every `push` is one) goes through an integer-cast pointer
that may alias `*c` as far as GCC knows.  Measured on the P2b corpus objects,
24 % of the sample-weighted ARM instructions of the hot translated functions
are CPU-struct GPR loads/stores, and the disassembly shows whole FLUSHes of
registers the body never wrote (sub_0056d8c0's early return stores all eight).

What.  A forward may-dirty analysis over each rendered body.  A register is
*clean* at a point when every path from the last synchronisation (body entry
GUEST_GPR_DECL, GUEST_GPR_RELOAD, GUEST_GPR_FLUSH) to that point leaves its
local unwritten, i.e. `c->reg == _reg` holds there.  Each FLUSH is rewritten to
GUEST_GPR_FLUSH_MASK(c, 0xMMU), bit i = register i in Intel order (eax..edi),
naming exactly the possibly-dirty registers; guest.h stores only those under
GUEST_GPR_FLUSH_MASKED=1 and the full eight otherwise, so a corpus carrying
the masks still builds the old objects with the header knob off.

Exactness.  Storing a clean register writes the value the field already
holds, so omitting it leaves every CPU field identical at every boundary.
That needs (1) every write of a local to be seen, and (2) nothing but FLUSH /
RELOAD / the boundary callee to touch the GPR fields of `*c` inside the body.
Both are enforced fail-closed on the rendered text: a body is masked only if
every called identifier is emitter vocabulary (seams and host helpers make
the whole body keep its full FLUSHes), no `c->eax`-style field is named, the
only preprocessor conditionals are the GUEST_LEAF_INLINE dual spelling, and
every call that receives the CPU and may change registers (translated callee,
guest_call, import, int3, cpuid) runs with the file published and is followed
by RELOAD or `return`.  Guest memory never aliases the host CPU struct; GPR
locals mode already relies on that.

The generation switch is the environment variable GUEST_GPR_FLUSH_MASK=1; off
(the default) the pass is not run and the corpus is byte-identical.
"""
from __future__ import annotations

import os
import re

ENVIRONMENT_SWITCH = "GUEST_GPR_FLUSH_MASK"
GPRS = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")
BIT = {r: 1 << i for i, r in enumerate(GPRS)}
ESP = BIT["esp"]
FULL = 0xFF
# Analysis-only bit: host code ran since the last synchronisation and may have
# written CPU GPR fields directly, so `c->reg == _reg` is no longer known.
HOST = 0x100
FLUSH_RE = re.compile(r"\bGUEST_GPR_FLUSH\(c\);")
MASK_TOKEN = "GUEST_GPR_FLUSH_MASK(c, 0x%02xU);"
MASK_TOKEN_RE = re.compile(r"\bGUEST_GPR_FLUSH_MASK\(c, 0x[0-9a-f]{2}U\);")


def enabled_by_environment() -> bool:
    return os.environ.get(ENVIRONMENT_SWITCH, "0") == "1"


def legacy_text(text: str) -> str:
    """The corpus text the pass started from (every mask back to FLUSH)."""
    return MASK_TOKEN_RE.sub("GUEST_GPR_FLUSH(c);", text)


# ----------------------------------------------------------- vocabulary ---
# Called identifiers a masked body may contain (the emitter's own spelling).
# Value-only helpers: they read their arguments and never write a register
# local or a GPR field of the CPU.
_VALUE_CALLS = frozenset((
    "GR", "ld8", "ld16", "ld32", "ld64", "ldf", "ldd", "ldx",
    "st8", "st16", "st32", "st64", "stf", "std_", "stx", "GSTACK_ADDR",
    "SET_FLAGS", "GUEST_STACK_CALLSITE_BARRIER", "guest_coverage_function",
    "guest_coverage_case", "sqrt", "sqrtf", "fabs", "fabsf", "nearbyint",
    "nearbyintf", "floor", "floorf", "UINT64_C", "guest_atomic_cmpxchg64",
    "__sync_lock_test_and_set", "__sync_lock_release", "sizeof",
    "if", "while", "for", "switch", "return",
))
# Receive the CPU but touch only x87 / flags / TIB state, or never return.
_CPU_NEUTRAL_CALLS = frozenset((
    "GUEST_FLAGS_FLUSH", "GUEST_FLAGS_LOAD", "guest_fs_base", "fpop", "fpush",
    "fst", "fcmp_set", "guest_fault",
))
# May read and write every register: legal only right after a publication
# (no local written since) and followed by RELOAD or `return`.
_BOUNDARY_CALLS = frozenset((
    "guest_call", "GUEST_IMPORT_CALL", "GUEST_IMPORT_JMP", "guest_int3",
    "guest_cpuid", "guest_xgetbv",
))
_STACK_WRITERS = frozenset(("GPUSH", "GPOP", "GESP_SET", "GESP_ADJ"))
_SYNC = frozenset(("GUEST_GPR_FLUSH", "GUEST_GPR_RELOAD", "GUEST_GPR_DECL"))
_ASSIGN_OPS = frozenset(("=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
                         "<<=", ">>=", "++", "--"))
_SUB_RE = re.compile(r"^(sub_[0-9a-f]{8}|guest_original_[0-9a-f]{8})\Z")
_LABEL_RE = re.compile(r"^L_[0-9A-Za-z_]+\Z")
_CPU_GPR_FIELD = frozenset(GPRS) | {"r"}


class Refuse(Exception):
    """The body leaves the vocabulary the analysis can prove; keep it as is."""


# ------------------------------------------------------------- tokenizer ---
_TOKEN_RE = re.compile(r"""
    (?P<ws>[ \t\r]+) |
    (?P<nl>\n) |
    (?P<comment>/\*.*?\*/) |
    (?P<line_comment>//[^\n]*) |
    (?P<str>"(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*') |
    (?P<id>[A-Za-z_][A-Za-z0-9_]*) |
    (?P<num>(?:0[xX][0-9A-Fa-f]+|\d+(?:\.\d*)?(?:[eE][-+]?\d+)?)[uUlLfF]*) |
    (?P<op><<=|>>=|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=|[-+*/%&|^~!<>=?:;,.(){}\[\]])
""", re.S | re.X)


def tokenize(body: str):
    """[(kind, text, start)]; preprocessor lines become one 'pp' token."""
    out, pos, n, bol = [], 0, len(body), True
    while pos < n:
        if bol:
            m = re.match(r"[ \t]*#[^\n]*", body[pos:])
            if m:
                out.append(("pp", m.group(0).strip(), pos))
                pos += m.end()
                continue
        m = _TOKEN_RE.match(body, pos)
        if not m:
            raise Refuse("untokenizable text at %d: %r" % (pos, body[pos:pos + 20]))
        kind = m.lastgroup
        if kind == "nl":
            bol = True
        elif kind not in ("ws", "comment", "line_comment"):
            bol = False
            out.append((kind, m.group(0), pos))
        pos = m.end()
    return out


# ---------------------------------------------------------------- parser ---
class Parser:
    def __init__(self, toks):
        self.t = toks
        self.i = 0

    def peek(self, k=0):
        j = self.i + k
        return self.t[j] if j < len(self.t) else ("eof", "", -1)

    def take(self, text=None):
        tok = self.peek()
        if text is not None and tok[1] != text:
            raise Refuse("expected %r, got %r" % (text, tok[1]))
        self.i += 1
        return tok

    def balanced(self, open_, close):
        start = self.i
        self.take(open_)
        depth = 1
        while depth:
            tok = self.take()
            if tok[0] == "eof":
                raise Refuse("unbalanced " + open_)
            if tok[0] == "pp":
                raise Refuse("preprocessor line inside an expression")
            if tok[1] == open_:
                depth += 1
            elif tok[1] == close:
                depth -= 1
        return self.t[start + 1:self.i - 1]

    def statements(self, stop):
        out = []
        while self.peek()[1] not in stop and not (self.peek()[0] == "pp" and self.peek()[1] in stop):
            if self.peek()[0] == "eof":
                if "eof" in stop:
                    break
                raise Refuse("unexpected end of body")
            out.append(self.statement())
        return out

    def statement(self):
        kind, text, _ = self.peek()
        if kind == "pp":
            return self.pp_block()
        if text == "{":
            self.take("{")
            body = self.statements(("}",))
            self.take("}")
            return ("block", body)
        if text == ";":
            self.take()
            return ("empty",)
        if kind == "id":
            if text == "if":
                self.take()
                cond = self.balanced("(", ")")
                then = self.statement()
                other = None
                if self.peek()[1] == "else":
                    self.take()
                    other = self.statement()
                return ("if", cond, then, other)
            if text == "while":
                self.take()
                cond = self.balanced("(", ")")
                return ("loop", [], cond, [], self.statement(), False)
            if text == "for":
                self.take()
                head = self.balanced("(", ")")
                parts, depth = [[]], 0
                for tok in head:
                    if tok[1] in "([{":
                        depth += 1
                    elif tok[1] in ")]}":
                        depth -= 1
                    if tok[1] == ";" and depth == 0:
                        parts.append([])
                    else:
                        parts[-1].append(tok)
                if len(parts) != 3:
                    raise Refuse("for header")
                return ("loop", parts[0], parts[1], parts[2], self.statement(), False)
            if text == "do":
                self.take()
                body = self.statement()
                self.take("while")
                cond = self.balanced("(", ")")
                self.take(";")
                return ("loop", [], cond, [], body, True)
            if text == "switch":
                self.take()
                cond = self.balanced("(", ")")
                return ("switch", cond, self.statement())
            if text == "case":
                self.take()
                while self.peek()[1] != ":":
                    if self.peek()[0] in ("eof", "pp"):
                        raise Refuse("case label")
                    self.take()
                self.take(":")
                return ("case", False)
            if text == "default" and self.peek(1)[1] == ":":
                self.take(); self.take(":")
                return ("case", True)
            if text == "goto":
                self.take()
                name = self.take()[1]
                self.take(";")
                return ("goto", name)
            if text == "return":
                self.take()
                while self.peek()[1] != ";":
                    if self.peek()[0] in ("eof", "pp"):
                        raise Refuse("return")
                    self.take()
                self.take(";")
                return ("return",)
            if text in ("break", "continue"):
                self.take(); self.take(";")
                return (text,)
            if self.peek(1)[1] == ":" and _LABEL_RE.match(text):
                self.take(); self.take(":")
                return ("label", text)
            if text in ("else", "default"):
                raise Refuse("stray " + text)
        return self.expression_statement()

    def expression_statement(self):
        start = self.i
        depth = 0
        while True:
            tok = self.take()
            if tok[0] == "eof":
                raise Refuse("unterminated statement")
            if tok[0] == "pp":
                raise Refuse("preprocessor line inside a statement")
            if tok[1] in "([{":
                depth += 1
            elif tok[1] in ")]}":
                depth -= 1
                if depth < 0:
                    raise Refuse("unbalanced statement")
            elif tok[1] == ";" and depth == 0:
                return ("expr", self.t[start:self.i - 1], self.t[start][2], tok[2] + 1)

    def pp_block(self):
        """#if/#ifdef/#ifndef ... [#elif ...] [#else] #endif around whole
        statements: every branch is analysed as a possible path (the build
        selects one), plus the empty path when there is no #else."""
        head = self.take()[1]
        if not re.match(r"#\s*(if|ifdef|ifndef)\b", head):
            raise Refuse("preprocessor line %r" % head)
        branches, has_else = [], False
        while True:
            branches.append(self.pp_statements())
            tok = self.take()
            if tok[0] != "pp":
                raise Refuse("unterminated preprocessor conditional")
            if re.match(r"#\s*endif\b", tok[1]):
                break
            if re.match(r"#\s*else\b", tok[1]):
                has_else = True
            elif not re.match(r"#\s*elif\b", tok[1]):
                raise Refuse("preprocessor line %r" % tok[1])
        if not has_else:
            branches.append([])
        return ("pp", branches)

    def pp_statements(self):
        out = []
        while True:
            kind, text, _ = self.peek()
            if kind == "eof":
                raise Refuse("unterminated preprocessor conditional")
            if kind == "pp" and re.match(r"#\s*(else|elif|endif)\b", text):
                return out
            out.append(self.statement())


# --------------------------------------------------------------- analysis ---
def _union(a, b):
    if a is None:
        return b
    if b is None:
        return a
    return a | b


def _calls(toks):
    """(identifier, index) for every `ident (` in a token list."""
    for j, tok in enumerate(toks[:-1]):
        if tok[0] == "id" and toks[j + 1][1] == "(":
            yield tok[1], j


def _writes(toks):
    mask = 0
    for j, tok in enumerate(toks):
        if tok[0] != "id":
            continue
        if tok[1] in _STACK_WRITERS and j + 1 < len(toks) and toks[j + 1][1] == "(":
            mask |= ESP
        elif (tok[1] == "GR" and j + 3 < len(toks) and toks[j + 1][1] == "("
              and toks[j + 3][1] == ")"):
            reg = toks[j + 2][1]
            if reg not in BIT:
                raise Refuse("GR(%s)" % reg)
            after = toks[j + 4][1] if j + 4 < len(toks) else ""
            before = toks[j - 1][1] if j else ""
            if after in _ASSIGN_OPS or before in ("++", "--", "&"):
                mask |= BIT[reg]
    return mask


_CPU_OTHER_FIELDS = frozenset((
    "x", "st", "st_top", "fsw", "df", "idf", "fl", "f_a", "f_b", "f_r",
    "f_op", "f_sz", "f_cf", "f_of", "f_zf", "f_sf", "f_pf",
))


def _cpu_uses(toks):
    """(boundary, host) for one statement or header.

    boundary: the callee that receives the CPU and may read and write every
    register (translated function, guest_call, import, int3, cpuid).
    host: the statement may read or write CPU GPR fields behind the locals'
    back -- a GPR (or unknown) field named, the CPU handed to a helper outside
    the emitter vocabulary, or any call outside it (a gen_all seam).  Such a
    point makes every later FLUSH a full one until a RELOAD or a full FLUSH
    re-synchronises (see Analysis).  A bare `c` used as a value (an alias)
    refuses the body."""
    boundary, host = None, False
    for j, tok in enumerate(toks):
        if tok[0] != "id" or tok[1] != "c":
            continue
        if j + 2 < len(toks) and toks[j + 1][1] == "->":
            field = toks[j + 2][1]
            if field in _CPU_GPR_FIELD or field not in _CPU_OTHER_FIELDS:
                host = True
            continue
        nxt = toks[j + 1][1] if j + 1 < len(toks) else ""
        if nxt not in (",", ")"):
            raise Refuse("CPU pointer used as a value")
        depth, k = 0, j - 1
        while k >= 0:
            if toks[k][1] == ")":
                depth += 1
            elif toks[k][1] == "(":
                if depth == 0:
                    break
                depth -= 1
            k -= 1
        callee = toks[k - 1][1] if k >= 1 and toks[k - 1][0] == "id" else None
        if callee in _SYNC or callee in _CPU_NEUTRAL_CALLS:
            continue
        if callee in _BOUNDARY_CALLS or (callee and _SUB_RE.match(callee)):
            boundary = callee
            continue
        host = True
    for name, _ in _calls(toks):
        if name in _VALUE_CALLS or name in _CPU_NEUTRAL_CALLS or name in _SYNC:
            continue
        if name in _BOUNDARY_CALLS or name in _STACK_WRITERS or _SUB_RE.match(name):
            continue
        if name.startswith("GUEST_XMM_") or re.match(r"^(cc|fl)_[a-z]+\Z", name):
            continue
        host = True
    for name in ("GUEST_IMPORT_CALL", "GUEST_IMPORT_JMP"):
        if any(t[1] == name for t in toks):
            boundary = boundary or name
    return boundary, host


def _is_sync(stmt, name):
    return (stmt[0] == "expr" and len(stmt[1]) == 4 and stmt[1][0][1] == name
            and stmt[1][1][1] == "(" and stmt[1][2][1] == "c" and stmt[1][3][1] == ")")


class Analysis:
    def __init__(self):
        self.labels = {}
        self.changed = False
        self.flush_masks = {}      # statement start offset -> mask or None

    def label_in(self, name, state):
        old = self.labels.get(name)
        new = _union(old, state)
        if new != old:
            self.labels[name] = new
            self.changed = True

    def run_list(self, stmts, state, ctx):
        for k, stmt in enumerate(stmts):
            nxt = stmts[k + 1] if k + 1 < len(stmts) else None
            state = self.run(stmt, state, ctx, nxt)
        return state

    def run(self, stmt, state, ctx, nxt=None):
        kind = stmt[0]
        if kind == "expr":
            toks = stmt[1]
            if _is_sync(stmt, "GUEST_GPR_FLUSH"):
                # Visited once per path through enclosing loops; the mask is
                # the union of the dirty sets of every reachable visit (a
                # HOST bit in it keeps this FLUSH a full one).  Either way
                # every field equals its local afterwards.
                if state is not None:
                    self.flush_masks[stmt[2]] = self.flush_masks.get(stmt[2], 0) | state
                return None if state is None else 0
            if (_is_sync(stmt, "GUEST_GPR_RELOAD") or
                    (len(toks) == 1 and toks[0][1] == "GUEST_GPR_DECL")):
                return None if state is None else 0
            boundary, host = _cpu_uses(toks)
            if boundary is not None:
                if state is not None and state & FULL:
                    raise Refuse("%s reached with unpublished registers" % boundary)
                ok = nxt is not None and (nxt[0] == "return" or
                                          _is_sync(nxt, "GUEST_GPR_RELOAD"))
                if not ok:
                    raise Refuse("%s not followed by RELOAD/return" % boundary)
                if _writes(toks):
                    raise Refuse("boundary statement writes a local")
            return _add(state, _writes(toks) | (HOST if host else 0))
        if kind == "block":
            return self.run_list(stmt[1], state, ctx)
        if kind == "empty":
            return state
        if kind == "if":
            state = _add(state, _header(stmt[1]))
            a = self.run(stmt[2], state, ctx)
            b = self.run(stmt[3], state, ctx) if stmt[3] is not None else state
            return _union(a, b)
        if kind == "loop":
            _, init, cond, step, body, is_do = stmt
            wc, ws = _header(cond), _header(step)
            head = _add(state, _header(init))   # state before the first test/body
            for _ in range(32):
                inner = {"break": None, "continue": None}
                entry = head if is_do else _add(head, wc)
                end = self.run(body, entry, dict(ctx, loop=inner))
                end = _union(end, inner["continue"])
                back = _add(end, ws | (wc if is_do else 0))
                new_head = _union(head, back)
                if new_head == head:
                    falls = back if is_do else entry
                    return _union(falls, inner["break"])
                head = new_head
            raise Refuse("loop did not converge")
        if kind == "switch":
            entry = _add(state, _header(stmt[1]))
            inner = {"break": None, "entry": entry, "default": False}
            body = stmt[2]
            stmts = body[1] if body[0] == "block" else [body]
            end = self.run_list(stmts, None, dict(ctx, loop=inner, switch=inner))
            out = _union(end, inner["break"])
            if not inner["default"]:
                out = _union(out, entry)
            return out
        if kind == "case":
            sw = ctx.get("switch")
            if sw is None:
                raise Refuse("case outside switch")
            if stmt[1]:
                sw["default"] = True
            return _union(state, sw["entry"])
        if kind == "label":
            self.label_in(stmt[1], state)
            return self.labels.get(stmt[1])
        if kind == "goto":
            self.label_in(stmt[1], state)
            return None
        if kind == "return":
            return None
        if kind in ("break", "continue"):
            loop = ctx.get("loop")
            if loop is None:
                raise Refuse(kind + " outside a loop")
            if kind == "continue" and "entry" in loop:
                raise Refuse("continue inside switch")
            loop[kind] = _union(loop[kind], state)
            return None
        if kind == "pp":
            res = None
            for branch in stmt[1]:
                res = _union(res, self.run_list(branch, state, ctx))
            return res
        raise Refuse("statement kind " + kind)


def _add(state, bits):
    return None if state is None else state | bits


def _header(toks):
    """Dirty bits of an if/loop/switch head; a boundary call there is refused
    (it could not be followed by its RELOAD)."""
    boundary, host = _cpu_uses(toks)
    if boundary is not None:
        raise Refuse("boundary call %s in a control header" % boundary)
    return _writes(toks) | (HOST if host else 0)


def analyse_body(body: str):
    """{flush statement offset: mask} for one body, or raises Refuse."""
    toks = tokenize(body)
    tree = Parser(toks).statements(("eof",))
    if not tree or not _is_sync_decl(tree):
        raise Refuse("body does not open with GUEST_GPR_DECL")
    an = Analysis()
    for _ in range(64):
        an.changed = False
        an.flush_masks = {}
        an.run_list(tree, 0, {})
        if not an.changed:
            return an.flush_masks
    raise Refuse("labels did not converge")


def _is_sync_decl(tree):
    for stmt in tree[:4]:
        if stmt[0] == "expr" and len(stmt[1]) == 1 and stmt[1][0][1] == "GUEST_GPR_DECL":
            return True
    return False


CORPUS_FUNCTIONS = r"(?:sub|guest_original)_[0-9a-f]{8}"


def mask_unit(text: str, stats=None, functions=CORPUS_FUNCTIONS):
    """Rewrite every provable FLUSH of one generated unit.  stats (a dict of
    counters) receives bodies/masked/refused, flushes/masked and stores
    removed (8 - popcount per masked site).  `functions` selects the bodies
    by name (a regex); bodies without GUEST_GPR_DECL are left alone."""
    func_re = re.compile(r"^void (%s)\(CPU \*__restrict c\)\n\{\n" % functions, re.M)
    out, pos = [], 0
    for m in func_re.finditer(text):
        start = m.end()
        end = text.find("\n}\n", start)
        if end < 0:
            raise RuntimeError("unterminated body " + m.group(1))
        body = text[start:end + 1]
        out.append(text[pos:start])
        out.append(_mask_body(m.group(1), body, stats))
        pos = end + 1
    out.append(text[pos:])
    return "".join(out)


def _mask_body(name, body, stats):
    if stats is not None:
        stats["bodies"] = stats.get("bodies", 0) + 1
    n_flush = len(FLUSH_RE.findall(body))
    try:
        masks = analyse_body(body)
    except Refuse as why:
        if stats is not None:
            stats["refused"] = stats.get("refused", 0) + 1
            stats["flushes_kept"] = stats.get("flushes_kept", 0) + n_flush
            reasons = stats.setdefault("reasons", {})
            key = str(why).split(" at ")[0][:60]
            reasons[key] = reasons.get(key, 0) + 1
        return body
    pieces, pos = [], 0
    for m in FLUSH_RE.finditer(body):
        mask = masks.get(m.start())
        pieces.append(body[pos:m.start()])
        if mask is None or mask & HOST or mask == FULL:
            pieces.append(m.group(0))
            if stats is not None:
                stats["flushes_kept"] = stats.get("flushes_kept", 0) + 1
        else:
            pieces.append(MASK_TOKEN % mask)
            if stats is not None:
                stats["flushes_masked"] = stats.get("flushes_masked", 0) + 1
                stats["stores_removed"] = (stats.get("stores_removed", 0)
                                           + 8 - bin(mask).count("1"))
        pos = m.end()
    pieces.append(body[pos:])
    if stats is not None:
        stats["masked_bodies"] = stats.get("masked_bodies", 0) + 1
    return "".join(pieces)


def _file_job(args):
    path, dst = args
    name = os.path.basename(path)
    with open(path, "r", encoding="utf-8", newline="") as fh:
        text = fh.read()
    stats = {}
    if re.fullmatch(r"guest_\d{4}\.c", name):
        new = mask_unit(text, stats)
        assert legacy_text(new) == text, name
        text = new
    with open(os.path.join(dst, name), "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    return stats


def main(argv):
    """Standalone measurement: mask every unit of corpus SRC into DST."""
    import glob
    import multiprocessing
    src, dst = argv[1], argv[2]
    os.makedirs(dst, exist_ok=True)
    stats = {}
    jobs = [(p, dst) for p in sorted(glob.glob(os.path.join(src, "*")))]
    with multiprocessing.Pool() as pool:
        for part in pool.imap_unordered(_file_job, jobs):
            for k, v in part.items():
                if k == "reasons":
                    for r, n in v.items():
                        stats.setdefault("reasons", {})[r] = stats.get("reasons", {}).get(r, 0) + n
                else:
                    stats[k] = stats.get(k, 0) + v
    reasons = stats.pop("reasons", {})
    print(stats)
    for k, v in sorted(reasons.items(), key=lambda kv: -kv[1])[:25]:
        print("  refused %6d  %s" % (v, k))


if __name__ == "__main__":
    import sys
    main(sys.argv)
