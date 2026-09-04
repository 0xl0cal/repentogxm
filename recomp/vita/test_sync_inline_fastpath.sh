#!/usr/bin/env bash
# Host differential oracle and ARM code-shape gates for
# ISAAC_VITA_SYNC_INLINE_FASTPATH (host_vita_sync_fastpath.h).
#
#   1. vita_sync_inline_fastpath_oracle.c against the production
#      host_vita_sync.c + vita_sync_services.c: seeded random replay of
#      Enter/Leave/TryEnter/Delete/Init/corruption programs, old route vs new
#      route from identical snapshots, pthread contender, thread latch;
#      -O2, ASan/UBSan and (when clang exists) TSan.
#   2. guest.c / host_vita_sync.c cross-compiled with the production flags:
#      option OFF must be -Werror clean; option ON must be -Werror clean and
#      guest_try_direct_sync_import_call must contain no dmb and no call other
#      than the registered endpoint and guest_fault.
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
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-sync-inline.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
objdump="$VITASDK/bin/arm-vita-eabi-objdump"
host_cc=${HOST_CC:-cc}
host_tsan_cc=${HOST_TSAN_CC:-clang}
steps=${ISAAC_SYNC_INLINE_STEPS:-200000}

oracle_sources="$root/runtime/vita_sync_services.c $root/runtime/host_vita_sync.c $root/runtime/vita_sync_inline_fastpath_oracle.c"
oracle_flags="-std=gnu11 -Wall -Wextra -Werror -pthread -no-pie -idirafter $VITASDK/arm-vita-eabi/include -I$root/runtime -DGUEST_IMAGE_BASE=0x98000000u -DGUEST_STACK_REQUIRED=1 -DISAAC_VITA_SYNC_INLINE_FASTPATH=1"
pass_prefix="Vita sync inline fast path oracle: PASS"

# shellcheck disable=SC2086
"$host_cc" -O2 $oracle_flags $oracle_sources -o "$work/inline-oracle"
"$work/inline-oracle" "$steps" | tee "$work/inline-oracle.out"
grep -q "^$pass_prefix" "$work/inline-oracle.out"
"$work/inline-oracle" "$steps" 0x1234567 | grep -q "^$pass_prefix"
"$work/inline-oracle" "$steps" 0xdeadbeef | grep -q "^$pass_prefix"

# Guest memory is byte-addressed x86 state: a misaligned ESP is a legal
# replay input, so alignment is the one UBSan check that does not apply.
# shellcheck disable=SC2086
"$host_cc" -O1 -g -fsanitize=address,undefined -fno-sanitize=alignment \
    -fno-omit-frame-pointer $oracle_flags $oracle_sources \
    -o "$work/inline-oracle-asan"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    "$work/inline-oracle-asan" 40000 | grep -q "^$pass_prefix"

if command -v "$host_tsan_cc" >/dev/null 2>&1; then
    # shellcheck disable=SC2086
    "$host_tsan_cc" -O1 -g -fsanitize=thread -fno-omit-frame-pointer \
        $oracle_flags $oracle_sources -o "$work/inline-oracle-tsan"
    TSAN_OPTIONS=halt_on_error=1 "$work/inline-oracle-tsan" 20000 \
        | grep -q "^$pass_prefix"
else
    echo "note: $host_tsan_cc not found, ThreadSanitizer pass skipped" >&2
fi

# Production compile shape of the two owners (CMakeLists.txt: guest.c and
# host_vita_sync.c are the only sources that receive the definition).
#
# The DEFINES default to the perf CMakeCache shape (DIRECT_DEFAULT, LUA,
# GUEST_LOOKUP_CACHE, IMPORT_ID_DISPATCH, PHASE_PROFILE, SYNC_IMPORT_FASTPATH
# for guest.c; the plain runtime set for host_vita_sync.c).  Point
# ISAAC_SYNC_BUILD_NINJA at the build.ninja of the build being certified to
# take each source's DEFINES line from it instead (any inline-fast-path
# definition is stripped so OFF/ON below stay meaningful); the -include and
# -I paths always come from $root so the sources under test are the ones
# compiled.
arm_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
common_defs="-DGUEST_IMAGE_BASE=0x98000000u -DGUEST_STACK_REQUIRED=1 -DISAAC_VITA_HAS_RUNTIME=1 -DISAAC_VITA_HEAP_MB=64 -DISAAC_VITA_LUA=1 -DISAAC_VITA_RAW_ALLOCATOR_GATE=1"
guest_defs="$common_defs -DGUEST_BUILD_OPT=\"O2\" -DGUEST_GENERATION_SYMBOL=guest_generation_test -DGUEST_LINK_ID=\"test\" -DISAAC_VITA_GUEST_LOOKUP_CACHE=1 -DISAAC_VITA_IMPORT_ID_DISPATCH=1 -DISAAC_VITA_PHASE_PROFILE=1 -DISAAC_VITA_RAW_ALLOCATOR_EXEMPT=1 -DISAAC_VITA_SYNC_IMPORT_FASTPATH=1"
sync_defs="$common_defs"

ninja_defines() {
    # $1 = build.ninja, $2 = source basename: the DEFINES line of its object.
    awk -v src="/$2.obj:" '
        index($0, "build ") == 1 && index($0, src) { grab = 1; next }
        grab && $1 == "DEFINES" { sub(/^[ \t]*DEFINES = /, ""); print; exit }
        grab && /^build / { exit }' "$1" \
        | sed -e 's/ *-DISAAC_VITA_SYNC_INLINE_FASTPATH=[^ ]*//g' \
              -e 's/\\"/"/g'
}
if [ -n "${ISAAC_SYNC_BUILD_NINJA:-}" ]; then
    guest_defs=$(ninja_defines "$ISAAC_SYNC_BUILD_NINJA" guest.c)
    sync_defs=$(ninja_defines "$ISAAC_SYNC_BUILD_NINJA" host_vita_sync.c)
    if [ -z "$guest_defs" ] || [ -z "$sync_defs" ]; then
        echo "could not read DEFINES from $ISAAC_SYNC_BUILD_NINJA" >&2
        exit 2
    fi
    case " $guest_defs " in
    *" -DISAAC_VITA_SYNC_IMPORT_FASTPATH=1 "*) ;;
    *)  echo "certified build does not enable ISAAC_VITA_SYNC_IMPORT_FASTPATH" >&2
        exit 2 ;;
    esac
    echo "guest.c DEFINES (build.ninja): $guest_defs"
    echo "host_vita_sync.c DEFINES (build.ninja): $sync_defs"
fi

compile_owner() {
    # $1 = recomp root, $2 = source basename, $3 = defs, $4 = extra, $5 = out
    # shellcheck disable=SC2086
    "$cc" $arm_flags -include "$1/vita/isaac_vita_raw_allocator_poison.h" \
        $3 $4 "-I$1/vita" "-I$1/runtime" -c "$1/runtime/$2.c" -o "$5"
}
disassemble() {
    "$objdump" -d --no-show-raw-insn "$1" | sed -e '/file format/d'
}
function_body() {
    # $1 = object, $2 = function: its instructions without addresses.
    "$objdump" -d --no-show-raw-insn "$1" \
        | awk -v f="<$2>:" 'index($0, f) {p=1; next} p && /^$/ {exit} p' \
        | sed -e 's/^ *[0-9a-f]*://'
}
count_insns() {
    grep -c . "$1" || true
}

compile_owner "$root" guest "$guest_defs" "" "$work/guest-off.o"
compile_owner "$root" guest "$guest_defs" \
    -DISAAC_VITA_SYNC_INLINE_FASTPATH=1 "$work/guest-on.o"
compile_owner "$root" host_vita_sync "$sync_defs" "" "$work/host_vita_sync-off.o"
compile_owner "$root" host_vita_sync "$sync_defs" \
    -DISAAC_VITA_SYNC_INLINE_FASTPATH=1 "$work/host_vita_sync-on.o"

# Option OFF must be the base branch's code, instruction for instruction,
# when a baseline recomp tree is supplied (ISAAC_SYNC_BASELINE_ROOT).
if [ -n "${ISAAC_SYNC_BASELINE_ROOT:-}" ]; then
    compile_owner "$ISAAC_SYNC_BASELINE_ROOT" guest "$guest_defs" "" \
        "$work/guest-base.o"
    compile_owner "$ISAAC_SYNC_BASELINE_ROOT" host_vita_sync "$sync_defs" "" \
        "$work/host_vita_sync-base.o"
    for source in guest host_vita_sync; do
        if ! cmp -s <(disassemble "$work/$source-off.o") \
                    <(disassemble "$work/$source-base.o"); then
            echo "$source.c with the option OFF differs from the baseline" >&2
            exit 1
        fi
        echo "$source.c OFF == baseline: IDENTICAL ($(disassemble "$work/$source-off.o" | grep -c '^ *[0-9a-f]*:') insns)"
    done
fi

# The generic dispatcher must not change shape: the inline bodies live only
# in guest_try_direct_sync_import_call.
function_body "$work/guest-off.o" guest_call > "$work/guest_call-off.dis"
function_body "$work/guest-on.o" guest_call > "$work/guest_call-on.dis"
if ! cmp -s "$work/guest_call-off.dis" "$work/guest_call-on.dis"; then
    echo "guest_call changed shape with the inline fast path ON" >&2
    diff "$work/guest_call-off.dis" "$work/guest_call-on.dis" >&2 || true
    exit 1
fi
echo "guest_call ON == OFF: IDENTICAL ($(count_insns "$work/guest_call-on.dis") insns)"

function_body "$work/guest-on.o" guest_try_direct_sync_import_call \
    > "$work/direct-sync-on.dis"
insns=$(count_insns "$work/direct-sync-on.dis")
dmb=$(grep -c 'dmb' "$work/direct-sync-on.dis" || true)
calls=$(grep -E '\<(bl|blx)\>' "$work/direct-sync-on.dis" \
    | sed 's/.*<//; s/>.*//' | grep -v '^guest_try_direct_sync_import_call' \
    | sort -u | tr '\n' ' ')
echo "guest_try_direct_sync_import_call(ON): insns=$insns dmb=$dmb calls=[$calls]"
if [ "$dmb" != "0" ]; then
    echo "inline fast path must not emit a barrier" >&2
    exit 1
fi
for callee in $calls; do
    case "$callee" in
    isaac_vita_sync_import_indexed|guest_fault) ;;
    *)  echo "inline fast path calls out of line: $callee" >&2; exit 1 ;;
    esac
done
if [ "$insns" -gt 200 ]; then
    echo "inline fast path grew past its code-size budget: $insns" >&2
    exit 1
fi
if "$objdump" -d "$work/guest-on.o" | grep -q '<isaac_vita_sync_inline_'; then
    echo "inline helpers were not inlined into guest.c" >&2
    exit 1
fi
function_body "$work/host_vita_sync-off.o" vita_sync_cs_validate > "$work/validate-off.dis"
function_body "$work/host_vita_sync-on.o" vita_sync_cs_validate > "$work/validate-on.dis"
validate_off=$(count_insns "$work/validate-off.dis")
validate_on=$(count_insns "$work/validate-on.dis")
echo "vita_sync_cs_validate: OFF=$validate_off ON=$validate_on insns (latch note); guest.c text: OFF=$(disassemble "$work/guest-off.o" | grep -c '^ *[0-9a-f]*:') ON=$(disassemble "$work/guest-on.o" | grep -c '^ *[0-9a-f]*:') insns"

echo "Vita sync inline fast path check: PASS (differential replay x3 seeds, ASan/UBSan, TSan when available; ARM guest.c/host_vita_sync.c -Werror ON/OFF; guest_call identical ON/OFF; direct sync call: $insns insns, 0 dmb, endpoint/guest_fault the only calls)"
