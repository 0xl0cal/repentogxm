#!/usr/bin/env python3
"""Derive the computed-goto lvm.c for ISAAC_VITA_LUA_VM_COMPUTED_GOTO.

Pristine Lua 5.3.3 dispatches every VM instruction through one C switch:
lvm.c:765-767 define `vmdispatch(o) switch(o)`, `vmcase(l) case l:` and
`vmbreak break`, and luaV_execute (lvm.c:786) runs `for (;;) { vmfetch();
vmdispatch (GET_OPCODE(i)) { vmcase(...) ... } }`.  Every instruction
therefore leaves its case body through the loop back-edge to one shared
range check and one shared indirect branch.  Lua 5.4 (ljumptab.h) offers the
other spelling of the same three macros under GCC: each case body ends with
its own fetch and its own `goto *table[opcode]`.  This derivation applies
that spelling to 5.3.3; no case body changes.

What changes, with every pristine line keeping its number:

* lvm.c:765-767 -- the three dispatch macros:
    vmdispatch(o)  goto *isaac_lvm_disptab[o];
    vmcase(l)      L_##l:
    vmbreak        vmfetch(); vmdispatch(GET_OPCODE(i))
  so `vmbreak;` performs exactly what the pristine loop back-edge performs
  (vmfetch, then dispatch on the fetched opcode), just without the trip
  through the loop head;
* lvm.c:768 (blank) -- the macro isaac_lvm_jumptable: the table, one entry
  per slot of the 6-bit opcode field (lopcodes.h:44 SIZE_OP), written with
  designated initializers `[OP_X] = &&L_OP_X` in lopcodes.h enum order, the
  slots NUM_OPCODES..(1 << SIZE_OP) - 1 pointing at L_isaac_op_none, plus
  _Static_asserts: every opcode has the value of its enum position,
  NUM_OPCODES is the count parsed here, and the table has exactly
  1 << SIZE_OP entries;
* lvm.c:790 `StkId base;` -- expands isaac_lvm_jumptable (labels as values
  are local to the function, so the table must live inside luaV_execute);
* lvm.c:1317, the brace that closes the pristine switch body -- gains
  `L_isaac_op_none: vmbreak;` before it.  GET_OPCODE masks the opcode to
  six bits (0..63) and the pristine switch has no default, so an opcode
  without a case leaves the switch and the loop fetches the next
  instruction; the filler label does the same.  No case body falls off its
  end (each ends in vmbreak, return or goto), so nothing else reaches it;
* after the last line -- three constants naming the mode, read by the
  bridge's state-creation log (host_vita_lua.c).  A pristine lvm.c does not
  define them, so a bridge compiled with the knob against a pristine archive
  fails to link instead of logging a mode it does not have.

Checks before writing: both inputs are the pristine 5.3.3 files (sha256 of
lua53_vanilla.cmake); the opcode enum parsed from lopcodes.h has 47 names
starting OP_MOVE and ending OP_EXTRAARG; lvm.c has exactly one vmcase per
enum name and no other; every `vmbreak;` is a statement on its own line
whose previous line ends a statement (vmbreak now expands to two
statements, so a braceless `if (c) vmbreak;` would change meaning);
luaV_execute has no plain `break;` or `continue;` (with no switch left
they would act on the for(;;) loop); the line count is preserved.

The proof is recomp/test_vita_lua_vm_computed_goto.py (host: pristine Lua
5.3.3 interpreter built with the pristine and the derived lvm.c, outputs
compared byte for byte over the official 5.3.3 test suite and a corpus that
dispatches every opcode, with per-opcode fetch and body counters).
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import sys

# lua53_vanilla.cmake: "lvm.c|4bef1d6b..." and "lopcodes.h|6a3e8b5a...".
# test_vita_lua_vm_computed_goto.py checks these against the manifest.
PRISTINE_LVM_SHA256 = (
    "4bef1d6b86248cefe49fa08c520b8c206a0b676c327677d72d282ae0d3949464")
PRISTINE_LOPCODES_SHA256 = (
    "6a3e8b5a6f29c6ba9c912edb2133d01f60228e69d045b0a09e9c54440b125804")
PRISTINE_LINES = 1322
# lopcodes.h:44 `#define SIZE_OP 6`; checked against the input below.
SIZE_OP = 6
EXPECTED_OPCODES = 47

KNOB = "ISAAC_VITA_LUA_VM_COMPUTED_GOTO"
FILLER = "L_isaac_op_none"

_MACROS_OLD = (b"#define vmdispatch(o)\tswitch(o)\n"
               b"#define vmcase(l)\tcase l:\n"
               b"#define vmbreak\t\tbreak\n"
               b"\n"
               b"\n"
               b"/*\n"
               b"** copy of 'luaV_gettable', but protecting the call to "
               b"potential\n")
_BASE_OLD = (b"  TValue *k;\n"
             b"  StkId base;\n"
             b"  ci->callstatus |= CIST_FRESH;  /* fresh invocation of "
             b"'luaV_execute\" */\n")
_CLOSE_OLD = (b"      vmcase(OP_EXTRAARG) {\n"
              b"        lua_assert(0);\n"
              b"        vmbreak;\n"
              b"      }\n"
              b"    }\n"
              b"  }\n"
              b"}\n")
_TAIL_OLD = (b"}\n"
             b"\n"
             b"/* }================================================"
             b"================== */\n"
             b"\n")
_DEFINITION = b"""
/*
** ISAAC_VITA_LUA_VM_COMPUTED_GOTO: the dispatch mode of this lvm.c, for the
** bridge's state-creation log (host_vita_lua.c) and nothing else.  Pristine
** lvm.c does not define these, so a bridge compiled with the knob against a
** pristine archive fails to link rather than logging the wrong mode.
*/
const char isaac_lua_vm_dispatch[] = "computed-goto";
const unsigned int isaac_lua_vm_dispatch_slots = 1u << SIZE_OP;
const unsigned int isaac_lua_vm_dispatch_opcodes = NUM_OPCODES;
"""


class DeriveError(Exception):
    pass


def opcode_names(lopcodes: bytes) -> list[str]:
    """The OpCode enum of lopcodes.h, in declaration (= value) order."""
    text = lopcodes.decode("ascii")
    if not re.search(r"^#define SIZE_OP\t\t%d$" % SIZE_OP, text, re.M):
        raise DeriveError(f"lopcodes.h does not define SIZE_OP {SIZE_OP}")
    m = re.search(r"typedef enum \{(.*?)\} OpCode;", text, re.S)
    if not m:
        raise DeriveError("lopcodes.h: OpCode enum not found")
    body = re.sub(r"/\*.*?\*/", " ", m.group(1), flags=re.S)
    names = [n.strip() for n in body.split(",")]
    names = [n for n in names if n]
    for n in names:
        if not re.fullmatch(r"OP_[A-Z]+", n):
            raise DeriveError(f"lopcodes.h: unexpected enumerator {n!r}")
    if len(names) != EXPECTED_OPCODES or len(set(names)) != len(names):
        raise DeriveError(f"lopcodes.h: {len(names)} opcodes, "
                          f"expected {EXPECTED_OPCODES} distinct")
    if names[0] != "OP_MOVE" or names[-1] != "OP_EXTRAARG":
        raise DeriveError("lopcodes.h: enum does not run OP_MOVE..OP_EXTRAARG")
    if "#define NUM_OPCODES\t(cast(int, OP_EXTRAARG) + 1)" not in text:
        raise DeriveError("lopcodes.h: NUM_OPCODES is not OP_EXTRAARG + 1")
    return names


def check_cases(lvm: bytes, names: list[str]) -> None:
    text = lvm.decode("ascii")
    cases = re.findall(r"\bvmcase\((\w+)\)", text)
    # the definition line itself spells vmcase(l)
    cases = [c for c in cases if c != "l"]
    if sorted(cases) != sorted(names):
        missing = sorted(set(names) - set(cases))
        extra = sorted(set(cases) - set(names))
        dup = sorted({c for c in cases if cases.count(c) > 1})
        raise DeriveError(f"lvm.c cases differ from the enum: missing={missing}"
                          f" extra={extra} duplicated={dup}")
    lines = text.split("\n")
    prev = ""
    count = 0
    for line in lines:
        if "vmbreak" in line and not line.startswith("#define vmbreak"):
            if line.strip() != "vmbreak;":
                raise DeriveError(f"vmbreak not a statement of its own: {line!r}")
            p = re.sub(r"/\*.*?\*/", "", prev).rstrip()
            if not (p.endswith((";", "}", "{")) or p.strip() == ")"):
                raise DeriveError(f"vmbreak after a non-statement: {prev!r}")
            count += 1
        if line.strip():
            prev = line
    if count != 45:
        raise DeriveError(f"lvm.c has {count} vmbreak statements, expected 45")
    # vmdispatch no longer opens a switch, so inside luaV_execute a plain
    # `break;` or `continue;` would now leave or restart the for(;;) loop
    # instead of the switch.  Pristine 5.3.3 has neither there (its case
    # bodies leave by vmbreak, goto or return); refuse an input that does.
    # The scan reads source text only: a break/continue inside a macro that
    # luaV_execute expands (Protect, dojump, ...) would not be seen.  It is
    # exhaustive because the input is pinned to the pristine 5.3.3 sha256
    # (PRISTINE_LVM_SHA256, checked in derive() before this runs).  If that pin
    # is ever relaxed, also scan the #define bodies of the macros referenced
    # in luaV_execute, or the gcc -E output.
    start = text.find("\nvoid luaV_execute (lua_State *L) {\n")
    if start < 0:
        raise DeriveError("lvm.c: luaV_execute definition not found")
    body = re.sub(r"/\*.*?\*/", " ", text[start:], flags=re.S)
    if re.search(r"\b(break|continue)\s*;", body):
        raise DeriveError("luaV_execute has a plain break/continue statement")


def jumptable_line(names: list[str]) -> bytes:
    slots = 1 << SIZE_OP
    entries = [f"[{n}] = &&L_{n}" for n in names]
    entries.append(f"[NUM_OPCODES ... {slots - 1}] = &&{FILLER}")
    order = " && ".join(f"{n} == {k}" for k, n in enumerate(names))
    return ("#define isaac_lvm_jumptable "
            "static const void *const isaac_lvm_disptab[] = { "
            + ", ".join(entries) + " }; "
            f"_Static_assert({order}, \"{KNOB}: opcode values follow "
            "lopcodes.h enum order\"); "
            f"_Static_assert(NUM_OPCODES == {len(names)}, \"{KNOB}: opcode "
            "count\"); "
            f"_Static_assert(sizeof(isaac_lvm_disptab) == (1 << SIZE_OP) * "
            f"sizeof(isaac_lvm_disptab[0]), \"{KNOB}: one entry per 6-bit "
            "opcode value\");"
            f"  /* {KNOB} */\n").encode("ascii")


def derive(lvm: bytes, lopcodes: bytes) -> bytes:
    """Return the computed-goto lvm.c (bytes in, bytes out)."""
    actual = hashlib.sha256(lvm).hexdigest()
    if actual != PRISTINE_LVM_SHA256:
        raise DeriveError(f"input is not pristine Lua 5.3.3 lvm.c: {actual}")
    actual = hashlib.sha256(lopcodes).hexdigest()
    if actual != PRISTINE_LOPCODES_SHA256:
        raise DeriveError(
            f"opcodes input is not pristine Lua 5.3.3 lopcodes.h: {actual}")
    if lvm.count(b"\n") != PRISTINE_LINES:
        raise DeriveError("pristine lvm.c line count changed")
    names = opcode_names(lopcodes)
    check_cases(lvm, names)
    macros_new = (
        b"#define vmdispatch(o)\tgoto *isaac_lvm_disptab[o];  /* " +
        KNOB.encode() + b"; pristine: switch(o) */\n"
        b"#define vmcase(l)\tL_##l:  /* pristine: case l: */\n"
        b"#define vmbreak\t\tvmfetch(); vmdispatch(GET_OPCODE(i))  /* pristine:"
        b" break, i.e. back to the loop head: vmfetch(); vmdispatch */\n" +
        jumptable_line(names) +
        b"\n"
        b"/*\n"
        b"** copy of 'luaV_gettable', but protecting the call to potential\n")
    base_new = (b"  TValue *k;\n"
                b"  StkId base;  isaac_lvm_jumptable\n"
                b"  ci->callstatus |= CIST_FRESH;  /* fresh invocation of "
                b"'luaV_execute\" */\n")
    close_new = (b"      vmcase(OP_EXTRAARG) {\n"
                 b"        lua_assert(0);\n"
                 b"        vmbreak;\n"
                 b"      }\n"
                 b"      " + FILLER.encode() + b": vmbreak;  /* " +
                 KNOB.encode() + b": opcodes NUM_OPCODES..63 have no case in "
                 b"the pristine switch, which then falls out to the next "
                 b"fetch */    }\n"
                 b"  }\n"
                 b"}\n")
    text = lvm
    for old, new in ((_MACROS_OLD, macros_new), (_BASE_OLD, base_new),
                     (_CLOSE_OLD, close_new)):
        if text.count(old) != 1:
            raise DeriveError(f"anchor not found exactly once: {old[:60]!r}")
        text = text.replace(old, new)
    if text.count(b"\n") != lvm.count(b"\n"):
        raise DeriveError("derivation changed the pristine line count")
    if not text.endswith(_TAIL_OLD):
        raise DeriveError("pristine lvm.c does not end where expected")
    return text + _DEFINITION


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--input", required=True, type=Path,
                        help="pristine lvm.c")
    parser.add_argument("--opcodes", required=True, type=Path,
                        help="pristine lopcodes.h")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        derived = derive(args.input.read_bytes(), args.opcodes.read_bytes())
    except (OSError, DeriveError) as exc:
        print(f"lua53_vm_computed_goto: {exc}", file=sys.stderr)
        return 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Rewrite when the bytes differ, and also when the output is older than
    # the script or an input, so the build rule that lists those as
    # dependencies converges after one run.
    stale = not args.output.is_file()
    if not stale:
        newest = max(os.stat(__file__).st_mtime, args.input.stat().st_mtime,
                     args.opcodes.stat().st_mtime)
        stale = (args.output.read_bytes() != derived or
                 args.output.stat().st_mtime < newest)
    if stale:
        args.output.write_bytes(derived)
    print("lua53_vm_computed_goto: "
          f"sha256={hashlib.sha256(derived).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
