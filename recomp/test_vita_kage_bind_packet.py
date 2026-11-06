#!/usr/bin/env python3
"""Prove ISAAC_VITA_KAGE_BIND_PACKET (RENDER60 W-pkt, bind packet stage 1).

The knob links a --wrap seam over frozen KAGE Shader::Bind (sub_005671d0,
runtime/host_vita_kage_bind_packet.c) that replays Bind, its two
Shader::SetUniformInt calls, Image::IsEmpty and RenderManager::Transform
natively and stages every GL call into ONE typed-recorder record
(gl_record_backend.c REC_KAGE_BIND_PACKET).

1. Frozen PE: identity; the byte ranges of the thirteen bodies the seam
   stands for (Bind, SetUniformInt, Transform, IsEmpty, the eight trivial
   target getters, the cookie check) up to their first return; the three
   uniform names; the Shader vtable slots (Bind and SetUniformInt each
   referenced exactly once in the whole image, from that vtable); the census
   of Image vtables whose +0x48 is IsEmpty and the getters in their
   +0x28/+0x2c (must equal the seam's k_getters, offsets and widths decoded
   from the getter bytes); the float/double/mask/cookie/extent constants.
2. Corpus (--generated-dir): the thirteen bodies, each defined in exactly one
   unit and pinned by text hash; sub_005671d0 is referenced only by its own
   unit, guest_table.c and guest_funcs.h (so a link-time --wrap is the whole
   seam); every address, offset, return RVA and matrix constant the seam
   uses appears in the body it was copied from.
3. Build gate: option default OFF, FATAL dependencies/conflicts, the define
   scoped to the seam TU and the recorder (and the phase profile under the
   opcode census), --wrap linked once per owner, the recorder surface
   (gl_record_surface.inc, a generated artefact this oracle compiles) equal
   to its generator's output, the README paragraph.
4. Differential oracle (MSVC x86, /LARGEADDRESSAWARE): the corpus bodies,
   extracted verbatim, in both GPR spellings, against the production
   dispatcher, gl_bridge, host_vita_gl and the REAL recorder with its
   consumer thread; the reference is the translated Bind, the candidate the
   --wrap entry.  See host_vita_kage_bind_packet_oracle.c for the case list
   and what is compared.  Flavours: prod (generated-unit shape of recipes
   A/B), guard (checked pushes), entries (body-entry census), leafcall
   (GUEST_LEAF_INLINE=0 + census), sampler (GUEST_SAMPLER: final word plus
   the word at every staged call), census (GL recorder opcode census: one
   REC_KAGE_BIND_PACKET replay per packet).
5. Must-FAIL mutants of the seam and the recorder, each failing for its
   named reason.

--pe and --generated-dir default to $REPENTOGXM_PE and $ISAAC_GENERATED_DIR.
Without both, legs 1, 2 and 4 are skipped and only the build gate and the
knob-OFF identity run.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import build_all as BA  # noqa: E402

RUNTIME = HERE / "runtime"
VITA = HERE / "vita"
VITA_CMAKE = VITA / "CMakeLists.txt"
VITA_README = VITA / "README.md"
SEAM_C = RUNTIME / "host_vita_kage_bind_packet.c"
SEAM_H = RUNTIME / "host_vita_kage_bind_packet.h"
ORACLE_C = RUNTIME / "host_vita_kage_bind_packet_oracle.c"
RECORDER_C = RUNTIME / "gl_record_backend.c"
RECORDER_H = RUNTIME / "gl_record_backend.h"
RECORDER_SOURCES = ("gl_record_backend.c", "gl_record_ring.c",
                    "gl_record_locations.c")
GL_RECORD_SURFACE_PY = VITA / "gl_record_surface.py"
GL_RECORD_SURFACE_INC = RUNTIME / "gl_record_surface.inc"

DEFAULT_PE = os.environ.get("REPENTOGXM_PE")
DEFAULT_GENERATED = os.environ.get("ISAAC_GENERATED_DIR")
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
PE_BASE = 0x00400000
GUEST_IMAGE_BASE = 0x98000000      # COMMON /DGUEST_IMAGE_BASE

# body: (rva, end rva after the first return, return bytes, sha256 of the
# bytes [rva, end), corpus text sha256)
BODIES = {
    "sub_005671d0": (0x005671D0, 0x005672C2, b"\xc2\x04\x00",
                     "15df4f923241d8a19c87ae8a848aefd1d268a35b99f3ba6e5049d875c27af00f",
                     "4c9eedac7d2a2d09bc555385245862b46e57fec86bad3062b325f791cc112d01"),
    "sub_00567430": (0x00567430, 0x00567486, b"\xc2\x0c\x00",
                     "89109c84f59f42793f533e269f4fe47bf2e7d539b0ed34fd997eb96a5ba31e07",
                     "6fe67a12c66b4f90a0cdfbb6ad1575c7f629ba173742b9dbe55df7afcc4246e8"),
    "sub_00565240": (0x00565240, 0x005653AF, b"\xc3",
                     "c424bc30a2572840e56605a4867daaabd2fa13a1c99b45b71a0096050f04ab04",
                     "47a1f6d55621f861bddff527a5628227e9ea92ad28eca45e6064e654250b8338"),
    "sub_0055f900": (0x0055F900, 0x0055F908, b"\xc3",
                     "bd48d2013e4d3b90ab7c5a0973c61f09aa4c79b48e148c90ed1829a0411c7da7",
                     "e46df3560d7442fc05c7d95fb9b471155c70953d2432742e4333edc54260b039"),
    "sub_005a0b90": (0x005A0B90, 0x005A0B97, b"\xc3",
                     "7ae3bbf974d887ad8912de6a67b7545cb3173c6a4b3ad7a4234712d65cd8d9db",
                     "d9cdcd2b414c5ac44a6a222c57f585c508a3d9ec481c7c78d0f3529c9f311466"),
    "sub_005a0ba0": (0x005A0BA0, 0x005A0BA7, b"\xc3",
                     "b6072d4fcefdd088e536843289a7c138f74a202b7213f420ff155a3cb220ec2e",
                     "d08df2011d68a044fb54a2064f91b67eb2cc572dc781bba3f0d588b081af489b"),
    "sub_005a0dd0": (0x005A0DD0, 0x005A0DD8, b"\xc3",
                     "4440170dfc72a045a3b05defd27f3ce3171a62f2382318ff2edf88f0a43a4d22",
                     "de66cf580e6c2d244f34afdc2a5da0b343449413ce66daf626bce9d8f0b8bd96"),
    "sub_005a0de0": (0x005A0DE0, 0x005A0DE8, b"\xc3",
                     "c68d32baa4a7b8d53dba2a021e3518e5419134e6a35d5988165a408104e1aa00",
                     "8b1fa18e6e1b5227d8f798b847ec23c65f9e983c63d2cc407209145b97417e5a"),
    "sub_00032ed0": (0x00032ED0, 0x00032ED7, b"\xc3",
                     "1f0f198e5bcd0f5ca2fad1aad795861a51bbff07576ba42fb28a2a28ca873926",
                     "65531d282a1322aec5a39b81c822e2d9e282784bccbd7225b8c7a5a386571d51"),
    "sub_005a1a70": (0x005A1A70, 0x005A1A77, b"\xc3",
                     "383eb2f05c2c47ea282d677f4288504de6e0bda1e96028fea9c9ee5b539ee25f",
                     "476422e9494cbc817c3aee0ba9be84f9d5b26a7d66017644d9177a426c438585"),
    "sub_005a1ef0": (0x005A1EF0, 0x005A1EF7, b"\xc3",
                     "9107d0afff035a7f6d24fc474ee1928e2f3e4c5dfea37a5285b22d5b1b196336",
                     "827242ed16f75279731b91aca0f333f2d24a4253552fc7705bf4de541f1e8514"),
    "sub_005a1f00": (0x005A1F00, 0x005A1F07, b"\xc3",
                     "2ca4d4b3b4236810e391c18abad88834f6438777c0a3a88da8f6fc857395ddaf",
                     "cbca7b64b31f379e5cb727ffc9bafa912cc1f6e22778deda2b43a233ba6c6925"),
    "sub_005eacd7": (0x005EACD7, 0x005EACE0, b"\xc3",
                     "c924d74dfee404bba76e1b666ac901a2ac8b75c081459686ef84db60f976caa6",
                     "1ef919a191eaa0654925dde6806ca45db9ca0a4bf64b6ae31495b05a69c4f872"),
}
SHADER_VTABLE = 0x0075E0A8
BIND_SLOT, SET_UNIFORM_INT_SLOT = 4, 7
IS_EMPTY = 0x0055F900
IMAGE_VTABLES = (0x0075D2B4, 0x00765E90, 0x00766028, 0x007661C0,
                 0x00766258, 0x007662F0, 0x00767FE8, 0x007682AC)
PURECALL = 0x005EC13A
NAMES = {0x0075E098: b"Texture0\x00", 0x0075E03C: b"Texture1\x00",
         0x00744AC0: b"Transform\x00"}
# The spans the oracle commits at their real addresses (argv "rva:hex").
SPANS = ((0x00744AC0, 10), (0x0075E03C, 9), (0x0075E098, 9),
         (SHADER_VTABLE, 0x24), (0x0076A7B0, 4), (0x0076C940, 16),
         (0x0076C9B0, 16), (0x007AA3B4, 4), (0x007AA92C, 8))

COMMON = [
    "/nologo", "/W4", "/WX", "/O2", "/Gy", "/std:c11",
    "/wd4310", "/wd4702", "/wd4996",
    "/DGUEST_IMAGE_BASE=0x98000000u",
]
DISPATCH_DEFINES = [
    "/DGUEST_STACK_REQUIRED=1",
    "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
    "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
    "/DISAAC_VITA_PHASE_PROFILE=1",
    "/DISAAC_VITA_GL_SHIM_FASTDISPATCH=1",
    "/DISAAC_VITA_SHADER_ATTRIB_FASTPATH=1",
]
RECORDER_DEFINES = [
    "/DISAAC_VITA_GL_THREAD_RECORD=1",
    "/DISAAC_VITA_GL_THREAD_WORKER=1",
    "/DISAAC_GL_RECORD_WORKER_JITTER=1",
    "/DISAAC_VITA_KAGE_BIND_PACKET=1",
]
# Production compile definitions of a generated unit that matter to these
# bodies (recomp/vita/CMakeLists.txt with GUEST_STACK_GUARD OFF:
# GUEST_GENERATED_STACK_GUARD=0, GUEST_FLAGS_LOCAL=1,
# GUEST_COVERAGE_HOOKS=0; GUEST_GPR_LOCAL per variant).
BODY_DEFINES = [
    "/DGUEST_STACK_REQUIRED=1",
    "/DGUEST_FLAGS_LOCAL=1",
    "/DGUEST_COVERAGE_HOOKS=0",
    "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
    "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
    "/DISAAC_VITA_PHASE_PROFILE=1",
    "/DISAAC_VITA_GL_SHIM_FASTDISPATCH=1",
]
# The seam TU's definitions from its CMake block (target-wide dispatch
# definitions reach it too).
SEAM_DEFINES = DISPATCH_DEFINES + [
    "/DISAAC_VITA_GL_THREAD_RECORD=1",
    "/DISAAC_VITA_GL_THREAD_WORKER=1",
    "/DISAAC_VITA_KAGE_BIND_PACKET=1",
]
STAGE_RENAMES = [
    f"/Disaac_gl_record_bind_packet_{name}=oracle_bp_{name}"
    for name in ("use_program", "active_texture", "bind_texture",
                 "uniform_location", "uniform1i", "uniform_matrix4fv")
]
ENTRIES = ["/DISAAC_VITA_PROFILE_FUNCTION_ENTRIES=1"]

# Every flavour runs the same case list to the same verdicts and counts.
ORACLE_RESULT = (
    "Vita KAGE bind packet oracle: PASS; cases=420; steps=538; "
    "handled=466; declined=72; cold=936; memo=138; cache=140; "
    "records(ref,seam)=4462,2338")
STAGE_RESULT = "stage words: checks=4462 mismatches=0"
REASON_RESULT = (
    "declines by reason: frame=8 object=8 records=4 "
    "image=4 target=8 tokens=36 recorder=2 owner=2")
# flavour: (body+seam defines, seam-only defines, oracle defines, result)
FLAVOURS = {
    "prod": (["/DGUEST_GENERATED_STACK_GUARD=0"], [], [], ORACLE_RESULT),
    "guard": (["/DGUEST_GENERATED_STACK_GUARD=1"], [], [], ORACLE_RESULT),
    "entries": (["/DGUEST_GENERATED_STACK_GUARD=0"] + ENTRIES, [], ENTRIES,
                ORACLE_RESULT),
    "leafcall": (["/DGUEST_GENERATED_STACK_GUARD=0", "/DGUEST_LEAF_INLINE=0"]
                 + ENTRIES, [], ENTRIES, ORACLE_RESULT),
    "sampler": (["/DGUEST_GENERATED_STACK_GUARD=0"],
                ["/DISAAC_VITA_GUEST_SAMPLER=1"] + STAGE_RENAMES,
                ["/DISAAC_VITA_GUEST_SAMPLER=1", "/DORACLE_STAGE_WORDS=1"],
                ORACLE_RESULT),
    "census": (["/DGUEST_GENERATED_STACK_GUARD=0"], [],
               ["/DISAAC_GL_RECORD_OPCODE_CENSUS=1"], ORACLE_RESULT),
}

# mutant: (flavour, file, old, new, the FAIL reason it must produce)
MUTANTS = {
    "matrix10": ("prod", "seam", "matrix[10] = 0xba83126fu;",
                 "matrix[10] = 0xba83126eu;", "GL call stream differs"),
    "uniform1i-rva": ("prod", "seam",
                      "isaac_gl_record_bind_packet_uniform1i(KBP_RET_UNIFORM1I,",
                      "isaac_gl_record_bind_packet_uniform1i(KBP_RET_UNIFORM1I + 1u,",
                      "GL call stream differs"),
    "edx": ("prod", "seam", "    c->edx = begin;\n", "    (void)begin;\n",
            "CPU/stack/object state differs"),
    "kind-rule": ("prod", "seam", "if (kind != 2u || unit == 0u) {",
                  "if (kind != 2u || unit == 1u) {", "GL call stream differs"),
    "low-water": ("prod", "seam", "guest_stack_note_low(c, k.entry - low);",
                  "guest_stack_note_low(c, k.entry - low + 4u);",
                  "CPU/stack/object state differs"),
    "list-store": ("prod", "seam", "        st32(k.entry + 4u, list);\n",
                   "        (void)list;\n", "CPU/stack/object state differs"),
    "memo-bytes": ("prod", "recorder",
                   "memo->name == name && record_bind_memo_bytes(memo, name)) {",
                   "memo->name == name) {", "GL call stream differs"),
    "memo-epoch": ("prod", "recorder",
                   "if (memo->epoch == s_location_epoch && memo->program == program &&",
                   "if (memo->epoch != 0u && memo->program == program &&",
                   "GL call stream differs"),
    "sampler-restore": ("sampler", "seam",
                        "            k.entry - KBP_MATRIX_BELOW_ENTRY, matrix);\n"
                        "        KBP_PUBLISH(k.enclosing);\n",
                        "            k.entry - KBP_MATRIX_BELOW_ENTRY, matrix);\n",
                        "CPU/stack/object state differs"),
    "stage-word": ("sampler", "seam",
                   "    if (kind != 2u || unit == 0u) {\n"
                   "        KBP_PUBLISH(s_tok_bind);\n",
                   "    if (kind != 2u || unit == 0u) {\n",
                   "stage words"),
}

BODY_RE = re.compile(
    r"^/\* (sub_[0-9a-f]{8})  RVA [0-9a-f]{8}  \d+ insns, \d+ switch entries \*/\n"
    r"void \1\(CPU \*__restrict c\)\n\{\n.*?^\}\n", re.S | re.M)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


BELOW_NORMAL_PRIORITY_CLASS = 0x00004000


def run(command: list[str], env: dict[str, str], cwd: Path | None = None,
        may_fail: bool = False) -> tuple[int, str]:
    completed = subprocess.run(
        command, cwd=cwd or HERE, env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        creationflags=BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0,
    )
    out = completed.stdout.decode("cp866", errors="replace").replace(
        "\r\n", "\n")
    if completed.returncode != 0 and not may_fail:
        print(out[-8000:])
        raise AssertionError(
            f"command failed ({completed.returncode}): {command[:3]}...")
    return completed.returncode, out


# ---- 1. frozen PE ------------------------------------------------------------

class Pe:
    def __init__(self, data: bytes):
        self.data = data
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        count = struct.unpack_from("<H", data, e_lfanew + 6)[0]
        optional = struct.unpack_from("<H", data, e_lfanew + 20)[0]
        self.image_size = struct.unpack_from("<I", data, e_lfanew + 24 + 56)[0]
        self.sections = []
        offset = e_lfanew + 24 + optional
        for _ in range(count):
            name = data[offset:offset + 8].rstrip(b"\0").decode("ascii")
            vsize, va, rsize, rp = struct.unpack_from("<IIII", data, offset + 8)
            self.sections.append((name, va, vsize, rsize, rp))
            offset += 40

    def read(self, rva: int, size: int) -> bytes:
        for _, va, vsize, rsize, rp in self.sections:
            if va <= rva and rva + size <= va + max(vsize, rsize):
                raw = self.data[rp + rva - va: rp + min(rva - va + size, rsize)]
                return raw + b"\0" * (size - len(raw))
        raise AssertionError(f"RVA {rva:#x}+{size} outside every section")

    def u32(self, rva: int) -> int:
        return struct.unpack("<I", self.read(rva, 4))[0]

    def section_of(self, rva: int) -> str:
        for name, va, vsize, rsize, _ in self.sections:
            if va <= rva < va + max(vsize, rsize):
                return name
        return "?"

    def dword_refs(self, value: int) -> list[int]:
        """RVAs of every occurrence of the little-endian dword (any
        alignment) in the raw section data."""
        needle = struct.pack("<I", value)
        out = []
        for _, va, _, rsize, rp in self.sections:
            blob = self.data[rp:rp + rsize]
            at = blob.find(needle)
            while at >= 0:
                out.append(va + at)
                at = blob.find(needle, at + 1)
        return sorted(out)


def seam_getters(text: str) -> list[tuple[int, int, int]]:
    block = re.search(r"static const kbp_getter k_getters\[8\] = \{(.*?)\};",
                      text, re.S)
    require(block is not None, "k_getters table not found in the seam")
    rows = re.findall(r"\{0x([0-9a-f]{8})u, 0x([0-9a-f]+)u, ([01])u\}",
                      block.group(1))
    require(len(rows) == 8, f"k_getters rows: {len(rows)}")
    return [(int(r, 16), int(o, 16), int(h)) for r, o, h in rows]


def decode_getter(body: bytes) -> tuple[int, int]:
    """(field offset, halfword) of `mov eax, [ecx+disp32]; ret` or
    `movzx eax, word [ecx+disp32]; ret`."""
    if len(body) == 7 and body[:2] == b"\x8b\x81" and body[6] == 0xC3:
        return struct.unpack_from("<I", body, 2)[0], 0
    if len(body) == 8 and body[:3] == b"\x0f\xb7\x81" and body[7] == 0xC3:
        return struct.unpack_from("<I", body, 3)[0], 1
    raise AssertionError(f"getter shape {body.hex()}")


def relocate_vtable(pe: Pe, rva: int, size: int) -> bytes:
    words = struct.unpack(f"<{size // 4}I", pe.read(rva, size))
    out = []
    for word in words:
        if PE_BASE <= word < PE_BASE + pe.image_size:
            word = word - PE_BASE + GUEST_IMAGE_BASE
        out.append(word)
    return struct.pack(f"<{len(out)}I", *out)


def verify_frozen_pe(pe_path: Path) -> tuple[list[str], list[str]]:
    require(pe_path.stat().st_size == PE_SIZE, "frozen PE size changed")
    require(digest(pe_path) == PE_SHA256, "frozen PE hash changed")
    pe = Pe(pe_path.read_bytes())
    lines = []
    for name, (rva, end, ret, want, _) in BODIES.items():
        body = pe.read(rva, end - rva)
        require(body.endswith(ret), f"{name}: does not end in {ret.hex()}")
        if want is not None:
            require(sha(body) == want, f"{name}: body bytes changed")
    for rva, text in NAMES.items():
        require(pe.read(rva, len(text)) == text, f"name at {rva:#x}")
    bind_va = PE_BASE + BODIES["sub_005671d0"][0]
    sui_va = PE_BASE + BODIES["sub_00567430"][0]
    require(pe.u32(SHADER_VTABLE + 4 * BIND_SLOT) == bind_va, "Bind slot")
    require(pe.u32(SHADER_VTABLE + 4 * SET_UNIFORM_INT_SLOT) == sui_va,
            "SetUniformInt slot")
    require(pe.dword_refs(bind_va) == [SHADER_VTABLE + 4 * BIND_SLOT],
            f"Bind references {[hex(r) for r in pe.dword_refs(bind_va)]}")
    require(pe.dword_refs(sui_va) == [SHADER_VTABLE + 4 * SET_UNIFORM_INT_SLOT],
            f"SetUniformInt references "
            f"{[hex(r) for r in pe.dword_refs(sui_va)]}")
    # Image vtables: every IsEmpty reference sits at +0x48 of one.
    refs = pe.dword_refs(PE_BASE + IS_EMPTY)
    vtables = tuple(r - 0x48 for r in refs)
    require(vtables == IMAGE_VTABLES,
            f"IsEmpty vtables {[hex(v) for v in vtables]}")
    require(all(pe.section_of(v) == ".rdata" for v in vtables),
            "IsEmpty vtable outside .rdata")
    slot_words = {pe.u32(v + off) - PE_BASE for v in vtables
                  for off in (0x28, 0x2C)}
    getters = seam_getters(SEAM_C.read_text(encoding="utf-8"))
    require(slot_words == {g[0] for g in getters} | {PURECALL},
            f"getter census {sorted(hex(w) for w in slot_words)}")
    for rva, offset, half in getters:
        name = f"sub_{rva:08x}"
        start, end = BODIES[name][0], BODIES[name][1]
        require(start == rva, f"{name}: body table")
        require(decode_getter(pe.read(rva, end - rva)) == (offset, half),
                f"{name}: seam offset/width differ from the bytes")
    # Constants Transform reads.
    require(struct.unpack("<f", pe.read(0x0076A7B0, 4))[0] == 2.0, "2.0f")
    require(struct.unpack("<2d", pe.read(0x0076C940, 16)) ==
            (0.0, 4294967296.0), "u32 bias doubles")
    require(struct.unpack("<4I", pe.read(0x0076C9B0, 16)) ==
            (0x80000000,) * 4, "sign mask")
    require(pe.u32(0x007AA3B4) == 0xBB40E64E, "cookie initial value")
    require(struct.unpack("<2f", pe.read(0x007AA92C, 8)) == (480.0, 270.0),
            "default extent")
    spans = []
    for rva, size in SPANS:
        data = (relocate_vtable(pe, rva, size) if rva == SHADER_VTABLE
                else pe.read(rva, size))
        spans.append(f"{rva:06x}:{data.hex()}")
    lines.append(
        f"frozen PE: 13 bodies, Bind/SetUniformInt referenced once each "
        f"(vtable {SHADER_VTABLE:06x}), {len(vtables)} Image vtables, "
        f"getters={len(getters)}+_purecall")
    return spans, lines


# ---- 2. corpus ---------------------------------------------------------------

def extract_bodies(generated_dir: Path) -> tuple[dict[str, str], list[str]]:
    bodies: dict[str, str] = {}
    owners: dict[str, list[str]] = {name: [] for name in BODIES}
    bind_refs = []
    for unit in sorted(generated_dir.glob("*.[ch]")):
        text = unit.read_text(encoding="utf-8")
        if "sub_005671d0" in text:
            bind_refs.append(unit.name)
        if not unit.name.startswith("guest_") or unit.suffix != ".c":
            continue
        for match in BODY_RE.finditer(text):
            name = match.group(1)
            if name in owners:
                owners[name].append(unit.name)
                bodies[name] = match.group(0)
    for name, units in owners.items():
        require(len(units) == 1, f"{name}: owner units {units}")
        want = BODIES[name][4]
        if want is not None:
            require(sha(bodies[name].encode("utf-8")) == want,
                    f"{name}: corpus text changed")
    bind_owner = owners["sub_005671d0"][0]
    require(sorted(bind_refs) == sorted({bind_owner, "guest_table.c",
                                         "guest_funcs.h"}),
            f"sub_005671d0 referenced from {bind_refs}")
    owner_text = (generated_dir / bind_owner).read_text(encoding="utf-8")
    require(owner_text.count("sub_005671d0") == 2,
            "sub_005671d0 referenced inside its own unit")
    # Callees outside the extracted set: only the cookie report's 005eb345.
    defined = set(bodies)
    called = set()
    for text in bodies.values():
        called |= set(re.findall(r"\b(sub_[0-9a-f]{8})\(c\)", text))
    require(called - defined == {"sub_005eb345"},
            f"external callees {sorted(called - defined)}")
    lines = [f"corpus: {len(bodies)} bodies in "
             f"{len({u[0] for u in owners.values()})} units; sub_005671d0 "
             f"referenced by {', '.join(sorted(bind_refs))}"]
    return bodies, lines


def macro_values(text: str) -> dict[str, int]:
    values = {}
    for name, va, value in re.findall(
            r"^#define (KBP_\w+) (KBP_VA\()?0x([0-9a-f]+)u\)?", text, re.M):
        values[name] = int(value, 16) + (GUEST_IMAGE_BASE if va else 0)
    return values


# (macro, body whose text must contain the snippet, snippet with {v} = value)
SEAM_CONSTANTS = [
    ("KBP_SHADER_VTABLE", None, None),
    ("KBP_SET_UNIFORM_INT", None, None),
    ("KBP_IMAGE_IS_EMPTY", None, None),
    ("KBP_OFF_RECORDS_LIST", "sub_005671d0", "lea eax, [esi + {h}]"),
    ("KBP_OFF_PROGRAM", "sub_005671d0", "push dword ptr [esi + {h}]"),
    ("KBP_OFF_SET_UNIFORM_INT", "sub_005671d0", "call dword ptr [eax + {h}]"),
    ("KBP_CURRENT_IMAGE", "sub_005671d0", "mov ecx, dword ptr [{a}]"),
    ("KBP_OFF_IS_EMPTY", "sub_005671d0", "mov eax, dword ptr [eax + {h}]"),
    ("KBP_IMAGE_OFF_HANDLE", "sub_0055f900", "cmp dword ptr [ecx + {h}], 0"),
    ("KBP_IMAGE_OFF_KIND", "sub_00567430", "cmp dword ptr [eax + {h}], 2"),
    ("KBP_IMAGE_OFF_TEX0", "sub_00567430", "push dword ptr [eax + {h}]"),
    ("KBP_IMAGE_OFF_TEX1", "sub_00567430", "push dword ptr [eax + {h}]"),
    ("KBP_MANAGER", "sub_005671d0", "mov ecx, {a}"),
    ("KBP_TARGET_OFF_WIDTH", "sub_00565240", "mov eax, dword ptr [eax + {h}]"),
    ("KBP_TARGET_OFF_HEIGHT", "sub_00565240", "mov eax, dword ptr [eax + {h}]"),
    ("KBP_NAME_TEXTURE0", "sub_005671d0", "push {a}"),
    ("KBP_NAME_TEXTURE1", "sub_005671d0", "push {a}"),
    ("KBP_NAME_TRANSFORM", "sub_00565240", "push {a}"),
    ("KBP_COOKIE", "sub_00565240", "mov eax, dword ptr [{a}]"),
    ("KBP_DEFAULT_WIDTH", "sub_00565240", "{a}"),
    ("KBP_DEFAULT_HEIGHT", "sub_00565240", "{a}"),
    ("KBP_TWO", "sub_00565240", "{a}"),
    ("KBP_U32_BIAS", "sub_00565240", "qword ptr [eax*8 - {n}]"),
    ("KBP_SIGN_MASK", "sub_00565240", "{a}"),
    ("KBP_GL_TEXTURE0", "sub_00567430", "lea eax, [esi + {h}]"),
    ("KBP_GL_TEXTURE_2D", "sub_00567430", "push {h}"),
    ("KBP_SLOT_USE_PROGRAM", "sub_005671d0", "call dword ptr [{a}]"),
    ("KBP_SLOT_ACTIVE_TEXTURE", "sub_00567430", "call dword ptr [{a}]"),
    ("KBP_SLOT_BIND_TEXTURE", "sub_00567430", "call dword ptr [{a}]"),
    ("KBP_SLOT_GET_UNIFORM", "sub_00567430", "call dword ptr [{a}]"),
    ("KBP_SLOT_UNIFORM1I", "sub_00567430", "call dword ptr [{a}]"),
    ("KBP_SLOT_MATRIX4", "sub_00565240", "call dword ptr [{a}]"),
    ("KBP_RET_USE_PROGRAM", "sub_005671d0", "GPUSH({r});"),
    ("KBP_RET_SET_UNIFORM_INT0", "sub_005671d0", "GPUSH({r});"),
    ("KBP_RET_IS_EMPTY", "sub_005671d0", "GPUSH({r});"),
    ("KBP_RET_SET_UNIFORM_INT1", "sub_005671d0", "GPUSH({r});"),
    ("KBP_RET_ACTIVE_TEXTURE", "sub_00567430", "GPUSH({r});"),
    ("KBP_RET_BIND_TEXTURE", "sub_00567430", "GPUSH({r});"),
    ("KBP_RET_GET_UNIFORM_INT", "sub_00567430", "GPUSH({r});"),
    ("KBP_RET_UNIFORM1I", "sub_00567430", "GPUSH({r});"),
    ("KBP_RET_GET_UNIFORM_TRANSFORM", "sub_00565240", "GPUSH({r});"),
    ("KBP_RET_MATRIX4", "sub_00565240", "GPUSH({r});"),
]
MATRIX_CONSTANTS = {10: "0xba83126fU", 14: "0x80000000U", 15: "0x3f800000U"}


def verify_seam_constants(bodies: dict[str, str]) -> list[str]:
    text = SEAM_C.read_text(encoding="utf-8")
    values = macro_values(text)
    checked = 0
    for macro, body, snippet in SEAM_CONSTANTS:
        require(macro in values, f"seam macro {macro} missing")
        if body is None:
            continue
        v = values[macro]
        literal = snippet.format(h=f"{v:#x}", a=f"{v:#010x}", r=f"{v:#x}U",
                                 n=f"{(1 << 32) - v:#x}")
        require(literal in bodies[body],
                f"{macro}={v:#x}: '{literal}' not in {body}")
        checked += 1
    require(values["KBP_SHADER_VTABLE"] == GUEST_IMAGE_BASE + SHADER_VTABLE,
            "seam Shader vtable")
    require(values["KBP_SET_UNIFORM_INT"] ==
            GUEST_IMAGE_BASE + BODIES["sub_00567430"][0], "seam SetUniformInt")
    require(values["KBP_IMAGE_IS_EMPTY"] == GUEST_IMAGE_BASE + IS_EMPTY,
            "seam IsEmpty")
    for index, literal in MATRIX_CONSTANTS.items():
        require(f"st32((uint32_t)(GR(ebp) + (uint32_t)(int32_t)"
                f"({-0x44 + 4 * index})), (uint32_t)({literal}));"
                in bodies["sub_00565240"], f"matrix[{index}] store in Transform")
        require(f"matrix[{index}] = {literal[:-1]}u;" in text,
                f"seam matrix[{index}]")
    # Every PE-derived address the oracle commits is one the seam names.
    for rva, _ in SPANS:
        require(any(v == GUEST_IMAGE_BASE + rva for v in values.values()),
                f"span {rva:#x} is no seam constant")
    return [f"seam constants: {checked} corpus literals, "
            f"{len(MATRIX_CONSTANTS)} matrix stores, {len(SPANS)} PE spans"]


# ---- 3. build gate -----------------------------------------------------------

def load_record_surface():
    spec = importlib.util.spec_from_file_location("gl_record_surface_pkt",
                                                  GL_RECORD_SURFACE_PY)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def verify_build_gate() -> list[str]:
    cmake = VITA_CMAKE.read_text(encoding="utf-8")
    require(re.search(r"option\(ISAAC_VITA_KAGE_BIND_PACKET\s+\"[^\"]+\"\s+OFF\)",
                      cmake) is not None, "option default OFF")
    fatal = re.search(
        r"if\(ISAAC_VITA_KAGE_BIND_PACKET AND\s*\((.*?)\)\)\s*\n\s*"
        r"message\(FATAL_ERROR \"KAGE_BIND_PACKET requires", cmake, re.S)
    require(fatal is not None, "FATAL block")
    terms = set(re.findall(r"(NOT )?ISAAC_VITA_(\w+)", fatal.group(1)))
    require(terms == {("NOT ", "KAGE"), ("NOT ", "GL_SHIM_FASTDISPATCH"),
                      ("NOT ", "SHADER_ATTRIB_FASTPATH"),
                      ("NOT ", "GL_THREAD_RECORD"),
                      ("NOT ", "GL_THREAD_WORKER"),
                      ("", "UNIFORM_SET_RECEIPT"), ("", "STALL_PROBE")},
            f"FATAL terms {sorted(terms)}")
    block = re.search(r"\n  if\(ISAAC_VITA_KAGE_BIND_PACKET\)\n(.*?)\n  endif\(\)\n",
                      cmake, re.S)
    require(block is not None, "knob block")
    body = block.group(1)
    require(body.count("-Wl,--wrap=sub_005671d0") == 1, "--wrap in the block")
    require(cmake.count("--wrap=sub_005671d0") == 2,
            "sub_005671d0 wrapped by exactly this knob and UNIFORM_SET_RECEIPT")
    require(cmake.count("ISAAC_VITA_KAGE_BIND_PACKET=1") == 2 and
            body.count("ISAAC_VITA_KAGE_BIND_PACKET=1") == 2,
            "the define is set only inside the knob block")
    scoped = re.findall(r"set_property\(SOURCE(.*?)APPEND PROPERTY "
                        r"COMPILE_DEFINITIONS ISAAC_VITA_KAGE_BIND_PACKET=1",
                        body, re.S)
    units = sorted(set(re.findall(r"\$\{ISAAC_RUNTIME\}/(\w+\.c)",
                                  "".join(scoped))))
    require(units == ["gl_record_backend.c", "host_vita_kage_bind_packet.c",
                      "kage_vita_phase_profile.c"],
            f"define scope {units}")
    require("if(ISAAC_VITA_GL_RECORD_OPCODE_CENSUS)" in body,
            "phase profile sees the define only with the opcode census")
    for define in ("GUEST_STACK_REQUIRED=1", "ISAAC_VITA_SHADER_ATTRIB_FASTPATH=1",
                   "ISAAC_VITA_GL_SHIM_FASTDISPATCH=1",
                   "ISAAC_VITA_GL_THREAD_RECORD=1",
                   "ISAAC_VITA_GL_THREAD_WORKER=1",
                   "GUEST_GENERATED_STACK_GUARD=0", "GUEST_LEAF_INLINE=0",
                   "ISAAC_VITA_GUEST_SAMPLER=1"):
        require(define in body, f"seam definition {define}")
    seam = SEAM_C.read_text(encoding="utf-8")
    for needle in ("#error Compile the bind packet only when explicitly enabled",
                   "void __wrap_sub_005671d0(CPU *__restrict c)",
                   "__real_sub_005671d0(c);"):
        require(needle in seam, f"seam: {needle}")
    # Opcode 81, last: spelled as a macro on this base, as REC_WORKER_CALL + 3
    # in the anonymous-enum form once RENDER60 S4's 79/80 are in the tree.
    # The recorder static-asserts it against the census bound (compiled by
    # the census flavour below).
    recorder = RECORDER_C.read_text(encoding="utf-8")
    require("# define REC_KAGE_BIND_PACKET 81u" in recorder or
            "enum { REC_KAGE_BIND_PACKET = REC_WORKER_CALL + 3 };" in recorder, "opcode 81")
    require(len(re.findall(r"_Static_assert\(REC_KAGE_BIND_PACKET \+ 1u == "
                           r"ISAAC_GL_RECORD_(?:CENSUS_OPS|OPCODE_SLOTS)", recorder)) >= 1,
            "opcode 81 asserted last")
    header = RECORDER_H.read_text(encoding="utf-8")
    require("82u" in header, "census bound 82 under the knob")
    surface = load_record_surface()
    require(GL_RECORD_SURFACE_INC.read_text(encoding="utf-8") == surface.emit(),
            "gl_record_surface.inc is stale: run recomp/vita/"
            "gl_record_surface.py --write")
    readme = VITA_README.read_text(encoding="utf-8")
    for needle in ("ISAAC_VITA_KAGE_BIND_PACKET", "test_vita_kage_bind_packet.py",
                   "KAGE VITA BIND PACKET:"):
        require(needle in readme, f"README: {needle}")
    return ["build gate: default OFF, FATAL deps/conflicts, define in "
            "seam+recorder(+phase profile with census), one --wrap, recorder "
            "surface regenerated equal, README"]


# ---- 3b. knob-OFF identity (host proxy of the ARM object identity gate) ------

BASE_COMMIT = "9a2b0992fbe1c1e27e6c0e218e62449a99b56b62"
KNOB = "ISAAC_VITA_KAGE_BIND_PACKET=1"
CENSUS = "ISAAC_GL_RECORD_OPCODE_CENSUS=1"


def commit_available(commit: str) -> bool:
    """False in a checkout whose history lacks the commit (a published snapshot)."""
    return subprocess.run(["git", "cat-file", "-e", commit + "^{commit}"], cwd=HERE,
                          capture_output=True).returncode == 0


def verify_off_identity(base: str, out: Path) -> list[str]:
    """clang -E (armv7a, __vita__, the production target-wide definitions,
    stub system headers) of every unit this knob touches and of every unit
    including gl_record_backend.h, at the base commit and now, without the
    knob: token-equal, with and without the opcode census.  Positive
    controls: the knob changes the recorder and (with the census) the phase
    profile."""
    if not commit_available(base):
        return [f"knob-OFF identity: SKIP (base commit {base[:7]} is not in this "
                "repository's history)"]
    sys.path.insert(0, str(VITA))
    import test_gl_record_free_wrap as s1  # noqa: E402

    base_runtime = s1.export_base(base, out)
    stubs = out / "stub-include"
    stubs.mkdir(parents=True, exist_ok=True)
    rec, wrk, pp = s1.REC, s1.WRK, "ISAAC_VITA_PHASE_PROFILE=1"
    includers = sorted(
        unit.name for unit in RUNTIME.glob("*.c")
        if re.search(r'#\s*include "gl_record_backend.h"',
                     unit.read_text(encoding="utf-8", errors="replace"))
        and not unit.name.endswith("_oracle.c"))
    expected = ["gl_bridge.c", "gl_record_backend.c", "gl_record_free_wrap.c",
                "gl_vita_backend.c", "host_vita_debug_overlay.c",
                "host_vita_kage_bind_packet.c",
                "host_vita_shader_attrib_fastpath.c", "kage_vita_backend.c",
                "kage_vita_loading.c", "kage_vita_phase_profile.c",
                "manual_kage_vita.c"]
    require(includers == expected, f"gl_record_backend.h includers {includers}")
    sets = {
        "gl_record_backend.c": [[rec], [rec, wrk], [rec, wrk, s1.CACHE],
                                [rec, wrk, s1.CACHE, CENSUS]],
        "kage_vita_phase_profile.c": [[pp], [pp, wrk], [pp, wrk, CENSUS]],
        # RENDER60 S4's replay TU includes the header only under its own knob.
        "host_vita_shader_attrib_fastpath.c": [
            ["ISAAC_VITA_SHADER_ATTRIB_FASTPATH=1", "ISAAC_VITA_GL_SHIM_FASTDISPATCH=1", rec, wrk]],
    }
    for unit in includers:
        if unit in sets or unit in ("host_vita_kage_bind_packet.c",
                                    "gl_record_free_wrap.c"):
            continue
        extra = (["ISAAC_VITA_DEBUG_OVERLAY=1"]
                 if unit == "host_vita_debug_overlay.c" else [])
        sets[unit] = [[rec, wrk, pp] + extra, [rec, wrk, pp, CENSUS] + extra]
    compared = 0
    for unit, variants in sets.items():
        for defines in variants:
            old = s1.preprocess(base_runtime, unit, defines, stubs).split()
            new = s1.preprocess(RUNTIME, unit, defines, stubs).split()
            require(old == new, f"identity {unit} {defines}: tokens differ "
                                f"from {base[:7]} ({len(old)} vs {len(new)})")
            require(len(new) > 1000, f"identity {unit}: vacuous ({len(new)})")
            compared += 1
    controls = (("gl_record_backend.c", [rec, wrk, s1.CACHE, KNOB],
                 "isaac_gl_record_bind_packet_admit"),
                ("kage_vita_phase_profile.c", [pp, wrk, CENSUS, KNOB], "82u"))
    for unit, defines, needle in controls:
        old = s1.preprocess(base_runtime, unit,
                            [d for d in defines if d != KNOB], stubs)
        new = s1.preprocess(RUNTIME, unit, defines, stubs)
        require(new.split() != old.split() and needle in new,
                f"identity control {unit} +knob: no effect")
    return [f"knob-OFF identity: {compared} unit/define sets token-equal to "
            f"{base[:7]} (clang -E armv7a), {len(controls)} positive controls"]


# ---- 4. oracle ---------------------------------------------------------------

def bodies_source(bodies: dict[str, str]) -> str:
    out = ["/* Generated by test_vita_kage_bind_packet.py: the frozen corpus",
           " * bodies, verbatim. */",
           "#include <math.h>", '#include "guest.h"']
    names = sorted(set(bodies) | {"sub_005eb345"})
    out += [f"void {name}(CPU *__restrict c);" for name in names]
    out.append("")
    out += [bodies[name] for name in BODIES]
    return "\n".join(out) + "\n"


def mem_renames() -> list[str]:
    return [f"/D{name}=mem_{name}" for name in sorted(set(BODIES) |
                                                       {"sub_005eb345"})]


def build_flavour(env, cl: str, work: Path, tag: str, bodies_c: Path,
                  flavour: str, seam_c: Path = SEAM_C,
                  recorder_c: Path = RECORDER_C) -> Path:
    shared, seam_only, oracle_defs, _ = FLAVOURS[flavour]
    out = work / tag
    out.mkdir()
    objects = []
    for variant, extra in (("gpr", ["/DGUEST_GPR_LOCAL=1"]),
                           ("mem", ["/DGUEST_GPR_LOCAL=0"] + mem_renames())):
        obj = out / f"bodies_{variant}.obj"
        run([cl, "/nologo", "/W3", "/O2", "/std:c11", "/wd4310", "/wd4996",
             "/DGUEST_IMAGE_BASE=0x98000000u"] + BODY_DEFINES + shared +
            extra + ["/I", str(RUNTIME), "/c", str(bodies_c),
                     "/Fo:" + str(obj)], env)
        objects.append(str(obj))
    for variant, extra in (
            ("gpr", ["/D__real_sub_005671d0=sub_005671d0"]),
            ("mem", ["/D__wrap_sub_005671d0=mem_wrap_sub_005671d0",
                     "/D__real_sub_005671d0=mem_sub_005671d0",
                     "/Disaac_vita_kage_bind_packet_counts="
                     "mem_isaac_vita_kage_bind_packet_counts",
                     "/Disaac_vita_kage_bind_packet_declines="
                     "mem_isaac_vita_kage_bind_packet_declines"])):
        obj = out / f"seam_{variant}.obj"
        run([cl] + COMMON + SEAM_DEFINES + shared + seam_only + extra +
            ["/I", str(RUNTIME), "/c", str(seam_c), "/Fo:" + str(obj)], env)
        objects.append(str(obj))
    recorder = [str(RUNTIME / unit) for unit in RECORDER_SOURCES]
    recorder[0] = str(recorder_c)
    exe = out / f"kage-bind-packet-oracle-{tag}.exe"
    objdir = out / "obj"
    objdir.mkdir()
    # guest.c (included by the oracle) defines the generated-unit stack
    # helpers itself: the body/seam spellings in `shared` stay out of it.
    run([cl] + COMMON + DISPATCH_DEFINES + RECORDER_DEFINES +
        oracle_defs + ["/I", str(RUNTIME), str(ORACLE_C),
                       str(RUNTIME / "gl_bridge.c"),
                       str(RUNTIME / "host_vita_gl.c")] + recorder + objects +
        ["/Fe:" + str(exe), "/Fo:" + str(objdir) + os.sep,
         "/link", "/LARGEADDRESSAWARE", "/OPT:REF", "/INCREMENTAL:NO"], env)
    return exe


def run_oracle(env, exe: Path, spans: list[str], work: Path, tag: str,
               may_fail: bool = False) -> tuple[int, list[str]]:
    code, out = run([str(exe)] + spans, env, cwd=work, may_fail=True)
    (work / f"oracle-{tag}.out").write_text(out, encoding="utf-8")
    if code != 0 and not may_fail:
        print(out[-8000:])
        raise AssertionError(f"oracle {tag} exited {code}")
    return code, out.strip().splitlines()


def run_flavours(env, cl: str, work: Path, bodies: dict[str, str],
                 spans: list[str], only: list[str] | None = None) -> list[str]:
    bodies_c = work / "kage_bind_bodies.c"
    bodies_c.write_text(bodies_source(bodies), encoding="utf-8", newline="\n")
    results = []
    for flavour, (_, _, _, expected) in FLAVOURS.items():
        if only is not None and flavour not in only:
            continue
        exe = build_flavour(env, cl, work, flavour, bodies_c, flavour)
        _, lines = run_oracle(env, exe, spans, work, flavour)
        result = lines[-1] if lines else ""
        failures = [line for line in lines if line.startswith("FAIL")]
        require(not failures, f"{flavour}: {failures[:3]}")
        require(result.startswith("Vita KAGE bind packet oracle: PASS;"),
                f"{flavour}: {result!r}")
        if expected is not None:
            require(result == expected, f"{flavour}: result changed: {result!r}")
        log = [line for line in lines
               if line.startswith("KAGE VITA BIND PACKET: binds=")]
        require(log, f"{flavour}: no seam log line")
        stage = [line for line in lines if line.startswith("stage words:")]
        require(stage == ([STAGE_RESULT] if flavour == "sampler" else []),
                f"{flavour}: stage word line {stage}")
        reasons = [line for line in lines
                   if line.startswith("declines by reason:")]
        require(reasons == [REASON_RESULT], f"{flavour}: {reasons}")
        results.append(f"[{flavour}] {result}" +
                       (f"; {stage[0]}" if stage else ""))
    for mutant, (flavour, which, old, new, reason) in MUTANTS.items():
        if only is not None and mutant not in only:
            continue
        source = SEAM_C if which == "seam" else RECORDER_C
        text = source.read_text(encoding="utf-8")
        require(text.count(old) == 1, f"mutant {mutant}: anchor count "
                                      f"{text.count(old)}")
        mutated = work / f"mutant-{mutant}-{source.name}"
        mutated.write_text(text.replace(old, new), encoding="utf-8",
                           newline="\n")
        kwargs = {"seam_c": mutated} if which == "seam" else \
            {"recorder_c": mutated}
        exe = build_flavour(env, cl, work, f"mutant-{mutant}", bodies_c,
                            flavour, **kwargs)
        code, lines = run_oracle(env, exe, spans, work, f"mutant-{mutant}",
                                 may_fail=True)
        failures = [line for line in lines if line.startswith("FAIL")]
        require(code != 0 and any(reason in line for line in failures),
                f"mutant {mutant} did not fail for '{reason}' "
                f"(exit {code}, {failures[:2]})")
        results.append(f"[must-FAIL {mutant}] exit={code} failures="
                       f"{len(failures)} first={failures[0]}")
    return results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, default=DEFAULT_PE,
                        help="frozen isaac-ng.exe.unpacked.exe (default: $REPENTOGXM_PE)")
    parser.add_argument("--generated-dir", type=Path, default=DEFAULT_GENERATED,
                        help="generated corpus (default: $ISAAC_GENERATED_DIR)")
    parser.add_argument("--keep", type=Path,
                        help="keep the oracle build directory here")
    parser.add_argument("--static-only", action="store_true",
                        help="skip the MSVC oracle builds")
    parser.add_argument("--only", help="comma list of flavours/mutants to "
                        "run (development; the full run is the proof)")
    parser.add_argument("--base", default=BASE_COMMIT,
                        help="knob-OFF identity base commit")
    arguments = parser.parse_args()

    if arguments.pe is None or arguments.generated_dir is None:
        lines = verify_build_gate()
        with tempfile.TemporaryDirectory(prefix="isaac-kage-bind-id-") as tmp:
            lines += verify_off_identity(arguments.base, Path(tmp))
        for line in lines:
            print(line)
        print("Vita KAGE bind packet: SKIP (frozen PE, corpus and oracle legs need "
              "--pe/--generated-dir or REPENTOGXM_PE/ISAAC_GENERATED_DIR)")
        return 0
    spans, lines = verify_frozen_pe(arguments.pe)
    bodies, more = extract_bodies(arguments.generated_dir)
    lines += more
    lines += verify_seam_constants(bodies)
    lines += verify_build_gate()
    with tempfile.TemporaryDirectory(prefix="isaac-kage-bind-id-") as tmp:
        lines += verify_off_identity(arguments.base, Path(tmp))
    for line in lines:
        print(line)
    if arguments.static_only:
        print("Vita KAGE bind packet: static PASS (oracle skipped)")
        return 0
    env, cl = BA.msvc_env()
    only = arguments.only.split(",") if arguments.only else None
    if arguments.keep:
        arguments.keep.mkdir(parents=True, exist_ok=True)
        results = run_flavours(env, cl, arguments.keep, bodies, spans, only)
    else:
        with tempfile.TemporaryDirectory(prefix="isaac-kage-bind-") as tmp:
            results = run_flavours(env, cl, Path(tmp), bodies, spans, only)
    for result in results:
        print(result)
    flavours = sum(1 for result in results if "must-FAIL" not in result)
    if only is not None:
        print(f"Vita KAGE bind packet: PARTIAL (--only {arguments.only})")
        return 0
    print(f"Vita KAGE bind packet: PASS; bodies={len(BODIES)}; oracle "
          f"flavours={flavours}; must-FAIL mutants={len(results) - flavours}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
