#!/usr/bin/env python3
"""Regenerate pinned native chains and exercise their actual link/draw helpers.

This is a host lifecycle/reflection-call test, not a GPU or FPS benchmark.
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
PATCH = RECIPE / "0022-isaac-coloroffset-link-proof.patch"


def function(source: str, name: str) -> str:
    match = re.search(r"(?m)^(?:static )?(?:inline )?(?:GLboolean|void|int32_t) "
                      + re.escape(name) + r"\s*\(", source)
    if not match:
        raise AssertionError(f"missing actual native function {name}")
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def require_mutation_contract(source: str) -> None:
    for name in ("glShaderSource", "glShaderBinary", "vglShaderGxpBinary"):
        body = function(source, name)
        for needle in ("isaac_coloroffset_source_exact[",
                       "isaac_coloroffset_compile_exact[",
                       "isaac_coloroffset_compiled_generation[",
                       "isaac_coloroffset_advance_shader_generation("):
            assert needle in body, (name, needle)
    compile_body = function(source, "glCompileShader")
    assert compile_body.count("isaac_coloroffset_note_compile(s);") == 2
    # The vanilla cache's successful load and the ordinary compile both notify.
    assert re.search(r"unserialize_shader\(buf, sz, s, GL_FALSE\);.*?"
                     r"isaac_coloroffset_note_compile\(s\);.*?return;",
                     compile_body, re.S)
    assert re.search(r"vgl_compile_shader\(s, GL_FALSE\);\s*"
                     r"#ifdef HAVE_ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS\s*"
                     r"isaac_coloroffset_note_compile\(s\);", compile_body)
    # The authenticated cache's commit is followed by the same notification.
    cache = (RECIPE / "isaac_shader_cache_vitagl.h").read_text(encoding="utf-8")
    commit = cache[cache.index("s->prog = (const SceGxmProgram *)record.body;"):]
    assert commit.index("isaac_coloroffset_note_compile(s);") < commit.index("return 1;")
    create = source[source.index("GLuint glCreateProgram("):source.index("void glDeleteProgram(")]
    assert "progs[i].isaac_coloroffset_link_sampler_program = NULL;" in create
    delete = function(source, "glDeleteProgram")
    assert delete.index("p->isaac_coloroffset_link_sampler_program = NULL;") < delete.index("if (p->status)")
    link = function(source, "glLinkProgram")
    assert link.index("p->isaac_coloroffset_link_sampler_program = NULL;") < link.index("return;")
    attach = function(source, "glAttachShader")
    assert "p->status == PROG_UNLINKED && s->valid" in attach
    delete_shader = function(source, "glDeleteShader")
    assert "if (s->ref_counter > 0)" in delete_shader and "s->dirty = GL_TRUE;" in delete_shader
    create_shader = source[source.index("GLuint glCreateShader("):source.index("void glGetShaderiv(")]
    for needle in ("isaac_coloroffset_source_exact[res - 1] = 0;",
                   "isaac_coloroffset_compile_exact[res - 1] = 0;",
                   "isaac_coloroffset_source_generation[res - 1] = 1u;",
                   "isaac_coloroffset_compiled_generation[res - 1] = 0;"):
        assert needle in create_shader


PRELUDE = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "isaac_coloroffset_gpu_policy.h"
#define HAVE_ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS 1
#define GL_FALSE 0
#define GL_TRUE 1
#define PROG_LINKED 2
#define PROG_UNLINKED 1
#define MAX_CUSTOM_SHADERS 4
#define VGL_MODE_POSTPONED 3
#define THREAD_SAFE()
#define vgl_log(...) ((void)0)
#define ISAAC_COLOROFFSET_COUNT(x) ((void)0)
#define SCE_GXM_PARAMETER_CATEGORY_SAMPLER 2
typedef int GLboolean;
typedef uint32_t GLuint;
typedef struct { uint32_t category, resource, size; } SceGxmProgramParameter;
typedef struct { uint32_t count; int exists, exact; SceGxmProgramParameter tex; } SceGxmProgram;
typedef struct { const SceGxmProgram *prog; uintptr_t id; int is_glsl, valid, dirty, ref_counter; const char *source; } shader;
typedef struct {
 int status; shader *fshader, *vshader;
 uintptr_t isaac_coloroffset_link_fragment_id;
 const SceGxmProgram *isaac_coloroffset_link_sampler_program;
 const SceGxmProgram *isaac_coloroffset_link_vertex_program;
 uint32_t isaac_coloroffset_link_source_generation, isaac_coloroffset_link_vertex_generation;
 uint8_t isaac_coloroffset_link_vertex_exact;
} program;
static shader shaders[MAX_CUSTOM_SHADERS];
static program progs[1];
static uint8_t isaac_coloroffset_source_exact[MAX_CUSTOM_SHADERS];
static uint8_t isaac_coloroffset_compile_exact[MAX_CUSTOM_SHADERS];
static uint32_t isaac_coloroffset_source_generation[MAX_CUSTOM_SHADERS];
static uint32_t isaac_coloroffset_compiled_generation[MAX_CUSTOM_SHADERS];
static int glsl_sema_mode;
static unsigned queries, checks;
static SceGxmProgram fs, vs, other;
static uint32_t sceGxmProgramGetParameterCount(const SceGxmProgram *p) { ++queries; return p->count; }
static const SceGxmProgramParameter *sceGxmProgramFindParameterByName(const SceGxmProgram *p, const char *n) { ++queries; return p->exists && strcmp(n, "Texture0") == 0 ? &p->tex : NULL; }
static uint32_t sceGxmProgramParameterGetCategory(const SceGxmProgramParameter *p) { ++queries; return p->category; }
static uint32_t sceGxmProgramParameterGetResourceIndex(const SceGxmProgramParameter *p) { ++queries; return p->resource; }
static uint32_t sceGxmProgramParameterGetArraySize(const SceGxmProgramParameter *p) { ++queries; return p->size; }
static GLboolean isaac_coloroffset_fragment_shader_is_exact(const shader *s) { return s && s->prog && s->prog->exact; }
static GLboolean isaac_coloroffset_vertex_shader_is_exact(const shader *s) { return s && s->prog && s->prog->exact; }
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
'''


TESTS = r'''
static void reset(void) {
 memset(shaders, 0, sizeof shaders); memset(progs, 0, sizeof progs);
 memset(isaac_coloroffset_source_exact, 1, sizeof isaac_coloroffset_source_exact);
 memset(isaac_coloroffset_compile_exact, 1, sizeof isaac_coloroffset_compile_exact);
 for (unsigned i=0; i<MAX_CUSTOM_SHADERS; ++i) {
  isaac_coloroffset_source_generation[i]=10+i;
  isaac_coloroffset_compiled_generation[i]=10+i;
 }
 fs=(SceGxmProgram){1,1,1,{2,0,1}}; vs=fs; other=fs;
 shaders[0].prog=&fs; shaders[0].id=1; shaders[0].valid=1;
 shaders[1].prog=&vs; shaders[1].id=2; shaders[1].valid=1;
 progs[0].status=PROG_LINKED; progs[0].fshader=&shaders[0]; progs[0].vshader=&shaders[1];
 glsl_sema_mode=0; queries=0; isaac_coloroffset_record_link(&progs[0]);
}
static void expected_reject(void) {
 unsigned before=queries;
 CHECK(!isaac_coloroffset_fragment_program_is_exact(&progs[0]));
 CHECK(queries==before); /* no metadata calls after a failed runtime identity gate */
}
int main(void) {
 reset();
#ifdef HAVE_ISAAC_COLOROFFSET_LINK_PROOF
 CHECK(queries==5); CHECK(progs[0].isaac_coloroffset_link_sampler_program==&fs);
#else
 CHECK(queries==0);
#endif
 queries=0;
 for (unsigned i=0;i<1000;++i) CHECK(isaac_coloroffset_fragment_program_is_exact(&progs[0]));
#ifdef HAVE_ISAAC_COLOROFFSET_LINK_PROOF
 CHECK(queries==0);
#else
 CHECK(queries==5000);
#endif
 /* Every existing runtime gate, independently. */
 reset(); progs[0].status=PROG_UNLINKED; expected_reject();
 reset(); progs[0].fshader=NULL; expected_reject();
 reset(); progs[0].vshader=NULL; expected_reject();
 reset(); shaders[0].prog=NULL; expected_reject();
 reset(); shaders[1].prog=NULL; expected_reject();
 reset(); isaac_coloroffset_source_exact[0]=0; expected_reject();
 reset(); isaac_coloroffset_compile_exact[0]=0; expected_reject();
 reset(); isaac_coloroffset_source_generation[0]=0; expected_reject();
 reset(); ++isaac_coloroffset_compiled_generation[0]; expected_reject();
 reset(); ++progs[0].isaac_coloroffset_link_source_generation; expected_reject();
 reset(); ++shaders[0].id; expected_reject();
 reset(); shaders[1].prog=&other; expected_reject();
 reset(); isaac_coloroffset_source_generation[1]=0; expected_reject();
 reset(); ++progs[0].isaac_coloroffset_link_vertex_generation; expected_reject();
 reset(); { shader outside=shaders[0]; progs[0].fshader=&outside; expected_reject(); }
 reset(); progs[0].vshader=&shaders[2]; shaders[2].prog=&vs; expected_reject();
 reset(); CHECK(!isaac_coloroffset_fragment_program_is_exact(NULL));
 /* Actual compile notification, also used by both successful cache loaders. */
 reset(); isaac_coloroffset_note_compile(&shaders[0]); expected_reject();
 isaac_coloroffset_record_link(&progs[0]); CHECK(isaac_coloroffset_fragment_program_is_exact(&progs[0]));
 reset(); fs.exact=0; isaac_coloroffset_note_compile(&shaders[0]); expected_reject();
 isaac_coloroffset_record_link(&progs[0]); expected_reject();
 fs.exact=1; isaac_coloroffset_note_compile(&shaders[0]); expected_reject();
 isaac_coloroffset_record_link(&progs[0]); CHECK(isaac_coloroffset_fragment_program_is_exact(&progs[0]));
 reset(); shaders[0].prog=NULL; isaac_coloroffset_note_compile(&shaders[0]); expected_reject();
 reset(); isaac_coloroffset_source_generation[0]=UINT32_MAX;
 isaac_coloroffset_note_compile(&shaders[0]); expected_reject();
 CHECK(isaac_coloroffset_source_generation[0]==0);
 isaac_coloroffset_note_compile(&shaders[0]); CHECK(isaac_coloroffset_source_generation[0]==0);
 isaac_coloroffset_record_link(&progs[0]); expected_reject();
 /* Previously positive link cache must be cleared on unsuccessful link proof. */
 for (unsigned bad=0;bad<5;++bad) {
  reset();
  switch(bad) { case 0:fs.exact=0;break;case 1:vs.exact=0;break;case 2:isaac_coloroffset_source_exact[0]=0;break;case 3:isaac_coloroffset_compile_exact[0]=0;break;default:isaac_coloroffset_compiled_generation[0]=0;break; }
  unsigned before=queries; isaac_coloroffset_record_link(&progs[0]);
  CHECK(queries==before); expected_reject();
#ifdef HAVE_ISAAC_COLOROFFSET_LINK_PROOF
  CHECK(progs[0].isaac_coloroffset_link_sampler_program==NULL);
#endif
 }
 /* Same GXP address/id reused: generation, not address uniqueness, fences it. */
 reset(); isaac_coloroffset_advance_shader_generation(0); expected_reject();
 isaac_coloroffset_note_compile(&shaders[0]); expected_reject();
 isaac_coloroffset_record_link(&progs[0]); CHECK(isaac_coloroffset_fragment_program_is_exact(&progs[0]));
 /* Reattach different shader slot even if program/id aliases the old values. */
 reset(); shaders[2]=shaders[0]; progs[0].status=PROG_UNLINKED; progs[0].fshader=&shaders[2]; expected_reject();
 progs[0].status=PROG_LINKED; expected_reject();
 isaac_coloroffset_record_link(&progs[0]); CHECK(isaac_coloroffset_fragment_program_is_exact(&progs[0]));
 /* Immutable reflection's five rejection points, checked anew at every link. */
 for (unsigned bad=0;bad<5;++bad) {
  reset();
  switch(bad) { case 0:fs.count=2;break;case 1:fs.exists=0;break;case 2:fs.tex.category=3;break;case 3:fs.tex.resource=1;break;default:fs.tex.size=2;break; }
  isaac_coloroffset_record_link(&progs[0]); queries=0;
  CHECK(!isaac_coloroffset_fragment_program_is_exact(&progs[0]));
#ifdef HAVE_ISAAC_COLOROFFSET_LINK_PROOF
  CHECK(queries==0); CHECK(progs[0].isaac_coloroffset_link_sampler_program==NULL);
#else
  CHECK(queries==bad+1);
#endif
  fs=(SceGxmProgram){1,1,1,{2,0,1}}; isaac_coloroffset_record_link(&progs[0]);
  CHECK(isaac_coloroffset_fragment_program_is_exact(&progs[0]));
 }
#ifdef HAVE_ISAAC_COLOROFFSET_LINK_PROOF
 /* Real glLinkProgram prefix: proof dies BEFORE either missing-shader return. */
 for (unsigned postponed=0;postponed<2;++postponed) {
  reset(); glsl_sema_mode=postponed ? VGL_MODE_POSTPONED : 0;
  shaders[0].prog=NULL; test_link_prefix(1); shaders[0].prog=&fs;
  CHECK(progs[0].isaac_coloroffset_link_sampler_program==NULL); expected_reject();
  isaac_coloroffset_record_link(&progs[0]); CHECK(isaac_coloroffset_fragment_program_is_exact(&progs[0]));
 }
 reset(); shaders[0].prog=&other; expected_reject();
 reset(); test_delete_clear(&progs[0]); CHECK(progs[0].isaac_coloroffset_link_sampler_program==NULL);
 test_create_clear(0); CHECK(progs[0].isaac_coloroffset_link_sampler_program==NULL);
 expected_reject(); isaac_coloroffset_record_link(&progs[0]); CHECK(isaac_coloroffset_fragment_program_is_exact(&progs[0]));
#endif
 printf("ColorOffset actual link/draw helpers: %u checks PASS; link-proof=%d\n", checks,
#ifdef HAVE_ISAAC_COLOROFFSET_LINK_PROOF
 1
#else
 0
#endif
 );
 return 0;
}
'''


def host_fixture(before: str, after: str) -> str:
    # OFF must contain exactly the old helpers after preprocessing; the only
    # new declaration is itself guarded. Runtime gate text stays byte-for-byte.
    old = function(before, "isaac_coloroffset_fragment_program_is_exact")
    new = function(after, "isaac_coloroffset_fragment_program_is_exact")
    old_gates = old[old.index("int32_t shader_index;"):old.index("sceGxmProgramGetParameterCount")]
    new_gates = new[new.index("int32_t shader_index;"):new.index("#ifdef HAVE_ISAAC_COLOROFFSET_LINK_PROOF")]
    assert old_gates.rstrip() == new_gates.rstrip()
    names = ("isaac_coloroffset_shader_index", "isaac_coloroffset_advance_shader_generation",
             "isaac_coloroffset_note_compile", "isaac_coloroffset_fragment_program_is_exact",
             "isaac_coloroffset_record_link")
    helpers = "\n".join(function(after, name) for name in names)
    sampler = function(after, "isaac_coloroffset_fragment_sampler_is_exact")
    link = function(after, "glLinkProgram")
    prefix = link[:link.index("\t// With VGL_MODE_POSTPONED")]+"}\n"
    prefix = prefix.replace("void glLinkProgram(", "static void test_link_prefix(", 1)
    delete = function(after, "glDeleteProgram")
    clear = re.search(r"#ifdef HAVE_ISAAC_COLOROFFSET_LINK_PROOF\n(.*?)#endif", delete, re.S).group(1)
    create_clear = "progs[i].isaac_coloroffset_link_sampler_program = NULL;"
    assert create_clear in after
    hooks = ("#ifdef HAVE_ISAAC_COLOROFFSET_LINK_PROOF\n" + prefix +
             "static void test_delete_clear(program *p) {\n"+clear+"}\n"+
             "static void test_create_clear(unsigned i) { "+create_clear+" }\n#endif\n")
    return (PRELUDE + "\n#ifdef HAVE_ISAAC_COLOROFFSET_LINK_PROOF\n" + sampler +
            "\n#endif\n" + helpers + "\n" + hooks + TESTS)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stock-tar", required=True, type=Path)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    assert chain.sha256(args.stock_tar) == chain.SOURCE_ARCHIVE_SHA256
    args.output.mkdir(parents=True, exist_ok=True)
    build=(RECIPE/"build.sh").read_text(encoding="utf-8")
    for needle in ('coloroffset_link_proof=${ISAAC_COLOROFFSET_LINK_PROOF:-0}',
                   '"coloroffset_link_proof:$coloroffset_link_proof"',
                   '"$coloroffset_link_proof" "$coloroffset_link_proof_patch_sha"',
                   'build_flags="$build_flags HAVE_ISAAC_COLOROFFSET_LINK_PROOF=1"',
                   'extra_cflags="$extra_cflags -DHAVE_ISAAC_COLOROFFSET_LINK_PROOF=1"',
                   'apply_source_patch "$coloroffset_link_proof_patch"'):
        assert needle in build, needle
    stages = {
        "color": (), "probe": (8,), "staging": (8,12), "plain": (8,12,13),
        "single": (8,12,14), "plain-single": (8,12,13,14),
        "pair": (8,12,13,14,15), "halo": (8,12,13,14,16,17),
        "halo-pair": (8,12,13,14,15,16,17), "transform": (8,12,13,14,15,18),
        "transform-halo": (8,12,13,14,15,16,17,18),
        "atlas": (8,12,13,14,15,16,17,19),
        "atlas-transform": (8,12,13,14,15,16,17,18,19),
    }
    patches = {int(p.name[:4]):p for p in RECIPE.glob("[0-9][0-9][0-9][0-9]-*.patch")}
    rows = []
    canonical = None
    with tempfile.TemporaryDirectory(prefix="isaac-link-proof-") as temporary:
        for cache in (False, True):
            for stage, optional in stages.items():
                # Production halo/atlas paths require the authenticated cache.
                if not cache and (16 in optional or 19 in optional):
                    continue
                directory=Path(temporary)/f"{stage}-cache{int(cache)}"
                native=chain.materialize_stock(directory,args.stock_tar,None)
                order=[1,2,3]
                if 8 in optional: order.append(8)
                if cache: order.append(4)
                order.extend(p for p in optional if p != 8)
                for number in order: chain.apply_patch(native,patches[number])
                path=native/"source/custom_shaders.c"
                before=path.read_text(encoding="utf-8")
                before_hash=chain.sha256(path)
                chain.apply_patch(native,PATCH)
                after=path.read_text(encoding="utf-8")
                require_mutation_contract(after)
                fixture=host_fixture(before,after)
                if canonical is None: canonical=fixture
                assert fixture==canonical, "core helper changed across native combinations"
                after_hash=chain.sha256(path)
                if cache:
                    assert f"{before_hash}) shader_source_sha={after_hash} ;;" in build
                elif stage=="transform":
                    assert after_hash in build
                rows.append(f"{stage} cache={int(cache)} {before_hash} {after_hash}")
        assert canonical is not None
        fixture_path=args.output/"actual-link-proof-helpers.c"
        fixture_path.write_text(canonical,encoding="utf-8",newline="\n")
        for enabled in (False,True):
            output=args.output/f"link-proof-{int(enabled)}.exe"
            options=["-DHAVE_ISAAC_COLOROFFSET_LINK_PROOF=1"] if enabled else []
            subprocess.run([args.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror", "-I",str(RECIPE),*options,
                            str(fixture_path),"-o",str(output)],check=True)
            subprocess.run([str(output)],check=True)
    (args.output/"source-hashes.txt").write_text("\n".join(rows)+"\n",encoding="utf-8")
    print(f"ColorOffset LINK_PROOF regenerated native chains: {len(rows)} PASS")
    print("\n".join(rows))


if __name__ == "__main__":
    main()
