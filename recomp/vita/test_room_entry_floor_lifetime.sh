#!/usr/bin/env bash
set -euo pipefail

root=${ISAAC_FLOOR_LIFETIME_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_FLOOR_LIFETIME_TEST_OUT:-}" ]; then
    work=$ISAAC_FLOOR_LIFETIME_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-floor-lifetime.XXXXXX")
fi

if ! host_gcc=$(command -v "${HOST_CC:-gcc}"); then
    echo "HOST_CC must resolve to GCC" >&2
    exit 2
fi
if ! host_clang=$(command -v "${HOST_CLANG:-clang}"); then
    echo "HOST_CLANG must resolve to Clang" >&2
    exit 2
fi
if [ "$(realpath "$host_gcc")" = "$(realpath "$host_clang")" ]; then
    echo "HOST_CC and HOST_CLANG must be distinct compilers" >&2
    exit 2
fi
"$host_gcc" --version > "$work/gcc.version"
"$host_clang" --version > "$work/clang.version"
"$host_gcc" -dM -E -x c /dev/null > "$work/gcc.macros"
"$host_clang" -dM -E -x c /dev/null > "$work/clang.macros"
if ! grep -q '^#define __GNUC__ ' "$work/gcc.macros" ||
   grep -q '^#define __clang__ ' "$work/gcc.macros"; then
    echo "HOST_CC is not a genuine GCC preprocessor backend" >&2
    exit 2
fi
if ! grep -q '^#define __clang__ ' "$work/clang.macros"; then
    echo "HOST_CLANG is not a genuine Clang preprocessor backend" >&2
    exit 2
fi
gcc_backend=$(sed -n '1p' "$work/gcc.version")
clang_backend=$(sed -n '1p' "$work/clang.version")
printf 'Host compiler backends: GCC=%s; Clang=%s\n' \
    "$gcc_backend" "$clang_backend"

host_flags=(
    -std=gnu11 -O1 -g -Wall -Wextra -Werror
    -fno-omit-frame-pointer -ffunction-sections -fdata-sections
    -fsanitize=address,undefined
    -DISAAC_VITA_ROOM_ENTRY_SLAB_TESTING=1
    -I"$root/runtime"
)
hybrid_flags=(
    -DISAAC_VITA_ROOM_ENTRY_HYBRID=1
    -DISAAC_VITA_ROOM_ENTRY_EXTERNAL_TESTING=1
)

for compiler in "$host_gcc" "$host_clang"; do
    tag=$(basename "$compiler")
    "$compiler" "${host_flags[@]}" \
        "$root/runtime/host_vita_room_entry_slab.c" \
        "$root/runtime/host_vita_room_entry_floor_lifetime_oracle.c" \
        -o "$work/floor-lifetime-raw96-$tag"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$work/floor-lifetime-raw96-$tag"

    "$compiler" "${host_flags[@]}" "${hybrid_flags[@]}" \
        "$root/runtime/host_vita_room_entry_slab.c" \
        "$root/runtime/host_vita_room_entry_floor_lifetime_oracle.c" \
        -o "$work/floor-lifetime-hybrid192-$tag"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$work/floor-lifetime-hybrid192-$tag"
done

arm_status=SKIP
if [ -n "${VITASDK:-}" ] && \
   [ -x "$VITASDK/bin/arm-vita-eabi-gcc" ]; then
    cc="$VITASDK/bin/arm-vita-eabi-gcc"
    nm="$VITASDK/bin/arm-vita-eabi-nm"
    readelf="$VITASDK/bin/arm-vita-eabi-readelf"
    arm_flags=(
        -std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon
        -mfloat-abi=softfp -mthumb -ffunction-sections -fdata-sections
        -Wall -Wextra -Werror -I"$root/runtime"
    )

    "$cc" "${arm_flags[@]}" \
        -DISAAC_VITA_ROOM_ENTRY_SLAB_TESTING=1 \
        -c "$root/runtime/host_vita_room_entry_slab.c" \
        -o "$work/floor-lifetime-raw96.arm.o"
    "$cc" "${arm_flags[@]}" \
        -DISAAC_VITA_ROOM_ENTRY_SLAB_TESTING=1 \
        -DISAAC_VITA_ROOM_ENTRY_HYBRID=1 \
        -DISAAC_VITA_ROOM_ENTRY_EXTERNAL_TESTING=1 \
        -c "$root/runtime/host_vita_room_entry_slab.c" \
        -o "$work/floor-lifetime-hybrid192.arm.o"

    for object in "$work/floor-lifetime-raw96.arm.o" \
                  "$work/floor-lifetime-hybrid192.arm.o"; do
        "$readelf" -h "$object" > "$object.header"
        "$readelf" -A "$object" > "$object.attributes"
        grep -q 'Machine:[[:space:]]*ARM$' "$object.header"
        grep -q 'Version5 EABI' "$object.header"
        if grep -q 'Tag_ABI_VFP_args' "$object.attributes"; then
            echo "$object gained a hardfp VFP-args tag" >&2
            exit 1
        fi
        if "$nm" -u "$object" | grep -Eq \
             ' (__atomic|__sync|malloc$|calloc$|realloc$|free$|mspace_)'; then
            echo "$object gained a forbidden allocator/atomic import" >&2
            exit 1
        fi
    done

    cat > "$work/arm-main.c" <<'EOF'
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "host_vita_heap_overflow_mspace.h"
#include "host_vita_room_entry_slab.h"

static unsigned char page[ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES]
    __attribute__((aligned(8)));
static int page_live;

void isaac_vita_log(const char *format, ...) { (void)format; }
int isaac_vita_heap_overflow_mspace_snapshot_get(
    isaac_vita_heap_overflow_mspace_snapshot *snapshot)
{
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY;
    snapshot->base = (uintptr_t)page;
    snapshot->end = (uintptr_t)page + sizeof page;
    snapshot->live_count = page_live ? 1U : 0U;
    snapshot->internal_live_count = page_live ? 1U : 0U;
    snapshot->internal_requested_bytes = page_live ? sizeof page : 0U;
    return 1;
}
void *isaac_vita_heap_overflow_mspace_internal_page_malloc(void)
{
    if (page_live) return NULL;
    page_live = 1;
    return page;
}
int isaac_vita_heap_overflow_mspace_internal_page_free(void *pointer)
{
    if (pointer != page || !page_live) return 0;
    page_live = 0;
    return 1;
}
int main(void)
{
    isaac_vita_room_entry_slab_decision decision;
    isaac_vita_room_entry_slab_floor_lifetime_snapshot floor;

    if (!isaac_vita_room_entry_slab_floor_lifetime_bootstrap_locked(
            ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_LEVEL_INIT)) return 1;
    if (isaac_vita_room_entry_slab_malloc_locked(
            ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA,
            ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES, &decision, NULL) !=
        ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED) return 2;
    if (!isaac_vita_room_entry_slab_floor_lifetime_snapshot_locked(&floor) ||
        floor.current_slots != 1U || floor.level_init_slots != 1U) return 3;
    if (!isaac_vita_room_entry_slab_floor_lifetime_rollover_locked()) return 4;
    if (!isaac_vita_room_entry_slab_free_locked(
            decision.pointer, ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA,
            NULL, &decision, NULL)) return 5;
    return !isaac_vita_room_entry_slab_test_reset_locked();
}
EOF
    "$cc" "${arm_flags[@]}" \
        -DISAAC_VITA_ROOM_ENTRY_SLAB_TESTING=1 \
        "$work/arm-main.c" "$work/floor-lifetime-raw96.arm.o" \
        -Wl,--gc-sections -o "$work/floor-lifetime-raw96.elf"
    "$readelf" -h "$work/floor-lifetime-raw96.elf" \
        > "$work/floor-lifetime-raw96.elf.header"
    grep -q 'Version5 EABI, soft-float ABI' \
        "$work/floor-lifetime-raw96.elf.header"
    arm_status=PASS
else
    echo "RoomConfig floor-lifetime ARM softfp gate: SKIP (VITASDK unavailable)"
fi

sha256sum \
    "$root/runtime/host_vita_room_entry_slab.h" \
    "$root/runtime/host_vita_room_entry_slab.c" \
    "$root/runtime/host_vita_room_entry_floor_lifetime_oracle.c" \
    "$root/vita/test_room_entry_floor_lifetime.sh"
echo "Vita RoomConfig floor-lifetime Raw96+Hybrid192 GCC/Clang sanitizer gate: PASS; ARM softfp: $arm_status"
