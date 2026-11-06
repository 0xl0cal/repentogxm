#!/usr/bin/env bash
# Host oracle and Vita compile checks for ISAAC_VITA_OGG_READAHEAD
# (recomp/runtime/host_vita_ogg_readahead.c).
#
# 1. Host oracle (host_vita_ogg_readahead_oracle.c, Linux/glibc, gcc or
#    clang): the module's three newlib wraps against POSIX descriptors, in
#    the production geometry and five edge geometries, plus a --coverage
#    build whose gcov branch summary is printed.  OGG input: the files named
#    in ISAAC_OGG_RA_TEST_OGG (space separated) or, when ffmpeg with
#    libvorbis is available ($FFMPEG, default ffmpeg), four deterministic
#    lavfi renders.  Without either the stdio/blocks decode families are
#    SKIPPED and only the byte-trace and fault families run.  Then six
#    seeded mutants that must fail, the CRT hook leg, and the device gates
#    evaluator (test_ogg_readahead_gates.py, python3) fed with this run's
#    own suspend receipts.
# 2. With VITASDK set: Vita compiles of the module (three geometries, with
#    the raw-allocator poison every eboot unit gets), host_vita_crt.c and
#    host_vita_native_vorbis.c with the knob, the symbol contract of the
#    link-time wraps, and the CRT's refusal to build the knob without
#    ISAAC_VITA_CRT_DESCRIPTOR_RECOVER.  Without VITASDK this part is
#    SKIPPED (an ARM build on the Linux build host runs it).
#
#   test_ogg_readahead.sh                 run everything that can run here
#   test_ogg_readahead.sh --render <dir>  only render the four OGGs into <dir>
set -eu

root=${ISAAC_OGG_RA_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
ffmpeg=${FFMPEG:-ffmpeg}

render_oggs() {
    out=$1
    render() { "$ffmpeg" -hide_banner -loglevel error -y "$@"; }
    # 60 s stereo 48 kHz, two tones
    render -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=60" \
        -f lavfi -i "sine=frequency=554:sample_rate=48000:duration=60" \
        -filter_complex "[0:a][1:a]join=inputs=2:channel_layout=stereo[a]" \
        -map "[a]" -c:a libvorbis -q:a 5 "$out/stereo48_q5.ogg"
    # 8 s mono 44.1 kHz: smaller than one 64 KiB window
    render -f lavfi -i "sine=frequency=330:sample_rate=44100:duration=8" \
        -c:a libvorbis -q:a 3 "$out/mono44_q3.ogg"
    # 20 s stereo noise at q10: about 1.3 MB, many pages per chunk
    render -f lavfi -i "anoisesrc=color=pink:sample_rate=48000:duration=20:seed=7" \
        -f lavfi -i "anoisesrc=color=brown:sample_rate=48000:duration=20:seed=9" \
        -filter_complex "[0:a][1:a]join=inputs=2:channel_layout=stereo[a]" \
        -map "[a]" -c:a libvorbis -q:a 10 "$out/noise48_q10.ogg"
    # 4 s 5.1
    render -f lavfi -i "sine=frequency=220:sample_rate=48000:duration=4" \
        -f lavfi -i "sine=frequency=330:sample_rate=48000:duration=4" \
        -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=4" \
        -f lavfi -i "sine=frequency=550:sample_rate=48000:duration=4" \
        -f lavfi -i "sine=frequency=660:sample_rate=48000:duration=4" \
        -f lavfi -i "sine=frequency=770:sample_rate=48000:duration=4" \
        -filter_complex "[0:a][1:a][2:a][3:a][4:a][5:a]join=inputs=6:channel_layout=5.1[a]" \
        -map "[a]" -c:a libvorbis -q:a 4 "$out/six48_q4.ogg"
}

if [ "${1:-}" = "--render" ]; then
    [ -n "${2:-}" ] || { echo "usage: $0 --render <dir>" >&2; exit 2; }
    mkdir -p "$2"
    render_oggs "$2"
    ls -l "$2"
    exit 0
fi

if [ -n "${ISAAC_OGG_RA_TEST_OUT:-}" ]; then
    work=$ISAAC_OGG_RA_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-ogg-readahead.XXXXXX")
fi

module="$root/runtime/host_vita_ogg_readahead.c"
oracle="$root/runtime/host_vita_ogg_readahead_oracle.c"
host_cc=${HOST_CC:-cc}
host_flags="-std=gnu11 -O2 -fno-strict-aliasing -Wall -Wextra -Werror -Wno-error=maybe-uninitialized -pthread"

# 1. Host oracle.
oggs=${ISAAC_OGG_RA_TEST_OGG:-}
if [ -z "$oggs" ] && command -v "$ffmpeg" >/dev/null 2>&1 &&
        "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q libvorbis; then
    mkdir -p "$work/ogg"
    render_oggs "$work/ogg"
    oggs="$work/ogg/stereo48_q5.ogg $work/ogg/mono44_q3.ogg $work/ogg/noise48_q10.ogg $work/ogg/six48_q4.ogg"
fi

build_oracle() {
    name=$1
    shift
    "$host_cc" $host_flags -DISAAC_VITA_OGG_READAHEAD_ORACLE=1 "$@" \
        -I"$root/runtime" "$module" "$oracle" -lm -o "$work/$name"
}

run_oracle() {
    name=$1
    mkdir -p "$work/run-$name"
    # shellcheck disable=SC2086
    "$work/$name" "$work/run-$name" $oggs > "$work/$name.log" 2>&1 || {
        cat "$work/$name.log"
        echo "ogg readahead oracle $name: FAIL" >&2
        exit 1
    }
    grep -E "^ogg readahead oracle: (chunk=|PASS)" "$work/$name.log"
    rm -f "$work/run-$name/ra-raw.a" "$work/run-$name/ra-blk.a"
}

# production geometry: 4 x 16 KiB, 20 ms wait (the CMake defaults)
build_oracle ra-oracle
run_oracle ra-oracle
cat "$work/ra-oracle.log"
# every branch counter of the census must have moved, lock_busy included:
# scenario lock-busy holds the entry lock across three reads, so the busy
# path runs by construction (it once ran only by chance)
census_count() {
    awk -v key="$2" '$1 == key { print $2; exit }' "$1"
}
busy=$(census_count "$work/ra-oracle.log" lock_busy)
[ "${busy:-0}" -ge 3 ] || {
    echo "ogg readahead oracle: lock_busy=${busy:-missing}, want >= 3" >&2
    exit 1
}
echo "ogg readahead lock-busy path: $busy busy reads (>= 3 from the held lock)"

# two chunks: the smallest window the module accepts (32 KiB)
build_oracle ra-oracle-c2 -DISAAC_OGG_RA_CHUNKS=2U
RA_TRACE_SEEDS=6 run_oracle ra-oracle-c2
# sixteen chunks: 256 KiB window, the largest CMake allows
build_oracle ra-oracle-c16 -DISAAC_OGG_RA_CHUNKS=16U
RA_TRACE_SEEDS=6 run_oracle ra-oracle-c16
# a 2 ms wait: more PENDING timeouts handed to the real read
build_oracle ra-oracle-w2ms -DISAAC_OGG_RA_WAIT_US=2000U
RA_TRACE_SEEDS=6 run_oracle ra-oracle-w2ms
# no wait at all (the CMake minimum): a PENDING chunk is always a miss, so
# the waits row stays zero by construction
build_oracle ra-oracle-w0 -DISAAC_OGG_RA_WAIT_US=0U
RA_TRACE_SEEDS=6 RA_CENSUS_EXEMPT=waits run_oracle ra-oracle-w0
# a single lock try on the game side: a stress geometry for the trace
# families (it makes contention with the worker likelier, it does not force
# it); the forced busy path is scenario lock-busy, required in every run
build_oracle ra-oracle-spin1 -DISAAC_OGG_RA_GAME_SPINS=1U
RA_TRACE_SEEDS=6 run_oracle ra-oracle-spin1

# branch coverage of the module under the production geometry
if command -v gcov >/dev/null 2>&1 && [ "${host_cc##*/}" != "clang" ]; then
    cov="$work/cov"
    mkdir -p "$cov"
    "$host_cc" -std=gnu11 -O0 -fno-strict-aliasing -Wall -Wextra -Werror \
        -Wno-error=maybe-uninitialized -pthread --coverage \
        -DISAAC_VITA_OGG_READAHEAD_ORACLE=1 -I"$root/runtime" \
        -c "$module" -o "$cov/host_vita_ogg_readahead.o"
    "$host_cc" $host_flags -DISAAC_VITA_OGG_READAHEAD_ORACLE=1 \
        -I"$root/runtime" -c "$oracle" -o "$cov/oracle.o"
    "$host_cc" -pthread --coverage "$cov/host_vita_ogg_readahead.o" \
        "$cov/oracle.o" -lm -o "$cov/ra-oracle-cov"
    mkdir -p "$cov/run"
    # shellcheck disable=SC2086
    "$cov/ra-oracle-cov" "$cov/run" $oggs > "$cov/run.log" 2>&1 || {
        cat "$cov/run.log"
        echo "ogg readahead coverage run: FAIL" >&2
        exit 1
    }
    rm -f "$cov/run/ra-raw.a" "$cov/run/ra-blk.a"
    (cd "$cov" && gcov -b -o . host_vita_ogg_readahead.o > gcov.txt 2>&1) || {
        cat "$cov/gcov.txt"
        exit 1
    }
    sed -n "/host_vita_ogg_readahead.c'/,/^\$/p" "$cov/gcov.txt"
    # host-only helpers and the Vita arms are compiled out of this build;
    # list the lines of the module that never ran
    echo "never executed lines (host build):"
    grep -n "#####" "$cov/host_vita_ogg_readahead.c.gcov" | head -40 || true
else
    echo "ogg readahead branch coverage: SKIPPED (gcov with gcc needed)"
fi

# The oracle must catch seeded exactness bugs: each mutant drops one step
# the exactness argument depends on and has to FAIL.  python3 does the
# edits so that a vanished anchor fails loudly instead of testing nothing.
mutants="hit-keeps-synced hit-keeps-errno eof-keeps-errno never-sync foreign-no-sync busy-as-hit"
mkdir -p "$work/mutants"
for mutant in $mutants; do
    python3 - "$module" "$work/mutants/$mutant.c" "$mutant" <<'PY'
import sys
src, dst, name = sys.argv[1:4]
text = open(src, encoding="utf-8").read()
edits = {
    # the kernel position stays behind after a hit (the bug the oracle
    # found on its first run)
    "hit-keeps-synced": ("        e->synced = 0U;\n        ++s_stats.hits;\n",
                         "        ++s_stats.hits;\n"),
    # newlib's _read_r clears errno on success
    "hit-keeps-errno": ("        r->_errno = 0;\n        ra_signal();\n"
                        "        return (ra_ssize_t)(end - vpos);\n",
                        "        ra_signal();\n"
                        "        return (ra_ssize_t)(end - vpos);\n"),
    "eof-keeps-errno": ("        ++s_stats.hit_eof;\n        r->_errno = 0;\n",
                        "        ++s_stats.hit_eof;\n"),
    # forwarded calls run at a stale kernel position
    "never-sync": ("    if (e->synced)\n        return 1;\n",
                   "    if (1)\n        return 1;\n"),
    # a foreign call reaches the kernel without the position handed back
    "foreign-no-sync": ("    ra_add(&s_stats.foreign, 1U);\n    if (!e->synced) {\n",
                        "    ra_add(&s_stats.foreign, 1U);\n    if (0 && !e->synced) {\n"),
    # a read that lost the lock race answers from chunks it never checked
    "busy-as-hit": ("    if (verdict == RA_COVERED) {\n",
                    "    if (verdict == RA_COVERED || verdict == RA_BUSY) {\n"),
}
old, new = edits[name]
if text.count(old) != 1:
    sys.exit(f"mutant {name}: anchor found {text.count(old)} times, expected 1")
open(dst, "w", encoding="utf-8").write(text.replace(old, new))
PY
    "$host_cc" $host_flags -Wno-error -DISAAC_VITA_OGG_READAHEAD_ORACLE=1 \
        -I"$root/runtime" "$work/mutants/$mutant.c" "$oracle" -lm \
        -o "$work/mutants/$mutant"
    mkdir -p "$work/mutants/run-$mutant"
    # shellcheck disable=SC2086
    if RA_TRACE_SEEDS=2 "$work/mutants/$mutant" "$work/mutants/run-$mutant" \
            $oggs > "$work/mutants/$mutant.log" 2>&1; then
        echo "ogg readahead mutant $mutant: survived (the oracle missed it)" >&2
        exit 1
    fi
    rm -f "$work/mutants/run-$mutant/ra-raw.a" "$work/mutants/run-$mutant/ra-blk.a"
    echo "ogg readahead mutant $mutant: killed ($(grep -c '^FAIL' "$work/mutants/$mutant.log") failures)"
done

# The CRT hooks on the host: the production CRT unit (through the
# ISAAC_VITA_CRT_FREAD_COMPLETE oracle, with the release CRT lane set and the
# flags of test_crt_fread_complete.sh) built with the knob, against a stub
# module that holds the context at 1 for the whole run.  The oracle must pass
# unchanged (the hooks touch no CRT state), the fread hook must fire with a
# live descriptor and a path, and fclose must hand descriptors back.
crt="$work/crt"
mkdir -p "$crt"
cat > "$crt/ra_stub.c" <<'EOF'
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "host_vita_ogg_readahead.h"
int isaac_vita_ogg_ra_context;
static unsigned s_notes, s_bad, s_not_ro, s_other_file, s_forgets;
void isaac_vita_ogg_ra_note(int fd, const char *path)
{
    struct stat by_fd;
    struct stat by_path;
    int flags;

    ++s_notes;
    if (fd < 0 || (flags = fcntl(fd, F_GETFL)) == -1 || !path || !path[0]) {
        ++s_bad;
        return;
    }
    /* the worker opens `path` read-only for its own preads: the hook must
     * hand over only read-only descriptors, `path` must be the guest's
     * native path (under the oracle's game root), and the descriptor must be the
     * file a read-mode open gave (the oracle maps every read-mode native
     * open to its fixture and every write-mode one to its scratch file) */
    if ((flags & O_ACCMODE) != O_RDONLY)
        ++s_not_ro;
    static const char root[] = "ux0:/data/isaacr001/";
    if (strncmp(path, root, sizeof root - 1U) != 0 ||
        fstat(fd, &by_fd) != 0 || stat(RA_STUB_FIXTURE, &by_path) != 0 ||
        by_fd.st_dev != by_path.st_dev || by_fd.st_ino != by_path.st_ino)
        ++s_other_file;
}
void isaac_vita_ogg_ra_forget(int fd) { (void)fd; ++s_forgets; }
void isaac_vita_ogg_ra_report_if_due(void) {}
void isaac_vita_ogg_ra_snapshot(isaac_vita_ogg_ra_stats *out) { (void)out; }
__attribute__((constructor)) static void ra_stub_on(void)
{
    isaac_vita_ogg_ra_context = 1;
}
__attribute__((destructor)) static void ra_stub_report(void)
{
    printf("ogg readahead CRT hooks: notes=%u bad=%u not_ro=%u other_file=%u "
           "forgets=%u\n", s_notes, s_bad, s_not_ro, s_other_file, s_forgets);
    fflush(stdout);
    if (!s_notes || s_bad || s_not_ro || s_other_file || !s_forgets)
        _exit(3);
}
EOF
crt_common="-std=gnu11 -O2 -fno-pie -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-maybe-uninitialized -DGUEST_IMAGE_BASE=0x98000000u"
crt_includes="-I$root/vita/qsort_oracle_include -I$root/runtime -I$root/vita"
"$host_cc" $crt_common $crt_includes -DISAAC_VITA_CRT_SEEK_SHADOW=1 \
    "-DRA_STUB_FIXTURE=\"$crt/fread-complete-fixture.bin\"" \
    -DISAAC_VITA_CRT_DESCRIPTOR_RECOVER=1 -DISAAC_VITA_CRT_FILE_LOOKUP_HINT=1 \
    -DISAAC_VITA_OGG_READAHEAD=1 \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/host_vita_crt_fread_complete_oracle.c" "$crt/ra_stub.c" \
    -Wl,--gc-sections -no-pie -o "$crt/crt-fread-ra"
"$crt/crt-fread-ra" "$crt" 2000 > "$crt/run.log" 2>&1 || {
    cat "$crt/run.log"
    echo "ogg readahead CRT hook leg: FAIL" >&2
    exit 1
}
grep -E "^(ogg readahead CRT hooks|fread complete oracle)" "$crt/run.log"

# The device gates (ogg_readahead_gates.py) against the module's own receipt
# lines: the suspend and suspend-late-open STATS/WORKER pairs of the run
# above go through the G8/G2 suspend cases, next to the P2b-derived G1-G5
# cases.  The first-draft gate text (--rules r0) must still fail on them:
# that is its two defects, reproduced.
gates_test="$root/vita/test_ogg_readahead_gates.py"
python3 "$gates_test" --oracle-log "$work/ra-oracle.log" > "$work/gates.log" 2>&1 || {
    cat "$work/gates.log"
    echo "ogg readahead gates: FAIL" >&2
    exit 1
}
grep -E "^(suspend receipts|ogg readahead gates|[0-9]+/[0-9]+ cases)" "$work/gates.log"
if python3 "$gates_test" --rules r0 --oracle-log "$work/ra-oracle.log" \
        > "$work/gates-r0.log" 2>&1; then
    cat "$work/gates-r0.log"
    echo "ogg readahead gates: the first-draft gate text (r0) passed the defect cases" >&2
    exit 1
fi
grep -E "^failed:" "$work/gates-r0.log" | sed "s/^/ogg readahead gates r0 (expected) /"

if [ -z "$oggs" ]; then
    echo "ogg readahead decode proofs: SKIP (no OGG input; set ISAAC_OGG_RA_TEST_OGG or install ffmpeg+libvorbis)"
else
    echo "ogg readahead decode proofs: PASS (stdio PCM and blocks PCM identical on both paths, 6 geometries)"
fi

# 2. Vita compile.
if [ -z "${VITASDK:-}" ]; then
    echo "ogg readahead Vita compile: SKIPPED (VITASDK unset)"
    echo "ogg readahead oracle: PASS ($work)"
    exit 0
fi
cc="$VITASDK/bin/arm-vita-eabi-gcc"
nm="$VITASDK/bin/arm-vita-eabi-nm"
common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
# The eboot compiles every runtime unit under the raw-allocator poison gate.
production_flags="$common_flags -DGUEST_STACK_REQUIRED=1 -DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_HAS_RUNTIME=1 -DISAAC_VITA_RAW_ALLOCATOR_GATE=1 -include $root/vita/isaac_vita_raw_allocator_poison.h"

for geometry in "4U 20000U 168" "2U 0U 64" "16U 200000U 191"; do
    # shellcheck disable=SC2086
    set -- $geometry
    object="$work/ra-vita-$1.o"
    "$cc" $production_flags -DISAAC_VITA_OGG_READAHEAD=1 \
        -DISAAC_OGG_RA_CHUNKS="$1" -DISAAC_OGG_RA_WAIT_US="$2" \
        -DISAAC_OGG_RA_PRIORITY="$3" \
        -I"$root/runtime" -I"$root/vita" -c "$module" -o "$object"
    symbols=$("$nm" "$object")
    for call in _read_r _lseek_r _close_r; do
        echo "$symbols" | grep -q " T __wrap_$call\$" ||
            { echo "missing __wrap_$call in $object" >&2; exit 1; }
        echo "$symbols" | grep -q " U __real_$call\$" ||
            { echo "missing __real_$call reference in $object" >&2; exit 1; }
    done
    for entry in isaac_vita_ogg_ra_note isaac_vita_ogg_ra_forget \
            isaac_vita_ogg_ra_report_if_due isaac_vita_ogg_ra_snapshot; do
        echo "$symbols" | grep -q " T $entry\$" ||
            { echo "missing $entry in $object" >&2; exit 1; }
    done
    echo "$symbols" | grep -Eq " [BCD] isaac_vita_ogg_ra_context\$" ||
        { echo "missing isaac_vita_ogg_ra_context in $object" >&2; exit 1; }
    if echo "$symbols" | grep -Eq "oracle|pthread| U (malloc|calloc|realloc|free|memalign)\$"; then
        echo "host-only or raw-allocator symbol in $object" >&2
        exit 1
    fi
    # the worker's descriptor is its own SceIo UID: pread, never read/lseek
    echo "$symbols" | grep -q " U sceIoPread\$" ||
        { echo "missing sceIoPread reference in $object" >&2; exit 1; }
    if echo "$symbols" | grep -Eq " U (sceIoRead|sceIoLseek|sceIoLseek32)\$"; then
        echo "worker uses a positional SceIo call in $object" >&2
        exit 1
    fi
done

crt_flags="$common_flags -DGUEST_STACK_REQUIRED=1 -DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_HAS_RUNTIME=1 -DISAAC_VITA_ARCHIVE_FILE_CACHE=1"
"$cc" $crt_flags -DISAAC_VITA_CRT_DESCRIPTOR_RECOVER=1 \
    -DISAAC_VITA_OGG_READAHEAD=1 \
    -I"$root/runtime" -I"$root/vita" -c "$root/runtime/host_vita_crt.c" \
    -o "$work/crt-ra.o"
crt_symbols=$("$nm" "$work/crt-ra.o")
for entry in isaac_vita_ogg_ra_note isaac_vita_ogg_ra_forget \
        isaac_vita_ogg_ra_context; do
    echo "$crt_symbols" | grep -q " U $entry\$" ||
        { echo "host_vita_crt.c does not reference $entry" >&2; exit 1; }
done
if "$cc" $crt_flags -DISAAC_VITA_OGG_READAHEAD=1 \
        -I"$root/runtime" -I"$root/vita" -c "$root/runtime/host_vita_crt.c" \
        -o "$work/crt-ra-norecover.o" 2>/dev/null; then
    echo "OGG_READAHEAD without CRT_DESCRIPTOR_RECOVER must not compile" >&2
    exit 1
fi

nv_flags="$production_flags -DISAAC_VITA_NATIVE_VORBIS=1 -DISAAC_VITA_NATIVE_VORBIS_BUILD_ID=\"native-vorbis:test\" -DISAAC_VITA_NATIVE_VORBIS_WORKER=1 -DISAAC_VITA_NATIVE_VORBIS_RECEIPT=1 -DISAAC_VITA_NATIVE_VORBIS_ASYNC=1"
"$cc" $nv_flags -DISAAC_VITA_OGG_READAHEAD=1 \
    -I"$root/runtime" -I"$root/vita" -c "$root/runtime/host_vita_native_vorbis.c" \
    -o "$work/nv-ra.o"
nv_symbols=$("$nm" "$work/nv-ra.o")
for entry in isaac_vita_ogg_ra_context isaac_vita_ogg_ra_report_if_due; do
    echo "$nv_symbols" | grep -q " U $entry\$" ||
        { echo "host_vita_native_vorbis.c does not reference $entry" >&2; exit 1; }
done
echo "ogg readahead Vita compile: PASS (module in 3 geometries with the raw-allocator poison, __wrap_/__real_ contract for _read_r/_lseek_r/_close_r, no host or raw-allocator symbol, CRT and native Vorbis hooks, knob refused without DESCRIPTOR_RECOVER)"
echo "ogg readahead oracle: PASS ($work)"
