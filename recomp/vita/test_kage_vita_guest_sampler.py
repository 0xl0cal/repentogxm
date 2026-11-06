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

import importlib.util
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
    """The body text from the opening brace to its matching close.  Braces in
    an #else/#elif arm are not counted (only the first arm of a conditional
    is, as if every #if were true): vita_lua_call_guest closes the same block
    in both arms of ISAAC_VITA_LUA_SCOPE_FASTPATH."""
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    skipping: list[bool] = []   # per open conditional: inside a later arm?
    offset = source.rfind("\n", 0, opening) + 1
    first = True
    for line in source[offset:].splitlines(keepends=True):
        stripped = line.lstrip()
        if stripped.startswith("#"):
            words = stripped[1:].split(None, 1)
            word = words[0] if words else ""
            if word in ("if", "ifdef", "ifndef"):
                skipping.append(False)
            elif word in ("else", "elif") and skipping:
                skipping[-1] = True
            elif word == "endif" and skipping:
                skipping.pop()
            offset += len(line)
            first = False
            continue
        begin = opening - offset if first else 0
        first = False
        if not any(skipping):
            for index in range(begin, len(line)):
                if line[index] == "{":
                    depth += 1
                elif line[index] == "}":
                    depth -= 1
                    if depth == 0:
                        return source[opening:offset + index + 1]
        offset += len(line)
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
    # Stop protocol (entry tail before guest_stack_free): the sample publishes
    # busy before it loads the CPU, the stop nulls the CPU before it spins on
    # busy, both SEQ_CST, so one side always sees the other.
    sample_body = function_body(sampler, "static void kvgs_sample(")
    busy_store = "__atomic_store_n(&s_busy, 1U, __ATOMIC_SEQ_CST);"
    cpu_load = "c = __atomic_load_n(&s_cpu, __ATOMIC_SEQ_CST);"
    if sample_body.count(busy_store) != 1 or sample_body.count(cpu_load) != 1 or \
            not sample_body.index(busy_store) < sample_body.index(cpu_load):
        raise AssertionError("kvgs_sample must publish busy before it loads the CPU")
    stop = function_body(sampler, "void kage_vita_guest_sampler_stop(")
    cpu_null = "__atomic_store_n(&s_cpu, (CPU *)0, __ATOMIC_SEQ_CST);"
    busy_load = "__atomic_load_n(&s_busy, __ATOMIC_SEQ_CST)"
    if stop.count(cpu_null) != 1 or busy_load not in stop or \
            "KVGS_REPORT_SPIN" not in stop or \
            not stop.index(cpu_null) < stop.index(busy_load):
        raise AssertionError(
            "kage_vita_guest_sampler_stop must null the CPU, then spin (bounded) on busy")
    if "void kage_vita_guest_sampler_stop(void);" not in header:
        raise AssertionError("sampler header does not export the stop")

    cmake = (root / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    # Several sources gain the macro in their own 6-space blocks; the owner
    # block is the one that adds the sampler TU to the runtime sources.
    block = None
    for found in re.finditer(
            r"      if\(ISAAC_VITA_GUEST_SAMPLER\)\n(.*?)\n      endif\(\)", cmake,
            re.S):
        if ('list(APPEND ISAAC_VITA_RUNTIME_SOURCES\n'
                '          "${ISAAC_RUNTIME}/kage_vita_guest_sampler.c")') in found[1]:
            block = found
            break
    if block is None:
        raise AssertionError("ISAAC_VITA_GUEST_SAMPLER block not found in CMake")
    for owner in ("guest.c", "kage_vita_fullspeed_scheduler.c",
                  "kage_vita_guest_sampler.c", "kage_vita_phase_profile.c",
                  "entry_vita.c", "host_vita_lua.c"):
        if f'"${{ISAAC_RUNTIME}}/{owner}"' not in block[1]:
            raise AssertionError(f"sampler macro does not reach {owner}")
    # The owner block is the one that defines the census on guest.c (seam
    # helpers mirror the macro in blocks of their own).
    census = None
    for found in re.finditer(
            r"      if\(ISAAC_VITA_PROFILE_IMPORT_KINDS\)\n(.*?)\n      endif\(\)",
            cmake, re.S):
        if '"${ISAAC_RUNTIME}/guest.c"' in found[1]:
            census = found
            break
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
    verify_residual_contract(root, cmake)


def verify_residual_contract(root: Path, cmake: str) -> None:
    """ISAAC_VITA_GUEST_SAMPLER_RESIDUAL and _PROFILE_FUNCTION_ENTRY_CENSUS
    (sprite-chain residual measurement): both default OFF, refused without their parents,
    the residual define reaches the sampler TU only, the census is
    target-wide from a configure-time id header."""
    for name in ("ISAAC_VITA_GUEST_SAMPLER_RESIDUAL",
                 "ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS"):
        if not re.search(r"option\(" + name + r'\n\s+"[^"]*"\s+OFF\)', cmake):
            raise AssertionError(f"{name} is not a default-OFF option")
    for fatal in (
            "if(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL AND NOT "
            "ISAAC_VITA_GUEST_SAMPLER_CHAIN)",
            "if(ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS AND NOT "
            "ISAAC_VITA_GUEST_SAMPLER)",
            "if(ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS AND\n"
            "   ISAAC_VITA_KAGE_REFCOUNT_SEAM_VERIFY)"):
        if cmake.count(fatal) != 1:
            raise AssertionError(f"missing FATAL gate: {fatal}")
    if cmake.count("ISAAC_VITA_GUEST_SAMPLER_RESIDUAL=1") != 1:
        raise AssertionError("residual define is not set exactly once")
    residual = re.search(
        r"        if\(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL\)\n(.*?)\n        endif\(\)",
        cmake, re.S)
    if residual is None or re.findall(r'"\$\{ISAAC_RUNTIME\}/([^"]+)"',
                                      residual[1]) != ["kage_vita_guest_sampler.c",
                                                       "host_vita_lua.c"]:
        raise AssertionError(
            "residual define must reach exactly the sampler TU and the Lua bridge")
    census = re.search(
        r"  if\(ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS\)\n(.*?)\n  endif\(\)",
        cmake, re.S)
    if census is None:
        raise AssertionError("entry census block not found")
    for needle in ("vita_render_watch.py", "kage_vita_render_watch.h",
                   '--coverage "${ISAAC_GENERATED_DIR}/guest_coverage.json"',
                   "isaac-generated-overrides/entry-census",
                   "target_include_directories(isaac_first_arm_fault PRIVATE",
                   "ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS=1)",
                   "CMAKE_CONFIGURE_DEPENDS", "message(FATAL_ERROR"):
        if needle not in census[1]:
            raise AssertionError(f"entry census block lacks {needle!r}")
    if not re.search(r'CMAKE_CONFIGURE_DEPENDS[^)]*"\$\{ISAAC_GENERATED_DIR\}/guest_coverage\.json"',
                     census[1]):
        raise AssertionError("the census ids do not re-run when guest_coverage.json changes "
                             "(CMAKE_CONFIGURE_DEPENDS)")
    if cmake.count("ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS=1") != 1:
        raise AssertionError("census define escaped its target-wide block")
    guest_h = (root / "runtime" / "guest.h").read_text(encoding="utf-8")
    if guest_h.count('# include "guest_entry_census.h"') != 1:
        raise AssertionError("guest.h does not route the census hook")
    census_h = (root / "runtime" / "guest_entry_census.h").read_text(
        encoding="utf-8")
    if "#error" in census_h:
        raise AssertionError(
            "guest_entry_census.h must not test source-scoped macros "
            "(generated units never see ISAAC_VITA_GUEST_SAMPLER)")
    verify_residual_publication(root)


def verify_residual_publication(root: Path) -> None:
    """The Lua bridge publishes the guest address of every live
    VITA_LUA_CALLBACK_RETURN word right after pushing it and clears it when
    the frame ends (frame pop, CPU abort; the allocator restores the enclosing
    value), all under ISAAC_VITA_GUEST_SAMPLER_RESIDUAL; the sampler links a
    boundary word only at a published address."""
    lua = (root / "runtime" / "host_vita_lua.c").read_text(encoding="utf-8")
    fence = "#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)"
    hunks = re.findall(re.escape(fence) + r"\n(.*?)#endif", lua, re.S)
    if lua.count(fence) != 6 or len(hunks) != 6:
        raise AssertionError(f"Lua bridge residual hunks: {lua.count(fence)} (expected 6)")
    for hunk in hunks:
        if "g_kage_vita_guest_sampler_lua_" not in hunk and \
                "KAGE_VITA_GUEST_SAMPLER_LUA_CFUNCTION_SLOTS" not in hunk:
            raise AssertionError(f"unexpected Lua residual hunk: {hunk!r}")
    publish = "g_kage_vita_guest_sampler_lua_cfunction[slot] = c->esp;"
    call_guest = function_body(lua, "static int vita_lua_call_guest(")
    if call_guest.count(publish) != 1 or not (
            call_guest.index("gpush(c, VITA_LUA_CALLBACK_RETURN);") <
            call_guest.index(publish) < call_guest.index("guest_call(c, target);")):
        raise AssertionError("C-function boundary is not published between its push and guest_call")
    pop = function_body(lua, "static void vita_lua_callback_frame_pop(")
    if pop.count("g_kage_vita_guest_sampler_lua_cfunction[slot] = 0U;") != 1:
        raise AssertionError("frame pop does not clear the published boundary")
    abort = function_body(lua, "void isaac_vita_lua_abort_cpu(")
    if abort.count("g_kage_vita_guest_sampler_lua_cfunction[i] = 0U;") != 1:
        raise AssertionError("CPU abort does not clear the published boundaries")
    # the definition, not the forward declaration (`new_size);`)
    alloc = function_body(lua, "static void *vita_lua_guest_allocator(void *opaque, void *pointer,"
                               "\n                                      size_t old_size, size_t new_size)\n{")
    save = "g_kage_vita_guest_sampler_lua_allocator = c->esp;"
    restore = "g_kage_vita_guest_sampler_lua_allocator = sampler_enclosing_allocator;"
    if alloc.count(save) != 1 or alloc.count(restore) != 1 or not (
            alloc.index("gpush(c, VITA_LUA_CALLBACK_RETURN);") < alloc.index(save) <
            alloc.index("guest_call(c, allocator->target);") < alloc.index(restore) <
            alloc.index("if (c->fault || c->esp != saved_esp - 16U)")):
        raise AssertionError("allocator boundary is not published around its guest_call")
    if lua.count("VITA_LUA_CALLBACK_RETURN);") != 2:
        raise AssertionError("a third boundary push needs its own publication")
    sampler = (root / "runtime" / "kage_vita_guest_sampler.c").read_text(encoding="utf-8")
    chain = function_body(sampler, "static void kvgs_chain(")
    if chain.count("kvgs_lua_live(address)") != 1 or "++buffer->lua_stale;" not in chain:
        raise AssertionError("the chain walk does not test boundary liveness")
    sample = function_body(sampler, "static void kvgs_sample(")
    if sample.count("kvgs_lua_live(address)") != 1 or "lua_below = 1;" not in sample:
        raise AssertionError("the innermost scan does not test boundary liveness")



def coverage_fixture(directory: Path, vita: Path, stale: bool) -> None:
    """A synthetic corpus id space for the census oracle: the watch list plus
    the oracle's own functions and an unwatched id 0, the ids header generated
    by vita_render_watch.py (as CMake does), and a guest_coverage_generated.h
    whose sorted-RVA hash matches it or, stale, does not."""
    sys.path.insert(0, str(vita))
    try:
        import vita_render_watch as watch
    finally:
        sys.path.pop(0)
    rvas = set(watch.read_watch(vita.parent / "runtime" /
                                "kage_vita_render_watch.h"))
    rvas |= {0x00001000, 0x000023f0, 0x002dbe30, 0x002dbe40, 0x004b0010,
             0x005a0de0, 0x00100000}
    ordered = sorted(rvas)
    digest = watch.rows_sha256(ordered)
    coverage = {
        "function_count": len(ordered),
        "function_rvas_sha256": digest,
        "generation_id": "gen:sampler-oracle",
        "functions": [{"id": i, "rva": rva} for i, rva in enumerate(ordered)],
    }
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "guest_coverage.json").write_text(
        __import__("json").dumps(coverage), encoding="utf-8")
    run([sys.executable, str(vita / "vita_render_watch.py"),
         "--watch", str(vita.parent / "runtime" / "kage_vita_render_watch.h"),
         "--coverage", str(directory / "guest_coverage.json"),
         "--output", str(directory / "guest_entry_census_ids.h")])
    corpus = "f" * 64 if stale else digest
    (directory / "guest_coverage_generated.h").write_text(
        "#ifndef GUEST_COVERAGE_GENERATED_H\n"
        "#define GUEST_COVERAGE_GENERATED_H\n"
        f"#define GUEST_COVERAGE_FUNCTION_COUNT {len(ordered)}U\n"
        f'#define GUEST_COVERAGE_FUNCTION_RVAS_SHA256 "{corpus}"\n'
        "#endif\n", encoding="utf-8", newline="\n")


FROZEN_RECORDS = ("ph120.hotr", "ph120.hotu", "ph120.hotw", "ph120.hotl",
                  "ph120.hcl")


def record_items(lines, window: int) -> dict:
    """(record, phase) -> the window's header lines plus list items (top=/w=/o=),
    so a record split over several lines compares with the same record on one."""
    out: dict = {}
    for line in lines:
        m = re.search(r"\] (ph120\.\w+) bid=sampler:oracle win=(\d+) ", line)
        if not m or m.group(1) not in FROZEN_RECORDS or int(m.group(2)) != window:
            continue
        phase = re.search(r"\bphase=(\S+)", line)
        items = out.setdefault((m.group(1), phase.group(1) if phase else ""), set())
        listed = re.search(r" (?:top|w|o)=(\S*)$", line)
        if listed:
            items.add("header " + line[:listed.start()].split("] ", 1)[1])
            items.update(filter(None, listed.group(1).split(",")))
        else:
            items.add("line " + line.split("] ", 1)[1])
    return out


def check_bench_frozen_lines(repo: Path, residual_stdout: str) -> None:
    """tools/bench/bench_run_test.py feeds the analyzer frozen copies
    of scenario 8 (window 10) and 8c (window 11).  Their hotr/hotu/hotw/hotl/hcl
    records must be this oracle's, item for item, or the analyzer test runs on
    stale lines (a watch-list change moved hcl watch= from 52 to 64).  The bench
    test's ph120.fe lines are hand-made: the oracle's fe records are scenario 9's."""
    spec = importlib.util.spec_from_file_location(
        "bench_run_test_frozen", repo / "tools" / "bench" / "bench_run_test.py")
    bench = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bench)
    oracle = residual_stdout.splitlines()
    for name, window in (("RESIDUAL_LINES", 10), ("RESIDUAL_STALE_LINES", 11)):
        frozen = record_items(getattr(bench, name), window)
        live = record_items(oracle, window)
        if not frozen or frozen != live:
            diff = {k: (sorted(frozen.get(k, set()) - live.get(k, set())),
                        sorted(live.get(k, set()) - frozen.get(k, set())))
                    for k in sorted(set(frozen) | set(live))
                    if frozen.get(k) != live.get(k)}
            raise AssertionError(
                f"bench_run_test.py {name} is not the oracle's window {window} "
                f"(frozen-only, oracle-only): {diff}")
    print("bench_run_test.py frozen scenario 8/8c lines = oracle windows 10/11")


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
        # ISAAC_VITA_GUEST_SAMPLER_CHAIN: same scenarios (self/caller/hs
        # records unchanged) plus scenario 6, the inclusive chain view.
        chain_exe = Path(value) / ("sampler-chain.exe" if os.name == "nt"
                                   else "sampler-chain")
        run([
            cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-DGUEST_IMAGE_BASE=0x98000000u",
            "-DISAAC_KAGE_VITA_GUEST_SAMPLER_ORACLE=1",
            "-DISAAC_VITA_GUEST_SAMPLER=1",
            "-DISAAC_VITA_GUEST_SAMPLER_CHAIN=1",
            "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
            "-DISAAC_VITA_PROFILE_IMPORT_KINDS=1",
            '-DISAAC_VITA_FULLSPEED_SCHEDULER_BUILD_ID="sampler:oracle"',
            f"-I{runtime}", f"-I{vita}",
            str(runtime / "kage_vita_guest_sampler.c"),
            str(runtime / "kage_vita_fullspeed_scheduler.c"),
            str(runtime / "kage_vita_guest_sampler_oracle.c"),
            "-o", str(chain_exe),
        ])
        chain_stdout = run([str(chain_exe)])
        # ISAAC_VITA_GUEST_SAMPLER_PRESENT_LABEL (profile-only): swp samples
        # are the native present, named "present" without a stack scan.
        present_exe = Path(value) / ("sampler-present.exe" if os.name == "nt"
                                     else "sampler-present")
        run([
            cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-DGUEST_IMAGE_BASE=0x98000000u",
            "-DISAAC_KAGE_VITA_GUEST_SAMPLER_ORACLE=1",
            "-DISAAC_VITA_GUEST_SAMPLER=1",
            "-DISAAC_VITA_GUEST_SAMPLER_PRESENT_LABEL=1",
            "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
            "-DISAAC_VITA_PROFILE_IMPORT_KINDS=1",
            '-DISAAC_VITA_FULLSPEED_SCHEDULER_BUILD_ID="sampler:oracle"',
            f"-I{runtime}", f"-I{vita}",
            str(runtime / "kage_vita_guest_sampler.c"),
            str(runtime / "kage_vita_fullspeed_scheduler.c"),
            str(runtime / "kage_vita_guest_sampler_oracle.c"),
            "-o", str(present_exe),
        ])
        present_stdout = run([str(present_exe)])
        # ISAAC_VITA_GUEST_SAMPLER_RESIDUAL + _PROFILE_FUNCTION_ENTRY_CENSUS
        # (profile-only): scenarios 1-6 unchanged, 8 (residual records), 9
        # (ph120.fe through guest.h's hook and a generated id header).  Then
        # the census alone against a stale id space.
        flavours = {}
        for name, stale, defines in (
                ("residual", False, ["-DISAAC_VITA_GUEST_SAMPLER_CHAIN=1",
                                     "-DISAAC_VITA_GUEST_SAMPLER_RESIDUAL=1",
                                     "-DISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS=1"]),
                ("census-stale", True, ["-DISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS=1",
                                        "-DORACLE_CENSUS_STALE=1"])):
            fixture = Path(value) / f"{name}-ids"
            coverage_fixture(fixture, vita, stale)
            flavour_exe = Path(value) / (f"sampler-{name}.exe" if os.name == "nt"
                                         else f"sampler-{name}")
            run([
                cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-DGUEST_IMAGE_BASE=0x98000000u",
                "-DISAAC_KAGE_VITA_GUEST_SAMPLER_ORACLE=1",
                "-DISAAC_VITA_GUEST_SAMPLER=1",
                *defines,
                "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
                "-DISAAC_VITA_PROFILE_IMPORT_KINDS=1",
                '-DISAAC_VITA_FULLSPEED_SCHEDULER_BUILD_ID="sampler:oracle"',
                f"-I{runtime}", f"-I{vita}", f"-I{fixture}",
                str(runtime / "kage_vita_guest_sampler.c"),
                str(runtime / "kage_vita_fullspeed_scheduler.c"),
                str(runtime / "kage_vita_guest_sampler_oracle.c"),
                "-o", str(flavour_exe),
            ])
            flavours[name] = run([str(flavour_exe)])
    residual_stdout = flavours["residual"]
    for needle in ("scenario 6: chain view", "scenario 8: residual",
                   "scenario 8b:", "scenario 8c: stale boundary words",
                   "ph120.hotr bid=sampler:oracle win=11 loops=1320 phase=rnd "
                   "samples=4 top=48bc50:2,800000:1,1c0eef0:1\n",
                   "ph120.hcl bid=sampler:oracle win=11 lb=2 lbm=0 lbs=0 lbx=3 ",
                   "scenario 9: entry census per phase",
                   "Vita guest sampler host oracle: PASS"):
        if needle not in residual_stdout:
            raise AssertionError(f"residual oracle lacks {needle!r}:\n{residual_stdout}")
    check_bench_frozen_lines(root.parent, residual_stdout)
    for needle in ("scenario 9: stale id space prints ids=STALE only",
                   "Vita guest sampler host oracle: PASS"):
        if needle not in flavours["census-stale"]:
            raise AssertionError(
                f"stale census oracle lacks {needle!r}:\n{flavours['census-stale']}")
    for record in ("ph120.hotw", "ph120.hotl", "ph120.hotu", "ph120.hcl",
                   "ph120.fe"):
        for flavour, text in (("default", stdout), ("chain", chain_stdout),
                              ("present", present_stdout)):
            if record in text:
                raise AssertionError(f"{record} leaks into the {flavour} sampler")
    if "ph120.hotw" in flavours["census-stale"]:
        raise AssertionError("residual records leak into the census-only sampler")
    for needle in ("phase=swp samples=3 top=present:3", "scenario 7: swp samples named present",
                   "Vita guest sampler host oracle: PASS"):
        if needle not in present_stdout:
            raise AssertionError(f"present-label oracle lacks {needle!r}:\n{present_stdout}")
    if "present:" in stdout or "present:" in chain_stdout:
        raise AssertionError("present label leaks into the default sampler")
    for needle in ("ph120.hci bid=sampler:oracle win=8 n=8 links=11 ind=1 "
                   "thunk=2 skip=2 full=0 indrej=2 us=8,1",
                   "scenario 6: chain view",
                   "Vita guest sampler host oracle: PASS"):
        if needle not in chain_stdout:
            raise AssertionError(f"chain oracle lacks {needle!r}:\n{chain_stdout}")
    if "ph120.hoti" in stdout or "ph120.hci" in stdout:
        raise AssertionError("chain records leak into the default sampler")
    for needle in (
        "phase=rnd samples=3 top=5a0de0:3",
        "phase=rnd samples=7 top=ext:4,562e00:2,ind:1",
        "phase=upd samples=1 top=23f0:1",
        "samples(svc,upd,rnd,swp,lim,oth,pace)=0,1,0,0,0,0,0 total=1 "
        "nomark=0 badesp=0 full=0 words=201 deep=1",
        "total=0 nomark=1 badesp=0 full=0 words=256 deep=0",
        "phase=pace samples=",
        "win=7 loops=840 phase=rnd samples=1 top=4b0010:1",
        "scenario 5: stop unbinds the CPU: one sample before it, no stack "
        "read and no sample (not even badesp) after it, report still prints",
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
