#!/usr/bin/env bash
# ARM (softfp Thumb-2) per-symbol gate for the two KAGE body-top seams on a
# configured and built Vita tree (recomp/vita, Ninja):
#
#   ISAAC_VITA_KAGE_MUTEX_SEAM    owner guest_0166.c -> isaac_vita_kage_mutex_{lock,unlock}_try
#   ISAAC_VITA_KAGE_REFCOUNT_SEAM owner guest_0000.c -> isaac_vita_kage_refcount_{release,addref,weaklock}_try
#
#   1. with the option ON: the owner object carries exactly one call
#      relocation to each *_try -- except addref_try, which GCC may reach
#      1..2 times (sub_00007b50 inlined into sub_00007b70) -- and no other
#      object references a *_try; the seam TU defines the *_try symbols and
#      the refcount TU imports no direct-edge census hook
#      (guest_phase_profile_note_lookup_cache_hit: generated units are
#      compiled without ISAAC_VITA_PHASE_PROFILE, so the seam replays none);
#   2. with the option OFF: no object in the tree mentions a *_try symbol;
#   3. when the linked ELF exists: every *_try has >= 1 `bl` site and the
#      sites sit in the frozen roots.
#
# usage: test_kage_seams_arm.sh <build-dir>   (VITASDK must be set)
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi
if [ $# -ne 1 ] || [ ! -f "$1/CMakeCache.txt" ]; then
    echo "usage: $0 <configured build dir>" >&2
    exit 2
fi
build=$1
nm="$VITASDK/bin/arm-vita-eabi-nm"
objdump="$VITASDK/bin/arm-vita-eabi-objdump"
failures=0

fail() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}

cache_on() { # option
    grep -Eq "^$1:BOOL=ON$" "$build/CMakeCache.txt"
}

find_obj() { # basename -> path (exactly one); otherwise the candidates (or
             # "none") and status 1 -- the caller records the failure (this
             # runs in a $(...) subshell, where `fail` could not count)
    local hits
    hits=$(find "$build" -name "$1.obj" -o -name "$1.o" | sort)
    if [ "$(printf '%s\n' "$hits" | grep -c .)" -ne 1 ]; then
        printf '%s\n' "${hits:-none}" | tr '\n' ' '
        return 1
    fi
    printf '%s\n' "$hits"
}

call_relocs() { # object symbol -> count of call/jump relocations
    "$objdump" -dr "$1" |
        grep -cE "R_ARM_(THM_CALL|THM_JUMP24|CALL|JUMP24)[[:space:]]+$2$" || true
}

references_anywhere() { # symbol -> objects (other than the two given) referencing it
    local symbol=$1 owner=$2 tu=$3
    find "$build" \( -name '*.obj' -o -name '*.o' \) -print0 |
        xargs -0 "$nm" -A 2>/dev/null |
        grep -E "[[:space:]][UTt][[:space:]]$symbol$" |
        cut -d: -f1 | sort -u | grep -vxF -e "$owner" -e "$tu" || true
}

gate_tu="" # set by gate_seam: the seam TU object (option ON), else empty
gate_seam() { # option owner-unit seam-tu helper[:min:max]...
    local option=$1 owner_unit=$2 tu_name=$3
    shift 3
    gate_tu=""
    if ! cache_on "$option"; then
        echo "$option=OFF: every object must be free of the seam symbols"
        for spec in "$@"; do
            local symbol=${spec%%:*}
            local refs
            refs=$(find "$build" \( -name '*.obj' -o -name '*.o' \) -print0 |
                xargs -0 "$nm" -A 2>/dev/null | grep -cE "[[:space:]]$symbol$" || true)
            if [ "$refs" -ne 0 ]; then
                fail "$option=OFF but $refs object symbol table(s) mention $symbol"
            else
                echo "  $symbol: absent"
            fi
        done
        return
    fi
    local owner tu
    if ! owner=$(find_obj "$owner_unit.c"); then
        fail "expected exactly one $owner_unit.c object, found: $owner"
        return 0
    fi
    if ! tu=$(find_obj "$tu_name.c"); then
        fail "expected exactly one $tu_name.c object, found: $tu"
        return 0
    fi
    echo "$option=ON: owner $owner"
    for spec in "$@"; do
        local symbol min max count defined others
        IFS=: read -r symbol min max <<< "$spec"
        count=$(call_relocs "$owner" "$symbol")
        if [ "$count" -lt "$min" ] || [ "$count" -gt "$max" ]; then
            fail "$owner_unit.c has $count call relocation(s) to $symbol (want $min..$max)"
        else
            echo "  $symbol: $count call relocation(s) in $owner_unit.c (want $min..$max)"
        fi
        defined=$("$nm" "$tu" | grep -cE "[[:space:]]T[[:space:]]$symbol$" || true)
        [ "$defined" -eq 1 ] || fail "$tu_name.c does not define $symbol exactly once ($defined)"
        others=$(references_anywhere "$symbol" "$owner" "$tu")
        [ -z "$others" ] || fail "$symbol is referenced outside its owner/TU: $others"
    done
    gate_tu=$tu
}

# ---- mutex seam --------------------------------------------------------------
# (gate_seam runs in this shell, never in a pipeline or $(...): its `fail`
# counts must reach the verdict below.)
gate_seam ISAAC_VITA_KAGE_MUTEX_SEAM guest_0166 host_vita_kage_mutex_seam \
    isaac_vita_kage_mutex_lock_try:1:1 isaac_vita_kage_mutex_unlock_try:1:1

# ---- refcount seam -------------------------------------------------------------
gate_seam ISAAC_VITA_KAGE_REFCOUNT_SEAM guest_0000 host_vita_kage_refcount_seam \
    isaac_vita_kage_refcount_release_try:1:1 isaac_vita_kage_refcount_addref_try:1:2 \
    isaac_vita_kage_refcount_weaklock_try:1:1
refcount_tu=$gate_tu
if [ -n "$refcount_tu" ] && [ -f "$refcount_tu" ]; then
    if "$nm" -u "$refcount_tu" | grep -q guest_phase_profile_note_lookup_cache_hit; then
        fail "the refcount seam TU imports the direct-edge census hook (generated units record none)"
    else
        echo "  refcount seam TU imports no direct-edge census hook"
    fi
fi

# ---- linked ELF ------------------------------------------------------------------
elf=$(find "$build" -maxdepth 1 -type f -name 'isaac_first_arm_fault' | head -n 1)
if [ -n "$elf" ]; then
    echo "ELF $elf: bl sites per caller"
    "$objdump" -d "$elf" |
        awk '/^[0-9a-f]+ <.*>:$/ {fn=$2}
             /bl.*<isaac_vita_kage_(mutex|refcount)_[a-z]+_try>/ {print fn, $NF}' |
        sort | uniq -c > "${TMPDIR:-/tmp}/kage-seams-bl.txt"
    cat "${TMPDIR:-/tmp}/kage-seams-bl.txt"
    for pair in \
        "ISAAC_VITA_KAGE_MUTEX_SEAM sub_00562e00 isaac_vita_kage_mutex_lock_try" \
        "ISAAC_VITA_KAGE_MUTEX_SEAM sub_00562ec0 isaac_vita_kage_mutex_unlock_try" \
        "ISAAC_VITA_KAGE_REFCOUNT_SEAM sub_00007af0 isaac_vita_kage_refcount_release_try" \
        "ISAAC_VITA_KAGE_REFCOUNT_SEAM sub_00007b50 isaac_vita_kage_refcount_addref_try" \
        "ISAAC_VITA_KAGE_REFCOUNT_SEAM sub_00007b70 isaac_vita_kage_refcount_weaklock_try"; do
        set -- $pair
        cache_on "$1" || continue
        if ! grep -qE "^ *[0-9]+ <$2>: <$3>$" "${TMPDIR:-/tmp}/kage-seams-bl.txt"; then
            fail "no bl from $2 to $3 in the ELF"
        fi
    done
else
    echo "(no linked ELF in $build: object gates only)"
fi

if [ "$failures" -ne 0 ]; then
    echo "KAGE seams ARM gate: FAIL ($failures)" >&2
    exit 1
fi
echo "KAGE seams ARM gate: PASS"
