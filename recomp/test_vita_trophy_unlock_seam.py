#!/usr/bin/env python3
"""Frozen-PE codegen oracle for the Vita trophy TryUnlock entry seam.

ISAAC_VITA_TROPHIES observes PersistentGameData::TryUnlock(id)
(sub_00484c80) at entry and always falls through into the translated body
(the native observer, runtime/host_vita_trophies.c, only reads guest memory).
The TryUnlock body is translated twice: in its owner and, inlined as a
tail-jump target, inside sub_00485ab0 (challenge completion: 45 switch cases
`mov [ebp+8], <id>; pop ebp; jmp 0x884c80`).  Both copies carry the block.

1. Frozen inputs: PE identity; the pinned body hash of the DEFAULT_BASE pin
   image and the unrelocated (image-base 0x400000) hash measured by the
   research step differ exactly in the relocated manager-pointer absolute
   read at 00484cb9 (RVA 0x7fd680, the pointer the observer compares `this`
   against); the 45 tail-jump ids decoded independently from the jump table
   equal the generator's constant.
2. Codegen for both roots at both image bases with the real switch
   discovery: one fence per root, placed after the entry coverage hook of
   00484c80 and before its first instruction comment; removing the fence
   gives the text rendered with the seam disabled; the packing delta cancels
   the measured seam bytes (unit membership cannot move).
3. Seam-pass output (A1 lesson): the generator's own GPR seam-pass input and
   output are captured; the input differs from the unguarded input by the
   five rendered lines only, the output is run through a tiny preprocessor
   with and without the knob: OFF equals the unguarded body token for token,
   every FLUSH/RELOAD the seam added sits inside the fence, and no direct
   guest-ESP writer appears in the block.
4. Fail-closed: a one-byte drift at every pinned instruction (both roots)
   raises; operand drift, an entry label on the owner, a missing label on the
   copy and a foreign root that reaches the body are refused; an unrelated
   root is untouched.
5. Generator wiring: result key, Lua-callback neutral default, packing
   compensation, render hook, the _main two-root membership check.
6. Corpus contract (--generated-dir): exactly one owner unit per root, one
   fence per root and none elsewhere, each fenced block is the rendered block
   bracketed inside the fence, placed after the 00484c80 coverage hook.
   A corpus with no fence at all is reported as unguarded, not as a drift.
7. Negative control (--base-generated-dir): the unguarded corpus has the
   same owners and no trophy text; with --generated-dir as well, every head
   unit minus its fences equals the base unit and only the owner unit
   changed.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import sys


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

VITA_BASE = 0x98000000
PE_IMAGE_BASE = 0x00400000
FENCE = "#if defined(__vita__) && defined(ISAAC_VITA_TROPHIES)"
KNOB = "ISAAC_VITA_TROPHIES"
OWNER_NAME = "sub_00484c80"
TAIL_NAME = "sub_00485ab0"
# sum(len(line) + 1) over the five rendered lines, measured 2026-09-27;
# an earlier 331 was the two-argument observer.
SEAM_PACKING_BYTES = 376
# Research value: sha256
# of 00484c80..00484d95 in the unrelocated file image.
UNRELOCATED_BODY_SHA256 = (
    "2132d9c77eb01ca9c2b791133ce674e961dc15f8bb3338bdfd5289591f442d96")
# Coverage ids of the two roots in the int2l corpus (guest_0139.c:10029 and
# :12135/:12129); any ids work, the test only needs the hooks rendered.
COVERAGE_IDS = {0x00484C80: 6787, 0x00485AB0: 6796}
# The jump table of sub_00485ab0 (`jmp dword ptr [eax*4 + 0x885d30]` at
# 0x885adb) as an RVA, and its entry count
# (`dec eax; cmp eax, 0x2c; ja` -> 0..0x2c).
TAIL_TABLE_RVA = 0x00485D30
TAIL_TABLE_ENTRIES = 0x2D
ENTRY_COMMENT = "    /* 00484c80  push ebp */"
# A root outside both translated TryUnlock bodies (the achievement reporter,
# sub_00484a10).
UNRELATED_ROOT = 0x00484A10


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def strip_guarded(text: str) -> str:
    """Drop every trophy fence (the frozen, unguarded shape)."""
    kept, skipping = [], False
    for line in text.split("\n"):
        if line.strip() == FENCE:
            require(not skipping, "nested trophy fence")
            skipping = True
            continue
        if skipping:
            require(not line.lstrip().startswith("#if"),
                    "nested preprocessor block inside the trophy fence")
            if line.strip() == "#endif":
                skipping = False
            continue
        kept.append(line)
    require(not skipping, "unterminated trophy fence")
    return "\n".join(kept)


def fenced_lines(text: str) -> list[str]:
    """The lines strictly inside the (single) trophy fence."""
    lines = text.split("\n")
    start = [i for i, line in enumerate(lines) if line.strip() == FENCE]
    require(len(start) == 1, "fences: %d" % len(start))
    end = start[0] + 1
    while lines[end].strip() != "#endif":
        end += 1
    return lines[start[0] + 1:end]


def expected_fenced(G) -> list[str]:
    block = G.render_vita_trophy_unlock_seam()
    return [block[1], block[2], "    GUEST_GPR_FLUSH(c);", block[3],
            "    GUEST_GPR_RELOAD(c);"]


# ---- tiny preprocessor -------------------------------------------------------

_DEFINED_RE = re.compile(r"defined\s*\(\s*([A-Za-z_]\w*)\s*\)")
_ALLOWED_EXPR_RE = re.compile(r"^[01()!&| \t]*$")
_COMMENT_RE = re.compile(r"/\*.*?\*/")
_TOKEN_RE = re.compile(r"[A-Za-z_]\w*|\d\w*|->|&&|\|\||[^\s]")


def _eval_condition(expression: str, defines: set[str]) -> bool:
    replaced = _DEFINED_RE.sub(
        lambda m: "1" if m.group(1) in defines else "0", expression)
    require(_ALLOWED_EXPR_RE.match(replaced) is not None,
            "tiny preprocessor cannot evaluate: %r" % expression)
    python = (replaced.replace("&&", " and ").replace("||", " or ")
              .replace("!", " not "))
    return bool(eval(python, {"__builtins__": {}}, {}))  # noqa: S307


def preprocess(text: str, defines: set[str]) -> list[str]:
    """Conditional compilation only (#if/#ifdef/#ifndef/#elif/#else/#endif
    over defined() expressions); returns the surviving non-directive lines."""
    out = []
    stack = []  # (parent_active, a_branch_was_taken)
    active = True
    for line in text.split("\n"):
        stripped = line.strip()
        if stripped.startswith("#"):
            words = stripped[1:].split(None, 1)
            word = words[0] if words else ""
            rest = words[1] if len(words) > 1 else ""
            if word in ("if", "ifdef", "ifndef"):
                if word == "ifdef":
                    value = rest.strip() in defines
                elif word == "ifndef":
                    value = rest.strip() not in defines
                else:
                    value = _eval_condition(rest, defines)
                stack.append((active, value))
                active = active and value
            elif word == "elif":
                require(stack, "#elif without #if")
                parent, taken = stack[-1]
                value = (not taken) and _eval_condition(rest, defines)
                stack[-1] = (parent, taken or value)
                active = parent and value
            elif word == "else":
                require(stack, "#else without #if")
                parent, taken = stack[-1]
                stack[-1] = (parent, True)
                active = parent and not taken
            elif word == "endif":
                require(stack, "#endif without #if")
                active = stack.pop()[0]
            else:
                require(False, "unexpected directive in a body: %r" % line)
            continue
        if active:
            out.append(line)
    require(not stack, "unterminated #if in a body")
    return out


def tokens(lines: list[str]) -> list[str]:
    result = []
    for line in lines:
        result.extend(_TOKEN_RE.findall(_COMMENT_RE.sub(" ", line)))
    return result


# ---- 1. frozen inputs ---------------------------------------------------------

def verify_frozen_inputs(G, Image, DEFAULT_BASE, pe_path: Path) -> None:
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs

    root = G.VITA_TROPHY_UNLOCK_ROOT_RVA
    end = G.VITA_TROPHY_UNLOCK_END_RVA
    pin = Image(str(pe_path), DEFAULT_BASE)
    native = Image(str(pe_path), PE_IMAGE_BASE)
    pin_body = bytes(pin.code_at(root, end - root))
    native_body = bytes(native.code_at(root, end - root))
    require(hashlib.sha256(pin_body).hexdigest() ==
            G.VITA_TROPHY_UNLOCK_BODY_SHA256, "pin-image body hash changed")
    require(hashlib.sha256(native_body).hexdigest() ==
            UNRELOCATED_BODY_SHA256, "unrelocated body hash changed")
    load = G.VITA_TROPHY_UNLOCK_MANAGER_LOAD_RVA
    differing = [root + i for i in range(len(pin_body))
                 if pin_body[i] != native_body[i]]
    # Only the high bytes of the absolute (0x00bfd680 vs 0x307fd680) differ.
    require(bool(differing) and min(differing) >= load + 2 and
            max(differing) < load + 6,
            "body images differ outside the manager-pointer absolute: %s" %
            ["%08x" % rva for rva in differing])
    for image, base in ((pin, DEFAULT_BASE), (native, PE_IMAGE_BASE)):
        code = bytes(image.code_at(load, 6))
        require(code[:2] == bytes.fromhex("8b35") and
                int.from_bytes(code[2:], "little") ==
                base + G.VITA_TROPHY_UNLOCK_MANAGER_PTR_RVA,
                "00484cb9 no longer loads the manager pointer (base %08x)" %
                base)
    require(G.VITA_TROPHY_UNLOCK_MANAGER_PTR_RVA == 0x007FD680,
            "manager pointer RVA changed")
    # Independent decode of the tail-jump ids: walk the jump table (relocated
    # absolutes in the pin image), not the address-sorted cases the proof
    # walks.
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    ids = []
    for index in range(TAIL_TABLE_ENTRIES):
        slot = bytes(pin.code_at(TAIL_TABLE_RVA + 4 * index, 4))
        case = int.from_bytes(slot, "little") - DEFAULT_BASE
        decoded = list(md.disasm(bytes(pin.code_at(case, 13)), case))
        require(len(decoded) == 3 and
                decoded[0].mnemonic == "mov" and
                decoded[0].op_str.startswith("dword ptr [ebp + 8], ") and
                decoded[1].mnemonic == "pop" and decoded[1].op_str == "ebp" and
                decoded[2].mnemonic == "jmp" and
                int(decoded[2].op_str, 16) == root,
                "tail case %d at %08x changed shape" % (index + 1, case))
        ids.append(int(decoded[0].op_str.split(", ")[1], 16))
    require(tuple(ids) == G.VITA_TROPHY_UNLOCK_TAIL_IDS,
            "tail-jump ids from the jump table disagree with gen_all")
    require(len(set(ids)) == TAIL_TABLE_ENTRIES and
            all(1 <= i <= 637 for i in ids), "tail-jump ids out of range")
    runtime = HERE / "runtime" / "host_vita_trophies.c"
    if runtime.exists():
        text = runtime.read_text(encoding="utf-8")
        match = re.search(r"#define TROPHY_MGR_PTR_RVA\s+0x([0-9a-fA-F]+)u",
                          text)
        require(match is not None and int(match.group(1), 16) ==
                G.VITA_TROPHY_UNLOCK_MANAGER_PTR_RVA,
                "host_vita_trophies.c TROPHY_MGR_PTR_RVA disagrees")


# ---- 2./3. codegen and seam-pass output ---------------------------------------

def _switch_info(G, pin):
    info = {}
    for root, expected in ((G.VITA_TROPHY_UNLOCK_ROOT_RVA, 0),
                           (G.VITA_TROPHY_UNLOCK_TAIL_ROOT_RVA,
                            TAIL_TABLE_ENTRIES)):
        entries, edges, rejected, tables = G.discover_jump_tables(pin, root)
        require(len(entries) == expected and not rejected,
                "switch discovery changed at %08x: %d entries" %
                (root, len(entries)))
        info[root] = {"entries": entries, "edges": edges,
                      "rejected": rejected, "tables": tables}
    return info


def _translate(G, img, pin, root, switch_info, disabled=False):
    """Translate one root; returns (result, (seam-pass input, output))."""
    original_for_body = G.vita_trophy_unlock_seam_for_body
    original_pass = G.bracket_seam_regions
    captured = []

    def capture(lines, name="?"):
        out = original_pass(lines, name)
        captured.append((list(lines), list(out)))
        return out

    G.bracket_seam_regions = capture
    if disabled:
        G.vita_trophy_unlock_seam_for_body = (
            lambda *args, **kwargs: False)
    try:
        result = G._translate_function(
            img, {"rva": root}, None, switch_info,
            function_coverage_ids=dict(COVERAGE_IDS), pin_img=pin)
    finally:
        G.vita_trophy_unlock_seam_for_body = original_for_body
        G.bracket_seam_regions = original_pass
    require(len(captured) == 1, "seam pass ran %d times" % len(captured))
    return result, captured[0]


def verify_seam_pass_output(G, bracket_seam_regions, name, text, plain,
                            seam_pass, plain_pass) -> None:
    lines = text.rstrip("\n").split("\n")
    plain_lines = plain.rstrip("\n").split("\n")
    require(lines[2] == "{" and lines[-1] == "}" and
            plain_lines[2] == "{" and plain_lines[-1] == "}",
            "rendered body header/footer changed")
    body, plain_body = lines[3:-1], plain_lines[3:-1]
    block = list(G.render_vita_trophy_unlock_seam())
    # (a) The analysed text is the generator's own seam-pass output; its
    #     input is the unguarded input plus the five rendered lines, and
    #     re-running the pass over that input reproduces the emitted body.
    pass_in, pass_out = seam_pass
    plain_in, plain_out = plain_pass
    require(pass_out == body and plain_out == plain_body,
            "%s: captured seam-pass output is not the emitted body" % name)
    fence_at = pass_in.index(FENCE)
    require(pass_in[fence_at:fence_at + len(block)] == block and
            pass_in[:fence_at] + pass_in[fence_at + len(block):] == plain_in,
            "%s: seam-pass input differs from the unguarded input by more "
            "than the rendered block" % name)
    require(bracket_seam_regions(list(pass_in), name) == pass_out,
            "%s: re-running the seam pass changes the body" % name)
    # (b) FLUSH/RELOAD added for the seam sit inside the fence.
    inside = fenced_lines(text)
    outside = strip_guarded(text)
    for statement in ("GUEST_GPR_FLUSH(c);", "GUEST_GPR_RELOAD(c);"):
        require(sum(line.strip() == statement for line in inside) == 1,
                "%s: %s inside the fence: not exactly one" % (name, statement))
        require(outside.count(statement) == plain.count(statement),
                "%s: %s count outside the fence changed" % (name, statement))
    require(inside == expected_fenced(G),
            "%s: fenced block content changed: %r" % (name, inside))
    # (c) no direct guest-ESP writer in the block (c->esp is only read).
    require(not G.GUEST_STACK_DIRECT_WRITER_RE.search("\n".join(inside)),
            "%s: direct guest ESP writer inside the trophy block" % name)
    # (d) tiny preprocessor, knob off vs on.
    for base_defines in (set(), {"__vita__"}):
        off = preprocess("\n".join(body), base_defines)
        ref = preprocess("\n".join(plain_body), base_defines)
        require(tokens(off) == tokens(ref),
                "%s: knob-off preprocessed body differs from the unguarded "
                "body (defines %s)" % (name, sorted(base_defines)))
    on = preprocess("\n".join(body), {"__vita__", KNOB})
    ref = preprocess("\n".join(plain_body), {"__vita__", KNOB})
    # With the knob on, exactly the fenced lines (comment, extern, FLUSH,
    # call, RELOAD) appear, right before the first instruction comment.
    added = len(inside)
    require(len(on) == len(ref) + added, "%s: knob-on body gained %d lines" %
            (name, len(on) - len(ref)))
    first = on.index(ENTRY_COMMENT)
    require(on[first - added:first] == inside and
            on[:first - added] + on[first:] == ref,
            "%s: knob-on body changed outside the observer block" % name)


def verify_codegen(G, Image, DEFAULT_BASE, legacy_text, bracket_seam_regions,
                   pe_path: Path):
    pin = Image(str(pe_path), DEFAULT_BASE)
    switch_info = _switch_info(G, pin)
    entry_cover = "    guest_coverage_function(%dU);" % COVERAGE_IDS[
        G.VITA_TROPHY_UNLOCK_ROOT_RVA]
    require(G.vita_trophy_unlock_seam_packing_bytes() == SEAM_PACKING_BYTES,
            "seam packing bytes: %d" %
            G.vita_trophy_unlock_seam_packing_bytes())
    count = 0
    for base in (DEFAULT_BASE, VITA_BASE):
        emit = Image(str(pe_path), base)
        for root, name in ((G.VITA_TROPHY_UNLOCK_ROOT_RVA, OWNER_NAME),
                           (G.VITA_TROPHY_UNLOCK_TAIL_ROOT_RVA, TAIL_NAME)):
            result, seam_pass = _translate(G, emit, pin, root, switch_info)
            plain, plain_pass = _translate(G, emit, pin, root, switch_info,
                                           disabled=True)
            where = "%s at base %08x" % (name, base)
            require(result["stub"] is None, "%s became a stub" % where)
            require(result["vita_trophy_unlock_seam"] is True,
                    "%s: seam not reported" % where)
            require(plain["vita_trophy_unlock_seam"] is False,
                    "%s: disabled translation still reports the seam" % where)
            text = result["text"]
            require(text.count(FENCE) == 1,
                    "%s: fences %d" % (where, text.count(FENCE)))
            # (a) only the fenced lines were added.
            require(strip_guarded(text) == plain["text"],
                    "%s: text outside the fence changed" % where)
            # (b) unit packing is exactly that of the unguarded body.
            require(G.unit_packing_size(text) + result["legacy_size_delta"] ==
                    G.unit_packing_size(plain["text"]) +
                    plain["legacy_size_delta"],
                    "%s: packing size moved" % where)
            require(result["legacy_size_delta"] ==
                    plain["legacy_size_delta"] - SEAM_PACKING_BYTES,
                    "%s: packing delta does not cancel the seam" % where)
            require(len(legacy_text(text)) -
                    len(legacy_text(plain["text"])) == SEAM_PACKING_BYTES,
                    "%s: legacy text grew by other than the seam" % where)
            # (c) placement: right after the 00484c80 entry coverage hook
            #     (owner: the function entry; copy: after L_00484c80:),
            #     right before the first instruction comment.
            lines = text.split("\n")
            fence = lines.index(FENCE)
            endif = fence + lines[fence:].index("#endif")
            require(lines[fence - 1] == entry_cover and
                    lines[endif + 1] == ENTRY_COMMENT,
                    "%s: seam placement changed" % where)
            if root == G.VITA_TROPHY_UNLOCK_TAIL_ROOT_RVA:
                require(lines[fence - 2] == "L_00484c80:" and
                        text.count("goto L_00484c80;") == TAIL_TABLE_ENTRIES,
                        "%s: the copy is not entered through its label" %
                        where)
            else:
                require("L_00484c80:" not in text,
                        "%s: owner has an entry label" % where)
            require(not re.search(r"\b(?:return|goto)\b",
                                  "\n".join(lines[fence:endif + 1])),
                    "%s: the observer block must fall through" % where)
            verify_seam_pass_output(G, bracket_seam_regions, name, text,
                                    plain["text"], seam_pass, plain_pass)
            count += 1
    return pin, switch_info, count


def _expect_raise(G, needle, *args):
    try:
        G.vita_trophy_unlock_seam_for_body(*args)
    except RuntimeError as exc:
        require(needle in str(exc), str(exc))
    else:
        raise AssertionError("proof accepted a drift (%s)" % needle)


def verify_fail_closed(G, B, pin, switch_info) -> int:
    root = G.VITA_TROPHY_UNLOCK_ROOT_RVA
    tail = G.VITA_TROPHY_UNLOCK_TAIL_ROOT_RVA
    pinned = {rva for rva, _m, _o, _b in G.VITA_TROPHY_UNLOCK_PINNED}
    common = pinned | {G.VITA_TROPHY_UNLOCK_MANAGER_LOAD_RVA + 2,
                       G.VITA_TROPHY_UNLOCK_LAST_RVA}
    # Tail-specific: its entry, the first/last case (store id byte, the
    # pop, the jmp displacement), the dispatch, and its last instruction.
    tail_only = {tail, 0x00485AE2 + 3, 0x00485AE9, 0x00485AEA + 1,
                 0x00485D1E + 3, 0x00485D25, 0x00485D26 + 1, 0x00485ADB,
                 G.VITA_TROPHY_UNLOCK_TAIL_LAST_RVA}
    total = 0
    pin.mem = bytearray(pin.mem)
    for rva_root, name, mutations in ((root, OWNER_NAME, common),
                                      (tail, TAIL_NAME, common | tail_only)):
        info = switch_info[rva_root]
        em, insns, order, body, unsupported, _i, _m = B.translate(
            pin, rva_root, name, None, extra_entries=info["entries"],
            indirect_edges=info["edges"])
        require(unsupported == [], "%s unsupported: %r" % (name, unsupported))
        require(G.vita_trophy_unlock_seam_for_body(
            pin, rva_root, em, insns, order, body) is True,
            "%s not selected" % name)
        for rva in sorted(mutations):
            original = pin.mem[rva]
            pin.mem[rva] ^= 0x01
            try:
                _expect_raise(G, "Vita trophy TryUnlock",
                              pin, rva_root, em, insns, order, body)
            finally:
                pin.mem[rva] = original
            total += 1
        # Operand drift with unchanged bytes (decode, not bytes, differs).
        first_pin = G.VITA_TROPHY_UNLOCK_PINNED[1][0]
        saved = insns[first_pin]

        class Fake:
            address = saved.address
            size = saved.size
            mnemonic = saved.mnemonic
            op_str = "edi, eax"
            bytes = saved.bytes
        insns[first_pin] = Fake()
        try:
            _expect_raise(G, "Vita trophy TryUnlock",
                          pin, rva_root, em, insns, order, body)
        finally:
            insns[first_pin] = saved
        # Entry label: forbidden on the owner, required on the copy.
        if rva_root == root:
            em.labels.add(root)
            try:
                _expect_raise(G, "Vita trophy TryUnlock",
                              pin, rva_root, em, insns, order, body)
            finally:
                em.labels.discard(root)
        else:
            em.labels.discard(root)
            try:
                _expect_raise(G, "tail-jump copy",
                              pin, rva_root, em, insns, order, body)
            finally:
                em.labels.add(root)
        # A foreign root that reaches the body is refused ...
        _expect_raise(G, "membership", pin, rva_root + 0x10, em, insns,
                      order, body)
        # ... even if it reaches only a mid-body instruction ...
        _expect_raise(G, "membership", pin, rva_root + 0x10, em, insns,
                      [rva_root + 0x10, G.VITA_TROPHY_UNLOCK_LAST_RVA], body)
        # ... and a root whose order does not reach it is untouched (the
        # Steam reporter sub_00484a10 sits outside both bodies).
        require(G.vita_trophy_unlock_seam_for_body(
            pin, UNRELATED_ROOT, em, insns, [UNRELATED_ROOT], body) is False,
            "unrelated root selected the seam")
        require(G.vita_trophy_unlock_seam_for_body(
            pin, rva_root, em, insns, order, body) is True,
            "%s lost after the mutations" % name)
    return total


# ---- 5. generator wiring ------------------------------------------------------

def verify_wiring(G) -> None:
    require(G.LUA_CALLBACK_NEUTRAL_TRANSLATION.get(
        "vita_trophy_unlock_seam") is False,
        "Lua-callback neutral default missing")
    require(G.VITA_TROPHY_UNLOCK_ROOTS == frozenset((0x00484C80, 0x00485AB0)),
            "seam roots changed")
    source = (HERE / "gen_all.py").read_text(encoding="utf-8")
    require(source.count("vita_trophy_unlock_seam_roots != "
                         "VITA_TROPHY_UNLOCK_ROOTS") == 1,
            "_main membership check missing")
    require(source.count("legacy_size_delta -= "
                         "vita_trophy_unlock_seam_packing_bytes()") == 1,
            "packing compensation missing")
    require(source.count("lines.extend(render_vita_trophy_unlock_seam())")
            == 1, "render hook missing")


# ---- 6. corpus contract --------------------------------------------------------

def verify_corpus(G, generated_dir: Path) -> list[str]:
    units = sorted(generated_dir.glob("guest_[0-9][0-9][0-9][0-9].c"))
    require(units, "no generated units in %s" % generated_dir)
    owners = {OWNER_NAME: [], TAIL_NAME: []}
    fenced = {}
    for unit in units:
        text = unit.read_text(encoding="utf-8")
        for name in owners:
            if "\nvoid %s(CPU *__restrict c)\n" % name in text:
                owners[name].append(unit)
        if KNOB in text:
            fenced[unit.name] = text.count(FENCE)
    for name, found in owners.items():
        require(len(found) == 1, "%s owners: %s" % (name, found))
    require(fenced, "no trophy fence anywhere in %s: this is an unguarded "
            "corpus (e.g. the fb4aa9d base); pass it as --base-generated-dir "
            "for the negative control" % generated_dir)
    expected = {}
    for name in owners:
        expected[owners[name][0].name] = expected.get(
            owners[name][0].name, 0) + 1
    require(fenced == expected,
            "trophy fences outside/inside the owners: %s" % fenced)
    for name in owners:
        text = owners[name][0].read_text(encoding="utf-8")
        head = "\nvoid %s(CPU *__restrict c)\n{\n" % name
        start = text.index(head)
        end = text.index("\n}\n", start)
        body = text[start:end + 3]
        require(fenced_lines(body) == expected_fenced(G),
                "%s: corpus fenced block differs from the rendered block" %
                name)
        lines = body.split("\n")
        fence = lines.index(FENCE)
        endif = fence + lines[fence:].index("#endif")
        require(lines[fence - 1].startswith("    guest_coverage_function(") and
                lines[endif + 1] == ENTRY_COMMENT,
                "%s: corpus seam placement changed" % name)
        if name == TAIL_NAME:
            require(lines[fence - 2] == "L_00484c80:",
                    "%s: corpus copy not entered through its label" % name)
    return ["corpus: %d units; %s in %s, %s in %s; one fence per root, none "
            "elsewhere" % (len(units), OWNER_NAME, owners[OWNER_NAME][0].name,
                           TAIL_NAME, owners[TAIL_NAME][0].name)]


def _owner_units(generated_dir: Path) -> tuple[list[Path], dict[str, Path]]:
    units = sorted(generated_dir.glob("guest_[0-9][0-9][0-9][0-9].c"))
    require(units, "no generated units in %s" % generated_dir)
    owners: dict[str, list[Path]] = {OWNER_NAME: [], TAIL_NAME: []}
    for unit in units:
        text = unit.read_text(encoding="utf-8")
        for name in owners:
            if "\nvoid %s(CPU *__restrict c)\n" % name in text:
                owners[name].append(unit)
    for name, found in owners.items():
        require(len(found) == 1, "%s owners in %s: %s" %
                (name, generated_dir, found))
    return units, {name: found[0] for name, found in owners.items()}


def verify_base_corpus(generated_dir: Path,
                       head_dir: Path | None) -> list[str]:
    """Negative control: the unguarded (fb4aa9d) corpus has the same two
    owners and no trophy text at all; with a head corpus, every head unit
    minus its trophy fences is the base unit byte for byte."""
    units, owners = _owner_units(generated_dir)
    for unit in units:
        require(KNOB not in unit.read_text(encoding="utf-8"),
                "base corpus %s mentions %s" % (unit.name, KNOB))
    lines = ["base corpus (negative control): %d units, %s in %s, %s in %s, "
             "0 trophy fences" % (len(units), OWNER_NAME,
                                  owners[OWNER_NAME].name, TAIL_NAME,
                                  owners[TAIL_NAME].name)]
    if head_dir is not None:
        head_units, head_owners = _owner_units(head_dir)
        require([u.name for u in head_units] == [u.name for u in units],
                "head and base corpora list different units")
        require({n: u.name for n, u in head_owners.items()} ==
                {n: u.name for n, u in owners.items()},
                "the owners moved between the base and head corpora")
        changed = []
        for base_unit, head_unit in zip(units, head_units):
            base_text = base_unit.read_text(encoding="utf-8")
            head_text = head_unit.read_text(encoding="utf-8")
            require(strip_guarded(head_text) == base_text,
                    "%s: head minus trophy fences != base" % head_unit.name)
            if head_text != base_text:
                changed.append(head_unit.name)
        require(changed == sorted({u.name for u in owners.values()}),
                "units changed by the seam: %s" % changed)
        lines.append("base vs head: head minus fences == base for all %d "
                     "units; changed: %s" % (len(units), ", ".join(changed)))
    return lines


def run(pe_path: Path, generated_dir: Path | None,
        base_generated_dir: Path | None = None) -> None:
    pe_path = pe_path.resolve()
    os.environ["REPENTOGXM_PE"] = str(pe_path)
    import build_one as B  # noqa: E402
    import gen_all as G  # noqa: E402
    from gpr_locals import bracket_seam_regions, legacy_text  # noqa: E402
    from image import DEFAULT_BASE, Image  # noqa: E402

    require(pe_path.stat().st_size == G.VITA_TROPHY_UNLOCK_PE_SIZE,
            "frozen PE size changed")
    require(_sha256(pe_path) == G.VITA_TROPHY_UNLOCK_PE_SHA256,
            "frozen PE hash changed")
    verify_frozen_inputs(G, Image, DEFAULT_BASE, pe_path)
    print("frozen inputs: PASS (pin/unrelocated body hashes differ only in "
          "the 00484cb9 manager-pointer absolute, RVA 0x7fd680; 45 tail-jump "
          "ids from the 0x485d30 jump table == gen_all)")
    pin, switch_info, count = verify_codegen(
        G, Image, DEFAULT_BASE, legacy_text, bracket_seam_regions, pe_path)
    print("codegen: PASS (%d renderings = 2 roots x 2 bases; one fence after "
          "the 00484c80 coverage hook; strip(fence) == unguarded text; "
          "packing delta cancels %d bytes)" % (count, SEAM_PACKING_BYTES))
    print("seam-pass output: PASS (captured pass input = unguarded input + 5 "
          "lines; FLUSH/RELOAD inside the fence; knob-off preprocessed body "
          "== unguarded body token for token; no ESP writer)")
    mutations = verify_fail_closed(G, B, pin, switch_info)
    print("fail-closed: PASS (%d one-byte mutations over both roots, operand "
          "drift, entry label, foreign roots refused)" % mutations)
    verify_wiring(G)
    print("generator wiring: PASS")
    if generated_dir is not None:
        for line in verify_corpus(G, generated_dir.resolve()):
            print(line)
    if base_generated_dir is not None:
        for line in verify_base_corpus(
                base_generated_dir.resolve(),
                generated_dir.resolve() if generated_dir is not None else None):
            print(line)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", type=Path, required=True)
    parser.add_argument("--generated-dir", type=Path,
                        help="a corpus generated with the seam (head)")
    parser.add_argument("--base-generated-dir", type=Path,
                        help="negative control: the unguarded corpus (base); "
                             "with --generated-dir also head minus fences == "
                             "base")
    args = parser.parse_args()
    try:
        run(args.pe, args.generated_dir, args.base_generated_dir)
    except AssertionError as exc:
        print("FAIL: %s" % exc)
        return 1
    print("Vita trophy TryUnlock seam codegen oracle: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
