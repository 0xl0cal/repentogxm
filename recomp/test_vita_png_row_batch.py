#!/usr/bin/env python3
"""Fresh frozen caller loop + actual native wrapper and scratch batch copy."""
from pathlib import Path
import argparse
import hashlib
import os
import subprocess
import sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pe', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    root = Path(__file__).resolve().parent
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    assert hashlib.sha256(args.pe.read_bytes()).hexdigest() == '31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404'
    os.environ['REPENTOGXM_PE'] = str(args.pe)
    sys.path.insert(0, str(root))
    import gen_all as G
    from image import Image, DEFAULT_BASE
    image, pin = Image(str(args.pe), 0x98000000), Image(str(args.pe), DEFAULT_BASE)
    record = G._translate_function(image, {'rva': 0x5a0e10}, None, {}, pin_img=pin)
    assert record['stub'] is None
    source = record['text']
    (out/'image-png.fresh.c').write_text(source)
    span = source[source.index('L_005a1390:'):source.index('    /* 005a13ca ')]
    assert span.count('isaac_vita_native_png_middle_rows_try(c) < 0') == 1
    def loop(name, enabled):
        return ('#undef ISAAC_VITA_NATIVE_PNG_ROW_BATCH\n' +
            ('#define ISAAC_VITA_NATIVE_PNG_ROW_BATCH 1\n' if enabled else '') +
            f'static void {name}(CPU *c) {{\nGUEST_FLAGS_DECL; GUEST_GPR_DECL;\n' +
            span.replace('isaac_vita_native_png_middle_rows_try', 'batch_checked') +
            '\nGUEST_GPR_FLUSH(c); return;\nL_guest_setjmp_cleanup_005a0e10: GUEST_GPR_FLUSH(c);return;\n}\n')
    text = (root/'vita/host_tests/kage_vita_png_row_batch.c').read_text()
    text = text.replace('#include "../../runtime/host_vita_texel_scratch.c"',
        '#include "' + (root/'runtime/host_vita_texel_scratch.c').as_posix() + '"')
    text = text.replace('/* FRESH_BATCH_LOOPS */',
        loop('batch_loop_original', False) + loop('batch_loop_candidate', True))
    prefix = '#define main unused_clock_main\n#include "' + (root/'vita/host_tests/kage_vita_native_png_clocks.c').as_posix() + '"\n#undef main\n'
    fixture = out/'fixture.c'
    fixture.write_text(prefix + text)
    guest = (root/'runtime/guest.c').read_text()
    stack = guest[guest.index('GUEST_STACK_HOT_NOINLINE uint32_t guest_stack_address('):guest.index('GUEST_STACK_HOT_NOINLINE int guest_stack_set_generated(')]
    stack += '#if GUEST_GENERATED_STACK_GUARD\n'
    stack += guest[guest.index('#if defined(_MSC_VER)'):guest.index('#ifdef __vita__')]
    stack += guest[guest.index('static const char *guest_stack_fault_text('):guest.index('GUEST_STACK_COLD_NOINLINE int guest_stack_violation(')]
    stack += guest[guest.index('static GUEST_STACK_COLD_NOINLINE int guest_stack_native_violation('):guest.index('#if GUEST_STACK_REQUIRED\nGUEST_STACK_HOT_NOINLINE int guest_stack_set(')]
    # The final original #endif now closes the fixture's guard-only region.
    stack += guest[guest.index('GUEST_STACK_HOT_NOINLINE int guest_stack_set_generated('):guest.index('int guest_stack_bind(')]
    (out/'native-png-clock-stack.inc').write_text(stack)
    env = os.environ.copy()
    if os.name == 'nt':
        from setuptools.msvc import msvc14_get_vc_env
        env = {k.upper(): v for k,v in env.items()}
        env.update({k.upper():v for k,v in msvc14_get_vc_env('x86').items()})
        compiler = 'C:/Program Files/LLVM/bin/clang.exe'
        target = ['--target=i686-pc-windows-msvc', '-D_CRT_SECURE_NO_WARNINGS=1']
    else:
        compiler, target = 'cc', ['-m32']
    for gpr, flags, guard in ((0,0,0), (1,1,0), (1,1,1)):
        exe = out/f'rows-{gpr}{flags}{guard}.exe'
        command = [compiler, *target, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-variable', '-Wno-unused-label', '-fno-strict-aliasing',
            '-D__vita__=1', '-DGUEST_STACK_REQUIRED=1', '-DGUEST_IMAGE_BASE=0x98000000u',
            f'-DGUEST_GPR_LOCAL={gpr}', f'-DGUEST_FLAGS_LOCAL={flags}', f'-DGUEST_GENERATED_STACK_GUARD={guard}',
            '-DISAAC_VITA_NATIVE_PNG=1', '-DISAAC_VITA_NATIVE_PNG_ROW_BATCH=1',
            '-DISAAC_VITA_HEAP_RANGE_LEASE=1', '-I', str(root/'vita/host_tests/native_png_clock_stubs'),
            '-I', str(root/'runtime'), '-I', str(root/'vita'), '-I', str(out),
            str(fixture), str(root/'runtime/host_vita_archive_miniz_native.c'), '-o', str(exe)]
        subprocess.run(command, env=env, check=True)
        subprocess.run([str(exe)], env=env, check=True)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
