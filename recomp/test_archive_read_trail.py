#!/usr/bin/env python3
"""Frozen-PE, wiring and OFF-identity gate for ISAAC_VITA_ARCHIVE_READ_TRAIL.

Pins the guest facts the read trail relies on: the ArchivedFile constructor
seeds its raw-XOR key from [entry+8] ^ 0xf9524287, pushes SEEK_SET and the
entry's table offset [entry+0xc] and returns from File::seek at 0x59c269
(ISAAC_VITA_CRT_ARCHIVE_SEEK_PARENT_RETURN_RVA), File::seek's SEEK_SET lane
returns from the fseek import at 0x59654d, and the mount key hash pair
(djb2 seed 0x1505; FNV-1a seed 0x5bb2220e, prime 0x1000193, A-Z and
backslash fold) that a host-side ph120.rt log analyzer re-implements for
tag naming.  Then the CMake/README wiring (option default OFF, the three
FATAL_ERROR guards, the define reaching exactly three sources) and the
OFF-identity structure: every trail token in the edited translation units
sits inside an #if defined(ISAAC_VITA_ARCHIVE_READ_TRAIL) block, except the
one disjunct appended to kage_vita_phase_profile.c's room-profile include
condition and a comment in kage_vita_phase_profile.h.  Finally the pure
accumulator's header constants and its host oracle files.

usage: python test_archive_read_trail.py [--pe <isaac-ng.exe.unpacked.exe>]

--pe defaults to $REPENTOGXM_PE; with neither, the frozen-PE leg is skipped
and the wiring and OFF-identity legs still run.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys
from pathlib import Path

import capstone
import pefile


HERE = Path(__file__).resolve().parent
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
IMAGE_BASE = 0x00400000
MACRO = "ISAAC_VITA_ARCHIVE_READ_TRAIL"

SEEK_PARENT_RETURN = 0x0059C269
FSEEK_RETURN = 0x0059654D
CTOR_XOR = 0x0059C248
HASH_DJB2 = 0x005637E0
HASH_FNV = 0x00563830


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def disasm(pe: pefile.PE, md: capstone.Cs, rva: int, size: int):
    off = pe.get_offset_from_rva(rva)
    return list(md.disasm(pe.__data__[off:off + size], rva))


def text(insns) -> list[str]:
    return [f"{i.mnemonic} {i.op_str}".strip() for i in insns]


def check_pe(path: Path) -> None:
    data = path.read_bytes()
    require(len(data) == PE_SIZE, f"PE size {len(data)} != {PE_SIZE}")
    require(hashlib.sha256(data).hexdigest() == PE_SHA256, "PE SHA-256 drift")
    pe = pefile.PE(data=data)
    require(pe.OPTIONAL_HEADER.ImageBase == IMAGE_BASE, "image base drift")
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    ctor = text(disasm(pe, md, CTOR_XOR, SEEK_PARENT_RETURN - CTOR_XOR + 1))
    want = [
        "mov eax, dword ptr [eax + 8]",       # entry h2
        "xor eax, 0xf9524287",                # raw-XOR seed
        "or eax, 1",
        "mov dword ptr [esi + 0xc24], eax",
        "mov ecx, dword ptr [esi + 8]",       # File*
        "mov eax, dword ptr [esi + 0xc]",     # entry*
        "push 0",                             # SEEK_SET
        "mov edx, dword ptr [ecx]",
        "push dword ptr [eax + 0xc]",         # entry offset (the trail's o= for k=S)
        "call dword ptr [edx + 0xc]",         # File::seek
        "pop edi",
    ]
    require(ctor == want, f"ArchivedFile constructor seam drift:\n{ctor}")
    call = disasm(pe, md, 0x0059C266, 3)[0]
    require(call.address + call.size == SEEK_PARENT_RETURN, "seek parent return drift")

    lane = text(disasm(pe, md, 0x0059653F, FSEEK_RETURN - 0x0059653F + 3))
    require(lane == ["push 0", "push dword ptr [ebp + 8]", "push dword ptr [ecx + 0xc]",
                     "call dword ptr [0xa065ec]", "add esp, 0xc"],
            f"File::seek SEEK_SET lane drift:\n{lane}")
    fseek_call = disasm(pe, md, 0x00596547, 6)[0]
    require(fseek_call.address + fseek_call.size == FSEEK_RETURN, "fseek import return drift")
    require(0xA065EC - IMAGE_BASE == 0x006065EC, "fseek IAT slot drift")

    djb2 = text(disasm(pe, md, HASH_DJB2, 8))
    require(djb2[:3] == ["push esi", "mov esi, ecx", "mov eax, 0x1505"], f"djb2 seed drift: {djb2}")
    fnv = text(disasm(pe, md, HASH_FNV, 0x56386E - HASH_FNV))
    require("mov edi, 0x5bb2220e" in fnv, f"FNV seed drift: {fnv}")
    require("mov ebx, 0x2f" in fnv and "cmp cl, 0x5c" in fnv, f"backslash fold drift: {fnv}")
    require("lea ecx, [eax + 0x20]" in fnv and "cmp dl, 0x19" in fnv, f"lower-case fold drift: {fnv}")
    require("imul edi, eax, 0x1000193" in fnv, f"FNV prime drift: {fnv}")


def check_wiring() -> None:
    cmake = (HERE / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    crt_h = (HERE / "runtime" / "host_vita_crt.h").read_text(encoding="utf-8")
    require(f"#define ISAAC_VITA_CRT_ARCHIVE_SEEK_PARENT_RETURN_RVA 0x{SEEK_PARENT_RETURN:08x}U" in crt_h,
            "seek parent RVA constant drift")
    require(f"#define ISAAC_VITA_CRT_ARCHIVE_FSEEK_RETURN_RVA     0x{FSEEK_RETURN:08x}U" in crt_h,
            "fseek return RVA constant drift")
    require(re.search(rf"option\({MACRO}\n\s+\"[^\"]+\" OFF\)", cmake), "option must default OFF")
    require(re.search(r"set\(ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP 64 CACHE STRING", cmake), "cap default 64")
    for guard in (f"if({MACRO} AND NOT ISAAC_VITA_ARCHIVE_FILE_CACHE)",
                  f"if({MACRO} AND NOT ISAAC_VITA_PHASE_PROFILE)",
                  f"if({MACRO} AND\n   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))"):
        require(guard in cmake, f"missing guard {guard!r}")
    block = re.search(rf"      if\({MACRO}\)\n(.*?)\n      endif\(\)", cmake, re.S)
    require(block is not None, "wiring block missing")
    body = block.group(1)
    require('list(APPEND ISAAC_VITA_RUNTIME_SOURCES\n          "${ISAAC_RUNTIME}/host_vita_archive_trail.c")' in body,
            "accumulator source not appended under the knob")
    sources = re.findall(r'"\$\{ISAAC_RUNTIME\}/([a-z_0-9]+\.c)"', body)
    require(sorted(set(sources)) == ["host_vita_archive_trail.c", "host_vita_crt.c", "kage_vita_phase_profile.c"],
            f"define reaches unexpected sources: {sources}")
    require(f"{MACRO}=1" in body and "ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP=${ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP}U" in body,
            "defines drift")
    require(cmake.count(f"{MACRO}=1") == 1, "the define must be set in exactly one place")
    readme = (HERE / "vita" / "README.md").read_text(encoding="utf-8")
    require(f"`{MACRO}`" in readme and "ph120.rt" in readme, "README knob list")


def strip_guarded(source: str) -> str:
    out, depth, skip = [], 0, None
    for line in source.split("\n"):
        s = line.strip()
        if s.startswith("#if"):
            depth += 1
            if skip is None and re.fullmatch(rf"#if\s+defined\({MACRO}\)", s):
                skip = depth
                continue
        elif s.startswith("#endif"):
            if skip == depth:
                skip = None
                depth -= 1
                continue
            depth -= 1
        if skip is None:
            out.append(line)
    require(skip is None and depth == 0, "unbalanced preprocessor guards")
    return "\n".join(out)


def strip_comments(source: str) -> str:
    return re.sub(r"/\*.*?\*/", "", source, flags=re.S)


def check_off_identity() -> None:
    crt = (HERE / "runtime" / "host_vita_crt.c").read_text(encoding="utf-8")
    room = (HERE / "runtime" / "kage_vita_room_profile.h").read_text(encoding="utf-8")
    phase_c = (HERE / "runtime" / "kage_vita_phase_profile.c").read_text(encoding="utf-8")
    phase_h = (HERE / "runtime" / "kage_vita_phase_profile.h").read_text(encoding="utf-8")
    require(crt.count(f"#if defined({MACRO})") == 7, "expected seven guarded blocks in host_vita_crt.c")
    require(room.count(f"#if defined({MACRO})") == 2, "expected two guarded blocks in kage_vita_room_profile.h")
    for name, src in (("host_vita_crt.c", crt), ("kage_vita_room_profile.h", room)):
        stripped = strip_guarded(src)
        require(MACRO not in stripped, f"{name}: macro outside its guards")
        require(not re.search(r"archive_trail|\btrail\b", stripped, re.I), f"{name}: trail code outside its guards")
    require(phase_c.count(MACRO) == 1 and f" || defined({MACRO})\n# include \"kage_vita_room_profile.h\"" in phase_c,
            "kage_vita_phase_profile.c: only the include-condition disjunct may mention the macro")
    require(MACRO not in strip_comments(phase_h), "kage_vita_phase_profile.h: macro must stay comment-only")
    # The accumulator itself is compiled only under the knob and is the only new TU.
    for f in ("host_vita_archive_trail.h", "host_vita_archive_trail.c", "host_vita_archive_trail_oracle.c"):
        require((HERE / "runtime" / f).is_file(), f"missing {f}")
    require((HERE / "vita" / "test_archive_trail.sh").is_file(), "missing oracle runner")
    header = (HERE / "runtime" / "host_vita_archive_trail.h").read_text(encoding="utf-8")
    require("#define ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP 64U" in header, "header cap default")
    require("#define ISAAC_VITA_ARCHIVE_TRAIL_KEY_CAPACITY 64U" in header, "key capacity must mirror the cache")
    cache = (HERE / "runtime" / "host_vita_archive_cache.h").read_text(encoding="utf-8")
    require("#define ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY 64U" in cache, "cache key capacity drift")
    require('"[kage-vita] ph120.rt bid=%.32s win=%u loops=%u "\n                    "a=%.63s o=%08x n=%u c=%u k=%c%c at=%08x%08x\\n"' in room,
            "ph120.rt record shape drift (the ph120.rt analyzer parses it)")


def main() -> int:
    parser = argparse.ArgumentParser()
    env_pe = os.environ.get("REPENTOGXM_PE")
    parser.add_argument("--pe", type=Path, default=Path(env_pe) if env_pe else None,
                        help="frozen isaac-ng.exe.unpacked.exe (default: $REPENTOGXM_PE)")
    args = parser.parse_args()
    skipped = ""
    if args.pe is None:
        print("frozen-PE leg: SKIP (no --pe and no REPENTOGXM_PE)")
        skipped = " (frozen-PE leg skipped)"
    elif not args.pe.is_file():
        print(f"frozen PE not found: {args.pe}", file=sys.stderr)
        return 2
    else:
        check_pe(args.pe)
    check_wiring()
    check_off_identity()
    print(f"test_archive_read_trail: PASS{skipped}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
