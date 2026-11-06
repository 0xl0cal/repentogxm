#!/usr/bin/env python3
"""Generate host_vita_lua_binding_table.h and host_vita_lua_binding_mirror.h for
ISAAC_VITA_LUA_BINDING_REPLAY (recomp/runtime/host_vita_lua_binding_replay.c).

The table is derived from two inputs and pins both:

* the frozen PE (isaac-ng.exe.unpacked.exe, size and sha256 pinned): every
  instance of the 92-byte LuaBridge CallMember scalar-getter template
  ("template A") in .text, plus the three singleton bodies (Entity:GetData
  0x446d90, the bool(int,int) free-function thunk 0x433870, the second
  indexMetaMethod 0x3f90b0).  The bytes are emitted as the image looks once
  it is mapped at GUEST_IMAGE_BASE: the loader applies the PE's own .reloc
  (guest.c guest_load_image), so the absolute IAT operands and the class key
  pushes hold Vita VAs.  The runtime seam compares these bytes against the
  mapped image on its first call and disables a row that differs.
* the generated corpus the Vita build compiles (ISAAC_GENERATED_DIR, the
  masked-flush corpus of the release recipe).  For every wrapped body
  and the three translated helpers it replays (3f8820, 3f8840, 280cc0) the
  generator pins the owner unit, the coverage id, zero in-unit references
  (GNU ld --wrap only redirects cross-unit references), the ordered list of
  import crossings (GUEST_IMPORT_CALL slot/id and plain guest_call sites),
  direct calls and pushed return markers, the absence of flag publication
  and leaf-inlined sites, and the body text sha256.  The 51 template-A
  bodies must be one text once the row constants are named; that text's
  sha256 is pinned too.

Both headers are game-derived (mapped PE bytes, translated corpus text), so
they are not tracked in git: the Vita build generates them at configure time
into its build tree (recomp/vita/CMakeLists.txt, ISAAC_VITA_LUA_BINDING_REPLAY:
`--write --pe ISAAC_PE_PATH --generated ISAAC_GENERATED_DIR --out-dir <dir>`),
and recomp/test_vita_lua_binding_replay.py generates them into its work
directory.  EXPECTED_SHA256 pins both outputs to the text the host oracle
verified, so a PE or corpus that yields any other text fails generation instead
of compiling an unverified replay.  To adopt a new corpus, set the pins to the
digests the failure prints and re-run the host oracle.

`--write` writes both headers into --out-dir (a file whose bytes are already
right is left untouched, so a reconfigure does not recompile its includers).
`--check` regenerates in memory and fails when the copies in --out-dir differ.
`--check-corpus` re-derives the corpus half of a table header and the whole
mirror from --generated alone (no PE) and fails on any difference.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import struct
import sys

TABLE_NAME = "host_vita_lua_binding_table.h"
MIRROR_NAME = "host_vita_lua_binding_mirror.h"
# sha256 of both generated headers for the frozen PE and the release corpus,
# the text recomp/test_vita_lua_binding_replay.py's host oracle verified.
EXPECTED_SHA256 = {
    TABLE_NAME: "548a4fa57855c6f2db851c16775062e46572f8b35953de8edf2edc0b30cac805",
    MIRROR_NAME: "4d10d5d4227454e99d38837dbb36d8f2d57e91a386276a1aafb4175a4e6680fd",
}

PE_SIZE = 8_650_240                     # recomp/test_vita_native_inflate.py:58
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
PE_BASE = 0x400000
GUEST_BASE = 0x98000000
DELTA = (GUEST_BASE - PE_BASE) & 0xffffffff

GETCLASS = 0x3f8e30
HELPER_SIGNED, HELPER_UNSIGNED, HELPER_REF = 0x3f8820, 0x3f8840, 0x280cc0
GETDATA, BOOLII, INDEX2 = 0x446d90, 0x433870, 0x3f90b0
SINGLETON_END = {GETDATA: 0x446e58, BOOLII: 0x4338bf, INDEX2: 0x3f9204}
STR_PROPGET, STR_PARENT = 0x74f2fc, 0x74f324
NO_COVERAGE = 0xffffffff                # unit generated without coverage hooks

# Template A, PE bytes (IAT operands as PE VAs), "??" = per-row hole.
TEMPLATE_A = (
    "55 8b ec 56 8b 75 08 57 6a 01 56 ff 15 0c 62 a0 00 83 c4 08 85 c0 75 04 33 ff eb 19 "
    "6a ?? 68 ?? ?? ?? ?? ba 01 00 00 00 8b ce e8 ?? ?? ?? ?? 83 c4 08 8b 78 04 "
    "68 d7 b9 f0 ff 56 ff 15 b0 61 a0 00 83 c4 08 8b cf 8b 10 ff d2 8b d0 8b ce e8 ?? ?? ?? ?? "
    "5f b8 01 00 00 00 5e 5d c3")
TEMPLATE_A_SIZE = 92
HOLE_CB, HOLE_KEY, HOLE_GETCLASS, HOLE_HELPER = 29, 31, 43, 79
TEMPLATE_A_RELOCS = (13, 31, 61)        # lua_type IAT operand, class key, touserdata IAT operand
# Return markers the template pushes, as offsets from the row RVA (the pushed
# return words are bare PE addresses, not GUEST_IMAGE_BASE-relative).
TEMPLATE_A_MARKERS = (0x11, 0x2f, 0x41, 0x4a, 0x53)

# The 17 Lua imports the replayed bodies cross: IAT slot RVA -> name.  The
# dense id is the slot's index in guest_table.c s_imports (checked).
IMPORTS = {
    0x60620c: "lua_type", 0x6061b0: "lua_touserdata", 0x606240: "lua_pushinteger",
    0x60624c: "lua_rawgeti", 0x60621c: "lua_getmetatable", 0x606258: "lua_pushvalue",
    0x606194: "lua_rawget", 0x606170: "lua_settop", 0x6061ac: "lua_pushstring",
    0x606254: "luaL_ref", 0x606248: "luaL_unref", 0x6061f0: "luaL_checkinteger",
    0x6061dc: "lua_pushboolean", 0x606220: "lua_absindex", 0x606214: "lua_rotate",
    0x606158: "lua_iscfunction", 0x6061c8: "lua_callk",
}
TYPED = {"lua_type", "lua_touserdata", "lua_pushinteger", "lua_rawgeti", "lua_getmetatable",
         "lua_pushvalue", "lua_rawget", "lua_settop", "lua_pushstring"}

# Plain `guest_call(c, _target)` sites of each body, keyed by the return word
# the site pushes (the template by its offset from the row RVA).  A member call
# names the register the translated code calls through; an import route also
# names the IAT slot whose value that register holds there (corpus instruction
# comments, guest_0120.c sub_003f90b0: `mov ebx, [0x98606170]` at 0x3f90dd
# feeds every `call ebx`; `mov edi, [0x98606194]` at 0x3f916a feeds `call edi`
# at 0x3f91a0; `mov edi, [0x98606214]` at 0x3f9124 feeds `call edi` at
# 0x3f91d8).  The runtime replays a
# route only while the register still equals that slot's tag word, so a wrong
# entry here costs speed, never exactness; the oracle counts real crossings.
MEMBER = "member"
PLAIN_SITES = {
    "template": {0x4a: (MEMBER, "edx")},
    GETDATA: {0x446de6: (MEMBER, "eax")},
    BOOLII: {0x4338a1: (MEMBER, "ecx")},
    INDEX2: {0x3f90f5: ("ebx", 0x606170), 0x3f9136: ("ebx", 0x606170), 0x3f9152: ("ebx", 0x606170),
             0x3f9194: ("ebx", 0x606170), 0x3f91a2: ("edi", 0x606194), 0x3f91c6: ("ebx", 0x606170),
             0x3f91da: ("edi", 0x606214), 0x3f91df: ("ebx", 0x606170)},
}
CALL_REG_MODRM = {"eax": 0xd0, "ecx": 0xd1, "edx": 0xd2, "ebx": 0xd3, "edi": 0xd7}


class GenError(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GenError(message)


# --- PE ------------------------------------------------------------------------

class Image:
    """The PE laid out at its RVAs, with its base relocations applied for
    GUEST_IMAGE_BASE exactly like the runtime loader."""

    def __init__(self, path: Path) -> None:
        raw = path.read_bytes()
        require(len(raw) == PE_SIZE, f"frozen PE size {len(raw)} != {PE_SIZE}")
        require(hashlib.sha256(raw).hexdigest() == PE_SHA256, "frozen PE sha256 changed")
        lfanew = struct.unpack_from("<I", raw, 0x3c)[0]
        sections = struct.unpack_from("<H", raw, lfanew + 6)[0]
        optional = struct.unpack_from("<H", raw, lfanew + 20)[0]
        require(struct.unpack_from("<I", raw, lfanew + 24 + 28)[0] == PE_BASE, "ImageBase != 0x400000")
        self.size_of_image = struct.unpack_from("<I", raw, lfanew + 24 + 56)[0]
        reloc_rva, reloc_size = struct.unpack_from("<II", raw, lfanew + 24 + 96 + 5 * 8)
        self.sections = []
        offset = lfanew + 24 + optional
        for _ in range(sections):
            name = raw[offset:offset + 8].rstrip(b"\0").decode("ascii")
            vsize, va, rsize, rptr = struct.unpack_from("<IIII", raw, offset + 8)
            self.sections.append((name, va, vsize, rptr, rsize))
            offset += 40
        self.pe = bytearray(self.size_of_image)
        for _, va, vsize, rptr, rsize in self.sections:
            count = min(vsize, rsize)
            self.pe[va:va + count] = raw[rptr:rptr + count]
        self.relocs: set[int] = set()
        position, end = reloc_rva, reloc_rva + reloc_size
        while position < end:
            page, block = struct.unpack_from("<II", self.pe, position)
            require(block >= 8, "malformed .reloc block")
            for i in range((block - 8) // 2):
                entry = struct.unpack_from("<H", self.pe, position + 8 + 2 * i)[0]
                kind, where = entry >> 12, entry & 0xfff
                if kind == 3:
                    self.relocs.add(page + where)
                else:
                    require(kind == 0, f"unexpected relocation type {kind}")
            position += block
        self.mapped = bytearray(self.pe)
        for where in self.relocs:
            value = struct.unpack_from("<I", self.mapped, where)[0]
            struct.pack_into("<I", self.mapped, where, (value + DELTA) & 0xffffffff)

    def section(self, name: str) -> tuple[int, int]:
        for section_name, va, vsize, _, _ in self.sections:
            if section_name == name:
                return va, vsize
        raise GenError(f"no {name} section")

    def relocs_in(self, start: int, size: int) -> list[int]:
        return sorted(r - start for r in self.relocs if start <= r < start + size)

    def dword(self, rva: int) -> int:
        return struct.unpack_from("<I", self.pe, rva)[0]

    def rel32(self, rva: int) -> int:
        return (rva + 4 + struct.unpack_from("<i", self.pe, rva)[0]) & 0xffffffff


def template_pattern() -> re.Pattern[bytes]:
    out = b""
    for token in TEMPLATE_A.split():
        out += b"." if token == "??" else re.escape(bytes.fromhex(token))
    return re.compile(out, re.S)


def scan_pe(image: Image) -> dict:
    require(len(TEMPLATE_A.split()) == TEMPLATE_A_SIZE, "template A is not 92 bytes")
    text_va, text_size = image.section(".text")
    rows = []
    for match in template_pattern().finditer(image.pe, text_va, text_va + text_size):
        rva = match.start()
        key_pe = image.dword(rva + HOLE_KEY)
        row = {
            "rva": rva,
            "cb": image.pe[rva + HOLE_CB],
            "key": (key_pe + DELTA) & 0xffffffff,
            "getclass": image.rel32(rva + HOLE_GETCLASS),
            "helper": image.rel32(rva + HOLE_HELPER),
        }
        require(row["getclass"] == GETCLASS, f"{rva:06x}: getClass target {row['getclass']:06x}")
        require(row["helper"] in (HELPER_SIGNED, HELPER_UNSIGNED), f"{rva:06x}: helper {row['helper']:06x}")
        require(row["cb"] in (0, 1), f"{rva:06x}: canBeConst byte {row['cb']}")
        require(image.relocs_in(rva, TEMPLATE_A_SIZE) == list(TEMPLATE_A_RELOCS),
                f"{rva:06x}: relocations {image.relocs_in(rva, TEMPLATE_A_SIZE)}")
        data_va, data_size = image.section(".data")
        require(data_va <= key_pe - PE_BASE < data_va + data_size, f"{rva:06x}: class key not in .data")
        rows.append(row)
    require(len(rows) == 51, f"template A instances: {len(rows)} (expected 51)")
    first = rows[0]["rva"]
    template = bytearray(image.mapped[first:first + TEMPLATE_A_SIZE])
    for hole, width in ((HOLE_CB, 1), (HOLE_KEY, 4), (HOLE_GETCLASS, 4), (HOLE_HELPER, 4)):
        template[hole:hole + width] = bytes(width)
    for row in rows:
        mapped = bytearray(image.mapped[row["rva"]:row["rva"] + TEMPLATE_A_SIZE])
        for hole, width in ((HOLE_CB, 1), (HOLE_KEY, 4), (HOLE_GETCLASS, 4), (HOLE_HELPER, 4)):
            mapped[hole:hole + width] = bytes(width)
        require(mapped == template, f"{row['rva']:06x}: relocated template differs")
    singletons = {}
    for rva, end in SINGLETON_END.items():
        singletons[rva] = {"bytes": bytes(image.mapped[rva:end]), "relocs": image.relocs_in(rva, end - rva)}
    # singleton anchors (PE disassembly; the corpus pins below re-derive the rest)
    require(image.pe[GETDATA:GETDATA + 9] == bytes.fromhex("558bec83e4f883ec14"), "446d90 prologue")
    require(image.pe[BOOLII:BOOLII + 7] == bytes.fromhex("558bec53568b75"), "433870 prologue")
    require(image.pe[INDEX2:INDEX2 + 8] == bytes.fromhex("558bec53568b7508"), "3f90b0 prologue")
    require(image.pe[SINGLETON_END[GETDATA] - 1] == 0xc3 and image.pe[SINGLETON_END[BOOLII] - 1] == 0xc3
            and image.pe[SINGLETON_END[INDEX2] - 1] == 0xc3, "singleton bodies must end with ret")
    for rva, text in ((STR_PROPGET, b"__propget\0"), (STR_PARENT, b"__parent\0")):
        require(bytes(image.pe[rva:rva + len(text)]) == text, f".rdata string at {rva:06x}")
    for slot in IMPORTS:
        require(slot in range(0x606150, 0x606150 + 67 * 4, 4), f"IAT slot {slot:06x} outside the Lua block")
    # the register loads that feed 3f90b0's plain `call reg` import routes
    for rva, reg, slot in ((0x3f90dd, "ebx", 0x606170), (0x3f916a, "edi", 0x606194),
                           (0x3f9124, "edi", 0x606214)):
        want = bytes([0x8b, {"ebx": 0x1d, "edi": 0x3d}[reg]]) + struct.pack("<I", PE_BASE + slot)
        require(bytes(image.pe[rva:rva + 6]) == want, f"{rva:06x}: mov {reg}, [{PE_BASE + slot:#x}] expected")
    # every plain call site is `call reg` (ff /2) right before its return word
    for body, sites in PLAIN_SITES.items():
        for ret, entry in sites.items():
            reg = entry[1] if entry[0] == MEMBER else entry[0]
            want = bytes([0xff, CALL_REG_MODRM[reg]])
            for base in ([r["rva"] for r in rows] if body == "template" else [0]):
                where = base + ret - 2
                require(bytes(image.pe[where:where + 2]) == want, f"{where:06x}: call {reg} expected")
    return {"rows": rows, "template": bytes(template), "singletons": singletons}


# --- corpus --------------------------------------------------------------------

DEF_RE = re.compile(r"^void (sub_[0-9a-f]{8})\(CPU \*__restrict c\)$", re.M)
CALL_RE = re.compile(
    r"GUEST_IMPORT_CALL\(_target, (0x[0-9a-f]{8})U, (\d+)U\)"
    r"|(?<![_a-z])guest_call\(c, _target\)"
    r"|(?<![_a-z0-9])(sub_[0-9a-f]{8})\(c\)"
    r"|GPUSH\((0x[0-9a-f]+)U\); GUEST_STACK_CALLSITE_BARRIER\(\); GUEST_GPR_FLUSH")


def extract_body(text: str, symbol: str) -> str:
    header = f"void {symbol}(CPU *__restrict c)\n{{"
    require(text.count(header) == 1, f"{symbol} must be defined exactly once in its unit")
    start = text.index(header)
    end = text.find("\n/* sub_", start)
    return text[start:end if end >= 0 else len(text)].rstrip() + "\n"


def call_sequence(body: str) -> list[str]:
    """Ordered import crossings / direct calls in a body: I:slot/id, G (plain
    guest_call of a register target), D:sub_X; markers pushed for each call."""
    out = []
    for match in CALL_RE.finditer(body):
        if match.group(1):
            out.append(f"I:{int(match.group(1), 16):06x}/{match.group(2)}")
        elif match.group(3):
            out.append(f"D:{match.group(3)[4:]}")
        elif match.group(4):
            continue
        else:
            out.append("G")
    return out


def call_markers(body: str) -> list[str]:
    """The return word pushed right before each call site (`GPUSH(ret)` in the
    call block, or the one-line direct-call spelling)."""
    markers = re.findall(r"GPUSH\((0x[0-9a-f]+)U\);\s*GUEST_STACK_CALLSITE_BARRIER\(\);\s*"
                         r"(?:GUEST_GPR_FLUSH|\n\s*GUEST_GPR_FLUSH)", body)
    return [f"{int(m, 16):06x}" for m in markers]


def normalize_template(body: str, row: dict) -> str:
    """The body with the row constants named: its own RVA (function name,
    instruction comments, labels, jump targets, pushed return markers -- every
    address inside [rva, rva+92)), the class key, the canBeConst push, the
    helper and the coverage line (absent in units generated without coverage
    hooks, e.g. guest_0190.c)."""
    rva = row["rva"]
    text = body.replace(f"sub_{rva:08x}", "sub_{R}")

    def inside(value: int) -> bool:
        return rva <= value < rva + TEMPLATE_A_SIZE

    def name(match: re.Match[str]) -> str:
        value = int(match.group(2), 16)
        return f"{match.group(1)}{{R+{value - rva:#x}}}" if inside(value) else match.group(0)

    # L_0043522c labels, "/* 0043522c  op */" comments, "jne 0x43522c", GPUSH(0x43522cU)
    text = re.sub(r"(L_|(?<![0-9a-zA-Z_]))(00[0-9a-f]{6})(?![0-9a-f])", name, text)
    text = re.sub(r"(0x)([0-9a-f]{5,8})(?![0-9a-f])", name, text)
    text = text.replace(f"0x{row['key']:08x}", "{KEY}")
    helper = row["helper"]
    text = text.replace(f"sub_{helper:08x}", "sub_{HELPER}").replace(f"call 0x{helper:x} ", "call {HELPER} ")
    text = re.sub(r"\n    guest_coverage_function\(\d+U\);", "", text, count=1)
    cb_line = re.compile(r"(/\* \{R\+0x1c\}  push )(\d)( \*/\n    GPUSH\(0x)(\d)(U\))")
    match = cb_line.search(text)
    require(match is not None and int(match.group(2)) == row["cb"] == int(match.group(4)),
            f"{rva:06x}: canBeConst push not found in the corpus body")
    return cb_line.sub(r"\1{CB}\3{CB}\5", text)


def scan_corpus(generated: Path, pe_scan: dict) -> dict:
    units = sorted(p for p in generated.glob("guest_*.c") if p.name != "guest_table.c")
    require(units, f"no generated units in {generated}")
    table = (generated / "guest_table.c").read_text(errors="replace")
    start = table.index("static const guest_import s_imports[] = {")
    imports = re.findall(r'^\s*\{ (0x[0-9A-Fa-f]+)U, "([^"]+)" \},', table[start:], re.M)
    import_index = {int(rva, 16): (i, name) for i, (rva, name) in enumerate(imports)}
    import_ids = {}
    for slot, name in IMPORTS.items():
        require(slot in import_index and import_index[slot][1] == f"Lua5.3.3r.dll!{name}",
                f"guest_table.c s_imports: slot {slot:06x} is {import_index.get(slot)}")
        import_ids[slot] = import_index[slot][0]
    owners: dict[str, str] = {}
    texts: dict[str, str] = {}
    wanted = {f"sub_{r['rva']:08x}" for r in pe_scan["rows"]}
    wanted |= {f"sub_{v:08x}" for v in (GETDATA, BOOLII, INDEX2, GETCLASS, HELPER_SIGNED, HELPER_UNSIGNED, HELPER_REF)}
    for unit in units:
        text = unit.read_text(errors="replace")
        for symbol in DEF_RE.findall(text):
            if symbol in wanted:
                require(symbol not in owners, f"{symbol} defined in {owners.get(symbol)} and {unit.name}")
                owners[symbol] = unit.name
                texts.setdefault(unit.name, text)
    for symbol in sorted(wanted):
        require(symbol in owners, f"{symbol} has no definition in the corpus")

    def pin(symbol: str, wrapped: bool) -> dict:
        unit = owners[symbol]
        text = texts[unit]
        body = extract_body(text, symbol)
        # the coverage call is the first statement after the two DECL lines,
        # or absent when the unit was generated without coverage hooks
        coverage = re.match(r"void sub_[0-9a-f]{8}\(CPU \*__restrict c\)\n\{\n    GUEST_FLAGS_DECL;\n"
                            r"    GUEST_GPR_DECL;\n(?:    guest_coverage_function\((\d+)U\);\n)?", body)
        require(coverage is not None, f"{symbol}: unexpected body preamble")
        require(body.count("guest_coverage_function(") == (1 if coverage.group(1) else 0),
                f"{symbol}: coverage call outside the preamble")
        for forbidden in ("GUEST_FLAGS_FLUSH", "GUEST_FLAGS_LOAD", "GUEST_LEAF_INLINE",
                          "GUEST_IMPORT_JMP", "guest_fault", "_setjmp", "guest_int3"):
            require(forbidden not in body, f"{symbol}: body contains {forbidden}")
        if wrapped:
            # --wrap redirects undefined references only: the owner unit may
            # name the symbol in its comment line and its definition, nothing else
            require(len(re.findall(rf"\b{symbol}\b", text)) == 2, f"{symbol}: in-unit reference in {unit}")
            require(table.count(f"{symbol}") >= 1, f"{symbol} is not in the guest_table.c dispatch table")
        for item in call_sequence(body):
            if item.startswith("I:"):
                slot, ident = item[2:].split("/")
                slot_value = int(slot, 16)
                require(slot_value in IMPORTS and import_ids[slot_value] == int(ident),
                        f"{symbol}: import crossing {item} is not a pinned Lua import")
        return {"symbol": symbol, "unit": unit,
                "coverage": int(coverage.group(1)) if coverage.group(1) else NO_COVERAGE,
                "calls": call_sequence(body), "markers": call_markers(body),
                "sha256": hashlib.sha256(body.encode("utf-8")).hexdigest(), "body": body}

    rows = []
    template_text = None
    getclass_unit = owners[f"sub_{GETCLASS:08x}"]
    for row in pe_scan["rows"]:
        info = pin(f"sub_{row['rva']:08x}", True)
        require(info["unit"] != getclass_unit, f"{info['symbol']} shares a unit with getClass: its call would bypass --wrap")
        want_calls = ["I:60620c/128", f"D:{GETCLASS:08x}", "I:6061b0/105", "G", f"D:{row['helper']:08x}"]
        require(info["calls"] == want_calls, f"{info['symbol']} calls {info['calls']}")
        require(info["markers"] == [f"{row['rva'] + m:06x}" for m in TEMPLATE_A_MARKERS],
                f"{info['symbol']} markers {info['markers']}")
        normalized = normalize_template(info["body"], row)
        if template_text is None:
            template_text = normalized
        require(normalized == template_text, f"{info['symbol']}: body differs from the template A text")
        rows.append({**row, **info})
    helpers = {v: pin(f"sub_{v:08x}", False) for v in (HELPER_SIGNED, HELPER_UNSIGNED, HELPER_REF)}
    require(helpers[HELPER_SIGNED]["calls"] == ["I:606240/141"] and helpers[HELPER_SIGNED]["markers"] == ["3f8832"],
            "3f8820 shape")
    require(helpers[HELPER_UNSIGNED]["calls"] == ["I:606240/141"] and helpers[HELPER_UNSIGNED]["markers"] == ["3f8850"],
            "3f8840 shape")
    require(helpers[HELPER_REF]["calls"] == ["I:60624c/144"] and helpers[HELPER_REF]["markers"] == ["280cda"],
            "280cc0 shape")
    singles = {v: pin(f"sub_{v:08x}", True) for v in (GETDATA, BOOLII, INDEX2)}
    require(singles[GETDATA]["unit"] != getclass_unit, "446d90 shares a unit with getClass")
    expected_calls = {
        GETDATA: ["I:60620c/128", f"D:{GETCLASS:08x}", "I:6061b0/105", "G", f"D:{HELPER_REF:08x}",
                  "I:606254/146", "I:606248/143", f"D:{HELPER_REF:08x}", "I:606248/143"],
        BOOLII: ["I:6061b0/105", "I:6061f0/121", "I:6061f0/121", "G", "I:6061dc/116"],
        INDEX2: ["I:60621c/132", "I:606258/147", "I:606194/98", "I:60620c/128",
                 "G", "I:606220/133", "I:6061ac/104", "I:606194/98", "I:606258/147", "I:606194/98",
                 "I:606214/130", "G", "I:606158/83",
                 "G", "I:606220/133", "I:6061ac/104", "I:606194/98", "I:60620c/128",
                 "I:606214/130", "G", "I:606258/147", "G", "I:60620c/128",
                 "I:606214/130", "G",
                 "G", "G", "I:606258/147", "I:6061c8/111"],
    }
    expected_markers = {
        GETDATA: ["446da8", "446dc6", "446dd8", "446de6", "446e00", "446e0d", "446e30", "446e3d", "446e49"],
        BOOLII: ["433885", "433892", "43389b", "4338a1", "4338b2"],
        INDEX2: ["3f90c2", "3f90cb", "3f90d4", "3f90dd", "3f90f5", "3f90fe", "3f910c", "3f9116", "3f911f",
                 "3f9124", "3f9131", "3f9136", "3f9142", "3f9152", "3f915b", "3f9169", "3f9173", "3f917c",
                 "3f918f", "3f9194", "3f919d", "3f91a2", "3f91ab", "3f91c1", "3f91c6", "3f91da", "3f91df",
                 "3f91e8", "3f91f7"],
    }
    for rva, info in singles.items():
        require(info["calls"] == expected_calls[rva], f"{info['symbol']} calls {info['calls']}")
        require(info["markers"] == expected_markers[rva], f"{info['symbol']} markers {info['markers']}")
    return {"rows": rows, "helpers": helpers, "singles": singles, "imports": import_ids,
            "template_sha256": hashlib.sha256(template_text.encode("utf-8")).hexdigest(),
            "template_text": template_text, "getclass_unit": getclass_unit}


# --- header ----------------------------------------------------------------------

def c_bytes(data: bytes, indent: str = "    ") -> str:
    lines = []
    for i in range(0, len(data), 12):
        lines.append(indent + ", ".join(f"0x{b:02x}" for b in data[i:i + 12]) + ",")
    return " \\\n".join(lines).rstrip(",")


def render(pe_scan: dict, corpus: dict) -> str:
    rows = corpus["rows"]
    out = []
    out.append("/* GENERATED by recomp/vita/lua_binding_replay_table.py -- do not edit.")
    out.append(" *")
    out.append(" * Rows and pins of ISAAC_VITA_LUA_BINDING_REPLAY")
    out.append(" * (host_vita_lua_binding_replay.c).  Inputs: the frozen PE")
    out.append(f" * (isaac-ng.exe.unpacked.exe, {PE_SIZE} bytes, sha256 {PE_SHA256[:16]}...) and")
    out.append(" * the generated corpus (owner units and body sha256s below).")
    out.append(" * recomp/test_vita_lua_binding_replay.py regenerates this file and fails")
    out.append(" * on any difference. */")
    out.append("#ifndef ISAAC_HOST_VITA_LUA_BINDING_TABLE_H")
    out.append("#define ISAAC_HOST_VITA_LUA_BINDING_TABLE_H")
    out.append("")
    out.append(f'#define ISAAC_LBR_PE_SHA256 "{PE_SHA256}"')
    out.append(f"#define ISAAC_LBR_SCALAR_COUNT {len(rows)}U")
    out.append(f"#define ISAAC_LBR_SCALAR_CONST_COUNT {sum(1 for r in rows if r['cb'])}U")
    out.append("#define ISAAC_LBR_ROW_COUNT (ISAAC_LBR_SCALAR_COUNT + 3U)")
    out.append("")
    out.append("/* Template A as mapped at GUEST_IMAGE_BASE (.reloc applied at")
    out.append(f" * +{', +'.join(str(r) for r in TEMPLATE_A_RELOCS)}), the per-row holes zero:")
    out.append(f" * +{HOLE_CB} canBeConst byte, +{HOLE_KEY} class key (Vita VA), +{HOLE_GETCLASS} getClass rel32,")
    out.append(f" * +{HOLE_HELPER} helper rel32.  Return markers at row RVA +"
               + ", +".join(f"{m:#x}" for m in TEMPLATE_A_MARKERS) + ". */")
    out.append(f"#define ISAAC_LBR_TEMPLATE_A_SIZE {TEMPLATE_A_SIZE}U")
    for name, value in (("CB", HOLE_CB), ("KEY", HOLE_KEY), ("GETCLASS", HOLE_GETCLASS), ("HELPER", HOLE_HELPER)):
        out.append(f"#define ISAAC_LBR_TEMPLATE_A_HOLE_{name} {value}U")
    out.append("#define ISAAC_LBR_TEMPLATE_A_BYTES { \\")
    out.append(c_bytes(pe_scan["template"]) + " }")
    out.append("")
    out.append("/* X(symbol, rva, canBeConst, class key VA, helper rva, coverage id): the 51")
    out.append(" * CallMember scalar getters, ascending RVA.  Helper 0x3f8820 pushes the")
    out.append(" * member's int sign-extended, 0x3f8840 zero-extended (lua_pushinteger).")
    out.append(f" * Coverage {NO_COVERAGE}U: the owner unit was generated without coverage hooks")
    out.append(" * (the translated body then has no guest_coverage_function call). */")
    out.append("#define ISAAC_LBR_SCALAR_ROWS(X) \\")
    for i, row in enumerate(rows):
        tail = " \\" if i + 1 < len(rows) else ""
        out.append(f"    X(sub_{row['rva']:08x}, 0x{row['rva']:08x}U, {row['cb']}U, 0x{row['key']:08x}U, "
                   f"0x{row['helper']:08x}U, {row['coverage']}U){tail}")
    out.append("")
    helpers = corpus["helpers"]
    out.append("/* Translated helpers the replay inlines (corpus bodies pinned below). */")
    out.append(f"#define ISAAC_LBR_HELPER_SIGNED_RVA 0x{HELPER_SIGNED:08x}U")
    out.append(f"#define ISAAC_LBR_HELPER_SIGNED_COVERAGE {helpers[HELPER_SIGNED]['coverage']}U")
    out.append(f"#define ISAAC_LBR_HELPER_SIGNED_RET 0x{int(helpers[HELPER_SIGNED]['markers'][0], 16):x}U")
    out.append(f"#define ISAAC_LBR_HELPER_UNSIGNED_RVA 0x{HELPER_UNSIGNED:08x}U")
    out.append(f"#define ISAAC_LBR_HELPER_UNSIGNED_COVERAGE {helpers[HELPER_UNSIGNED]['coverage']}U")
    out.append(f"#define ISAAC_LBR_HELPER_UNSIGNED_RET 0x{int(helpers[HELPER_UNSIGNED]['markers'][0], 16):x}U")
    out.append(f"#define ISAAC_LBR_HELPER_REF_RVA 0x{HELPER_REF:08x}U")
    out.append(f"#define ISAAC_LBR_HELPER_REF_COVERAGE {helpers[HELPER_REF]['coverage']}U")
    out.append(f"#define ISAAC_LBR_HELPER_REF_RET 0x{int(helpers[HELPER_REF]['markers'][0], 16):x}U")
    out.append("")
    singles = corpus["singles"]
    for tag, rva in (("GETDATA", GETDATA), ("BOOLII", BOOLII), ("INDEX2", INDEX2)):
        data = pe_scan["singletons"][rva]
        out.append(f"/* {singles[rva]['symbol']}: PE 0x{rva:06x}-0x{SINGLETON_END[rva] - 1:06x} as mapped "
                   f"(.reloc at +{', +'.join(str(r) for r in data['relocs'])}) */")
        out.append(f"#define ISAAC_LBR_{tag}_RVA 0x{rva:08x}U")
        out.append(f"#define ISAAC_LBR_{tag}_SIZE {len(data['bytes'])}U")
        out.append(f"#define ISAAC_LBR_{tag}_COVERAGE {singles[rva]['coverage']}U")
        out.append(f"#define ISAAC_LBR_{tag}_BYTES {{ \\")
        out.append(c_bytes(data["bytes"]) + " }")
        out.append("")
    out.append("/* .rdata strings 0x3f90b0 pushes (lua_pushstring takes the image pointer). */")
    out.append(f"#define ISAAC_LBR_STR_PROPGET_RVA 0x{STR_PROPGET:08x}U   /* \"__propget\" */")
    out.append(f"#define ISAAC_LBR_STR_PARENT_RVA 0x{STR_PARENT:08x}U    /* \"__parent\" */")
    out.append("")
    out.append("/* X(NAME, IAT slot RVA, dense import id, typed endpoint 1/0, import name):")
    out.append(" * the Lua imports the replayed bodies cross.  The loader tags each slot word")
    out.append(" * with its own VA (guest.c tag_imports); the seam pins that word and the")
    out.append(" * slot's import name (guest_import_name) at first use. */")
    out.append("#define ISAAC_LBR_IMPORTS(X) \\")
    items = sorted(IMPORTS.items())
    for i, (slot, name) in enumerate(items):
        tail = " \\" if i + 1 < len(items) else ""
        upper = name.replace("luaL_", "L_").replace("lua_", "").upper()
        out.append(f"    X({upper}, 0x{slot:08x}U, {corpus['imports'][slot]}U, {1 if name in TYPED else 0}, "
                   f"\"Lua5.3.3r.dll!{name}\"){tail}")
    out.append("")
    out.extend(render_corpus_pins(corpus))
    out.append("")
    out.append("#endif /* ISAAC_HOST_VITA_LUA_BINDING_TABLE_H */")
    return "\n".join(out) + "\n"


def render_corpus_pins(corpus: dict) -> list[str]:
    """The corpus-derived block of the table header (owner units, coverage ids,
    call sequences, body sha256s).  --check-corpus re-derives exactly this
    block and the mirror from a corpus directory, without the PE."""
    out = ["/* Corpus pins (" + corpus["getclass_unit"] + " owns getClass; no wrapped body shares it).",
           f" * template A normalized text sha256 {corpus['template_sha256']}"]
    for info in list(corpus["singles"].values()) + list(corpus["helpers"].values()):
        out.append(f" * {info['symbol']} {info['unit']} cov {info['coverage']} sha256 {info['sha256']}")
        out.append(f" *   calls {' '.join(info['calls'])}")
    for row in corpus["rows"]:
        out.append(f" * {row['symbol']} {row['unit']} sha256 {row['sha256']}")
    out.append(" */")
    return out


ROWS_RE = re.compile(r"^    X\((sub_[0-9a-f]{8}), 0x([0-9a-f]{8})U, (\d)U, 0x([0-9a-f]{8})U, "
                     r"0x([0-9a-f]{8})U, (\d+)U\)", re.M)
IMPORT_ROW_RE = re.compile(r"^    X\([A-Z_]+, 0x([0-9a-f]{8})U, (\d+)U, [01], \"Lua5\.3\.3r\.dll!(\w+)\"\)", re.M)


def check_corpus(generated: Path, header_path: Path, mirror_path: Path) -> str:
    """Configure-time gate (recomp/vita/CMakeLists.txt, ISAAC_VITA_LUA_BINDING_REPLAY,
    run on the headers the configure just generated): the corpus the build
    compiles must be the one the mirror was generated from, and the summary
    names the owner units the build checks.  The PE half of the header (template bytes, keys, helpers)
    is taken from the header itself; everything corpus-derived is re-derived
    from `generated` and compared: the 51 row coverage ids, the helper and
    singleton coverage ids, the 17 dense import ids, the pin block (owner
    units, call sequences, body sha256s) and the whole mirror text."""
    header = header_path.read_bytes().decode("utf-8")
    rows = [{"symbol": m.group(1), "rva": int(m.group(2), 16), "cb": int(m.group(3)),
             "key": int(m.group(4), 16), "helper": int(m.group(5), 16), "coverage": int(m.group(6))}
            for m in ROWS_RE.finditer(header)]
    require(len(rows) == 51, f"{header_path.name}: {len(rows)} scalar rows parsed (expected 51)")
    stub = {"rows": [{"rva": r["rva"], "cb": r["cb"], "key": r["key"], "getclass": GETCLASS,
                      "helper": r["helper"]} for r in rows]}
    corpus = scan_corpus(generated, stub)
    for want, got in zip(rows, corpus["rows"]):
        require(want["symbol"] == got["symbol"] and want["coverage"] == got["coverage"],
                f"{want['symbol']}: corpus coverage {got['coverage']} != header {want['coverage']}")
    defines = dict(re.findall(r"^#define (ISAAC_LBR_\w+_COVERAGE) (\d+)U$", header, re.M))
    for name, value in (("HELPER_SIGNED", corpus["helpers"][HELPER_SIGNED]["coverage"]),
                        ("HELPER_UNSIGNED", corpus["helpers"][HELPER_UNSIGNED]["coverage"]),
                        ("HELPER_REF", corpus["helpers"][HELPER_REF]["coverage"]),
                        ("GETDATA", corpus["singles"][GETDATA]["coverage"]),
                        ("BOOLII", corpus["singles"][BOOLII]["coverage"]),
                        ("INDEX2", corpus["singles"][INDEX2]["coverage"])):
        require(defines.get(f"ISAAC_LBR_{name}_COVERAGE") == str(value),
                f"ISAAC_LBR_{name}_COVERAGE: header {defines.get(f'ISAAC_LBR_{name}_COVERAGE')} != corpus {value}")
    ids = {int(slot, 16): (int(ident), name) for slot, ident, name in IMPORT_ROW_RE.findall(header)}
    require(len(ids) == len(IMPORTS), f"{header_path.name}: {len(ids)} import rows parsed")
    for slot, name in IMPORTS.items():
        require(ids.get(slot) == (corpus["imports"][slot], name),
                f"import {name}: header {ids.get(slot)} != corpus id {corpus['imports'][slot]}")
    pins = "\n".join(render_corpus_pins(corpus))
    require(pins in header, f"{header_path.name}: corpus pin block differs (owner unit, call sequence or "
                            f"body sha256 of a replayed body changed)")
    mirror = mirror_path.read_bytes().decode("utf-8")
    require(render_mirror(corpus) == mirror, f"{mirror_path.name} differs from the corpus text")
    return (f"rows=51+3 template={corpus['template_sha256'][:16]} "
            f"units={','.join(sorted({r['unit'] for r in corpus['rows']} | {corpus['getclass_unit']}))}")


# --- mirror -----------------------------------------------------------------------
#
# host_vita_lua_binding_mirror.h is the translated text of the six bodies (the
# 51 template-A rows share one parameterised copy) with exactly four kinds of
# statement rewritten; every other line is the corpus line, byte for byte:
#
#   GUEST_GPR_FLUSH_MASK(c, m); GUEST_IMPORT_CALL(_target, slot, id); RELOAD
#       -> LBR_IMPORT(NAME, CALL, _target, slot, id);
#   GUEST_GPR_FLUSH_MASK(c, m); guest_call(c, _target); RELOAD
#       -> LBR_IMPORT(NAME, PLAIN, _target, slot, id);   (PLAIN_SITES routes)
#       -> LBR_MEMBER_CALL(_target);                      (PLAIN_SITES members)
#   GUEST_GPR_FLUSH_MASK(c, m); sub_X(c); RELOAD
#       -> LBR_GETCLASS(); | LBR_HELPER(lbr_m_X); | LBR_HELPER_ROW(row);
#   GUEST_GPR_FLUSH_MASK(c, m); return;  -> GUEST_GPR_FLUSH(c); return;
#
# (full publication: an emulated import publishes nothing, so a later partial
# mask could miss a register the translated crossing had published), plus
# `if (C) goto L;` -> `if (LBR_COND(tag, k, C)) goto L;` (the identity outside
# the oracle, which counts both directions of every conditional branch).  The
# macros live in host_vita_lua_binding_replay.c.

IMPORT_BLOCK_RE = re.compile(
    r"\{ uint32_t _target = (?P<tgt>[^;\n]+);\n(?P<i1>[ ]+)GPUSH\(0x(?P<ret>\{R\+0x[0-9a-f]+\}|[0-9a-f]+)U\);\n"
    r"(?P<i2>[ ]+)GUEST_STACK_CALLSITE_BARRIER\(\);\n(?P<i3>[ ]+)"
    r"GUEST_GPR_FLUSH_MASK\(c, 0x[0-9a-f]+U\); (?P<call>GUEST_IMPORT_CALL\(_target, (?P<slot>0x[0-9a-f]{8})U, "
    r"(?P<id>\d+)U\)|guest_call\(c, _target\)); GUEST_GPR_RELOAD\(c\);")
DIRECT_RE = re.compile(
    r"GUEST_GPR_FLUSH_MASK\(c, 0x[0-9a-f]+U\); (?P<fn>sub_[0-9a-f]{8}|sub_\{HELPER\})\(c\); GUEST_GPR_RELOAD\(c\);")
RETURN_RE = re.compile(r"GUEST_GPR_FLUSH_MASK\(c, 0x[0-9a-f]+U\); return;")
COND_RE = re.compile(r"^([ ]+)if \((.+)\) goto (L_[0-9a-f]{8}|L_\{R\+0x[0-9a-f]+\});$", re.M)


def import_macro_name(slot: int) -> str:
    return IMPORTS[slot].replace("luaL_", "L_").replace("lua_", "").upper()


def strip_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def mirror_body(body: str, key, tag: str, import_ids: dict) -> tuple[str, dict]:
    """Rewrite one corpus body (see the block comment above).  `key` is the
    PLAIN_SITES key; the template text carries {R+..} placeholders."""
    sites = PLAIN_SITES.get(key, {})
    seen_sites: set[int] = set()
    counts = {"import": 0, "route": 0, "member": 0, "getclass": 0, "helper": 0, "return": 0, "branch": 0}

    def ret_value(text: str) -> int:
        match = re.fullmatch(r"\{R\+0x([0-9a-f]+)\}", text)
        return int(match.group(1), 16) if match else int(text, 16)

    def import_block(match: re.Match[str]) -> str:
        prefix = match.group(0)[:match.start("i3") - match.start(0) + len(match.group("i3"))]
        if match.group("slot"):
            slot = int(match.group("slot"), 16)
            require(slot in IMPORTS, f"{tag}: import slot {slot:06x} is not pinned")
            require(import_ids[slot] == int(match.group("id")), f"{tag}: import id of {slot:06x}")
            counts["import"] += 1
            return prefix + (f"LBR_IMPORT({import_macro_name(slot)}, CALL, _target, "
                             f"{match.group('slot')}U, {match.group('id')}U);")
        ret = ret_value(match.group("ret"))
        require(ret in sites, f"{tag}: plain guest_call before return word {match.group('ret')} is not in PLAIN_SITES")
        seen_sites.add(ret)
        entry = sites[ret]
        if entry[0] == MEMBER:
            require(match.group("tgt") == f"GR({entry[1]})", f"{tag}: member site {ret:#x} calls {match.group('tgt')}")
            counts["member"] += 1
            return prefix + "LBR_MEMBER_CALL(_target);"
        reg, slot = entry
        require(match.group("tgt") == f"GR({reg})", f"{tag}: route {ret:#x} calls {match.group('tgt')}, not {reg}")
        load = f"GR({reg}) = (uint32_t)(ld32((uint32_t)((uint32_t)(int32_t)({GUEST_BASE + slot - (1 << 32)}))));"
        require(load in body, f"{tag}: no `{load}` feeds route {ret:#x}")
        counts["route"] += 1
        return prefix + f"LBR_IMPORT({import_macro_name(slot)}, PLAIN, _target, 0x{slot:08x}U, {import_ids[slot]}U);"

    def direct(match: re.Match[str]) -> str:
        fn = match.group("fn")
        if fn == f"sub_{GETCLASS:08x}":
            counts["getclass"] += 1
            return "LBR_GETCLASS();"
        counts["helper"] += 1
        if fn == "sub_{HELPER}":
            return "LBR_HELPER_ROW(row);"
        require(int(fn[4:], 16) in (HELPER_SIGNED, HELPER_UNSIGNED, HELPER_REF), f"{tag}: direct call to {fn}")
        return f"LBR_HELPER(lbr_m_{fn[4:]});"

    def returns(_: re.Match[str]) -> str:
        counts["return"] += 1
        return "GUEST_GPR_FLUSH(c); return;"

    text = IMPORT_BLOCK_RE.sub(import_block, body)
    text = DIRECT_RE.sub(direct, text)
    text = RETURN_RE.sub(returns, text)
    require(seen_sites == set(sites), f"{tag}: PLAIN_SITES entries {sorted(set(sites) - seen_sites)} not in the body")

    def branch(match: re.Match[str]) -> str:
        counts["branch"] += 1
        return (f"{match.group(1)}if (LBR_COND({tag}, {counts['branch'] - 1}, {match.group(2)})) "
                f"goto {match.group(3)};")

    text = COND_RE.sub(branch, text)
    if key == "template":
        text = re.sub(r"0x\{R\+0x([0-9a-f]+)\}U", r"(row->rva + 0x\1U)", text)
        text = re.sub(r"L_\{R\+0x([0-9a-f]+)\}", r"L_R_\1", text)
        text = text.replace("GPUSH({KEY}U)", "GPUSH(row->key)").replace("GPUSH(0x{CB}U)", "GPUSH(row->cb)")
        header = ("static void lbr_m_template_a(CPU *__restrict c, lbr_ctx *__restrict x,\n"
                  "                             const lbr_row *__restrict row)\n{")
        text = text.replace("void sub_{R}(CPU *__restrict c)\n{", header, 1)
        text = text.replace("    GUEST_GPR_DECL;\n", "    GUEST_GPR_DECL;\n    LBR_ROW_COVERAGE(row);\n", 1)
    else:
        symbol = re.match(r"void (sub_[0-9a-f]{8})\(CPU \*__restrict c\)\n\{", text)
        require(symbol is not None, f"{tag}: body header")
        text = text.replace(symbol.group(0), f"static void lbr_m_{symbol.group(1)[4:]}(CPU *__restrict c, "
                            f"lbr_ctx *__restrict x)\n{{", 1)
    code = strip_comments(text)
    for leftover in ("GUEST_GPR_FLUSH_MASK", "GUEST_IMPORT_CALL", "guest_call(", "{R", "{KEY}", "{CB}",
                     "{HELPER}", "GUEST_GPR_RELOAD(c); }"):
        require(leftover not in code, f"{tag}: `{leftover}` left after the rewrite")
    require(re.search(r"\bsub_[0-9a-f]{8}\(", code) is None, f"{tag}: direct call left after the rewrite")
    return text, counts


def render_mirror(corpus: dict) -> str:
    out = []
    out.append("/* GENERATED by recomp/vita/lua_binding_replay_table.py -- do not edit.")
    out.append(" *")
    out.append(" * Mirrors of the translated bodies ISAAC_VITA_LUA_BINDING_REPLAY serves")
    out.append(" * (host_vita_lua_binding_replay.c includes this file once).  Each function is")
    out.append(" * the corpus body with four statement kinds rewritten (generator block comment):")
    out.append(" * import crossings -> LBR_IMPORT, member calls -> LBR_MEMBER_CALL, direct")
    out.append(" * calls -> LBR_GETCLASS / LBR_HELPER, masked flushes -> GUEST_GPR_FLUSH; and")
    out.append(" * each conditional goto's condition wrapped in LBR_COND (the identity outside")
    out.append(" * the oracle's branch census).  Every other line is the corpus text.")
    out.append(" * Source bodies (sha256 of the corpus text, pinned in")
    out.append(" * host_vita_lua_binding_table.h):")
    for info in list(corpus["helpers"].values()) + list(corpus["singles"].values()):
        out.append(f" *   {info['symbol']} {info['unit']} {info['sha256'][:16]}")
    out.append(f" *   template A (51 rows) normalized {corpus['template_sha256'][:16]}")
    out.append(" */")
    out.append("#ifndef ISAAC_HOST_VITA_LUA_BINDING_MIRROR_H")
    out.append("#define ISAAC_HOST_VITA_LUA_BINDING_MIRROR_H")
    out.append("")
    census = []
    bodies = [(HELPER_SIGNED, corpus["helpers"][HELPER_SIGNED], "H8820"),
              (HELPER_UNSIGNED, corpus["helpers"][HELPER_UNSIGNED], "H8840"),
              (HELPER_REF, corpus["helpers"][HELPER_REF], "H0cc0")]
    for rva, info, tag in bodies:
        text, counts = mirror_body(info["body"], rva, tag, corpus["imports"])
        out.append(f"/* {info['symbol']} ({info['unit']}) */")
        out.append(text.rstrip())
        out.append("")
        census.append((tag, counts))
    text, counts = mirror_body(corpus["template_text"], "template", "TA", corpus["imports"])
    out.append("/* template A: the 51 CallMember scalar getters (row = rva, cb, key, helper, coverage) */")
    out.append(text.rstrip())
    out.append("")
    census.append(("TA", counts))
    for rva, tag in ((GETDATA, "GD"), (BOOLII, "BI"), (INDEX2, "IX")):
        info = corpus["singles"][rva]
        text, counts = mirror_body(info["body"], rva, tag, corpus["imports"])
        out.append(f"/* {info['symbol']} ({info['unit']}) */")
        out.append(text.rstrip())
        out.append("")
        census.append((tag, counts))
    want = {"H8820": (1, 0, 0, 0, 0), "H8840": (1, 0, 0, 0, 0), "H0cc0": (1, 0, 0, 0, 0),
            "TA": (2, 0, 1, 1, 1), "GD": (5, 0, 1, 1, 2), "BI": (4, 0, 1, 0, 0), "IX": (21, 8, 0, 0, 0)}
    out.append("/* rewrite census: LBR_BRANCHES_<tag> = conditional branches; then imports routes")
    out.append(" * members getclass helpers returns */")
    for tag, counts in census:
        got = (counts["import"], counts["route"], counts["member"], counts["getclass"], counts["helper"])
        require(got == want[tag], f"mirror {tag}: rewrite census {got} != {want[tag]}")
        out.append(f"#define LBR_BRANCHES_{tag} {counts['branch']}U   /* {' '.join(str(v) for v in got)} "
                   f"{counts['return']} */")
    out.append("")
    out.append("#endif /* ISAAC_HOST_VITA_LUA_BINDING_MIRROR_H */")
    return "\n".join(out) + "\n"


def verify_pins(outputs: dict[str, str]) -> None:
    """Both generated headers must be the bytes the host oracle verified
    (EXPECTED_SHA256); the message carries the digests to adopt."""
    wrong = []
    for name, content in outputs.items():
        digest = hashlib.sha256(content.encode("utf-8")).hexdigest()
        if digest != EXPECTED_SHA256[name]:
            wrong.append(f"{name} sha256 {digest} (pinned {EXPECTED_SHA256[name]})")
    require(not wrong, "generated text differs from the pinned, oracle-verified headers: " + "; ".join(wrong) +
            ".  This PE/corpus pair is not the one recomp/test_vita_lua_binding_replay.py verified; "
            "to adopt it, set EXPECTED_SHA256 to these digests and re-run that host oracle")


def generate(pe: Path, generated: Path) -> tuple[str, str, dict, dict, Image]:
    image = Image(pe)
    pe_scan = scan_pe(image)
    corpus = scan_corpus(generated, pe_scan)
    text, mirror = render(pe_scan, corpus), render_mirror(corpus)
    verify_pins({TABLE_NAME: text, MIRROR_NAME: mirror})
    return text, mirror, pe_scan, corpus, image


def write_if_changed(path: Path, content: str) -> bool:
    """Write `content` unless the file already holds exactly these bytes (an
    unchanged header keeps its mtime, so its includers are not rebuilt)."""
    data = content.encode("utf-8")
    if path.is_file() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(data)
    os.replace(temporary, path)
    return True


def env_path(*names: str) -> Path | None:
    for name in names:
        value = os.environ.get(name)
        if value:
            return Path(value)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--pe", type=Path, default=env_path("ISAAC_FROZEN_PE", "ISAAC_PE_PATH"),
                        help="frozen isaac-ng.exe.unpacked.exe (default: $ISAAC_FROZEN_PE, then $ISAAC_PE_PATH)")
    parser.add_argument("--generated", type=Path, default=env_path("ISAAC_GENERATED_DIR"),
                        help="generated corpus directory, guest_*.c + guest_table.c (default: $ISAAC_GENERATED_DIR)")
    parser.add_argument("--out-dir", type=Path,
                        help="directory holding both headers: --write writes them there, --check compares them")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="fail if the headers in --out-dir differ from a "
                                                           "fresh generation")
    mode.add_argument("--write", action="store_true", help="generate both headers into --out-dir")
    mode.add_argument("--check-corpus", action="store_true",
                      help="configure-time gate: re-derive the corpus half of the header and the "
                           "mirror from --generated (no PE needed)")
    parser.add_argument("--header", type=Path, help=f"--check-corpus: table header (default: --out-dir/{TABLE_NAME})")
    parser.add_argument("--mirror", type=Path, help=f"--check-corpus: mirror header (default: --out-dir/{MIRROR_NAME})")
    args = parser.parse_args()
    if args.generated is None:
        parser.error("--generated is required (or set ISAAC_GENERATED_DIR)")
    if args.check_corpus:
        header = args.header or (args.out_dir / TABLE_NAME if args.out_dir else None)
        mirror_path = args.mirror or (args.out_dir / MIRROR_NAME if args.out_dir else None)
        if header is None or mirror_path is None:
            parser.error("--check-corpus needs --header and --mirror, or --out-dir")
        try:
            summary = check_corpus(args.generated, header, mirror_path)
        except (GenError, OSError) as error:
            print(f"lua_binding_replay_table: corpus check FAIL: {error}", file=sys.stderr)
            return 1
        print(f"lua_binding_replay_table: corpus {args.generated} matches the mirror ({summary})")
        return 0
    if args.pe is None:
        parser.error("--pe is required (or set ISAAC_FROZEN_PE / ISAAC_PE_PATH)")
    if args.out_dir is None:
        parser.error("--write and --check need --out-dir")
    try:
        text, mirror, pe_scan, corpus, _ = generate(args.pe, args.generated)
    except (GenError, OSError) as error:
        print(f"lua_binding_replay_table: FAIL: {error}", file=sys.stderr)
        return 1
    summary = (f"rows={len(corpus['rows'])}+3 cb1={sum(1 for r in corpus['rows'] if r['cb'])} "
               f"keys={len({r['key'] for r in corpus['rows']})} "
               f"signed={sum(1 for r in corpus['rows'] if r['helper'] == HELPER_SIGNED)} "
               f"template={corpus['template_sha256'][:16]}")
    outputs = ((args.out_dir / TABLE_NAME, text), (args.out_dir / MIRROR_NAME, mirror))
    if args.write:
        try:
            written = [path.name for path, content in outputs if write_if_changed(path, content)]
        except OSError as error:
            print(f"lua_binding_replay_table: FAIL: {error}", file=sys.stderr)
            return 1
        state = f"wrote {', '.join(written)}" if written else "up to date"
        print(f"lua_binding_replay_table: {TABLE_NAME} + {MIRROR_NAME} in {args.out_dir}: {state} ({summary})")
        return 0
    stale = [path.name for path, content in outputs
             if (path.read_bytes().decode("utf-8") if path.is_file() else "") != content]
    if stale:
        print(f"lua_binding_replay_table: FAIL: {', '.join(stale)} in {args.out_dir} differ(s) from a fresh "
              f"generation (run --write)", file=sys.stderr)
        return 1
    print(f"lua_binding_replay_table: {TABLE_NAME} + {MIRROR_NAME} in {args.out_dir} up to date ({summary})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
