#!/usr/bin/env bash
# ISAAC_VITA_GUEST_STACK_MEMBLOCK: host oracles of the dedicated USER_RW guest
# stack memblock (mandatory, sanitizers, no VitaSDK needed): the unit's
# transaction oracle, the stack owners of guest.c compiled ON and executed
# against a fake sysmem, and entry_vita.c OFF vs ON receipt pins; when
# VITASDK is set, the ARM allow-list / raw-allocator-row gate for the unit,
# guest.c OFF vs ON and the entry_vita.c receipts with the Vita toolchain.
set -eu

root=${ISAAC_GUEST_STACK_MEMBLOCK_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_GUEST_STACK_MEMBLOCK_TEST_OUT:-}" ]; then
    work=$ISAAC_GUEST_STACK_MEMBLOCK_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-guest-stack-memblock.XXXXXX")
fi

host_cc=${HOST_CC:-cc}
fake_include="$root/vita/anm2_scratch_oracle_include"
host_flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fsanitize=address,undefined"

"$host_cc" $host_flags -I"$fake_include" -I"$root/runtime" \
    "$root/runtime/guest_stack_memblock.c" \
    "$root/runtime/guest_stack_memblock_oracle.c" \
    -o "$work/guest-stack-memblock-oracle"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$work/guest-stack-memblock-oracle" | tee "$work/oracle.log"
grep -q '^guest stack memblock oracle: PASS$' "$work/oracle.log"

# Link-probe mode keeps the fake sysmem and drops main, so another owner can
# link the unit against it; the probe must run one READY transaction.
cat > "$work/link-probe-main.c" <<'EOF'
#include "guest_stack_memblock.h"

int main(void)
{
    guest_stack_memblock_snapshot snapshot;
    int rc = guest_stack_memblock_acquire(
        GUEST_STACK_MEMBLOCK_REQUEST_BYTES - GUEST_STACK_MEMBLOCK_PAGE_BYTES,
        &snapshot);

    (void)guest_stack_memblock_status_text(snapshot.status);
    if (guest_stack_memblock_release(&snapshot) != 0)
        return 2;
    return rc != 1;
}
EOF
"$host_cc" $host_flags -DGUEST_STACK_MEMBLOCK_LINK_PROBE=1 \
    -I"$fake_include" -I"$root/runtime" \
    "$root/runtime/guest_stack_memblock.c" \
    "$root/runtime/guest_stack_memblock_oracle.c" \
    "$work/link-probe-main.c" \
    -o "$work/guest-stack-memblock-link-probe"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$work/guest-stack-memblock-link-probe"

# The production unit carries no allocator or memset token: the -include
# poison header applies to it and the raw allocator gate rejects any raw edge.
if grep -nE '\b(malloc|calloc|realloc|free|memalign|aligned_alloc|strdup|memset)[[:space:]]*\(' \
        "$root/runtime/guest_stack_memblock.c"; then
    echo "guest_stack_memblock.c uses an allocator or memset token" >&2
    exit 1
fi

# Owner integration on the host (mandatory): guest.c compiled ON with
# -D__vita__ against stub psp2 headers (processmgr/threadmgr/clib empty: the
# stack owners use no symbol of them; sysmem from the anm2 oracle include),
# linked strictly with the memblock unit and the integration oracle, whose
# fake sysmem owns one 0x401000 slot below 4 GiB.  guest.c gets -w because
# the stubs leave two functions of unrelated paths implicitly declared;
# --gc-sections drops those paths, so the strict link proves the stack owners
# resolve to nothing but the unit.  The oracle executes acquire -> margins ->
# memset of exactly GUEST_STACK_SIZE -> bind, both init release sites, free
# (uid -1 / none, second free silent) and the fail-closed alloc and get-base
# failures, and pins floor = base + 4 KiB, ceiling = base + 4 MiB - 4 KiB.
# The production sampler (oracle mode, no thread) is linked in as well: the
# oracle binds it to the stack-owned CPU, scans the live slot, stops it the
# way the entry tail does before guest_stack_free and pins that no load
# follows the stop or the unmap, and that the free publishes the unbinding
# (floor/ceiling/esp zero) before sceKernelFreeMemBlock.
stub_include="$work/stub-psp2"
mkdir -p "$stub_include/psp2/kernel"
for header in processmgr threadmgr clib; do
    printf '/* host stub: the guest.c stack owners use no symbol of this header */\n' \
        > "$stub_include/psp2/kernel/$header.h"
done
cp "$fake_include/psp2/kernel/sysmem.h" "$stub_include/psp2/kernel/sysmem.h"
owner_flags="-std=gnu11 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -ffunction-sections -fdata-sections -fno-strict-aliasing -D__vita__ -DGUEST_STACK_REQUIRED=1 -DGUEST_IMAGE_BASE=0x98000000u"
"$host_cc" $owner_flags -DISAAC_VITA_GUEST_STACK_MEMBLOCK=1 -w \
    -I"$stub_include" -I"$root/runtime" \
    -c "$root/runtime/guest.c" -o "$work/guest.on.host.o"
"$host_cc" $owner_flags -Wall -Wextra -Werror \
    -I"$stub_include" -I"$root/runtime" \
    -c "$root/runtime/guest_stack_memblock.c" \
    -o "$work/guest-stack-memblock.host.o"
"$host_cc" $owner_flags -Wall -Wextra -Werror -DISAAC_VITA_GUEST_SAMPLER=1 \
    -I"$stub_include" -I"$root/runtime" \
    -c "$root/runtime/guest_stack_memblock_integration_oracle.c" \
    -o "$work/guest-stack-memblock-integration-oracle.o"
"$host_cc" $owner_flags -Wall -Wextra -Werror \
    -DISAAC_KAGE_VITA_GUEST_SAMPLER_ORACLE=1 -DISAAC_VITA_GUEST_SAMPLER=1 \
    -I"$stub_include" -I"$root/runtime" \
    -c "$root/runtime/kage_vita_guest_sampler.c" \
    -o "$work/kage-vita-guest-sampler.oracle.host.o"
"$host_cc" $owner_flags \
    "$work/guest-stack-memblock-integration-oracle.o" \
    "$work/guest.on.host.o" \
    "$work/guest-stack-memblock.host.o" \
    "$work/kage-vita-guest-sampler.oracle.host.o" \
    -Wl,--gc-sections -o "$work/guest-stack-memblock-integration"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$work/guest-stack-memblock-integration" | tee "$work/integration.log"
grep -q '^guest stack memblock integration oracle: PASS$' "$work/integration.log"
grep -q '^ON free: unbinding published before sceKernelFreeMemBlock (reader guard dead at the unmap)$' \
    "$work/integration.log"
grep -q '^ON log: KAGE VITA GUEST SAMPLER: stopped ticks=1 busy_spins=0$' \
    "$work/integration.log"
grep -q '^ON sampler: 256 words read while bound, 0 after kage_vita_guest_sampler_stop, 0 after guest_stack_free$' \
    "$work/integration.log"
grep -Eq '^ON init: floor=[0-9a-f]{8} ceiling=[0-9a-f]{8} capacity=4186112 uid=0x00004242 base=0x[0-9a-f]{8} request=4198400$' \
    "$work/integration.log"
grep -q '^ON alloc failure: status=alloc-failed uid=0xfffffeef get=0x00000000 base=0x00000000 rollback=0x00000000$' \
    "$work/integration.log"
grep -q '^ON get failure: status=get-base-failed uid=0x00004242 get=0xfffffdef base=0x00000000 rollback=0x00000000$' \
    "$work/integration.log"
grep -q '^ON bind failure: released by guest_stack_init, status=none$' \
    "$work/integration.log"

# entry_vita.c receipts: the ON object carries both "guest stack reserve"
# literals, exactly two more isaac_vita_log call relocations than OFF (failure
# receipt + ready receipt), two guest_stack_memblock_snapshot_get calls and one
# guest_stack_memblock_status_text call; OFF has none of them.  Run on the host
# objects here and on the ARM objects below.
ready_receipt='guest stack reserve: status=ready path=memblock uid=0x%08x base=0x%08x request=%u vita3k_retained=%u floor=%08x ceiling=%08x capacity=%u'
failure_receipt='guest stack reserve: status=%s path=memblock uid=0x%08x get=0x%08x base=0x%08x rollback=0x%08x'
symbol_relocations () {
    # readelf object symbol -> number of relocations against that symbol
    "$1" -Wr "$2" | awk -v symbol="$3" '$5 == symbol' | wc -l | tr -d ' '
}
check_entry_receipts () {
    # label readelf strings off_object on_object
    local label=$1 readelf_tool=$2 strings_tool=$3 off=$4 on=$5
    local off_logs on_logs
    off_logs=$(symbol_relocations "$readelf_tool" "$off" isaac_vita_log)
    on_logs=$(symbol_relocations "$readelf_tool" "$on" isaac_vita_log)
    if [ "$on_logs" -ne $((off_logs + 2)) ]; then
        echo "$label: entry_vita.c ON has $on_logs isaac_vita_log relocations, OFF $off_logs (expected OFF + 2)" >&2
        exit 1
    fi
    if [ "$(symbol_relocations "$readelf_tool" "$on" guest_stack_memblock_snapshot_get)" -ne 2 ] ||
       [ "$(symbol_relocations "$readelf_tool" "$on" guest_stack_memblock_status_text)" -ne 1 ] ||
       [ "$(symbol_relocations "$readelf_tool" "$off" guest_stack_memblock_snapshot_get)" -ne 0 ] ||
       [ "$(symbol_relocations "$readelf_tool" "$off" guest_stack_memblock_status_text)" -ne 0 ]; then
        echo "$label: entry_vita.c memblock call counts drifted (ON wants 2 snapshot_get + 1 status_text, OFF none)" >&2
        exit 1
    fi
    "$strings_tool" -a "$on" > "$on.strings"
    if ! grep -Fxq "$ready_receipt" "$on.strings"; then
        echo "$label: entry_vita.c ON lost the ready receipt literal" >&2
        exit 1
    fi
    if ! grep -Fxq "$failure_receipt" "$on.strings"; then
        echo "$label: entry_vita.c ON lost the failure receipt literal" >&2
        exit 1
    fi
    if "$strings_tool" -a "$off" | grep -Fq 'guest stack reserve'; then
        echo "$label: entry_vita.c OFF carries a guest stack reserve literal" >&2
        exit 1
    fi
    echo "$label: entry_vita.c receipts OK (isaac_vita_log relocations $off_logs -> $on_logs)"
}
# Teardown order of the entry tail: the cross-core readers of the guest stack
# are unbound before guest_stack_free.  Pinned on relocations (readelf -Wr,
# "section offset symbol" rows; code order under -ffunction-sections): every
# relocation against the stop symbol precedes the single guest_stack_free
# relocation of the same section.  Objects without the reader's knob carry
# no stop relocation at all (the default tail is unchanged).
relocation_rows () {
    "$1" -Wr "$2" | awk '
        /^Relocation section/ { section = $3; next }
        $1 ~ /^[0-9a-fA-F]+$/ && NF >= 5 { print section, tolower($1), $5 }'
}
check_entry_stop_order () {
    # label readelf object stop_symbol
    local label=$1 readelf_tool=$2 object=$3 stop=$4
    local rows free_rows free_section free_offset stop_rows
    rows=$(relocation_rows "$readelf_tool" "$object")
    free_rows=$(printf '%s\n' "$rows" | awk '$3 == "guest_stack_free"')
    if [ "$(printf '%s\n' "$free_rows" | grep -c .)" -ne 1 ]; then
        echo "$label: entry_vita.c does not call guest_stack_free exactly once" >&2
        exit 1
    fi
    free_section=$(printf '%s\n' "$free_rows" | awk '{ print $1 }')
    free_offset=$(printf '%s\n' "$free_rows" | awk '{ print $2 }')
    stop_rows=$(printf '%s\n' "$rows" | awk -v symbol="$stop" '$3 == symbol')
    if [ -z "$stop_rows" ]; then
        echo "$label: entry_vita.c has no $stop call" >&2
        exit 1
    fi
    # fixed-width lower-case hex offsets: string order == numeric order
    if printf '%s\n' "$stop_rows" | awk -v section="$free_section" -v offset="$free_offset" \
            '$1 != section || ($2 "") >= (offset "") { bad = 1 } END { exit bad ? 0 : 1 }'; then
        echo "$label: a $stop call does not precede guest_stack_free" >&2
        exit 1
    fi
    echo "$label: $stop precedes guest_stack_free ($(printf '%s\n' "$stop_rows" | grep -c .) call site(s))"
}
check_entry_no_stops () {
    # label readelf object: neither stop symbol is referenced
    local label=$1 readelf_tool=$2 object=$3
    if [ "$(symbol_relocations "$readelf_tool" "$object" kage_vita_guest_sampler_stop)" -ne 0 ] ||
       [ "$(symbol_relocations "$readelf_tool" "$object" isaac_vita_lua_beat_stop)" -ne 0 ]; then
        echo "$label: entry_vita.c without the reader knobs references a stop" >&2
        exit 1
    fi
}
entry_host_flags="-std=gnu11 -O2 -ffunction-sections -fdata-sections -fno-strict-aliasing -w -D__vita__ -DGUEST_STACK_REQUIRED=1 -DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_KAGE=1 -DISAAC_VITA_AUDIO=0"
entry_sampler_flags="-DISAAC_VITA_GUEST_STACK_MEMBLOCK=1 -DISAAC_VITA_GUEST_SAMPLER=1"
entry_beat_flags="-DISAAC_VITA_GUEST_STACK_MEMBLOCK=1 -DISAAC_VITA_LUA=1 -DISAAC_VITA_LUA_RECEIPT=1"
"$host_cc" $entry_host_flags -I"$stub_include" -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/entry_vita.c" -o "$work/entry_vita.off.host.o"
"$host_cc" $entry_host_flags -DISAAC_VITA_GUEST_STACK_MEMBLOCK=1 \
    -I"$stub_include" -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/entry_vita.c" -o "$work/entry_vita.on.host.o"
"$host_cc" $entry_host_flags $entry_sampler_flags \
    -I"$stub_include" -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/entry_vita.c" -o "$work/entry_vita.on-sampler.host.o"
"$host_cc" $entry_host_flags $entry_beat_flags \
    -I"$stub_include" -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/entry_vita.c" -o "$work/entry_vita.on-beat.host.o"
check_entry_receipts host readelf strings \
    "$work/entry_vita.off.host.o" "$work/entry_vita.on.host.o"
check_entry_no_stops host readelf "$work/entry_vita.off.host.o"
check_entry_no_stops host readelf "$work/entry_vita.on.host.o"
check_entry_stop_order host readelf "$work/entry_vita.on-sampler.host.o" kage_vita_guest_sampler_stop
check_entry_stop_order host readelf "$work/entry_vita.on-beat.host.o" isaac_vita_lua_beat_stop

if [ -z "${VITASDK:-}" ]; then
    sha256sum \
        "$root/runtime/guest_stack_memblock.h" \
        "$root/runtime/guest_stack_memblock.c" \
        "$root/runtime/guest_stack_memblock_oracle.c" \
        "$root/runtime/guest_stack_memblock_integration_oracle.c" \
        "$root/runtime/kage_vita_guest_sampler.c" \
        "$root/vita/test_guest_stack_memblock.sh"
    echo "guest stack memblock host gate: PASS (ARM part skipped: VITASDK unset)"
    exit 0
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
nm="$VITASDK/bin/arm-vita-eabi-nm"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
strings="$VITASDK/bin/arm-vita-eabi-strings"
arm_flags="-std=gnu11 -O2 -DGUEST_STACK_REQUIRED=1 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"

undefined_names () {
    "$nm" -u "$1" | awk '$1 == "U" { print $2 }' | LC_ALL=C sort
}

# Identical objects against the real and the fake sysmem header prove the
# oracle's pinned prototypes hide no ABI drift.
"$cc" $arm_flags -I"$root/runtime" \
    -c "$root/runtime/guest_stack_memblock.c" \
    -o "$work/guest-stack-memblock.real-header.o"
"$cc" $arm_flags -I"$fake_include" -I"$root/runtime" \
    -c "$root/runtime/guest_stack_memblock.c" \
    -o "$work/guest-stack-memblock.fake-header.o"
if ! cmp -s "$work/guest-stack-memblock.real-header.o" \
              "$work/guest-stack-memblock.fake-header.o"; then
    echo "fake sysmem header changed the guest stack memblock object" >&2
    exit 3
fi
undefined_names "$work/guest-stack-memblock.real-header.o" \
    > "$work/guest-stack-memblock.undefined.names"
printf '%s\n' sceKernelAllocMemBlock sceKernelFreeMemBlock \
    sceKernelGetMemBlockBase | LC_ALL=C sort \
    > "$work/guest-stack-memblock.expected.names"
if ! cmp -s "$work/guest-stack-memblock.expected.names" \
              "$work/guest-stack-memblock.undefined.names"; then
    echo "guest stack memblock unresolved-symbol allow-list changed" >&2
    diff -u "$work/guest-stack-memblock.expected.names" \
        "$work/guest-stack-memblock.undefined.names" >&2 || true
    exit 1
fi

# guest.c raw-allocator rows of the two stack owners: OFF keeps the three
# newlib edges the gate pins (EXPECTED_FIXED["guest.c"]), ON has none
# (GUEST_STACK_NEWLIB_RELOCATIONS is subtracted under the knob).
# GCC -O2 partitions a function's cold path into .text.unlikely.<fn> (the
# error-path free() of guest_stack_init lands there under -ffunction-sections),
# so the partition prefix is folded back into the owner's name the way the
# raw allocator gate does.
stack_rows () {
    "$readelf" -Wr "$1" | awk '
        /^Relocation section/ {
            section = $3; gsub(/\047/, "", section)
            sub(/^\.rel\.text\.(unlikely\.|hot\.|startup\.|exit\.)?/, "", section)
            next }
        (section == "guest_stack_init" || section == "guest_stack_free") &&
        ($5 == "memalign" || $5 == "free") { print section, $5, $3 }' |
        LC_ALL=C sort
}
"$cc" $arm_flags -I"$root/runtime" \
    -c "$root/runtime/guest.c" -o "$work/guest.off.o"
"$cc" $arm_flags -DISAAC_VITA_GUEST_STACK_MEMBLOCK=1 -I"$root/runtime" \
    -c "$root/runtime/guest.c" -o "$work/guest.on.o"
stack_rows "$work/guest.off.o" > "$work/guest.off.rows"
stack_rows "$work/guest.on.o" > "$work/guest.on.rows"
printf '%s\n' \
    "guest_stack_free free R_ARM_THM_CALL" \
    "guest_stack_init free R_ARM_THM_CALL" \
    "guest_stack_init memalign R_ARM_THM_CALL" | LC_ALL=C sort \
    > "$work/guest.off.expected"
if ! cmp -s "$work/guest.off.expected" "$work/guest.off.rows"; then
    echo "guest.c OFF stack rows drifted from the gate pins" >&2
    diff -u "$work/guest.off.expected" "$work/guest.off.rows" >&2 || true
    exit 1
fi
if [ -s "$work/guest.on.rows" ]; then
    echo "guest.c ON kept a newlib stack edge:" >&2
    cat "$work/guest.on.rows" >&2
    exit 1
fi
echo "guest.c OFF rows: $(tr '\n' ';' < "$work/guest.off.rows")"
echo "guest.c ON rows: (none)"

# ON link of the stack owner, the memblock unit and the exhaustive PC oracle
# (not executed): no unresolved memblock or kernel symbol may remain.
"$cc" $arm_flags -DISAAC_VITA_GUEST_STACK_MEMBLOCK=1 -I"$root/runtime" \
    "$root/runtime/guest.c" \
    "$root/runtime/guest_stack_memblock.c" \
    "$root/runtime/guest_stack_oracle.c" \
    -Wl,--gc-sections \
    -lSceLibKernel_stub -lSceSysmem_stub -lSceKernelThreadMgr_stub \
    -o "$work/vita-guest-stack-memblock-on.elf"
"$readelf" -h "$work/vita-guest-stack-memblock-on.elf" > "$work/on.header"
grep -q 'Machine:[[:space:]]*ARM$' "$work/on.header"
grep -q 'soft-float ABI' "$work/on.header"
undefined_names "$work/vita-guest-stack-memblock-on.elf" > "$work/on.undefined"
if grep -Eq 'guest_stack_memblock|sceKernel|^(malloc|free|memalign)$' \
        "$work/on.undefined"; then
    cat "$work/on.undefined" >&2
    echo "ON link left a memblock/kernel/allocator symbol unresolved" >&2
    exit 5
fi

# entry_vita.c OFF vs ON with the production flags of
# test_kage_vita_teardown.sh (KAGE on, audio off): the receipt literals and
# call relocation deltas pinned above, now on the Vita toolchain's objects.
entry_arm_flags="-DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_KAGE=1 -DISAAC_VITA_AUDIO=0"
"$cc" $arm_flags $entry_arm_flags -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/entry_vita.c" -o "$work/entry_vita.off.o"
"$cc" $arm_flags $entry_arm_flags -DISAAC_VITA_GUEST_STACK_MEMBLOCK=1 \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/entry_vita.c" -o "$work/entry_vita.on.o"
"$cc" $arm_flags $entry_arm_flags $entry_sampler_flags \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/entry_vita.c" -o "$work/entry_vita.on-sampler.o"
"$cc" $arm_flags $entry_arm_flags $entry_beat_flags \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/entry_vita.c" -o "$work/entry_vita.on-beat.o"
check_entry_receipts ARM "$readelf" "$strings" \
    "$work/entry_vita.off.o" "$work/entry_vita.on.o"
check_entry_no_stops ARM "$readelf" "$work/entry_vita.off.o"
check_entry_no_stops ARM "$readelf" "$work/entry_vita.on.o"
check_entry_stop_order ARM "$readelf" "$work/entry_vita.on-sampler.o" kage_vita_guest_sampler_stop
check_entry_stop_order ARM "$readelf" "$work/entry_vita.on-beat.o" isaac_vita_lua_beat_stop

sha256sum \
    "$root/runtime/guest_stack_memblock.h" \
    "$root/runtime/guest_stack_memblock.c" \
    "$root/runtime/guest_stack_memblock_oracle.c" \
    "$root/runtime/guest_stack_memblock_integration_oracle.c" \
    "$root/runtime/kage_vita_guest_sampler.c" \
    "$root/runtime/guest.c" \
    "$root/runtime/entry_vita.c" \
    "$root/vita/test_guest_stack_memblock.sh" \
    "$work/guest-stack-memblock.real-header.o" \
    "$work/guest.off.o" \
    "$work/guest.on.o" \
    "$work/entry_vita.off.o" \
    "$work/entry_vita.on.o" \
    "$work/entry_vita.on-sampler.o" \
    "$work/entry_vita.on-beat.o" \
    "$work/vita-guest-stack-memblock-on.elf"
echo "Vita guest stack memblock host oracles + ARM allow-list/row/receipt gate: PASS (ARM objects linked/compiled, not executed)"
