#!/usr/bin/env python3
"""Corpus-level contract for the direct IAT import dispatch.

Runs against a generated Vita corpus (`--generated-dir`) and checks, from the
emitted text, guest_table.c and the frozen runtime map
host_vita_import_id_map.inc alone -- independently of the emitter's own
analysis -- that every import site carries the exact constants the runtime
guard (guest.h GUEST_IMPORT_CALL/GUEST_IMPORT_JMP, guest.c guest_import_call)
compares against:

  * guest_table.c and the frozen map agree row for row (ID, slot RVA, name);
  * every `call dword ptr [0xVA]` whose VA is an IAT slot materialises
    `_target = ld32(VA)` and dispatches through
    `GUEST_GPR_FLUSH(c); GUEST_IMPORT_CALL(_target, 0xSLOT, ID); GUEST_GPR_RELOAD(c);`
    with SLOT == VA - image base and ID == the table index of that slot;
  * every `jmp dword ptr [0xVA]` through a slot is
    `GUEST_GPR_FLUSH(c); GUEST_IMPORT_JMP(ld32(VA), 0xSLOT, ID); return;`
    with no GUEST_FLAGS_FLUSH (a host import never reads the flags);
  * every `call reg` spelled through GUEST_IMPORT_CALL names a slot that a
    `mov reg, dword ptr [0xVA]` of the same body loaded earlier with no label
    between the two (the run-time token compare makes a wrong guess a plain
    fallback; this pins that the constants were derived, not invented);
  * no GUEST_IMPORT_ token appears anywhere else;
  * every ID is below the frozen row count.

Prints the census the orchestrator asked for: converted sites per kind, the
frozen family kind of each converted site (UNRESOLVED rows always fall back at
run time), and the `call reg` sites left on the slow path.
"""

from __future__ import annotations

import argparse
import collections
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
BODY_RE = re.compile(
    r"^void ((?:sub|guest_original)_[0-9a-f]{8})\(CPU \*__restrict c\)\n\{\n(.*?)^\}\n",
    re.M | re.S)
COMMENT_RE = re.compile(r"^\s*/\* ([0-9a-f]{8})  (\S+) ?(.*?) \*/\s*$")
IAT_MEM_RE = re.compile(r"^dword ptr \[0x([0-9a-f]{8})\]$")
MOV_IAT_RE = re.compile(r"^(e[abcd]x|e[sd]i|ebp), dword ptr \[0x([0-9a-f]{8})\]$")
REG_RE = re.compile(r"^(e[abcd]x|e[sd]i|ebp)$")
LABEL_RE = re.compile(r"^L_[0-9a-f]{8}:")
LD32_RE = re.compile(
    r"ld32\(\(uint32_t\)\((?:\(uint32_t\)\(int32_t\)\((-?\d+)\)|0x([0-9a-f]+)U)\)\)")
TARGET_RE = re.compile(r"^\s*\{ uint32_t _target = (ld32\(.*\));$")
IMPORT_CALL_RE = re.compile(
    r"^\s*GUEST_GPR_FLUSH\(c\); GUEST_IMPORT_CALL\(_target, 0x([0-9a-f]{8})U, (\d+)U\);"
    r" GUEST_GPR_RELOAD\(c\);$")
IMPORT_JMP_RE = re.compile(
    r"^\s*GUEST_GPR_FLUSH\(c\); GUEST_IMPORT_JMP\((ld32\(.*\)), 0x([0-9a-f]{8})U, (\d+)U\);"
    r" return;$")
TABLE_ROW_RE = re.compile(r'^\s*\{ 0x([0-9a-f]{8})U, "([^"]+)" \},\s*$', re.M)
# A store whose address is a compile-time constant (`st32((uint32_t)(0x...U),`).
# GUEST_IMPORT_JMP publishes no flag state (guest.h); the only way a jmp
# [IAT] fallback could ever reach translated code is a guest store into the
# IAT, so the corpus must not contain one at a constant IAT address.
CONST_STORE_RE = re.compile(r"\bst(8|16|32|64)\(\(uint32_t\)\(0x([0-9a-f]{8})U\)")
MAP_ROW_RE = re.compile(
    r'^ISAAC_VITA_IMPORT_ID_ROW\((\d+)U, 0x([0-9a-f]{8})U, "([^"]+)", '
    r'(ISAAC_VITA_IMPORT_[A-Z0-9_]+), (\d+)U\)$', re.M)


def fail(message: str) -> None:
    raise SystemExit(message)


def ld32_va(expression: str):
    match = LD32_RE.fullmatch(expression)
    if match is None:
        return None
    if match.group(1) is not None:
        return int(match.group(1)) & 0xFFFFFFFF
    return int(match.group(2), 16) & 0xFFFFFFFF


def load_tables(generated_dir: pathlib.Path, runtime_dir: pathlib.Path):
    table = (generated_dir / "guest_table.c").read_text(encoding="utf-8")
    rows = [(int(slot, 16), name) for slot, name in TABLE_ROW_RE.findall(table)]
    if not rows:
        fail("guest_table.c has no import rows")
    length = re.search(r"guest_import_table_len = (\d+)U;", table)
    if length is None or int(length.group(1)) != len(rows):
        fail("guest_table.c import table length does not match its rows")
    frozen = {}
    for import_id, slot, name, kind, local in MAP_ROW_RE.findall(
            (runtime_dir / "host_vita_import_id_map.inc").read_text(
                encoding="utf-8")):
        frozen[int(import_id)] = (int(slot, 16), name, kind, int(local))
    if len(frozen) != len(rows):
        fail("frozen import map has %d rows, guest_table.c %d"
             % (len(frozen), len(rows)))
    for import_id, (slot, name) in enumerate(rows):
        if frozen.get(import_id, (None, None))[:2] != (slot, name):
            fail("import ID %d differs between guest_table.c and the frozen "
                 "map: %r vs %r" % (import_id, (slot, name),
                                    frozen.get(import_id)))
    slots = {slot: import_id for import_id, (slot, _name) in enumerate(rows)}
    return rows, frozen, slots


def statement_region(lines, start):
    """Lines after the instruction comment at `start` up to the next
    instruction comment or label (the statements of that instruction plus any
    seam text gen_all wrapped around them)."""
    region = []
    for index in range(start + 1, len(lines)):
        line = lines[index]
        if COMMENT_RE.match(line) or LABEL_RE.match(line):
            break
        region.append(line)
    return region


def check_body(name, body, base, slots, frozen, stats, problems, dll_counter):
    lines = body.split("\n")
    total_import_tokens = body.count("GUEST_IMPORT_CALL(") + \
        body.count("GUEST_IMPORT_JMP(")
    seen_tokens = 0
    for index, line in enumerate(lines):
        comment = COMMENT_RE.match(line)
        if comment is None:
            continue
        mnemonic, operands = comment.group(2), comment.group(3)
        if mnemonic == "call" or mnemonic == "jmp":
            mem = IAT_MEM_RE.match(operands)
            if mem is not None:
                va = int(mem.group(1), 16)
                rva = (va - base) & 0xFFFFFFFF
                import_id = slots.get(rva)
                if import_id is None:
                    stats["abs_%s_not_import" % mnemonic] += 1
                    region = statement_region(lines, index)
                    if any("GUEST_IMPORT_" in text for text in region):
                        problems.append(
                            f"{name}:{comment.group(1)}: non-import absolute "
                            f"{mnemonic} spelled through an import token")
                    continue
                region = statement_region(lines, index)
                if mnemonic == "call":
                    ok = check_call_site(name, comment.group(1), region, va,
                                         rva, import_id, problems)
                    key = "call_slot"
                else:
                    ok = check_jmp_site(name, comment.group(1), region, va,
                                        rva, import_id, problems)
                    key = "jmp_slot"
                if ok:
                    seen_tokens += 1
                    stats[key] += 1
                    kind = frozen[import_id][2]
                    stats["kind_" + kind.replace("ISAAC_VITA_IMPORT_", "")] += 1
                    dll_counter[frozen[import_id][1]] += 1
                continue
        if mnemonic == "call" and REG_RE.match(operands):
            register = operands
            region = statement_region(lines, index)
            token_lines = [text for text in region if "GUEST_IMPORT_CALL(" in text]
            if not token_lines:
                stats["call_reg_slow"] += 1
                continue
            if len(token_lines) != 1:
                problems.append(f"{name}:{comment.group(1)}: call {register} "
                                f"carries {len(token_lines)} import tokens")
                continue
            match = IMPORT_CALL_RE.match(token_lines[0])
            if match is None:
                problems.append(f"{name}:{comment.group(1)}: call {register} "
                                f"token shape: {token_lines[0].strip()[:100]}")
                continue
            slot_rva, import_id = int(match.group(1), 16), int(match.group(2))
            if slots.get(slot_rva) != import_id:
                problems.append(f"{name}:{comment.group(1)}: call {register} "
                                f"names slot {slot_rva:08x} with ID {import_id}, "
                                f"table says {slots.get(slot_rva)}")
                continue
            # The register must have been loaded from exactly that slot
            # earlier in this body with no label (block leader) in between.
            witness = None
            for back in range(index - 1, -1, -1):
                if LABEL_RE.match(lines[back]):
                    break
                earlier = COMMENT_RE.match(lines[back])
                if earlier is None:
                    continue
                if earlier.group(2) == "mov":
                    load = MOV_IAT_RE.match(earlier.group(3))
                    if load is not None and load.group(1) == register:
                        witness = (int(load.group(2), 16) - base) & 0xFFFFFFFF
                        break
            if witness != slot_rva:
                problems.append(
                    f"{name}:{comment.group(1)}: call {register} guards slot "
                    f"{slot_rva:08x} but the nearest unlabelled load is "
                    f"{witness!r}")
                continue
            target_lines = [text for text in region if TARGET_RE.match(text)]
            if (len(target_lines) != 1 or
                    TARGET_RE.match(target_lines[0]).group(1) !=
                    "ld32(GR(%s))" % register and
                    not target_lines[0].strip().startswith(
                        "{ uint32_t _target = GR(%s);" % register)):
                if not any(text.strip() == "{ uint32_t _target = GR(%s);" % register
                           for text in region):
                    problems.append(
                        f"{name}:{comment.group(1)}: call {register} does not "
                        f"materialise GR({register}) as _target")
                    continue
            seen_tokens += 1
            stats["call_reg"] += 1
            kind = frozen[import_id][2]
            stats["kind_" + kind.replace("ISAAC_VITA_IMPORT_", "")] += 1
            dll_counter[frozen[import_id][1]] += 1
    if seen_tokens != total_import_tokens:
        problems.append(f"{name}: {total_import_tokens} import tokens but "
                        f"{seen_tokens} verified at import sites")


def check_call_site(name, site, region, va, rva, import_id, problems):
    # A seam may wrap the site in `#if defined(__vita__)` with its own
    # `_target` read of the same slot (the sync fast path); every `_target`
    # in the region must read this slot and exactly one token may dispatch.
    target_lines = [text for text in region if TARGET_RE.match(text)]
    token_lines = [text for text in region if "GUEST_IMPORT_CALL(" in text]
    if not target_lines or len(token_lines) != 1:
        problems.append(f"{name}:{site}: call [slot] region has "
                        f"{len(target_lines)} _target lines and "
                        f"{len(token_lines)} import tokens")
        return False
    for target_line in target_lines:
        if ld32_va(TARGET_RE.match(target_line).group(1)) != va:
            problems.append(f"{name}:{site}: _target does not read slot VA "
                            f"{va:08x}: {target_line.strip()[:100]}")
            return False
    match = IMPORT_CALL_RE.match(token_lines[0])
    if match is None:
        problems.append(f"{name}:{site}: call [slot] token shape: "
                        f"{token_lines[0].strip()[:110]}")
        return False
    if int(match.group(1), 16) != rva or int(match.group(2)) != import_id:
        problems.append(f"{name}:{site}: token constants ({match.group(1)}, "
                        f"{match.group(2)}) != slot {rva:08x} / ID {import_id}")
        return False
    if "GUEST_FLAGS_FLUSH" in token_lines[0]:
        problems.append(f"{name}:{site}: call [slot] publishes flags")
        return False
    return True


def check_jmp_site(name, site, region, va, rva, import_id, problems):
    token_lines = [text for text in region if "GUEST_IMPORT_JMP(" in text]
    if len(token_lines) != 1:
        problems.append(f"{name}:{site}: jmp [slot] region has "
                        f"{len(token_lines)} import tokens")
        return False
    match = IMPORT_JMP_RE.match(token_lines[0])
    if match is None:
        problems.append(f"{name}:{site}: jmp [slot] token shape: "
                        f"{token_lines[0].strip()[:110]}")
        return False
    if ld32_va(match.group(1)) != va:
        problems.append(f"{name}:{site}: jmp target does not read slot VA "
                        f"{va:08x}: {match.group(1)[:80]}")
        return False
    if int(match.group(2), 16) != rva or int(match.group(3)) != import_id:
        problems.append(f"{name}:{site}: jmp token constants ({match.group(2)}, "
                        f"{match.group(3)}) != slot {rva:08x} / ID {import_id}")
        return False
    if "GUEST_FLAGS_FLUSH" in token_lines[0]:
        problems.append(f"{name}:{site}: jmp [slot] publishes flags to a host "
                        f"import")
        return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--generated-dir", type=pathlib.Path, required=True)
    parser.add_argument("--runtime-dir", type=pathlib.Path,
                        default=HERE / "runtime")
    parser.add_argument("--image-base", type=lambda value: int(value, 0),
                        default=0x98000000)
    arguments = parser.parse_args()
    units = sorted(arguments.generated_dir.glob("guest_[0-9][0-9][0-9][0-9].c"))
    if not units:
        fail(f"no generated units under {arguments.generated_dir}")
    rows, frozen, slots = load_tables(arguments.generated_dir,
                                      arguments.runtime_dir)
    stats: dict = collections.Counter()
    dll_counter: dict = collections.Counter()
    problems: list[str] = []
    iat_lo = (min(slots) + arguments.image_base) & 0xFFFFFFFF
    iat_hi = (max(slots) + arguments.image_base + 4) & 0xFFFFFFFF
    for path in units:
        source = path.read_bytes().replace(b"\r\n", b"\n").decode("utf-8")
        for store in CONST_STORE_RE.finditer(source):
            width = int(store.group(1)) // 8
            address = int(store.group(2), 16)
            stats["const_store"] += 1
            if address < iat_hi and address + width > iat_lo:
                stats["const_store_iat"] += 1
                problems.append(f"{path.name}: constant-address st{store.group(1)} "
                                f"into the IAT range at {address:08x}")
        for match in BODY_RE.finditer(source):
            stats["functions"] += 1
            check_body(match.group(1), match.group(2), arguments.image_base,
                       slots, frozen, stats, problems, dll_counter)
        # A token outside any body (a seam or hand-written helper) is a
        # generator drift: the guard constants are only meaningful at a site.
        stripped = BODY_RE.sub("", source)
        if "GUEST_IMPORT_" in stripped:
            problems.append(f"{path.name}: GUEST_IMPORT_ token outside a body")
    for line in problems[:40]:
        print("FAIL", line, file=sys.stderr)
    converted = stats["call_slot"] + stats["jmp_slot"] + stats["call_reg"]
    print("iat-direct corpus contract: %d units, %d functions, %d import rows "
          "(guest_table.c == frozen map); converted sites: call [slot]=%d, "
          "jmp [slot]=%d, call reg=%d (total %d); slow path: call reg without "
          "a block-local slot load=%d, absolute call/jmp not through a "
          "slot=%d/%d"
          % (len(units), stats["functions"], len(rows), stats["call_slot"],
             stats["jmp_slot"], stats["call_reg"], converted,
             stats["call_reg_slow"], stats["abs_call_not_import"],
             stats["abs_jmp_not_import"]))
    kinds = sorted((key[5:], value) for key, value in stats.items()
                   if key.startswith("kind_"))
    print("iat-direct sites by frozen family kind: %s"
          % ", ".join("%s=%d" % item for item in kinds))
    print("iat-direct sites, top imports: %s"
          % ", ".join("%s=%d" % item for item in dll_counter.most_common(8)))
    print("iat-direct constant-address stores: %d, into the IAT range "
          "[%08x, %08x): %d"
          % (stats["const_store"], iat_lo, iat_hi, stats["const_store_iat"]))
    if problems:
        fail("iat-direct corpus contract: %d violations" % len(problems))
    print("iat-direct corpus contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
