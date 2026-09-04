#!/usr/bin/env python3
"""Corpus-level contract for the inlined tiny leaves (recomp/leaf_inline.py).

Runs against a generated corpus (`--generated-dir`) and checks, from the
emitted text alone and independently of the emitter's own analysis, that
every dual-spelled direct call site

    GPUSH(ret); GUEST_STACK_CALLSITE_BARRIER();
    #if GUEST_LEAF_INLINE
        <copy>
    #else
        GUEST_GPR_FLUSH(c); sub_X(c); GUEST_GPR_RELOAD(c);
    #endif

is exactly what the knob promises:

  * leaf copy: the region between the braces is, line for line, the
    out-of-line body of sub_X in the same corpus minus its declarations and
    coverage hook, with the translated `ret` (`(void)GPOP(); BARRIER;
    GUEST_GPR_FLUSH(c); return;`, or the `ret n` form) replaced by the pop
    (and the argument adjust) alone; the body is a single block (no label,
    exactly one return) and the copy carries no boundary, return, goto,
    preprocessor line or host call;
  * guard copy: the callee body opens with `cmp/test ; jcc` paired into one
    direct condition, one arm of the site's `if (cond) {..} else {..}` is the
    popped return and the other the ordinary call, on the side that matches
    the callee's ret block (branch target or fall-through);
  * the `#else` branch is the exact pre-inlining call and
    gpr_locals.legacy_text collapses every unit to a text without the knob;
  * every inlined callee keeps its out-of-line body.

The census (sites, callees, kinds, top callees) is printed and, with
`--census leaf_inline_census.json`, compared with gen_all's own record;
`--hot-set FILE` additionally reports the sites inside hot-set bodies.
The default pins are the wf/opt-leaf corpus at max 8 instructions.
"""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from gpr_locals import legacy_text                            # noqa: E402

BODY_RE = re.compile(
    r"^void ((?:sub|guest_original)_[0-9a-f]{8})\(CPU \*__restrict c\)\n\{\n(.*?)^\}\n",
    re.M | re.S)
SITE_RE = re.compile(
    r"^[ \t]*GPUSH\((0x[0-9a-fA-F]+U)\); GUEST_STACK_CALLSITE_BARRIER\(\);\n"
    r"[ \t]*#if GUEST_LEAF_INLINE\n"
    r"((?:(?![ \t]*#)[^\n]*\n)*?)"
    r"[ \t]*#else\n"
    r"[ \t]*GUEST_GPR_FLUSH\(c\); (sub_[0-9a-f]{8})\(c\); GUEST_GPR_RELOAD\(c\);\n"
    r"[ \t]*#endif\n", re.M)
LEAF_HEAD_RE = re.compile(
    r"^\{ /\* GUEST_LEAF_INLINE: (sub_[0-9a-f]{8}), (\d+) instructions \*/\Z")
GUARD_HEAD_RE = re.compile(r"^/\* GUEST_LEAF_INLINE guard: (sub_[0-9a-f]{8}) \*/\Z")
GUARD_IF_RE = re.compile(r"^if \((.*)\) \{ (.*?) \} else \{ (.*?) \}\Z")
JCC_RE = re.compile(r"^if \((.*)\) goto L_([0-9a-f]{8});\Z")
COMMENT_RE = re.compile(r"^/\* ([0-9a-f]{8})  (\S+) ?(.*) \*/\Z")
RET_PLAIN = ("(void)GPOP();", "GUEST_STACK_CALLSITE_BARRIER();",
             "GUEST_GPR_FLUSH(c); return;")
RET_ADJ_RE = re.compile(r"^if \(!GESP_ADJ\((\d+U)\)\) return;\Z")
FORBIDDEN = ("return", "goto", "GUEST_GPR_", "guest_", "#", "L_",
             "GESP_SET(", "c->e", "c->r[")
PROLOGUE = ("GUEST_FLAGS_DECL;", "GUEST_GPR_DECL;")

EXPECTED = {"sites": 13943, "callees": 351}    # wf/opt-leaf corpus, max 8


def fail(message):
    print("FAIL", message, file=sys.stderr)
    raise SystemExit(message)


def body_statements(body):
    """The callee body after its declarations and coverage hook, 4-space
    indentation removed; labels keep their column-0 form."""
    lines = body.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    if tuple(line.strip() for line in lines[:2]) != PROLOGUE:
        return None
    rest = lines[2:]
    if rest and rest[0].strip().startswith("guest_coverage_function("):
        rest = rest[1:]
    out = []
    for line in rest:
        if line.startswith("    "):
            out.append(line[4:])
        else:
            out.append(line)
    return out


def inline_ret(statements):
    statements = tuple(statements)
    if statements == RET_PLAIN:
        return ["(void)GPOP();", "GUEST_STACK_CALLSITE_BARRIER();"]
    if (len(statements) == 4 and statements[0] == RET_PLAIN[0] and
            statements[2:] == RET_PLAIN[1:]):
        match = RET_ADJ_RE.match(statements[1])
        if match:
            return ["(void)GPOP();", "(void)GESP_ADJ(%s);" % match.group(1),
                    "GUEST_STACK_CALLSITE_BARRIER();"]
    return None


def expected_leaf_copy(callee_lines):
    """The callee's out-of-line statements with the ret rewritten, or an
    error string."""
    if any(not line.startswith("/*") and not line.startswith(" ") and
           line.endswith(":") for line in callee_lines):
        return None, "callee body has a label"
    # Exactly one `return;` besides the `ret n` adjust's own early return.
    returns = sum(line.count("return;") for line in callee_lines
                  if not RET_ADJ_RE.match(line.strip()))
    if returns != 1:
        return None, "callee body does not return exactly once"
    # find the last instruction comment: the ret
    comment_indexes = [i for i, line in enumerate(callee_lines)
                       if COMMENT_RE.match(line)]
    if not comment_indexes:
        return None, "callee body has no instruction comments"
    last = comment_indexes[-1]
    match = COMMENT_RE.match(callee_lines[last])
    if match.group(2) != "ret":
        return None, "callee body does not end in ret"
    replacement = inline_ret(callee_lines[last + 1:])
    if replacement is None:
        return None, "callee ret rendering shape unknown"
    return callee_lines[:last + 1] + replacement, len(comment_indexes)


def check_leaf(site_name, inner, callee, callee_lines, problems):
    head = LEAF_HEAD_RE.match(inner[0])
    if head is None or head.group(1) != callee or inner[-1] != "}":
        problems.append("%s: leaf block header/footer malformed for %s"
                        % (site_name, callee))
        return
    copy = [line[2:] if line.startswith("  ") else line for line in inner[1:-1]]
    expected, count = expected_leaf_copy(callee_lines)
    if expected is None:
        problems.append("%s: %s: %s" % (site_name, callee, count))
        return
    # Line for line, indentation aside: the copy sits two levels deeper
    # (site indent + block indent) and the emitter's multi-line statements
    # carry their own continuation indentation in both renderings.
    if [line.strip() for line in copy] != [line.strip() for line in expected]:
        problems.append("%s: copy of %s differs from its body" % (site_name,
                                                                 callee))
        return
    if int(head.group(2)) != count:
        problems.append("%s: %s instruction count %s != %d"
                        % (site_name, callee, head.group(2), count))
    for line in copy:
        if COMMENT_RE.match(line):
            continue
        for needle in FORBIDDEN:
            if needle in line:
                problems.append("%s: copy of %s carries %r: %s"
                                % (site_name, callee, needle, line[:80]))
                return


def check_guard(site_name, inner, callee, callee_lines, problems):
    if len(inner) != 5 or GUARD_HEAD_RE.match(inner[0]) is None or \
            GUARD_HEAD_RE.match(inner[0]).group(1) != callee:
        problems.append("%s: guard block malformed for %s" % (site_name, callee))
        return
    comments = [COMMENT_RE.match(line) for line in inner[1:4]]
    site_if = GUARD_IF_RE.match(inner[4])
    if not all(comments) or site_if is None:
        problems.append("%s: guard block lines malformed for %s"
                        % (site_name, callee))
        return
    producer, jcc, ret = comments
    if producer.group(2) not in ("cmp", "test") or not (
            jcc.group(2).startswith("j") and jcc.group(2) != "jmp") or \
            ret.group(2) != "ret":
        problems.append("%s: guard shape is not cmp/test;jcc;ret for %s"
                        % (site_name, callee))
        return
    # The callee body: first two instruction comments, the direct condition.
    body_comments = [(i, COMMENT_RE.match(line))
                     for i, line in enumerate(callee_lines) if COMMENT_RE.match(line)]
    if len(body_comments) < 3 or body_comments[0][1].group(0) != producer.group(0) \
            or body_comments[1][1].group(0) != jcc.group(0):
        problems.append("%s: guard entry of %s differs from its body"
                        % (site_name, callee))
        return
    p_index, j_index = body_comments[0][0], body_comments[1][0]
    if j_index != p_index + 1:
        problems.append("%s: guard compare of %s emitted statements"
                        % (site_name, callee))
        return
    jcc_statement = JCC_RE.match(callee_lines[j_index + 1])
    if jcc_statement is None or jcc_statement.group(1) != site_if.group(1):
        problems.append("%s: guard condition of %s differs from the body's"
                        % (site_name, callee))
        return
    target = jcc_statement.group(2)
    call = "GUEST_GPR_FLUSH(c); %s(c); GUEST_GPR_RELOAD(c);" % callee
    # Which arm pops: the callee's ret block is either the fall-through
    # (next comment after the jcc) or the label the jcc names.
    after = callee_lines[j_index + 2]
    if COMMENT_RE.match(after) and COMMENT_RE.match(after).group(2) == "ret":
        ret_index = j_index + 2
        fast_is_then = False
    else:
        try:
            ret_index = callee_lines.index("L_%s:" % target) + 1
        except ValueError:
            problems.append("%s: guard ret block of %s not found"
                            % (site_name, callee))
            return
        fast_is_then = True
        if not (COMMENT_RE.match(callee_lines[ret_index]) and
                COMMENT_RE.match(callee_lines[ret_index]).group(2) == "ret"):
            problems.append("%s: guard target of %s is not a ret"
                            % (site_name, callee))
            return
    if callee_lines[ret_index] != ret.group(0):
        problems.append("%s: guard ret comment of %s differs"
                        % (site_name, callee))
        return
    ret_statements = []
    k = ret_index + 1
    while k < len(callee_lines) and not COMMENT_RE.match(callee_lines[k]) \
            and not callee_lines[k].endswith(":"):
        ret_statements.append(callee_lines[k])
        k += 1
    fast = inline_ret(ret_statements)
    if fast is None:
        problems.append("%s: guard ret rendering of %s unknown"
                        % (site_name, callee))
        return
    fast = " ".join(fast)
    then_arm, else_arm = site_if.group(2), site_if.group(3)
    expected = (fast, call) if fast_is_then else (call, fast)
    if (then_arm, else_arm) != expected:
        problems.append("%s: guard arms of %s are wrong: %r"
                        % (site_name, callee, (then_arm, else_arm)))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--generated-dir", type=pathlib.Path, required=True)
    parser.add_argument("--census", type=pathlib.Path,
                        help="leaf_inline_census.json written by gen_all")
    parser.add_argument("--hot-set", type=pathlib.Path,
                        help="translated_cpu_hot_set.txt")
    parser.add_argument("--expect-sites", type=int, default=EXPECTED["sites"])
    parser.add_argument("--expect-callees", type=int,
                        default=EXPECTED["callees"])
    arguments = parser.parse_args()
    units = sorted(arguments.generated_dir.glob("guest_[0-9][0-9][0-9][0-9].c"))
    if not units:
        fail("no generated units under %s" % arguments.generated_dir)

    bodies = {}
    unit_of = {}
    sites = []                      # (caller, callee, kind)
    problems = []
    for path in units:
        source = path.read_bytes().replace(b"\r\n", b"\n").decode("utf-8")
        if "GUEST_LEAF_INLINE" in legacy_text(source):
            problems.append("%s: legacy_text does not collapse every site"
                            % path.name)
        for match in BODY_RE.finditer(source):
            name, body = match.group(1), match.group(2)
            bodies[name] = body
            unit_of[name] = path.name
    for name, body in bodies.items():
        for match in SITE_RE.finditer(body):
            callee = match.group(3)
            inner = [line.strip() if line.strip().startswith(("{", "}", "/*", "if ("))
                     else line[4:] for line in match.group(2).split("\n")[:-1]]
            kind = "guard" if inner and inner[0].startswith(
                "/* GUEST_LEAF_INLINE guard") else "leaf"
            sites.append((name, callee, kind))
            callee_body = bodies.get(callee)
            if callee_body is None:
                problems.append("%s: inlined callee %s has no out-of-line body"
                                % (name, callee))
                continue
            callee_lines = body_statements(callee_body)
            if callee_lines is None:
                problems.append("%s: callee %s body prologue malformed"
                                % (name, callee))
                continue
            if kind == "leaf":
                check_leaf(name, inner, callee, callee_lines, problems)
            else:
                check_guard(name, inner, callee, callee_lines, problems)
        if "#if GUEST_LEAF_INLINE" in body and \
                body.count("#if GUEST_LEAF_INLINE") != len(SITE_RE.findall(body)):
            problems.append("%s: a GUEST_LEAF_INLINE block does not match the "
                            "site shape" % name)

    per_callee = collections.Counter(callee for _c, callee, _k in sites)
    per_kind = collections.Counter(kind for _c, _callee, kind in sites)
    print("leaf inline corpus contract: %d units, %d bodies, %d inlined sites "
          "(%d leaf, %d guard) at %d callees"
          % (len(units), len(bodies), len(sites), per_kind["leaf"],
             per_kind["guard"], len(per_callee)))
    for callee, count in per_callee.most_common(8):
        print("   %5d  %s (%s, %s)" % (count, callee, unit_of.get(callee, "?"),
                                      next(k for _c, cc, k in sites if cc == callee)))
    if arguments.hot_set is not None:
        hot = {line.strip() for line in
               arguments.hot_set.read_text(encoding="utf-8").splitlines()
               if line.strip() and not line.startswith("#")}
        hot_sites = sum(1 for caller, _callee, _k in sites if caller in hot)
        hot_callees = {callee for _c, callee, _k in sites if callee in hot}
        print("   hot set: %d sites inside %d hot bodies; %d inlined callees "
              "are themselves in the hot set"
              % (hot_sites, len({c for c, _cc, _k in sites if c in hot}),
                 len(hot_callees)))
    if arguments.census is not None:
        census = json.loads(arguments.census.read_text(encoding="utf-8"))
        recorded = collections.Counter("sub_" + s["callee"] for s in census["sites"])
        if recorded != per_callee:
            missing = {k: (recorded[k], per_callee[k]) for k in
                       set(recorded) | set(per_callee) if recorded[k] != per_callee[k]}
            problems.append("census disagrees with the text at %d callees: %s"
                            % (len(missing), sorted(missing.items())[:5]))
        eligible = {"sub_" + rva for rva in census["eligible"]}
        stray = set(per_callee) - eligible
        if stray:
            problems.append("inlined callees outside the census: %s"
                            % sorted(stray)[:5])
        print("   census: %d eligible callees, %d recorded sites, text delta %+d"
              % (len(eligible), len(census["sites"]), census["code_size_delta"]))
    if arguments.expect_sites is not None and len(sites) != arguments.expect_sites:
        problems.append("site count %d != pinned %d"
                        % (len(sites), arguments.expect_sites))
    if arguments.expect_callees is not None and \
            len(per_callee) != arguments.expect_callees:
        problems.append("callee count %d != pinned %d"
                        % (len(per_callee), arguments.expect_callees))
    for line in problems[:40]:
        print("FAIL", line, file=sys.stderr)
    if problems:
        fail("leaf inline corpus contract: %d violations" % len(problems))
    print("leaf inline corpus contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
