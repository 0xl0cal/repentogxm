#!/usr/bin/env python3
"""Regenerate native chains; execute actual fragment upload/restore branches.

This validates host call/state semantics, not native GXM execution or speed.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

import test_vitagl_stock_patch_chain as chain

RECIPE = Path(__file__).resolve().parent / "vitagl-stock-reference"


def braced(source: str, start: int) -> str:
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def branches(source: str) -> list[tuple[str, str]]:
    source = source.replace("\\\n", "\n")
    blocks = []
    for match in re.finditer(r"if \(p->frag_uniforms && dirty_shader_frag_unifs\) \{", source):
        fragment = braced(source, match.start())
        ubo_start = source.index("if (p->frag_ubos) {", match.end())
        blocks.append((fragment, braced(source, ubo_start)))
    assert len(blocks) == 2, "both native FFP macro variants must be tested"
    return blocks


PRELUDE = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GL_FALSE 0
#define GL_TRUE 1
#define THREAD_SAFE()
#define SKIP_ERROR_HANDLING 1
#define UNIFORM_CIRCULAR_POOL_SIZE (2 * 1024 * 1024)
typedef struct { uint32_t unif_buf_size; } shader;
typedef unsigned GLuint;
typedef struct ubo { struct ubo *chain; unsigned idx, bind; } ubo;
typedef struct { void *ptr; unsigned last_frame; } buffer;
typedef struct { shader *fshader; void *frag_uniforms, *unif_fbuffer; ubo *frag_ubos; } program;
static unsigned checks, binds, copies, copied_bytes, block_binds, dirty_shader_frag_unifs;
static unsigned cur_program, dirty_shader_vert_unifs, dirty_vert_unifs, dirty_frag_unifs;
static unsigned vgl_framecount = 17;
static unsigned char pool[UNIFORM_CIRCULAR_POOL_SIZE], payload[128], marker;
static uint8_t *unif_pool = pool;
static uint32_t unif_idx;
static void *vgl_def_frag_buf, *context_buffer, *gxm_context;
static buffer block_storage[3];
static buffer *ubo_buf[] = { &block_storage[0], &block_storage[1], &block_storage[2] };
static unsigned ubo_offset[3];
static void *bound_blocks[14];
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static int sceGxmSetFragmentDefaultUniformBuffer(void *ctx, const void *data) {
 (void)ctx; CHECK(data != NULL); ++binds; context_buffer=(void *)data; return 0;
}
static int sceGxmSetFragmentUniformBuffer(void *ctx, unsigned idx, const void *data) {
 (void)ctx; CHECK(idx < 14 && data != NULL); ++block_binds; bound_blocks[idx]=(void *)data; return 0;
}
static void *vgl_fast_memcpy(void *dst, const void *src, size_t n) {
 ++copies; copied_bytes+=(unsigned)n;
 /* Stock sceClibMemcpy accepts this zero-copy path; do not feed NULL to libc. */
 return n ? memcpy(dst,src,n) : dst;
}
'''

TESTS = r'''
typedef void (*upload_fn)(program *);
static void reset(void) {
 binds=copies=copied_bytes=block_binds=0; dirty_shader_frag_unifs=1; unif_idx=0;
 vgl_def_frag_buf=context_buffer=NULL; memset(pool,0xa5,sizeof pool);
 memset(bound_blocks,0,sizeof bound_blocks); memset(block_storage,0,sizeof block_storage);
 for (unsigned i=0;i<sizeof payload;++i) payload[i]=(unsigned char)(i^0x5a);
}
static void verify_render(const shader *s) {
 /* Native contract: size zero consumes no default-buffer bytes. */
 if (s->unif_buf_size) CHECK(context_buffer && memcmp(context_buffer,payload,s->unif_buf_size)==0);
}
static void run_cases(upload_fn upload, unsigned skips_zero) {
 shader s={0}; program p={&s,&marker,NULL,NULL};
 reset(); upload(&p); /* first-ever draw has no pre-existing fragment buffer */
 CHECK(dirty_shader_frag_unifs==0); CHECK(binds==!skips_zero && copies==!skips_zero);
 CHECK(copied_bytes==0 && unif_idx==0); CHECK((context_buffer==NULL)==!!skips_zero);
 verify_render(&s);
 unsigned before=binds; upload(&p); CHECK(binds==before); /* clean means no upload */
 /* A clear before any nonempty custom FS leaves no saved buffer to restore. */
 context_buffer=&marker; before=binds; vglRestoreFragmentUniformBuffer();
 CHECK(binds-before==!skips_zero); verify_render(&s);
 /* A clean shader with no reflected uniforms retains the original dirty flag. */
 reset(); p.frag_uniforms=NULL; upload(&p); CHECK(dirty_shader_frag_unifs==1 && binds==0);
 p.frag_uniforms=&marker;
 for (unsigned n=1;n<=128;n*=2) {
  reset(); s.unif_buf_size=n; p.unif_fbuffer=payload; upload(&p);
  CHECK(binds==1 && copies==1 && copied_bytes==n && unif_idx==n);
  CHECK(dirty_shader_frag_unifs==0); verify_render(&s);
 }
 /* nonzero -> zero -> clear/restore -> zero -> nonzero, repeated across wrap */
 for (unsigned iteration=0;iteration<64;++iteration) {
  reset(); unif_idx=UNIFORM_CIRCULAR_POOL_SIZE-32;
  s.unif_buf_size=64; p.unif_fbuffer=payload; upload(&p); verify_render(&s);
  CHECK(unif_idx==64); void *previous=vgl_def_frag_buf;
  s.unif_buf_size=0; p.unif_fbuffer=NULL; glUseProgram(2); before=binds;
  CHECK(cur_program==2 && dirty_shader_frag_unifs && dirty_shader_vert_unifs);
  upload(&p); CHECK(binds-before==!skips_zero); CHECK(unif_idx==64); verify_render(&s);
  if (skips_zero) CHECK(vgl_def_frag_buf==previous && context_buffer==previous);
  /* Existing native temporary clear buffer and actual restore are unchanged. */
  context_buffer=&marker; before=binds; vglRestoreFragmentUniformBuffer();
  CHECK(binds==before+1 && context_buffer==vgl_def_frag_buf); verify_render(&s);
  glUseProgram(2); upload(&p); CHECK(dirty_shader_frag_unifs==0);
  s.unif_buf_size=64; p.unif_fbuffer=payload; glUseProgram(1);
  before=binds; upload(&p); CHECK(binds==before+1 && unif_idx==128); verify_render(&s);
 }
 /* Named UBOs bind on every draw, even with a zero-size or clean default UBO. */
 reset(); s.unif_buf_size=0; p.unif_fbuffer=NULL;
 ubo tail={NULL,9,1}, head={&tail,2,0}; p.frag_ubos=&head;
 block_storage[0].ptr=payload; block_storage[1].ptr=payload+32;
 ubo_offset[0]=4; ubo_offset[1]=8; upload(&p);
 CHECK(block_binds==2 && bound_blocks[2]==payload+4 && bound_blocks[9]==payload+40);
 CHECK(block_storage[0].last_frame==vgl_framecount && block_storage[1].last_frame==vgl_framecount);
 ubo_offset[1]=16; upload(&p); CHECK(block_binds==4 && bound_blocks[9]==payload+48);
 CHECK(dirty_shader_frag_unifs==0); p.frag_ubos=NULL;
}
int main(void) {
 run_cases(upload_reference,0);
#ifdef HAVE_ISAAC_SKIP_ZERO_FRAGMENT_UNIFORM
 run_cases(upload_actual_nonffp,1); run_cases(upload_actual_ffp,1);
#else
 run_cases(upload_actual_nonffp,0); run_cases(upload_actual_ffp,0);
#endif
 printf("Actual fragment default UBO branches: %u checks PASS; skip-zero=%d\n", checks,
#ifdef HAVE_ISAAC_SKIP_ZERO_FRAGMENT_UNIFORM
 1
#else
 0
#endif
 ); return 0;
}
'''


def fixture(before: str, after: str, native: Path) -> str:
    old = branches(before)
    new = branches(after)
    assert old[0] == old[1] and new[0] == new[1]
    assert old[0][1] == new[0][1], "named UBO loop must remain byte-identical"
    # Exactly the two fragment branches and the guarded size macro may change.
    macro_start=after.index("/* A sampler is reflected in frag_uniforms")
    macro_end=after.index("#ifndef HAVE_FFP_SHADER_SUPPORT", macro_start)
    stripped=after[:macro_start]+after[macro_end:]
    for original, modified in zip(old, new):
        original_cont=original[0].replace("\n", "\\\n")
        modified_cont=modified[0].replace("\n", "\\\n")
        stripped=stripped.replace(modified_cont,original_cont,1)
    assert stripped==before, "unexpected native source edit beyond the exact fragment upload"
    gxm=(native/"source/utils/gxm_utils.c").read_text(encoding="utf-8")
    header=(native/"source/utils/gxm_utils.h").read_text(encoding="utf-8")
    reserve=braced(gxm,gxm.index("void *vglReserveUniformCircularPoolBuffer("))
    helper_names=("vglRestoreFragmentUniformBuffer", "vglReserveFragmentUniformBuffer")
    helpers=[]
    for name in helper_names:
        start=header.rfind("static inline",0,header.index(name+"("))
        helpers.append(braced(header,start))
    helpers.append(braced(after, after.index("void glUseProgram(")))
    funcs=["static void upload_reference(program *p) {\n"+"\n".join(old[0])+"\n}"]
    for suffix, actual in zip(("nonffp","ffp"),new):
        funcs.append("static void upload_actual_"+suffix+"(program *p) {\n"+"\n".join(actual)+"\n}")
    return PRELUDE+reserve+"\n"+"\n".join(helpers)+"\n"+after[macro_start:macro_end]+"\n"+"\n".join(funcs)+TESTS


def main() -> None:
    parser=argparse.ArgumentParser()
    parser.add_argument("--stock-tar",required=True,type=Path)
    parser.add_argument("--cc",default=os.environ.get("CC","cc"))
    parser.add_argument("--output",required=True,type=Path)
    parser.add_argument("--derive-hashes",action="store_true",
                        help="print identities before build.sh's table is populated")
    args=parser.parse_args()
    assert chain.sha256(args.stock_tar)==chain.SOURCE_ARCHIVE_SHA256
    args.output.mkdir(parents=True,exist_ok=True)
    patches={int(p.name[:4]):p for p in RECIPE.glob("[0-9][0-9][0-9][0-9]-*.patch")}
    stages={"stock":(),"draw":(2,),"color":(2,3),"probe":(2,3,8),
            "staging":(2,3,8,12),"plain":(2,3,8,12,13),"single":(2,3,8,12,14),
            "plain-single":(2,3,8,12,13,14),"pair":(2,3,8,12,13,14,15),
            "halo":(2,3,8,12,13,14,16,17),"halo-pair":(2,3,8,12,13,14,15,16,17),
            "transform":(2,3,8,12,13,14,15,18),
            "transform-halo":(2,3,8,12,13,14,15,16,17,18),
            "atlas":(2,3,8,12,13,14,15,16,17,19),
            "atlas-transform":(2,3,8,12,13,14,15,16,17,18,19)}
    build=(RECIPE/"build.sh").read_text(encoding="utf-8")
    rows=[]; canonical=None; table={}
    with tempfile.TemporaryDirectory(prefix="isaac-zero-fragment-") as temporary:
        for cache in (False,True):
            for link in (False,True):
                for stage,optional in stages.items():
                    if link and 3 not in optional: continue
                    if not cache and (16 in optional or 19 in optional): continue
                    native=chain.materialize_stock(Path(temporary)/f"{stage}-c{int(cache)}-l{int(link)}",args.stock_tar,None)
                    order=[1]+[p for p in optional if p<10]
                    if cache: order.append(4)
                    order.extend(p for p in optional if p>=10)
                    if link: order.append(22)
                    for number in order: chain.apply_patch(native,patches[number])
                    path=native/"source/custom_shaders.c"
                    before=path.read_text(encoding="utf-8"); old_hash=chain.sha256(path)
                    chain.apply_patch(native,patches[23])
                    after=path.read_text(encoding="utf-8"); new_hash=chain.sha256(path)
                    candidate=fixture(before,after,native)
                    if canonical is None: canonical=candidate
                    assert canonical==candidate,"actual branch differs between optional native chains"
                    table[old_hash]=new_hash
                    if not args.derive_hashes and (cache or stage=="transform"):
                        assert f"{old_hash}) shader_source_sha={new_hash} ;;" in build
                    rows.append(f"{stage} cache={int(cache)} link={int(link)} {old_hash} {new_hash}")
        assert canonical is not None
        fixture_path=args.output/"actual-zero-fragment-uniform.c"
        fixture_path.write_text(canonical,encoding="utf-8",newline="\n")
        for enabled in (False,True):
            output=args.output/f"zero-fragment-uniform-{int(enabled)}.exe"
            options=["-DHAVE_ISAAC_SKIP_ZERO_FRAGMENT_UNIFORM=1"] if enabled else []
            subprocess.run([args.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror",*options,
                            str(fixture_path),"-o",str(output)],check=True)
            subprocess.run([str(output)],check=True)
    if not args.derive_hashes:
        for needle in ('skip_zero_fragment_uniform=${ISAAC_SKIP_ZERO_FRAGMENT_UNIFORM:-0}',
                       '"skip_zero_fragment_uniform:$skip_zero_fragment_uniform"',
                       '"$skip_zero_fragment_uniform" "$skip_zero_fragment_uniform_patch_sha"',
                       'build_flags="$build_flags HAVE_ISAAC_SKIP_ZERO_FRAGMENT_UNIFORM=1"',
                       'extra_cflags="$extra_cflags -DHAVE_ISAAC_SKIP_ZERO_FRAGMENT_UNIFORM=1"',
                       'apply_source_patch "$skip_zero_fragment_uniform_patch"'):
            assert needle in build,needle
    (args.output/"source-hashes.txt").write_text("\n".join(rows)+"\n",encoding="utf-8")
    print(f"Regenerated native zero-fragment upload chains: {len(rows)} PASS")
    for old_hash,new_hash in sorted(table.items()):
        print(f"{old_hash}) shader_source_sha={new_hash} ;;")


if __name__=="__main__":
    main()
