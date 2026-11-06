#!/usr/bin/env python3
"""Execute the real video-service receipt producer and validate its optional build surface."""
import argparse
import hashlib
import itertools
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile

from test_theora_worker_pump_sites import check_pe
from vita.test_kage_vita_fullspeed_scheduler import compiler, run

HERE = Path(__file__).resolve().parent
KNOB = "ISAAC_VITA_THEORA_RENDER_PRIORITY"


def function(source, signature):
    start = source.index(signature)
    end = source.index("\n}", start) + 2
    return source[start:end] + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", type=Path, required=True)
    parser.add_argument("--baseline-tar", type=Path)
    args = parser.parse_args()
    check_pe(args.pe)
    runtime = HERE / "runtime"
    pump = (runtime / "host_vita_theora_worker_pump.c").read_text()
    cmake = (HERE / "vita/CMakeLists.txt").read_text()
    assert re.search(rf'option\({KNOB}\s+"[^"]+" OFF\)', cmake)
    assert "#define VITA_THEORA_MAIN_SERVICE_RETURN_RVA 0x0048bdf5U" in pump
    service = function(pump, "void __wrap_sub_00598d60(")
    assert service.index("video_service_site = vita_theora_seam_site(c)") < service.index("__real_sub_00598d60(c)")
    assert service.index("__real_sub_00598d60(c)") < service.index("kage_vita_fullspeed_scheduler_note_video_service()")
    pins = "\n".join(re.findall(r"^#define VITA_THEORA_(?:MANAGER_SINGLETON_RVA|MANAGER_CLIPS_BEGIN|MANAGER_CLIPS_END|MAIN_SERVICE_RETURN_RVA)\s+0x[0-9a-f]+U.*$", pump, re.M))
    assert pins.count("#define") == 4
    fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define ISAAC_VITA_THEORA_RENDER_PRIORITY 1
#define GUEST_IMAGE_BASE 0U
#include "kage_vita_fullspeed_scheduler.h"
typedef struct CPU { uint32_t esp; const char *fault; } CPU;
static uint32_t manager, begin, end, clip, site, notes, real_calls, pumps, fail_real;
static int s_vita_theora_pump_active;
static uint32_t ld32(uint32_t address) {
    if (address == VITA_THEORA_MANAGER_SINGLETON_RVA) return manager;
    if (address == 0x1000U + VITA_THEORA_MANAGER_CLIPS_BEGIN) return begin;
    if (address == 0x1000U + VITA_THEORA_MANAGER_CLIPS_END) return end;
    if (address == 0x2000U) return clip;
    if (address == 0x4000U) return site;
    assert(!"unexpected memory read (including an already-popped return)"); return 0;
}
static uint32_t guest_stack_address(CPU *c, uint32_t address, uint32_t size, uint32_t rva) {
    assert(size == 4U && rva == 0U); return c->fault ? 0U : address;
}
static void vita_theora_pump_slice(CPU *c, uint32_t caller) {
    (void)c; assert(caller == site); ++pumps;
}
void kage_vita_fullspeed_scheduler_note_video_service(void) { ++notes; }
void __real_sub_00598d60(CPU *c) { ++real_calls; c->esp += 4U; if (fail_real) c->fault = "test fault"; }
'''
    # Pins are copied from the current production source and machine-checked above.
    fixture = fixture.replace("static uint32_t manager", pins + "\nstatic uint32_t manager")
    fixture += function(pump, "static uint32_t vita_theora_seam_site(")
    fixture += function(pump, "static int vita_theora_has_single_clip(")
    fixture += service
    fixture += r'''
int main(void) {
    for (unsigned test = 0; test < 13; ++test) {
        CPU cpu = {0x4000U, 0};
        manager = 0x1000U; begin = 0x2000U; end = begin + 4U; clip = 0x3000U;
        site = VITA_THEORA_MAIN_SERVICE_RETURN_RVA;
        notes = real_calls = pumps = fail_real = 0;
        switch (test) {
        case 1: manager = 0; break;
        case 2: ++manager; break;
        case 3: begin = 0; break;
        case 4: ++begin; break;
        case 5: end = begin; break;
        case 6: end = begin - 4U; break;
        case 7: end = begin + 8U; break;
        case 8: clip = 0; break;
        case 9: ++clip; break;
        case 10: site = 0x00476b38U; break; /* pinned internal CL=1 return */
        case 11: fail_real = 1; break;
        case 12: site = 0; break;
        }
        uint32_t before[] = {manager, begin, end, clip};
        __wrap_sub_00598d60(&cpu);
        assert(notes == (test == 0));
        assert(real_calls == 1 && pumps == 1 && cpu.esp == 0x4004U);
        assert(manager == before[0] && begin == before[1] && end == before[2] && clip == before[3]);
    }
    puts("video priority producer: PASS (current wrapper/helper, 13 states, main/internal caller, return-pop, fault, no guest writes)");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="video-priority-") as temp:
        temp = Path(temp)
        cc = compiler()
        common = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", f"-I{runtime}", f"-I{HERE/'vita'}"]
        source = temp / "producer.c"; source.write_text(fixture)
        exe = temp / "producer.exe"
        run(common + [str(source), "-o", str(exe)])
        print(run([str(exe)]).strip())
        run(common + [f"-D{KNOB}=1", "-DISAAC_VITA_THEORA_WORKER_PUMP=1",
            "-DISAAC_VITA_THEORA_CADENCE_PROFILE=1", "-c",
            str(runtime/"host_vita_theora_worker_pump.c"), "-o", str(temp/"pump-on.o")])
        guard = re.search(rf"if\({KNOB} AND\n.*?\nendif\(\)", cmake, re.S).group()
        for on, scheduler, worker in itertools.product((0, 1), repeat=3):
            script = temp/"deps.cmake"
            script.write_text(f"set({KNOB} {on})\nset(ISAAC_VITA_FULLSPEED_SCHEDULER {scheduler})\n"
                              f"set(ISAAC_VITA_THEORA_WORKER_PUMP {worker})\n" + guard)
            result = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
            assert (result.returncode == 0) == (not on or bool(scheduler and worker)), result.stdout + result.stderr
        print("video priority CMake: PASS (8 dependency combinations; real pump TU compiles with both options)")
        if args.baseline_tar:
            # Optional historical identity proof. Never consume previously
            # compiled objects as a regression test: rebuild each input now.
            with tarfile.open(args.baseline_tar) as tar:
                baseline = tar.extractfile("recomp/runtime/kage_vita_fullspeed_scheduler.c").read().decode()
            assert KNOB not in baseline, "baseline already contains this change"
            source = temp/"scheduler.c"
            current = (runtime/"kage_vita_fullspeed_scheduler.c").read_text()
            objects = []
            for index, (text, flags) in enumerate(((baseline, []), (current, []), (current, [f"-D{KNOB}=0"]))):
                source.write_text(text)
                obj = temp/f"scheduler-{index}.o"
                run(common + flags + ["-c", str(source), "-o", str(obj)])
                data = obj.read_bytes()
                if data[:2] == b"\x64\x86": # COFF AMD64 TimeDateStamp only
                    data = data[:4] + b"\0"*4 + data[8:]
                objects.append(data)
            assert objects[0] == objects[1] == objects[2], "historical scheduler OFF drift"
            print("video priority OFF: PASS (fresh historical/absent/zero objects identical; baseline tar SHA " +
                  hashlib.sha256(args.baseline_tar.read_bytes()).hexdigest() + ")")


if __name__ == "__main__":
    main()
