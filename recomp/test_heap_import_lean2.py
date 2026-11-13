#!/usr/bin/env python3
"""ISAAC_VITA_HEAP_IMPORT_LEAN2 host gates (Windows Clang, no Vita).

1. OFF identity against BASE (the lane base): host_vita_heap.c preprocessed
   (-E -P) and whole COFF object (timestamp zeroed) under several define sets,
   each side compiled with its own host_vita_heap.h; plus preprocessed
   identity of three host_vita_heap.h includers.
2. runtime/heap_import_lean2_oracle.c: the LEAN_IMPORT-only comparator and
   LEAN_IMPORT+IMPORT_LEAN2(+VERIFY) give the same [lean2-trace],
   [lean2-retag] digest and [lean2-rollback] line, per family: the memblock
   ledger, the growing ledger (no LEDGER_MEMBLOCK) and the stage/cost/
   prefilter profile; lean2 leaves every sidecar zero (the comparator's
   slab-move retag stores the inherited token); VERIFY checks > 0 with zero
   disagreements; the banner names the resolved cuts; no log under the lock.
3. The existing deterministic trace oracle and the integrated router oracle
   (the modes that pass under LEAN_IMPORT) pass with LEAN2 and LEAN2+VERIFY.
4. Mutations of each cut are caught.
Timing on the host is not a Vita number.
"""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import test_vita_lua_runtime as R

ROOT = Path(__file__).resolve().parent
RT = ROOT / "runtime"
BASE = "d874a40397640d856791c65f0bfc7ff18314a159"  # int-020-20261112 (IA4)
LEAN = "-DISAAC_VITA_HEAP_LEAN_IMPORT=1"
L2 = "-DISAAC_VITA_HEAP_IMPORT_LEAN2=1"
L2V = "-DISAAC_VITA_HEAP_IMPORT_LEAN2_VERIFY=1"
PROFILE = ["-DISAAC_VITA_HEAP_STAGE_PROFILE=1", "-DISAAC_VITA_HEAP_FREE_COST_PROFILE=1",
           "-DISAAC_VITA_HEAP_FREE_PREFILTER=1"]
OWNERS = ["-DISAAC_VITA_ANM2_SCRATCH=1", "-DISAAC_VITA_TEXEL_SCRATCH=1"]
# copied: test_heap_stage_profile.py:28-37
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
# copied: test_heap_stage_profile.py:85-93
DEFS = ["-DGUEST_IMAGE_BASE=0x98000000u", "-DISAAC_VITA_HEAP_TESTING=1",
        "-DISAAC_VITA_HEAP_RANGE_LEASE=1", "-DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=1",
        "-DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=1", "-DISAAC_VITA_HEAP_OVERFLOW_MSPACE=1",
        "-DISAAC_VITA_HEAP_OVERFLOW_MSPACE_TESTING=1", "-DISAAC_VITA_ROOM_ENTRY_SLAB=1",
        "-DISAAC_VITA_ROOM_ENTRY_SLAB_TESTING=1", "-DISAAC_VITA_ROOM_ENTRY_HYBRID=1",
        "-DISAAC_VITA_ROOM_ENTRY_EXTERNAL_TESTING=1", "-DISAAC_VITA_HEAP_TERMINAL_FASTPATH=1",
        "-DISAAC_VITA_HEAP_SLAB_REJECT_FASTPATH=1",
        "-DISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES=0x017fd000U",
        "-DISAAC_VITA_HEAP_ORACLE_NO_LINKER_WRAP=1"]
# Overflow mspace (which IMPORT_LEAN2 requires) requires the memblock ledger,
# so every family keeps it; "nofast" drops the two fast paths whose presence
# turns the TERMINAL and SLABINL cuts on.
NOFAST = [d for d in DEFS if d not in ("-DISAAC_VITA_HEAP_TERMINAL_FASTPATH=1",
                                       "-DISAAC_VITA_HEAP_SLAB_REJECT_FASTPATH=1")]
MODULES = ["host_vita_heap.c", "host_vita_heap_overflow_mspace.c", "host_vita_heap_ledger_memblock.c",
           "host_vita_room_entry_slab.c", "host_vita_room_entry_external.c"]
FIXTURE_IO = ["-Dmalloc=fixture_malloc", "-Dcalloc=fixture_calloc",
              "-Drealloc=fixture_realloc", "-Dfree=fixture_free"]
ROUTER_IO = ["-Dmalloc=oracle_native_malloc", "-Dcalloc=oracle_native_calloc",
             "-Drealloc=oracle_native_realloc", "-Dfree=oracle_native_free"]


def need(ok: bool, msg: str) -> None:
    if not ok:
        raise AssertionError(msg)


class Tool:
    def __init__(self, work: Path):
        self.w = work
        self.clang = Path("C:/Program Files/LLVM/bin/clang.exe")
        need(self.clang.is_file(), "Clang must be installed")
        # One vcvars call; every compile then runs with its environment.
        out = subprocess.run(f"call {R.quote(R.find_vcvars())} x64 >nul && set", shell=True,
                             text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout
        self.env = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
        need("INCLUDE" in self.env and "LIB" in self.env, "vcvars environment missing")
        self.fake = work / "fake-include"
        self.cc = [str(self.clang), "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-D_CRT_SECURE_NO_WARNINGS", f"-I{self.fake}", f"-I{RT}", f"-I{ROOT / 'vita'}"]
        self.cache: dict[tuple, Path] = {}

    def run(self, cmd, cwd=None, ok=True, timeout=600) -> subprocess.CompletedProcess:
        p = subprocess.run(cmd, cwd=cwd or self.w, env=self.env, text=True,
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout)
        if ok:
            need(p.returncode == 0, f"command failed {p.returncode}: {cmd}\n{p.stdout[-4000:]}")
        return p

    def obj(self, src: Path, flags: list[str]) -> Path:
        key = (str(src), tuple(flags))
        if key not in self.cache:
            h = hashlib.sha256(repr(key).encode()).hexdigest()[:16]
            out = self.w / "obj" / f"{src.stem}-{h}.obj"
            out.parent.mkdir(exist_ok=True)
            self.run([*self.cc, *flags, "-c", str(src), "-o", str(out)])
            self.cache[key] = out
        return self.cache[key]

    def link(self, tag: str, objs: list[Path], extra: list[str] = ()) -> Path:
        exe = self.w / f"{tag}.exe"
        self.run([str(self.clang), *map(str, objs), str(self.w / "clock_fixture.c"), "-o", str(exe),
                  "-Xlinker", "/BASE:0x20000000", "-Xlinker", "/DYNAMICBASE:NO", *extra])
        return exe


def setup(w: Path) -> None:
    # copied: test_heap_stage_profile.py:58-80
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
    (w / "clock_fixture.c").write_text(CLOCK)


def identity(t: Tool) -> None:
    ident = t.w / "identity"
    ident.mkdir(exist_ok=True)
    git = lambda path: subprocess.run(["git", "show", f"{BASE}:{path}"], cwd=ROOT.parent, text=True,
                                      stdout=subprocess.PIPE, check=True).stdout
    base_c, base_h = git("recomp/runtime/host_vita_heap.c"), git("recomp/runtime/host_vita_heap.h")
    cur_c, cur_h = (RT / "host_vita_heap.c").read_text(), (RT / "host_vita_heap.h").read_text()
    need(base_h != cur_h and base_c != cur_c, "identity sides must differ in text")
    sets = {
        "lean-off": DEFS,
        "lean": [*DEFS, LEAN],
        "lean-profile": [*DEFS, LEAN, *PROFILE],
        "lean-owners": [*DEFS, LEAN, *OWNERS],
        "nofast-lean": [*NOFAST, LEAN],
        "minimal": ["-DGUEST_IMAGE_BASE=0x98000000u"],
    }
    # Same file names on both sides; the header sits next to the source so
    # each side includes its own copy ahead of -I runtime.
    for tag, flags in sets.items():
        res = {}
        for side, (c_text, h_text) in (("base", (base_c, base_h)), ("off", (cur_c, cur_h))):
            (ident / "host_vita_heap.c").write_text(c_text)
            (ident / "host_vita_heap.h").write_text(h_text)
            obj = ident / f"{side}-{tag}.obj"
            t.run([*t.cc, *flags, "-c", str(ident / "host_vita_heap.c"), "-o", str(obj)])
            data = obj.read_bytes()
            pre = t.run([*t.cc, *flags, "-E", "-P", str(ident / "host_vita_heap.c")]).stdout
            res[side] = (pre, data[:4] + b"\0\0\0\0" + data[8:])
        need(res["base"][0] == res["off"][0], f"OFF preprocess identity failed: {tag}")
        need(res["base"][1] == res["off"][1], f"OFF object identity failed: {tag}")
        print(f"OFF preprocessed + whole COFF object identity vs {BASE[:7]}: host_vita_heap.c [{tag}] PASS")
    for name in ("heap_fastpath_fixture.c", "host_vita_heap_overflow_router_oracle.c",
                 "host_vita_archive_xor.c"):
        res = {}
        for side, h_text in (("base", base_h), ("off", cur_h)):
            (ident / name).write_text((RT / name).read_text())
            (ident / "host_vita_heap.h").write_text(h_text)
            extra = ["-DISAAC_VITA_ARCHIVE_XOR_FASTPATH=1"] if name == "host_vita_archive_xor.c" else []
            res[side] = t.run([*t.cc, *DEFS, LEAN, *extra, "-E", "-P", str(ident / name)]).stdout
        need(res["base"] == res["off"], f"includer preprocess identity failed: {name}")
        print(f"OFF preprocessed identity of host_vita_heap.h includer vs {BASE[:7]}: {name} PASS")


def lean2_exe(t: Tool, tag: str, family: list[str], feature: list[str], heap: Path = RT / "host_vita_heap.c"):
    flags = [*family, *feature]
    objs = [t.obj(heap if name == "host_vita_heap.c" else RT / name,
                  [*flags, *FIXTURE_IO] if name == "host_vita_heap.c" else flags) for name in MODULES]
    objs += [t.obj(RT / "heap_fastpath_fixture.c", flags), t.obj(RT / "heap_import_lean2_oracle.c", flags)]
    return t.link(tag, objs)


def line(out: str, prefix: str) -> str:
    rows = [x for x in out.splitlines() if x.startswith(prefix)]
    need(len(rows) >= 1, f"missing {prefix!r} in output:\n{out[-3000:]}")
    return rows[0]


def field(row: str, name: str) -> str:
    m = re.search(rf"\b{name}=(\S+)", row)
    need(m is not None, f"no {name}= in {row!r}")
    return m.group(1)


def oracle_runs(t: Tool) -> None:
    families = {"mem": (DEFS, "1"), "nofast": (NOFAST, "0"), "profile": ([*DEFS, *PROFILE], "1")}
    for fam, (base_flags, fast) in families.items():
        outs = {}
        with ThreadPoolExecutor(max_workers=3) as pool:
            exes = dict(zip(("lean", "l2", "l2v"), pool.map(
                lambda f: lean2_exe(t, f"lean2-{fam}-{f[0]}", base_flags, f[1]),
                (("lean", [LEAN]), ("l2", [LEAN, L2]), ("l2v", [LEAN, L2, L2V])))))
        for mode, exe in exes.items():
            out = t.run([str(exe)], timeout=300).stdout
            (t.w / f"lean2-{fam}-{mode}.log").write_text(out)
            outs[mode] = out
            need("Heap import lean2 oracle: PASS" in out, f"{fam}/{mode} oracle failed:\n{out[-3000:]}")
        trace = {m: line(o, "[lean2-trace]") for m, o in outs.items()}
        need(trace["lean"] == trace["l2"] == trace["l2v"],
             f"{fam}: lean2 changed pointers/routes/faults/telemetry\n{trace}")
        retag = {m: line(o, "[lean2-retag] digest") for m, o in outs.items()}
        need(len({field(r, "digest") for r in retag.values()}) == 1, f"{fam}: retag digest differs {retag}")
        for m, r in retag.items():
            need(field(r, "retag") == "1" and field(r, "commit") == "1", f"{fam}/{m}: retag/commit {r}")
            inherited = field(r, "inherited")
            need(inherited != "0x0", f"{fam}/{m}: no inherited token to retag {r}")
            want = inherited if m == "lean" else "0x0"
            need(field(r, "final") == want, f"{fam}/{m}: final token {field(r, 'final')} != {want}")
        roll = {m: line(o, "[lean2-rollback]") for m, o in outs.items()}
        need(len(set(roll.values())) == 1, f"{fam}: rollback differs {roll}")
        need(line(outs["lean"], "[lean2]") == "[lean2] comparator (IMPORT_LEAN2 not compiled)",
             f"{fam}: comparator built lean2")
        for m in ("l2", "l2v"):
            out = outs[m]
            need(line(out, "[lean2-retag] live") == "[lean2-retag] live sidecar_nonzero=0",
                 f"{fam}/{m}: a slab-move token reached the sidecars")
            banner = line(out, "[lean2] guest heap lean2 import:")
            want = (f"mode={'VERIFY' if m == 'l2v' else 'ON'} sidecars=1 prefetch=1 terminal={fast} "
                    f"owner=0/0 reserve=1 slabinl={fast}")
            need(want in banner, f"{fam}/{m}: banner {banner!r} lacks {want!r}")
            stats = line(out, "[lean2] verify=")
            need(field(stats, "sidecar_nonzero") == "0", f"{fam}/{m}: {stats}")
            checks = [int(x) for x in field(stats, "checks").split("/")]
            bad = [int(x) for x in field(stats, "bad").split("/")]
            need(bad == [0, 0, 0, 0] and field(stats, "mismatch_lines") == "0", f"{fam}/{m}: {stats}")
            if m == "l2v":
                need(field(stats, "verify") == "1", stats)
                # sidecars / owner (no ANM2/texel owner linked here) / reserve / prefetch
                need(checks[0] > 0 and checks[1] == 0 and checks[2] > 0 and checks[3] > 0,
                     f"{fam}/l2v: VERIFY did not check every compiled cut {stats}")
            else:
                need(field(stats, "verify") == "0" and checks == [0, 0, 0, 0], stats)
            print(f"{fam}/{m}: {stats}")
        print(f"PASS {fam}: {trace['lean'].split()[1]} identical lean/lean2/lean2+verify; "
              f"retag final {field(retag['lean'], 'final')} -> 0x0 under lean2; rollback identical")


def existing_oracles(t: Tool) -> None:
    traces = {}
    for tag, feature in (("lean", [LEAN]), ("l2", [LEAN, L2]), ("l2v", [LEAN, L2, L2V])):
        flags = [*DEFS, *feature]
        objs = [t.obj(RT / n, [*flags, *FIXTURE_IO] if n == "host_vita_heap.c" else flags) for n in MODULES]
        objs += [t.obj(RT / n, flags) for n in ("heap_fastpath_fixture.c", "heap_fastpath_trace_oracle.c")]
        out = t.run([str(t.link(f"trace-{tag}", objs))], timeout=300).stdout
        (t.w / f"trace-{tag}.log").write_text(out)
        need("Heap slab reject trace/NULL/alias/boundary/domain/contention: PASS" in out, out[-2000:])
        traces[tag] = line(out, "[trace]")
        need("[floor] live_total=0" in out, f"{tag}: floor sidecars must stay off")
    need(len(set(traces.values())) == 1, f"trace oracle digest differs {traces}")
    print(f"PASS existing trace oracle identical lean/lean2/lean2+verify: {traces['lean']}")
    # copied: test_heap_stage_profile.py:148-163 (Windows adaptation of the router oracle)
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
    adapted = t.w / "windows-router-oracle.c"
    adapted.write_text(oracle)
    for tag, feature in (("l2", [LEAN, L2]), ("l2v", [LEAN, L2, L2V])):
        exe = router_exe(t, f"router-{tag}", adapted, [*DEFS, *feature])
        # The full and --room-commit-terminal modes assert floor totals (off
        # under lean); --room-cold-invariant-terminal needs GNU ld --wrap.
        for mode in (["--terminal-observer"], ["--room-invariant-terminal"]):
            print(f"{tag} {mode[0]}: " + t.run([str(exe), *mode], timeout=300).stdout.strip().splitlines()[-1])
    print("PASS integrated router oracle modes with lean2 and lean2+verify")
    return adapted


def router_exe(t: Tool, tag: str, adapted: Path, flags: list[str], heap: Path = RT / "host_vita_heap.c"):
    objs = []
    for name in MODULES + ["host_vita_heap_overflow_router_oracle.c"]:
        src = adapted if name == "host_vita_heap_overflow_router_oracle.c" else (
            heap if name == "host_vita_heap.c" else RT / name)
        objs.append(t.obj(src, [*flags, *ROUTER_IO] if name == "host_vita_heap.c" else flags))
    return t.link(tag, objs, ["-Xlinker", "/ALIGN:65536"])


def mutations(t: Tool) -> None:
    source = (RT / "host_vita_heap.c").read_text()
    publish = ("    s_ledger_tombstones = 0U;\n#if VITA_HEAP_LEAN2_PREFETCH\n"
               "    vita_heap_lean2_publish_ledger_locked();\n#endif\n    return 1;\n}")
    prefetch_addr = "    return base + (vita_heap_hash(key) & mask) * sizeof *s_ledger;"
    # The RESERVE cut has no mutation here: with the fixed memblock ledger
    # (required by overflow mspace) reserve(1) only compares the live count
    # with a 393216 limit this fixture cannot reach, so a dropped router
    # reserve is unobservable on the host; insert-reserved-drops-count
    # covers the body copied from vita_heap_ledger_insert.
    # Nor for the terminal test kept in the inline not-slab branch: the
    # import entry already faults on a terminal heap, so that second load
    # only matters when another thread turns the heap terminal between the
    # two loads (kept so the route matches the full slab route), which a
    # single-threaded oracle cannot schedule.  import-terminal-never covers
    # the macro both loads use.
    # (label, family, feature, (old, new, expected count), how it must show)
    cases = [
        ("retag-store-kept", DEFS, [LEAN, L2],
         ("#if !VITA_HEAP_LEAN2_SIDECARS\n    vita_heap_floor_lifetime_slot_store(",
          "#if 1\n    vita_heap_floor_lifetime_slot_store(", 1), "sidecar"),
        ("retag-store-kept-verify", DEFS, [LEAN, L2, L2V],
         ("#if !VITA_HEAP_LEAN2_SIDECARS\n    vita_heap_floor_lifetime_slot_store(",
          "#if 1\n    vita_heap_floor_lifetime_slot_store(", 1), "verify:sidecars"),
        ("publish-dropped", DEFS, [LEAN, L2, L2V],
         (publish, "    s_ledger_tombstones = 0U;\n    return 1;\n}", 2), "verify:prefetch"),
        # A prefetch is a hint, so a wrong address changes no trace; only the
        # VERIFY home-slot comparison can see it (review item 4).
        ("prefetch-hash-dropped", DEFS, [LEAN, L2, L2V],
         (prefetch_addr, "    return base + (key & mask) * sizeof *s_ledger;", 1), "verify:prefetch"),
        ("prefetch-stride-wrong", DEFS, [LEAN, L2, L2V],
         (prefetch_addr, "    return base + (vita_heap_hash(key) & mask) * sizeof(uintptr_t);", 1),
         "verify:prefetch"),
        ("insert-reserved-drops-count", DEFS, [LEAN, L2],
         ("    s_ledger[slot].requested_size = requested_size;\n    ++s_ledger_count;\n"
          "    floor_token = vita_heap_floor_lifetime_new_token_locked();\n"
          "    vita_heap_floor_lifetime_slot_insert_locked(\n        slot, requested_size, floor_token);\n"
          "    return 1;\n}\n#endif",
          "    s_ledger[slot].requested_size = requested_size;\n"
          "    floor_token = vita_heap_floor_lifetime_new_token_locked();\n"
          "    vita_heap_floor_lifetime_slot_insert_locked(\n        slot, requested_size, floor_token);\n"
          "    return 1;\n}\n#endif", 1), "trace"),
        ("slabinl-inverted", DEFS, [LEAN, L2],
         ("    if (vita_heap_definitely_not_slab((const void *)(uintptr_t)pointer)) {\n"
          "        VITA_HEAP_STAGE_BEGIN(F_SLAB)",
          "    if (!vita_heap_definitely_not_slab((const void *)(uintptr_t)pointer)) {\n"
          "        VITA_HEAP_STAGE_BEGIN(F_SLAB)", 1), "trace"),
        ("import-terminal-never", DEFS, [LEAN, L2],
         ("#define VITA_HEAP_IMPORT_TERMINAL() \\\n"
          "    atomic_load_explicit(&s_heap_terminal, memory_order_relaxed)",
          "#define VITA_HEAP_IMPORT_TERMINAL() 0", 1), "rollback"),
    ]
    lean_log = (t.w / "lean2-mem-lean.log").read_text()
    reference = line(lean_log, "[lean2-trace]")
    # Terminal-route mutations change only which check faults (the router
    # oracles accept any fault), so they are judged on the rollback line's
    # post-terminal free/malloc fault text against the lean comparator.
    terminal_reference = line(lean_log, "[lean2-rollback]")
    caught = 0
    for label, family, feature, (old, new, count), how in cases:
        need(source.count(old) == count, f"mutation anchor drift: {label} ({source.count(old)} != {count})")
        mutated = source.replace(old, new)
        need(mutated != source, label)
        src = t.w / f"mut-{label}" / "host_vita_heap.c"
        src.parent.mkdir(exist_ok=True)
        src.write_text(mutated)
        exe = lean2_exe(t, f"mut-{label}", family, feature, src)
        p = t.run([str(exe)], ok=False, timeout=300)
        out = p.stdout
        if how == "trace":
            hit = p.returncode != 0 or reference not in out
        elif how == "rollback":
            hit = p.returncode != 0 or terminal_reference not in out
        elif how == "sidecar":
            hit = p.returncode != 0 or "[lean2-retag] live sidecar_nonzero=0" not in out
        else:
            kind = how.split(":")[1]
            index = ["sidecars", "owner", "reserve", "prefetch"].index(kind)
            stats = next((x for x in out.splitlines() if x.startswith("[lean2] verify=")), "")
            bad = stats and int(field(stats, "bad").split("/")[index])
            hit = bool(bad) and f"FIRST MISMATCH kind={kind}" in out
        need(hit, f"mutation survived: {label}\n{p.stdout[-2500:]}")
        caught += 1
        print(f"mutation caught: {label} ({how})")
    print(f"PASS mutations: {caught}/{len(cases)} caught")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", type=Path, required=True)
    ap.add_argument("--skip-identity", action="store_true")
    w = ap.parse_args().work.resolve()
    w.mkdir(parents=True, exist_ok=True)
    setup(w)
    t = Tool(w)
    args = ap.parse_args()
    if not args.skip_identity:
        identity(t)
    oracle_runs(t)
    adapted = existing_oracles(t)
    mutations(t)
    print("PASS heap import lean2: OFF identity, lean2 == lean traces, zero sidecars, "
          "VERIFY clean, existing oracles, mutations")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as e:
        print(f"FAIL: {e}", file=sys.stderr)
        raise SystemExit(1)
