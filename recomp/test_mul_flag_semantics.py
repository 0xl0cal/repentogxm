#!/usr/bin/env python3
"""Compile and execute current emitted MUL/IMUL flag semantics."""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import tempfile
import textwrap

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

import emit as emitter_module


HERE = pathlib.Path(__file__).resolve().parent


def fail(message: str) -> None:
    raise SystemExit(message)


def emitted(raw: bytes, live_after: bool) -> str:
    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    decoder.detail = True
    instructions = list(decoder.disasm(raw, 0x1000))
    if len(instructions) != 1 or instructions[0].size != len(raw):
        fail(f"test instruction did not decode exactly: {raw.hex()}")
    emitter = emitter_module.Emitter(None, "mul_flag_oracle", {}, {}, {})
    return "\n".join(emitter.emit(instructions[0], live_after, None))


def room_chain_from_emitter() -> str:
    # Exact 003e5ac5..003e5ad8 instruction bytes from the frozen PE.  MUL is
    # the sole flag producer consumed by SETO; XOR/NEG/OR have dead flags.
    instructions = (
        (bytes.fromhex("0fb6c1"), False),       # movzx eax, cl
        (bytes.fromhex("ba0c000000"), False), # mov edx, 12
        (bytes.fromhex("33c9"), False),       # xor ecx, ecx
        (bytes.fromhex("f7e2"), True),        # mul edx
        (bytes.fromhex("0f90c1"), False),     # seto cl
        (bytes.fromhex("f7d9"), False),       # neg ecx
        (bytes.fromhex("0bc8"), False),       # or ecx, eax
    )
    return "\n".join(emitted(raw, live) for raw, live in instructions)


def generated_room_chain(generated_dir: pathlib.Path) -> str:
    begin_marker = "/* 003e5ac5  movzx eax, cl */"
    end_marker = "/* 003e5ad8  push ecx */"
    matches: list[tuple[pathlib.Path, str]] = []
    for path in sorted(generated_dir.glob("guest_[0-9][0-9][0-9][0-9].c")):
        source = path.read_bytes().replace(b"\r\n", b"\n").decode("ascii")
        if begin_marker not in source:
            continue
        begin = source.index(begin_marker)
        end = source.index(end_marker, begin)
        matches.append((path, source[begin:end]))
    if len(matches) != 1:
        fail(f"generated RoomConfig request chain maps to {len(matches)} units")
    return matches[0][1]


def normalized_statements(source: str) -> str:
    return "\n".join(
        line.strip()
        for line in source.splitlines()
        if line.strip() and not line.strip().startswith("/*")
    )


def indent(source: str) -> str:
    return textwrap.indent(source, "    ")


def build_source(room_chain: str) -> str:
    mul32 = emitted(bytes.fromhex("f7e2"), True)
    mul_request_tail = "\n".join((
        mul32,
        emitted(bytes.fromhex("0f90c1"), False),
        emitted(bytes.fromhex("f7d9"), False),
        emitted(bytes.fromhex("0bc8"), False),
    ))
    imul32_one = emitted(bytes.fromhex("f7ea"), True)
    imul32_two = emitted(bytes.fromhex("0fafc2"), True)
    imul16_two = emitted(bytes.fromhex("660fafc2"), True)
    imul32_imm = emitted(bytes.fromhex("6bc20c"), True)
    imul32_neg1 = emitted(bytes.fromhex("6bc2ff"), True)
    return f"""
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "guest.h"

#define CHECK(condition) do {{ \
    if (!(condition)) {{ \
        fprintf(stderr, "MUL flag oracle failed at line %d: %s\\n", \
                __LINE__, #condition); \
        return 1; \
    }} \
}} while (0)

static int check_mul32(uint32_t a, uint32_t b, uint8_t poison,
                       uint32_t low, uint32_t high, int overflow)
{{
    CPU state;
    CPU *c = &state;
    memset(c, 0, sizeof *c);
    c->eax = a;
    c->edx = b;
    c->f_cf = poison;
    c->f_of = poison;
{indent(mul32)}
    CHECK(c->eax == low && c->edx == high);
    CHECK(fl_cf(&c->fl) == overflow && fl_of(&c->fl) == overflow);
    CHECK(c->f_op == FLAG_MUL);
    return 0;
}}

static int check_mul_request_tail(uint32_t a, uint32_t b, uint8_t poison,
                                  uint32_t request)
{{
    CPU state;
    CPU *c = &state;
    memset(c, 0, sizeof *c);
    c->eax = a;
    c->edx = b;
    c->f_cf = poison;
    c->f_of = poison;
{indent(mul_request_tail)}
    CHECK(c->ecx == request);
    return 0;
}}

static int check_imul32_one(int32_t a, int32_t b, uint8_t poison,
                            uint32_t low, uint32_t high, int overflow)
{{
    CPU state;
    CPU *c = &state;
    memset(c, 0, sizeof *c);
    c->eax = (uint32_t)a;
    c->edx = (uint32_t)b;
    c->f_cf = poison;
    c->f_of = poison;
{indent(imul32_one)}
    CHECK(c->eax == low && c->edx == high);
    CHECK(fl_cf(&c->fl) == overflow && fl_of(&c->fl) == overflow);
    CHECK(c->f_op == FLAG_IMUL);
    return 0;
}}

static int check_imul32_two(int32_t a, int32_t b, uint8_t poison,
                            uint32_t low, int overflow)
{{
    CPU state;
    CPU *c = &state;
    memset(c, 0, sizeof *c);
    c->eax = (uint32_t)a;
    c->edx = (uint32_t)b;
    c->f_cf = poison;
    c->f_of = poison;
{indent(imul32_two)}
    CHECK(c->eax == low);
    CHECK(fl_cf(&c->fl) == overflow && fl_of(&c->fl) == overflow);
    CHECK(c->f_op == FLAG_IMUL);
    return 0;
}}

static int check_imul16_two(int16_t a, int16_t b, uint8_t poison,
                            uint16_t low, int overflow)
{{
    CPU state;
    CPU *c = &state;
    memset(c, 0, sizeof *c);
    c->eax = UINT32_C(0xa5a50000) | (uint16_t)a;
    c->edx = (uint16_t)b;
    c->f_cf = poison;
    c->f_of = poison;
{indent(imul16_two)}
    CHECK(c->eax == (UINT32_C(0xa5a50000) | low));
    CHECK(fl_cf(&c->fl) == overflow && fl_of(&c->fl) == overflow);
    CHECK(c->f_op == FLAG_IMUL);
    return 0;
}}

static int check_imul32_imm(int32_t b, uint8_t poison,
                            uint32_t low, int overflow)
{{
    CPU state;
    CPU *c = &state;
    memset(c, 0, sizeof *c);
    c->edx = (uint32_t)b;
    c->f_cf = poison;
    c->f_of = poison;
{indent(imul32_imm)}
    CHECK(c->eax == low);
    CHECK(fl_cf(&c->fl) == overflow && fl_of(&c->fl) == overflow);
    CHECK(c->f_op == FLAG_IMUL);
    return 0;
}}

static int check_imul32_neg1(int32_t b, uint8_t poison,
                             uint32_t low, int overflow)
{{
    CPU state;
    CPU *c = &state;
    memset(c, 0, sizeof *c);
    c->edx = (uint32_t)b;
    c->f_cf = poison;
    c->f_of = poison;
{indent(imul32_neg1)}
    CHECK(c->eax == low);
    CHECK(fl_cf(&c->fl) == overflow && fl_of(&c->fl) == overflow);
    CHECK(c->f_op == FLAG_IMUL);
    return 0;
}}

static int check_room_request(uint8_t count, uint8_t poison)
{{
    CPU state;
    CPU *c = &state;
    memset(c, 0, sizeof *c);
    c->ecx = count;
    c->f_op = FLAG_EXPLICIT;
    c->f_cf = poison;
    c->f_of = poison;
{indent(room_chain)}
    CHECK(c->ecx == (uint32_t)count * UINT32_C(12));
    return 0;
}}

int main(void)
{{
    unsigned count;
    unsigned poison;
    for (count = 1U; count <= 255U; ++count)
        for (poison = 0U; poison <= 1U; ++poison)
            CHECK(check_room_request((uint8_t)count, (uint8_t)poison) == 0);
    CHECK(check_mul32(1U, 12U, 1U, 12U, 0U, 0) == 0);
    CHECK(check_mul32(UINT32_MAX, 1U, 1U, UINT32_MAX, 0U, 0) == 0);
    CHECK(check_mul32(UINT32_MAX, 2U, 0U,
                      UINT32_C(0xfffffffe), 1U, 1) == 0);
    CHECK(check_mul32(UINT32_C(0x80000000), 2U, 0U, 0U, 1U, 1) == 0);
    CHECK(check_mul_request_tail(1U, 12U, 1U, 12U) == 0);
    CHECK(check_mul_request_tail(UINT32_MAX, 2U, 0U,
                                 UINT32_MAX) == 0);
    CHECK(check_imul32_one(-2, 3, 1U, UINT32_C(0xfffffffa),
                           UINT32_MAX, 0) == 0);
    CHECK(check_imul32_one(INT32_MAX, 2, 0U, UINT32_C(0xfffffffe),
                           0U, 1) == 0);
    CHECK(check_imul32_one(INT32_MIN, 1, 1U, UINT32_C(0x80000000),
                           UINT32_MAX, 0) == 0);
    CHECK(check_imul32_one(INT32_MIN, -1, 0U, UINT32_C(0x80000000),
                           0U, 1) == 0);
    CHECK(check_imul32_two(-2, 3, 1U, UINT32_C(0xfffffffa), 0) == 0);
    CHECK(check_imul32_two(INT32_MAX, 2, 0U,
                           UINT32_C(0xfffffffe), 1) == 0);
    CHECK(check_imul32_two(INT32_MIN, 1, 1U,
                           UINT32_C(0x80000000), 0) == 0);
    CHECK(check_imul32_two(INT32_MIN, -1, 0U,
                           UINT32_C(0x80000000), 1) == 0);
    CHECK(check_imul16_two(-2, 3, 1U, UINT16_C(0xfffa), 0) == 0);
    CHECK(check_imul16_two(30000, 3, 0U, UINT16_C(0x5f90), 1) == 0);
    CHECK(check_imul32_imm(-2, 1U, UINT32_C(0xffffffe8), 0) == 0);
    CHECK(check_imul32_imm(INT32_MAX, 0U, UINT32_C(0xfffffff4), 1) == 0);
    CHECK(check_imul32_neg1(INT32_MAX, 1U, UINT32_C(0x80000001), 0) == 0);
    CHECK(check_imul32_neg1(INT32_MIN, 0U,
                            UINT32_C(0x80000000), 1) == 0);
    puts("generated MUL/IMUL flag execution oracle: PASS");
    return 0;
}}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", required=True)
    parser.add_argument("--generated-dir", type=pathlib.Path)
    arguments = parser.parse_args()

    current_chain = room_chain_from_emitter()
    room_chain = current_chain
    coverage = "current-emitter"
    if arguments.generated_dir is not None:
        generated_chain = generated_room_chain(arguments.generated_dir)
        if normalized_statements(generated_chain) != \
                normalized_statements(current_chain):
            fail("frozen generated RoomConfig request chain is stale vs emit.py")
        room_chain = generated_chain
        coverage = "fresh-generated"

    source = build_source(room_chain)
    with tempfile.TemporaryDirectory(prefix="isaac-mul-flags-") as work_raw:
        work = pathlib.Path(work_raw)
        source_path = work / "mul_flags.c"
        binary_path = work / "mul_flags"
        source_path.write_text(source, encoding="ascii", newline="\n")
        command = [
            arguments.cc,
            "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
            "-I", os.fspath(HERE / "runtime"), os.fspath(source_path),
            "-o", os.fspath(binary_path),
        ]
        built = subprocess.run(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, check=False,
        )
        if built.stdout:
            print(built.stdout, end="")
        if built.returncode:
            fail(f"MUL flag oracle compile failed: {built.returncode}")
        environment = os.environ.copy()
        environment["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        ran = subprocess.run(
            [os.fspath(binary_path)], stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, check=False,
            timeout=30, env=environment,
        )
        if ran.stdout:
            print(ran.stdout, end="")
        if ran.returncode:
            fail(f"MUL flag oracle execution failed: {ran.returncode}")
    print(
        f"generated MUL/IMUL compiler gate ({arguments.cc}; {coverage}): PASS"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
