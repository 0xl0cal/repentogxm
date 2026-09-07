#!/usr/bin/env python3
"""ANM2 scratch integration and optional fresh-codegen pool-init comparison."""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def function_body(text: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", text, re.DOTALL)
    if not match:
        raise AssertionError(f"function absent: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    raise AssertionError(f"unterminated function: {name}")


POOL_FIXTURE = r'''
#include "guest.h"
#include "host_vita_anm2_scratch.h"
#include <psp2/kernel/sysmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
enum { ARENA = 0x22000000U, BYTES = 0x340000U,
       FLOOR = ARENA + 0x300000U, CEILING = ARENA + BYTES };
static unsigned char *arena, *input, *expected, *first_input;
static unsigned native_mode, calls, real_calls, capture;
static CPU first_cpu;
static guest_flags tail_flags;
static char last_log[512];
static int reserve_error;
unsigned char *g_guest_coverage_functions, *g_guest_coverage_cases;
void __wrap_sub_00002120(CPU *__restrict c);
void original_color(CPU *__restrict c);
static void fail(int line, const char *condition) {
    fprintf(stderr, "ANM2 pool comparison failed line %d: %s\n", line, condition);
    exit(1);
}
#define CHECK(x) do { if (!(x)) fail(__LINE__, #x); } while (0)
int guest_stack_violation(CPU *c, uint32_t pc, uint32_t kind,
                          uint32_t address, uint32_t size) {
    (void)c; (void)pc; (void)kind; (void)address; (void)size;
    fail(__LINE__, "unexpected stack violation"); return 0;
}
int guest_stack_owner_violation(CPU *c, uint32_t pc) {
    (void)c; (void)pc; fail(__LINE__, "unexpected owner violation"); return 0;
}
SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                             SceSize size, SceKernelAllocMemBlockOpt *opt) {
    (void)name; CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
    CHECK(size == ISAAC_VITA_ANM2_MEMBLOCK_BYTES && !opt);
    return reserve_error ? reserve_error : 1;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base) {
    CHECK(uid == 1); *base = arena; return 0;
}
int sceKernelFreeMemBlock(SceUID uid) { CHECK(uid == 1); return 0; }
void isaac_vita_log(const char *format, ...) {
    va_list ap; va_start(ap, format);
    vsnprintf(last_log, sizeof last_log, format, ap); va_end(ap);
}
void __real_sub_00002120(CPU *__restrict c) {
    ++real_calls; original_color(c);
}
void sub_00002120(CPU *__restrict c) {
    ++calls;
    if (capture && calls == 1U) {
        first_cpu = *c; memcpy(first_input, arena, BYTES);
    }
    if (native_mode) __wrap_sub_00002120(c);
    else __real_sub_00002120(c);
}
/* FRESH_FUNCTIONS */
static void (*const loops[6])(CPU *) = {
    loop_0, loop_1, loop_2, loop_3, loop_4, loop_5
};
static const uint32_t owners[6] = { 0xaa5d,0xaaa9,0xaaf9,0xab59,0xaba9,0xac25 };
static const unsigned offsets[6] = { 0,420000,840000,1260000,1680000,2220000 };
static void setup(CPU *c, unsigned index, unsigned seed) {
    unsigned i;
    for (i = 0; i < BYTES; ++i) arena[i] = (unsigned char)(i * 71U + seed);
    memset(c, 0, sizeof *c);
    for (i = 0; i < 8; ++i) c->r[i] = 0x12345678U + i;
    memset(c->x, (int)seed, sizeof c->x);
    c->eax = ARENA + offsets[index]; c->esp = FLOOR + 0x100U;
    c->ebp = CEILING - 0x100U;
    c->stack_owner = c; c->stack_floor = FLOOR; c->stack_ceiling = CEILING;
    c->stack_low_water = c->esp;
    memset(&c->fl, 0xa5, sizeof c->fl);
    c->f_op = FLAG_EXPLICIT;
    c->f_cf = 1; c->f_of = 0; c->f_zf = 1; c->f_sf = 0; c->f_pf = 1;
}
static void compare_rejections(void) {
    CPU c, before, after;
    unsigned mode;
    unsigned char coverage[8] = { 0 };
    /* The snapshot is the first real helper entry of the final large pool. */
    for (mode = 0; mode < 12; ++mode) {
        memcpy(arena, first_input, BYTES); c = first_cpu; c.stack_owner = &c;
        switch (mode) {
        case 0: st32(c.esp, 0x111111U); break;
        case 1: c.esi = 4999U; break;
        case 2: c.ecx += 4U; break;
        case 3: c.edx += 4U; break;
        case 4: c.eax += 4U; break;
        case 5: st32(c.ebp - 0x26688U, ARENA + offsets[5] + 4U); break;
        case 6: c.ebp = 0U; break;
        case 7: c.fault = "existing fault"; break;
        case 8: g_guest_coverage_functions = coverage; break;
        case 9: g_guest_coverage_cases = coverage; break;
        case 10: c.stack_floor += 4U; break;
        case 11: c.esi = 0U; break;
        }
        before = c; memcpy(input, arena, BYTES); real_calls = 0;
        __real_sub_00002120(&c); after = c; memcpy(expected, arena, BYTES);
        c = before; memcpy(arena, input, BYTES); real_calls = 0;
        __wrap_sub_00002120(&c);
        CHECK(real_calls == 1U && !memcmp(&c, &after, sizeof c));
        CHECK(!memcmp(arena, expected, BYTES));
        g_guest_coverage_functions = g_guest_coverage_cases = NULL;
    }
}
int main(void) {
    CPU c, before, after;
    guest_flags flags;
    isaac_vita_anm2_scratch_decision decision;
    unsigned round, index;
#ifdef _WIN32
    arena = VirtualAlloc((void *)(uintptr_t)ARENA, BYTES,
                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    arena = mmap((void *)(uintptr_t)ARENA, BYTES, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    CHECK((uintptr_t)arena == ARENA);
    input = malloc(BYTES); expected = malloc(BYTES); first_input = malloc(BYTES);
    CHECK(input && expected && first_input);
    for (round = 0; round < 2; ++round) {
        for (index = 0; index < 6; ++index) {
            CHECK(isaac_vita_anm2_scratch_malloc(owners[index],
                index < 4 ? 420000U : 540000U, FLOOR, CEILING, &decision) == 1);
            CHECK((uintptr_t)decision.pointer == ARENA + offsets[index]);
            setup(&c, index, round * 113U + 19U);
            before = c; memcpy(input, arena, BYTES);
            native_mode = 0; calls = real_calls = 0; capture = 1;
            loops[index](&c); capture = 0;
            CHECK(calls == 5000U && real_calls == 5000U && c.esi == 0U);
            after = c; flags = tail_flags; memcpy(expected, arena, BYTES);
            c = before; memcpy(arena, input, BYTES);
            native_mode = 1; calls = real_calls = 0;
            loops[index](&c);
            CHECK(calls == 1U && real_calls == 0U);
            CHECK(!memcmp(&c, &after, sizeof c));
            CHECK(!memcmp(&tail_flags, &flags, sizeof flags));
            CHECK(!memcmp(arena, expected, BYTES));
        }
        compare_rejections();
        /* Exact ownership admission failures must not mutate any pool. */
        memcpy(input, arena, BYTES);
        CHECK(!isaac_vita_anm2_scratch_init_pool(6, ARENA, FLOOR, CEILING));
        CHECK(!isaac_vita_anm2_scratch_init_pool(0, ARENA, FLOOR, CEILING));
        CHECK(!isaac_vita_anm2_scratch_init_pool(5, ARENA+2220001, FLOOR, CEILING));
        CHECK(!isaac_vita_anm2_scratch_init_pool(5, ARENA+2220000, FLOOR+4, CEILING));
        CHECK(!memcmp(input, arena, BYTES));
        for (index = 0; index < 6; ++index)
            CHECK(isaac_vita_anm2_scratch_free(arena+offsets[index], &decision) == 1);
        CHECK(strstr(last_log, "complete=yes bulk_init=6") != NULL);
        CHECK(!isaac_vita_anm2_scratch_init_pool(5, ARENA+2220000, FLOOR, CEILING));
    }
    /* Same generated callers with the scratch owner absent: ordinary helper. */
    setup(&c, 0, 31); calls = real_calls = 0; native_mode = 1;
    loops[0](&c); CHECK(calls == 5000U && real_calls == 5000U);
    CHECK(isaac_vita_anm2_scratch_oracle_reset() == 0);
    reserve_error = ISAAC_VITA_ANM2_NO_FREE_PHYSICAL_PAGE;
    CHECK(isaac_vita_anm2_scratch_malloc(owners[0], 420000U,
                                       FLOOR, CEILING, &decision) == 2);
    memcpy(input, arena, BYTES);
    CHECK(!isaac_vita_anm2_scratch_init_pool(0, ARENA, FLOOR, CEILING));
    CHECK(!memcmp(input, arena, BYTES));
    setup(&c, 0, 57); calls = real_calls = 0;
    loops[0](&c); CHECK(calls == 5000U && real_calls == 5000U);
    printf("ANM2 pool fresh-codegen PASS: gpr=%d flags=%d guard=%d; 12 loops, "
           "5000->1 calls; full CPU/flags/arena/padding, 24 entry rejections, "
           "lifecycle/heap fallback; bulk_init=6/session\n",
           GUEST_GPR_LOCAL, GUEST_FLAGS_LOCAL, GUEST_GENERATED_STACK_GUARD);
    return 0;
}
'''


def check_pool_codegen(pe: Path, cc: str, output: Path) -> None:
    """Compile unmodified emitted loop spans + helper, not a model of them."""
    here = Path(__file__).resolve().parent
    sys.path.insert(0, str(here))
    import gen_all as G
    from image import Image
    assert pe.stat().st_size == 8650240
    assert hashlib.sha256(pe.read_bytes()).hexdigest() == (
        "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404")
    img = Image(str(pe), 0x98000000)
    caller = G._translate_function(img, {"rva": 0x9b40}, None, {})
    helper = G._translate_function(img, {"rva": 0x2120}, None, {})
    assert caller["stub"] is None and helper["stub"] is None
    output.mkdir(parents=True, exist_ok=True)
    (output / "fresh-caller.c").write_text(caller["text"], encoding="utf-8")
    (output / "fresh-helper.c").write_text(helper["text"], encoding="utf-8")
    emitted = helper["text"].replace("void sub_00002120(", "void original_color(")
    for i, (begin, end) in enumerate(((0xaa5d, 0xaa9f), (0xaaa9, 0xaaef),
            (0xaaf9, 0xab4f), (0xab59, 0xab9f), (0xaba9, 0xac1b), (0xac25, 0xac8f))):
        source = caller["text"]
        span = source[source.index(f"    /* {begin:08x}  "):
                      source.index(f"    /* {end:08x}  ")]
        assert span.count("sub_00002120(c)") == 1
        assert "GUEST_GPR_FLUSH(c); sub_00002120(c); GUEST_GPR_RELOAD(c)" in span
        emitted += (f"\nstatic void loop_{i}(CPU *c) {{\n"
                    "GUEST_FLAGS_DECL; GUEST_GPR_DECL;\n" + span +
                    "tail_flags = *GUEST_FL; GUEST_GPR_FLUSH(c);\n}\n")
    fixture = output / "pool-comparison.c"
    fixture.write_text(POOL_FIXTURE.replace("/* FRESH_FUNCTIONS */", emitted), encoding="utf-8")
    modes = [(gpr, flags, 0) for gpr in (0, 1) for flags in (0, 1)] + [(1, 1, 1)]
    for gpr, flags, guard in modes:
        exe = output / f"pool-gpr{gpr}-flags{flags}-guard{guard}.exe"
        cmd = [cc, "-std=gnu11", "-O2", "-fno-strict-aliasing", "-Wall", "-Wextra",
               "-Werror", "-Wno-unused-variable", "-Wno-unused-label",
               "-D_CRT_SECURE_NO_WARNINGS", "-DISAAC_VITA_ANM2_SCRATCH_ORACLE=1",
               "-DISAAC_VITA_ANM2_POOL_INIT=1", f"-DGUEST_GPR_LOCAL={gpr}",
               f"-DGUEST_FLAGS_LOCAL={flags}", f"-DGUEST_GENERATED_STACK_GUARD={guard}",
               "-DGUEST_IMAGE_BASE=0x98000000U", "-I" + str(here / "runtime"),
               "-I" + str(here / "vita"),
               "-I" + str(here / "vita/anm2_scratch_oracle_include"), str(fixture),
               str(here / "runtime/host_vita_anm2_pool_init.c"),
               str(here / "runtime/host_vita_anm2_scratch.c"), "-o", str(exe)]
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--heap", type=Path, required=True)
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--pe", type=Path, help="also compare freshly emitted pool loops/helper")
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--out", type=Path)
    arguments = parser.parse_args()

    heap = arguments.heap.read_text(encoding="utf-8")
    cmake = arguments.cmake.read_text(encoding="utf-8")

    conditional_include = (
        "#ifdef ISAAC_VITA_ANM2_SCRATCH\n"
        "#include \"host_vita_anm2_scratch.h\"\n"
        "#endif"
    )
    if conditional_include not in heap:
        raise AssertionError("scratch header is not default-OFF guarded")

    malloc = function_body(heap, "vita_heap_malloc")
    free = function_body(heap, "vita_heap_free")
    realloc = function_body(heap, "vita_heap_realloc")
    if "#ifdef ISAAC_VITA_ANM2_SCRATCH" not in malloc:
        raise AssertionError("malloc seam is not compile-time guarded")
    required_malloc = (
        "vita_heap_arg(c, 0U)",
        "vita_heap_arg(c, 2U)",
        "c->stack_floor",
        "c->stack_ceiling",
        "isaac_vita_anm2_scratch_malloc",
        "ISAAC_VITA_ANM2_SCRATCH_REJECTED",
        "ISAAC_VITA_ANM2_SCRATCH_HANDLED",
    )
    for marker in required_malloc:
        if marker not in malloc:
            raise AssertionError(f"malloc seam omits {marker}")
    if "isaac_vita_anm2_scratch_free" not in free or \
       "#ifdef ISAAC_VITA_ANM2_SCRATCH" not in free:
        raise AssertionError("free exact-base seam is absent or unguarded")
    if "isaac_vita_anm2_scratch" in realloc:
        raise AssertionError("realloc semantics were changed")

    option = re.search(
        r"option\(ISAAC_VITA_ANM2_SCRATCH\s+\n?"
        r"\s*\"[^\"]+\"\s+(ON|OFF)\)",
        cmake,
    )
    if not option or option.group(1) != "OFF":
        raise AssertionError("ISAAC_VITA_ANM2_SCRATCH is not default OFF")
    block = re.search(
        r"if\(ISAAC_VITA_ANM2_SCRATCH\)\n(.*?)\n\s*endif\(\)",
        cmake,
        re.DOTALL,
    )
    if not block:
        raise AssertionError("ANM2 CMake selection block is absent")
    selected = block.group(1)
    for marker in (
        "host_vita_anm2_scratch.c",
        "host_vita_heap.c",
        "APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_ANM2_SCRATCH=1",
    ):
        if marker not in selected:
            raise AssertionError(f"ANM2 CMake block omits {marker}")
    if "target_compile_definitions" in selected:
        raise AssertionError("ANM2 macro became target-wide")
    if selected.count('"${ISAAC_RUNTIME}/host_vita_anm2_scratch.c"') != 1:
        raise AssertionError("ANM2 source selection is not singular")
    assert re.search(r'option\(ISAAC_VITA_ANM2_POOL_INIT\s+"[^"]+"\s+OFF\)', cmake)
    assert "NOT ISAAC_VITA_KAGE OR NOT ISAAC_VITA_ANM2_SCRATCH" in cmake
    assert "-Wl,--wrap=sub_00002120" in cmake
    pool_block = cmake[cmake.index("  if(ISAAC_VITA_ANM2_POOL_INIT)"):
                       cmake.index("  if(ISAAC_VITA_TEXEL_SCRATCH)")]
    assert "host_vita_anm2_pool_init.c" in pool_block
    assert "host_vita_anm2_scratch.c" in pool_block
    assert "target_compile_definitions" not in pool_block

    print(
        "ANM2 scratch integration gate: PASS; default=OFF; "
        "owner-stack=ESP+12; macro=host_vita_heap.c-only; realloc=unchanged"
    )
    if arguments.pe:
        if arguments.out:
            check_pool_codegen(arguments.pe.resolve(), arguments.cc, arguments.out.resolve())
        else:
            with tempfile.TemporaryDirectory(prefix="anm2-pool-") as temporary:
                check_pool_codegen(arguments.pe.resolve(), arguments.cc, Path(temporary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
