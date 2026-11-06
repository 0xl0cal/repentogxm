#!/usr/bin/env python3
"""ISAAC_VITA_HEAP_STAGE_PROFILE host gates (no Vita):
OFF preprocessed + whole-object identity of host_vita_heap.c and
kage_vita_phase_profile.c against IDENTITY_BASE; the deterministic heap trace is
identical OFF/ON; exact stage counts/span counts ON; the existing integrated
router oracle passes ON.  Timing values on the host are not Vita numbers.
"""
from __future__ import annotations
import argparse
from pathlib import Path
import re
import subprocess
import sys
import test_vita_lua_runtime as R

ROOT = Path(__file__).resolve().parent
RT = ROOT / "runtime"
BASE = "2d8404cd53b8f723158eb6e86390d38e1a53da6b"  # the heap-lean change's base
# kage_vita_phase_profile.c is compiled with GFUEL_SEED_NATIVE below, where a
# later integration legitimately added ph120.gn/gb (the decomp counters,
# merged at 8f851c3).  host_vita_heap.c is changed by no other line of work,
# so it keeps the heap-lean base.  A base commit that is not in this
# repository's history skips that file's identity leg.
PHASE_PROFILE_BASE = "8f851c3b7cfd66dc00e66629ab7c310e15fc247d"
IDENTITY_BASE = {"host_vita_heap.c": BASE, "kage_vita_phase_profile.c": PHASE_PROFILE_BASE}
FLAG = "-DISAAC_VITA_HEAP_STAGE_PROFILE=1"
COST = "-DISAAC_VITA_HEAP_FREE_COST_PROFILE=1"
PREFILTER = "-DISAAC_VITA_HEAP_FREE_PREFILTER=1"
CLOCK = r'''#include <stdint.h>
#include <windows.h>
/* Host stand-in for the Vita process clock (microseconds). */
uint64_t sceKernelGetProcessTimeWide(void)
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
    return (uint64_t)(t.QuadPart / (f.QuadPart / 1000000));
}
'''


def need(ok: bool, msg: str) -> None:
    if not ok:
        raise AssertionError(msg)


def run(cmd, cwd: Path) -> str:
    p = subprocess.run(cmd, cwd=cwd, shell=isinstance(cmd, str), text=True,
                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    need(p.returncode == 0, f"command failed {p.returncode}: {cmd}\n{p.stdout}")
    return p.stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", type=Path, required=True)
    w = ap.parse_args().work.resolve()
    w.mkdir(parents=True, exist_ok=True)
    fake = w / "fake-include"
    text = (ROOT / "vita/test_heap_overflow_router.sh").read_text()
    headers = re.findall(r'cat > "\$fake_include/([^\"]+)" <<\x27EOF\x27\n(.*?)\nEOF', text, re.S)
    need(len(headers) == 3, "fake native header closure changed")
    for name, body in headers:
        dest = fake / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(body + "\n")
    (fake / "pthread.h").write_text('''#include <windows.h>
#include <stdlib.h>
typedef struct pt_ctx {void *(*fn)(void *);void *arg,*result;} pt_ctx;
typedef struct pthread_t {HANDLE h;pt_ctx *ctx;} pthread_t;
static DWORD WINAPI pt_run(void *p){pt_ctx *c=p;c->result=c->fn(c->arg);return 0;}
static int pthread_create(pthread_t *t,void *attr,void *(*fn)(void *),void *arg){
 (void)attr;t->ctx=malloc(sizeof *t->ctx);if(!t->ctx)return 1;
 t->ctx->fn=fn;t->ctx->arg=arg;t->ctx->result=NULL;t->h=CreateThread(NULL,0,pt_run,t->ctx,0,NULL);
 if(!t->h){free(t->ctx);return 1;}return 0;}
static int pthread_join(pthread_t t,void **result){
 if(WaitForSingleObject(t.h,INFINITE)!=WAIT_OBJECT_0)return 1;
 if(result)*result=t.ctx->result;CloseHandle(t.h);free(t.ctx);return 0;}
''')
    clock = w / "clock_fixture.c"
    clock.write_text(CLOCK)
    clang = Path("C:/Program Files/LLVM/bin/clang.exe")
    need(clang.is_file(), "Clang must be installed")
    pre = f"call {R.quote(R.find_vcvars())} x64 >nul && "
    q = subprocess.list2cmdline
    defs = ["-DGUEST_IMAGE_BASE=0x98000000u", "-DISAAC_VITA_HEAP_TESTING=1",
            "-DISAAC_VITA_HEAP_RANGE_LEASE=1", "-DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=1",
            "-DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=1", "-DISAAC_VITA_HEAP_OVERFLOW_MSPACE=1",
            "-DISAAC_VITA_HEAP_OVERFLOW_MSPACE_TESTING=1", "-DISAAC_VITA_ROOM_ENTRY_SLAB=1",
            "-DISAAC_VITA_ROOM_ENTRY_SLAB_TESTING=1", "-DISAAC_VITA_ROOM_ENTRY_HYBRID=1",
            "-DISAAC_VITA_ROOM_ENTRY_EXTERNAL_TESTING=1", "-DISAAC_VITA_HEAP_TERMINAL_FASTPATH=1",
            "-DISAAC_VITA_HEAP_SLAB_REJECT_FASTPATH=1",
            "-DISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES=0x017fd000U",
            "-DISAAC_VITA_HEAP_ORACLE_NO_LINKER_WRAP=1"]
    cc = [str(clang), "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
          "-D_CRT_SECURE_NO_WARNINGS", f"-I{fake}", f"-I{RT}", f"-I{ROOT / 'vita'}"]
    profile_defs = ["-DISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE=1", "-DISAAC_VITA_PHASE_PROFILE=1",
                    '-DISAAC_VITA_PHASE_PROFILE_BUILD_ID="phase:oracle"',
                    "-DISAAC_VITA_GFUEL_SEED_NATIVE=1", "-DISAAC_VITA_CAMERA_RING_NATIVE=1"]
    # OFF identity: same source path for both sides, so file-name metadata is equal.
    ident = w / "identity"
    ident.mkdir(exist_ok=True)
    for name, extra in (("host_vita_heap.c", defs), ("kage_vita_phase_profile.c", profile_defs)):
        if subprocess.run(["git", "cat-file", "-e", IDENTITY_BASE[name] + "^{commit}"],
                          cwd=ROOT.parent, capture_output=True).returncode != 0:
            print(f"OFF identity vs {IDENTITY_BASE[name][:7]}: {name} SKIP "
                  "(base commit is not in this repository's history)")
            continue
        src = ident / name
        out = {}
        for tag, data in (("base", run(["git", "show", f"{IDENTITY_BASE[name]}:recomp/runtime/{name}"],
                                       ROOT.parent)),
                          ("off", (RT / name).read_text())):
            src.write_text(data)
            obj = ident / f"{tag}-{name}.obj"
            run(pre + q([*cc, *extra, "-c", str(src), "-o", str(obj)]), w)
            data_obj = obj.read_bytes()
            out[tag] = (run(pre + q([*cc, *extra, "-E", "-P", str(src)]), w),
                        data_obj[:4] + b"\0\0\0\0" + data_obj[8:])
        need(out["base"][0] == out["off"][0], f"OFF preprocess identity failed: {name}")
        need(out["base"][1] == out["off"][1], f"OFF object identity failed: {name}")
        print(f"OFF preprocessed + whole host COFF object identity vs {IDENTITY_BASE[name][:7]}: {name} PASS")
    modules = ["host_vita_heap.c", "host_vita_heap_overflow_mspace.c", "host_vita_heap_ledger_memblock.c",
               "host_vita_room_entry_slab.c", "host_vita_room_entry_external.c"]
    trace = {}
    # cost / cost-pf: ISAAC_VITA_HEAP_FREE_COST_PROFILE (ph120.hg/hv) on the
    # stage profile, without and with ISAAC_VITA_HEAP_FREE_PREFILTER (cost-pf
    # also lean, the recipe-A heap shape); the oracle pins its exact counts
    # and 1-in-64 sub-span rate.
    for tag, feature in (("off", []), ("on", [FLAG]), ("cost", [FLAG, COST]),
                         ("cost-pf", [FLAG, COST, PREFILTER, "-DISAAC_VITA_HEAP_LEAN_IMPORT=1"])):
        units = []
        for name in modules + ["heap_fastpath_fixture.c", "heap_fastpath_trace_oracle.c"]:
            obj = w / f"{tag}-{Path(name).stem}.obj"
            interpose = (["-Dmalloc=fixture_malloc", "-Dcalloc=fixture_calloc",
                          "-Drealloc=fixture_realloc", "-Dfree=fixture_free"]
                         if name == "host_vita_heap.c" else [])
            run(pre + q([*cc, *defs, *feature, *interpose, "-c", str(RT / name), "-o", str(obj)]), w)
            units.append(obj)
        exe = w / f"trace-{tag}.exe"
        run(pre + q([str(clang), *map(str, units), str(clock), "-o", str(exe),
                     "-Xlinker", "/BASE:0x20000000", "-Xlinker", "/DYNAMICBASE:NO"]), w)
        out = run([str(exe)], w)
        (w / f"trace-{tag}.log").write_text(out)
        print(out.strip())
        trace[tag] = next(x for x in out.splitlines() if x.startswith("[trace]"))
        need(("[stages]" in out) == (tag != "off"), "stage check ran in the wrong build")
        need(("[cost]" in out) == tag.startswith("cost"), "free cost check ran in the wrong build")
    need(trace["off"] == trace["on"], "stage profile changed pointers/ownership/accounting")
    need(trace["off"] == trace["cost"] == trace["cost-pf"],
         "free cost profile / prefilter changed pointers/ownership/accounting")
    print("PASS deterministic trace identical OFF/ON; exact stage/span counts ON")
    print("PASS free cost profile (+prefilter): trace identical to OFF; exact hg counts, 1-in-64 k/e/r")
    oracle = (RT / "host_vita_heap_overflow_router_oracle.c").read_text()
    decl = ("static _Alignas(ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGE_BYTES)\n"
            "unsigned char s_oracle_room_external_blocks[\n"
            "    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS][0x00110000U];")
    need(oracle.count(decl) == 1, "external fixture storage drift")
    oracle = oracle.replace(decl, "#include <windows.h>\n"
        "typedef unsigned char external_fixture[ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS][0x00110000U];\n"
        "static external_fixture *external_pointer;\n"
        "#define s_oracle_room_external_blocks (*external_pointer)")
    oracle = oracle.replace("int main(int argc, char **argv)", "static int existing_main(int argc, char **argv)")
    oracle += ("\nint main(int argc,char **argv){\n"
               " external_pointer=VirtualAlloc((void *)(uintptr_t)0x70000000U,sizeof(external_fixture),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);\n"
               " if((uintptr_t)external_pointer!=0x70000000U)return 90;\n"
               " return existing_main(argc,argv);\n}\n")
    adapted = w / "windows-router-oracle.c"
    adapted.write_text(oracle)
    units = []
    for name in modules + ["host_vita_heap_overflow_router_oracle.c"]:
        obj = w / f"router-{Path(name).stem}.obj"
        interpose = (["-Dmalloc=oracle_native_malloc", "-Dcalloc=oracle_native_calloc",
                      "-Drealloc=oracle_native_realloc", "-Dfree=oracle_native_free"]
                     if name == "host_vita_heap.c" else [])
        src = adapted if name == "host_vita_heap_overflow_router_oracle.c" else RT / name
        run(pre + q([*cc, *defs, FLAG, *interpose, "-c", str(src), "-o", str(obj)]), w)
        units.append(obj)
    exe = w / "router-on.exe"
    run(pre + q([str(clang), *map(str, units), str(clock), "-o", str(exe), "-Xlinker", "/BASE:0x20000000",
                 "-Xlinker", "/DYNAMICBASE:NO", "-Xlinker", "/ALIGN:65536"]), w)
    # --room-cold-invariant-terminal needs GNU ld --wrap (fails OFF on this leg too).
    for mode in ([], ["--terminal-observer"], ["--room-commit-terminal"], ["--room-invariant-terminal"]):
        print(run([str(exe), *mode], w).strip().splitlines()[-1])
    print("PASS existing integrated router oracle with the stage profile ON")
    # ISAAC_VITA_HEAP_LEAN_IMPORT: the trace digest (pointers, ledger, slab,
    # telemetry) equals OFF; floor totals drop to 0 (measurement-only data
    # off); router modes that do not assert floor totals pass; mutations of
    # the ledger work lean must keep are caught.
    LEAN = "-DISAAC_VITA_HEAP_LEAN_IMPORT=1"

    def trace_exe(tag, heap_src):
        units = []
        for name in modules + ["heap_fastpath_fixture.c", "heap_fastpath_trace_oracle.c"]:
            obj = w / f"{tag}-{Path(name).stem}.obj"
            src = heap_src if name == "host_vita_heap.c" else RT / name
            interpose = (["-Dmalloc=fixture_malloc", "-Dcalloc=fixture_calloc",
                          "-Drealloc=fixture_realloc", "-Dfree=fixture_free"]
                         if name == "host_vita_heap.c" else [])
            run(pre + q([*cc, *defs, LEAN, *interpose, "-c", str(src), "-o", str(obj)]), w)
            units.append(obj)
        exe = w / f"trace-{tag}.exe"
        run(pre + q([str(clang), *map(str, units), str(clock), "-o", str(exe),
                     "-Xlinker", "/BASE:0x20000000", "-Xlinker", "/DYNAMICBASE:NO"]), w)
        return exe

    off_out = (w / "trace-off.log").read_text()
    need("[floor] live_total=1" in off_out, "OFF floor totals must count the live block")
    out = run([str(trace_exe("lean", RT / "host_vita_heap.c"))], w)
    (w / "trace-lean.log").write_text(out)
    print(out.strip())
    need(next(x for x in out.splitlines() if x.startswith("[trace]")) == trace["off"],
         "lean import changed pointers/ownership/accounting")
    need("[floor] live_total=0" in out, "lean must skip the floor sidecars")
    source = (RT / "host_vita_heap.c").read_text()
    mutations = {
        "lean-drops-ledger-key": ("    s_ledger[slot].base = key;\n#ifdef ISAAC_VITA_HEAP_RANGE_LEASE\n",
                                  "#if !defined(ISAAC_VITA_HEAP_LEAN_IMPORT)\n    s_ledger[slot].base = key;\n"
                                  "#endif\n#ifdef ISAAC_VITA_HEAP_RANGE_LEASE\n"),
        "lean-keeps-ledger-count": (
            "s_ledger_floor_current);\n    ++s_ledger_tombstones;\n#endif\n    --s_ledger_count;\n}",
            "s_ledger_floor_current);\n    ++s_ledger_tombstones;\n#endif\n"
            "#if !defined(ISAAC_VITA_HEAP_LEAN_IMPORT)\n    --s_ledger_count;\n#endif\n}"),
    }
    for label, (old, new) in mutations.items():
        need(source.count(old) == 1, f"mutation anchor drift: {label}")
        src = w / f"mut-{label}" / "host_vita_heap.c"
        src.parent.mkdir(exist_ok=True)
        src.write_text(source.replace(old, new))
        p = subprocess.run([str(trace_exe(f"mut-{label}", src))], cwd=w, text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        caught = p.returncode != 0 or trace["off"] not in p.stdout
        need(caught, f"mutation survived: {label}")
        print(f"mutation caught: {label}")
    units = []
    for name in modules + ["host_vita_heap_overflow_router_oracle.c"]:
        obj = w / f"router-lean-{Path(name).stem}.obj"
        interpose = (["-Dmalloc=oracle_native_malloc", "-Dcalloc=oracle_native_calloc",
                      "-Drealloc=oracle_native_realloc", "-Dfree=oracle_native_free"]
                     if name == "host_vita_heap.c" else [])
        src = adapted if name == "host_vita_heap_overflow_router_oracle.c" else RT / name
        run(pre + q([*cc, *defs, LEAN, *interpose, "-c", str(src), "-o", str(obj)]), w)
        units.append(obj)
    exe = w / "router-lean.exe"
    run(pre + q([str(clang), *map(str, units), str(clock), "-o", str(exe), "-Xlinker", "/BASE:0x20000000",
                 "-Xlinker", "/DYNAMICBASE:NO", "-Xlinker", "/ALIGN:65536"]), w)
    # The full and --room-commit-terminal modes assert floor totals (now off).
    for mode in (["--terminal-observer"], ["--room-invariant-terminal"]):
        print(run([str(exe), *mode], w).strip().splitlines()[-1])
    print("PASS lean import: trace identical to OFF, floor sidecars off, 2/2 mutations caught, router modes")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as e:
        print(f"FAIL: {e}", file=sys.stderr)
        raise SystemExit(1)
