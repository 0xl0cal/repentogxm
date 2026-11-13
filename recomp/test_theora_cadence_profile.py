#!/usr/bin/env python3
"""Execute the actual diagnostic aggregator, pin fetch semantics and OFF surface.

Host-only: no generation of the game corpus, Vita builds or device operations.
"""
from __future__ import annotations

import argparse
import hashlib
import itertools
import re
import subprocess
import tempfile
from pathlib import Path

import pefile

from test_theora_worker_pump_sites import (PE_SHA256, PE_SIZE, decode, expect, va,
                                         check_pe as check_worker_pe)
from vita.test_kage_vita_fullspeed_scheduler import compiler, run

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
KNOB = "ISAAC_VITA_THEORA_CADENCE_PROFILE"
# The commit before the knob: the OFF text of the three gated TUs must equal it.
BASELINE = "a2576d1"


def baseline_available() -> bool:
    """False in a checkout whose history lacks BASELINE (a published snapshot)."""
    return subprocess.run(["git", "cat-file", "-e", BASELINE + "^{commit}"], cwd=ROOT,
                          capture_output=True).returncode == 0


def check_fetch_pe(path: Path) -> None:
    data = path.read_bytes()
    assert len(data) == PE_SIZE and hashlib.sha256(data).hexdigest() == PE_SHA256
    check_worker_pe(path)  # includes exact sole direct fetch caller, vtable, manager vector
    pe = pefile.PE(data=data, fast_load=True)
    body = decode(pe, 0x599DB0, 0x2A0)
    # Copied from frozen generated body, not inferred from upstream types.
    for rva, text in {
        0x599DD8: "mov eax, ecx",
        0x599DDD: "mov ebx, dword ptr [eax + 4]",
        0x599DE3: "test ebx, ebx",
        0x599DE5: f"je {va(0x59A03C)}",
        0x599ECA: "add dword ptr [ebx + 0x9c], edx",
        0x599ED1: f"call {va(0x5A5230)}",
        0x599F23: "test esi, esi",
        0x599F25: f"je {va(0x59A01F)}",
        0x599F72: "push dword ptr [esi + 0x18]",
        0x599F8E: f"call dword ptr [{va(0x7C2268)}]",
        0x599F94: "mov edi, dword ptr [ebx + 4]",
        0x599F9A: "inc dword ptr [edi + 0xa0]",
        0x599FC6: "push 1",
        0x599FD1: f"call {va(0x5A5230)}",
        0x599FFA: "push 1",
        0x59A005: f"call {va(0x5A5230)}",
        0x59A04D: "ret 8",
    }.items():
        expect(body, rva, text)
    print("theora cadence PE: PASS (frozen identity, sole fetch caller, clip, discard, upload-before-displayed, both pop paths)")


def strip_profile_blocks(text: str) -> str:
    """Remove complete opt-in blocks, then separately verify emitted OFF code."""
    result = []
    depth = 0
    for line in text.splitlines(keepends=True):
        # gl_vita_backend.c also carries the additive ISAAC_VITA_FBO_READBACK_FIX
        # blocks (issue #10, plain defined() guards, OFF surface untouched).
        if not depth and (any(line.startswith(f"#if defined({knob}) && {knob}")
                for knob in (KNOB, "ISAAC_VITA_THEORA_RENDER_PRIORITY", "ISAAC_VITA_VITAGL_RGB_SUBUPLOAD"))
                or line.startswith("#if defined(ISAAC_VITA_FBO_READBACK_FIX)")
                # ISAAC_VITA_FBO_DEFERRED_ATTACH: additive, plain defined() guards.
                or line.startswith("#if defined(ISAAC_VITA_FBO_DEFERRED_ATTACH)")):
            depth = 1
            continue
        if depth:
            if re.match(r"#\s*if", line):
                depth += 1
            elif re.match(r"#\s*endif", line):
                depth -= 1
            continue
        result.append(line)
    assert depth == 0
    return "".join(result)


def check_sources() -> None:
    cmake = (HERE / "vita/CMakeLists.txt").read_text()
    assert re.search(rf'option\({KNOB}\s+"[^"]+" OFF\)', cmake)
    assert f'{KNOB} requires NATIVE_THEORA=ON, THEORA_WORKER_PUMP>0 and FULLSPEED_SCHEDULER=ON' in cmake
    wiring = cmake.split(f"    if({KNOB})\n", 1)[1].split("    endif()", 1)[0]
    assert wiring.count(f"{KNOB}=1") == 1
    assert wiring.count('"${ISAAC_RUNTIME}/') == 3
    assert "host_vita_native_theora.c" in wiring and "host_vita_theora_worker_pump.c" in wiring
    assert "gl_vita_backend.c" in wiring
    assert "--wrap=sub_00599db0" in wiring and "list(APPEND" not in wiring
    gate = (HERE / "vita/vita_raw_allocator_gate.py").read_text()
    have_baseline = baseline_available()
    if not have_baseline:
        print(f"theora cadence OFF text vs {BASELINE}: SKIP (commit not in this repository's history)")
    for filename in ("host_vita_native_theora.c", "host_vita_theora_worker_pump.c", "gl_vita_backend.c"):
        assert filename in gate
        if not have_baseline:
            continue
        baseline = subprocess.check_output(["git", "show", f"{BASELINE}:recomp/runtime/{filename}"], cwd=ROOT, text=True)
        current = (HERE / "runtime" / filename).read_text()
        # Blank lines have no token/behaviour effect; no changed old comments or code.
        normalize = lambda s: "\n".join(l for l in s.splitlines() if l.strip())
        assert normalize(strip_profile_blocks(current)) == normalize(baseline), filename
    pump = (HERE / "runtime/host_vita_theora_worker_pump.c").read_text()
    for name, value in (("VIDEO_CLIP", "0x4U"), ("CLIP_DROPPED", "0x9cU"), ("CLIP_DISPLAYED", "0xa0U")):
        assert f"#define VITA_TC_{name}_OFFSET {value}" in pump
    for filename in ("host_vita_native_theora_snapshot.h", "host_vita_theora_cadence_profile.h"):
        text = (HERE / "runtime" / filename).read_text()
        assert not re.search(r"\b(malloc|calloc|realloc|free|sceKernelCreateThread)\s*\(", text)
    logger = (HERE / "vita/platform.c").read_text()
    assert "char body[384];" in logger and "char line[512];" in logger
    assert '"[%u.%03u thr 0x%x] %s\\n"' in logger
    print("theora cadence sources: PASS (OFF guards, three existing gated TUs, pins, logger capacities)")


def host_tests() -> None:
    cc = compiler()
    runtime = HERE / "runtime"
    with tempfile.TemporaryDirectory(prefix="theora-cadence-") as tmp:
        tmp = Path(tmp)
        exe = tmp / "cadence.exe"
        common = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", f"-I{runtime}"]
        run(common + [str(HERE / "vita/host_tests/theora_cadence_profile_oracle.c"), "-o", str(exe)])
        print(run([str(exe)]).strip())
        # Compile the real pump (not a synthetic substitute), ON and OFF.
        filename = "host_vita_theora_worker_pump.c"
        source = tmp / filename
        current = (runtime / filename).read_text()
        # baseline (when the commit is present), knob absent, knob explicitly 0
        texts = [current, current]
        if baseline_available():
            texts.insert(0, subprocess.check_output(
                ["git", "show", f"{BASELINE}:recomp/runtime/{filename}"], cwd=ROOT, text=True))
        else:
            print(f"theora cadence pump OFF object vs {BASELINE}: SKIP (commit not in this repository's history)")
        objects = []
        for index, text in enumerate(texts):
            source.write_text(text)
            obj = tmp / f"off{index}.o"
            flags = ["-DISAAC_VITA_THEORA_WORKER_PUMP=1"]
            if index == len(texts) - 1:
                flags += [f"-D{KNOB}=0"]
            run(common + flags + ["-c", str(source), "-o", str(obj)])
            data = obj.read_bytes()
            # Measured drift was only COFF byte 4 (TimeDateStamp). ELF has no
            # such timestamp. Compare every other byte, including relocations.
            if int.from_bytes(data[:2], "little") == pefile.MACHINE_TYPE["IMAGE_FILE_MACHINE_AMD64"]:
                data = data[:4] + b"\0" * 4 + data[8:]
            objects.append(data)
        if any(data != objects[0] for data in objects[1:]):
            for index in range(1, len(objects)):
                changed = [i for i, (a, b) in enumerate(zip(objects[0], objects[index])) if a != b]
                print(f"OFF object {index}: sizes={len(objects[0])}/{len(objects[index])} changed={changed[:30]}")
            raise AssertionError("pump OFF object drift")
        run(common + ["-DISAAC_VITA_THEORA_WORKER_PUMP=1", f"-D{KNOB}=1", "-c", str(source), "-o", str(tmp / "on.o")])
        kinds = "baseline/absent/explicit-zero" if len(objects) == 3 else "absent/explicit-zero"
        print(f"theora cadence pump: PASS (actual TU compiles ON; {kinds} OFF objects identical except COFF timestamp)")
        # Execute the actual read-only accessor, re-extracted on every test run.
        native = (runtime / "host_vita_native_theora.c").read_text()
        counters = re.search(r"static struct \{\n.*?\} s_nt;", native, re.S).group()
        accessor = re.search(r"void isaac_vita_native_theora_snapshot\([^}]+\n\}", native).group()
        fields = ("packets", "rgb_decodes", "vorbis_syntheses", "dup_frames", "packet_errors",
                  "packetin_us_sum", "rgb_us_sum", "vorbis_us_sum")
        oracle = '#include <assert.h>\n#include <string.h>\n#define ' + KNOB + ' 1\n'
        oracle += '#include "host_vita_native_theora_snapshot.h"\n' + counters + '\n' + accessor
        oracle += '\nint main(void) { VitaTheoraNativeSnapshot out; unsigned char before[sizeof s_nt];\n'
        oracle += '\n'.join(f's_nt.{field} = {i + 1}U;' for i, field in enumerate(fields))
        oracle += '\nmemcpy(before, &s_nt, sizeof s_nt); isaac_vita_native_theora_snapshot(&out);\n'
        oracle += '\n'.join(f'assert(out.{field} == {i + 1}U);' for i, field in enumerate(fields))
        oracle += '\nassert(memcmp(before, &s_nt, sizeof s_nt) == 0); return 0; }\n'
        src = tmp / "accessor.c"; src.write_text(oracle)
        exe = tmp / "accessor.exe"
        run(common + [str(src), "-o", str(exe)])
        run([str(exe)])
        print("theora cadence native snapshot: PASS (current accessor executes, exact fields, no counter writes)")
        # Exercise the current typed native-GL wrapper and actual begin/end
        # observers together. Non-video uploads must not become video costs.
        def body(text, signature):
            start = text.index(signature)
            return text[start:text.index("\n}", start) + 2] + "\n"
        pump = (runtime / "host_vita_theora_worker_pump.c").read_text()
        fetch = body(pump, "void __wrap_sub_00599db0(")
        guarded_fetch = fetch[fetch.index("++s_vita_tc_fetch_depth;"):]
        assert guarded_fetch.index("__real_sub_00599db0(c)") < guarded_fetch.index("--s_vita_tc_fetch_depth;")
        gl = (runtime / "gl_vita_backend.c").read_text()
        oracle = r'''
#define _CRT_SECURE_NO_WARNINGS 1
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#define ISAAC_VITA_THEORA_CADENCE_PROFILE 1
#include "host_vita_theora_cadence_profile.h"
#include "host_vita_theora_upload_profile.h"
#include "gl_bridge.h"
#include "gl_vita_backend_test_vitagl.h"
#define KAGE_VITA_DEEP_SCOPE(x) ((void)0)
#define GL_VITA_PHASE_COUNT(x) ((void)0)
#define GL_VITA_TIME_BEGIN() ((void)0)
#define GL_VITA_TIME_END(x) ((void)0)
#define GL_VITA_TEXTURE_2D 0xde1U
typedef struct SceGxmTexture { uint32_t format, width, height, kind; } SceGxmTexture;
static SceGxmTexture native_texture = {123U, 1024U, 544U, 456U};
static uint32_t layout_queries;
static const SceGxmTexture *vglGetGxmTexture(GLenum target) {
    assert(target == GL_VITA_TEXTURE_2D); ++layout_queries; return &native_texture;
}
static uint32_t sceGxmTextureGetFormat(const SceGxmTexture *p) { return p->format; }
static uint32_t sceGxmTextureGetWidth(const SceGxmTexture *p) { return p->width; }
static uint32_t sceGxmTextureGetHeight(const SceGxmTexture *p) { return p->height; }
static uint32_t sceGxmTextureGetType(const SceGxmTexture *p) { return p->kind; }
static VitaTheoraCadenceProfile s_vita_tc;
static uint32_t s_vita_tc_fetch_depth, s_vita_tc_upload_info_rows, native_calls, clock_reads;
static uint32_t s_vita_tc_layout_rows;
static uint64_t now = 100U;
static uint64_t vita_theora_now_us(void) { ++clock_reads; return now; }
void isaac_vita_log(const char *format, ...) {
    char line[384]; va_list args; va_start(args, format);
    int n = vsnprintf(line, sizeof line, format, args); va_end(args);
    assert(n >= 0 && n < (int)sizeof line);
    if (strncmp(line, "tc.layout ", 10) == 0) {
        unsigned f, w, h, t;
        assert(sscanf(line, "tc.layout native_format=%x width=%u height=%u texture_type=%x",
                      &f, &w, &h, &t) == 4);
        assert(f == native_texture.format && w == native_texture.width &&
               h == native_texture.height && t == native_texture.kind);
    }
}
void oracle_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
    GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels) {
    /* Frozen video call args, with a harmless stand-in pixel address. */
    assert(target == 0xde1U && level == 0 && x == 0 && y == 0);
    assert(w == 960 && h == 544 && format == 0x1907U && type == 0x1401U);
    assert((uintptr_t)pixels == 0x1230U);
    ++native_calls; now += 1234U;
}
'''
        oracle += body(pump, "uint64_t isaac_vita_theora_subupload_begin(")
        oracle += body(pump, "void isaac_vita_theora_subupload_end(")
        oracle += body(pump, "int isaac_vita_theora_subupload_layout_needed(")
        oracle += body(pump, "void isaac_vita_theora_subupload_layout(")
        oracle += body(gl, "static void vita_glTexSubImage2D(")
        oracle += r'''
static void upload(void) { vita_glTexSubImage2D(0xde1U, 0, 0, 0, 960, 544, 0x1907U, 0x1401U, 0x1230U); }
int main(void) {
    VitaTheoraCadenceSample a = {0}; a.clips = 1U;
    vita_tc_begin(&s_vita_tc, &a, 0x48bdf5U);
    upload(); /* same GL call, outside the video fetch */
    assert(s_vita_tc.subuploads == 0U && clock_reads == 0U);
    s_vita_tc_fetch_depth = 1U;
    upload();
    assert(s_vita_tc.subuploads == 1U && s_vita_tc.subupload_us == 1234U);
    assert(clock_reads == 2U && s_vita_tc_upload_info_rows == 1U);
    s_vita_tc.overflow = 1U; upload();
    s_vita_tc.overflow = 0U; s_vita_tc.pending = 0U; upload();
    assert(s_vita_tc.subuploads == 1U && clock_reads == 2U);
    assert(native_calls == 4U); /* every real call forwarded exactly once */
    assert(layout_queries == 1U && s_vita_tc_layout_rows == 1U);
    s_vita_tc.pending = 1U;
    for (unsigned i = 0; i < 7U; ++i) upload();
    assert(layout_queries == 4U && s_vita_tc_layout_rows == 4U);
    assert(s_vita_tc_upload_info_rows == 4U);
    assert(s_vita_tc.subuploads == 8U && s_vita_tc.subupload_us == 8U * 1234U);
    vita_tc_fetch(&s_vita_tc, 0, 0, 0, 1, 0);
    vita_tc_fetch_time(&s_vita_tc, 0, 1500);
    vita_tc_end(&s_vita_tc, &a, 0, 0, 0);
    vita_tc_close(&s_vita_tc, &a, 0x48bdf5U);
    puts("theora upload observer: PASS (current GL wrapper/observers, exact args/call count/native time/layout, four-row bounds, inactive/overflow no clocks)");
    return 0;
}
'''
        src = tmp / "upload-observer.c"; src.write_text(oracle)
        exe = tmp / "upload-observer.exe"
        run(common + [str(src), "-o", str(exe)])
        print(run([str(exe)]).strip())
        # Evaluate the real dependency guard with CMake, independent of SDK.
        cmake = (HERE / "vita/CMakeLists.txt").read_text()
        guard = re.search(rf"if\({KNOB} AND\n.*?\nendif\(\)", cmake, re.S).group()
        for enabled, native_on, pump_on, scheduler_on in itertools.product((0, 1), repeat=4):
            script = tmp / "deps.cmake"
            script.write_text(f'set({KNOB} {enabled})\nset(ISAAC_VITA_NATIVE_THEORA {native_on})\n'
                f'set(ISAAC_VITA_THEORA_WORKER_PUMP {pump_on})\nset(ISAAC_VITA_FULLSPEED_SCHEDULER {scheduler_on})\n' + guard)
            result = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
            should_pass = not enabled or (native_on and pump_on and scheduler_on)
            assert (result.returncode == 0) == bool(should_pass), result.stdout + result.stderr
        print("theora cadence CMake: PASS (all 16 dependency combinations, OFF imposes no new dependency)")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    args = parser.parse_args()
    check_sources()
    check_fetch_pe(args.pe)
    host_tests()


if __name__ == "__main__":
    main()
