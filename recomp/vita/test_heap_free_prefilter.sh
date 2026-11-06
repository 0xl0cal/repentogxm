#!/usr/bin/env bash
# W-heap host gates (Linux host gcc; the ARM legs run when VITASDK is set):
#
#  1. ISAAC_VITA_HEAP_FREE_PREFILTER differential trace oracle
#     (runtime/host_vita_heap_free_prefilter_oracle.c): the real heap import
#     and the real OGG emergency / ANM2 scratch / texel scratch owners at the
#     measured device layout, prefilter OFF and ON, for the default, 8M and
#     32M texel capacities.  Every [phase] digest must be identical, owner
#     calls for pointers inside a block identical, and ON makes no owner call
#     for a pointer outside every block.
#  2. Mutations that must be caught (digest change or oracle failure).
#  3. OFF identity against the branch base: preprocessed token stream and
#     whole host object of every touched translation unit under the define
#     sets the existing host oracles use.  The define must change the ON
#     token stream (it reached the unit).
#  4. ARM (VITASDK): Thumb compile of heap + owners OFF/ON, the range record
#     symbols exist exactly with the knob, and the heap object references
#     them only with the knob.
set -eu

root=${ISAAC_PREFILTER_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_PREFILTER_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-heap-free-prefilter.XXXXXX")}
base_rev=${ISAAC_PREFILTER_BASE:-9a2b0992fbe1c1e27e6c0e218e62449a99b56b62}
host_cc=${HOST_CC:-gcc}
mkdir -p "$work"

oracle_include="$root/vita/anm2_scratch_oracle_include"
routing="-std=gnu11 -O2 -Wall -Wextra -Werror -Wno-maybe-uninitialized \
-DGUEST_IMAGE_BASE=0x98000000u -DGUEST_STACK_REQUIRED=1 \
-ffunction-sections -fdata-sections \
-I$root/runtime -I$root/vita -I$oracle_include"
heap_defs="-DISAAC_VITA_OGG_EMERGENCY=1 -DISAAC_VITA_ANM2_SCRATCH=1 \
-DISAAC_VITA_TEXEL_SCRATCH=1 -DISAAC_VITA_TEXEL_OOM_DIAGNOSTIC=1 \
-DISAAC_VITA_HEAP_RANGE_LEASE=1 -DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=1"
interpose="-Dmalloc=oracle_guest_malloc -Dfree=oracle_guest_free \
-Drealloc=oracle_guest_realloc"
wraps="-Wl,--wrap=isaac_vita_ogg_emergency_free \
-Wl,--wrap=isaac_vita_anm2_scratch_free \
-Wl,--wrap=isaac_vita_texel_scratch_free"
PF=-DISAAC_VITA_HEAP_FREE_PREFILTER=1
owner_oracles="-DISAAC_VITA_OGG_EMERGENCY_ORACLE=1 \
-DISAAC_VITA_ANM2_SCRATCH_ORACLE=1 -DISAAC_VITA_TEXEL_SCRATCH_ORACLE=1"

"$host_cc" $routing -c "$root/runtime/guest.c" -o "$work/guest.o"

# build_oracle DIR TEXEL_FLAG PF_FLAG [HEAP_SRC [OGG_SRC [ANM2_SRC [TEXEL_SRC]]]]
build_oracle() {
    dir=$1 texel=$2 pf=$3
    heap=${4:-$root/runtime/host_vita_heap.c}
    ogg=${5:-$root/runtime/host_vita_ogg_emergency.c}
    anm2=${6:-$root/runtime/host_vita_anm2_scratch.c}
    tex=${7:-$root/runtime/host_vita_texel_scratch.c}
    mkdir -p "$dir"
    # shellcheck disable=SC2086
    "$host_cc" $routing $heap_defs $interpose $pf -c "$heap" -o "$dir/heap.o"
    # shellcheck disable=SC2086
    "$host_cc" $routing -DISAAC_VITA_OGG_EMERGENCY_ORACLE=1 $pf \
        -c "$ogg" -o "$dir/ogg.o"
    # shellcheck disable=SC2086
    "$host_cc" $routing -DISAAC_VITA_ANM2_SCRATCH_ORACLE=1 $pf \
        -c "$anm2" -o "$dir/anm2.o"
    # shellcheck disable=SC2086
    "$host_cc" $routing -DISAAC_VITA_TEXEL_SCRATCH_ORACLE=1 $texel $pf \
        -c "$tex" -o "$dir/texel.o"
    # shellcheck disable=SC2086
    "$host_cc" $routing $owner_oracles $texel $pf \
        -c "$root/runtime/host_vita_heap_free_prefilter_oracle.c" \
        -o "$dir/oracle.o"
    # shellcheck disable=SC2086
    "$host_cc" -pthread "$dir/heap.o" "$work/guest.o" "$dir/ogg.o" \
        "$dir/anm2.o" "$dir/texel.o" "$dir/oracle.o" $wraps \
        -Wl,--gc-sections -o "$dir/oracle"
}

run_oracle() {
    if "$1/oracle" > "$1/out" 2>&1; then
        return 0
    fi
    return 1
}

# 1. Differential trace, three texel capacities.
for variant in default 8m 32m; do
    case $variant in
    default) texel_flag="" ;;
    8m) texel_flag="-DISAAC_VITA_TEXEL_SCRATCH_8M=1" ;;
    32m) texel_flag="-DISAAC_VITA_TEXEL_SCRATCH_32M=1" ;;
    esac
    build_oracle "$work/$variant-off" "$texel_flag" ""
    build_oracle "$work/$variant-on" "$texel_flag" "$PF"
    for side in off on; do
        if ! run_oracle "$work/$variant-$side"; then
            cat "$work/$variant-$side/out" >&2
            echo "prefilter oracle $variant-$side failed" >&2
            exit 1
        fi
    done
    grep '^\[phase\]' "$work/$variant-off/out" > "$work/$variant-off/phases"
    grep '^\[phase\]' "$work/$variant-on/out" > "$work/$variant-on/phases"
    [ "$(wc -l < "$work/$variant-off/phases")" -eq 6 ] ||
        { echo "expected 6 phases ($variant)" >&2; exit 1; }
    if ! cmp -s "$work/$variant-off/phases" "$work/$variant-on/phases"; then
        diff "$work/$variant-off/phases" "$work/$variant-on/phases" >&2 || true
        echo "prefilter changed a route/result ($variant)" >&2
        exit 1
    fi
    if ! cmp -s <(grep '^\[calls\]' "$work/$variant-off/out") \
                <(grep '^\[calls\]' "$work/$variant-on/out"); then
        echo "owner calls inside the blocks differ ($variant)" >&2
        exit 1
    fi
    echo "prefilter differential ($variant): 6 phases identical OFF/ON"
    grep '^\[calls\]\|^\[outside\]' "$work/$variant-off/out" | sed 's/^/  OFF /'
    grep '^\[outside\]' "$work/$variant-on/out" | sed 's/^/  ON  /'
done
sed 's/^/  /' "$work/default-off/phases"

# 2. Mutations: each must change a digest or fail the oracle's own checks.
mutate() {
    # mutate NAME SOURCE OLD NEW (python replace, exactly one occurrence)
    mkdir -p "$work/mut-$1"
    python3 - "$2" "$work/mut-$1/$(basename "$2")" "$3" "$4" <<'EOF'
import pathlib, sys
src, dst, old, new = sys.argv[1:5]
text = pathlib.Path(src).read_text(encoding="utf-8")
count = text.count(old)
if count != 1:
    raise SystemExit(f"mutation anchor count {count} in {src}")
pathlib.Path(dst).write_text(text.replace(old, new), encoding="utf-8")
EOF
}
caught() {
    # caught NAME VARIANT_OFF_DIR (the build is in $work/mut-NAME/build)
    if run_oracle "$work/mut-$1/build" &&
            grep '^\[phase\]' "$work/mut-$1/build/out" |
            cmp -s - "$2/phases"; then
        echo "mutation survived: $1" >&2
        exit 1
    fi
    why=$(grep -m1 'failed at' "$work/mut-$1/build/out" || true)
    if [ -z "$why" ]; then
        why=$(grep '^\[phase\]' "$work/mut-$1/build/out" |
              diff - "$2/phases" | grep -m1 '^<' | cut -d' ' -f3)
        why="digest differs from phase $why"
    fi
    echo "mutation caught: $1 ($why)"
}
rt="$root/runtime"
nl='
'
mutate heap-no-texel "$rt/host_vita_heap.c" \
"#ifdef ISAAC_VITA_TEXEL_SCRATCH${nl}    may_own |= isaac_vita_heap_free_range_may_own(${nl}        &isaac_vita_texel_scratch_free_range, value);${nl}#endif${nl}" ""
build_oracle "$work/mut-heap-no-texel/build" "" "$PF" \
    "$work/mut-heap-no-texel/host_vita_heap.c"
caught heap-no-texel "$work/default-off"
mutate heap-no-ogg "$rt/host_vita_heap.c" \
"#ifdef ISAAC_VITA_OGG_EMERGENCY${nl}    may_own |= isaac_vita_heap_free_range_may_own(${nl}        &isaac_vita_ogg_emergency_free_range, value);${nl}#endif${nl}" ""
build_oracle "$work/mut-heap-no-ogg/build" "" "$PF" \
    "$work/mut-heap-no-ogg/host_vita_heap.c"
caught heap-no-ogg "$work/default-off"
mutate anm2-short-extent "$rt/host_vita_anm2_scratch.c" \
"&isaac_vita_anm2_scratch_free_range.end,${nl}            (uintptr_t)base + ISAAC_VITA_ANM2_MEMBLOCK_BYTES," \
"&isaac_vita_anm2_scratch_free_range.end,${nl}            (uintptr_t)base + 2220000U,"
build_oracle "$work/mut-anm2-short-extent/build" "" "$PF" "" "" \
    "$work/mut-anm2-short-extent/host_vita_anm2_scratch.c"
caught anm2-short-extent "$work/default-off"
mutate texel-8m-extent "$rt/host_vita_texel_scratch.c" \
"&isaac_vita_texel_scratch_free_range.end,${nl}            (uintptr_t)base + KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES," \
"&isaac_vita_texel_scratch_free_range.end,${nl}            (uintptr_t)base + 0x00801000U,"
build_oracle "$work/mut-texel-8m-extent/build" \
    "-DISAAC_VITA_TEXEL_SCRATCH_32M=1" "$PF" "" "" "" \
    "$work/mut-texel-8m-extent/host_vita_texel_scratch.c"
caught texel-8m-extent "$work/32m-off"
mutate ogg-reset-keeps-extent "$rt/host_vita_ogg_emergency.c" \
"    atomic_store_explicit(${nl}        &isaac_vita_ogg_emergency_free_range.end,${nl}        (uintptr_t)0U, memory_order_relaxed);${nl}" ""
build_oracle "$work/mut-ogg-reset-keeps-extent/build" "" "$PF" "" \
    "$work/mut-ogg-reset-keeps-extent/host_vita_ogg_emergency.c"
caught ogg-reset-keeps-extent "$work/default-off"
echo "prefilter mutations: 5/5 caught"

# 3. OFF identity against the branch base.
# ISAAC_PREFILTER_BASE_TREE: a directory holding recomp/runtime and
# recomp/vita of the base (for a worktree whose git metadata this shell
# cannot read); otherwise git archive extracts them.  A checkout without
# the base commit (the public snapshot) skips this leg only.
base_tree=${ISAAC_PREFILTER_BASE_TREE:-}
identity_skip=
if [ -z "$base_tree" ]; then
    if git -C "$root/.." cat-file -e "$base_rev^{commit}" 2>/dev/null; then
        base_tree="$work/base-tree"
        mkdir -p "$base_tree"
        git -C "$root/.." archive "$base_rev" recomp/runtime recomp/vita |
            tar -x -C "$base_tree"
    else
        identity_skip=1
        echo "OFF identity: SKIP (base commit ${base_rev:0:7} is not in this checkout's" \
            "history; set ISAAC_PREFILTER_BASE or ISAAC_PREFILTER_BASE_TREE)"
    fi
fi
[ -n "$identity_skip" ] || [ -f "$base_tree/recomp/runtime/host_vita_heap.c" ] ||
    { echo "base tree lacks recomp/runtime ($base_tree)" >&2; exit 2; }
fake="$work/fake-include"
mkdir -p "$fake/psp2/kernel"
cat > "$fake/psp2/types.h" <<'EOF'
#ifndef ISAAC_PREFILTER_PSP2_TYPES_H
#define ISAAC_PREFILTER_PSP2_TYPES_H
#include <stdint.h>
typedef int32_t SceUID;
typedef uint32_t SceSize;
#endif
EOF
cat > "$fake/psp2/kernel/sysmem.h" <<'EOF'
#ifndef ISAAC_PREFILTER_PSP2_SYSMEM_H
#define ISAAC_PREFILTER_PSP2_SYSMEM_H
#include <psp2/types.h>
typedef int SceKernelMemBlockType;
typedef struct SceKernelAllocMemBlockOpt {
    SceSize size;
    uint32_t opaque[15];
} SceKernelAllocMemBlockOpt;
enum { SCE_KERNEL_MEMBLOCK_TYPE_USER_RW = 0x0c20d060 };
SceUID sceKernelAllocMemBlock(const char *, SceKernelMemBlockType,
                              SceSize, SceKernelAllocMemBlockOpt *);
int sceKernelGetMemBlockBase(SceUID, void **);
int sceKernelFreeMemBlock(SceUID);
#endif
EOF
cat > "$fake/psp2/kernel/clib.h" <<'EOF'
#ifndef ISAAC_PREFILTER_PSP2_CLIB_H
#define ISAAC_PREFILTER_PSP2_CLIB_H
#include <psp2/types.h>
typedef void *SceClibMspace;
SceClibMspace sceClibMspaceCreate(void *, SceSize);
void sceClibMspaceDestroy(SceClibMspace);
void *sceClibMspaceMalloc(SceClibMspace, SceSize);
void *sceClibMspaceCalloc(SceClibMspace, SceSize, SceSize);
void *sceClibMspaceRealloc(SceClibMspace, void *, SceSize);
void sceClibMspaceFree(SceClibMspace, void *);
#endif
EOF
identity_runs=0
# identity TAG UNIT INCLUDE ON_FLAGS -- OFF_FLAGS...
identity() {
    [ -z "$identity_skip" ] || return 0
    tag=$1 unit=$2 include=$3 on_flags=$4
    shift 4
    out="$work/identity/$tag"
    mkdir -p "$out"
    for side in base cur; do
        case $side in
        base) tree="$base_tree/recomp" ;;
        cur) tree="$root" ;;
        esac
        (cd "$tree/runtime" &&
            "$host_cc" -std=gnu11 -O2 -w -I. -I../vita -I"$include" "$@" \
                -E -P "$unit" > "$out/$side.i" &&
            "$host_cc" -std=gnu11 -O2 -w -I. -I../vita -I"$include" "$@" \
                -c "$unit" -o "$out/$side.o")
    done
    # Token stream (whitespace-insensitive): an OFF hook macro on its own
    # line expands to nothing but leaves its indentation behind.
    for side in base cur; do
        tr -s ' \t\n' '\n' < "$out/$side.i" > "$out/$side.tok"
    done
    cmp -s "$out/base.tok" "$out/cur.tok" ||
        { echo "OFF preprocessed identity failed: $tag ($unit)" >&2; exit 1; }
    cmp -s "$out/base.o" "$out/cur.o" ||
        { echo "OFF object identity failed: $tag ($unit)" >&2; exit 1; }
    if [ -n "$on_flags" ]; then
        # shellcheck disable=SC2086
        (cd "$root/runtime" &&
            "$host_cc" -std=gnu11 -O2 -w -I. -I../vita -I"$include" "$@" \
                $on_flags -E -P "$unit" > "$out/on.i")
        if cmp -s "$out/cur.i" "$out/on.i"; then
            echo "ON define did not reach $unit ($tag)" >&2
            exit 1
        fi
    fi
    identity_runs=$((identity_runs + 1))
}
prod_heap="-DGUEST_IMAGE_BASE=0x98000000u -DGUEST_STACK_REQUIRED=1 \
-DISAAC_VITA_HEAP_TESTING=1 -DISAAC_VITA_HEAP_RANGE_LEASE=1 \
-DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=1 -DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=1 \
-DISAAC_VITA_HEAP_OVERFLOW_MSPACE=1 -DISAAC_VITA_HEAP_OVERFLOW_MSPACE_TESTING=1 \
-DISAAC_VITA_ROOM_ENTRY_SLAB=1 -DISAAC_VITA_ROOM_ENTRY_SLAB_TESTING=1 \
-DISAAC_VITA_ROOM_ENTRY_HYBRID=1 -DISAAC_VITA_ROOM_ENTRY_EXTERNAL_TESTING=1 \
-DISAAC_VITA_HEAP_TERMINAL_FASTPATH=1 -DISAAC_VITA_HEAP_SLAB_REJECT_FASTPATH=1 \
-DISAAC_VITA_OGG_EMERGENCY=1 -DISAAC_VITA_ANM2_SCRATCH=1 \
-DISAAC_VITA_TEXEL_SCRATCH=1 -DISAAC_VITA_TEXEL_OOM_DIAGNOSTIC=1"
stage="-DISAAC_VITA_HEAP_STAGE_PROFILE=1"
cost="-DISAAC_VITA_HEAP_FREE_COST_PROFILE=1"
# shellcheck disable=SC2086
identity heap-routing host_vita_heap.c "$oracle_include" "$PF" \
    -DGUEST_IMAGE_BASE=0x98000000u -DGUEST_STACK_REQUIRED=1 $heap_defs $interpose
# shellcheck disable=SC2086
identity heap-production host_vita_heap.c "$fake" "$PF" $prod_heap
# shellcheck disable=SC2086
identity heap-production-stage host_vita_heap.c "$fake" "$cost" $prod_heap $stage
# shellcheck disable=SC2086
identity ogg-oracle host_vita_ogg_emergency.c "$oracle_include" "$PF" \
    -DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_OGG_EMERGENCY_ORACLE=1
# shellcheck disable=SC2086
identity ogg-queue-oracle host_vita_ogg_emergency.c "$oracle_include" "$PF" \
    -DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_OGG_EMERGENCY_ORACLE=1 \
    -DISAAC_VITA_OGG_QUEUE_EMERGENCY=1
# shellcheck disable=SC2086
identity anm2-oracle host_vita_anm2_scratch.c "$oracle_include" "$PF" \
    -DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_ANM2_SCRATCH_ORACLE=1
for texel_flag in "" -DISAAC_VITA_TEXEL_SCRATCH_8M=1 -DISAAC_VITA_TEXEL_SCRATCH_32M=1; do
    # shellcheck disable=SC2086
    identity "texel-oracle${texel_flag:+-${texel_flag#-DISAAC_VITA_TEXEL_SCRATCH_}}" \
        host_vita_texel_scratch.c "$oracle_include" "$PF" \
        -DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_TEXEL_SCRATCH_ORACLE=1 $texel_flag
done
nv_mask="-DISAAC_VITA_NATIVE_VORBIS_FREE_WATCH_MASK=1"
for nv_extra in "" -DISAAC_VITA_NATIVE_VORBIS_ASYNC=1; do
    # shellcheck disable=SC2086
    identity "native-vorbis-watch${nv_extra:+-async}" host_vita_native_vorbis.c \
        "$oracle_include" "$nv_mask" -DGUEST_IMAGE_BASE=0x27000000u \
        -DISAAC_VITA_NATIVE_VORBIS_ORACLE=1 -DISAAC_VITA_NATIVE_VORBIS=1 \
        -DISAAC_VITA_NATIVE_VORBIS_FREE_WATCH=1 $nv_extra
done
pp="-DISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE=1 -DISAAC_VITA_PHASE_PROFILE=1 \
-DISAAC_VITA_PHASE_PROFILE_BUILD_ID=\"phase:oracle\""
# Only units that reach the eboot are compared. The host test drivers
# (vita_native_vorbis_oracle.c, kage_vita_phase_profile_oracle.c) gained lines,
# so every __LINE__ in their CHECK macros moves even with the knobs OFF; they
# never link into the Vita image and are covered by their own test runners.
for unit in kage_vita_phase_profile.c; do
    # shellcheck disable=SC2086
    identity "phase-profile-stage-${unit%.c}" "$unit" "$oracle_include" "$cost" \
        $pp $stage
    # shellcheck disable=SC2086
    identity "phase-profile-probe-${unit%.c}" "$unit" "$oracle_include" "$cost" \
        $pp $stage -DISAAC_VITA_GFUEL_SEED_NATIVE=1 \
        -DISAAC_VITA_CAMERA_RING_NATIVE=1 -DISAAC_VITA_UPDATE_PROBE=1
done
if [ -z "$identity_skip" ]; then
    echo "OFF identity vs ${base_rev:0:7}: $identity_runs unit/define sets (preprocessed + host object), ON reaches every unit"
fi

# 4. ARM legs.
if [ -z "${VITASDK:-}" ]; then
    echo "ARM legs: SKIP (VITASDK unset)"
else
    cc="$VITASDK/bin/arm-vita-eabi-gcc"
    nm="$VITASDK/bin/arm-vita-eabi-nm"
    arm="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb \
-fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror \
-DGUEST_STACK_REQUIRED=1 -DGUEST_IMAGE_BASE=0x98000000u \
-DISAAC_VITA_HAS_RUNTIME=1 -I$root/runtime -I$root/vita"
    for side in off on; do
        flag=""
        [ "$side" = on ] && flag="$PF"
        # shellcheck disable=SC2086
        "$cc" $arm $heap_defs -DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=1 \
            -DISAAC_VITA_HEAP_OVERFLOW_MSPACE=1 -DISAAC_VITA_ROOM_ENTRY_SLAB=1 \
            -DISAAC_VITA_ROOM_ENTRY_HYBRID=1 $flag \
            -c "$root/runtime/host_vita_heap.c" -o "$work/arm-heap-$side.o"
        for owner in ogg_emergency anm2_scratch texel_scratch; do
            # shellcheck disable=SC2086
            "$cc" $arm $flag -c "$root/runtime/host_vita_$owner.c" \
                -o "$work/arm-$owner-$side.o"
            if [ "$side" = on ]; then
                "$nm" "$work/arm-$owner-$side.o" |
                    grep -q " B isaac_vita_${owner}_free_range\$" ||
                    { echo "ARM $owner lacks its range record" >&2; exit 1; }
                "$nm" "$work/arm-heap-on.o" |
                    grep -q " U isaac_vita_${owner}_free_range\$" ||
                    { echo "ARM heap does not read the $owner record" >&2; exit 1; }
            elif "$nm" "$work/arm-$owner-$side.o" | grep -q "_free_range"; then
                echo "ARM $owner exports a range record while OFF" >&2
                exit 1
            fi
        done
    done
    if "$nm" "$work/arm-heap-off.o" | grep -q "_free_range"; then
        echo "ARM heap references a range record while OFF" >&2
        exit 1
    fi
    echo "ARM legs: heap + 3 owners compile OFF/ON; records exported and read only ON"
fi
echo "heap free prefilter gates: PASS ($work)"
