#!/usr/bin/env bash
set -eu

root=${ISAAC_IMPORT_ID_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_IMPORT_ID_TEST_OUT:-}" ]; then
    work=$ISAAC_IMPORT_ID_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-import-id.XXXXXX")
fi

if [ -n "${ISAAC_IMPORT_ID_GUEST_TABLE:-}" ]; then
    python3 "$root/test_vita_import_id_map.py" \
        --guest-table "$ISAAC_IMPORT_ID_GUEST_TABLE"
else
    python3 "$root/test_vita_import_id_map.py"
fi

host_cc=${HOST_CC:-${CC:-cc}}
common="-std=c11 -O2 -Wall -Wextra -Werror -I$root/runtime"
for config in audio-off audio-on census; do
    if [ "$config" = audio-on ]; then
        feature_flags="-DISAAC_VITA_AUDIO=1 -DISAAC_VITA_XINPUT=1"
    elif [ "$config" = census ]; then
        # ISAAC_VITA_PROFILE_IMPORT_KINDS: per-ID counters and hot pins.
        feature_flags="-DISAAC_VITA_AUDIO=1 -DISAAC_VITA_XINPUT=1 -DISAAC_VITA_PROFILE_IMPORT_KINDS=1"
    else
        feature_flags="-DISAAC_VITA_AUDIO=0 -DISAAC_VITA_XINPUT=0"
    fi
    "$host_cc" $common $feature_flags \
        "$root/runtime/host_vita_import_id.c" \
        "$root/runtime/host_vita_import_id_oracle.c" \
        -o "$work/host-vita-import-id-oracle-$config"
    "$work/host-vita-import-id-oracle-$config"
done

if [ -n "${VITASDK:-}" ]; then
    vita_cc="$VITASDK/bin/arm-vita-eabi-gcc"
    vita_ld="$VITASDK/bin/arm-vita-eabi-ld"
    readelf="$VITASDK/bin/arm-vita-eabi-readelf"
    nm="$VITASDK/bin/arm-vita-eabi-nm"
    "$vita_cc" $common -DISAAC_VITA_AUDIO=1 -DISAAC_VITA_XINPUT=1 \
        -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp \
        -mthumb -ffunction-sections -fdata-sections \
        "$root/runtime/host_vita_import_id.c" \
        "$root/runtime/host_vita_import_id_oracle.c" \
        -Wl,--gc-sections -o "$work/vita-import-id-oracle.elf"
    "$readelf" -h "$work/vita-import-id-oracle.elf" > "$work/header"
    "$readelf" -A "$work/vita-import-id-oracle.elf" > "$work/attributes"
    "$nm" -u "$work/vita-import-id-oracle.elf" > "$work/undefined"
    if ! grep -q "soft-float ABI" "$work/header"; then
        echo "import-ID oracle is not marked soft-float ABI" >&2
        exit 1
    fi
    if grep -q "Tag_ABI_VFP_args" "$work/attributes"; then
        echo "import-ID oracle unexpectedly advertises hardfp args" >&2
        exit 1
    fi
    if [ -s "$work/undefined" ]; then
        cat "$work/undefined" >&2
        echo "import-ID oracle has unresolved symbols" >&2
        exit 1
    fi
    echo "Vita softfp compile/link: PASS"

    # Compile every production owner changed by the indexed entrypoints.  The
    # established guest-stack inline-asm guard triggers GCC 10's known
    # maybe-uninitialized false positive in several otherwise-green oracles.
    # Suppress only that diagnostic; all other warnings remain fatal.
    production_sources="guest host_vita_first_fault host_vita_import_id \
host_vita_audio host_vita_com host_vita_console host_vita_crt \
host_vita_exception host_vita_file_lock host_vita_filesystem host_vita_find \
host_vita_fls host_vita_gl host_vita_heap host_vita_math host_vita_memory \
host_vita_post_com host_vita_rtti host_vita_startup host_vita_steam \
host_vita_sync host_vita_user32"
    production_flags="$common -mcpu=cortex-a9 -mfpu=neon \
-mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections \
-fdata-sections -Wno-maybe-uninitialized -DGUEST_IMAGE_BASE=0x98000000u"
    production_objects=
    for stem in $production_sources; do
        owner_flags=
        case "$stem" in
            guest)
                # Standalone guest.c lacks the generated stack users which
                # retain this cold diagnostic in the real target.
                owner_flags="-DISAAC_VITA_IMPORT_ID_DISPATCH=1 \
-Wno-unused-function"
                ;;
            host_vita_first_fault|host_vita_import_id)
                owner_flags="-DISAAC_VITA_AUDIO=1 -DISAAC_VITA_XINPUT=1"
                ;;
            host_vita_crt)
                # Vita newlib hides fileno behind its feature-view macros in
                # this isolated TU compile; test_crt_imports.sh owns the
                # integrated CRT compile/link gate.
                owner_flags="-Wno-implicit-function-declaration"
                ;;
        esac
        "$vita_cc" $production_flags $owner_flags \
            -I"$root/vita" -c "$root/runtime/$stem.c" \
            -o "$work/$stem.o"
        production_objects="$production_objects $work/$stem.o"
    done
    echo "Vita 22-owner production compile: PASS"
    # The opt-in direct IAT dispatch (ISAAC_VITA_IAT_DIRECT) adds the per-ID
    # table and guest_import_call to guest.c; it must compile warning-free
    # with the same production flags and export exactly that entry point.
    "$vita_cc" $production_flags -DISAAC_VITA_IMPORT_ID_DISPATCH=1 \
        -DISAAC_VITA_IMPORT_DIRECT=1 -Wno-unused-function \
        -I"$root/vita" -c "$root/runtime/guest.c" \
        -o "$work/guest_import_direct.o"
    if ! "$nm" "$work/guest_import_direct.o" | grep -q " T guest_import_call$"; then
        echo "direct IAT dispatch build lacks guest_import_call" >&2
        exit 1
    fi
    echo "Vita guest.c ISAAC_VITA_IMPORT_DIRECT compile: PASS"
    # Shape and size pin of the direct entry point under these flags
    # (arm-vita-eabi-gcc 10.3, -O2 Thumb-2): exactly one indirect family
    # call, one tail jump to guest_call (the fail-closed path), one
    # guest_fault call, and no more than 46 instructions.  The perf
    # configuration (PHASE_PROFILE, DISPATCH_TABLE, LOOKUP_CACHE, sync fast
    # path) compiles the same body to 59 instructions / 0xa4 B: the
    # per-import census note (GUEST_PHASE_PROFILE_NOTE_IMPORT) adds 8.
    objdump="$VITASDK/bin/arm-vita-eabi-objdump"
    "$objdump" -dr --no-show-raw-insn -j .text.guest_import_call \
        "$work/guest_import_direct.o" > "$work/guest_import_call.dis"
    direct_insns=$(grep -cE '^ +[0-9a-f]+:[[:space:]]' \
        "$work/guest_import_call.dis" || true)
    direct_blx=$(grep -cE '^ +[0-9a-f]+:[[:space:]]+blx[[:space:]]+r[0-9]+$' \
        "$work/guest_import_call.dis" || true)
    direct_tail=$(grep -cE 'R_ARM_THM_JUMP24[[:space:]]+guest_call$' \
        "$work/guest_import_call.dis" || true)
    direct_fault=$(grep -cE 'R_ARM_THM_CALL[[:space:]]+guest_fault$' \
        "$work/guest_import_call.dis" || true)
    if [ "$direct_insns" -lt 1 ] || [ "$direct_insns" -gt 46 ] ||
       [ "$direct_blx" != 1 ] || [ "$direct_tail" != 1 ] ||
       [ "$direct_fault" != 1 ]; then
        cat "$work/guest_import_call.dis" >&2
        echo "guest_import_call shape drifted: $direct_insns insns," \
             "$direct_blx blx, $direct_tail guest_call tails," \
             "$direct_fault guest_fault calls" >&2
        exit 1
    fi
    echo "Vita guest_import_call shape: $direct_insns insns (<= 46)," \
         "1 blx family, 1 b.w guest_call, 1 bl guest_fault: PASS"
    "$vita_ld" -r $production_objects -o "$work/production-import-owners.o"
    "$nm" -u "$work/production-import-owners.o" \
        > "$work/production-import-owners.undefined"
    if grep -Eq \
        'guest_host_import_ids_register|guest_host_import_id|isaac_vita_[a-z0-9_]+_import_(indexed|name)' \
        "$work/production-import-owners.undefined"; then
        grep -E \
            'guest_host_import_ids_register|guest_host_import_id|isaac_vita_[a-z0-9_]+_import_(indexed|name)' \
            "$work/production-import-owners.undefined" >&2
        echo "production import-ID owner closure is unresolved" >&2
        exit 1
    fi
    echo "Vita 22-owner partial-link closure: PASS"
else
    echo "VITASDK unset: target compile/link skipped; host oracle passed"
fi
