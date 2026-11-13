#!/usr/bin/env python3
"""Execute the ring and non-evicting memo against freshly extracted old code."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def original_locations(out):
    source = (ROOT / 'recomp/runtime/gl_vita_backend.c').read_text()
    key = source[source.index('#define GL_VITA_LOCATION_KIND_ATTRIB'):
                 source.index('#if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)')]
    start = source.index('#define GL_VITA_LOCATION_CACHE_CAPACITY')
    stop = source.index('\n#else\nstatic inline int gl_vita_location_cache_lookup', start)
    body = source[start:stop]
    assert 'GL_VITA_LOCATION_CACHE_CAPACITY 256u' in body
    assert 'static void gl_vita_location_cache_store' in body
    fixture = '''#include <stdint.h>
#include <string.h>
typedef uint32_t guest_gl_uint;
typedef uint32_t guest_gl_addr;
typedef int32_t guest_gl_int;
#define GL_VITA_LOCATION_GENERATION_BUMP() ((void)0)
static uint8_t ld8(uint32_t p) { return *(const uint8_t *)(uintptr_t)p; }
''' + key + body
    (out / 'gl_record_original_locations.inc').write_text(fixture)


def original_renderbuffers(out):
    source = (ROOT / 'recomp/runtime/gl_vita_backend.c').read_text()
    constants = '\n'.join(re.findall(
        r'^#define GL_VITA_(?:RBO_CAPACITY|RENDERBUFFER(?:_WIDTH|_HEIGHT)?|'
        r'TOKEN_(?:BIND_RENDERBUFFER|DELETE_RENDERBUFFERS|GEN_RENDERBUFFERS|'
        r'GET_RBO_PARAMETER|RBO_STORAGE))\s+[^\n]+', source, re.M))
    assert len(constants.splitlines()) == 9
    start = source.index('typedef struct gl_vita_renderbuffer_record {')
    stop = source.index('\n#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)', start)
    declarations = source[start:stop]
    start = source.index('static void gl_vita_rbo_reset(void)')
    stop = source.index('\n#if defined(GL_VITA_FBO_TABLE)', start)
    helpers = source[start:stop]
    bodies = []
    for name in ('BindRenderbuffer', 'DeleteRenderbuffers', 'GenRenderbuffers',
                 'GetRenderbufferParameteriv', 'RenderbufferStorage'):
        start = source.index('static void vita_gl' + name + '(')
        stop = source.index('\n}\n', start) + len('\n}\n')
        bodies.append(source[start:stop])
    prelude = '''#include <stdint.h>
#include <string.h>
typedef uint32_t guest_gl_uint;
typedef uint32_t guest_gl_addr;
typedef uint32_t guest_gl_enum;
typedef int32_t guest_gl_sizei;
typedef int32_t guest_gl_int;
typedef uint32_t GLenum;
typedef uint32_t GLuint;
typedef int32_t GLsizei;
#define GUEST_NORETURN __declspec(noreturn)
static uint32_t ld32(uint32_t p) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)p, 4u); return v; }
static void st32(uint32_t p, uint32_t v) { memcpy((void *)(uintptr_t)p, &v, 4u); }
static GUEST_NORETURN void guest_gl_backend_fault(uint32_t, const char *);
static void glBindRenderbuffer(GLenum, GLuint);
static void glRenderbufferStorage(GLenum, GLenum, GLsizei, GLsizei);
static void glDeleteRenderbuffers(GLsizei, const GLuint *);
static void glGenRenderbuffers(GLsizei, GLuint *);
'''
    (out / 'gl_record_original_renderbuffers.inc').write_text(
        # helpers may end in a preprocessor line (a knob block now sits right
        # before GL_VITA_FBO_TABLE): keep that line on its own.
        prelude + constants + '\n' + declarations + helpers + '\n'
        + '\n'.join(bodies))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/glthread-units')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    # No thread-local storage in the GL-thread code: on the device __thread is
    # libgcc emutls with __gthread_active_p() == 0, i.e. ONE copy shared by all
    # threads (measured on the device).  The host's TLS works, so only a
    # source check can keep that out.
    import re
    for name in ('gl_record_backend.c', 'gl_record_ring.c', 'gl_record_locations.c',
                 'gl_owner_check.c', 'kage_vita_backend.c', 'kage_vita_loading.c',
                 'manual_kage_vita.c', 'gl_vita_backend.c', 'gl_bridge.c'):
        text = (ROOT / 'recomp/runtime' / name).read_text(encoding='utf-8')
        code = re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)
        assert not re.search(r'\b(__thread|_Thread_local|thread_local)\b|declspec\s*\(\s*thread', code), name
    print('PASS no thread-local storage in the GL-thread sources')
    original_locations(out)  # Never run yesterday's extracted reference.
    original_renderbuffers(out)
    from setuptools.msvc import msvc14_get_vc_env
    vc = msvc14_get_vc_env('x86')
    env = {k: v for k, v in os.environ.items() if k.lower() not in vc}
    env.update(vc)
    cc = shutil.which('cl.exe', path=vc['path'])
    if not cc:
        raise RuntimeError('enumerated MSVC environment has no cl.exe')
    for unit in ('ring', 'locations', 'renderbuffers'):
        exe = out / (unit + '-test.exe')
        command = [cc, '/nologo', '/W4', '/WX', '/std:c11', '/O2',
                   '/I' + str(ROOT / 'recomp/runtime'), '/I' + str(out),
                   '/Fo' + str(out) + '\\', '/Fe' + str(exe),
                   str(ROOT / ('recomp/runtime/gl_record_' +
                       ('ring' if unit == 'renderbuffers' else unit) + '.c')),
                   str(ROOT / ('recomp/vita/host_tests/gl_record_' + unit + '_test.c'))]
        subprocess.run(command, cwd=out, env=env, check=True)
        subprocess.run([str(exe)], cwd=out, check=True)


if __name__ == '__main__':
    main()
