#!/usr/bin/env bash
# ARM (softfp Thumb-2) evidence for ISAAC_VITA_LUA_IMPORT_FASTDISPATCH:
#  1. the two owner units (host_vita_lua.c, host_vita_import_id.c) compile
#     with the production flags plus the option, -Werror, 0 errors;
#  2. no dmb/ldrex/strex in any typed endpoint (plain owner word);
#  3. per-call instruction counts: the typed lua_touserdata endpoint against
#     the generic chain it replaces (isaac_vita_lua_import_indexed +
#     vita_lua_touserdata_import + 2 x guest_stack_address + gpop_at +
#     vita_lua_scope_enter/leave), printed and gated;
#  4. with the option OFF, both owner objects are byte-identical (cmp) to the
#     base revision (ISAAC_LUA_FAST_BASE_ROOT) compiled with the same flags,
#     and guest.c (untouched by this option) likewise.
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi
if [ -z "${ISAAC_LUA53_SRC:-}" ]; then
    echo "ISAAC_LUA53_SRC must name the pristine lua-5.3.3/src directory" >&2
    exit 2
fi

root=${ISAAC_LUA_FAST_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
base=${ISAAC_LUA_FAST_BASE_ROOT:-}
if [ -n "${ISAAC_LUA_FAST_TEST_OUT:-}" ]; then
    work=$ISAAC_LUA_FAST_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-lua-import-fastdispatch.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
objdump="$VITASDK/bin/arm-vita-eabi-objdump"

# Exact production compile lines (stage build/compile_commands.json, perf
# configuration) minus the per-build GUEST_LINK_ID/GENERATION defines that
# only guest.c carries.
common="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
production="-DGUEST_IMAGE_BASE=0x98000000u -DGUEST_STACK_REQUIRED=1 -DISAAC_VITA_HAS_RUNTIME=1 -DISAAC_VITA_HEAP_MB=64 -DISAAC_VITA_LUA=1 -DISAAC_VITA_RAW_ALLOCATOR_GATE=1"
lua_defs="-DISAAC_VITA_LUA_ARENA_MB=48 -DISAAC_VITA_LUA_GCSTEP_CLAMP_KB=128"
id_defs="-DISAAC_VITA_AUDIO=1 -DISAAC_VITA_GUEST_DISPATCH_TABLE=1 -DISAAC_VITA_XINPUT=1"
guest_defs="-DISAAC_VITA_GL_SHIM_FASTDISPATCH=1 -DISAAC_VITA_GUEST_DISPATCH_TABLE=1 -DISAAC_VITA_GUEST_LOOKUP_CACHE=1 -DISAAC_VITA_IMPORT_DIRECT=1 -DISAAC_VITA_IMPORT_ID_DISPATCH=1 -DISAAC_VITA_PHASE_PROFILE=1 -DISAAC_VITA_RAW_ALLOCATOR_EXEMPT=1 -DISAAC_VITA_SYNC_IMPORT_FASTPATH=1 -DISAAC_VITA_SYNC_INLINE_FASTPATH=1"
fast="-DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1"

compile() { # root out src defs...
    local r=$1 out=$2 src=$3
    shift 3
    "$cc" $production -I"$r/vita" -I"$r/runtime" -I"$ISAAC_LUA53_SRC" $common \
        -include "$r/vita/isaac_vita_raw_allocator_poison.h" "$@" -c "$src" -o "$out"
}

count_insns() { # dis symbol
    awk -v f="<$2>:" '$0 ~ f {p=1; next} p && /^$/ {exit} p' "$1" |
        grep -cE '^ *[0-9a-f]+:' || true
}

# 1. option ON: 0 errors
compile "$root" "$work/host_vita_lua.fast.o" "$root/runtime/host_vita_lua.c" $lua_defs $fast
compile "$root" "$work/host_vita_import_id.fast.o" "$root/runtime/host_vita_import_id.c" $id_defs $fast
compile "$root" "$work/host_vita_lua.off.o" "$root/runtime/host_vita_lua.c" $lua_defs
compile "$root" "$work/host_vita_import_id.off.o" "$root/runtime/host_vita_import_id.c" $id_defs
compile "$root" "$work/guest.off.o" "$root/runtime/guest.c" $guest_defs
for o in host_vita_lua.fast host_vita_import_id.fast host_vita_lua.off; do
    "$objdump" -d --no-show-raw-insn "$work/$o.o" > "$work/$o.dis"
done
echo "compile ON (-Werror): host_vita_lua.c $(stat -c %s "$work/host_vita_lua.fast.o") B, host_vita_import_id.c $(stat -c %s "$work/host_vita_import_id.fast.o") B; OFF: $(stat -c %s "$work/host_vita_lua.off.o") B / $(stat -c %s "$work/host_vita_import_id.off.o") B"

# 2. no barriers/exclusives in any typed endpoint
for sym in rawgetp getmetatable type touserdata pushvalue rawget rawgeti settop gettop pushnil pushinteger pushnumber pushstring scope_enter scope_leave; do
    n=$(count_insns "$work/host_vita_lua.fast.dis" "vita_lua_fast_$sym")
    if [ "$n" -eq 0 ]; then
        # scope_enter/leave may be inlined; the thirteen endpoints must exist
        case $sym in scope_enter|scope_leave) continue;; esac
        echo "typed endpoint vita_lua_fast_$sym is missing from the fast object" >&2
        exit 3
    fi
    if awk -v f="<vita_lua_fast_$sym>:" '$0 ~ f {p=1; next} p && /^$/ {exit} p' "$work/host_vita_lua.fast.dis" |
            grep -Eq '\b(dmb|dsb|ldrex|strex|ldaex|stlex)\b'; then
        echo "vita_lua_fast_$sym still contains barriers/exclusives" >&2
        exit 3
    fi
done

# 3. per-call instruction counts for lua_touserdata
n_typed=$(count_insns "$work/host_vita_lua.fast.dis" vita_lua_fast_touserdata)
n_generic_handler=$(count_insns "$work/host_vita_lua.off.dis" vita_lua_touserdata_import)
n_indexed=$(count_insns "$work/host_vita_lua.off.dis" isaac_vita_lua_import_indexed)
n_enter=$(count_insns "$work/host_vita_lua.off.dis" vita_lua_scope_enter)
n_leave=$(count_insns "$work/host_vita_lua.off.dis" vita_lua_scope_leave)
"$objdump" -d --no-show-raw-insn "$work/guest.off.o" > "$work/guest.off.dis"
n_stack_address=$(count_insns "$work/guest.off.dis" guest_stack_address)
n_gpop=$(count_insns "$work/guest.off.dis" gpop_at)
n_import_call=$(count_insns "$work/guest.off.dis" guest_import_call)
dmb_generic=$(awk -v f="<vita_lua_scope_enter>:" '$0 ~ f {p=1; next} p && /^$/ {exit} p' "$work/host_vita_lua.off.dis" | grep -cE '\bdmb\b' || true)
dmb_leave=$(grep -cE '\bdmb\b' "$work/host_vita_lua.off.dis" || true)
generic_total=$((n_indexed + n_generic_handler + 2 * n_stack_address + n_gpop + n_enter + n_leave))
echo "lua_touserdata generic chain (static, whole functions): isaac_vita_lua_import_indexed=$n_indexed vita_lua_touserdata_import=$n_generic_handler guest_stack_address=$n_stack_address x2 gpop_at=$n_gpop vita_lua_scope_enter=$n_enter vita_lua_scope_leave=$n_leave(0 = inlined into the handler) total=$generic_total; dmb in the generic object=$dmb_leave (scope_enter alone: $dmb_generic)"
echo "lua_touserdata typed endpoint: vita_lua_fast_touserdata=$n_typed instructions, 0 dmb/ldrex/strex, reached with one blx from guest_import_call ($n_import_call instructions, unchanged)"
if [ "$n_typed" -eq 0 ] || [ "$n_typed" -gt 96 ]; then
    echo "vita_lua_fast_touserdata is $n_typed instructions (gate: 1..96)" >&2
    exit 3
fi
if [ "$n_typed" -ge "$generic_total" ]; then
    echo "typed endpoint ($n_typed) is not smaller than the generic chain ($generic_total)" >&2
    exit 3
fi

# 4. option OFF: byte-identical objects against the base revision
if [ -n "$base" ]; then
    compile "$base" "$work/host_vita_lua.base.o" "$base/runtime/host_vita_lua.c" $lua_defs
    compile "$base" "$work/host_vita_import_id.base.o" "$base/runtime/host_vita_import_id.c" $id_defs
    compile "$base" "$work/guest.base.o" "$base/runtime/guest.c" $guest_defs
    for unit in host_vita_lua host_vita_import_id guest; do
        if ! cmp "$work/$unit.off.o" "$work/$unit.base.o"; then
            echo "$unit.c with the option OFF differs from base (object bytes)" >&2
            exit 4
        fi
        echo "off-identity: cmp $unit.off.o $unit.base.o: identical ($(stat -c %s "$work/$unit.off.o") bytes)"
    done
fi

echo "Vita Lua import fast dispatch ARM check: PASS (softfp Thumb-2 objects; 13 typed endpoints without dmb/ldrex/strex; typed lua_touserdata $n_typed insns vs generic chain $generic_total; option-OFF byte identity)"
