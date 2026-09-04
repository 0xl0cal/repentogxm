#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_GUEST_STACK_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_GUEST_STACK_TEST_OUT:-}" ]; then
    work=$ISAAC_GUEST_STACK_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-guest-stack.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
objdump="$VITASDK/bin/arm-vita-eabi-objdump"

flags="-std=gnu11 -O2 -DGUEST_STACK_REQUIRED=1 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
compiler_version=$("$cc" -dumpfullversion -dumpversion)
compiler_sha=$(sha256sum "$cc" | sed 's/[[:space:]].*//')
if [ "$compiler_version" != "10.3.0" ] ||
   [ "$compiler_sha" != \
     "bd695a1274bdb545a73e487726384a61c499ff301a794e276ac597741d1d6495" ]; then
    echo "guest stack LR contract requires pinned GCC 10.3.0/bd695a12" >&2
    echo "actual version=$compiler_version sha256=$compiler_sha" >&2
    exit 9
fi
"$cc" --version > "$work/compiler-version"
printf '%s\n' "$flags" > "$work/compiler-flags"

# The same exhaustive source executes twice on PC.  This edge proves that the
# checked owner/bounds/setter/address API and its guest.c fault boundary are a
# link-clean ARMv7 Thumb softfp contract; the ELF is intentionally not run.
"$cc" $flags -I"$root/runtime" \
    "$root/runtime/guest.c" \
    "$root/runtime/guest_stack_oracle.c" \
    -Wl,--gc-sections \
    -lSceLibKernel_stub -lSceKernelThreadMgr_stub \
    -o "$work/vita-guest-stack-oracle.elf"

"$readelf" -h "$work/vita-guest-stack-oracle.elf" > "$work/header"
"$readelf" -A "$work/vita-guest-stack-oracle.elf" > "$work/attributes"
"$nm" -u "$work/vita-guest-stack-oracle.elf" > "$work/undefined"
for spec in \
    action_generated_set:guest_stack_set_generated \
    action_generated_adjust:guest_stack_adjust_generated \
    action_generated_access:guest_stack_address_generated \
    action_generated_push:gpush_generated \
    action_generated_pop:gpop_generated
do
    action=${spec%%:*}
    helper=${spec#*:}
    "$objdump" -dr --disassemble="$action" \
        "$work/vita-guest-stack-oracle.elf" > "$work/$action.disassembly"
    "$objdump" -dr --disassemble="$helper" \
        "$work/vita-guest-stack-oracle.elf" > "$work/$helper.disassembly"
done
"$objdump" -dr --disassemble=guest_stack_native_violation \
    "$work/vita-guest-stack-oracle.elf" > \
    "$work/native-violation.disassembly"
cat "$work/header"
cat "$work/attributes"

if ! grep -q "Machine:.*ARM" "$work/header" ||
   ! grep -q "soft-float ABI" "$work/header"; then
    echo "guest stack oracle is not ARM soft-float" >&2
    exit 3
fi
if grep -q "Tag_ABI_VFP_args" "$work/attributes"; then
    echo "guest stack oracle unexpectedly advertises hardfp arguments" >&2
    exit 4
fi
if grep -Eq 'guest_stack|guest_host_|isaac_vita_fixed_|__atomic|libatomic' \
        "$work/undefined"; then
    cat "$work/undefined" >&2
    echo "guest stack oracle retained a forbidden unresolved symbol" >&2
    exit 5
fi
if ! "$nm" "$work/vita-guest-stack-oracle.elf" |
        grep -Eq '[[:space:]]T[[:space:]]+guest_stack_bind$'; then
    echo "guest stack bind owner disappeared from ARM ELF" >&2
    exit 6
fi

# Native attribution is valid only if every generated owner uses a real BL (so
# LR is the exact final-ELF continuation) and no hot helper reads LR before a
# rejecting branch.  The fixed-LR local keeps the incoming value live across
# the validator without adding an instruction to the successful path.
for spec in \
    action_generated_set:guest_stack_set_generated \
    action_generated_adjust:guest_stack_adjust_generated \
    action_generated_access:guest_stack_address_generated \
    action_generated_push:gpush_generated \
    action_generated_pop:gpop_generated
do
    action=${spec%%:*}
    helper=${spec#*:}
    caller_disassembly="$work/$action.disassembly"
    helper_disassembly="$work/$helper.disassembly"
    call_line=$(grep -E \
        "^[[:space:]]*[0-9a-f]+:.*\\bbl\\b.*<$helper>" \
        "$caller_disassembly" || true)
    if [ "$(printf '%s\n' "$call_line" | grep -c .)" -ne 1 ]; then
        cat "$caller_disassembly" >&2
        echo "$action is not one direct BL to $helper" >&2
        exit 7
    fi
    call_hex=$(printf '%s\n' "$call_line" | sed -E \
        's/^[[:space:]]*([0-9a-f]+):.*/\1/')
    call_return=$(printf '%08x' "$((16#$call_hex + 4))")
    first_branch=$(grep -nE \
        '\b(cbz|cbnz|b(eq|ne|cs|cc|hi|ls|ge|lt|gt|le))\b' \
        "$helper_disassembly" | sed -n '1s/:.*//p')
    first_lr=$(grep -nE \
        '\bmov[^[:space:]]*[[:space:]]+r[0-9]+,[[:space:]]*lr\b' \
        "$helper_disassembly" | sed -n '1s/:.*//p')
    if [ -z "$first_branch" ] || [ -z "$first_lr" ] ||
       [ "$first_lr" -le "$first_branch" ]; then
        cat "$helper_disassembly" >&2
        echo "$helper reads LR on the successful hot prefix" >&2
        exit 8
    fi
    echo "$helper attribution: BL=0x$call_hex LR=0x$call_return; LR read is fault-dominated"
done

if ! grep -Eq '\bbic[^[:space:]]*[[:space:]].*,[[:space:]]*#1\b' \
        "$work/native-violation.disassembly"; then
    cat "$work/native-violation.disassembly" >&2
    echo "Thumb native return address is not normalized in the cold path" >&2
    exit 10
fi
echo "native attribution compiler: GCC $compiler_version sha256=$compiler_sha"
echo "Thumb LR normalization: cold BIC #1 present"

sha256sum \
    "$root/runtime/guest.h" \
    "$root/runtime/guest.c" \
    "$root/runtime/guest_stack_oracle.c" \
    "$root/vita/test_guest_stack.sh" \
    "$work/vita-guest-stack-oracle.elf"
echo "Vita guest stack ARMv7 Thumb softfp compile/link/static gate: PASS (same exhaustive oracle executes on PC; ARM ELF was not run)"
