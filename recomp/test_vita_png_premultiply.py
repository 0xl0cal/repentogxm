#!/usr/bin/env python3
"""Focused frozen ImagePng-loop differential, regenerated on every invocation."""
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
#include "host_vita_heap.h"
#include "host_vita_texel_scratch.h"
#include "host_vita_png_premultiply.h"
#include "kage_vita_texture_memory.h"
#include <psp2/kernel/sysmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
enum { BUFFER=0x70000000U, OBJECT=0x22000000U, STACK=0x23000000U,
       FRAME=STACK+0x8000U, LOCAL_BYTES=0x10000U, IMAGE_BYTES=0x860000U };
static uint8_t *pixels, *image, *object, *stackmem, *before, *expected;
static unsigned mem_live, object_live=1, release_ok=1, lease_live, lease_calls;
static unsigned image_valid=1, fault_calls, comparisons;
static guest_flags tail_flags;
static void fail(int line,const char *expr) {
    fprintf(stderr,"PNG premultiply line %d: %s\n",line,expr); exit(1);
}
#define CHECK(x) do { if (!(x)) fail(__LINE__,#x); } while(0)
int isaac_vita_png_premultiply_guest_try(CPU *__restrict c);
int guest_stack_violation(CPU *c,uint32_t pc,uint32_t kind,uint32_t addr,uint32_t n) {
    (void)c;(void)pc;(void)kind;(void)addr;(void)n; CHECK(0); return 0;
}
int guest_stack_owner_violation(CPU *c,uint32_t pc) {
    (void)c;(void)pc; CHECK(0); return 0;
}
void guest_fault(CPU *c,uint32_t addr,const char *why) {
    (void)addr; ++fault_calls; c->fault=why;
}
int guest_image_contains(uint32_t addr,uint32_t n) {
    return image_valid && n && addr>=GUEST_IMAGE_BASE &&
        (uint64_t)addr+n<=(uint64_t)GUEST_IMAGE_BASE+IMAGE_BYTES;
}
uint32_t isaac_vita_guest_heap_lease_exact_range(const void *base,const void *range,size_t n) {
    ++lease_calls; CHECK(!lease_live);
    if (!object_live || (uintptr_t)base!=OBJECT || range!=base || n!=0x8cu) return 0;
    lease_live=1; return 7;
}
int isaac_vita_guest_heap_lease_release(uint32_t token) {
    CHECK(token==7 && lease_live); lease_live=0; return (int)release_ok;
}
SceUID sceKernelAllocMemBlock(const char *name,SceKernelMemBlockType type,SceSize size,
                            SceKernelAllocMemBlockOpt *opt) {
    CHECK(!mem_live && name && type==SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && !opt);
    CHECK(size==KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES); mem_live=1; return 1;
}
int sceKernelGetMemBlockBase(SceUID uid,void **out) {
    CHECK(uid==1 && mem_live); *out=pixels; return 0;
}
int sceKernelFreeMemBlock(SceUID uid) { CHECK(uid==1 && mem_live); mem_live=0; return 0; }
void isaac_vita_log(const char *fmt,...) { (void)fmt; errno=ERANGE; }

static void *map_at(uint32_t base,size_t size) {
#ifdef _WIN32
    void *p=VirtualAlloc((void *)(uintptr_t)base,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#else
    void *p=mmap((void *)(uintptr_t)base,size,PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
#endif
    CHECK((uintptr_t)p==base); return p;
}
static void load_table(const char *path,uint32_t rva) {
    FILE *f=fopen(path,"rb"); CHECK(f);
    CHECK(fread(image+rva,1,65536,f)==65536); CHECK(fgetc(f)==EOF); fclose(f);
}
static void setup(CPU *c,unsigned w,unsigned h,unsigned pw,unsigned ph,unsigned gamma) {
    memset(c,0,sizeof *c); memset(object,0x8d,LOCAL_BYTES); memset(stackmem,0xa7,LOCAL_BYTES);
    c->stack_owner=c; c->stack_floor=STACK; c->stack_ceiling=STACK+LOCAL_BYTES;
    c->stack_low_water=FRAME-0x440u; c->esp=FRAME-0x440u; c->ebp=FRAME;
    c->edi=OBJECT; c->esi=OBJECT+0x80u; c->eax=0; c->ecx=0;
    c->ebx=0x78345612u; c->edx=0x11223344u;
    c->fl=(guest_flags){0};
    st32(FRAME+4,gamma ? 0x5a0d62u : 0x5a0c7eu); st32(FRAME-0x430u,OBJECT);
    st32(FRAME-0x428u,OBJECT+0x80u); st32(FRAME-0x424u,BUFFER);
    st32(FRAME-0x410u,0); st32(FRAME-0x418u,0x55667788u);
    st32(FRAME-0x41cu,0x12345678u); st32(OBJECT+0x18u,2);
    st16(OBJECT+0x80u,(uint16_t)w); st16(OBJECT+0x82u,(uint16_t)h);
    st16(OBJECT+0x84u,(uint16_t)pw); st16(OBJECT+0x86u,(uint16_t)ph);
    st8(GUEST_IMAGE_BASE+0x7c7a4fu,1); st8(GUEST_IMAGE_BASE+0x7c7a50u,(uint8_t)gamma);
}
static void acquire(unsigned bytes,unsigned owner) {
    isaac_vita_texel_scratch_decision d;
    CHECK(isaac_vita_texel_scratch_malloc(owner,bytes,STACK,STACK+LOCAL_BYTES,&d)==1);
    CHECK(d.pointer==pixels);
}
static void release_buffer(void) {
    isaac_vita_texel_scratch_decision d;
    CHECK(isaac_vita_texel_scratch_free(pixels,&d)==1);
}
#define __vita__ 1
/* FRESH_FUNCTIONS */

static void compare(unsigned w,unsigned h,unsigned pw,unsigned ph,unsigned gamma,unsigned seed) {
    CPU c,initial,answer; guest_flags flags; unsigned x,y,n=pw*ph*4u;
    uint8_t saved_stack[LOCAL_BYTES],answer_stack[LOCAL_BYTES],saved_object[LOCAL_BYTES];
    setup(&c,w,h,pw,ph,gamma); acquire(n,KAGE_VITA_TEXEL_PNG_LOADER_RETURN);
    memset(pixels,0xd3,n+32u);
    for(y=0;y<h;++y) for(x=0;x<w;++x) {
        uint8_t *p=pixels+(y*pw+x)*4u;
        p[0]=(uint8_t)x; p[1]=(uint8_t)(x+71u); p[2]=(uint8_t)(255u-x);
        p[3]=(uint8_t)(y+seed);
    }
    memcpy(before,pixels,n+32u); initial=c;
    memcpy(saved_stack,stackmem,LOCAL_BYTES); memcpy(saved_object,object,LOCAL_BYTES);
    original_loop(&c); answer=c; flags=tail_flags; memcpy(expected,pixels,n+32u);
    memcpy(answer_stack,stackmem,LOCAL_BYTES);
    c=initial; memcpy(pixels,before,n+32u); memcpy(stackmem,saved_stack,LOCAL_BYTES);
    lease_calls=0; errno=EDOM; candidate_loop(&c);
    CHECK(errno==EDOM && !lease_live && !fault_calls);
    CHECK(!memcmp(&c,&answer,sizeof c)); CHECK(!memcmp(&tail_flags,&flags,sizeof flags));
    CHECK(!memcmp(pixels,expected,n+32u)); CHECK(!memcmp(object,saved_object,LOCAL_BYTES));
    CHECK(!memcmp(stackmem,answer_stack,LOCAL_BYTES));
    CHECK(lease_calls==(unsigned)(h>0u));
    /* Compare the same continuation after a proven admitted direct prefix,
     * not merely an output-equivalent silent fallback. */
    if(w && h>=2u) {
        c=initial; memcpy(pixels,before,n+32u); memcpy(stackmem,saved_stack,LOCAL_BYTES);
        CHECK(isaac_vita_png_premultiply_guest_try(&c)==1);
        CHECK(c.ecx==h-1u && ld32(FRAME-0x410u)==h-1u);
        for(y=0;y<ph;++y) for(x=0;x<pw;++x) if(y>=h-1u || x>=w)
            CHECK(!memcmp(pixels+(y*pw+x)*4u,before+(y*pw+x)*4u,4u));
    }
    release_buffer(); ++comparisons;
}
static void reject_unchanged(CPU *c) {
    CPU old=*c; uint8_t saved_stack[LOCAL_BYTES];
    memcpy(saved_stack,stackmem,LOCAL_BYTES); memcpy(before,pixels,4096u);
    errno=EDOM; CHECK(isaac_vita_png_premultiply_guest_try(c)==0);
    CHECK(errno==EDOM && !memcmp(c,&old,sizeof old) && !lease_live);
    CHECK(!memcmp(before,pixels,4096u) && !memcmp(saved_stack,stackmem,LOCAL_BYTES));
}
static void guards(void) {
    CPU c; unsigned i;
    setup(&c,7,8,8,8,0); acquire(256u,KAGE_VITA_TEXEL_PNG_LOADER_RETURN);
    for(i=0;i<16;++i) {
        setup(&c,7,8,8,8,0);
        switch(i) {
        case 0:c.ecx=1;break; case 1:c.eax=1;break; case 2:c.esi++;break;
        case 3:st32(FRAME+4,0x1234);break; case 4:st32(FRAME-0x428u,OBJECT);break;
        case 5:c.stack_owner=NULL;break; case 6:st16(OBJECT+0x80u,0);break;
        case 7:st16(OBJECT+0x82u,1);break; case 8:st16(OBJECT+0x80u,9);break;
        case 9:st16(OBJECT+0x82u,9);break; case 10:st32(FRAME-0x424u,0xfffffffcu);break;
        case 11:st32(FRAME-0x424u,OBJECT);break; case 12:st32(FRAME-0x424u,STACK);break;
        case 13:st32(OBJECT+0x18u,1);break; case 14:object_live=0;break;
        case 15:image_valid=0;break;
        }
        reject_unchanged(&c); object_live=image_valid=1;
    }
    setup(&c,7,8,8,8,0);
    CHECK(isaac_vita_texel_scratch_oracle_hold_lock());
    reject_unchanged(&c); isaac_vita_texel_scratch_oracle_drop_lock();
    release_buffer(); reject_unchanged(&c);
    acquire(256u,KAGE_VITA_TEXEL_LOADER_RETURN_4); reject_unchanged(&c); release_buffer();
    acquire(512u,KAGE_VITA_TEXEL_PNG_LOADER_RETURN); reject_unchanged(&c); release_buffer();
    acquire(256u,KAGE_VITA_TEXEL_PNG_LOADER_RETURN);
    { IsaacVitaPngPremultiply p={BUFFER,256u,7u,7u,32u,pixels};
      memcpy(before,pixels,4096u);
      CHECK(!isaac_vita_texel_scratch_png_premultiply(&p));
      p.table=image+0x626a40u; p.rows=UINT32_MAX;
      CHECK(!isaac_vita_texel_scratch_png_premultiply(&p));
      CHECK(!memcmp(before,pixels,4096u)); }
    release_ok=0; CHECK(isaac_vita_png_premultiply_guest_try(&c)==-1);
    CHECK(c.fault && fault_calls==1u && !lease_live && c.ecx==0u);
    release_ok=1; release_buffer();
}
int main(int argc,char **argv) {
    unsigned g,w;
    CHECK(argc==3);
    pixels=map_at(BUFFER,KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES);
    image=map_at(GUEST_IMAGE_BASE,IMAGE_BYTES); object=map_at(OBJECT,LOCAL_BYTES);
    stackmem=map_at(STACK,LOCAL_BYTES); before=malloc(2u*1024u*1024u);
    expected=malloc(2u*1024u*1024u); CHECK(before && expected);
    load_table(argv[1],0x626a40u); load_table(argv[2],0x636a40u);
    for(g=0;g<2;++g) {
        compare(256,257,264,264,g,0); /* all256 alpha x all256 RGB inputs */
        for(w=1;w<=17;++w) compare(w,5,w+3,8,g,w*17u);
        compare(1,1,8,8,g,0); compare(0,3,8,8,g,0); compare(7,0,8,8,g,0);
        compare(7,3,8,8,g,254); /* partial/opaque/zero final rows */
    }
    guards();
    printf("PNG premultiply fresh-loop PASS: gpr=%d flags=%d guard=%d; "
           "%u cases, both65536-entry tables, guards/fault/ownership\n",
           GUEST_GPR_LOCAL,GUEST_FLAGS_LOCAL,GUEST_GENERATED_STACK_GUARD,comparisons);
    return 0;
}
'''

def run(command: list[str]) -> str:
    p = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if p.returncode:
        raise RuntimeError(f"command failed {command!r}\n{p.stdout}")
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
    img, pin = Image(str(args.pe), 0x98000000), Image(str(args.pe), DEFAULT_BASE)
    candidate = G._translate_function(img, {"rva": 0x5a0e10}, None, {}, pin_img=pin)
    proof = G.vita_png_premultiply_for_body
    G.vita_png_premultiply_for_body = lambda *unused: False
    original = G._translate_function(img, {"rva": 0x5a0e10}, None, {}, pin_img=pin)
    G.vita_png_premultiply_for_body = proof
    assert candidate["stub"] is None and candidate["vita_png_premultiply"]
    off_text, fence_count = re.subn(
        r"(?m)^#if defined\(__vita__\) && defined\(ISAAC_VITA_PNG_PREMULTIPLY_NATIVE\)\n"
        r"[\s\S]*?^#endif\n", "", candidate["text"])
    assert fence_count == 1 and off_text == original["text"], "OFF owner text changed"
    assert len(legacy_text(candidate["text"])) + candidate["legacy_size_delta"] == (
        len(legacy_text(original["text"])) + original["legacy_size_delta"])
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "image-png.fresh.c").write_text(candidate["text"], encoding="utf-8")
    (args.out / "image-png.original.fresh.c").write_text(original["text"], encoding="utf-8")
    emitted = ""
    for name, record in (("original_loop", original), ("candidate_loop", candidate)):
        source = record["text"]
        span = source[source.index("L_005a142c:"):source.index("L_005a152d:")]
        emitted += (f"static void {name}(CPU *c) {{\nGUEST_FLAGS_DECL; GUEST_GPR_DECL;\n" + span +
            "L_005a152d: tail_flags=*GUEST_FL; GUEST_GPR_FLUSH(c); return;\n"
            "L_guest_setjmp_cleanup_005a0e10: GUEST_GPR_FLUSH(c); return;\n}\n")
    fixture = args.out / "premultiply-comparison.c"
    fixture.write_text(FIXTURE.replace("/* FRESH_FUNCTIONS */", emitted), encoding="utf-8")
    tables = []
    for rva, digest in G.VITA_PNG_PREMULTIPLY_TABLES:
        data = img.code_at(rva, 65536)
        assert hashlib.sha256(data).hexdigest() == digest
        table = args.out / f"table-{rva:08x}.bin"
        table.write_bytes(data); tables.append(str(table))
    for gpr, flags, guard in ((0,0,0),(0,1,0),(1,0,0),(1,1,0),(1,1,1)):
        exe = args.out / f"premultiply-gpr{gpr}-flags{flags}-guard{guard}.exe"
        cmd = [args.cc,"-std=gnu11","-O2","-fno-strict-aliasing","-Wall","-Wextra",
            "-Werror","-Wno-unused-variable","-Wno-unused-label","-D_CRT_SECURE_NO_WARNINGS",
            "-DISAAC_VITA_TEXEL_SCRATCH_ORACLE=1","-DISAAC_VITA_PNG_PREMULTIPLY_NATIVE=1",
            "-DISAAC_VITA_HEAP_RANGE_LEASE=1",f"-DGUEST_GPR_LOCAL={gpr}",
            f"-DGUEST_FLAGS_LOCAL={flags}",f"-DGUEST_GENERATED_STACK_GUARD={guard}",
            "-DGUEST_IMAGE_BASE=0x98000000U","-I"+str(HERE/"runtime"),
            "-I"+str(HERE/"vita"),"-I"+str(HERE/"vita/anm2_scratch_oracle_include"),
            str(fixture),str(HERE/"runtime/host_vita_png_premultiply.c"),
            str(HERE/"runtime/host_vita_texel_scratch.c"),"-o",str(exe)]
        run(cmd); run([str(exe),*tables])
    print("PNG premultiply codegen/frozen tables/packing: PASS; native prefix + original final row")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
