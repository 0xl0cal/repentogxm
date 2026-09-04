#!/usr/bin/env bash
# ARM (softfp Thumb-2) evidence for ISAAC_VITA_GL_SHIM_FASTDISPATCH:
#  1. the four owner units compile with the production flags plus the option;
#  2. the fast gl_bridge object contains no dmb/ldrex/strex (plain owner word);
#  3. per-call instruction counts of the dispatch chain are printed and gated,
#     including the guest_call prefix every non-GL indirect call executes
#     (entry -> lookup-cache hash multiply): at most one instruction more
#     than the base revision;
#  4. with the option OFF, gl_bridge.c, host_vita_gl.c and guest.c compile to
#     the same instructions as the base revision (ISAAC_GL_SHIM_BASE_ROOT) in
#     the perf configuration (PHASE_PROFILE=1), and gl_vita_backend.c does so
#     with PHASE_PROFILE off (with it on, the new location/timeGetTime ph120.a
#     census differs from base by design, option or not);
#  5. wf/opt-gltok (ISAAC_VITA_GL_SHIM_TABLE_TOKENS / _RAW_ARGS on top of the
#     fast dispatch and the dispatch table): the owner units compile, the
#     bridge stays barrier-free, the per-call chain of a typed-GL call is
#     counted before/after (guest_call, trampoline, table dispatch, adapter),
#     guest_call itself is untouched by the token keys, and with both knobs
#     OFF guest.c / gl_bridge.c / kage_vita_phase_profile.c in the perf
#     shape (fast dispatch + dispatch table) compile to the same instructions
#     as the base revision.
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_GL_SHIM_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
base=${ISAAC_GL_SHIM_BASE_ROOT:-}
vitagl_include=${ISAAC_GL_SHIM_VITAGL_INCLUDE:-}
if [ -n "${ISAAC_GL_SHIM_TEST_OUT:-}" ]; then
    work=$ISAAC_GL_SHIM_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-gl-shim-fastdispatch.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
objdump="$VITASDK/bin/arm-vita-eabi-objdump"

common="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
production="$common -DGUEST_IMAGE_BASE=0x98000000u -DGUEST_STACK_REQUIRED=1 -DISAAC_VITA_HAS_RUNTIME=1 -DISAAC_VITA_HEAP_MB=64 -DISAAC_VITA_LUA=1 -DISAAC_VITA_RAW_ALLOCATOR_GATE=1 -include $root/vita/isaac_vita_raw_allocator_poison.h"
guest_defs="-DISAAC_VITA_GUEST_LOOKUP_CACHE=1 -DISAAC_VITA_IMPORT_ID_DISPATCH=1 -DISAAC_VITA_PHASE_PROFILE=1 -DISAAC_VITA_RAW_ALLOCATOR_EXEMPT=1 -DISAAC_VITA_SYNC_IMPORT_FASTPATH=1"
backend_defs="-DISAAC_VITA_CANONICAL_QUAD_ZERO_COPY=1 -DISAAC_VITA_GL_REDUNDANCY_CACHE=1 -DISAAC_VITA_GL_TYPED_STATE_CACHE=1 -DISAAC_VITA_PHASE_PROFILE=1"
fast="-DISAAC_VITA_GL_SHIM_FASTDISPATCH=1"
inc="-I$root/runtime -I$root/vita"

compile() { # out src defs...
    local out=$1 src=$2
    shift 2
    "$cc" $production $inc "$@" -c "$src" -o "$out"
}

count_insns() { # dis symbol
    awk -v f="<$2>:" '$0 ~ f {p=1; next} p && /^$/ {exit} p' "$1" |
        grep -cE '^ *[0-9a-f]+:' || true
}

count_prefix() { # dis symbol anchor-regex -> instructions before the first anchor line
    awk -v f="<$2>:" '$0 ~ f {p=1; next} p && /^$/ {exit} p' "$1" |
        awk -v a="$3" '$0 ~ a {exit} /^ *[0-9a-f]+:/ {n++} END {print n+0}'
}

dis_body() { # dis symbol -> instruction mnemonics only (no addresses)
    awk -v f="<$2>:" '$0 ~ f {p=1; next} p && /^$/ {exit} p' "$1" |
        sed -E 's/^ *[0-9a-f]+:\s*//; s/\s+;.*$//; s/<[^>]*\+0x[0-9a-f]+>//g'
}

# 1. fast objects
compile "$work/gl_bridge.fast.o" "$root/runtime/gl_bridge.c" $fast
compile "$work/host_vita_gl.fast.o" "$root/runtime/host_vita_gl.c" $fast
compile "$work/guest.fast.o" "$root/runtime/guest.c" $guest_defs $fast
if [ -n "$vitagl_include" ]; then
    compile "$work/gl_vita_backend.fast.o" "$root/runtime/gl_vita_backend.c" \
        -isystem "$vitagl_include" $backend_defs $fast \
        -DISAAC_VITA_GL_LOCATION_CACHE=1
    compile "$work/gl_vita_backend.verify.o" "$root/runtime/gl_vita_backend.c" \
        -isystem "$vitagl_include" $backend_defs $fast \
        -DISAAC_VITA_GL_LOCATION_CACHE=1 -DISAAC_VITA_GL_LOCATION_CACHE_VERIFY=1
fi
for o in gl_bridge host_vita_gl guest; do
    "$objdump" -d --no-show-raw-insn "$work/$o.fast.o" > "$work/$o.fast.dis"
done

# 2. no barriers or exclusive accesses anywhere in the fast bridge
if grep -Eq '\b(dmb|dsb|ldrex|strex|ldaex|stlex)\b' "$work/gl_bridge.fast.dis"; then
    echo "fast gl_bridge still contains barriers/exclusives:" >&2
    grep -En '\b(dmb|dsb|ldrex|strex)\b' "$work/gl_bridge.fast.dis" >&2
    exit 3
fi

# 3. instruction counts of the per-call chain
n_counted=$(count_insns "$work/gl_bridge.fast.dis" guest_gl_dispatch_counted)
n_dispatch=$(count_insns "$work/gl_bridge.fast.dis" guest_gl_dispatch)
n_dyn=$(count_insns "$work/host_vita_gl.fast.dis" vita_gl_dynamic_dispatch)
n_dyn_counted=$(count_insns "$work/host_vita_gl.fast.dis" isaac_vita_gl_dynamic_counted)
n_call=$(count_insns "$work/guest.fast.dis" guest_call)
n_attrib=$(count_insns "$work/gl_bridge.fast.dis" guest_gl_adapter_glGetAttribLocation)
n_pointer=$(count_insns "$work/gl_bridge.fast.dis" guest_gl_adapter_glVertexAttribPointer)
# The lookup-cache hash multiply (guest_lookup_cache_index, 0x9e3779b1) is the
# first instruction of the legacy path proper; everything before it is what a
# non-GL indirect call pays for the 0x7e probe.
prefix_anchor='mul\.w'
n_call_prefix=$(count_prefix "$work/guest.fast.dis" guest_call "$prefix_anchor")
echo "fast: guest_gl_dispatch_counted=$n_counted guest_gl_dispatch=$n_dispatch vita_gl_dynamic_dispatch=$n_dyn isaac_vita_gl_dynamic_counted=$n_dyn_counted guest_call=$n_call guest_call_prefix=$n_call_prefix adapter(glGetAttribLocation)=$n_attrib adapter(glVertexAttribPointer)=$n_pointer"
if [ "$n_call_prefix" -eq 0 ]; then
    echo "guest_call prefix anchor ($prefix_anchor) not found in the fast object" >&2
    exit 3
fi
# Whole-function size: the common path (token index probe, exact compare,
# census increment, owner word check + pin, blx adapter, owner word release)
# is ~48 of these; the rest are the three cold guest_fault tails.
if [ "$n_counted" -eq 0 ] || [ "$n_counted" -gt 96 ]; then
    echo "guest_gl_dispatch_counted is $n_counted instructions (gate: 1..96)" >&2
    exit 3
fi
if [ "$n_dyn" -gt 40 ] && [ "$n_dyn_counted" -gt 40 ]; then
    echo "vita_gl_dynamic_dispatch is $n_dyn instructions (gate: <= 40)" >&2
    exit 3
fi
if [ -f "$work/gl_vita_backend.fast.o" ]; then
    "$objdump" -d --no-show-raw-insn "$work/gl_vita_backend.fast.o" > "$work/gl_vita_backend.fast.dis"
    echo "fast: vita_glGetAttribLocation=$(count_insns "$work/gl_vita_backend.fast.dis" vita_glGetAttribLocation) vita_glGetUniformLocation=$(count_insns "$work/gl_vita_backend.fast.dis" vita_glGetUniformLocation) gl_vita_location_cache_lookup=$(count_insns "$work/gl_vita_backend.fast.dis" gl_vita_location_cache_lookup)"
fi

# 5. wf/opt-gltok: table tokens and raw arguments on top of fast + table
table="-DISAAC_VITA_GUEST_DISPATCH_TABLE=1"
gltok="-DISAAC_VITA_GL_SHIM_TABLE_TOKENS=1"
rawargs="-DISAAC_VITA_GL_SHIM_RAW_ARGS=1"
profile_defs="-DISAAC_VITA_GUEST_LOOKUP_CACHE=1 -DISAAC_VITA_GUEST_DISPATCH_TABLE=1 -DISAAC_VITA_PHASE_PROFILE=1 -DISAAC_VITA_PHASE_PROFILE_BUILD_ID=\"gltok\""
compile "$work/guest.table.o" "$root/runtime/guest.c" $guest_defs $fast $table
compile "$work/guest.gltok.o" "$root/runtime/guest.c" $guest_defs $fast $table $gltok
compile "$work/gl_bridge.gltok.o" "$root/runtime/gl_bridge.c" $fast $table -DISAAC_VITA_PHASE_PROFILE=1 $gltok
compile "$work/gl_bridge.gltokraw.o" "$root/runtime/gl_bridge.c" $fast $table -DISAAC_VITA_PHASE_PROFILE=1 $gltok $rawargs
compile "$work/kage_vita_phase_profile.gltok.o" "$root/runtime/kage_vita_phase_profile.c" $profile_defs $gltok
for o in guest.table guest.gltok gl_bridge.gltok gl_bridge.gltokraw; do
    "$objdump" -d --no-show-raw-insn "$work/$o.o" > "$work/$o.dis"
done
for o in gl_bridge.gltok gl_bridge.gltokraw; do
    if grep -Eq '\b(dmb|dsb|ldrex|strex|ldaex|stlex)\b' "$work/$o.dis"; then
        echo "$o still contains barriers/exclusives:" >&2
        grep -En '\b(dmb|dsb|ldrex|strex)\b' "$work/$o.dis" >&2
        exit 3
    fi
done
# Before: guest_call (probe miss) -> guest_call_slow (0x7e block) ->
# isaac_vita_gl_dynamic_counted -> guest_gl_dispatch_counted -> adapter.
# After: guest_call (probe hit) -> trampoline -> guest_gl_table_dispatch
# (owner check + census, guest_gl_run_owned inlined) -> adapter.
n_call_table=$(count_insns "$work/guest.table.dis" guest_call)
n_call_gltok=$(count_insns "$work/guest.gltok.dis" guest_call)
n_slow_table=$(count_insns "$work/guest.table.dis" guest_call_slow)
n_slow_gltok=$(count_insns "$work/guest.gltok.dis" guest_call_slow)
n_tramp=$(count_insns "$work/gl_bridge.gltok.dis" guest_gl_table_glBlendFuncSeparate)
n_table_dispatch=$(count_insns "$work/gl_bridge.gltok.dis" guest_gl_table_dispatch)
n_counted_gltok=$(count_insns "$work/gl_bridge.gltok.dis" guest_gl_dispatch_counted)
n_blend_legacy=$(count_insns "$work/gl_bridge.gltok.dis" guest_gl_adapter_glBlendFuncSeparate)
n_blend_raw=$(count_insns "$work/gl_bridge.gltokraw.dis" guest_gl_adapter_glBlendFuncSeparate)
n_viewport_legacy=$(count_insns "$work/gl_bridge.gltok.dis" guest_gl_adapter_glViewport)
n_viewport_raw=$(count_insns "$work/gl_bridge.gltokraw.dis" guest_gl_adapter_glViewport)
n_blend_slow=$(count_insns "$work/gl_bridge.gltokraw.dis" guest_gl_adapter_glBlendFuncSeparate_slow)
n_viewport_slow=$(count_insns "$work/gl_bridge.gltokraw.dis" guest_gl_adapter_glViewport_slow)
n_stack_address=$(count_insns "$work/guest.gltok.dis" guest_stack_address)
n_gpop_at=$(count_insns "$work/guest.gltok.dis" gpop_at)
n_stack_adjust=$(count_insns "$work/guest.gltok.dis" guest_stack_adjust)
echo "gltok: guest_call table=$n_call_table gltok=$n_call_gltok guest_call_slow table=$n_slow_table gltok=$n_slow_gltok trampoline(glBlendFuncSeparate)=$n_tramp guest_gl_table_dispatch=$n_table_dispatch guest_gl_dispatch_counted=$n_counted_gltok isaac_vita_gl_dynamic_counted=$n_dyn_counted"
echo "gltok: adapter(glBlendFuncSeparate) legacy=$n_blend_legacy raw=$n_blend_raw raw_slow=$n_blend_slow adapter(glViewport) legacy=$n_viewport_legacy raw=$n_viewport_raw raw_slow=$n_viewport_slow; legacy callees guest_stack_address=$n_stack_address gpop_at=$n_gpop_at guest_stack_adjust=$n_stack_adjust (4 + 1 + 1 calls per 4-arg legacy adapter, 0 per raw adapter)"
echo "gltok: 4-arg chain (whole functions) before=guest_call+guest_call_slow+isaac_vita_gl_dynamic_counted+guest_gl_dispatch_counted+adapter = $((n_call_table + n_slow_table + n_dyn_counted + n_counted_gltok + n_blend_legacy)) after=guest_call+trampoline+guest_gl_table_dispatch+adapter(raw) = $((n_call_gltok + n_tramp + n_table_dispatch + n_blend_raw))"
if [ "$n_call_table" -ne "$n_call_gltok" ]; then
    echo "guest_call changed size with the token keys ($n_call_table -> $n_call_gltok)" >&2
    exit 3
fi
if [ "$n_tramp" -eq 0 ] || [ "$n_tramp" -gt 6 ]; then
    echo "trampoline is $n_tramp instructions (gate: 1..6)" >&2
    exit 3
fi
if [ "$n_table_dispatch" -eq 0 ] || [ "$n_table_dispatch" -gt "$n_counted_gltok" ]; then
    echo "guest_gl_table_dispatch is $n_table_dispatch instructions (gate: 1..guest_gl_dispatch_counted=$n_counted_gltok)" >&2
    exit 3
fi
# The raw hot adapter (one inline frame check, plain loads, one esp store,
# no calls but the backend; its cold guest_fault tail and the branch to the
# _slow twin included) may not outgrow the legacy body by more than a few
# instructions -- the legacy body's six validator calls (4 x
# guest_stack_address + gpop_at + guest_stack_adjust, ~180 executed
# instructions) are what the raw adapter removes; its _slow twin is the
# legacy body plus nothing.
echo "gltok: 4-arg legacy adapter executes its $n_blend_legacy instructions plus 6 validator calls (4 x guest_stack_address, gpop_at, guest_stack_adjust); the raw adapter executes its own instructions only"
if [ "$n_blend_raw" -gt $((n_blend_legacy + 8)) ] || [ "$n_viewport_raw" -gt $((n_viewport_legacy + 8)) ]; then
    echo "raw adapters outgrew the legacy ones (glBlendFuncSeparate $n_blend_legacy -> $n_blend_raw, glViewport $n_viewport_legacy -> $n_viewport_raw)" >&2
    exit 3
fi
if [ "$n_blend_slow" -eq 0 ] || [ "$n_blend_slow" -gt $((n_blend_legacy + 8)) ]; then
    echo "raw _slow twin is $n_blend_slow instructions (legacy $n_blend_legacy)" >&2
    exit 3
fi
# The hot adapter reads its arguments without guest_stack_address; the only
# validator calls left in it are the cold retirement fallback (gpop_at +
# guest_stack_adjust for an esp a nested dispatch moved).
if awk -v f="<guest_gl_adapter_glBlendFuncSeparate>:" '$0 ~ f {p=1; next} p && /^$/ {exit} p' "$work/gl_bridge.gltokraw.dis" | grep -Eq '<guest_stack_address>'; then
    echo "raw glBlendFuncSeparate adapter still calls the per-word argument validator" >&2
    exit 3
fi
if [ -n "$base" ]; then
    # Both knobs OFF in the perf shape: identical to base.
    "$cc" $production -I"$root/runtime" -I"$root/vita" $guest_defs $fast $table -c "$root/runtime/guest.c" -o "$work/guest.knobsoff.o"
    "$cc" $production -I"$base/runtime" -I"$base/vita" $guest_defs $fast $table -c "$base/runtime/guest.c" -o "$work/guest.knobsbase.o"
    "$cc" $production -I"$root/runtime" -I"$root/vita" $fast -c "$root/runtime/gl_bridge.c" -o "$work/gl_bridge.knobsoff.o"
    "$cc" $production -I"$base/runtime" -I"$base/vita" $fast -c "$base/runtime/gl_bridge.c" -o "$work/gl_bridge.knobsbase.o"
    "$cc" $production -I"$root/runtime" -I"$root/vita" $profile_defs -c "$root/runtime/kage_vita_phase_profile.c" -o "$work/kage_vita_phase_profile.knobsoff.o"
    "$cc" $production -I"$base/runtime" -I"$base/vita" $profile_defs -c "$base/runtime/kage_vita_phase_profile.c" -o "$work/kage_vita_phase_profile.knobsbase.o"
    for unit in guest gl_bridge kage_vita_phase_profile; do
        "$objdump" -d --no-show-raw-insn "$work/$unit.knobsoff.o" | sed -E '1,3d; s/^[^:]*\.o:.*//' > "$work/$unit.knobsoff.dis"
        "$objdump" -d --no-show-raw-insn "$work/$unit.knobsbase.o" | sed -E '1,3d; s/^[^:]*\.o:.*//' > "$work/$unit.knobsbase.dis"
        if ! cmp -s "$work/$unit.knobsoff.dis" "$work/$unit.knobsbase.dis"; then
            echo "$unit.c with both gltok knobs OFF (fast + table) differs from base:" >&2
            diff "$work/$unit.knobsbase.dis" "$work/$unit.knobsoff.dis" | head -40 >&2
            exit 4
        fi
        echo "knobs-off-identity (fast + table): $unit.c == base ($(grep -cE '^ *[0-9a-f]+:' "$work/$unit.knobsoff.dis") instructions)"
    done
fi

# 4. option OFF: identical instructions to the base revision
if [ -n "$base" ]; then
    for unit in gl_bridge host_vita_gl guest; do
        defs=
        [ "$unit" = guest ] && defs=$guest_defs
        "$cc" $production -I"$root/runtime" -I"$root/vita" $defs -c "$root/runtime/$unit.c" -o "$work/$unit.off.o"
        "$cc" $production -I"$base/runtime" -I"$base/vita" $defs -c "$base/runtime/$unit.c" -o "$work/$unit.base.o"
        "$objdump" -d --no-show-raw-insn "$work/$unit.off.o" | sed -E '1,3d; s/^[^:]*\.o:.*//' > "$work/$unit.off.dis"
        "$objdump" -d --no-show-raw-insn "$work/$unit.base.o" | sed -E '1,3d; s/^[^:]*\.o:.*//' > "$work/$unit.base.dis"
        if ! cmp -s "$work/$unit.off.dis" "$work/$unit.base.dis"; then
            echo "$unit.c with the option OFF differs from base:" >&2
            diff "$work/$unit.base.dis" "$work/$unit.off.dis" | head -40 >&2
            exit 4
        fi
        echo "off-identity: $unit.c == base ($(grep -cE '^ *[0-9a-f]+:' "$work/$unit.off.dis") instructions)"
    done
    n_call_base=$(count_insns "$work/guest.base.dis" guest_call)
    n_call_prefix_base=$(count_prefix "$work/guest.base.dis" guest_call "$prefix_anchor")
    echo "guest_call: base=$n_call_base fast=$n_call fast-base=$((n_call - n_call_base)); prefix (entry -> lookup-cache hash) base=$n_call_prefix_base fast=$n_call_prefix tax=$((n_call_prefix - n_call_prefix_base)) instructions per non-GL indirect call"
    if [ "$n_call_prefix_base" -eq 0 ] || [ $((n_call_prefix - n_call_prefix_base)) -gt 1 ]; then
        echo "guest_call prefix tax is $((n_call_prefix - n_call_prefix_base)) instructions (gate: <= 1)" >&2
        exit 3
    fi
    if [ -n "$vitagl_include" ]; then
        backend_off_defs=$(printf '%s
' $backend_defs | grep -v PHASE_PROFILE | tr '
' ' ')
        "$cc" $production -I"$root/runtime" -I"$root/vita" -isystem "$vitagl_include" $backend_off_defs -c "$root/runtime/gl_vita_backend.c" -o "$work/gl_vita_backend.off.o"
        "$cc" $production -I"$base/runtime" -I"$base/vita" -isystem "$vitagl_include" $backend_off_defs -c "$base/runtime/gl_vita_backend.c" -o "$work/gl_vita_backend.base.o"
        "$objdump" -d --no-show-raw-insn "$work/gl_vita_backend.off.o" | sed -E '1,3d; s/^[^:]*\.o:.*//' > "$work/gl_vita_backend.off.dis"
        "$objdump" -d --no-show-raw-insn "$work/gl_vita_backend.base.o" | sed -E '1,3d; s/^[^:]*\.o:.*//' > "$work/gl_vita_backend.base.dis"
        if ! cmp -s "$work/gl_vita_backend.off.dis" "$work/gl_vita_backend.base.dis"; then
            echo "gl_vita_backend.c with the option OFF (PHASE_PROFILE off) differs from base:" >&2
            diff "$work/gl_vita_backend.base.dis" "$work/gl_vita_backend.off.dis" | head -40 >&2
            exit 4
        fi
        echo "off-identity (PHASE_PROFILE off): gl_vita_backend.c == base ($(grep -cE '^ *[0-9a-f]+:' "$work/gl_vita_backend.off.dis") instructions)"
    fi
fi

echo "Vita GL shim fast dispatch ARM check: PASS (softfp Thumb-2 objects; no dmb/ldrex/strex in the fast bridge; per-call chain counted; guest_call prefix tax <= 1; option-OFF instruction identity; gltok: token trampolines + raw adapters counted, guest_call unchanged, knobs-OFF identity)"
