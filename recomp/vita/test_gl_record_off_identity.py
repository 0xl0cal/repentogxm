#!/usr/bin/env python3
"""ISAAC_VITA_GL_THREAD_RECORD OFF identity: preprocessed text and COFF object
of every touched production TU equal the base revision (timestamp excepted)."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / 'recomp/runtime'
FILES = ('gl_bridge.c', 'gl_vita_backend.c', 'kage_vita_backend.c', 'manual_kage_vita.c',
         'kage_vita_loading.c', 'host_vita_debug_overlay.c', 'host_vita_audio_refill.c',
         'kage_vita_phase_profile.c', 'kage_vita_guest_sampler.c', 'host_vita_image_retain.c',
         'kage_vita_fullspeed_scheduler.c')
# Knob guards the OFF build never takes (#else bodies are kept).
OFF_GUARDS = (
    b'#if defined(ISAAC_VITA_GL_THREAD_RECORD) && ISAAC_VITA_GL_THREAD_RECORD',
    b'#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER',
    b'#if defined(ISAAC_VITA_GL_THREAD_WORKER_PROOF) && ISAAC_VITA_GL_THREAD_WORKER_PROOF',
    b'#if defined(ISAAC_VITA_GL_THREAD_RENDER_ADMISSION) && ISAAC_VITA_GL_THREAD_RENDER_ADMISSION',
    b'#if defined(ISAAC_VITA_GUEST_SAMPLER_PRESENT_LABEL)',   # profile-only, default OFF
)


def unifdef_off(text):
    """Drop OFF_GUARDS blocks (keep their #else part); returns (text, blocks)."""
    out, stack, blocks = [], [], 0  # stack entries: 'off-if', 'off-else', 'other'
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        emit = all(s != 'off-if' for s in stack)
        if stripped in OFF_GUARDS:
            stack.append('off-if'); blocks += 1; continue
        if stripped.startswith((b'#if', b'#ifdef', b'#ifndef')):
            stack.append('other')
        elif stripped.startswith(b'#else') and stack and stack[-1] in ('off-if', 'off-else'):
            stack[-1] = 'off-else'; continue
        elif stripped.startswith(b'#endif') and stack:
            if stack.pop() in ('off-if', 'off-else'):
                continue
        if emit:
            out.append(line)
    assert not stack, 'unbalanced conditionals'
    return b''.join(out), blocks
COMMON = ['/nologo', '/std:c11', '/O2', '/W4', '/wd4310', '/D_CRT_SECURE_NO_WARNINGS',
          '/DGUEST_IMAGE_BASE=0x98000000u']
CONFIGS = {
    'oracle-off': ['/DISAAC_GL_VITA_BACKEND_ORACLE=1', '/DISAAC_VITA_IO_PROFILE=1'],
    'oracle-prod': ['/DISAAC_GL_VITA_BACKEND_ORACLE=1', '/DISAAC_VITA_IO_PROFILE=1',
                    '/DISAAC_VITA_GL_REDUNDANCY_CACHE=1', '/DISAAC_VITA_GL_TYPED_STATE_CACHE=1',
                    '/DISAAC_VITA_GL_SHIM_FASTDISPATCH=1', '/DISAAC_VITA_CANONICAL_QUAD_ZERO_COPY=1',
                    '/DISAAC_VITA_PHASE_PROFILE=1'],
    'oracle-coalesce-readback': ['/DISAAC_GL_VITA_BACKEND_ORACLE=1', '/DISAAC_VITA_IO_PROFILE=1',
                                 '/DISAAC_VITA_GL_TYPED_STATE_CACHE=1',
                                 '/DISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE=1',
                                 '/DISAAC_VITA_FBO_READBACK_FIX=1', '/DISAAC_VITA_PHASE_PROFILE=1'],
    'oracle-off-explicit-zero': ['/DISAAC_GL_VITA_BACKEND_ORACLE=1', '/DISAAC_VITA_IO_PROFILE=1',
                                 '/DISAAC_VITA_GL_THREAD_RECORD=0'],
}
# The kage/manual TUs have their own host facades; same flags plus these.
EXTRA = {'kage_vita_backend.c': ['/DISAAC_KAGE_VITA_BACKEND_ORACLE=1'],
         'manual_kage_vita.c': ['/DISAAC_VITA_AUDIO_MANUAL_ORACLE=1']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', default='91c374cd98288ea4f709d77ef26cbb89bde6449e')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/glthread-off-identity')
    parser.add_argument('--plant', action='store_true',
                        help='self-test: add one OFF-visible line; the run must fail')
    args = parser.parse_args()
    if subprocess.run(['git', 'cat-file', '-e', args.baseline + '^{commit}'], cwd=ROOT,
                      capture_output=True).returncode != 0:
        print(f'SKIP GL recorder OFF identity: base commit {args.baseline} is not in this '
              "checkout's history (every leg compares against it; give --baseline)")
        return
    from setuptools.msvc import msvc14_get_vc_env
    vc = msvc14_get_vc_env('x86')
    env = {k: v for k, v in os.environ.items() if k.lower() not in vc}
    env.update(vc)
    cc = shutil.which('cl.exe', path=vc['path'])
    assert cc, 'no cl.exe in the enumerated MSVC environment'
    out = args.output.resolve()
    compared = skipped = 0
    text_only = []
    for name in FILES:
        base_text = subprocess.run(['git', 'show', f'{args.baseline}:recomp/runtime/{name}'],
                                   cwd=ROOT, check=True, capture_output=True).stdout
        cur_text = (RUNTIME / name).read_bytes()
        if args.plant and name == 'gl_vita_backend.c':
            cur_text += b'\nint gl_record_off_identity_plant;\n'
        for config, flags in CONFIGS.items():
            results = {}
            for side, text in (('base', base_text), ('cur', cur_text)):
                # One directory for both sides: MSVC records the object path.
                work = out / config
                work.mkdir(parents=True, exist_ok=True)
                (work / name).write_bytes(text)
                command = [cc, *COMMON, *flags, *EXTRA.get(name, []), '/I' + str(RUNTIME)]
                pre = subprocess.run([*command, '/EP', name], cwd=work, env=env,
                                     capture_output=True)
                obj = subprocess.run([*command, '/c', name, '/Fo' + name + '.obj'],
                                     cwd=work, env=env, capture_output=True)
                results[side] = (pre.returncode, pre.stdout, obj.returncode,
                                 (work / (name + '.obj')).read_bytes() if obj.returncode == 0 else b'',
                                 pre.stderr + obj.stdout + obj.stderr)
            base, cur = results['base'], results['cur']
            if base[0] != 0 or base[2] != 0:
                # A config this TU does not support on the host: must fail identically.
                if (base[0], base[2]) != (cur[0], cur[2]):
                    raise SystemExit(f'FAIL {name} {config}: base/cur build status differ')
                skipped += 1
                text_only.append(name)
                print(f'SKIP {name} {config}: not host-buildable at base either')
                continue
            # /EP keeps a blank line per removed directive line: compare tokens.
            if base[1].split() != cur[1].split():
                raise SystemExit(f'FAIL {name} {config}: preprocessed output differs')
            # COFF TimeDateStamp is bytes 4..8 of the file header.
            if base[3][:4] + base[3][8:] != cur[3][:4] + cur[3][8:]:
                raise SystemExit(f'FAIL {name} {config}: object differs')
            compared += 1
            print(f'EQUAL {name} {config}: /EP {len(cur[1])} B, obj {len(cur[3])} B')
    if not compared:
        raise SystemExit('FAIL nothing compared')
    # TUs not host-buildable even at base: the OFF reading of the knob guards
    # (blocks dropped, #else kept) must give the base bytes exactly.
    # These two compile to near-empty host objects (bodies under their own
    # Vita/feature guards), so the object comparison above proves nothing.
    text_only += ['host_vita_debug_overlay.c', 'host_vita_audio_refill.c']
    for name in sorted(set(text_only)):
        base_text = subprocess.run(['git', 'show', f'{args.baseline}:recomp/runtime/{name}'],
                                   cwd=ROOT, check=True, capture_output=True).stdout
        stripped, blocks = unifdef_off((RUNTIME / name).read_bytes())
        if args.plant and name == 'manual_kage_vita.c':
            stripped += b'int plant;\n'
        if stripped != base_text or not blocks:
            raise SystemExit(f'FAIL {name}: OFF reading differs from base ({blocks} guarded block(s))')
        print(f'EQUAL {name}: OFF reading of {blocks} guarded block(s) = base bytes')
    print(f'PASS GL recorder OFF identity: {compared} TU/config pairs equal, {skipped} skipped')


if __name__ == '__main__':
    main()
