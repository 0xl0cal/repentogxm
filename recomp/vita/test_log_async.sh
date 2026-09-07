#!/usr/bin/env bash
# Host oracle and scope checks for ISAAC_VITA_LOG_ASYNC (host_vita_log_async.c).
#
# 1. Builds and runs recomp/runtime/host_vita_log_async_oracle.c against the
#    production module with fake sinks/clock/thread step: FIFO order across two
#    producers on both sinks, byte identity, a 512-byte and a maximal line,
#    ring-full drop accounting and the rate-bounded dropped=N report, flush,
#    synchronous fallback before start, wake discipline, and the hostile
#    cases (ring-end straddle sweep, binary bytes, exact fit after a refusal,
#    overload report position, over-long printf written synchronously with
#    exact bytes, mixed-size two-producer fill).
# 2. Checks the CMake scope: the option exists, exactly the eight owners see
#    the macro, abort() is wrapped, the raw allocator gate knows the source.
# 3. With VITASDK set: cross-compiles the module for the Vita under the
#    eboot's flags and checks the __wrap_abort/__real_abort contract and that
#    no libc allocator is referenced.
set -eu

root=${ISAAC_LOG_ASYNC_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_LOG_ASYNC_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-log-async.XXXXXX")}
host_cc=${CC:-cc}
flags="-std=gnu11 -O2 -Wall -Wextra -Werror -DISAAC_VITA_LOG_ASYNC_ORACLE=1"

mkdir -p "$work"

"$host_cc" $flags -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_log_async.c" \
    "$root/runtime/host_vita_log_async_oracle.c" \
    -o "$work/log-async-host-oracle"
if command -v timeout >/dev/null 2>&1; then
    timeout 20s "$work/log-async-host-oracle"
else
    "$work/log-async-host-oracle"
fi

# A small ring (the knob) must still pass every check.
"$host_cc" $flags -DISAAC_VITA_LOG_ASYNC_RING_KB=8 -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_log_async.c" \
    "$root/runtime/host_vita_log_async_oracle.c" \
    -o "$work/log-async-host-oracle-8k"
"$work/log-async-host-oracle-8k"

# The same oracle checks actual native batch append/recovery, exact bytes,
# record boundaries and no-lock fallback, with both supported ring sizes.
for ring_kb in 256 8; do
    "$host_cc" $flags -DISAAC_VITA_LOG_ASYNC_FILE_BATCH=1 \
        -DISAAC_VITA_LOG_ASYNC_RING_KB="$ring_kb" \
        -I"$root/runtime" -I"$root/vita" \
        "$root/runtime/host_vita_log_async.c" \
        "$root/runtime/host_vita_log_async_oracle.c" \
        -o "$work/log-async-file-batch-oracle-${ring_kb}k"
    "$work/log-async-file-batch-oracle-${ring_kb}k"
done

cmake="$root/vita/CMakeLists.txt"
grep -q '^option(ISAAC_VITA_LOG_ASYNC$' "$cmake"
grep -q '^set(ISAAC_VITA_LOG_ASYNC_RING_KB 256 CACHE STRING$' "$cmake"
grep -q '^option(ISAAC_VITA_LOG_ASYNC_FILE_BATCH$' "$cmake"
grep -q '^if(ISAAC_VITA_LOG_ASYNC_FILE_BATCH AND NOT ISAAC_VITA_LOG_ASYNC)$' "$cmake"
batch_block=$(awk '/^  if\(ISAAC_VITA_LOG_ASYNC_FILE_BATCH\)$/,/^  endif\(\)$/' "$cmake")
printf '%s\n' "$batch_block" | grep -q '^      platform.c$'
printf '%s\n' "$batch_block" | grep -q '"${ISAAC_RUNTIME}/host_vita_log_async.c"'
test "$(printf '%s\n' "$batch_block" | grep -c '"${ISAAC_RUNTIME}/')" -eq 1
block=$(awk '/^  if\(ISAAC_VITA_LOG_ASYNC\)$/,/^  endif\(\)$/' "$cmake")
for owner in entry_vita.c host_vita_log_async.c kage_vita_phase_profile.c \
             kage_vita_guest_sampler.c kage_vita_backend.c kage_vita_input.c \
             kage_vita_generated_hooks.c; do
    printf '%s\n' "$block" | grep -q "\${ISAAC_RUNTIME}/$owner\"" \
        || { echo "log-async CMake scope lost $owner" >&2; exit 1; }
done
printf '%s\n' "$block" | grep -q '^      platform.c$' \
    || { echo "log-async CMake scope lost platform.c" >&2; exit 1; }
printf '%s\n' "$block" | grep -q -- '-Wl,--wrap=abort' \
    || { echo "log-async CMake scope lost --wrap=abort" >&2; exit 1; }
if printf '%s\n' "$block" | grep -q 'ISAAC_GENERATED\|gl_vita_backend\|guest\.c'; then
    echo "log-async CMake scope leaked into generated units or guest.c" >&2
    exit 1
fi
grep -q '("ISAAC_VITA_LOG_ASYNC", "host_vita_log_async.c")' \
    "$root/vita/vita_raw_allocator_gate.py"

# Every routed owner spells the macro, none of them still calls the kernel
# printf at a routed site: the phase profiler's record sites (26 when this
# landed; a new record adds one) are KVPP_PRINTF, the other owners use
# ISAAC_VITA_LOG_PRINTF.
profile="$root/runtime/kage_vita_phase_profile.c"
test "$(grep -c '^    KVPP_PRINTF($' "$profile")" -ge 26 \
    || { echo "phase profiler lost KVPP_PRINTF record sites (expected >= 26)" >&2; exit 1; }
test "$(grep -c 'sceClibPrintf(' "$profile")" -eq 1 \
    || { echo "phase profiler still calls sceClibPrintf outside the macro (only the oracle declaration may remain)" >&2; exit 1; }
for owner in kage_vita_guest_sampler.c kage_vita_backend.c kage_vita_input.c \
             kage_vita_generated_hooks.c; do
    grep -q 'ISAAC_VITA_LOG_PRINTF(' "$root/runtime/$owner" \
        || { echo "$owner lost its ISAAC_VITA_LOG_PRINTF route" >&2; exit 1; }
done

if [ -n "${VITASDK:-}" ]; then
    cc="$VITASDK/bin/arm-vita-eabi-gcc"
    nm="$VITASDK/bin/arm-vita-eabi-nm"
    vita_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror -DGUEST_STACK_REQUIRED=1 -DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_HAS_RUNTIME=1 -DISAAC_VITA_RAW_ALLOCATOR_GATE=1 -include $root/vita/isaac_vita_raw_allocator_poison.h -DISAAC_VITA_LOG_ASYNC=1 -DISAAC_VITA_LOG_ASYNC_RING_KB=256"
    "$cc" $vita_flags -I"$root/runtime" -I"$root/vita" -c \
        "$root/runtime/host_vita_log_async.c" -o "$work/log-async.o"
    "$cc" $vita_flags -DISAAC_VITA_LOG_ASYNC_FILE_BATCH=1 -fstack-usage \
        -I"$root/runtime" -I"$root/vita" -c \
        "$root/runtime/host_vita_log_async.c" -o "$work/log-async-file-batch.o"
    symbols=$("$nm" "$work/log-async.o")
    echo "$symbols" | grep -q " T __wrap_abort\$" ||
        { echo "missing __wrap_abort" >&2; exit 1; }
    echo "$symbols" | grep -q " U __real_abort\$" ||
        { echo "missing __real_abort reference" >&2; exit 1; }
    if echo "$symbols" | grep -Eq " U (malloc|calloc|realloc|free)\$"; then
        echo "log-async module references a raw libc allocator" >&2
        exit 1
    fi
    batch_symbols=$("$nm" "$work/log-async-file-batch.o")
    echo "$batch_symbols" | grep -q ' U isaac_vita_log_sink_file_batch$'
    if echo "$batch_symbols" | grep -Eq " U (malloc|calloc|realloc|free)\$"; then
        echo "file-batch module references a raw libc allocator" >&2
        exit 1
    fi
    echo "Vita cross-compile of host_vita_log_async.c: OK"
fi

sha256sum \
    "$root/runtime/host_vita_log_async.h" \
    "$root/runtime/host_vita_log_async.c" \
    "$root/runtime/host_vita_log_async_oracle.c" \
    "$root/runtime/host_vita_log_file_batch_private.h" \
    "$root/vita/test_log_async.sh" \
    "$work/log-async-host-oracle"
echo "Vita asynchronous logger host behavior: PASS"
