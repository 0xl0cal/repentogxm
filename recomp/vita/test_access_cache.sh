#!/usr/bin/env bash
# Host oracles and scope checks for ISAAC_VITA_ACCESS_NEGATIVE_CACHE
# (recomp/runtime/host_vita_access_cache.c, the exact negative cache for
# _access probes below mods/).
#
# 1. The module oracle: pinned hit/miss/learn/bypass/invalidation/race/bound
#    cases and a randomized exactness run against a real directory tree, plain
#    and under ASan/UBSan.
# 2. The _access import (production host_vita_filesystem.c with fake psp2
#    headers) OFF and ON: identical guest-visible transcripts, fewer stats ON.
# 3. The CRT side (seek-shadow oracle with the knob, with and without
#    descriptor recovery): writing fopen and recovery invalidate.
# 4. CMake scope and the raw allocator gate entry.
# 5. With VITASDK set: the three owners and the module cross-compile under
#    the eboot's flags; the module references no allocator.
set -eu

root=${ISAAC_ACCESS_CACHE_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_ACCESS_CACHE_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-access-cache.XXXXXX")}
host_cc=${CC:-cc}
warn="-std=gnu11 -O2 -Wall -Wextra -Werror"

mkdir -p "$work"

# 1. Module oracle.
"$host_cc" $warn -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_access_cache_oracle.c" \
    -o "$work/access-cache-oracle"
mkdir -p "$work/plain" "$work/san"
"$work/access-cache-oracle" "$work/plain" 20000
"$host_cc" $warn -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_access_cache_oracle.c" \
    -o "$work/access-cache-oracle-san"
"$work/access-cache-oracle-san" "$work/san" 20000
# The randomized run must catch a cache that is never invalidated.
"$host_cc" $warn -DACCESS_ORACLE_MUTANT_NO_INVALIDATE=1 \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_access_cache_oracle.c" \
    -o "$work/access-cache-oracle-mutant"
mkdir -p "$work/mutant"
if "$work/access-cache-oracle-mutant" "$work/mutant" 20000 random \
        > "$work/mutant.log" 2>&1; then
    echo "the randomized oracle did not catch a missing invalidation" >&2
    exit 1
fi
echo "mutant without invalidation: caught ($(head -n 1 "$work/mutant.log"))"

# 2. The _access import, OFF and ON.  Fake psp2 headers: the constants are
# the vitasdk-softfp psp2common/kernel/iofilemgr.h values (octal).
fake="$work/fake_include"
mkdir -p "$fake/psp2/io" "$fake/psp2/kernel"
cat > "$fake/psp2/io/stat.h" <<'EOF'
#ifndef ORACLE_PSP2_IO_STAT_H
#define ORACLE_PSP2_IO_STAT_H
typedef enum SceIoMode {
    SCE_S_IWUSR = 000200,
    SCE_S_IFDIR = 0010000,
    SCE_S_IFREG = 0100000,
    SCE_S_IFMT  = 0170000
} SceIoMode;
#define SCE_S_ISDIR(m) (((m) & SCE_S_IFMT) == SCE_S_IFDIR)
typedef struct SceIoStat {
    SceIoMode st_mode;
    unsigned int st_attr;
    long long st_size;
} SceIoStat;
int sceIoGetstat(const char *file, SceIoStat *stat);
#endif
EOF
cat > "$fake/psp2/kernel/clib.h" <<'EOF'
int sceClibPrintf(const char *fmt, ...);
EOF
crt_common="$warn -fno-pie -ffunction-sections -fdata-sections -Wno-maybe-uninitialized -DGUEST_IMAGE_BASE=0x98000000u"
for knob in off on; do
    flag=
    extra=
    if [ "$knob" = on ]; then
        flag=-DISAAC_VITA_ACCESS_NEGATIVE_CACHE=1
        extra="$root/runtime/host_vita_access_cache.c"
    fi
    "$host_cc" $crt_common $flag -I"$fake" -I"$root/vita/qsort_oracle_include" \
        -I"$root/runtime" -I"$root/vita" \
        "$root/runtime/guest_stack_legacy_oracle_stub.c" \
        "$root/runtime/host_vita_filesystem_access_oracle.c" $extra \
        -Wl,--gc-sections -no-pie -o "$work/fs-access-$knob"
    "$work/fs-access-$knob" > "$work/fs-access-$knob.txt" 2> "$work/fs-access-$knob.err"
done
cmp "$work/fs-access-off.txt" "$work/fs-access-on.txt" ||
    { echo "_access transcripts differ OFF/ON" >&2; diff "$work/fs-access-off.txt" "$work/fs-access-on.txt" >&2; exit 1; }
off_stats=$(sed -n 's/^stats=\([0-9]*\).*/\1/p' "$work/fs-access-off.err")
on_stats=$(sed -n 's/^stats=\([0-9]*\).*/\1/p' "$work/fs-access-on.err")
test -n "$off_stats" && test -n "$on_stats" && test "$on_stats" -lt "$off_stats" ||
    { echo "ON must issue fewer stats: off=$off_stats on=$on_stats" >&2; exit 1; }
echo "_access import: $(wc -l < "$work/fs-access-on.txt") identical transcript lines OFF/ON; sceIoGetstat calls off=$off_stats on=$on_stats"
echo "  OFF $(grep '^redirect' "$work/fs-access-off.err"); ON $(grep '^redirect' "$work/fs-access-on.err")"

# 3. The CRT side.
for recover in off on; do
    flag=
    if [ "$recover" = on ]; then
        flag=-DISAAC_VITA_CRT_DESCRIPTOR_RECOVER=1
    fi
    "$host_cc" $crt_common -DISAAC_VITA_CRT_FILE_LOOKUP_HINT=1 $flag \
        -DISAAC_VITA_ACCESS_NEGATIVE_CACHE=1 \
        -I"$root/vita/qsort_oracle_include" -I"$root/runtime" -I"$root/vita" \
        "$root/runtime/guest_stack_legacy_oracle_stub.c" \
        "$root/runtime/host_vita_crt_seek_shadow_oracle.c" \
        "$root/runtime/host_vita_access_cache.c" \
        -Wl,--gc-sections -no-pie -o "$work/crt-access-$recover"
    mkdir -p "$work/crt-$recover"
    if ! "$work/crt-access-$recover" "$work/crt-$recover" 2000 \
            > "$work/crt-$recover.log" 2>&1; then
        cat "$work/crt-$recover.log" >&2
        exit 1
    fi
    grep -v '^KAGE VITA' "$work/crt-$recover.log" | tail -n 1
done

# 4. CMake scope and the gate.
cmake="$root/vita/CMakeLists.txt"
grep -q '^option(ISAAC_VITA_ACCESS_NEGATIVE_CACHE$' "$cmake"
test "$(grep -c 'ISAAC_VITA_ACCESS_NEGATIVE_CACHE=1' "$cmake")" -eq 1 ||
    { echo "the access cache define must appear once" >&2; exit 1; }
block=$(awk '/^  if\(ISAAC_VITA_ACCESS_NEGATIVE_CACHE\)$/,/^  endif\(\)$/' "$cmake")
test "$(printf '%s\n' "$block" | grep -c '"${ISAAC_RUNTIME}/')" -eq 4 ||
    { echo "access cache CMake block must name the module and three owners" >&2; exit 1; }
for owner in host_vita_access_cache.c host_vita_filesystem.c host_vita_crt.c \
             host_vita_startup.c; do
    printf '%s\n' "$block" | grep -q "\${ISAAC_RUNTIME}/$owner\"" ||
        { echo "access cache CMake scope lost $owner" >&2; exit 1; }
done
grep -q '("ISAAC_VITA_ACCESS_NEGATIVE_CACHE", "host_vita_access_cache.c")' \
    "$root/vita/vita_raw_allocator_gate.py"

# 5. Vita cross-compile.
if [ -n "${VITASDK:-}" ]; then
    cc="$VITASDK/bin/arm-vita-eabi-gcc"
    nm="$VITASDK/bin/arm-vita-eabi-nm"
    vita_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror -DGUEST_STACK_REQUIRED=1 -DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_HAS_RUNTIME=1 -DISAAC_VITA_RAW_ALLOCATOR_GATE=1 -include $root/vita/isaac_vita_raw_allocator_poison.h -DISAAC_VITA_ACCESS_NEGATIVE_CACHE=1"
    "$cc" $vita_flags -I"$root/runtime" -I"$root/vita" -c \
        "$root/runtime/host_vita_access_cache.c" -o "$work/access-cache.o"
    if "$nm" -u "$work/access-cache.o" | grep -Eq ' (malloc|calloc|realloc|free|memalign)$'; then
        echo "access cache references an allocator" >&2
        exit 1
    fi
    for unit in host_vita_filesystem host_vita_startup; do
        "$cc" $vita_flags -I"$root/runtime" -I"$root/vita" -c \
            "$root/runtime/$unit.c" -o "$work/$unit.o"
    done
    "$nm" -u "$work/host_vita_filesystem.o" | grep -q ' isaac_vita_access_cache_absent$'
    "$nm" -u "$work/host_vita_startup.o" | grep -q ' isaac_vita_access_cache_note_create$'
    echo "Vita cross-compile of the access cache and its owners: OK"
fi

sha256sum \
    "$root/runtime/host_vita_access_cache.h" \
    "$root/runtime/host_vita_access_cache.c" \
    "$root/runtime/host_vita_access_cache_oracle.c" \
    "$root/runtime/host_vita_filesystem_access_oracle.c" \
    "$root/vita/test_access_cache.sh"
echo "Vita _access negative cache host behavior: PASS"
