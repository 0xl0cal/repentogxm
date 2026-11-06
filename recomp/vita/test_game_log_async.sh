#!/usr/bin/env bash
# Host oracles and scope checks for ISAAC_VITA_GAME_LOG_ASYNC
# (recomp/runtime/host_vita_game_log.c, the game's log.txt write-behind lane).
#
# 1. Builds and runs recomp/runtime/host_vita_game_log_oracle.c against the
#    production module with a fake clock, disk and hand-driven worker, at the
#    default 64 KiB ring, the 32 KiB minimum, and every
#    ISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY 0..4: eligibility, open
#    modes, worker start failure, INFO deferral with a single wake, WARN drain
#    behind a stranded span, FIFO across ring wraps, maximal 10240-byte
#    records, ring-full waits and self-rescue, the stranded-worker FAILED
#    protocol and the late worker, append failure retry and persistence,
#    tell/seek/release, path and directory coherence, close/reuse, the abort
#    drain, the shutdown summary (parsed back) and binary bytes.
# 2. Builds and runs recomp/runtime/host_vita_crt_game_log_oracle.c, which
#    includes the production CRT TU and drives fopen/fwrite/fseek/ftell/
#    _fileno/_get_osfhandle/fflush/fread/vfprintf/fclose through guest-ABI
#    calls with the frozen logger's stack shape, with and without
#    ISAAC_VITA_GAME_LOG_BATCH (which must stay inert for the lane token).
# 3. Checks the CMake scope: the option and its two knobs exist with their
#    defaults, the three FATAL_ERROR guards, exactly the seven owners see the
#    macro, no generated unit or GL/KAGE owner does, no link option is added,
#    and the raw allocator gate knows the optional source.
# 4. With VITASDK set: cross-compiles the module for the Vita under the
#    eboot's flags and checks that no libc allocator is referenced.
set -eu

root=${ISAAC_GAME_LOG_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_GAME_LOG_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-game-log.XXXXXX")}
host_cc=${CC:-cc}
flags="-std=gnu11 -O2 -Wall -Wextra -Werror -DISAAC_VITA_GAME_LOG_ORACLE=1 -DISAAC_VITA_GAME_LOG_BUILD_ID=\"perf:game-log-oracle\""

mkdir -p "$work"

run_oracle() {
    if command -v timeout >/dev/null 2>&1; then
        timeout 60s "$1"
    else
        "$1"
    fi
}

# The lane module alone, at the default ring and the minimum ring.
for ring_kb in 64 32; do
    "$host_cc" $flags -DISAAC_VITA_GAME_LOG_ASYNC_RING_KB="${ring_kb}U" \
        -I"$root/runtime" -I"$root/vita" \
        "$root/runtime/host_vita_game_log.c" \
        "$root/runtime/host_vita_game_log_oracle.c" \
        -o "$work/game-log-host-oracle-${ring_kb}k"
    run_oracle "$work/game-log-host-oracle-${ring_kb}k"
done

# Every durable-severity policy, including 0 (all synchronous at fflush) and
# 4 (every chain severity deferred).
for durable in 0 1 2 3 4; do
    "$host_cc" $flags -DISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY="${durable}U" \
        -I"$root/runtime" -I"$root/vita" \
        "$root/runtime/host_vita_game_log.c" \
        "$root/runtime/host_vita_game_log_oracle.c" \
        -o "$work/game-log-host-oracle-durable${durable}"
    run_oracle "$work/game-log-host-oracle-durable${durable}"
done

# The CRT dispatch: include the production CRT TU (as test_async_write.sh
# does) and drive the FILE-token handlers through guest-ABI calls against the
# lane's oracle configuration, with and without the INFO batch classifier.
crt_common="-std=gnu11 -O2 -fno-pie -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-maybe-uninitialized -DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_GAME_LOG_ORACLE=1 -DISAAC_VITA_GAME_LOG_BUILD_ID=\"perf:game-log-oracle\""
crt_includes="-I$root/vita/qsort_oracle_include -I$root/runtime -I$root/vita"
for variant in plain batch; do
    extra=""
    if [ "$variant" = batch ]; then
        extra="-DISAAC_VITA_GAME_LOG_BATCH=1"
    fi
    "$host_cc" $crt_common $extra $crt_includes \
        "$root/runtime/guest_stack_legacy_oracle_stub.c" \
        "$root/runtime/host_vita_game_log.c" \
        "$root/runtime/host_vita_crt_game_log_oracle.c" \
        -Wl,--gc-sections -no-pie \
        -o "$work/crt-game-log-host-oracle-$variant"
    run_oracle "$work/crt-game-log-host-oracle-$variant"
done

# The production CMake scope: the macro reaches exactly the CRT FILE owner,
# the four path-based import owners, the process-exit tail, the abort hook
# owner and the lane itself; no generated unit and no GL/KAGE owner sees it;
# no link option is added (the OFF build links exactly what it links today).
cmake="$root/vita/CMakeLists.txt"
grep -q '^option(ISAAC_VITA_GAME_LOG_ASYNC$' "$cmake"
grep -q '^set(ISAAC_VITA_GAME_LOG_ASYNC_RING_KB 64 CACHE STRING$' "$cmake"
grep -q '^set(ISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY 1 CACHE STRING$' "$cmake"
grep -q 'ISAAC_VITA_GAME_LOG_ASYNC requires ISAAC_VITA_ARCHIVE_FILE_CACHE=ON' "$cmake"
grep -q 'ISAAC_VITA_GAME_LOG_ASYNC requires ISAAC_VITA_LOG_ASYNC=ON' "$cmake"
grep -q 'ISAAC_VITA_GAME_LOG_ASYNC requires the real Vita runtime' "$cmake"
grep -q 'ISAAC_VITA_GAME_LOG_ASYNC_RING_KB must be an integer in 32..1024' "$cmake"
grep -q 'ISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY must be 0..4' "$cmake"
test "$(grep -c 'ISAAC_VITA_GAME_LOG_ASYNC=1' "$cmake")" -eq 1 \
    || { echo "ISAAC_VITA_GAME_LOG_ASYNC=1 must be defined in exactly one place" >&2; exit 1; }
test "$(grep -c 'host_vita_game_log\.c' "$cmake")" -eq 3 \
    || { echo "host_vita_game_log.c must appear exactly three times (list, owner set, knob set)" >&2; exit 1; }
block=$(awk '/^  if\(ISAAC_VITA_GAME_LOG_ASYNC\)$/,/^  endif\(\)$/' "$cmake")
test -n "$block" || { echo "game-log CMake block not found" >&2; exit 1; }
for owner in host_vita_crt.c host_vita_filesystem.c host_vita_startup.c \
             host_vita_find.c entry_vita.c host_vita_log_async.c \
             host_vita_game_log.c; do
    printf '%s\n' "$block" | grep -q "\${ISAAC_RUNTIME}/$owner\"" \
        || { echo "game-log CMake scope lost $owner" >&2; exit 1; }
done
test "$(printf '%s\n' "$block" | grep -c '"${ISAAC_RUNTIME}/')" -eq 9 \
    || { echo "game-log CMake block lists an unexpected number of runtime sources" >&2; exit 1; }
if printf '%s\n' "$block" | grep -q 'ISAAC_GENERATED\|gl_vita_backend\|kage_vita\|guest\.c'; then
    echo "game-log CMake scope leaked beyond the CRT/path/exit/abort owners" >&2
    exit 1
fi
if printf '%s\n' "$block" | grep -q -- '--wrap\|target_link_options'; then
    echo "game-log CMake block must not add link options" >&2
    exit 1
fi
printf '%s\n' "$block" | grep -q 'ISAAC_VITA_GAME_LOG_ASYNC_RING_KB=${ISAAC_VITA_GAME_LOG_ASYNC_RING_KB}U'
printf '%s\n' "$block" | grep -q 'ISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY=${ISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY}U'
printf '%s\n' "$block" | grep -q 'ISAAC_VITA_GAME_LOG_BUILD_ID='
# The raw allocator gate must know the optional source so the fail-closed
# object closure accepts the ON build, and must verify the owner scope.
grep -q '("ISAAC_VITA_GAME_LOG_ASYNC", "host_vita_game_log.c")' \
    "$root/vita/vita_raw_allocator_gate.py"
grep -q '^GAME_LOG_ASYNC_CACHE_KEY = "ISAAC_VITA_GAME_LOG_ASYNC"$' \
    "$root/vita/vita_raw_allocator_gate.py"
grep -q '^def verify_game_log_async_compile_scope($' \
    "$root/vita/vita_raw_allocator_gate.py"

if [ -n "${VITASDK:-}" ]; then
    cc="$VITASDK/bin/arm-vita-eabi-gcc"
    nm="$VITASDK/bin/arm-vita-eabi-nm"
    vita_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror -DGUEST_STACK_REQUIRED=1 -DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_HAS_RUNTIME=1 -DISAAC_VITA_RAW_ALLOCATOR_GATE=1 -include $root/vita/isaac_vita_raw_allocator_poison.h -DISAAC_VITA_GAME_LOG_ASYNC=1 -DISAAC_VITA_GAME_LOG_ASYNC_RING_KB=64U -DISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY=1U"
    "$cc" $vita_flags -I"$root/runtime" -I"$root/vita" -c \
        "$root/runtime/host_vita_game_log.c" -o "$work/game-log.o"
    symbols=$("$nm" "$work/game-log.o")
    for symbol in isaac_vita_game_log_open isaac_vita_game_log_write \
                  isaac_vita_game_log_flush isaac_vita_game_log_shutdown \
                  isaac_vita_game_log_abort_drain; do
        echo "$symbols" | grep -q " T $symbol\$" ||
            { echo "missing $symbol" >&2; exit 1; }
    done
    echo "$symbols" | grep -q " U sceIoWrite\$" ||
        { echo "missing sceIoWrite reference" >&2; exit 1; }
    if echo "$symbols" | grep -Eq " U (malloc|calloc|realloc|free)\$"; then
        echo "game-log module references a raw libc allocator" >&2
        exit 1
    fi
    echo "Vita cross-compile of host_vita_game_log.c: OK"
fi

sha256sum \
    "$root/runtime/host_vita_game_log.h" \
    "$root/runtime/host_vita_game_log.c" \
    "$root/runtime/host_vita_game_log_oracle.c" \
    "$root/runtime/host_vita_crt_game_log_oracle.c" \
    "$root/runtime/host_vita_crt.c" \
    "$root/vita/test_game_log_async.sh" \
    "$work/game-log-host-oracle-64k" \
    "$work/crt-game-log-host-oracle-plain"
echo "Vita game log write-behind host behavior: PASS"
