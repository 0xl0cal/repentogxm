#!/usr/bin/env python3
"""Frozen-PE proof for ISAAC_VITA_THEORA_WORKER_PUMP.

Pins every guest fact host_vita_theora_worker_pump.c relies on: the shape of
TheoraWorkerThread::execute (sub_005c2dc0: the mRunning/mStarted bytes at
+4/+0x14, requestWork, the clip's mThreadAccessMutex at +0x38, mSeekFrame at
+0x88, doSeek at vt+0x1c, decodeNextFrame at vt+0x10, mAssignedWorkerThread at
+0x30, the two Sleep sites and the tail re-check), the cdecl proof in
Thread::process (push this; call; add esp, 4), the worker object layout from
createWorkerThreads (0x1c bytes, loop entry at +0xc, vtable), the manager
singleton and its clip vector / mWorkMutex offsets from requestWork, the
hltypes Mutex lock byte and its re-lock spin, the clip vtable slots, the
audio mutex decodeNextFrame takes, the shape of the game-inlined
TheoraVideoManager::update that triggers a slice (sub_00598d60: manager
singleton, mWorkMutex lock/unlock, clip vector walk) and its exact two call
sites (the main loop at 0x48bdf0, sub_00475810 at 0x476b33), the frame fetch
the renderer really uses (getNextFrame sub_00599db0 at 0x4b89b3), the
end-of-file gate at 0x4b78a0 that keeps getFirstAvailableFrame/getReadyCount
unreached before the video ends (why they are not the trigger), and the
generator's worker bypass that makes the pump necessary.  The runtime/CMake
text that mirrors them is checked without a PE.

usage: python test_theora_worker_pump_sites.py --pe <isaac-ng.exe.unpacked.exe>
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
from pathlib import Path

import capstone
import pefile


HERE = Path(__file__).resolve().parent
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
IMAGE_BASE = 0x00400000

WORKER_LOOP = 0x005C2DC0       # TheoraWorkerThread::execute(TheoraWorkerThread*)
THREAD_PROCESS = 0x005A58C0    # hltypes::Thread::process (thiscall)
THREAD_TRAMPOLINE = 0x005A5720 # CreateThread start routine
CREATE_WORKERS = 0x005AD490    # TheoraVideoManager::createWorkerThreads(int)
REQUEST_WORK = 0x005AD6A0      # TheoraVideoManager::requestWork(TheoraWorkerThread*)
MUTEX_LOCK = 0x005A5550        # hltypes::Mutex::lock (thiscall)
DECODE_NEXT_FRAME = 0x005AA6D0 # TheoraVideoClip_Theora::decodeNextFrame
DO_SEEK = 0x005AADA0           # TheoraVideoClip_Theora::doSeek
MANAGER_UPDATE = 0x00598D60    # TheoraVideoManager::update, game-inlined (the trigger seam)
CLIP_UPDATE = 0x00595390       # TheoraVideoClip::update (called per clip by the above)
GET_NEXT_FRAME = 0x00599DB0    # TheoraVideoClip::getNextFrame, game-inlined
GET_FIRST_AVAILABLE = 0x005A4BB0  # TheoraFrameQueue::getFirstAvailableFrame (behind EOF)
GET_READY_COUNT = 0x005A4C00   # TheoraFrameQueue::getReadyCount (behind EOF)
CLIP_VTABLE = 0x00767A18       # TheoraVideoClip_Theora vtable
CLIP_VTABLE_STORE = 0x005A9B00 # ctor: mov [esi], vtable
WORKER_VTABLE = 0x00769338
MANAGER_SINGLETON = 0x007FEC00
SLEEP_IAT = 0x00606138
LEAVE_CS_IAT = 0x006060F8
ENTER_CS_IAT = 0x006060FC
CREATE_THREAD_IAT = 0x006060B4
MANAGER_UPDATE_SITES = [0x00476B33, 0x0048BDF0]
GET_NEXT_FRAME_SITES = [0x004B89B3]
GET_FIRST_AVAILABLE_SITES = [0x004B0745, 0x004B78A9, 0x004B89E8]
GET_READY_COUNT_SITES = [0x004B8921, 0x005AD79D]
MUTEX_LOCK_GAME_SITES = [0x005950BD, 0x005951B5, 0x0059526E, 0x00598E19,
                         0x00599E10, 0x00599FBE, 0x00599FF5]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def decode(pe: pefile.PE, start: int, length: int) -> dict[int, str]:
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    instructions = list(decoder.disasm(pe.get_data(start, length),
                                       IMAGE_BASE + start))
    require(bool(instructions), f"empty decode at {start:08x}")
    return {i.address - IMAGE_BASE: f"{i.mnemonic} {i.op_str}".strip()
            for i in instructions}


def expect(body: dict[int, str], rva: int, text: str) -> None:
    require(body.get(rva) == text,
            f"{rva:08x}: expected `{text}`, found `{body.get(rva)}`")


def va(rva: int) -> str:
    return f"0x{IMAGE_BASE + rva:x}"


def direct_call_sites(code: bytes, text_rva: int, target: int) -> list[int]:
    sites = []
    for match in re.finditer(rb"\xe8", code):
        offset = match.start()
        rel = int.from_bytes(code[offset + 1:offset + 5], "little", signed=True)
        if text_rva + offset + 5 + rel == target:
            sites.append(text_rva + offset)
    return sites


def check_pe(path: Path) -> None:
    data = path.read_bytes()
    require(len(data) == PE_SIZE, f"PE size {len(data)} != {PE_SIZE}")
    require(hashlib.sha256(data).hexdigest() == PE_SHA256, "PE SHA-256 drifted")
    pe = pefile.PE(data=data, fast_load=True)
    text_section = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    code = bytes(pe.get_data(text_section.VirtualAddress, text_section.Misc_VirtualSize))

    # Worker loop: flags, requestWork, locks, virtual calls, sleeps, tail.
    loop = decode(pe, WORKER_LOOP, 0x110)
    expect(loop, 0x5C2DF3, "mov esi, dword ptr [ebp + 8]")
    expect(loop, 0x5C2DFD, "cmp byte ptr [esi + 4], 0")
    expect(loop, 0x5C2E07, f"mov edi, dword ptr [{va(SLEEP_IAT)}]")
    expect(loop, 0x5C2E10, "cmp byte ptr [esi + 0x14], 0")
    expect(loop, 0x5C2E1A, "push esi")
    expect(loop, 0x5C2E1B, f"call {va(REQUEST_WORK)}")
    expect(loop, 0x5C2E22, "mov dword ptr [esi + 0x18], ecx")
    expect(loop, 0x5C2E33, "push 0x64")
    expect(loop, 0x5C2E35, f"jmp {va(0x5C2EA1)}")   # both sleeps share one call site
    expect(loop, 0x5C2E3B, "mov eax, dword ptr [ecx + 0x38]")
    expect(loop, 0x5C2E49, f"call {va(MUTEX_LOCK)}")
    expect(loop, 0x5C2E51, "cmp dword ptr [ecx + 0x88], 0")
    expect(loop, 0x5C2E5C, "call dword ptr [eax + 0x1c]")
    expect(loop, 0x5C2E64, "mov eax, dword ptr [eax + 0x10]")
    expect(loop, 0x5C2E67, "call eax")
    expect(loop, 0x5C2E6E, "cmp dword ptr [ecx + 0x30], esi")
    expect(loop, 0x5C2E73, "mov dword ptr [ecx + 0x30], 0")
    expect(loop, 0x5C2E7A, "mov dword ptr [esi + 0x18], 0")
    expect(loop, 0x5C2E8A, "mov byte ptr [eax + 4], 0")
    expect(loop, 0x5C2E8E, f"call dword ptr [{va(LEAVE_CS_IAT)}]")
    expect(loop, 0x5C2E9F, "push 1")
    expect(loop, 0x5C2EA1, "call edi")
    expect(loop, 0x5C2EA3, "cmp byte ptr [esi + 4], 0")
    expect(loop, 0x5C2EA7, f"jne {va(0x5C2E10)}")
    expect(loop, 0x5C2EC6, "ret")

    # Thread::process: the loop is cdecl with the thread object as argument.
    process = decode(pe, THREAD_PROCESS, 0x80)
    expect(process, 0x5A58F3, "cmp byte ptr [esi + 0x14], 0")
    expect(process, 0x5A5905, "mov byte ptr [esi + 4], 1")
    expect(process, 0x5A5910, "mov eax, dword ptr [esi + 0xc]")
    expect(process, 0x5A5913, "push esi")
    expect(process, 0x5A5914, "call eax")
    expect(process, 0x5A5916, "add esp, 4")
    expect(process, 0x5A5919, "mov byte ptr [esi + 4], 0")
    expect(process, 0x5A591D, "mov byte ptr [esi + 0x14], 0")

    # Trampoline: [param] is the thread object; SetThreadName runs before it.
    trampoline = decode(pe, THREAD_TRAMPOLINE, 0x20)
    expect(trampoline, 0x5A5730, "mov ecx, dword ptr [ebp + 8]")
    expect(trampoline, 0x5A5733, "mov ecx, dword ptr [ecx]")
    expect(trampoline, 0x5A5735, f"call {va(THREAD_PROCESS)}")

    # createWorkerThreads: worker object layout and the CreateThread call.
    create = decode(pe, CREATE_WORKERS, 0x200)
    expect(create, 0x5AD4D2, "push 0x1c")
    expect(create, 0x5AD4F6, "lea ebx, [esi + 8]")
    expect(create, 0x5AD4F9, "mov byte ptr [esi + 4], 0")
    expect(create, 0x5AD4FD, "mov dword ptr [ebx], esi")
    expect(create, 0x5AD4FF, "mov dword ptr [esi + 0x10], 0")
    expect(create, 0x5AD506, "mov byte ptr [esi + 0x14], 0")
    expect(create, 0x5AD50A, f"mov dword ptr [esi + 0xc], {va(WORKER_LOOP)}")
    expect(create, 0x5AD511, f"mov dword ptr [esi], {va(WORKER_VTABLE)}")
    expect(create, 0x5AD517, "mov dword ptr [esi + 0x18], 0")
    expect(create, 0x5AD637, "mov byte ptr [esi + 0x14], 1")
    expect(create, 0x5AD651, "push ebx")
    expect(create, 0x5AD652, f"push {va(THREAD_TRAMPOLINE)}")
    expect(create, 0x5AD65B, f"call dword ptr [{va(CREATE_THREAD_IAT)}]")
    expect(create, 0x5AD661, "mov dword ptr [esi + 0x10], eax")

    # requestWork: singleton, mWorkMutex, clip vector, assignment, stdcall.
    request = decode(pe, REQUEST_WORK, 0x3C2)
    expect(request, 0x5AD6C7, f"mov esi, dword ptr [{va(MANAGER_SINGLETON)}]")
    expect(request, 0x5AD6DB, "mov ecx, dword ptr [esi + 0x28]")
    expect(request, 0x5AD6E5, f"call {va(MUTEX_LOCK)}")
    expect(request, 0x5AD73E, "mov esi, dword ptr [esi + 0x10]")
    expect(request, 0x5AD741, "cmp esi, dword ptr [eax + 0x14]")
    expect(request, 0x5AD752, "cmp dword ptr [ecx + 0x30], 0")
    expect(request, 0x5AD79D, f"call {va(GET_READY_COUNT)}")
    expect(request, 0x5AD949, "mov eax, dword ptr [ebp + 8]")
    expect(request, 0x5AD94C, "mov dword ptr [edx + 0x30], eax")
    expect(request, 0x5ADA5F, "ret 4")

    # hltypes Mutex::lock: EnterCriticalSection, lock byte at +4, spin.
    lock = decode(pe, MUTEX_LOCK, 0x139)
    expect(lock, 0x5A557E, "push dword ptr [esi]")
    expect(lock, 0x5A5580, f"call dword ptr [{va(ENTER_CS_IAT)}]")
    expect(lock, 0x5A5586, "cmp byte ptr [esi + 4], 0")
    expect(lock, 0x5A565F, f"mov esi, dword ptr [{va(SLEEP_IAT)}]")
    expect(lock, 0x5A5665, "push 1")
    expect(lock, 0x5A5667, "call esi")
    expect(lock, 0x5A5669, f"jmp {va(0x5A5665)}")
    expect(lock, 0x5A566B, "mov byte ptr [esi + 4], 1")

    # Clip vtable: decodeNextFrame at +0x10, doSeek at +0x1c; ctor store.
    table = pe.get_data(CLIP_VTABLE, 0x24)
    slots = [int.from_bytes(table[i:i + 4], "little") - IMAGE_BASE
             for i in range(0, 0x24, 4)]
    require(slots[4] == DECODE_NEXT_FRAME, f"clip vt+0x10 is {slots[4]:08x}")
    require(slots[7] == DO_SEEK, f"clip vt+0x1c is {slots[7]:08x}")
    ctor = decode(pe, CLIP_VTABLE_STORE, 0x06)
    expect(ctor, CLIP_VTABLE_STORE, f"mov dword ptr [esi], {va(CLIP_VTABLE)}")

    # decodeNextFrame takes the clip's audio mutex ([this+0x34]) itself.
    decode_body = decode(pe, DECODE_NEXT_FRAME, 0x22C)
    expect(decode_body, 0x5AA806, "mov ecx, dword ptr [esi + 0x34]")
    expect(decode_body, 0x5AA81B, f"call {va(MUTEX_LOCK)}")
    expect(decode_body, 0x5AA8FB, "ret")

    # The trigger seam, TheoraVideoManager::update: no theora mutex is held at
    # entry (it takes mWorkMutex itself), it walks the clip vector, and it
    # releases the mutex before returning.  A bool arrives in CL.
    update = decode(pe, MANAGER_UPDATE, 0x124)
    expect(update, 0x598DE5, f"mov edi, dword ptr [{va(MANAGER_SINGLETON)}]")
    expect(update, 0x598E0A, "mov ecx, dword ptr [edi + 0x28]")
    expect(update, 0x598E19, f"call {va(MUTEX_LOCK)}")
    expect(update, 0x598E2D, "mov esi, dword ptr [edi + 0x10]")
    expect(update, 0x598E30, "cmp esi, dword ptr [edi + 0x14]")
    expect(update, 0x598E37, f"call {va(CLIP_UPDATE)}")
    expect(update, 0x598E40, "call dword ptr [eax + 0x18]")
    expect(update, 0x598E59, "mov byte ptr [eax + 4], 0")
    expect(update, 0x598E5D, f"call dword ptr [{va(LEAVE_CS_IAT)}]")
    expect(update, 0x598E83, "ret")
    main_loop = decode(pe, 0x48BDEE, 0x0C)
    expect(main_loop, 0x48BDEE, "xor cl, cl")
    expect(main_loop, 0x48BDF0, f"call {va(MANAGER_UPDATE)}")
    other = decode(pe, 0x476B31, 0x07)
    expect(other, 0x476B31, "mov cl, 1")
    expect(other, 0x476B33, f"call {va(MANAGER_UPDATE)}")
    sites = direct_call_sites(code, text_section.VirtualAddress, MANAGER_UPDATE)
    require(sites == MANAGER_UPDATE_SITES,
            f"TheoraVideoManager::update sites drifted: {[hex(s) for s in sites]}")

    # The renderer fetches frames through the game-inlined getNextFrame.
    sites = direct_call_sites(code, text_section.VirtualAddress, GET_NEXT_FRAME)
    require(sites == GET_NEXT_FRAME_SITES,
            f"getNextFrame sites drifted: {[hex(s) for s in sites]}")

    # Frame queue accessors: thiscall, lock the queue mutex [this+0xc]
    # themselves; the game reaches them only behind the clip's end-of-file
    # byte (+0x74), which is why they cannot trigger the pump.
    first = decode(pe, GET_FIRST_AVAILABLE, 0x49)
    expect(first, 0x5A4BBA, "mov esi, ecx")
    expect(first, 0x5A4BC9, "mov ecx, dword ptr [esi + 0xc]")
    expect(first, 0x5A4BD4, f"call {va(MUTEX_LOCK)}")
    expect(first, 0x5A4BF8, "ret")
    ready = decode(pe, GET_READY_COUNT, 0x60)
    expect(ready, 0x5A4C0A, "mov esi, ecx")
    expect(ready, 0x5A4C1A, "mov ecx, dword ptr [esi + 0xc]")
    expect(ready, 0x5A4C25, f"call {va(MUTEX_LOCK)}")
    gate = decode(pe, 0x4B78A0, 0x0E)
    expect(gate, 0x4B78A0, "cmp byte ptr [esi + 0x74], 0")
    expect(gate, 0x4B78A4, f"je {va(0x4B78BD)}")
    expect(gate, 0x4B78A6, "mov ecx, dword ptr [esi + 0x1c]")
    expect(gate, 0x4B78A9, f"call {va(GET_FIRST_AVAILABLE)}")

    # Exact direct call sites of the accessors and of the lock in game code.
    sites = direct_call_sites(code, text_section.VirtualAddress, GET_FIRST_AVAILABLE)
    require(sites == GET_FIRST_AVAILABLE_SITES,
            f"getFirstAvailableFrame sites drifted: {[hex(s) for s in sites]}")
    sites = direct_call_sites(code, text_section.VirtualAddress, GET_READY_COUNT)
    require(sites == GET_READY_COUNT_SITES,
            f"getReadyCount sites drifted: {[hex(s) for s in sites]}")
    sites = [s for s in direct_call_sites(code, text_section.VirtualAddress, MUTEX_LOCK)
             if s < 0x005A0000]
    require(sites == MUTEX_LOCK_GAME_SITES,
            f"game-side Mutex::lock sites drifted: {[hex(s) for s in sites]}")

    # The generator's bypass site is the only reason no worker exists.
    generator = (HERE / "gen_all.py").read_text(encoding="utf-8")
    machine = re.search(r"THEORAPLAYER_PC_WORKER_BYPASS_MACHINE_RVA = (0x[0-9A-Fa-f]+)", generator)
    site = re.search(r"THEORAPLAYER_PC_WORKER_BYPASS_SITE_RVA = (0x[0-9A-Fa-f]+)", generator)
    require(machine is not None and site is not None, "generator bypass constants missing")
    assert machine is not None and site is not None
    machine_rva = int(machine.group(1), 16)
    site_rva = int(site.group(1), 16)
    require(machine_rva == 0x005ACA57 and site_rva == 0x005ACA65,
            "generator bypass site moved")
    bypass = decode(pe, machine_rva, 0x14)
    expect(bypass, 0x5ACA65, "push eax")
    expect(bypass, 0x5ACA66, f"call {va(CREATE_WORKERS)}")


def check_sources() -> None:
    runtime = (HERE / "runtime" / "host_vita_theora_worker_pump.c").read_text(encoding="utf-8")
    for symbol in ("__wrap_sub_00598d60", "__real_sub_00598d60",
                   "__wrap_sub_005aa6d0", "__real_sub_005aa6d0"):
        require(symbol in runtime, f"runtime symbol {symbol}")
    for symbol in ("sub_005a4bb0", "sub_005a4c00"):
        require(f"__wrap_{symbol}" not in runtime,
                f"runtime still wraps the EOF-gated accessor {symbol}")
    pins = {
        "VITA_THEORA_MANAGER_SINGLETON_RVA": "0x007fec00U",
        "VITA_THEORA_WORKER_LOOP_RVA": "0x005c2dc0U",
        "VITA_THEORA_WORKER_VTABLE_RVA": "0x00769338U",
        "VITA_THEORA_WORKER_SLEEP_RETURN_RVA": "0x005c2ea3U",
        "VITA_THEORA_MUTEX_SPIN_RETURN_RVA": "0x005a5669U",
        "VITA_THEORA_WORKER_SIZE": "0x1cU",
        "VITA_THEORA_WORKER_RUNNING_OFFSET": "0x4U",
        "VITA_THEORA_WORKER_SELF_OFFSET": "0x8U",
        "VITA_THEORA_WORKER_FUNCTION_OFFSET": "0xcU",
        "VITA_THEORA_WORKER_HANDLE_OFFSET": "0x10U",
        "VITA_THEORA_WORKER_STARTED_OFFSET": "0x14U",
        "VITA_THEORA_WORKER_CLIP_OFFSET": "0x18U",
        "VITA_THEORA_MANAGER_CLIPS_BEGIN": "0x10U",
        "VITA_THEORA_MANAGER_CLIPS_END": "0x14U",
        "VITA_THEORA_MANAGER_WORK_MUTEX": "0x28U",
        "VITA_THEORA_CLIP_ASSIGNED_WORKER": "0x30U",
        "VITA_THEORA_CLIP_AUDIO_MUTEX": "0x34U",
        "VITA_THEORA_CLIP_THREAD_MUTEX": "0x38U",
        "VITA_THEORA_MUTEX_LOCKED_OFFSET": "0x4U",
    }
    for name, value in pins.items():
        match = re.search(rf"#define {name}\s+(0x[0-9a-fA-F]+U)", runtime)
        require(match is not None and match.group(1) == value,
                f"runtime pin {name} is {match and match.group(1)}, expected {value}")
    require("#error" in runtime, "runtime file must refuse a knob-less compile")
    require("isaac_vita_theora_worker_pump_intercept_sleep" in runtime, "sleep intercept")
    # Translated call sites push bare return RVAs; a based comparison never
    # matches and a full queue then sleeps the real Sleep(100) forever.
    require("return_address == VITA_THEORA_WORKER_SLEEP_RETURN_RVA" in runtime and
            "return_address == VITA_THEORA_MUTEX_SPIN_RETURN_RVA" in runtime,
            "sleep intercept must compare bare RVAs")
    require("GUEST_IMAGE_BASE + VITA_THEORA_WORKER_SLEEP_RETURN_RVA" not in runtime and
            "GUEST_IMAGE_BASE + VITA_THEORA_MUTEX_SPIN_RETURN_RVA" not in runtime,
            "sleep intercept compares a based return address")
    sync = (HERE / "runtime" / "host_vita_sync.c").read_text(encoding="utf-8")
    require("#if defined(ISAAC_VITA_THEORA_WORKER_PUMP)" in sync and
            "isaac_vita_theora_worker_pump_intercept_sleep(c, vita_sync_arg(c, 0U))" in sync,
            "Sleep shim does not consult the pump behind the knob")
    cmake = (HERE / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    require("set(ISAAC_VITA_THEORA_WORKER_PUMP 0 CACHE STRING" in cmake,
            "knob default is not 0")
    require("-Wl,--wrap=sub_00598d60 -Wl,--wrap=sub_005aa6d0" in cmake,
            "wrap link options")
    require("--wrap=sub_005a4bb0" not in cmake and "--wrap=sub_005a4c00" not in cmake,
            "CMake still wraps the EOF-gated accessors")
    require("host_vita_theora_worker_pump.c" in cmake, "runtime source wiring")
    require('"${ISAAC_RUNTIME}/host_vita_sync.c"\n      APPEND PROPERTY COMPILE_DEFINITIONS\n'
            "        ISAAC_VITA_THEORA_WORKER_PUMP=${ISAAC_VITA_THEORA_WORKER_PUMP})" in cmake,
            "host_vita_sync.c does not receive the knob definition")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path,
                        default=Path(os.environ.get("ISAAC_AUDIO_TEST_PE", "")))
    arguments = parser.parse_args()
    check_sources()
    if not arguments.pe or not arguments.pe.is_file():
        print("theora worker pump sites: sources OK, no --pe given (PE pins not run)")
        return 0
    check_pe(arguments.pe)
    print("theora worker pump sites: PASS (worker loop, thread object, requestWork, "
          "mutex, clip vtable, manager update seam, getNextFrame, EOF gate, "
          "generator bypass, sources)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
