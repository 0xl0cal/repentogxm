#!/usr/bin/env python3
"""Execute actual async transport, with explicit file/debug seam doubles.

The fixture and OFF source are regenerated each run. No Vita/network action.
SDK file persistence, kernel contention and real producer progress remain open.
"""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(command):
    result = subprocess.run([str(x) for x in command], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


FIXTURE = r'''
#include "host_vita_log_async.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(ISAAC_VITA_LOG_ASYNC_PRINTF_FILE) && ISAAC_VITA_LOG_ASYNC_PRINTF_FILE
#define MIRROR 1
#else
#define MIRROR 0
#endif
#define CAP (2u * ISAAC_VITA_LOG_ASYNC_RING_BYTES + 8192u)
static char file[CAP], debug[CAP], want_file[CAP], want_debug[CAP];
static unsigned nf, nd, wf, wd, file_calls, debug_calls, checks;
static void check(int value) { ++checks; if (!value) { fprintf(stderr, "failed check %u\n", checks); exit(1); } }
static void append(char *dst, unsigned *used, const char *src, unsigned n) {
    check(*used + n < CAP); memcpy(dst + *used, src, n); *used += n;
}
static void printf_sink(const char *line, unsigned n) {
    ++debug_calls; append(debug, &nd, line, n);
}
/* Models the actual platform.c FILE sink: append, then one debug call. */
static void file_sink(const char *line, unsigned n) {
    ++file_calls; append(file, &nf, line, n); printf_sink(line, n);
}
#if defined(ISAAC_VITA_LOG_ASYNC_FILE_BATCH)
static unsigned batch_opens, batch_closes, batch_count;
static int batch_open(void) { ++batch_opens; return 4; }
static int batch_write(int fd, const char *line, unsigned n) {
    check(fd == 4); append(file, &nf, line, n); return (int)n;
}
static int batch_close(int fd) { check(fd == 4); ++batch_closes; return 0; }
#define ISAAC_LOG_BATCH_OPEN() batch_open()
#define ISAAC_LOG_BATCH_WRITE(fd, data, size) batch_write(fd, data, size)
#define ISAAC_LOG_BATCH_CLOSE(fd) batch_close(fd)
#define ISAAC_LOG_BATCH_DEBUG(line, size) printf_sink(line, size)
#include "host_vita_log_file_batch_private.h"
static void batch_sink(char *data, const uint16_t *sizes, unsigned count) {
    ++batch_count; isaac_vita_log_file_batch_emit(data, sizes, count);
}
#endif
static void expect(const char *line, unsigned n, int durable) {
    append(want_debug, &wd, line, n);
    if (durable) append(want_file, &wf, line, n);
}
static void equal(void) {
    check(nf == wf && !memcmp(file, want_file, nf));
    check(nd == wd && !memcmp(debug, want_debug, nd));
    check(g_isaac_vita_log_async_oracle_lock_faults == 0);
}
static void reset_capture(void) { nf = nd = wf = wd = 0; }
int main(void) {
    char line[160], maximum[ISAAC_VITA_LOG_ASYNC_PRINTF_BYTES], big[ISAAC_VITA_LOG_ASYNC_PRINTF_BYTES + 60];
    unsigned i, expected_records = 0, accepted = 0;
    IsaacVitaLogAsyncStats stats;
    g_isaac_vita_log_async_oracle_file_sink = file_sink;
    g_isaac_vita_log_async_oracle_printf_sink = printf_sink;
#if defined(ISAAC_VITA_LOG_ASYNC_FILE_BATCH)
    g_isaac_vita_log_async_oracle_file_batch_sink = batch_sink;
#endif
    isaac_vita_log_async_printf("before %u\n", 7u);
    expect("before 7\n", 9, MIRROR); equal(); reset_capture();
    check(isaac_vita_log_async_start() == 1);
    for (i = 0; i < 20; ++i) {
        int n = snprintf(line, sizeof line, "native-%u\n", i);
        check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_FILE, line, (unsigned)n) == 1);
        expect(line, (unsigned)n, 1);
        n = snprintf(line, sizeof line, "[kage-vita] ph120.test win=%u zero=0\n", i);
        isaac_vita_log_async_printf("%s", line); expect(line, (unsigned)n, MIRROR);
        expected_records += 2;
    }
    /* No sink on the started producer path; consumers alone perform I/O. */
    check(nf == 0 && nd == 0);
    check(isaac_vita_log_async_oracle_service() == expected_records); equal(); reset_capture();
#if defined(ISAAC_VITA_LOG_ASYNC_FILE_BATCH)
    check(batch_opens == batch_closes && batch_count > 0);
    if (MIRROR) check(batch_opens < expected_records);
#endif
    memset(maximum, 'z', sizeof maximum - 2); maximum[sizeof maximum - 2] = '\n'; maximum[sizeof maximum - 1] = 0;
    isaac_vita_log_async_printf("%s", maximum);
    expect(maximum, (unsigned)sizeof maximum - 1, MIRROR);
    isaac_vita_log_async_flush(); equal(); reset_capture();
    check(isaac_vita_log_async_enqueue(3, "bad\n", 4) == -1);
    expect("[kage-vita] log-async dropped=1\n", (unsigned)strlen("[kage-vita] log-async dropped=1\n"), MIRROR);
    check(isaac_vita_log_async_oracle_service() == 0); equal(); reset_capture();
    /* Fill the ring from both producers; refusal does not poison older rows. */
    for (;;) {
        unsigned sink = accepted & 1 ? ISAAC_VITA_LOG_ASYNC_SINK_FILE : ISAAC_VITA_LOG_ASYNC_SINK_PRINTF;
        int rc = isaac_vita_log_async_enqueue(sink, maximum, (unsigned)sizeof maximum - 1);
        if (rc == -1) break;
        check(rc == 1); expect(maximum, (unsigned)sizeof maximum - 1, sink == ISAAC_VITA_LOG_ASYNC_SINK_FILE);
        ++accepted; check(accepted <= ISAAC_VITA_LOG_ASYNC_RING_BYTES);
        /* log_async_drain reports overload after the first 64 drained rows,
         * not after a large drain. Preserve that actual arrival ordering. */
        if (accepted == 64)
            expect("[kage-vita] log-async dropped=2\n", (unsigned)strlen("[kage-vita] log-async dropped=2\n"), MIRROR);
    }
    /* Direct enqueue is not rewritten: only the printf wrapper selects FILE. */
    g_isaac_vita_log_async_oracle_now_us = ISAAC_VITA_LOG_ASYNC_DROP_REPORT_US;
    if (accepted < 64)
        expect("[kage-vita] log-async dropped=2\n", (unsigned)strlen("[kage-vita] log-async dropped=2\n"), MIRROR);
    check(isaac_vita_log_async_oracle_service() == accepted); equal(); reset_capture();
    memset(big, 'Q', sizeof big - 2); big[sizeof big - 2] = '\n'; big[sizeof big - 1] = 0;
    if (MIRROR) {
        int n = snprintf(line, sizeof line, "[kage-vita] log-async printf-file overlong=%u network_only=1\n", (unsigned)sizeof big - 1);
        expect(line, (unsigned)n, 1);
    }
    expect(big, (unsigned)sizeof big - 1, 0);
    isaac_vita_log_async_printf("%s", big); equal();
    isaac_vita_log_async_get_stats(&stats);
    check(stats.dropped == 2 && stats.used == 0 && stats.enqueued == stats.sunk);
    check(g_isaac_vita_log_async_oracle_wakes > 0 && debug_calls > file_calls);
    printf("printf file transport PASS: checks=%u mirror=%u ring=%u max=%u drop=%u; file/debug exact, no producer sink while queued\n",
           checks, MIRROR, ISAAC_VITA_LOG_ASYNC_RING_KB, (unsigned)sizeof maximum - 1, stats.dropped);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--base", default="f8bfc582661b8b56fcc3893b85584c5de0f737c9")
    args = parser.parse_args()
    cmake = (ROOT / "vita/CMakeLists.txt").read_text()
    assert re.search(r'option\(ISAAC_VITA_LOG_ASYNC_PRINTF_FILE\s+"[^"]+" OFF\)', cmake)
    assert 'if(ISAAC_VITA_LOG_ASYNC_PRINTF_FILE AND NOT ISAAC_VITA_LOG_ASYNC)' in cmake
    block = re.search(r'  if\(ISAAC_VITA_LOG_ASYNC_PRINTF_FILE\)(.*?)  endif\(\)', cmake, re.S)[1]
    assert block.count('set_property(SOURCE') == 1 and block.count('"${ISAAC_RUNTIME}/') == 1
    assert 'platform.c' in block and 'host_vita_log_async.c' in block
    # The base leg reads the pre-knob logger from git history; a checkout
    # without that commit (the public snapshot) skips only this leg.
    have_base = subprocess.run(['git', '-C', str(ROOT.parent), 'cat-file', '-e', args.base + '^{commit}'],
                               capture_output=True).returncode == 0
    old = (run(['git', '-C', ROOT.parent, 'show', args.base + ':recomp/runtime/host_vita_log_async.c'])
           if have_base else None)
    current = (ROOT / 'runtime/host_vita_log_async.c').read_text()
    with tempfile.TemporaryDirectory(prefix='isaac-printf-file-') as tmp:
        work = Path(tmp)
        fixture = work / 'fixture.c'
        fixture.write_text(FIXTURE)
        flags = [args.clang, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                 '-D_CRT_SECURE_NO_WARNINGS', '-DISAAC_VITA_LOG_ASYNC_ORACLE=1',
                 '-I', ROOT / 'runtime']
        for ring in (4, 8, 256):
            for batch in (0, 1):
                for mirror in (0, 1):
                    defines = [f'-DISAAC_VITA_LOG_ASYNC_RING_KB={ring}',
                               f'-DISAAC_VITA_LOG_ASYNC_PRINTF_FILE={mirror}']
                    if batch:
                        defines.append('-DISAAC_VITA_LOG_ASYNC_FILE_BATCH=1')
                    exe = work / 'transport.exe'
                    run([*flags, *defines, ROOT / 'runtime/host_vita_log_async.c', fixture, '-o', exe])
                    print(run([exe]).strip() + f'; batch={batch}')
        # Same physical source path; no debug/filename differences can excuse an OFF delta.
        source = work / 'transport.c'
        identities = {}
        legs = [('absent', current, []), ('zero', current, ['-DISAAC_VITA_LOG_ASYNC_PRINTF_FILE=0'])]
        if have_base:
            legs.insert(0, ('base', old, []))
        else:
            print(f"SKIP base leg: commit {args.base} is not in this checkout's history; "
                  'absent == zero is still checked')
        for label, text, extra in legs:
            source.write_text(text)
            obj = work / (label + '.o')
            run([*flags, *extra, '-c', source, '-o', obj])
            data = bytearray(obj.read_bytes())
            data[4:8] = b'\0' * 4  # Windows COFF timestamp, not a code/data section.
            tokens = re.findall(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|\d+(?:\.\d*)?\w*|[^\s]',
                                run([*flags, *extra, '-E', '-P', source]))
            identities[label] = (bytes(data), tokens)
        assert all(identity == identities['absent'] for identity in identities.values())
        print('OFF whole logger tokens + x86 object identity PASS; only COFF timestamp normalized')
    print('CMake only two native owners; no generated unit/corpus/shader modification. ARM/device I/O gates not run.')


if __name__ == '__main__':
    main()
