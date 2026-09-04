#!/usr/bin/env bash
set -euo pipefail

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_ROOM_SLAB_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_ROOM_SLAB_TEST_OUT:-}" ]; then
    work=$ISAAC_ROOM_SLAB_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-room-slab.XXXXXX")
fi

frozen_pe_status=SKIP
if [ -n "${ISAAC_FROZEN_PE:-}" ] && \
   [ -n "${ISAAC_FROZEN_GENERATED_DIR:-}" ]; then
    python3 "$root/test_room_entry_frozen_contract.py" \
        --pe "$ISAAC_FROZEN_PE" \
        --generated-dir "$ISAAC_FROZEN_GENERATED_DIR"
    frozen_pe_status=PASS
elif [ -n "${ISAAC_FROZEN_PE:-}" ] || \
     [ -n "${ISAAC_FROZEN_GENERATED_DIR:-}" ]; then
    echo "ISAAC_FROZEN_PE and ISAAC_FROZEN_GENERATED_DIR must be set together" >&2
    exit 2
else
    echo "RoomConfig Entry frozen PE/generated contract: SKIP (paired env vars unset)"
fi

corpus_status=SKIP
if [ -n "${ISAAC_REPENTANCE_ARCHIVE:-}" ]; then
    python3 "$root/test_room_entry_corpus.py" "$ISAAC_REPENTANCE_ARCHIVE"
    corpus_status=PASS
else
    echo "RoomConfig Entry corpus receipt: SKIP (ISAAC_REPENTANCE_ARCHIVE unset)"
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

mul_flag_status=emitter
mul_flag_args=()
recomp_python=${ISAAC_RECOMP_PYTHON:-python3}
if ! "$recomp_python" -c 'import capstone' >/dev/null 2>&1; then
    echo "ISAAC_RECOMP_PYTHON must provide the generator's capstone module" >&2
    exit 2
fi
if [ "$frozen_pe_status" = PASS ]; then
    mul_flag_args=(--generated-dir "$ISAAC_FROZEN_GENERATED_DIR")
    mul_flag_status=fresh-generated
fi
for compiler in "$host_gcc" "$host_clang"; do
    "$recomp_python" "$root/test_mul_flag_semantics.py" \
        --cc "$compiler" "${mul_flag_args[@]}"
done

host_flags=(
    -std=gnu11 -O1 -g -Wall -Wextra -Werror
    -fno-omit-frame-pointer -ffunction-sections -fdata-sections
    -fsanitize=address,undefined
    -DISAAC_VITA_ROOM_ENTRY_SLAB_TESTING=1
    -I"$root/runtime"
)
for compiler in "$host_gcc" "$host_clang"; do
    tag=$(basename "$compiler")
    "$compiler" "${host_flags[@]}" \
        "$root/runtime/host_vita_room_entry_slab.c" \
        "$root/runtime/host_vita_room_entry_slab_oracle.c" \
        -o "$work/room-entry-slab-$tag"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$work/room-entry-slab-$tag"
done

cc="$VITASDK/bin/arm-vita-eabi-gcc"
nm="$VITASDK/bin/arm-vita-eabi-nm"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
arm_flags=(
    -std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb
    -ffunction-sections -fdata-sections -Wall -Wextra -Werror
)

"$cc" "${arm_flags[@]}" -DISAAC_VITA_ROOM_ENTRY_SLAB_TESTING=1 \
    -I"$root/runtime" -c "$root/runtime/host_vita_room_entry_slab.c" \
    -o "$work/room-entry-slab.arm.o"

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
    isaac_vita_heap_overflow_mspace_snapshot *s)
{
    memset(s, 0, sizeof *s);
    s->state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY;
    s->base = (uintptr_t)page;
    s->end = (uintptr_t)page + sizeof page;
    s->live_count = page_live ? 1U : 0U;
    s->internal_live_count = page_live ? 1U : 0U;
    s->internal_requested_bytes = page_live ? sizeof page : 0U;
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
    isaac_vita_room_entry_slab_decision d;
    isaac_vita_room_entry_slab_event e;
    void *p;
    isaac_vita_room_entry_slab_event_init(&e);
    if (isaac_vita_room_entry_slab_malloc_locked(
            ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U, &d, &e) !=
        ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED) return 1;
    p = d.pointer;
    if (!isaac_vita_room_entry_slab_owns_exact_locked(p)) return 2;
    if (isaac_vita_room_entry_slab_free_locked(
            p, ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA, NULL,
            &d, &e) !=
        ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED) return 3;
    return !isaac_vita_room_entry_slab_test_reset_locked();
}
EOF

"$cc" "${arm_flags[@]}" -DISAAC_VITA_ROOM_ENTRY_SLAB_TESTING=1 \
    -I"$root/runtime" "$work/arm-main.c" \
    "$work/room-entry-slab.arm.o" -Wl,--gc-sections \
    -o "$work/room-entry-slab.elf"

"$readelf" -h "$work/room-entry-slab.elf" > "$work/elf.header"
"$readelf" -A "$work/room-entry-slab.elf" > "$work/elf.attributes"
grep -q 'Machine:[[:space:]]*ARM$' "$work/elf.header"
grep -q 'Version5 EABI, soft-float ABI' "$work/elf.header"
if grep -q 'Tag_ABI_VFP_args' "$work/elf.attributes"; then
    echo "room-entry slab ARM link gained a hardfp VFP-args tag" >&2
    exit 1
fi
"$nm" -u "$work/room-entry-slab.arm.o" | \
    awk '$1 == "U" { print $2 }' | LC_ALL=C sort \
    > "$work/undefined.names"
printf '%s\n' \
    isaac_vita_heap_overflow_mspace_internal_page_free \
    isaac_vita_heap_overflow_mspace_internal_page_malloc \
    isaac_vita_heap_overflow_mspace_snapshot_get \
    isaac_vita_log memmove memset | LC_ALL=C sort > "$work/expected.names"
if ! cmp -s "$work/expected.names" "$work/undefined.names"; then
    diff -u "$work/expected.names" "$work/undefined.names" >&2 || true
    exit 1
fi
if "$nm" "$work/room-entry-slab.elf" | \
     grep -Eq ' U (__atomic|__sync|malloc$|calloc$|realloc$|free$|mspace_)'; then
    echo "room-entry slab ARM link gained a forbidden allocator/helper" >&2
    exit 1
fi

sha256sum \
    "$root/runtime/host_vita_room_entry_slab.h" \
    "$root/runtime/host_vita_room_entry_slab.c" \
    "$root/runtime/host_vita_room_entry_slab_oracle.c" \
    "$root/runtime/guest.h" \
    "$root/emit.py" \
    "$root/test_mul_flag_semantics.py" \
    "$root/test_room_entry_frozen_contract.py" \
    "$root/test_room_entry_corpus.py" \
    "$root/vita/test_room_entry_slab.sh" \
    "$work/room-entry-slab.arm.o" "$work/room-entry-slab.elf"
echo "Vita RoomConfig Entry slab GCC/Clang sanitizer + ARM softfp gate: PASS; frozen PE: $frozen_pe_status; generated MUL/IMUL: $mul_flag_status; corpus: $corpus_status"
