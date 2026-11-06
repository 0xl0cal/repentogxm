#!/usr/bin/env python3
"""Wiring and OFF-identity gate for ISAAC_VITA_GUEST_STACK_MEMBLOCK.

The knob backs the 4 MiB guest stack with a dedicated USER_RW memblock
(request 0x401000, name isaac_guest_stack) instead of newlib memalign.  This
gate pins, without a compiler:

1. fences: every occurrence of the macro in guest.c and entry_vita.c sits
   inside a canonical `#if defined(ISAAC_VITA_GUEST_STACK_MEMBLOCK)` fence
   (nesting inside `#ifdef __vita__` allowed);
2. OFF identity at text level: evaluating only those fences with the macro
   undefined (keep the #else arm, drop the fenced arm and the directives)
   reproduces the pre-knob guest.c / entry_vita.c byte for byte (pinned
   sha256 digests, no git history needed), and guest.h is untouched;
3. the header constants (0x401000 request with a 4 KiB low bit, 0x402000
   retained, the block name) and the allocator-free memblock unit;
4. CMake wiring (option default OFF, source and define scoped to exactly
   guest.c and entry_vita.c inside if(<knob>), SCAFFOLD_ONLY guard, knob
   absent from release/v0.1.1-alpha.cmake);
5. raw allocator gate wiring (cache key, definition owners, the three
   OFF-only newlib rows, optional source, compile-scope verifier);
6. the entry receipts and the frozen final "guest stack:" line order.

usage: python recomp/test_guest_stack_memblock.py
"""

from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
RUNTIME = HERE / "runtime"
VITA = HERE / "vita"
MACRO = "ISAAC_VITA_GUEST_STACK_MEMBLOCK"
FENCE = f"#if defined({MACRO})"
# Fences of the lanes that share entry_vita.c in integration trees (game log
# write-behind, runtime settings, trophy Stage A).  Evaluated OFF exactly like
# the knob's own fences but not counted, so the pinned pre-knob digest holds
# there as well.
FOREIGN_FENCES = (
    "#if defined(ISAAC_VITA_GAME_LOG_ASYNC)",
    "#if defined(ISAAC_VITA_RUNTIME_SETTINGS)",
    "#if defined(ISAAC_VITA_TROPHIES)",
)
# sha256 of guest.c / entry_vita.c with the knob's fences evaluated OFF, and
# of guest.h as is.  guest.c and guest.h: the LF text at
# e2389e42c8398bdfac10f28ead55e58621c4b72b, the commit before the knob.
# entry_vita.c: that text plus the entry tail's sampler/beat stop calls
# (kage_vita_guest_sampler_stop / isaac_vita_lua_beat_stop, fenced by
# ISAAC_VITA_GUEST_SAMPLER / ISAAC_VITA_LUA_RECEIPT, both OFF in the default
# build), pinned below by check_entry.  The pin is history-free (holds in a
# one-commit checkout).  Change a digest only together with an intentional
# edit of that file (the failure message prints the new value) and re-prove
# OFF identity on an ARM build: the preprocessed default-path text of the file
# must be unchanged by the knob's fences.
OFF_TEXT_SHA256 = {
    "recomp/runtime/guest.c":
        "d5dc42fcf96b83d03e6caa120b65e14da184506393498a93ce00949caa63fbea",
    "recomp/runtime/entry_vita.c":
        "e9a939aecc3632c39cfac342abe64e3b251dd88a68daf3c375212b4c56a1a44d",
    "recomp/runtime/guest.h":
        "bb7c21ecb30604deaa4ac218e4b2d9e23d3ec33f55397d32a18f8f3c223a71d8",
}
GUEST_FENCES = 9      # include, globals, alloc, wrap free, bind free,
                      # free flag, free arm, free release, accessor
ENTRY_FENCES = 3      # include, failure receipt, ready receipt

IF_RE = re.compile(r"^\s*#\s*(?:if|ifdef|ifndef)\b")
ELIF_RE = re.compile(r"^\s*#\s*elif\b")
ELSE_RE = re.compile(r"^\s*#\s*else\b")
ENDIF_RE = re.compile(r"^\s*#\s*endif\b")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read(path: Path) -> str:
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def evaluate_off(text: str, name: str) -> tuple[str, int]:
    """Drop every canonical knob arm, keep its #else arm and every other
    directive verbatim; reject the macro anywhere outside such a fence."""
    out: list[str] = []
    stack: list[list[object]] = []
    fences = 0

    def emitting() -> bool:
        return all(bool(frame[1]) for frame in stack if frame[0] == "knob")

    for lineno, line in enumerate(text.split("\n"), 1):
        where = f"{name}:{lineno}"
        stripped = line.strip()
        if stripped == FENCE:
            stack.append(["knob", False])
            fences += 1
            continue
        if stripped in FOREIGN_FENCES:
            stack.append(["knob", False])
            continue
        if IF_RE.match(line):
            require(MACRO not in line,
                    f"{where}: non-canonical knob fence: {stripped}")
            stack.append(["other", True])
            if emitting():
                out.append(line)
            continue
        if ELIF_RE.match(line):
            require(bool(stack), f"{where}: #elif without #if")
            require(stack[-1][0] == "other", f"{where}: #elif on the knob fence")
            require(MACRO not in line, f"{where}: knob macro in #elif")
            if emitting():
                out.append(line)
            continue
        if ELSE_RE.match(line):
            require(bool(stack), f"{where}: #else without #if")
            if stack[-1][0] == "knob":
                require(stack[-1][1] is False,
                        f"{where}: second #else on the knob fence")
                stack[-1][1] = True
                continue
            if emitting():
                out.append(line)
            continue
        if ENDIF_RE.match(line):
            require(bool(stack), f"{where}: #endif without #if")
            top = stack.pop()
            if top[0] == "knob":
                continue
            if emitting():
                out.append(line)
            continue
        if MACRO in line:
            require(any(frame[0] == "knob" for frame in stack),
                    f"{where}: {MACRO} outside its fence: {stripped}")
        if emitting():
            out.append(line)
    require(not stack, f"{name}: unbalanced preprocessor directives")
    return "\n".join(out), fences


def check_off_identity() -> dict[str, str]:
    digests: dict[str, str] = {}
    for relative, expected_fences in (
        ("recomp/runtime/guest.c", GUEST_FENCES),
        ("recomp/runtime/entry_vita.c", ENTRY_FENCES),
    ):
        current = read(ROOT / relative)
        require(MACRO in current, f"{relative}: knob not wired")
        off, fences = evaluate_off(current, relative)
        require(fences == expected_fences,
                f"{relative}: {fences} knob fences, expected {expected_fences}")
        digest = sha256(off)
        require(digest == OFF_TEXT_SHA256[relative],
                f"{relative}: OFF-evaluated text differs from the pinned "
                f"pre-knob text (sha256 off={digest} "
                f"pinned={OFF_TEXT_SHA256[relative]})")
        digests[relative] = digest
    # guest.h is included by every generated unit: it must not have moved.
    header = read(RUNTIME / "guest.h")
    digest = sha256(header)
    require(digest == OFF_TEXT_SHA256["recomp/runtime/guest.h"],
            f"guest.h differs from the pinned pre-knob text (sha256 {digest} "
            f"pinned={OFF_TEXT_SHA256['recomp/runtime/guest.h']})")
    require(MACRO not in header, "guest.h must not know the knob")
    require("guest_stack_memblock" not in header,
            "guest.h must not declare the memblock accessor")
    digests["recomp/runtime/guest.h"] = digest
    return digests


def check_header_and_unit() -> None:
    header = read(RUNTIME / "guest_stack_memblock.h")
    for line in (
        "#define GUEST_STACK_MEMBLOCK_PAGE_BYTES      0x00001000U",
        "#define GUEST_STACK_MEMBLOCK_REQUEST_BYTES   0x00401000U",
        "#define GUEST_STACK_MEMBLOCK_RETAINED_BYTES  0x00402000U",
        '#define GUEST_STACK_MEMBLOCK_NAME            "isaac_guest_stack"',
        "int guest_stack_memblock_acquire(size_t usable_bytes,",
        "int guest_stack_memblock_release(const guest_stack_memblock_snapshot *s);",
        "const char *guest_stack_memblock_status_text(uint32_t status);",
        "int guest_stack_memblock_snapshot_get(guest_stack_memblock_snapshot *out);",
    ):
        require(line in header, f"guest_stack_memblock.h: missing {line!r}")
    require("#include <psp2" not in header and "Sce" not in header,
            "guest_stack_memblock.h must stay psp2-free")
    request = 0x00401000
    require(request == 4 * 1024 * 1024 + 0x1000, "request is stack + one page")
    require(request & -request == 0x1000, "request low set bit must be 4 KiB")
    require(0x00402000 == request + 0x1000, "retained is request + one quantum")

    unit = read(RUNTIME / "guest_stack_memblock.c")
    require("#include <psp2/kernel/sysmem.h>" in unit, "unit includes sysmem")
    require("SCE_KERNEL_MEMBLOCK_TYPE_USER_RW" in unit, "USER_RW block type")
    require(unit.count("sceKernelAllocMemBlock(") == 1, "one alloc call")
    require(unit.count("sceKernelGetMemBlockBase(") == 1, "one get-base call")
    require(unit.count("sceKernelFreeMemBlock(") == 2,
            "rollback free + release free")
    forbidden = re.compile(
        r"\b(malloc|calloc|realloc|free|memalign|aligned_alloc|strdup|memset)"
        r"\s*\("
    )
    require(forbidden.search(unit) is None,
            "guest_stack_memblock.c must not call an allocator or memset")
    require("isaac_vita_log" not in unit, "the unit does not log")
    for status in ("alloc-failed", "get-base-failed", "invalid-base", "wrap",
                   "orphaned", "ready", "none"):
        require(f'"{status}"' in unit, f"status text {status!r}")

    oracle = read(RUNTIME / "guest_stack_memblock_oracle.c")
    require("#ifndef GUEST_STACK_MEMBLOCK_LINK_PROBE" in oracle,
            "oracle link-probe fence")
    require('puts("guest stack memblock oracle: PASS");' in oracle,
            "oracle PASS line")
    # The integration oracle executes guest.c's stack owners compiled ON on
    # the host: it must include guest.h, define the three sysmem fakes once
    # each and drive both release sites and both fail-closed failures.
    integration = read(RUNTIME / "guest_stack_memblock_integration_oracle.c")
    require('#include "guest.h"' in integration and
            '#include "guest_stack_memblock.h"' in integration,
            "integration oracle includes guest.h and the memblock header")
    for fake in ("SceUID sceKernelAllocMemBlock(",
                 "int sceKernelGetMemBlockBase(",
                 "int sceKernelFreeMemBlock("):
        require(integration.count(fake) == 1,
                f"integration oracle defines {fake!r} once")
    for needle in (
        "CHECK(guest_stack_init(&cpu) == 0);",
        "guest_stack_free(&cpu);",
        "s_fail_alloc = 1;",
        "s_fail_get = 1;",
        "s_high_base = 1;",
        "cpu.stack_low_water = 1U;",
        "s_reader_cpu = &cpu;",
        "    if (reader_guard_admits(s_reader_cpu))\n"
        "        ++s_frees_under_live_guard;",
        "CHECK(s_frees_under_live_guard == 0U);",
        # case 9: the production sampler (oracle mode) bound, stopped the way
        # the entry tail stops it, then the free; no read after either.
        "kage_vita_guest_sampler_oracle_bind(&cpu, NULL, 0U, 0U);",
        "    kage_vita_guest_sampler_stop();\n"
        "    CHECK(strcmp(s_sampler_log,\n"
        '                 "KAGE VITA GUEST SAMPLER: stopped ticks=1 busy_spins=0") == 0);\n'
        "    kage_vita_guest_sampler_oracle_sample();\n"
        "    CHECK(s_sampler_reads == kage_vita_guest_sampler_oracle_scan_words());\n"
        "    CHECK(reader_guard_admits(&cpu));\n"
        "    guest_stack_free(&cpu);\n",
        "CHECK(s_sampler_reads_dead == 0U);",
        'puts("guest stack memblock integration oracle: PASS");',
    ):
        require(needle in integration, f"integration oracle: missing {needle!r}")
    require(integration.count("kage_vita_guest_sampler_stop();") == 1,
            "integration oracle stops the sampler once, before the free")
    require(integration.count("#if defined(ISAAC_VITA_GUEST_SAMPLER)") == 3,
            "sampler accessors, fakes and case 9 are fenced (3 fences)")
    for path in (RUNTIME / "guest_stack_memblock.h",
                 RUNTIME / "guest_stack_memblock.c",
                 RUNTIME / "guest_stack_memblock_oracle.c",
                 RUNTIME / "guest_stack_memblock_integration_oracle.c",
                 VITA / "test_guest_stack_memblock.sh"):
        raw = path.read_bytes()
        require(b"\r" not in raw and b"\0" not in raw,
                f"{path.name}: must be LF text without NUL")


def check_cmake() -> None:
    cmake = read(VITA / "CMakeLists.txt")
    require(re.search(
        r'option\(ISAAC_VITA_GUEST_STACK_MEMBLOCK\s+"[^"]*"\s+OFF\)', cmake)
        is not None, "ISAAC_VITA_GUEST_STACK_MEMBLOCK must default to OFF")
    require(cmake.count("guest_stack_memblock.c") == 1,
            "guest_stack_memblock.c must be wired exactly once")
    wiring = cmake.index('"${ISAAC_RUNTIME}/guest_stack_memblock.c")')
    opening = cmake.rfind("\n  if(", 0, wiring)
    require(opening >= 0 and
            cmake[opening:].startswith(f"\n  if({MACRO})\n"),
            "memblock source wiring is not scoped by if(<knob>)")
    require(cmake.count(f"{MACRO}=1") == 1,
            "the define must be emitted exactly once")
    define = cmake.index(f"{MACRO}=1")
    block = cmake[cmake.rfind("set_property(SOURCE", 0, define):define]
    sources = re.findall(r'"\$\{ISAAC_RUNTIME\}/([a-z_]+\.c)"', block)
    require(sources == ["guest.c", "entry_vita.c"],
            f"define scope must be exactly guest.c + entry_vita.c: {sources}")
    require(f"if({MACRO} AND ISAAC_VITA_SCAFFOLD_ONLY)" in cmake,
            "SCAFFOLD_ONLY guard")
    require(f"{MACRO} requires the translated runtime" in cmake,
            "SCAFFOLD_ONLY guard message")
    # ISAAC_VITA_LUA_RECEIPT reaches entry_vita.c so the tail can stop the
    # beat thread's guest stack sampling before guest_stack_free.
    receipt = cmake.index("APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_LUA_RECEIPT=1)")
    block = cmake[cmake.rfind("set_property(SOURCE", 0, receipt):receipt]
    owners = re.findall(r'"\$\{ISAAC_RUNTIME\}/([a-z_]+\.c)"', block)
    require(owners == ["host_vita_lua.c", "host_vita_filesystem.c",
                       "vita_sync_services.c", "entry_vita.c"],
            f"ISAAC_VITA_LUA_RECEIPT owners drifted: {owners}")
    release = read(ROOT / "release" / "v0.1.1-alpha.cmake")
    require(MACRO not in release,
            "release knob set must not mention the guest stack memblock")


def check_gate() -> None:
    gate = read(VITA / "vita_raw_allocator_gate.py")
    require(f'GUEST_STACK_MEMBLOCK_CACHE_KEY = "{MACRO}"' in gate, "cache key")
    require('GUEST_STACK_MEMBLOCK_DEFINITION_OWNERS: frozenset[str] = frozenset(\n'
            '    ("guest.c", "entry_vita.c")\n)' in gate, "definition owners")
    rows = gate[gate.index("GUEST_STACK_NEWLIB_RELOCATIONS = _counter("):]
    rows = rows[:rows.index(")\n)") + 3]
    for row in (
        '(1, "guest_stack_free", "free", "R_ARM_THM_CALL")',
        '(1, "guest_stack_init", "free", "R_ARM_THM_CALL")',
        '(1, "guest_stack_init", "memalign", "R_ARM_THM_CALL")',
    ):
        require(row in rows, f"OFF-only row {row} missing")
    fixed = gate[gate.index('EXPECTED_FIXED: Mapping'):]
    fixed = fixed[:fixed.index('"host_vita_openal_pool.c"')]
    for row in (
        '(1, "guest_stack_free", "free", "R_ARM_THM_CALL")',
        '(1, "guest_stack_init", "free", "R_ARM_THM_CALL")',
        '(1, "guest_stack_init", "memalign", "R_ARM_THM_CALL")',
    ):
        require(row in fixed, f"EXPECTED_FIXED guest.c lost {row}")
    require("expected -= GUEST_STACK_NEWLIB_RELOCATIONS" in gate,
            "ON subtraction in expected_relocations")
    require('(GUEST_STACK_MEMBLOCK_CACHE_KEY, "guest_stack_memblock.c"),' in gate,
            "optional source")
    require("def verify_guest_stack_memblock_compile_scope(" in gate,
            "compile-scope verifier")
    require("    verify_guest_stack_memblock_compile_scope(records, cache)\n" in gate,
            "compile-scope verifier is called")
    require("GUEST_STACK_MEMBLOCK_DEFINITION_OWNERS if active else frozenset()"
            in gate, "verifier expects the owners only when ON")


def check_entry() -> None:
    entry = read(RUNTIME / "entry_vita.c")
    ready = ('"guest stack reserve: status=ready path=memblock uid=0x%08x "\n'
             '                "base=0x%08x request=%u vita3k_retained=%u floor=%08x "\n'
             '                "ceiling=%08x capacity=%u"')
    failed = ('"guest stack reserve: status=%s path=memblock "\n'
              '                    "uid=0x%08x get=0x%08x base=0x%08x rollback=0x%08x"')
    require(ready in entry, "ready receipt literal")
    require(failed in entry, "failure receipt literal")
    require('"guest stack: floor=%08x ceiling=%08x low=%08x "' in entry,
            "frozen final guest stack line")
    require('"guest setup FAILED: 4 MiB guest stack allocation"' in entry,
            "frozen failure line")
    init = entry.index("guest_stack_init(&cpu)")
    failure_receipt = entry.index(failed)
    failure_line = entry.index('"guest setup FAILED: 4 MiB guest stack allocation"')
    ready_receipt = entry.index(ready)
    heap_init = entry.index("isaac_vita_guest_heap_init(")
    final_line = entry.index('"guest stack: floor=%08x')
    stack_free = entry.index("guest_stack_free(&cpu)")
    require(init < failure_receipt < failure_line < ready_receipt < heap_init,
            "receipt order: init < failure receipt < FAILED < ready < heap init")
    require(heap_init < final_line < stack_free,
            "final guest stack line stays before guest_stack_free")
    require(entry.count("guest_stack_memblock_snapshot_get(") == 2,
            "two snapshot reads (failure + ready)")
    require(entry.count("guest_stack_free(&cpu)") == 1,
            "no teardown receipt: guest_stack_free stays a single call")
    # Cross-core readers of the guest stack (sampler thread, Lua beat thread)
    # are unbound in the tail after the coverage shutdown and before the
    # stack is released; each call sits in its own knob fence, so the default
    # build (both knobs OFF) keeps the pre-knob tail.
    sampler_stop = "    kage_vita_guest_sampler_stop();\n"
    beat_stop = "    isaac_vita_lua_beat_stop();\n"
    require(entry.count(sampler_stop) == 1 and entry.count(beat_stop) == 1,
            "one sampler stop and one beat stop in the entry tail")
    coverage = entry.rindex("    guest_coverage_shutdown();\n")
    require(coverage < entry.index(sampler_stop) < entry.index(beat_stop) <
            final_line < stack_free,
            "tail order: coverage shutdown < sampler stop < beat stop < "
            "final guest stack line < guest_stack_free")
    require(entry[coverage:entry.index(sampler_stop)].count(
                "#if defined(ISAAC_VITA_GUEST_SAMPLER)\n") == 1,
            "sampler stop fenced by ISAAC_VITA_GUEST_SAMPLER")
    require(entry.count(
                "#if ISAAC_VITA_KAGE && defined(ISAAC_VITA_LUA) && "
                "defined(ISAAC_VITA_LUA_RECEIPT)\n" + beat_stop + "#endif\n") == 1,
            "beat stop fenced by KAGE + LUA + LUA_RECEIPT")

    guest = read(RUNTIME / "guest.c")
    require("guest_stack_memblock_acquire(GUEST_STACK_SIZE, &g_stack_memblock)"
            in guest, "acquire with GUEST_STACK_SIZE")
    require(guest.count("guest_stack_memblock_release(&g_stack_memblock)") == 3,
            "release at the two init failure sites and in guest_stack_free")
    require("memalign(16U, GUEST_STACK_SIZE)" in guest,
            "OFF arm keeps the memalign path")
    require("static guest_stack_memblock_snapshot g_stack_memblock;" in guest,
            "static snapshot in guest.c")
    # guest_stack_free publishes the unbinding (CPU floor/ceiling/esp and the
    # g_stack_* words) before the memblock release, behind a full fence: a
    # cross-core reader re-checking its guard after the unmap sees floor 0.
    free_body = guest[guest.index("void guest_stack_free(CPU *c)\n"):
                      guest.index("int guest_stack_memblock_snapshot_get(")]
    require(free_body.count("release_memblock = 1;") == 1 and
            free_body.count("if (release_memblock) {") == 1,
            "guest_stack_free defers the memblock release behind a flag")
    require(free_body.index("release_memblock = 1;") <
            free_body.index("g_stack_mem = NULL;") <
            free_body.index("c->stack_floor = 0U;") <
            free_body.index("c->stack_ceiling = 0U;") <
            free_body.index("__atomic_thread_fence(__ATOMIC_SEQ_CST);") <
            free_body.rindex("guest_stack_memblock_release(&g_stack_memblock)"),
            "guest_stack_free: unbinding, fence, then the memblock release")
    require("_Static_assert(GUEST_STACK_MEMBLOCK_REQUEST_BYTES ==\n"
            "                   GUEST_STACK_SIZE + GUEST_STACK_MEMBLOCK_PAGE_BYTES,"
            in guest, "request static assert")
    # The png row-batch fixture slices guest.c from '#if defined(_MSC_VER)' to
    # the first '#ifdef __vita__': the include fence must come after that.
    require(guest.index("#ifdef __vita__") < guest.index(FENCE),
            "knob include fence must stay inside the first #ifdef __vita__ block")

    readme = read(VITA / "README.md")
    require(f"`{MACRO}` (default OFF" in readme, "README knob entry")
    script = read(VITA / "test_guest_stack_memblock.sh")
    require(script.startswith("#!/usr/bin/env bash\n") and "set -eu\n" in script,
            "test script header")
    require('if [ -z "${VITASDK:-}" ]; then' in script,
            "ARM part is VITASDK-conditional")
    # The ON owner integration and the entry_vita.c receipt pins run on the
    # host before the VITASDK gate (mandatory); the ARM part repeats the
    # entry_vita.c OFF/ON compile with the teardown gate's production flags.
    gate = script.index('if [ -z "${VITASDK:-}" ]; then')
    for needle in (
        '-c "$root/runtime/guest.c" -o "$work/guest.on.host.o"',
        '-c "$root/runtime/guest_stack_memblock_integration_oracle.c"',
        '-Wl,--gc-sections -o "$work/guest-stack-memblock-integration"',
        "grep -q '^guest stack memblock integration oracle: PASS$'",
        "capacity=4186112 uid=0x00004242 base=0x[0-9a-f]{8} request=4198400$'",
        "status=alloc-failed uid=0xfffffeef get=0x00000000 base=0x00000000 "
        "rollback=0x00000000$'",
        "status=get-base-failed uid=0x00004242 get=0xfffffdef base=0x00000000 "
        "rollback=0x00000000$'",
        "'^ON bind failure: released by guest_stack_init, status=none$'",
        # the production sampler in oracle mode is linked into the integration
        # oracle; stop receipt, publish-before-unmap receipt and read census
        "-DISAAC_KAGE_VITA_GUEST_SAMPLER_ORACLE=1 -DISAAC_VITA_GUEST_SAMPLER=1",
        '-c "$root/runtime/kage_vita_guest_sampler.c"',
        "grep -q '^ON free: unbinding published before sceKernelFreeMemBlock "
        "(reader guard dead at the unmap)$'",
        "grep -q '^ON log: KAGE VITA GUEST SAMPLER: stopped ticks=1 busy_spins=0$'",
        "grep -q '^ON sampler: 256 words read while bound, 0 after "
        "kage_vita_guest_sampler_stop, 0 after guest_stack_free$'",
        '-c "$root/runtime/entry_vita.c" -o "$work/entry_vita.on.host.o"',
        "check_entry_receipts host readelf strings",
        # entry tail order pins: stop relocations precede guest_stack_free
        'entry_sampler_flags="-DISAAC_VITA_GUEST_STACK_MEMBLOCK=1 '
        '-DISAAC_VITA_GUEST_SAMPLER=1"',
        'entry_beat_flags="-DISAAC_VITA_GUEST_STACK_MEMBLOCK=1 -DISAAC_VITA_LUA=1 '
        '-DISAAC_VITA_LUA_RECEIPT=1"',
        'check_entry_no_stops host readelf "$work/entry_vita.off.host.o"',
        'check_entry_no_stops host readelf "$work/entry_vita.on.host.o"',
        'check_entry_stop_order host readelf "$work/entry_vita.on-sampler.host.o" '
        'kage_vita_guest_sampler_stop',
        'check_entry_stop_order host readelf "$work/entry_vita.on-beat.host.o" '
        'isaac_vita_lua_beat_stop',
    ):
        require(needle in script[:gate],
                f"host stanza before the VITASDK gate: missing {needle!r}")
    ready_line = ready.replace('"\n                "', "").strip('"')
    failed_line = failed.replace('"\n                    "', "").strip('"')
    require(f"ready_receipt='{ready_line}'" in script,
            "script pins the ready receipt literal of entry_vita.c")
    require(f"failure_receipt='{failed_line}'" in script,
            "script pins the failure receipt literal of entry_vita.c")
    require('if [ "$on_logs" -ne $((off_logs + 2)) ]; then' in script,
            "isaac_vita_log relocations: ON == OFF + 2")
    for needle in (
        "-DISAAC_VITA_KAGE=1 -DISAAC_VITA_AUDIO=0",
        '-c "$root/runtime/entry_vita.c" -o "$work/entry_vita.on.o"',
        'check_entry_receipts ARM "$readelf" "$strings"',
        'check_entry_no_stops ARM "$readelf" "$work/entry_vita.off.o"',
        'check_entry_no_stops ARM "$readelf" "$work/entry_vita.on.o"',
        'check_entry_stop_order ARM "$readelf" "$work/entry_vita.on-sampler.o" '
        'kage_vita_guest_sampler_stop',
        'check_entry_stop_order ARM "$readelf" "$work/entry_vita.on-beat.o" '
        'isaac_vita_lua_beat_stop',
    ):
        require(needle in script[gate:],
                f"ARM entry_vita.c stanza after the gate: missing {needle!r}")


def main() -> int:
    digests = check_off_identity()
    check_header_and_unit()
    check_cmake()
    check_gate()
    check_entry()
    for relative, digest in sorted(digests.items()):
        print(f"{relative}: OFF text sha256={digest}")
    print("guest stack memblock wiring + OFF identity vs pinned OFF-text "
          "digests: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
