#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_SYNC_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_SYNC_TEST_OUT:-}" ]; then
    work=$ISAAC_SYNC_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-sync-imports.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
host_cc=${HOST_CC:-cc}
host_tsan_cc=${HOST_TSAN_CC:-clang}

# Execute the production UID-free embedded lock backend on the build host
# against strict Vita service mocks.  `-idirafter` exposes Vita declarations
# without replacing the host C library headers with newlib's target headers.
"$host_cc" -std=gnu11 -O2 -Wall -Wextra -Werror -pthread \
    -idirafter "$VITASDK/arm-vita-eabi/include" \
    -I"$root/runtime" \
    "$root/runtime/vita_sync_services.c" \
    "$root/runtime/vita_sync_embedded_host_oracle.c" \
    -o "$work/vita-sync-embedded-host-oracle"
"$work/vita-sync-embedded-host-oracle"

"$host_tsan_cc" -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread \
    -fno-omit-frame-pointer -fsanitize=thread \
    -idirafter "$VITASDK/arm-vita-eabi/include" \
    -I"$root/runtime" \
    "$root/runtime/vita_sync_services.c" \
    "$root/runtime/vita_sync_embedded_host_oracle.c" \
    -o "$work/vita-sync-embedded-host-oracle-tsan"
TSAN_OPTIONS=halt_on_error=1 \
    "$work/vita-sync-embedded-host-oracle-tsan"

"$host_cc" -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread \
    -fno-omit-frame-pointer -fsanitize=address,undefined \
    -idirafter "$VITASDK/arm-vita-eabi/include" \
    -I"$root/runtime" \
    "$root/runtime/vita_sync_services.c" \
    "$root/runtime/vita_sync_embedded_host_oracle.c" \
    -o "$work/vita-sync-embedded-host-oracle-asan"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
    "$work/vita-sync-embedded-host-oracle-asan"

# GCC 10's established false positive sees the ARM inline-asm guarded value in
# guest.h as maybe-uninitialized.  Keep every other warning fatal.
common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-maybe-uninitialized -Disaac_vita_exception_import_counted=isaac_vita_sync_test_skip_exception -Disaac_vita_file_lock_import_counted=isaac_vita_sync_test_skip_file_lock -Disaac_vita_filesystem_import_counted=isaac_vita_sync_test_skip_later_import -Disaac_vita_math_import_counted=isaac_vita_sync_test_skip_later_import -Disaac_vita_find_import_counted=isaac_vita_sync_test_skip_later_import -Disaac_vita_com_import_counted=isaac_vita_sync_test_skip_later_import -Disaac_vita_post_com_import_counted=isaac_vita_sync_test_skip_later_import -Disaac_vita_heap_import_counted=isaac_vita_sync_test_skip_heap -Disaac_vita_steam_import_counted=isaac_vita_sync_test_skip_steam -Disaac_vita_gl_import_counted=isaac_vita_sync_test_skip_later_import -Disaac_vita_gl_dynamic_counted=isaac_vita_sync_test_skip_gl_dynamic -Disaac_vita_rtti_import_counted=isaac_vita_sync_test_skip_later_import -Disaac_vita_user32_import_counted=isaac_vita_sync_test_skip_later_import"

# The embedded backend may use thread identity and scheduler delay, but must
# never regress to a process UID/LwMutex allocator or pull libatomic helpers.
"$cc" $common_flags -I"$root/runtime" -c \
    "$root/runtime/vita_sync_services.c" \
    -o "$work/vita-sync-services.o"
"$nm" -u "$work/vita-sync-services.o" \
    > "$work/vita-sync-services.undefined"
if grep -Eq 'sceKernel.*(Lw)?Mutex|__atomic_' \
        "$work/vita-sync-services.undefined"; then
    cat "$work/vita-sync-services.undefined" >&2
    echo "embedded sync backend references a forbidden mutex/libatomic API" >&2
    exit 1
fi
grep -q 'sceKernelGetThreadId' "$work/vita-sync-services.undefined"
grep -q 'sceKernelDelayThread' "$work/vita-sync-services.undefined"

"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_first_fault.c" \
    "$root/runtime/host_vita_console.c" \
    "$root/runtime/host_vita_crt.c" \
    "$root/runtime/host_vita_fls.c" \
    "$root/runtime/host_vita_sync.c" \
    "$root/runtime/host_vita_memory.c" \
    "$root/runtime/host_vita_startup.c" \
    "$root/runtime/vita_sync_services.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/vita_sync_import_test.c" \
    -o "$work/vita-sync-import-oracle.elf"

"$readelf" -h "$work/vita-sync-import-oracle.elf" \
    > "$work/vita-sync-import-oracle.header"
"$readelf" -A "$work/vita-sync-import-oracle.elf" \
    > "$work/vita-sync-import-oracle.attributes"
"$nm" -u "$work/vita-sync-import-oracle.elf" \
    > "$work/vita-sync-import-oracle.undefined"
cat "$work/vita-sync-import-oracle.header"
cat "$work/vita-sync-import-oracle.attributes"
if ! grep -q "soft-float ABI" "$work/vita-sync-import-oracle.header"; then
    echo "sync oracle is not marked soft-float ABI" >&2
    exit 1
fi
if grep -q "Tag_ABI_VFP_args" "$work/vita-sync-import-oracle.attributes"; then
    echo "sync oracle unexpectedly advertises hardfp argument passing" >&2
    exit 1
fi
if [ -s "$work/vita-sync-import-oracle.undefined" ]; then
    cat "$work/vita-sync-import-oracle.undefined" >&2
    echo "sync oracle has unresolved symbols" >&2
    exit 1
fi
sha256sum \
    "$root/runtime/host_vita_first_fault.c" \
    "$root/runtime/host_vita_console.h" \
    "$root/runtime/host_vita_console.c" \
    "$root/runtime/host_vita_crt.h" \
    "$root/runtime/host_vita_crt.c" \
    "$root/runtime/host_vita_fls.h" \
    "$root/runtime/host_vita_fls.c" \
    "$root/runtime/host_vita_sync.h" \
    "$root/runtime/host_vita_sync.c" \
    "$root/runtime/host_vita_memory.h" \
    "$root/runtime/host_vita_memory.c" \
    "$root/runtime/host_vita_startup.h" \
    "$root/runtime/host_vita_startup.c" \
    "$root/runtime/vita_sync_services.h" \
    "$root/runtime/vita_sync_services.c" \
    "$root/runtime/vita_sync_embedded_host_oracle.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/vita_sync_import_test.c" \
    "$root/vita/test_sync_imports.sh" \
    "$work/vita-sync-import-oracle.elf"
echo "Vita integrated boot compile/link/static artifact check: PASS (14 sync handlers; recursive UID-free embedded backend host-executed; owned CreateEventW/CloseHandle exit closure; ARM source oracle linked; runtime evidence requires hardware)"
