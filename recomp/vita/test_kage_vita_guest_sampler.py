#!/usr/bin/env python3
"""Source contract and host oracle for the guest sampler's attribution fixes.

Pins (research-sampler-hotlists finding 9 + finding 5):
  * guest.c publishes the indirect callee on the Enter/LeaveCriticalSection
    fast path and restores the enclosing target after every indirect dispatch
    (guest_call wrapper), all under ISAAC_VITA_GUEST_SAMPLER only;
  * guest_import_call (ISAAC_VITA_IMPORT_DIRECT) publishes its IAT slot once,
    restores the enclosing target after the family endpoint returns (sampler
    only) and notes the import ID in the per-import census once;
  * host_vita_lua.c restores the word on both Lua unwind routes (sampler only);
  * the fullspeed scheduler brackets its Game::Update pacing sleep with the
    PACE sampler phase and accumulates pace waits only under
    ISAAC_VITA_PROFILE_IMPORT_KINDS (the ph120.i reader);
  * the sampler scans 256 words, names the seventh phase "pace", prints
    `deep`, and its ph120.hs worst case computed from the real format string
    stays under the 384-byte log bound;
  * CMake feeds ISAAC_VITA_GUEST_SAMPLER to guest.c, the scheduler and the Lua
    bridge, feeds the census macro to the scheduler, defaults the census to the
    sampler's value and refuses FUNCTION_ENTRIES without the census.
Then compiles kage_vita_guest_sampler.c in oracle mode together with the
production scheduler and runs kage_vita_guest_sampler_oracle.c.  guest.c's own
publish/restore is driven white-box by recomp/test_guest_sampler_word.py
(guest_sampler_word_oracle.c includes guest.c; MSVC host like the other guest.c
oracles).
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def compiler() -> str:
    for name in ("cc", "gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    raise AssertionError("no host C compiler (cc/gcc/clang) on PATH")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def verify_source_contract(root: Path) -> None:
    runtime = root / "runtime"
    guest = (runtime / "guest.c").read_text(encoding="utf-8")
    sync = function_body(guest, "int guest_try_direct_sync_import_call(")
    if sync.count("g_kage_guest_last_indirect_target = target;") != 1:
        raise AssertionError("sync fast path does not publish its target once")
    # Two returns complete the import (the ISAAC_VITA_SYNC_INLINE_FASTPATH
    # single-owner state and the registered endpoint); each restores.
    if sync.count("g_kage_guest_last_indirect_target = "
                  "sampler_enclosing_target;") != 2:
        raise AssertionError("sync fast path does not restore the enclosing target")
    if sync.count("#if defined(ISAAC_VITA_GUEST_SAMPLER)") != 3:
        raise AssertionError("sync fast path sampler stores escaped their guard")
    if sync.index("isaac_vita_sync_inline_enter(") < sync.index(
            "g_kage_guest_last_indirect_target = sampler_enclosing_target;") < \
            sync.index("isaac_vita_sync_import_indexed("):
        pass
    else:
        raise AssertionError("inline fast path return does not restore first")
    # The direct IAT route never enters guest_call's wrapper: it publishes
    # once, restores once after the family endpoint, both under the sampler
    # guard, and notes the per-import census exactly like the indexed path.
    direct = function_body(guest, "void guest_import_call(")
    if direct.count("g_kage_guest_last_indirect_target = target;") != 1:
        raise AssertionError("guest_import_call does not publish its target once")
    if direct.count("g_kage_guest_last_indirect_target = "
                    "sampler_enclosing_target;") != 1:
        raise AssertionError("guest_import_call does not restore the enclosing target")
    if direct.count("#if defined(ISAAC_VITA_GUEST_SAMPLER)") != 2:
        raise AssertionError("guest_import_call sampler stores escaped their guard")
    if not direct.index("g_kage_guest_last_indirect_target = target;") < \
            direct.index("entry->fn(c, entry->local_index, &g_host_import_calls)") < \
            direct.index("g_kage_guest_last_indirect_target = sampler_enclosing_target;"):
        raise AssertionError("guest_import_call restore is not after the family endpoint")
    if direct.count("GUEST_PHASE_PROFILE_NOTE_CALL();") != 1 or \
            direct.count("GUEST_PHASE_PROFILE_NOTE_IMPORT(import_id);") != 1:
        raise AssertionError("guest_import_call census notes are absent or duplicated")
    if guest.count("# define guest_call guest_call_dispatch") != 1 or \
            guest.count("# undef guest_call") != 1:
        raise AssertionError("guest_call sampler wrapper rename is not exact")
    wrapper_start = guest.index("# undef guest_call")
    wrapper = function_body(guest[wrapper_start:], "void guest_call(")
    if wrapper.count("guest_call_dispatch(c, addr);") != 1 or wrapper.count(
            "g_kage_guest_last_indirect_target = sampler_enclosing_target;") != 1:
        raise AssertionError("guest_call sampler wrapper does not restore")
    # The entry publish sits in whichever body is guest_call for the build:
    # the complete classification (first definition; option-OFF guest_call)
    # and the ISAAC_VITA_GUEST_DISPATCH_TABLE inline probe (second), each
    # exactly once; the wrapper follows the probe so guest_import_call's
    # fallback and the TLS callbacks reach the wrapper.
    dispatch = function_body(guest, "void guest_call(")
    if dispatch.count("g_kage_guest_last_indirect_target = addr;") != 1:
        raise AssertionError("guest_call entry publish is absent or duplicated")
    probe_start = guest.index("void guest_call(") + 1
    probe = function_body(guest[probe_start:], "void guest_call(")
    if probe.count("g_kage_guest_last_indirect_target = addr;") != 1 or \
            probe.count("guest_dispatch_probe(addr)") != 1:
        raise AssertionError("inline probe entry publish is absent or duplicated")
    if not guest.index("guest_dispatch_probe(addr)") < wrapper_start < \
            guest.index("#if defined(ISAAC_VITA_IMPORT_DIRECT)\n"
                        "static void guest_import_direct_rebuild("):
        raise AssertionError("sampler wrapper is not between the probe and "
                             "guest_import_call")

    scheduler = (runtime / "kage_vita_fullspeed_scheduler.c").read_text(
        encoding="utf-8")
    pace = function_body(scheduler, "static void kage_vita_fullspeed_pace_game_update(")
    if pace.count("KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_PACE);") != 1:
        raise AssertionError("pace sleep is not bracketed by the PACE phase")
    if pace.count("KAGE_VITA_GUEST_SAMPLER_PHASE(sampler_phase);") != 1:
        raise AssertionError("pace sleep does not restore the sampler phase")
    set_pace = pace.index("KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_PACE);")
    wait = pace.index("kage_vita_fullspeed_wait_until(")
    restore = pace.index("KAGE_VITA_GUEST_SAMPLER_PHASE(sampler_phase);")
    if not set_pace < wait:
        raise AssertionError("PACE phase is not set before the wait")
    if not wait < restore:
        raise AssertionError("sampler phase is not restored after the wait")
    if pace.count("s_counters.pace_wait_calls") != 1 or \
            pace.count("s_counters.pace_waited_us") != 1:
        raise AssertionError("pace wait counters are not accumulated once")
    if pace.count("#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)") != 2:
        raise AssertionError("pace accounting is not confined to the census build")
    census_blocks = re.findall(
        r"#if defined\(ISAAC_VITA_PROFILE_IMPORT_KINDS\)\n(.*?)#endif", pace, re.S)
    if len(census_blocks) != 2 or \
            "wait_calls_before = s_counters.wait_calls" not in census_blocks[0] or \
            "&s_counters.pace_wait_calls" not in census_blocks[1] or \
            "&s_counters.pace_waited_us" not in census_blocks[1]:
        raise AssertionError("pace accounting escaped the census guard")

    lua = (runtime / "host_vita_lua.c").read_text(encoding="utf-8")
    if lua.count("#if defined(ISAAC_VITA_GUEST_SAMPLER)") != 5:
        raise AssertionError("Lua bridge sampler hunks are not exactly five")
    call_guest = function_body(lua, "static int vita_lua_call_guest(")
    if call_guest.count(
            "frame->sampler_enclosing_target = g_kage_guest_last_indirect_target;") != 1:
        raise AssertionError("Lua callback frame does not save the enclosing target")
    if call_guest.count(
            "g_kage_guest_last_indirect_target = frame->sampler_enclosing_target;") != 1:
        raise AssertionError("vita_lua_call_guest does not restore after its setjmp")
    if not call_guest.index("setjmp(frame->escape)") < call_guest.index(
            "g_kage_guest_last_indirect_target = frame->sampler_enclosing_target;"):
        raise AssertionError("Lua restore is not after the escape landing")
    if not call_guest.index("frame->sampler_enclosing_target = ") < \
            call_guest.index("setjmp(frame->escape)"):
        raise AssertionError("Lua frame save is not before the setjmp")
    recover = function_body(lua, "static int vita_lua_recover_callbacks(")
    if recover.count(
            "g_kage_guest_last_indirect_target = frame->sampler_enclosing_target;") != 1:
        raise AssertionError("vita_lua_recover_callbacks does not restore the word")
    if not recover.index("g_kage_guest_last_indirect_target = ") < \
            recover.index("vita_lua_callback_frame_pop(slot);"):
        raise AssertionError("Lua recovery restores after the frame is popped")
    if lua.count("uint32_t sampler_enclosing_target;") != 1:
        raise AssertionError("Lua callback frame lacks the sampler field")
    for hunk in re.findall(
            r"#if defined\(ISAAC_VITA_GUEST_SAMPLER\)\n(.*?)#endif", lua, re.S):
        if "sampler" not in hunk and "kage_vita_guest_sampler.h" not in hunk:
            raise AssertionError(f"unexpected Lua sampler hunk: {hunk!r}")

    sampler = (runtime / "kage_vita_guest_sampler.c").read_text(encoding="utf-8")
    if "#define KVGS_STACK_SCAN_WORDS 256U" not in sampler:
        raise AssertionError("sampler scan bound is not 256 words")
    if '{ "svc", "upd", "rnd", "swp", "lim", "oth", "pace" };' not in sampler:
        raise AssertionError("sampler phase names lack the pace bucket")
    if "samples(svc,upd,rnd,swp,lim,oth,pace)=" not in sampler or \
            " deep=%u " not in sampler:
        raise AssertionError("ph120.hs record lacks pace/deep")
    hs_pieces = re.findall(r'"([^"\n]*)"', sampler[
        sampler.index('"[kage-vita] ph120.hs bid='):sampler.index(
            'build_id, window, last_outer_loop, (unsigned)KVGS_PERIOD_US')])
    hs_text = "".join(hs_pieces).replace("\\n", "\n")
    if hs_text.count("%u") != 18 or "%.32s" not in hs_text or \
            hs_text.count("%") != 19:
        raise AssertionError(f"ph120.hs format drifted: {hs_text!r}")
    hs_worst = len(hs_text.replace("%.32s", "x" * 32).replace(
        "%u", "4294967295"))
    if hs_worst != 361 or hs_worst >= 384:
        raise AssertionError(f"ph120.hs worst case {hs_worst} (expected 361 < 384)")
    if "Worst case 361 bytes" not in sampler:
        raise AssertionError("ph120.hs worst-case comment disagrees with 361")
    header = (runtime / "kage_vita_guest_sampler.h").read_text(encoding="utf-8")
    if header.index("KAGE_VITA_GUEST_SAMPLER_OTH,") > header.index(
            "KAGE_VITA_GUEST_SAMPLER_PACE,"):
        raise AssertionError("PACE must follow OTH so older bucket indices hold")

    cmake = (root / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    block = re.search(
        r"      if\(ISAAC_VITA_GUEST_SAMPLER\)\n(.*?)\n      endif\(\)", cmake, re.S)
    if block is None:
        raise AssertionError("ISAAC_VITA_GUEST_SAMPLER block not found in CMake")
    for owner in ("guest.c", "kage_vita_fullspeed_scheduler.c",
                  "kage_vita_guest_sampler.c", "kage_vita_phase_profile.c",
                  "entry_vita.c", "host_vita_lua.c"):
        if f'"${{ISAAC_RUNTIME}}/{owner}"' not in block[1]:
            raise AssertionError(f"sampler macro does not reach {owner}")
    census = re.search(
        r"      if\(ISAAC_VITA_PROFILE_IMPORT_KINDS\)\n(.*?)\n      endif\(\)",
        cmake, re.S)
    if census is None:
        raise AssertionError("ISAAC_VITA_PROFILE_IMPORT_KINDS block not found")
    for owner in ("guest.c", "host_vita_import_id.c",
                  "kage_vita_phase_profile.c", "kage_vita_fullspeed_scheduler.c"):
        if f'"${{ISAAC_RUNTIME}}/{owner}"' not in census[1]:
            raise AssertionError(f"census macro does not reach {owner}")
    if "host_vita_lua.c" in census[1] or "entry_vita.c" in census[1]:
        raise AssertionError("census macro reaches a non-owner")
    default = re.search(
        r"if\(ISAAC_VITA_GUEST_SAMPLER\)\n"
        r"  set\(ISAAC_VITA_PROFILE_IMPORT_KINDS_DEFAULT ON\)\n"
        r"else\(\)\n"
        r"  set\(ISAAC_VITA_PROFILE_IMPORT_KINDS_DEFAULT OFF\)\n"
        r"endif\(\)\n"
        r"option\(ISAAC_VITA_PROFILE_IMPORT_KINDS\n"
        r'       "[^"]*"\n'
        r"       \$\{ISAAC_VITA_PROFILE_IMPORT_KINDS_DEFAULT\}\)", cmake)
    if default is None:
        raise AssertionError(
            "ISAAC_VITA_PROFILE_IMPORT_KINDS default does not follow the sampler")
    if 'option(ISAAC_VITA_GUEST_SAMPLER' not in cmake[:default.start()]:
        raise AssertionError("sampler option must be declared before the census default")
    if re.search(r'option\(ISAAC_VITA_PROFILE_IMPORT_KINDS\n\s+"[^"]*"\s+ON\)',
                 cmake):
        raise AssertionError("census must not default ON unconditionally")
    if ("if(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES AND NOT "
            "ISAAC_VITA_PROFILE_IMPORT_KINDS)") not in cmake:
        raise AssertionError("FUNCTION_ENTRIES without the census is not rejected")


def run(command: list[str]) -> str:
    completed = subprocess.run(command, check=False, text=True,
                               capture_output=True)
    if completed.returncode != 0:
        raise AssertionError(
            f"{command[0]} failed ({completed.returncode}):\n"
            f"{completed.stdout}\n{completed.stderr}")
    return completed.stdout


def main() -> int:
    vita = Path(__file__).resolve().parent
    root = vita.parent
    runtime = root / "runtime"
    verify_source_contract(root)
    cc = compiler()
    with tempfile.TemporaryDirectory(prefix="isaac-guest-sampler-") as value:
        exe = Path(value) / ("sampler-oracle.exe" if os.name == "nt"
                             else "sampler-oracle")
        run([
            cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-DGUEST_IMAGE_BASE=0x98000000u",
            "-DISAAC_KAGE_VITA_GUEST_SAMPLER_ORACLE=1",
            "-DISAAC_VITA_GUEST_SAMPLER=1",
            "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
            "-DISAAC_VITA_PROFILE_IMPORT_KINDS=1",
            '-DISAAC_VITA_FULLSPEED_SCHEDULER_BUILD_ID="sampler:oracle"',
            f"-I{runtime}", f"-I{vita}",
            str(runtime / "kage_vita_guest_sampler.c"),
            str(runtime / "kage_vita_fullspeed_scheduler.c"),
            str(runtime / "kage_vita_guest_sampler_oracle.c"),
            "-o", str(exe),
        ])
        stdout = run([str(exe)])
    for needle in (
        "phase=rnd samples=3 top=5a0de0:3",
        "phase=rnd samples=7 top=ext:4,562e00:2,ind:1",
        "phase=upd samples=1 top=23f0:1",
        "samples(svc,upd,rnd,swp,lim,oth,pace)=0,1,0,0,0,0,0 total=1 "
        "nomark=0 badesp=0 full=0 words=201 deep=1",
        "total=0 nomark=1 badesp=0 full=0 words=256 deep=0",
        "phase=pace samples=",
        "Vita guest sampler host oracle: PASS",
    ):
        if needle not in stdout:
            raise AssertionError(f"oracle output lacks {needle!r}:\n{stdout}")
    if re.search(r"phase=upd samples=\d+ top=4b0010", stdout):
        raise AssertionError("pacing sleep still charged to upd")
    sys.stdout.write(stdout)
    print("guest sampler source contract + host oracle: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
