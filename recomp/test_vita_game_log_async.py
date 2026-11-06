#!/usr/bin/env python3
"""Frozen-PE, wiring and OFF-identity gate for ISAAC_VITA_GAME_LOG_ASYNC.

Pins the guest facts the game log write-behind lane relies on (the frozen
logger sub_0055e330, its flush gate, the fflush adapter 0x005965c0 whose
return words 0x005965c9 / 0x0055e4fa the CRT reads from the guest stack, the
two fflush import sites, the fwrite adapter call, and the fopen import-site
census: every `call [fopen]` of the PE decoded to the mode literal it pushes,
so that every write/append open -- log.txt's among them -- uses a mode the
lane adopts and none passes its mode in a register), the CMake / build wrapper
/ allocator-gate wiring (option default OFF, the three FATAL_ERROR guards,
the define reaching exactly seven sources, no link option), the CRT seams
(both severity readers use the same stack shape; the GAME_LOG_BATCH markers
are intact), the module's constants and receipt formats, the docs, and the
OFF-identity structure: every lane token in the six edited translation units
sits inside an `#if defined(ISAAC_VITA_GAME_LOG_ASYNC)` block and the
stripped remainder hashes to the base-commit blob; the two shared headers are
byte-identical; a preprocessed host_vita_log_async.c without the knob has no
lane token.

usage: python test_vita_game_log_async.py [--pe <isaac-ng.exe.unpacked.exe>]
       [--root <repo>] [--generated-dir <dir>] [--cc <compiler>]
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

MACRO = "ISAAC_VITA_GAME_LOG_ASYNC"
# Changes that added their own `#if defined()` blocks in the six edited
# translation units after this lane's base commit (integration trees carry
# them together).  Only the base-blob comparison strips these blocks as well;
# every token check runs on the text with the game log blocks alone removed.
FOREIGN_LANE_MACROS: tuple[str, ...] = (
    "ISAAC_VITA_RUNTIME_SETTINGS",
    "ISAAC_VITA_GUEST_STACK_MEMBLOCK",
    "ISAAC_VITA_TROPHIES",
)
# Blocks another lane fenced under a macro the base commit already used (the
# guest stack memblock lane stops the sampler and the Lua beat in the entry
# tail).  Stripping by macro would also drop the base commit's own blocks, so
# these are removed by pattern, each exactly once, before the blob comparison.
FOREIGN_LANE_BLOCKS: dict[str, tuple[str, ...]] = {
    "entry_vita.c": (
        r"#if defined\(ISAAC_VITA_GUEST_SAMPLER\)\n"
        r"(?:    /\*(?:[^*]|\*(?!/))*\*/\n)?"
        r"    kage_vita_guest_sampler_stop\(\);\n#endif\n",
        r"#if ISAAC_VITA_KAGE && defined\(ISAAC_VITA_LUA\) && "
        r"defined\(ISAAC_VITA_LUA_RECEIPT\)\n"
        r"    isaac_vita_lua_beat_stop\(\);\n#endif\n",
    ),
}
IMAGE_BASE = 0x00400000
FFLUSH_FIRST_RETURN_RVA = 0x005965C9
# The logger sub_0055e330 reaches both adapters through the sink object's
# vtable (`mov eax, [ecx]; call [eax+0x1c]` = fwrite adapter, `call [eax+0x20]`
# = fflush adapter), so the origin call sites are 3-byte indirect calls and
# their return words follow 3 bytes later; the vtable itself sits in .rdata.
FFLUSH_FIRST_ORIGIN_CALL_RVA = 0x0055E4F7
FFLUSH_FIRST_ORIGIN_RETURN_RVA = 0x0055E4FA
FFLUSH_ORIGIN_CALL = bytes([0xFF, 0x50, 0x20])
FWRITE_ADAPTER_RVA = 0x00596590
FWRITE_ADAPTER_SIZE = 0x2C
FWRITE_ADAPTER_SHA256 = "77a6cca2922d3dea6bfb4a59e83e4537431ad4a0228d58454bdf12b8c8a89180"
FWRITE_CALL_RVA = 0x005965AB
FWRITE_RETURN_RVA = 0x005965B1
FWRITE_IAT_RVA = 0x006065E8
FWRITE_FIRST_ORIGIN_CALL_RVA = 0x0055E4E3
FWRITE_FIRST_ORIGIN_RETURN_RVA = 0x0055E4E6
FWRITE_ORIGIN_CALL = bytes([0xFF, 0x50, 0x1C])
SINK_VTABLE_RVA = 0x00764894
SINK_VTABLE_FWRITE_SLOT = 0x1C
SINK_VTABLE_FFLUSH_SLOT = 0x20
# The fopen import (recomp/runtime/host_vita_crt.h ISAAC_VITA_CRT_FOPEN_*):
# `call [IAT]` bytes, the two call sites whose mode literals are pinned with
# their relocated (Vita) addresses, and the lane's whitelist
# (host_vita_game_log.c isaac_vita_game_log_eligible).
FOPEN_IAT_RVA = 0x00606608
FOPEN_CALL = bytes([0xFF, 0x15]) + (IMAGE_BASE + FOPEN_IAT_RVA).to_bytes(4, "little")
FOPEN_SAVEPATH_CALL_RVA = 0x0050AFFE      # fopen("savedatapath.txt", "w")
FOPEN_KAGE_CALL_RVA = 0x00596384          # fopen("kage_mount_points.dat", "rb")
FOPEN_WRITE_MODE_VA = 0x9875AAAC          # "w", relocated
FOPEN_READ_MODE_VA = 0x9874E754           # "rb", relocated
LANE_MODES = ("w", "wb", "a", "ab")
# vita_crt_fopen_mode_valid: [rwa], then 'b' and '+' at most once each in
# either order, at most three characters.
MODE_LITERAL = re.compile(rb"^[rwa](?:b\+?|\+b?)?$")
FOPEN_PUSH_WINDOW = 32

# sha256 of the edited translation units with every `#if defined(MACRO)`
# block removed: the base commit d0c7f66 blobs.
STRIPPED_BASELINE = {
    "host_vita_crt.c":
        "91990dc46f620cb4a34920e64cfcd095c8bec49c7d87a6c6ffdee442499aa36e",
    "host_vita_filesystem.c":
        "19fab46a2983ae7bbdf5c090b9282c4bfb68a46782353d3babe394b24a495c15",
    "host_vita_startup.c":
        "430ac48e7ea259f624895c077f192918241e2905bccdbfd3f40dc54062f6da3c",
    "host_vita_find.c":
        "7fe72b5b096549ffa6651136c952d4a27c55229ed5a9cbb1454ab51fe8ae9405",
    "entry_vita.c":
        "f6447640dacb3982f66968c2d90d691efa7d3cdc25aeb59aad1e89829f81a1a9",
    "host_vita_log_async.c":
        "ffe6fc7364af9f848d1f4e40104381ca8d7bb5e887f3e4631e3678d787206242",
}
# Number of `#if defined(MACRO)` blocks per edited unit (frozen shape).
GUARDED_BLOCKS = {
    "host_vita_crt.c": 27,
    "host_vita_filesystem.c": 3,
    "host_vita_startup.c": 2,
    "host_vita_find.c": 2,
    "entry_vita.c": 2,
    "host_vita_log_async.c": 2,
}
UNTOUCHED_HEADERS = {
    "host_vita_crt.h":
        "e0f7ab986e108cb729da1d9922845cd1da4f51735c8a761c8044934a213a9ccd",
    "host_vita_log_async.h":
        "82de7df5aee7def13204dd0855581754027625e0b57f48d183081eecb6f0e333",
}
OWNERS = (
    "host_vita_crt.c", "host_vita_filesystem.c", "host_vita_startup.c",
    "host_vita_find.c", "entry_vita.c", "host_vita_log_async.c",
    "host_vita_game_log.c",
)
RECEIPTS = (
    '"[isaac-gamelog] open bid40=%.40s mode=%s size=%u ring_kb=%u "\n'
    '        "durable=%u worker=%u prio=0x%08x affinity=0x%08x rc=0x%08x"',
    '"[isaac-gamelog] refuse bid40=%.40s mode=%s reason=%s"',
    '"[isaac-gamelog] append bid40=%.40s who=%s bytes=%u "\n'
    '                "rc=0x%08x retry=%u"',
    '"[isaac-gamelog] wait bid40=%.40s state=abandoned span=%u "\n'
    '        "pending=%u wait_us=%u"',
    '"[isaac-gamelog] summary bid40=%.40s state=%s records=%u bytes=%u "\n'
    '        "flush(info,warn,error,assert,other,null)=%u,%u,%u,%u,%u,%u "\n'
    '        "appends(worker,guest,exit,abort,direct)=%u,%u,%u,%u,%u "\n'
    '        "io_us(worker,worker_max,guest,guest_max)=%u,%u,%u,%u "\n'
    '        "wait(n,us,max)=%u,%u,%u ring(high,full_waits)=%u,%u "\n'
    '        "coherence(open,remove,access,attrib,find,seek,tell)='
    '%u,%u,%u,%u,%u,%u,%u "\n'
    '        "demote=%u late=%u lost_bytes=%u abandoned_bytes=%u fail=%u"',
)


def sha256(data: bytes | bytearray) -> str:
    return hashlib.sha256(data).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def run(command: list[str]) -> str:
    result = subprocess.run(
        command, check=False, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}): {command!r}\n{result.stdout}"
        )
    return result.stdout


def compiler(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    for candidate in (os.environ.get("CC"), "clang", "gcc", "cc"):
        if candidate and shutil.which(candidate):
            return candidate
    if os.name == "nt":
        installed = Path(os.environ.get(
            "ProgramFiles", r"C:\Program Files")) / "LLVM" / "bin" / "clang.exe"
        if installed.is_file():
            return str(installed)
    return None


# --------------------------------------------------------------------------
# 1. frozen PE
# --------------------------------------------------------------------------
def decode_fopen_sites(payload: bytes, call: bytes, rva_of_offset,
                       cstring_at_va, window: int = FOPEN_PUSH_WINDOW) -> list[dict]:
    """Every `call [fopen]` in payload with the mode literal pushed before it.

    fopen is cdecl: the mode is pushed first, then the path, so the mode is
    the nearest `push imm32` (0x68) before the call whose immediate names a
    NUL-terminated fopen mode literal inside the image (a path literal never
    matches).  The backward scan stops at the previous fopen call so adjacent
    sites never share a push.  A site without such a push takes its mode from
    a register or a stack slot: mode None (unresolved).
    """
    sites: list[dict] = []
    previous_end = 0
    start = 0
    while True:
        offset = payload.find(call, start)
        if offset < 0:
            break
        start = offset + len(call)
        lowest = max(previous_end, offset - window)
        mode = None
        immediate = None
        for probe in range(offset - 5, lowest - 1, -1):
            if payload[probe] != 0x68:
                continue
            candidate = int.from_bytes(payload[probe + 1:probe + 5], "little")
            text = cstring_at_va(candidate)
            if text is None or MODE_LITERAL.match(text) is None:
                continue
            mode, immediate = text.decode("ascii"), candidate
            break
        sites.append({"rva": rva_of_offset(offset), "mode": mode,
                      "mode_va": immediate})
        previous_end = start
    return sites


def check_fopen_census(sites: list[dict]) -> str:
    """Assert the census proves the lane's whitelist covers log.txt's open.

    The two pinned sites must carry their pinned literals and agree on one
    image relocation; every write/append site must use a lane mode (a "w+"
    or "a+" logger would leave log.txt on the synchronous newlib FILE with
    every host gate green); no site may take its mode from a register, or
    the coverage is unproven.  Returns the printable census.
    """
    by_rva = {site["rva"]: site for site in sites}
    require(len(by_rva) == len(sites), "duplicate fopen site RVAs")
    savepath = by_rva.get(FOPEN_SAVEPATH_CALL_RVA)
    kage = by_rva.get(FOPEN_KAGE_CALL_RVA)
    require(savepath is not None and savepath["mode"] == "w",
            "the savedatapath.txt fopen site (0x0050affe) no longer pushes \"w\"")
    require(kage is not None and kage["mode"] == "rb",
            "the kage_mount_points.dat fopen site (0x00596384) no longer pushes \"rb\"")
    assert savepath is not None and kage is not None
    delta = (FOPEN_WRITE_MODE_VA - savepath["mode_va"]) & 0xFFFFFFFF
    require(((kage["mode_va"] + delta) & 0xFFFFFFFF) == FOPEN_READ_MODE_VA,
            "the two pinned mode literals disagree on the image relocation")
    lines = []
    for site in sites:
        if site["mode"] is None:
            lines.append(f"  {site['rva']:08x}: mode in a register (unresolved)")
        else:
            relocated = (site["mode_va"] + delta) & 0xFFFFFFFF
            lines.append(f"  {site['rva']:08x}: \"{site['mode']}\" "
                         f"(literal {site['mode_va']:08x} -> {relocated:08x})")
    report = "\n".join(lines)
    outside = [s for s in sites
               if s["mode"] is not None and s["mode"][0] in "wa" and
               s["mode"] not in LANE_MODES]
    require(not outside,
            "a write/append fopen site uses a mode the lane refuses, so "
            "log.txt may stay on the synchronous newlib FILE: " +
            ", ".join(f"{s['rva']:08x}=\"{s['mode']}\"" for s in outside) +
            "\n" + report)
    unresolved = [s for s in sites if s["mode"] is None]
    require(not unresolved,
            "fopen site(s) take the mode from a register; the lane's coverage "
            "of log.txt is unproven -- resolve them and pin the modes: " +
            ", ".join(f"{s['rva']:08x}" for s in unresolved) + "\n" + report)
    return report


def expect_failure(action, needle: str) -> None:
    try:
        action()
    except AssertionError as error:
        require(needle in str(error), f"unexpected failure text: {error}")
        return
    raise AssertionError(f"expected a failure mentioning {needle!r}")


def self_test_fopen_census() -> None:
    """The decoder and checker on a synthetic image (runs without a PE)."""
    image = bytearray(0x400)
    literals: dict[bytes, int] = {}
    cursor = 0x300
    for literal in (b"w", b"rb", b"w+", b"wb", b"savedatapath.txt", b"ab"):
        literals[literal] = cursor
        image[cursor:cursor + len(literal) + 1] = literal + b"\0"
        cursor += len(literal) + 1

    def push(literal: bytes) -> bytes:
        return b"\x68" + (IMAGE_BASE + literals[literal]).to_bytes(4, "little")

    code = bytearray()
    sites = []

    def site(prefix: bytes) -> None:
        code.extend(prefix)
        sites.append(0x100 + len(code))
        code.extend(FOPEN_CALL)

    site(push(b"w") + b"\x50")                       # push "w"; push eax
    code.extend(b"\x85\xc0")                          # test eax, eax
    site(push(b"rb") + push(b"savedatapath.txt"))     # path literal ignored
    site(push(b"w+") + b"\xff\x75\x08")               # push "w+"; push [ebp+8]
    site(b"\xff\x75\x0c\xff\x75\x08")                 # both from the stack
    site(push(b"wb") + b"\x68\x78\x56\x34\x12")        # junk imm32 after the mode
    site(push(b"ab") + b"\x51")                      # push "ab"; push ecx
    image[0x100:0x100 + len(code)] = code
    payload = bytes(image)

    def cstring_at_va(va: int) -> bytes | None:
        rva = va - IMAGE_BASE
        if rva < 0 or rva >= len(payload):
            return None
        end = payload.find(b"\0", rva, rva + 4)
        return None if end < 0 else payload[rva:end]

    decoded = decode_fopen_sites(payload, FOPEN_CALL, lambda o: o, cstring_at_va)
    require([s["rva"] for s in decoded] == sites, "census decoder lost a site")
    require([s["mode"] for s in decoded] == ["w", "rb", "w+", None, "wb", "ab"],
            f"census decoder modes: {[s['mode'] for s in decoded]}")
    require(decoded[0]["mode_va"] == IMAGE_BASE + literals[b"w"],
            "census decoder mode literal address")

    delta = 0x98000000 - IMAGE_BASE
    good = [
        {"rva": FOPEN_SAVEPATH_CALL_RVA, "mode": "w",
         "mode_va": (FOPEN_WRITE_MODE_VA - delta) & 0xFFFFFFFF},
        {"rva": FOPEN_KAGE_CALL_RVA, "mode": "rb",
         "mode_va": (FOPEN_READ_MODE_VA - delta) & 0xFFFFFFFF},
        {"rva": 0x1000, "mode": "wb", "mode_va": 0x00500000},
        {"rva": 0x1100, "mode": "r", "mode_va": 0x00500004},
    ]
    report = check_fopen_census(good)
    require(report.count("\n") == 3 and "unresolved" not in report,
            "census report shape")
    expect_failure(lambda: check_fopen_census(
        good + [{"rva": 0x2000, "mode": "w+", "mode_va": 0x00500010}]),
        "lane refuses")
    expect_failure(lambda: check_fopen_census(
        good + [{"rva": 0x2000, "mode": "a+", "mode_va": 0x00500010}]),
        "lane refuses")
    expect_failure(lambda: check_fopen_census(
        good + [{"rva": 0x3000, "mode": None, "mode_va": None}]),
        "register")
    skewed = [dict(s) for s in good]
    skewed[1]["mode_va"] += 4
    expect_failure(lambda: check_fopen_census(skewed), "relocation")
    expect_failure(lambda: check_fopen_census(good[1:]), "savedatapath")
    print("fopen census decoder self-test: OK")


def verify_pe(pe_path: Path) -> None:
    # pefile and the sibling batch pins are needed only with --pe, so the
    # wiring/OFF-identity half runs on a host without pefile.
    import pefile
    from test_vita_game_log_batch import (
        ADAPTER, ADAPTER_RVA, ADAPTER_SHA256, FFLUSH_CALL, FFLUSH_SITES,
        GATE_RVA, GATE_SHA256, GATE_SIZE, LOGGER_RVA, LOGGER_SHA256,
        LOGGER_SIZE, PE_SHA256, PE_SIZE,
    )

    payload = pe_path.read_bytes()
    require(len(payload) == PE_SIZE, "frozen PE size changed")
    require(sha256(payload) == PE_SHA256, "frozen PE identity changed")
    pe = pefile.PE(data=payload, fast_load=True)
    require(getattr(pe.OPTIONAL_HEADER, "ImageBase", None) == IMAGE_BASE,
            "frozen PE image base changed")
    logger = pe.get_data(LOGGER_RVA, LOGGER_SIZE)
    gate = pe.get_data(GATE_RVA, GATE_SIZE)
    adapter = pe.get_data(ADAPTER_RVA, len(ADAPTER))
    require(len(logger) == LOGGER_SIZE and sha256(logger) == LOGGER_SHA256,
            "logger body changed")
    require(len(gate) == GATE_SIZE and sha256(gate) == GATE_SHA256,
            "logger flush gate changed")
    require(adapter == ADAPTER and sha256(adapter) == ADAPTER_SHA256,
            "fflush adapter changed")
    # The return words the CRT compares on the guest stack: the adapter's
    # `call [fflush]` returns to 0x005965c9, the logger's `call adapter`
    # returns to 0x0055e4fa (inside the pinned logger body).
    require(ADAPTER_RVA + 3 == FFLUSH_FIRST_RETURN_RVA - 6 and
            adapter[3:9] == FFLUSH_CALL,
            "fflush adapter call/return layout changed")
    # The logger reaches both adapters through the sink object's vtable: the
    # origin call sites are 3-byte `call [eax+slot]` instructions and the
    # return words the CRT compares at ESP+8 follow them immediately.
    require(FFLUSH_FIRST_ORIGIN_CALL_RVA + 3 == FFLUSH_FIRST_ORIGIN_RETURN_RVA and
            pe.get_data(FFLUSH_FIRST_ORIGIN_CALL_RVA, 3) == FFLUSH_ORIGIN_CALL,
            "logger -> fflush adapter call changed")
    require(FWRITE_FIRST_ORIGIN_CALL_RVA + 3 == FWRITE_FIRST_ORIGIN_RETURN_RVA and
            pe.get_data(FWRITE_FIRST_ORIGIN_CALL_RVA, 3) == FWRITE_ORIGIN_CALL,
            "logger -> fwrite adapter call changed")
    require(LOGGER_RVA <= FWRITE_FIRST_ORIGIN_CALL_RVA < FFLUSH_FIRST_ORIGIN_CALL_RVA
            < LOGGER_RVA + LOGGER_SIZE,
            "origin calls left the pinned logger body")
    # The vtable slots those calls read hold the adapters' absolute addresses,
    # and each adapter address occurs exactly once in the image: no other data
    # word can route the logger's record to a different fwrite/fflush.
    fwrite_va = (IMAGE_BASE + FWRITE_ADAPTER_RVA).to_bytes(4, "little")
    fflush_va = (IMAGE_BASE + ADAPTER_RVA).to_bytes(4, "little")
    require(pe.get_data(SINK_VTABLE_RVA + SINK_VTABLE_FWRITE_SLOT, 4) == fwrite_va and
            pe.get_data(SINK_VTABLE_RVA + SINK_VTABLE_FFLUSH_SLOT, 4) == fflush_va,
            "logger sink vtable slots changed")
    require(payload.count(fwrite_va) == 1 and payload.count(fflush_va) == 1,
            "adapter address census changed")
    # The fwrite adapter: `call [fwrite]` at 0x005965ab returns to 0x005965b1
    # (the return word the CRT's fwrite seam sees) and it is the PE's only
    # fwrite import call.
    fwrite_adapter = pe.get_data(FWRITE_ADAPTER_RVA, FWRITE_ADAPTER_SIZE)
    require(len(fwrite_adapter) == FWRITE_ADAPTER_SIZE and
            sha256(fwrite_adapter) == FWRITE_ADAPTER_SHA256,
            "fwrite adapter changed")
    fwrite_call = bytes([0xFF, 0x15]) + (IMAGE_BASE + FWRITE_IAT_RVA).to_bytes(4, "little")
    require(FWRITE_CALL_RVA + 6 == FWRITE_RETURN_RVA and
            pe.get_data(FWRITE_CALL_RVA, 6) == fwrite_call,
            "fwrite adapter call/return layout changed")
    require(payload.count(fwrite_call) == 1, "fwrite import-call census changed")
    require(payload.count(FFLUSH_CALL) == len(FFLUSH_SITES),
            "fflush import-call census changed")
    for rva in FFLUSH_SITES:
        require(pe.get_data(rva, len(FFLUSH_CALL)) == FFLUSH_CALL,
                f"fflush callsite changed at {rva:08x}")
    require(b"log.txt" in payload, "the PE lost its log.txt literal")
    # The fopen import-site census: the lane adopts log.txt only for the
    # modes w/wb/a/ab, so every write/append `call [fopen]` of the PE must
    # push one of them and none may pass its mode in a register; otherwise
    # the knob could be silently inert on device with every host gate green.
    require(payload.count(FOPEN_CALL) >= 2,
            "fopen import-call census lost the pinned sites")
    size_of_image = int(getattr(pe.OPTIONAL_HEADER, "SizeOfImage", 0))

    def rva_of_offset(offset: int) -> int:
        rva = pe.get_rva_from_offset(offset)
        require(rva is not None,
                f"fopen call at file offset {offset:#x} lies outside every section")
        assert rva is not None
        return int(rva)

    def cstring_at_va(va: int) -> bytes | None:
        rva = va - IMAGE_BASE
        if rva <= 0 or rva >= size_of_image:
            return None
        try:
            data = pe.get_data(rva, 4)
        except pefile.PEFormatError:
            return None
        end = data.find(b"\0")
        return None if end < 0 else bytes(data[:end])

    sites = decode_fopen_sites(payload, FOPEN_CALL, rva_of_offset, cstring_at_va)
    report = check_fopen_census(sites)
    print(f"PE pins: {len(sites)} fopen import sites, every write/append mode "
          f"within {LANE_MODES}:\n{report}")
    print("PE pins: OK")


# --------------------------------------------------------------------------
# 2. wiring pins
# --------------------------------------------------------------------------
def verify_wiring(root: Path) -> None:
    cmake = read(root / "recomp/vita/CMakeLists.txt")
    wrapper = read(root / "tools/build_vita.py")
    gate = read(root / "recomp/vita/vita_raw_allocator_gate.py")
    release_cache = read(root / "release/v0.1.1-alpha.cmake")

    require(cmake.count(f"option({MACRO}\n") == 1, "CMake option is not unique")
    require(re.search(
        rf"option\({MACRO}\n\s+\"[^\"]+\" OFF\)", cmake) is not None,
        "CMake option is not default OFF")
    require(f"set({MACRO}_RING_KB 64 CACHE STRING" in cmake,
            "ring knob default changed")
    require(f"set({MACRO}_DURABLE_SEVERITY 1 CACHE STRING" in cmake,
            "durable severity default changed")
    for guard in (
        f"{MACRO} requires ISAAC_VITA_ARCHIVE_FILE_CACHE=ON",
        f"{MACRO} requires ISAAC_VITA_LOG_ASYNC=ON",
        f"{MACRO} requires the real Vita runtime",
        f"{MACRO}_RING_KB must be an integer in 32..1024",
        f"{MACRO}_DURABLE_SEVERITY must be 0..4",
    ):
        require(guard in cmake, f"CMake guard missing: {guard}")
    require(cmake.count(f"{MACRO}=1") == 1,
            "the define must be set in exactly one place")
    require(cmake.count("host_vita_game_log.c") == 3,
            "host_vita_game_log.c must appear exactly three times")
    block = re.search(
        rf"^  if\({MACRO}\)\n(.*?)^  endif\(\)\n", cmake, re.S | re.M)
    require(block is not None, "CMake feature block not found")
    assert block is not None
    body = block.group(1)
    owners = "".join(
        f'      "${{ISAAC_RUNTIME}}/{owner}"\n' for owner in OWNERS)
    require(
        "    set_property(SOURCE\n" + owners +
        f"      APPEND PROPERTY COMPILE_DEFINITIONS\n        {MACRO}=1)\n" in body,
        "the define does not reach exactly the seven owners in order")
    require(f"{MACRO}_RING_KB=${{{MACRO}_RING_KB}}U" in body and
            f"{MACRO}_DURABLE_SEVERITY=${{{MACRO}_DURABLE_SEVERITY}}U" in body and
            'ISAAC_VITA_GAME_LOG_BUILD_ID=\\"${ISAAC_VITA_GUEST_LINK_ID}\\"' in body,
            "module knob definitions drifted")
    require("--wrap" not in body and "target_link_options" not in body,
            "the feature block must not add link options")
    require(re.search(r"ISAAC_GENERATED|gl_vita_backend|kage_vita|guest\.c",
                      body) is None,
            "the define leaked beyond the CRT/path/exit/abort owners")
    require('message(STATUS\n      "Isaac Vita game log write-behind: '
            'disabled (log.txt is a "' in cmake,
            "OFF status message missing")

    require(f'f"-D{MACRO}={{on_off(game_log_async)}}"' in wrapper,
            "build wrapper does not pin the flag")
    require(f'"{MACRO}": game_log_async,' in wrapper,
            "build wrapper does not map the cache key")
    require('"--game-log-async",' in wrapper, "build wrapper lost the CLI flag")
    require(wrapper.count("game_log_async=False,") >= 7,
            "a build profile enables the knob by default")
    require("assert not defaults.game_log_async" in wrapper,
            "build wrapper self-test lost the default-OFF assert")

    require(f'GAME_LOG_ASYNC_CACHE_KEY = "{MACRO}"' in gate,
            "allocator gate lost the cache key")
    require(f'("{MACRO}", "host_vita_game_log.c")' in gate,
            "allocator gate does not know the optional source")
    require("def verify_game_log_async_compile_scope(" in gate and
            "verify_game_log_async_compile_scope(records, cache)" in gate,
            "allocator gate does not verify the owner scope")
    owners_literal = "".join(f'        "{owner}",\n' for owner in OWNERS)
    require("GAME_LOG_ASYNC_DEFINITION_OWNERS: frozenset[str] = frozenset(\n"
            "    (\n" + owners_literal + "    )\n)" in gate,
            "allocator gate owner set drifted")
    require(f"{MACRO} requires ISAAC_VITA_ARCHIVE_FILE_CACHE=ON" in gate.replace(
                f"{{GAME_LOG_ASYNC_CACHE_KEY}}", MACRO) and
            f"{MACRO} requires ISAAC_VITA_LOG_ASYNC=ON" in gate.replace(
                f"{{GAME_LOG_ASYNC_CACHE_KEY}}", MACRO),
            "allocator gate does not mirror the CMake dependencies")
    require(MACRO not in release_cache,
            "the release cache must not mention the parked knob")
    print("wiring pins: OK")


# --------------------------------------------------------------------------
# 3. CRT seams, module and docs
# --------------------------------------------------------------------------
def verify_sources(root: Path) -> None:
    runtime = root / "recomp/runtime"
    crt = read(runtime / "host_vita_crt.c")
    header = read(runtime / "host_vita_game_log.h")
    module = read(runtime / "host_vita_game_log.c")

    # The runtime's frozen-PE constants are the ones this test pins in the
    # image (so a drift in either place fails here, with or without --pe).
    crt_header = read(runtime / "host_vita_crt.h")
    defines = dict(re.findall(
        r"^#define (ISAAC_VITA_CRT_F(?:WRITE|FLUSH)_\w+RVA)\s+(0x[0-9A-Fa-f]{8})U\s*$",
        crt_header, re.MULTILINE))
    for name, value in (
        ("ISAAC_VITA_CRT_FWRITE_IAT_RVA", FWRITE_IAT_RVA),
        ("ISAAC_VITA_CRT_FWRITE_ADAPTER_RVA", FWRITE_ADAPTER_RVA),
        ("ISAAC_VITA_CRT_FWRITE_CALL_RVA", FWRITE_CALL_RVA),
        ("ISAAC_VITA_CRT_FWRITE_RETURN_RVA", FWRITE_RETURN_RVA),
        ("ISAAC_VITA_CRT_FWRITE_FIRST_ORIGIN_CALL_RVA", FWRITE_FIRST_ORIGIN_CALL_RVA),
        ("ISAAC_VITA_CRT_FWRITE_FIRST_ORIGIN_RETURN_RVA", FWRITE_FIRST_ORIGIN_RETURN_RVA),
        ("ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA", FFLUSH_FIRST_RETURN_RVA),
        ("ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_CALL_RVA", FFLUSH_FIRST_ORIGIN_CALL_RVA),
        ("ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA", FFLUSH_FIRST_ORIGIN_RETURN_RVA),
    ):
        require(name in defines and int(defines[name], 16) == value,
                f"host_vita_crt.h {name} disagrees with the PE pin")

    # Both severity readers (the batch classifier and the lane) read the same
    # frozen stack shape; each RVA constant is used exactly twice.
    require(crt.count("ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA") == 2 and
            crt.count("ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA") == 2,
            "fflush return-RVA constants are not used by exactly two readers")
    require(crt.count("guest_stack_contains(c, c->esp, 12U)") == 2 and
            crt.count("ld32(c->esp + 8U)") == 2,
            "the two severity readers disagree on the stack shape")
    require(crt.count("ld32(severity_address) & 3U") == 1 and
            crt.count("ld32(c->ebp + 8U) & 3U") == 1,
            "severity masking changed")
    for marker in (
        "game_log.live",
        "isaac_vita_game_log_flush_null()",
        "vita_crt_game_log_demote_locked",
        "isaac_vita_game_log_owns_path(native_path)",
        "vita_crt_game_log_severity_locked(c)",
        "isaac_vita_game_log_flush_exit()",
        "ISAAC_VITA_CRT_GAME_LOG_FD_BASE +",
        "vita_crt_game_log_native_fopen(native_path, write_only ? \"r+b\" : \"ab\")",
        "s_file_tokens[index].game_log.demoted_write_only",
    ):
        require(marker in crt, f"CRT lane seam lost marker: {marker}")
    # Every guest formatter output starts with the lane flag cleared: the
    # string formatters (vsprintf, vsprintf_s) and both vfprintf passes.
    # int2 booted into "Failed to obtain writable save data directory!"
    # when the two string formatters left the flag as stack garbage.
    require(crt.count("vita_crt_format_output out;") == 3,
            "guest formatter output declarations changed")
    require(crt.count("out.game_log = 0;") == 3 and
            crt.count("out.game_log = 0;") == crt.count("out.async_image = NULL;"),
            "a guest formatter output leaves out.game_log unset")
    for site in ("vita_crt_stdio_common_vsprintf(CPU", "vita_crt_stdio_common_vsprintf_s(CPU"):
        body = crt[crt.index(site):]
        body = body[:body.index("vita_crt_stdio_format(c, format, arguments, &out, 1)")]
        require("out.game_log = 0;" in body, f"{site}: out.game_log is not cleared before formatting")
    for marker in (
        "guest_stack_contains(c, severity_address, 4U)",
        "s_log_batch.sticky_fail_open = 1U",
        "VITA_CRT_LOG_BATCH_DEFER",
        "vita_crt_log_batch_classify_locked(c, token, &exact_logger)",
        "vita_crt_log_batch_tracks_flush_locked(token, exact_logger)",
        "vita_crt_log_batch_native_result_locked(tracked, result)",
    ):
        require(marker in crt, f"GAME_LOG_BATCH marker lost: {marker}")
    crt_header = read(runtime / "host_vita_crt.h")
    require(crt_header.count("#define ISAAC_VITA_CRT_LOG_BATCH_SIZE      8U") == 1,
            "bounded batch size is not frozen at eight")
    # The lane flush is answered before the batch classifier runs.
    lane_flush = crt.index("s_file_tokens[index].game_log.live) {\n        errno = 0;\n"
                           "        result = isaac_vita_game_log_flush(")
    classify = crt.index("vita_crt_log_batch_classify_locked(c, token, &exact_logger) ==")
    require(lane_flush < classify, "the lane flush must precede the batch classifier")

    # Path owners, entry tail, abort hook.
    fs = read(runtime / "host_vita_filesystem.c")
    require(fs.count("isaac_vita_game_log_sync_path(native_path, "
                     "ISAAC_VITA_GAME_LOG_SYNC_REMOVE);") == 1 and
            fs.count("isaac_vita_game_log_sync_path(native_path, "
                     "ISAAC_VITA_GAME_LOG_SYNC_ACCESS);") == 1,
            "remove/_access coherence drains missing")
    require(read(runtime / "host_vita_startup.c").count(
                "isaac_vita_game_log_sync_path(native_path, "
                "ISAAC_VITA_GAME_LOG_SYNC_ATTRIB);") == 1,
            "GetFileAttributesA coherence drain missing")
    require(read(runtime / "host_vita_find.c").count(
                "isaac_vita_game_log_sync_directory(directory);") == 1,
            "FindFirstFileW coherence drain missing")
    entry = read(runtime / "entry_vita.c")
    require(re.search(rf"^done:\n#if defined\({MACRO}\)\n(?:.*\n)*?"
                      r"    isaac_vita_game_log_shutdown\(\);\n#endif\n",
                      entry, re.M) is not None,
            "the entry tail does not drain the lane first")
    log_async = read(runtime / "host_vita_log_async.c")
    abort_hook = re.search(
        r"void __wrap_abort\(void\)\n\{\n(.*?)\n\}", log_async, re.S)
    require(abort_hook is not None, "__wrap_abort not found")
    assert abort_hook is not None
    hook_body = abort_hook.group(1)
    require(hook_body.index("isaac_vita_game_log_abort_drain();") <
            hook_body.index("isaac_vita_log_async_flush();"),
            "the abort hook must drain the game log before the runtime log")

    # Module header: constants, defaults, static assert, loss table.
    for marker in (
        "#define ISAAC_VITA_GAME_LOG_PATH_MAX            1024U",
        "# define ISAAC_VITA_GAME_LOG_ASYNC_RING_KB      64U",
        "# define ISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY 1U",
        "#define ISAAC_VITA_GAME_LOG_RECORD_MAX          10240U",
        "#define ISAAC_VITA_CRT_GAME_LOG_FD_BASE         0x00410000U",
        "#define ISAAC_VITA_GAME_LOG_SEEK_DEMOTE         (-2)",
        "#define ISAAC_VITA_GAME_LOG_SEVERITY_UNKNOWN    4U",
        "_Static_assert(ISAAC_VITA_GAME_LOG_RING_BYTES >=\n"
        "                   2U * ISAAC_VITA_GAME_LOG_RECORD_MAX + 64U,",
        "controlled guest exit",
        "native data abort, kernel",
        "storage failure on append",
        "worker wedged > 10 s while",
        "suspend/PS-button ENODEV",
    ):
        require(marker in header, f"module header lost: {marker}")
    require("psp2/" not in header, "the module header must stay host-neutral")
    for receipt in RECEIPTS:
        require(module.count(receipt) == 1,
                f"receipt format must appear exactly once: {receipt[:40]}...")
    require('"isaac_game_log"' in module and
            '"isaac_game_log_wake"' in module and
            '"isaac_game_log_done"' in module,
            "kernel object names changed")
    require("#define GAME_LOG_WAIT_TIMEOUT_US   2000000U" in module and
            "#define GAME_LOG_WAIT_TIMEOUTS_MAX 5U" in module and
            "#define GAME_LOG_ABORT_SPINS       1000000U" in module and
            "#define GAME_LOG_APPEND_RECEIPTS   8U" in module and
            "#define GAME_LOG_REFUSE_RECEIPTS   8U" in module,
            "wait/abort/receipt bounds changed")
    # The refusal receipt is the device-side answer to "is the lane inert":
    # a write/append open of the game log the lane does not take is counted
    # and logged with the guest's mode string.
    require('game_log_refuse(mode, "mode");' in module and
            'game_log_refuse(mode, "live");' in module and
            'game_log_refuse(mode, "path");' in module,
            "refusal receipt reasons changed")
    require("uint32_t refused;" in header, "snapshot lost the refused counter")
    # The demotion places the reopened FILE at the lane's logical position
    # before the guest's own seek runs (a SEEK_CUR must resolve from where
    # the synchronous FILE would have been, not from newlib's fresh cursor).
    require("    if (fseek(stream, position, SEEK_SET) != 0)\n"
            "        return -1;\n"
            "    if (origin == -1)\n"
            "        return 0;\n"
            "    return fseek(stream, offset, origin);\n" in crt,
            "demotion no longer positions the reopened FILE first")
    for state in ('"cold"', '"start-failed"', '"live"', '"failed"', '"stopped"'):
        require(state in module, f"summary state name missing: {state}")
    require(re.search(r"\b(malloc|calloc|realloc|free)\s*\(", module) is None,
            "the module must not allocate")

    # Docs.
    readme = read(root / "recomp/vita/README.md")
    status = read(root / "STATUS.md")
    notes = read(root / "release/RELEASE_NOTES.md")
    require(f"- `{MACRO}` (default OFF, needs `ISAAC_VITA_ARCHIVE_FILE_CACHE`" in readme,
            "README knob paragraph missing")
    require(f"- With `{MACRO}` the game log is written behind the game" in readme,
            "README loss paragraph missing")
    require("17. Borrowed and parked OFF (2026-09-10): the game's `log.txt` written behind"
            in status, "STATUS known-problem item missing")
    require(f"- Game log write-behind lane (`{MACRO}`," in status,
            "STATUS tree bullet missing")
    require(f"New knob\n  `{MACRO}` (default OFF; needs" in notes,
            "release notes bullet missing")
    print("source pins: OK")


# --------------------------------------------------------------------------
# 4. OFF identity
# --------------------------------------------------------------------------
def strip_guarded(source: str, macros: tuple[str, ...] = (MACRO,)) -> str:
    out, depth, skip = [], 0, None
    guard = re.compile(r"#if\s+defined\((" + "|".join(map(re.escape, macros)) + r")\)")
    for line in source.split("\n"):
        s = line.strip()
        if s.startswith("#if"):
            depth += 1
            if skip is None and guard.fullmatch(s):
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


def verify_off_identity(root: Path) -> None:
    runtime = root / "recomp/runtime"
    for name, expected in STRIPPED_BASELINE.items():
        text = read(runtime / name)
        require("\r" not in text, f"{name} has CR line endings")
        blocks = re.findall(rf"^#if defined\({MACRO}\)$", text, re.M)
        require(len(blocks) == GUARDED_BLOCKS[name],
                f"{name}: {len(blocks)} guarded blocks, expected "
                f"{GUARDED_BLOCKS[name]}")
        require(re.search(rf"^#\s*(ifdef|ifndef|elif|else).*{MACRO}", text,
                          re.M) is None and
                re.search(rf"^#\s*if\s+.*{MACRO}.*(&&|\|\|)", text, re.M) is None and
                re.search(rf"^#\s*if\s+(?!defined\({MACRO}\)$).*{MACRO}", text,
                          re.M) is None,
                f"{name}: the macro appears outside a plain #if defined() guard")
        stripped = strip_guarded(text)
        require(MACRO not in stripped,
                f"{name}: the macro survives outside its guards")
        require(re.search(r"game_log|GAME_LOG_FD|isaac_vita_game_log",
                          stripped) is None,
                f"{name}: a lane token survives outside the guards")
        # The blob comparison also strips the blocks other lanes guarded in
        # these units after the base commit; the token checks above did not.
        remainder = strip_guarded(text, (MACRO, *FOREIGN_LANE_MACROS))
        for pattern in FOREIGN_LANE_BLOCKS.get(name, ()):
            remainder, hits = re.subn(pattern, "", remainder)
            require(hits == 1,
                    f"{name}: foreign lane block matched {hits} times, "
                    f"expected once: {pattern[:48]}")
        digest = sha256(remainder.encode("utf-8"))
        require(digest == expected,
                f"{name}: stripped remainder differs from the base commit "
                f"({digest})")
    for name, expected in UNTOUCHED_HEADERS.items():
        digest = sha256(read(runtime / name).encode("utf-8"))
        require(digest == expected, f"{name} changed ({digest})")
    print("OFF-identity pins: OK")


def verify_preprocess(root: Path, cc: str | None) -> None:
    if cc is None:
        print("preprocess check: skipped (no GCC-compatible host compiler; "
              "pass --cc)")
        return
    runtime = root / "recomp/runtime"
    with tempfile.TemporaryDirectory(prefix="isaac-game-log-pp-") as scratch_name:
        scratch = Path(scratch_name)
        stubs = scratch / "psp2" / "kernel"
        stubs.mkdir(parents=True)
        for stub in ("clib.h", "cpu.h", "processmgr.h", "threadmgr.h"):
            (stubs / stub).write_text("", encoding="ascii")
        common = [cc, "-std=gnu11", "-E", "-P", "-I", str(scratch),
                  "-I", str(runtime), "-I", str(root / "recomp/vita"),
                  "-DISAAC_VITA_LOG_ASYNC=1", "-DISAAC_VITA_LOG_ASYNC_FILE_BATCH=1"]
        unit = str(runtime / "host_vita_log_async.c")
        off = run([*common, unit])
        require(re.search(r"game_log", off) is None,
                "host_vita_log_async.c preprocessed without the knob carries a "
                "lane token")
        on = run([*common, f"-D{MACRO}=1", unit])
        # The header prototype plus exactly one call.
        require(on.count("isaac_vita_game_log_abort_drain") == 2,
                "host_vita_log_async.c preprocessed with the knob lost the drain")
    print("preprocess check: OK")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path)
    parser.add_argument("--root", type=Path, default=HERE.parent)
    parser.add_argument("--generated-dir", type=Path)
    parser.add_argument("--cc")
    args = parser.parse_args()
    root = args.root.resolve()

    self_test_fopen_census()
    if args.pe is not None:
        verify_pe(args.pe)
    else:
        print("PE pins: skipped (no --pe)")
    if args.generated_dir is not None:
        print("generated pins: none for this knob (no generated unit changes)")
    verify_wiring(root)
    verify_sources(root)
    verify_off_identity(root)
    verify_preprocess(root, compiler(args.cc))
    print("test_vita_game_log_async: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
