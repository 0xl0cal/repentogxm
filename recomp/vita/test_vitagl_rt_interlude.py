#!/usr/bin/env python3
"""Fresh pinned native attachment/create/GC/deletion tests; no SDK timing claim."""
from __future__ import annotations
import argparse
import json
import subprocess
from pathlib import Path
from test_vitagl_stock_patch_chain import SOURCE_ARCHIVE_SHA256, apply_patch, materialize_stock, run, sha256
from test_vitagl_rt_lease import function


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock-tar', type=Path, required=True)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--baseline-policy', type=Path, required=True)
    args = parser.parse_args()
    assert sha256(args.stock_tar) == SOURCE_ARCHIVE_SHA256
    args.out.mkdir(parents=True, exist_ok=False)
    vita = Path(__file__).resolve().parent
    recipe = vita / 'vitagl-stock-reference'
    src = materialize_stock(args.out / 'fresh', args.stock_tar, None)
    for name in ('0001-deterministic-build-and-init-oob.patch',
                 '0002-exact-gpu-draw-optimizations.patch',
                 '0007-isaac-fbo-rt-scenes.patch',
                 '0021-isaac-fbo-rt-reuse.patch',
                 '0024-isaac-fbo-rt-bounded-lease.patch'):
        apply_patch(src, recipe / name)
    # Extract full native bodies, not a copied state machine. Only platform
    # boundaries/types are mocked. Return type adaptation avoids stock's unused
    # non-void single-threaded GC fallthrough in the hosted oracle.
    def emit():
        shared = (src / 'source/shared.h').read_text()
        gxm = (src / 'source/gxm.c').read_text()
        framebuffers = (src / 'source/framebuffers.c').read_text()
        textures = (src / 'source/textures.c').read_text()
        gc = function(gxm, 'int garbage_collector(')
        gc = gc.replace('int garbage_collector(', 'void garbage_collector(', 1)
        # Stock selects pthread/non-pthread signatures immediately before this
        # body. We select its non-pthread signature, not its unmatched #endif.
        gc = gc.replace('{\n#endif\n', '{\n', 1)
        assert gc.index('sceGxmDestroyRenderTarget(') < gc.index('isaacFboRtCollect(') < gc.index('frame_purge_clean_idx =')
        start = gxm.index('int r = setup_render_target(&active_write_fb->target,')
        end = gxm.index('#ifdef LOG_ERRORS', start)
        text = function(shared, 'static inline __attribute__((always_inline)) void _glFramebufferTexture2D(')
        text += function(framebuffers, 'inline __attribute__((always_inline)) void glGenFramebuffers(').replace('inline __attribute__((always_inline))', 'static inline __attribute__((always_inline))', 1)
        text += function(framebuffers, 'void glDeleteFramebuffers(')
        text += function(textures, 'void glDeleteTextures(')
        text += gc
        text += 'static int native_create(void) {\n' + gxm[start:end] + 'return r;\n}\n'
        (args.out / 'native_rt_interlude.h').write_text(text, encoding='utf-8')
    common = [args.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
              '-Wno-unused-parameter', '-Wno-unused-function', '-DHAVE_SINGLE_THREADED_GC=1',
              '-I', str(args.out), '-I', str(recipe)]
    receipt = {'archive_sha256': SOURCE_ARCHIVE_SHA256, 'commands': [], 'legs': []}
    def normalized_object(path):
        data = path.read_bytes()
        # COFF TimeDateStamp is the only omitted field. No code/data/relocation
        # or symbol bytes are ignored.
        return data[:4] + b'\0'*4 + data[8:] if data[:2] == b'\x64\x86' else data
    def execute(name, flags):
        exe = args.out / (name + '.exe')
        obj = args.out / (name + '.o')
        command = [*common, *flags, '-c', str(vita / 'host_tests/vita_fbo_rt_interlude_native_test.c'), '-o', str(obj)]
        receipt['commands'].append(command)
        run(command)
        command = [args.cc, str(obj), '-o', str(exe)]
        receipt['commands'].append(command)
        run(command)
        output = run([str(exe)]).decode().strip()
        print(name + ': ' + output, flush=True)
        receipt['legs'].append({'name': name, 'output': output})
        return output
    emit()
    baseline = {}
    for lease in (0, 1):
        baseline[lease] = execute(f'original-lease{lease}', [f'-DHAVE_ISAAC_FBO_RT_REUSE_LEASE={lease}'])
    apply_patch(src, recipe / '0033-isaac-fbo-rt-interlude-reuse.patch')
    emit()
    for lease in (0, 1):
        for mode in ('undefined', '0', '1'):
            flags = [f'-DHAVE_ISAAC_FBO_RT_REUSE_LEASE={lease}']
            if mode != 'undefined':
                flags += [f'-DHAVE_ISAAC_FBO_RT_INTERLUDE_REUSE={mode}']
            output = execute(f'interlude{mode}-lease{lease}', flags)
            if mode != '1':
                assert output == baseline[lease], (mode, lease, output, baseline[lease])
                assert normalized_object(args.out / f'original-lease{lease}.o') == normalized_object(args.out / f'interlude{mode}-lease{lease}.o'), 'native OFF object differs outside COFF timestamp'
        execute(f'interlude1-lease{lease}-profile1', [f'-DHAVE_ISAAC_FBO_RT_REUSE_LEASE={lease}',
                '-DHAVE_ISAAC_FBO_RT_INTERLUDE_REUSE=1', '-DHAVE_ISAAC_NATIVE_RESOURCE_PROFILE=1'])
    # Byte identity of the changed helper with the flag absent and zero, against
    # frozen pre-edit source; identical basename suppresses COFF .file variation.
    for lease in (0, 1):
        objects = []
        for mode in ('baseline', 'undefined', '0'):
            directory = args.out / f'object-{lease}-{mode}'
            directory.mkdir()
            policy = args.baseline_policy if mode == 'baseline' else recipe / 'isaac_fbo_rt_reuse.c'
            local = directory / 'policy.c'
            local.write_bytes(policy.read_bytes())
            obj = directory / 'policy.o'
            command = [*common, f'-DHAVE_ISAAC_FBO_RT_REUSE_LEASE={lease}', '-c', str(local), '-o', str(obj)]
            if mode == '0':
                command += ['-DHAVE_ISAAC_FBO_RT_INTERLUDE_REUSE=0']
            receipt['commands'].append(command)
            run(command)
            objects.append(normalized_object(obj))
        assert objects[0] == objects[1] == objects[2], 'OFF object differs from frozen baseline outside COFF timestamp'
    # The build applies 0024 only when LEASE is on. Prove 0033 also composes
    # without it and run the actual resulting unleased creation/GC path.
    with_lease = src
    src = materialize_stock(args.out / 'fresh-no-lease', args.stock_tar, None)
    for name in ('0001-deterministic-build-and-init-oob.patch',
                 '0002-exact-gpu-draw-optimizations.patch',
                 '0007-isaac-fbo-rt-scenes.patch',
                 '0021-isaac-fbo-rt-reuse.patch',
                 '0033-isaac-fbo-rt-interlude-reuse.patch'):
        apply_patch(src, recipe / name)
    emit()
    execute('interlude1-no-0024', ['-DHAVE_ISAAC_FBO_RT_INTERLUDE_REUSE=1'])
    src = with_lease
    emit()
    # Kill the missing bit2 native queue mutation: the test must observe that
    # old current leaked, not merely that a helper counter changed.
    header = args.out / 'native_rt_interlude.h'
    good = header.read_text()
    needle = 'if (isaac_rt_switch & ISAAC_FBO_RT_RETIRES_CURRENT)'
    assert good.count(needle) == 1
    header.write_text(good.replace(needle, 'if (0)'), encoding='utf-8')
    exe = args.out / 'missing-retire-mutant.exe'
    command = [*common, '-DHAVE_ISAAC_FBO_RT_INTERLUDE_REUSE=1',
               '-DHAVE_ISAAC_FBO_RT_REUSE_LEASE=1',
               str(vita / 'host_tests/vita_fbo_rt_interlude_native_test.c'), '-o', str(exe)]
    run(command)
    bad = subprocess.run([str(exe)], capture_output=True, text=True)
    header.write_text(good, encoding='utf-8')
    assert bad.returncode != 0 and 'queued' in bad.stderr, bad
    receipt['negative_missing_retire'] = bad.stderr.strip()
    policy_text = (recipe / 'isaac_fbo_rt_reuse.c').read_text()
    collect_start = policy_text.index('uintptr_t isaacFboRtCollect(')
    prefix, collect_text = policy_text[:collect_start], policy_text[collect_start:]
    for name, old, new, failure in (
            ('early-collect', 'if (rt_reuse.spare_bucket != purge_bucket)', 'if (0)', 'deadline'),
            ('lease-interlude', 'if (rt_reuse.interlude) {', 'if (0) {', 'destructions')):
        assert old in collect_text
        mutant = args.out / (name + '.c')
        mutant.write_text(prefix + collect_text.replace(old, new, 1), encoding='utf-8')
        exe = args.out / (name + '.exe')
        command = [*common, '-DHAVE_ISAAC_FBO_RT_INTERLUDE_REUSE=1',
                   '-DHAVE_ISAAC_FBO_RT_REUSE_LEASE=1',
                   f'-DRT_POLICY_SOURCE="{mutant.as_posix()}"',
                   str(vita / 'host_tests/vita_fbo_rt_interlude_native_test.c'), '-o', str(exe)]
        receipt['commands'].append(command)
        run(command)
        bad = subprocess.run([str(exe)], capture_output=True, text=True)
        assert bad.returncode != 0 and failure in bad.stderr, bad
        receipt['negative_' + name] = bad.stderr.strip()
    receipt['helper_off_object_identity_except_coff_timestamp'] = True
    receipt['native_off_object_identity_except_coff_timestamp'] = True
    receipt['patch_sha256'] = sha256(recipe / '0033-isaac-fbo-rt-interlude-reuse.patch')
    (args.out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print('Fresh native interlude tests and OFF object identity: PASS', flush=True)


if __name__ == '__main__':
    main()
