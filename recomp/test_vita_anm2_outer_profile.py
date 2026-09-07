#!/usr/bin/env python3
"""Fresh whole-loader entry/cleanup proof and bounded runtime-scope checks."""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
PE_SHA = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"

FIXTURE = r'''
#include "guest.h"
#include "host_vita_anm2_outer_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <setjmp.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
enum { STACK=0x23000000u, BYTES=65536u };
static uint64_t now_us;
static unsigned clocks, violations, expect_depth;
static jmp_buf escape_env;
static void fail(int line,const char *what) {
    fprintf(stderr,"ANM2 outer line %d: %s\n",line,what); exit(1);
}
#define CHECK(x) do { if (!(x)) fail(__LINE__,#x); } while(0)
uint64_t sceKernelGetProcessTimeWide(void) { ++clocks; errno=ERANGE; return now_us; }
int guest_stack_violation(CPU *c,uint32_t pc,uint32_t kind,uint32_t addr,uint32_t size) {
    (void)c;(void)pc;(void)kind;(void)addr;(void)size; ++violations; return 0;
}
int guest_stack_owner_violation(CPU *c,uint32_t pc) {
    (void)c;(void)pc; CHECK(0); return 0;
}
static IsaacVitaAnm2OuterSnapshot snap(void) {
    IsaacVitaAnm2OuterSnapshot s; unsigned before=clocks; int e=errno;
    isaac_vita_anm2_outer_snapshot(&s); isaac_vita_anm2_outer_snapshot(NULL);
    CHECK(clocks==before && errno==e); return s;
}
static void work(void) { CHECK(snap().depth==expect_depth); now_us+=137u; }
/* FRESH_FUNCTIONS */

static void setup(CPU *c) {
    memset(c,0,sizeof *c); c->stack_owner=c;
    c->stack_floor=STACK; c->stack_ceiling=STACK+BYTES;
    c->stack_low_water=STACK+0x8000u; c->esp=STACK+0x8000u;
    c->eax=11; c->ecx=22; c->edx=33; c->ebx=44;
    c->ebp=55; c->esi=66; c->edi=77;
    c->fl.f_op=FLAG_PARTIAL; c->fl.f_cf=1; c->fl.f_zf=1;
    memset((void *)(uintptr_t)STACK,0xa3,BYTES);
    st32(c->esp,0x9995u);
}
static void reset(void) {
    isaac_vita_anm2_outer_oracle_reset(); now_us=1000; clocks=0; errno=EDOM;
}
static void generated_checks(void) {
    CPU c, original; IsaacVitaAnm2OuterSnapshot s; unsigned before;
    reset(); setup(&c); original=c; original.stack_owner=&original;
    expect_depth=0; original_return(&original);
    CHECK(clocks==0 && original.esp==c.esp+12u);
    expect_depth=1; generated_return(&c); s=snap();
    original.stack_owner=&c;
    CHECK(!memcmp(&original,&c,sizeof c));
    CHECK(clocks==2 && errno==EDOM && s.started==1 && s.completed==1 &&
          s.timed==1 && s.total_us==137u && s.max_us==137u && s.depth==0);
    /* Missing-file and success both reach this same original ret8: completed
     * is deliberately not named successful in the public ABI. */
    reset(); setup(&c); c.esp=STACK+4u; c.stack_low_water=c.esp;
    original=c; original.stack_owner=&original; violations=0;
    original_guard(&original); before=violations;
    generated_guard(&c); original.stack_owner=&c; s=snap();
    CHECK(!memcmp(&original,&c,sizeof c) && violations==2u*before);
    CHECK(clocks==2 && s.completed==1 && s.timed==1 && s.depth==0 && errno==EDOM);
#if GUEST_GENERATED_STACK_GUARD
    CHECK(before==1u);
#else
    CHECK(before==0u);
#endif
    reset(); setup(&c);
    if (setjmp(escape_env)==0) generated_escape(&c);
    CHECK(clocks==1 && snap().depth==1 && snap().completed==0);
    expect_depth=2; generated_return(&c); s=snap();
    CHECK(clocks==1 && s.started==1 && s.completed==0 && s.timed==0 &&
          s.depth==1 && s.nested==1); /* escaped outer cannot be completed later */
}
static void runtime_checks(void) {
    uint64_t a,b; IsaacVitaAnm2OuterSnapshot s;
    reset(); a=isaac_vita_anm2_outer_begin(); b=isaac_vita_anm2_outer_begin();
    CHECK(a && b && a!=b && clocks==1 && snap().nested==1 && snap().depth==2);
    now_us+=10; isaac_vita_anm2_outer_cleanup(&b);
    CHECK(clocks==1 && snap().depth==1 && snap().completed==0);
    now_us+=20; isaac_vita_anm2_outer_cleanup(&a); s=snap();
    CHECK(clocks==2 && s.total_us==30 && s.max_us==30 && s.completed==1 && errno==EDOM);
    a=isaac_vita_anm2_outer_begin(); now_us-=1; isaac_vita_anm2_outer_cleanup(&a); s=snap();
    CHECK(s.completed==2 && s.timed==1 && s.bad_clock==1 && s.total_us==30);
    reset(); a=isaac_vita_anm2_outer_begin(); b=isaac_vita_anm2_outer_begin();
    isaac_vita_anm2_outer_cleanup(&a); isaac_vita_anm2_outer_cleanup(&b);
    CHECK(!isaac_vita_anm2_outer_begin() && clocks==1 && snap().bad_sequence==1 &&
          snap().depth==2 && snap().completed==0);
    reset(); isaac_vita_anm2_outer_cleanup(NULL);
    CHECK(snap().bad_sequence==1 && !isaac_vita_anm2_outer_begin() && clocks==0);
    reset(); a=isaac_vita_anm2_outer_begin(); b=a;
    isaac_vita_anm2_outer_cleanup(&a); a=isaac_vita_anm2_outer_begin();
    isaac_vita_anm2_outer_cleanup(&b); s=snap();
    CHECK(s.bad_sequence==1 && s.completed==1 && s.depth==1 && clocks==3);
    reset(); s=snap(); s.total_us=UINT64_MAX-2u; s.started=UINT32_MAX;
    isaac_vita_anm2_outer_oracle_seed(&s,0); a=isaac_vita_anm2_outer_begin();
    now_us+=3u; isaac_vita_anm2_outer_cleanup(&a); s=snap();
    CHECK(s.total_us==UINT64_MAX && s.saturated && s.started==UINT32_MAX && s.timed==1);
    reset(); s=snap(); s.depth=UINT32_MAX;
    isaac_vita_anm2_outer_oracle_seed(&s,1); CHECK(!isaac_vita_anm2_outer_begin());
    CHECK(snap().saturated && snap().bad_sequence==1 && clocks==0);
    reset(); s=snap(); isaac_vita_anm2_outer_oracle_seed(&s,UINT32_MAX);
    CHECK(!isaac_vita_anm2_outer_begin() && snap().saturated && clocks==0);
}
int main(void) {
    void *p;
#ifdef _WIN32
    p=VirtualAlloc((void *)(uintptr_t)STACK,BYTES,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#else
    p=mmap((void *)(uintptr_t)STACK,BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
#endif
    CHECK((uintptr_t)p==STACK); generated_checks(); runtime_checks();
    printf("ANM2 outer fresh entry/ret8/guard/cleanup PASS: gpr=%d flags=%d guard=%d; "
           "CPU unchanged, errno, nested, nonlocal incomplete, saturation, no snapshot clocks\n",
           GUEST_GPR_LOCAL,GUEST_FLAGS_LOCAL,GUEST_GENERATED_STACK_GUARD);
    return 0;
}
'''

def run(cmd: list[str]) -> str:
    p = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if p.returncode:
        raise RuntimeError(f"command failed {cmd!r}\n{p.stdout}")
    print(p.stdout, end="")
    return p.stdout

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pe", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--cc", default="C:/Program Files/LLVM/bin/clang.exe")
    args = ap.parse_args()
    assert args.pe.stat().st_size == 8650240
    assert hashlib.sha256(args.pe.read_bytes()).hexdigest() == PE_SHA
    os.environ["REPENTOGXM_PE"] = str(args.pe)
    sys.path.insert(0, str(HERE))
    import gen_all as G
    from image import Image, DEFAULT_BASE
    from gpr_locals import legacy_text
    img, pin = Image(str(args.pe),0x98000000), Image(str(args.pe),DEFAULT_BASE)
    candidate = G._translate_function(img,{"rva":0x9b40},None,{},pin_img=pin)
    proof = G.vita_anm2_outer_for_body
    G.vita_anm2_outer_for_body = lambda *unused: False
    original = G._translate_function(img,{"rva":0x9b40},None,{},pin_img=pin)
    G.vita_anm2_outer_for_body = proof
    assert candidate["stub"] is None and candidate["vita_anm2_outer"]
    text = candidate["text"]
    off, n = re.subn(r"(?m)^#if defined\(__vita__\) && defined\(ISAAC_VITA_ANM2_WINDOW_PROFILE\)\n"
                    r"[\s\S]*?^#endif\n", "", text)
    assert n==1 and off==original["text"], "OFF body differs"
    assert text.count("return;")==G.VITA_ANM2_OUTER_C_RETURNS==144
    assert text.count("__attribute__((cleanup(isaac_vita_anm2_outer_cleanup)))")==1
    entry=text[:text.index("    /* 00009b40  push ebx */")]
    assert entry.count("{")==1 and entry.count("}")==0, "token not function-scope"
    assert len(legacy_text(text))+candidate["legacy_size_delta"] == (
        len(legacy_text(original["text"]))+original["legacy_size_delta"])
    callback=G._translate_function(img,{"rva":0x3f9370},None,{},pin_img=pin)
    G.validate_lua_callback_translation({"rva":0x3f9370,
        "instruction_count":callback["insns"]},callback)
    assert not callback["vita_anm2_outer"]
    args.out.mkdir(parents=True,exist_ok=True)
    (args.out/"anm2.fresh.c").write_text(text,encoding="utf-8")
    (args.out/"anm2.original.fresh.c").write_text(original["text"],encoding="utf-8")
    emitted=""
    for record,prefix in ((original,"original"),(candidate,"generated")):
        source=record["text"]
        head=source[:source.index("    /* 00009b40  push ebx */")]
        tail=source[source.index("    /* 0000e98c  ret 8 */"):]
        guard=source[source.index("    /* 00009b40  push ebx */"):
                     source.index("    /* 00009b46  and esp,")]
        emitted+=head.replace("sub_00009b40",prefix+"_return")+"    work();\n"+tail
        emitted+=head.replace("sub_00009b40",prefix+"_guard")+guard+"    GUEST_GPR_FLUSH(c); return;\n}\n"
        if prefix=="generated":
            emitted+=head.replace("sub_00009b40","generated_escape")+"    (void)c; longjmp(escape_env,1);\n}\n"
    fixture=args.out/"anm2-scope.c"
    fixture.write_text(FIXTURE.replace("/* FRESH_FUNCTIONS */",emitted),encoding="utf-8")
    common=[args.cc,"-std=gnu11","-fno-strict-aliasing","-Wall","-Wextra","-Werror",
        "-Wno-unused-variable","-Wno-unused-label","-D_CRT_SECURE_NO_WARNINGS",
        "-D__vita__=1","-DISAAC_VITA_ANM2_WINDOW_PROFILE=1",
        "-DISAAC_VITA_ANM2_OUTER_PROFILE_ORACLE=1","-I"+str(HERE/"runtime")]
    outputs=[]
    for gpr,flags,guard in ((0,0,0),(0,1,0),(1,0,0),(1,1,0),(1,1,1)):
        exe=args.out/f"anm2-gpr{gpr}-flags{flags}-guard{guard}.exe"
        cmd=common+["-O2",f"-DGUEST_GPR_LOCAL={gpr}",f"-DGUEST_FLAGS_LOCAL={flags}",
            f"-DGUEST_GENERATED_STACK_GUARD={guard}",str(fixture),
            str(HERE/"runtime/host_vita_anm2_outer_profile.c"),"-o",str(exe)]
        run(cmd); outputs.append(run([str(exe)]))
    # Compile the ENTIRE fresh body without mocked callees. At O0 Clang merges
    # every ordinary return into one cleanup-and-return block; unresolved guest
    # callees are declarations only, not emulated behavior or an executable.
    unit=args.out/"anm2-whole-compile.c"
    decls="".join(f"extern void {name}(CPU *);\n" for name in
                  sorted(set(re.findall(r"\bsub_[0-9a-f]{8}(?=\()",text)) - {"sub_00009b40"}))
    unit.write_text('#include "guest.h"\n'+decls+text,encoding="utf-8")
    for guard in (0,1):
        ir=args.out/f"anm2-whole-guard{guard}.ll"
        run(common+["-O0","-Wno-parentheses-equality","-DGUEST_GPR_LOCAL=1","-DGUEST_FLAGS_LOCAL=1",
            f"-DGUEST_GENERATED_STACK_GUARD={guard}","-S","-emit-llvm",str(unit),"-o",str(ir)])
        body=re.search(r"(?m)^define [^\n]*@sub_00009b40\([^\n]*\)[^\n]*\{\n([\s\S]*?)^}",
                       ir.read_text(encoding="utf-8")).group(1)
        assert len(re.findall(r"\bret void\b",body))==1
        assert len(re.findall(r"\bcall void @isaac_vita_anm2_outer_cleanup\(",body))==1
        lastblock=re.split(r"(?m)^\w[^\n]*:\s*[^\n]*\n",body)[-1]
        assert "@isaac_vita_anm2_outer_cleanup(" in lastblock and "ret void" in lastblock
    result="ANM2 outer fresh OFF identity/packing,144-return whole-body cleanup IR,Lua neutral: PASS\n"
    print(result,end=""); outputs.append(result)
    (args.out/"results.txt").write_text("".join(outputs),encoding="utf-8")
    return 0

if __name__=="__main__":
    raise SystemExit(main())
