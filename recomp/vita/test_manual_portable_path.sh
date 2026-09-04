#!/usr/bin/env bash
set -euo pipefail

root=${ISAAC_MANUAL_PORTABLE_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_MANUAL_PORTABLE_TEST_OUT:-}" ]; then
    work=$ISAAC_MANUAL_PORTABLE_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-manual-portable.XXXXXX")
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
"$host_gcc" --version > "$work/gcc.version"
"$host_clang" --version > "$work/clang.version"
"$host_gcc" -dM -E -x c /dev/null > "$work/gcc.macros"
"$host_clang" -dM -E -x c /dev/null > "$work/clang.macros"
if ! grep -q '^#define __GNUC__ ' "$work/gcc.macros" ||
   grep -q '^#define __clang__ ' "$work/gcc.macros"; then
    echo "HOST_CC is not a genuine GCC preprocessor backend" >&2
    exit 2
fi
if ! grep -q '^#define __clang__ ' "$work/clang.macros"; then
    echo "HOST_CLANG is not a genuine Clang preprocessor backend" >&2
    exit 2
fi
printf 'Host compiler backends: GCC=%s; Clang=%s\n' \
    "$(sed -n '1p' "$work/gcc.version")" \
    "$(sed -n '1p' "$work/clang.version")"

source_file="$root/runtime/manual_portable.c"
oracle_file="$root/runtime/manual_portable_path_oracle.c"
stack_stub="$root/runtime/manual_portable_path_stack_oracle.c"
oracle_include="$root/vita/manual_portable_oracle_include"

if grep -En \
     '\<(malloc|calloc|realloc|free|aligned_alloc|strdup)[[:space:]]*\(' \
     "$source_file" > "$work/source.raw-allocators"; then
    cat "$work/source.raw-allocators" >&2
    echo "production manual portable source retained a raw allocator call" >&2
    exit 3
fi

host_common=(
    -std=gnu11 -O1 -g -Wall -Wextra -Werror
    -fno-omit-frame-pointer -fno-pie
    -ffunction-sections -fdata-sections
    -fsanitize=address,undefined
    -DGUEST_STACK_REQUIRED=1
    -I"$oracle_include" -I"$root/runtime"
)
export ASAN_OPTIONS='detect_leaks=1:halt_on_error=1'
export UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'

for compiler in "$host_gcc" "$host_clang"; do
    tag=$(basename "$compiler")
    binary="$work/manual-portable-path-$tag"
    object="$work/manual-portable-production-$tag.o"
    "$compiler" "${host_common[@]}" \
        "$source_file" "$stack_stub" "$oracle_file" \
        -Wl,--gc-sections -no-pie -o "$binary"
    if command -v timeout >/dev/null 2>&1; then
        timeout 90s "$binary"
    else
        "$binary"
    fi

    "$compiler" -std=gnu11 -O2 -Wall -Wextra -Werror \
        -DGUEST_STACK_REQUIRED=1 \
        -ffunction-sections -fdata-sections \
        -I"$oracle_include" -I"$root/runtime" \
        -c "$source_file" -o "$object"
    nm -u "$object" > "$object.undefined"
    if grep -Eq \
         '[[:space:]]U[[:space:]]+(malloc|calloc|realloc|free|aligned_alloc|strdup)$' \
         "$object.undefined"; then
        cat "$object.undefined" >&2
        echo "production manual portable object retained a raw allocator import" >&2
        exit 4
    fi
    if ! nm "$object" | grep -Eq \
         '[[:space:]]T[[:space:]]+sub_002599c0$'; then
        echo "production object lost the PathFromBase handler" >&2
        exit 5
    fi
done

arm_status=SKIP
if [ -n "${VITASDK:-}" ] &&
   [ -x "$VITASDK/bin/arm-vita-eabi-gcc" ]; then
    cc="$VITASDK/bin/arm-vita-eabi-gcc"
    nm_arm="$VITASDK/bin/arm-vita-eabi-nm"
    readelf="$VITASDK/bin/arm-vita-eabi-readelf"
    arm_flags=(
        -std=gnu11 -mcpu=cortex-a9 -mfpu=neon
        -mfloat-abi=softfp -mthumb -ffunction-sections -fdata-sections
        -Wall -Wextra -Werror
        -I"$oracle_include" -I"$root/runtime"
    )
    arm_object="$work/manual-portable-production.arm.o"
    arm_elf="$work/manual-portable-path.arm.elf"
    "$cc" "${arm_flags[@]}" -O2 -DGUEST_STACK_REQUIRED=1 -fstack-usage \
        -c "$source_file" -o "$arm_object"
    "$readelf" -h "$arm_object" > "$arm_object.header"
    "$readelf" -A "$arm_object" > "$arm_object.attributes"
    "$nm_arm" -u "$arm_object" > "$arm_object.undefined"
    grep -q 'Machine:[[:space:]]*ARM$' "$arm_object.header"
    grep -q 'Version5 EABI' "$arm_object.header"
    if grep -q 'Tag_ABI_VFP_args' "$arm_object.attributes"; then
        echo "manual portable ARM object gained a hardfp VFP-args tag" >&2
        exit 6
    fi
    if grep -Eq \
         '[[:space:]]U[[:space:]]+(malloc|calloc|realloc|free|aligned_alloc|strdup)$' \
         "$arm_object.undefined"; then
        cat "$arm_object.undefined" >&2
        echo "manual portable ARM object retained a raw allocator import" >&2
        exit 7
    fi
    stack_usage=${arm_object%.o}.su
    path_frame=$(awk -F '\t' '
        $1 ~ /(path_from_base|sub_002599c0)/ && $2 + 0 > maximum {
            maximum = $2 + 0
        }
        END { print maximum + 0 }
    ' "$stack_usage")
    if [ "$path_frame" -le 0 ] || [ "$path_frame" -gt 4096 ]; then
        cat "$stack_usage" >&2
        echo "manual portable PathFromBase ARM frame is absent or exceeds 4096 bytes" >&2
        exit 8
    fi

    "$cc" "${arm_flags[@]}" -O1 -DGUEST_STACK_REQUIRED=1 \
        "$source_file" "$stack_stub" "$oracle_file" \
        -Wl,--gc-sections -o "$arm_elf"
    "$readelf" -h "$arm_elf" > "$arm_elf.header"
    "$readelf" -A "$arm_elf" > "$arm_elf.attributes"
    grep -q 'Version5 EABI, soft-float ABI' "$arm_elf.header"
    if grep -q 'Tag_ABI_VFP_args' "$arm_elf.attributes"; then
        echo "manual portable ARM oracle gained a hardfp VFP-args tag" >&2
        exit 9
    fi
    printf 'Manual portable PathFromBase ARM bounded frame: %s bytes\n' \
        "$path_frame"
    arm_status=PASS
else
    echo "Manual portable PathFromBase ARM softfp gate: SKIP (VITASDK unavailable)"
fi

sha256sum \
    "$root/runtime/manual_portable.c" \
    "$root/runtime/manual_portable_path_oracle.c" \
    "$root/runtime/manual_portable_path_stack_oracle.c" \
    "$root/vita/manual_portable_oracle_include/guest_funcs.h" \
    "$root/vita/manual_portable_oracle_include/guest_coverage_generated.h" \
    "$root/vita/test_manual_portable_path.sh"
echo "Manual portable allocation-free PathFromBase GCC/Clang ASan/UBSan differential gate: PASS; raw allocator imports: 0; ARM softfp: $arm_status"
