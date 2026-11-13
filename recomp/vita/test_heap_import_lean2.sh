#!/usr/bin/env bash
# ISAAC_VITA_HEAP_IMPORT_LEAN2 host gates, gcc leg (Linux host gcc; the ARM
# leg runs when VITASDK is set).  The clang leg (memblock ledger, overflow
# mspace, room slab, retag/rollback, threads, router oracles, whole-object
# OFF identity) is recomp/test_heap_import_lean2.py.
#
#  1. Differential: the prefilter trace oracle
#     (runtime/host_vita_heap_free_prefilter_oracle.c: the real heap import
#     with the real OGG emergency / ANM2 scratch / texel scratch owners at
#     the measured device layout, growing ledger) built with
#     ISAAC_VITA_HEAP_LEAN_IMPORT as the comparator, then with
#     + ISAAC_VITA_HEAP_IMPORT_LEAN2 and + ..._LEAN2_VERIFY, for the default,
#     8M and 32M texel capacities with the free prefilter, and the default
#     capacity without it.  Every [phase] digest and the [calls]/[outside]
#     lines must be identical; VERIFY must report sidecar, owner and reserve
#     checks > 0 and no disagreement.  This is where the owner cuts (ANM2
#     site switch, texel owner-0 gate) and the reserve cut on a growing
#     ledger run against the real owners.
#  2. Mutations: an ANM2 site dropped from the owner switch and a closed
#     texel gate must change a digest or fail the oracle; the same ANM2 drop
#     and a removed router reserve must show as VERIFY disagreements.
#  3. OFF identity against the lane base: preprocessed token stream and host
#     object of host_vita_heap.c under the lean define sets; the LEAN2 define
#     must reach the unit.  The base tree comes from `git archive` of
#     ISAAC_LEAN2_BASE (default d874a40) unless ISAAC_LEAN2_BASE_TREE (or the
#     copied name ISAAC_PREFILTER_BASE_TREE) names an extracted tree.  Under
#     WSL from a Windows-created worktree, git cannot follow the worktree's
#     .git file (it holds a Windows path) and this step exits 2: set
#     ISAAC_LEAN2_BASE_TREE to an extracted d874a40 tree, or GIT_DIR to the
#     common git dir.  A normal clone (the box) needs neither.
#  4. ARM (VITASDK): Thumb compile of the production heap define set with
#     LEAN2 and LEAN2+VERIFY under -Werror; the banner literal is in the
#     object exactly with LEAN2.
set -eu

root=${ISAAC_LEAN2_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_LEAN2_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-heap-lean2.XXXXXX")}
base_rev=${ISAAC_LEAN2_BASE:-d874a40397640d856791c65f0bfc7ff18314a159}
host_cc=${HOST_CC:-gcc}
mkdir -p "$work"

# copied: test_heap_free_prefilter.sh:27-43
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

LEAN=-DISAAC_VITA_HEAP_LEAN_IMPORT=1
L2=-DISAAC_VITA_HEAP_IMPORT_LEAN2=1
L2V=-DISAAC_VITA_HEAP_IMPORT_LEAN2_VERIFY=1

"$host_cc" $routing -c "$root/runtime/guest.c" -o "$work/guest.o"

# Adapted from test_heap_free_prefilter.sh:46-73: the heap object takes the
# lean flags, and the shim TU (heap_import_lean2_prefilter_shim.c) receives
# the heap's isaac_vita_log calls and prints the [lean2] receipt.
# build_oracle DIR TEXEL_FLAG PF_FLAG LEAN_FLAGS [HEAP_SRC]
build_oracle() {
    dir=$1 texel=$2 pf=$3 lean=$4
    heap=${5:-$root/runtime/host_vita_heap.c}
    mkdir -p "$dir"
    # shellcheck disable=SC2086
    "$host_cc" $routing $heap_defs $interpose $pf $lean -c "$heap" -o "$dir/heap.o"
    # shellcheck disable=SC2086
    "$host_cc" $routing -DISAAC_VITA_OGG_EMERGENCY_ORACLE=1 $pf \
        -c "$root/runtime/host_vita_ogg_emergency.c" -o "$dir/ogg.o"
    # shellcheck disable=SC2086
    "$host_cc" $routing -DISAAC_VITA_ANM2_SCRATCH_ORACLE=1 $pf \
        -c "$root/runtime/host_vita_anm2_scratch.c" -o "$dir/anm2.o"
    # shellcheck disable=SC2086
    "$host_cc" $routing -DISAAC_VITA_TEXEL_SCRATCH_ORACLE=1 $texel $pf \
        -c "$root/runtime/host_vita_texel_scratch.c" -o "$dir/texel.o"
    # shellcheck disable=SC2086
    "$host_cc" $routing $owner_oracles $texel $pf \
        -c "$root/runtime/host_vita_heap_free_prefilter_oracle.c" \
        -o "$dir/oracle.o"
    # shellcheck disable=SC2086
    "$host_cc" $routing $heap_defs $lean \
        -c "$root/runtime/heap_import_lean2_prefilter_shim.c" -o "$dir/shim.o"
    # shellcheck disable=SC2086
    "$host_cc" -pthread "$dir/heap.o" "$work/guest.o" "$dir/ogg.o" \
        "$dir/anm2.o" "$dir/texel.o" "$dir/oracle.o" "$dir/shim.o" $wraps \
        -Wl,--wrap=isaac_vita_log -Wl,--gc-sections -o "$dir/oracle"
}

# Adapted from test_heap_free_prefilter.sh:75-80: stdout and stderr apart.
# guest_fault's stderr lines land between stdout lines at buffer flush
# points, and the lean2 receipt lines move those points, so a merged file
# differs between builds whose [phase] lines are identical.
run_oracle() {
    if "$1/oracle" > "$1/out" 2> "$1/err"; then
        return 0
    fi
    return 1
}

# lean2_field DIR NAME: a field of the [lean2] receipt line.
lean2_field() {
    grep -m1 '^\[lean2\] verify=' "$1/out" | tr ' ' '\n' |
        sed -n "s/^$2=//p"
}
# same_lines REF DIR TAG LABEL: the [TAG] lines of two runs are identical.
same_lines() {
    if ! cmp -s <(grep "^\[$3\]" "$1/out") <(grep "^\[$3\]" "$2/out"); then
        diff <(grep "^\[$3\]" "$1/out") <(grep "^\[$3\]" "$2/out") >&2 || true
        echo "lean2 changed the [$3] lines ($4)" >&2
        exit 1
    fi
}

# 1. Differential.
for variant in default 8m 32m default-nopf; do
    case $variant in
    default) texel_flag="" pf="$PF" ;;
    8m) texel_flag="-DISAAC_VITA_TEXEL_SCRATCH_8M=1" pf="$PF" ;;
    32m) texel_flag="-DISAAC_VITA_TEXEL_SCRATCH_32M=1" pf="$PF" ;;
    default-nopf) texel_flag="" pf="" ;;
    esac
    for side in lean l2 l2v; do
        case $side in
        lean) flags="$LEAN" ;;
        l2) flags="$LEAN $L2" ;;
        l2v) flags="$LEAN $L2 $L2V" ;;
        esac
        build_oracle "$work/$variant-$side" "$texel_flag" "$pf" "$flags"
        if ! run_oracle "$work/$variant-$side"; then
            cat "$work/$variant-$side/out" "$work/$variant-$side/err" >&2
            echo "lean2 oracle $variant-$side failed" >&2
            exit 1
        fi
    done
    [ "$(grep -c '^\[phase\]' "$work/$variant-lean/out")" -eq 6 ] ||
        { echo "expected 6 phases ($variant)" >&2; exit 1; }
    grep '^\[phase\]' "$work/$variant-lean/out" > "$work/$variant-lean/phases"
    for side in l2 l2v; do
        for tag in phase calls outside; do
            same_lines "$work/$variant-lean" "$work/$variant-$side" "$tag" \
                "$variant-$side"
        done
    done
    grep -q '^\[lean2\] comparator' "$work/$variant-lean/out" ||
        { echo "comparator receipt missing ($variant)" >&2; exit 1; }
    [ "$(lean2_field "$work/$variant-l2" verify)" = 0 ] &&
        [ "$(lean2_field "$work/$variant-l2" sidecar_nonzero)" = 0 ] ||
        { echo "lean2 receipt wrong ($variant-l2)" >&2; exit 1; }
    v=$work/$variant-l2v
    checks=$(lean2_field "$v" checks)
    [ "$(lean2_field "$v" verify)" = 1 ] &&
        [ "$(lean2_field "$v" sidecar_nonzero)" = 0 ] &&
        [ "$(lean2_field "$v" bad)" = 0/0/0/0 ] &&
        [ "$(lean2_field "$v" mismatch_lines)" = 0 ] &&
        [ "$(echo "$checks" | cut -d/ -f1)" -gt 0 ] &&
        [ "$(echo "$checks" | cut -d/ -f2)" -gt 0 ] &&
        [ "$(echo "$checks" | cut -d/ -f3)" -gt 0 ] ||
        { grep '^\[lean2\]' "$v/out" >&2;
          echo "lean2 verify receipt wrong ($variant-l2v)" >&2; exit 1; }
    echo "lean2 differential ($variant): 6 phases + owner calls identical lean/lean2/lean2+verify"
    echo "  l2v $(grep -m1 '^\[lean2\] verify=' "$v/out")"
done
grep '^\[calls\]\|^\[outside\]' "$work/default-lean/out" | sed 's/^/  /'
sed 's/^/  /' "$work/default-lean/phases"

# 2. Mutations.
# copied: test_heap_free_prefilter.sh:119-131
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
# caught_digest NAME REF_DIR: the mutant oracle fails or a digest changes.
caught_digest() {
    if run_oracle "$work/mut-$1/build" &&
            grep '^\[phase\]' "$work/mut-$1/build/out" |
            cmp -s - "$2/phases"; then
        echo "mutation survived: $1" >&2
        exit 1
    fi
    why=$(grep -m1 'failed at' "$work/mut-$1/build/err" || true)
    if [ -z "$why" ]; then
        why=$(grep '^\[phase\]' "$work/mut-$1/build/out" |
              diff - "$2/phases" | grep -m1 '^<' | cut -d' ' -f3)
        why="digest differs from phase $why"
    fi
    echo "mutation caught: $1 ($why)"
}
# caught_verify NAME REF_DIR INDEX KIND: VERIFY keeps the route (digests
# equal), counts the disagreement and logs its first mismatch.
caught_verify() {
    d=$work/mut-$1/build
    run_oracle "$d" ||
        { cat "$d/out" "$d/err" >&2; echo "mutant $1 failed" >&2; exit 1; }
    grep '^\[phase\]' "$d/out" | cmp -s - "$2/phases" ||
        { echo "VERIFY did not keep the route under $1" >&2; exit 1; }
    bad=$(lean2_field "$d" bad | cut -d/ -f"$3")
    if [ "${bad:-0}" -gt 0 ] &&
            grep -q "lean2 verify: FIRST MISMATCH kind=$4" "$d/out"; then
        echo "mutation caught: $1 (verify $4 bad=$bad)"
    else
        echo "mutation survived: $1" >&2
        exit 1
    fi
}
rt="$root/runtime"
nl='
'
mutate anm2-site-dropped "$rt/host_vita_heap.c" \
"    case ISAAC_VITA_ANM2_OWNER_RETURN_5:${nl}" ""
build_oracle "$work/mut-anm2-site-dropped/build" "" "$PF" "$LEAN $L2" \
    "$work/mut-anm2-site-dropped/host_vita_heap.c"
caught_digest anm2-site-dropped "$work/default-lean"
mkdir -p "$work/mut-anm2-site-dropped-verify"
build_oracle "$work/mut-anm2-site-dropped-verify/build" "" "$PF" \
    "$LEAN $L2 $L2V" "$work/mut-anm2-site-dropped/host_vita_heap.c"
caught_verify anm2-site-dropped-verify "$work/default-lean" 2 owner
mutate texel-gate-closed "$rt/host_vita_heap.c" \
"    if (owner_return_rva)${nl}#endif${nl}" "    if (0)${nl}#endif${nl}"
build_oracle "$work/mut-texel-gate-closed/build" "" "$PF" "$LEAN $L2" \
    "$work/mut-texel-gate-closed/host_vita_heap.c"
caught_digest texel-gate-closed "$work/default-lean"
mutate router-reserve-skipped "$rt/host_vita_heap.c" \
"    if (!vita_heap_ledger_reserve(1U)) {${nl}        if (failure_reason)${nl}            *failure_reason = \"ledger-reserve-failed\";" \
"    if (0) {${nl}        if (failure_reason)${nl}            *failure_reason = \"ledger-reserve-failed\";"
build_oracle "$work/mut-router-reserve-skipped/build" "" "$PF" \
    "$LEAN $L2 $L2V" "$work/mut-router-reserve-skipped/host_vita_heap.c"
caught_verify router-reserve-skipped "$work/default-lean" 3 reserve
echo "lean2 mutations: 4/4 caught"

# 3. OFF identity against the lane base.
# copied: test_heap_free_prefilter.sh:185-232 (the base-tree override also
# reads the lane-named ISAAC_LEAN2_BASE_TREE first; see the header, step 3)
base_tree=${ISAAC_LEAN2_BASE_TREE:-${ISAAC_PREFILTER_BASE_TREE:-}}
if [ -z "$base_tree" ]; then
    base_tree="$work/base-tree"
    mkdir -p "$base_tree"
    git -C "$root/.." archive "$base_rev" recomp/runtime recomp/vita |
        tar -x -C "$base_tree"
fi
[ -f "$base_tree/recomp/runtime/host_vita_heap.c" ] ||
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
# copied: test_heap_free_prefilter.sh:233-271
identity_runs=0
# identity TAG UNIT INCLUDE ON_FLAGS -- OFF_FLAGS...
identity() {
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
# copied: test_heap_free_prefilter.sh:272-281
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
# shellcheck disable=SC2086
identity heap-routing-lean host_vita_heap.c "$oracle_include" "$L2" \
    -DGUEST_IMAGE_BASE=0x98000000u -DGUEST_STACK_REQUIRED=1 $heap_defs \
    $interpose $PF $LEAN
# shellcheck disable=SC2086
identity heap-production-lean host_vita_heap.c "$fake" "$L2" $prod_heap $LEAN
# shellcheck disable=SC2086
identity heap-production-lean-pf host_vita_heap.c "$fake" "$L2" $prod_heap \
    $LEAN $PF
# shellcheck disable=SC2086
identity heap-production-lean-stage host_vita_heap.c "$fake" "$L2 $L2V" \
    $prod_heap $LEAN $PF $stage
# shellcheck disable=SC2086
identity heap-production host_vita_heap.c "$fake" "" $prod_heap
echo "OFF identity vs ${base_rev:0:7}: $identity_runs define sets (preprocessed + host object), LEAN2 reaches the unit"

# 4. ARM leg.
if [ -z "${VITASDK:-}" ]; then
    echo "ARM leg: SKIP (VITASDK unset)"
else
    cc="$VITASDK/bin/arm-vita-eabi-gcc"
    arm="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb \
-fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror \
-DGUEST_STACK_REQUIRED=1 -DGUEST_IMAGE_BASE=0x98000000u \
-DISAAC_VITA_HAS_RUNTIME=1 -I$root/runtime -I$root/vita"
    prod_arm="$heap_defs -DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=1 \
-DISAAC_VITA_HEAP_OVERFLOW_MSPACE=1 -DISAAC_VITA_ROOM_ENTRY_SLAB=1 \
-DISAAC_VITA_ROOM_ENTRY_HYBRID=1 -DISAAC_VITA_HEAP_TERMINAL_FASTPATH=1 \
-DISAAC_VITA_HEAP_SLAB_REJECT_FASTPATH=1 $PF $LEAN"
    for side in lean l2 l2v; do
        case $side in
        lean) flags="" mode="" ;;
        l2) flags="$L2" mode=ON ;;
        l2v) flags="$L2 $L2V" mode=VERIFY ;;
        esac
        # shellcheck disable=SC2086
        "$cc" $arm $prod_arm $flags -c "$root/runtime/host_vita_heap.c" \
            -o "$work/arm-heap-$side.o"
        n=$(strings "$work/arm-heap-$side.o" |
            grep -c 'guest heap lean2 import: mode=' || true)
        if [ -z "$mode" ]; then
            [ "$n" -eq 0 ] || { echo "ARM lean object carries the lean2 banner" >&2; exit 1; }
        else
            strings "$work/arm-heap-$side.o" |
                grep -q "guest heap lean2 import: mode=$mode sidecars=1 prefetch=1 terminal=1 owner=1/1 reserve=1 slabinl=1" ||
                { echo "ARM $side object lacks its banner" >&2; exit 1; }
        fi
    done
    echo "ARM leg: production heap compiles lean/lean2/lean2+verify (-Werror), banner only with lean2"
fi
echo "heap import lean2 gcc gates: PASS ($work)"
