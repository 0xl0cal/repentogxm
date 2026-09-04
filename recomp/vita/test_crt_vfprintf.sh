#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_CRT_VFPRINTF_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_CRT_VFPRINTF_TEST_OUT:-}" ]; then
    work=$ISAAC_CRT_VFPRINTF_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-crt-vfprintf.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
nm="$VITASDK/bin/arm-vita-eabi-nm"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"

common="-std=gnu11 -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-maybe-uninitialized -DGUEST_IMAGE_BASE=0x98000000u"
arm="$common -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb"
sources="$root/runtime/guest_stack_legacy_oracle_stub.c $root/runtime/vita_crt_vfprintf_oracle.c"
includes="-I$root/vita/qsort_oracle_include -I$root/runtime -I$root/vita"

ISAAC_FLOAT_FORMAT_TEST_ROOT="$root" \
ISAAC_FLOAT_FORMAT_TEST_OUT="$work/float-format" \
    bash "$root/vita/test_float_format.sh"

run_host_oracle() {
    compiler=$1
    suffix=$2
    sanitizer=${3:-}
    feature=${4:-}
    "$compiler" $common -fno-pie $sanitizer $feature $includes $sources \
        -Wl,--gc-sections -no-pie $sanitizer -lm \
        -o "$work/vita-crt-vfprintf-$suffix"
    if command -v timeout >/dev/null 2>&1; then
        timeout 20s "$work/vita-crt-vfprintf-$suffix"
    else
        "$work/vita-crt-vfprintf-$suffix"
    fi
}

host_cc=${CC:-cc}
run_host_oracle "$host_cc" host
run_host_oracle "$host_cc" host-log-batch "" \
    "-DISAAC_VITA_GAME_LOG_BATCH=1"

if command -v gcc >/dev/null 2>&1; then
    run_host_oracle gcc gcc-sanitize \
        "-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined" \
        "-DISAAC_VITA_GAME_LOG_BATCH=1"
fi
if command -v clang >/dev/null 2>&1; then
    run_host_oracle clang clang-sanitize \
        "-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -Wno-unknown-warning-option" \
        "-DISAAC_VITA_GAME_LOG_BATCH=1"
fi

"$cc" $arm -DISAAC_VITA_GAME_LOG_BATCH=1 \
    -I"$root/runtime" -I"$root/vita" $sources \
    -Wl,--gc-sections -lm -o "$work/vita-crt-vfprintf-softfp.elf"
"$cc" $arm -DISAAC_VITA_ARCHIVE_FILE_CACHE=1 \
    -DISAAC_VITA_GAME_LOG_BATCH=1 \
    -I"$root/runtime" -I"$root/vita" \
    -fstack-usage -c "$root/runtime/host_vita_crt.c" \
    -o "$work/host_vita_crt.production.o"

"$readelf" -h "$work/vita-crt-vfprintf-softfp.elf" > "$work/header"
"$readelf" -A "$work/vita-crt-vfprintf-softfp.elf" > "$work/attributes"
"$nm" -u "$work/host_vita_crt.production.o" > "$work/production.undefined"
"$nm" --defined-only "$work/host_vita_crt.production.o" \
    > "$work/production.defined"
cat "$work/header"
cat "$work/attributes"

if ! grep -q "soft-float ABI" "$work/header"; then
    echo "vfprintf oracle is not marked soft-float ABI" >&2
    exit 3
fi
if grep -q "Tag_ABI_VFP_args" "$work/attributes"; then
    echo "vfprintf oracle unexpectedly advertises hardfp arguments" >&2
    exit 4
fi
if grep -Eq \
        '[[:space:]]U[[:space:]]+([^[:space:]]*dtoa[^[:space:]]*|malloc|calloc|realloc|free|memalign|aligned_alloc|strdup|_malloc_r|_calloc_r|_realloc_r|_free_r|_Balloc|_Bfree|__d2b|snprintf|sprintf|vsnprintf|vsprintf)$' \
        "$work/production.undefined"; then
    cat "$work/production.undefined" >&2
    echo "production CRT object retains an allocator/printf/dtoa import" >&2
    exit 5
fi
if grep -Eq 'isaac_vita_musl_fp_(write|repeat|u32)$' \
        "$work/production.defined"; then
    cat "$work/production.defined" >&2
    echo "production CRT retained a supposedly always-inline float helper" >&2
    exit 5
fi
if ! grep -Eq '[[:space:]]U[[:space:]]+fwrite$' \
        "$work/production.undefined"; then
    cat "$work/production.undefined" >&2
    echo "production CRT object lost its bounded fwrite sink" >&2
    exit 6
fi

stack_file=$(find "$work" -maxdepth 1 -name '*.su' -print)
if [ -z "$stack_file" ] || [ "$(printf '%s\n' "$stack_file" | wc -l)" -ne 1 ]; then
    echo "expected exactly one ARM stack-usage report" >&2
    exit 7
fi
stack_record() {
    pattern=$1
    label=$2
    matches=$(grep "$pattern" "$stack_file" || true)
    if [ "$(printf '%s\n' "$matches" | sed '/^$/d' | wc -l)" -ne 1 ]; then
        echo "expected one $label ARM stack record, got: $matches" >&2
        exit 8
    fi
    bytes=$(printf '%s\n' "$matches" | awk -F '\t' '{print $2}')
    kind=$(printf '%s\n' "$matches" | awk -F '\t' '{print $3}')
    case "$bytes" in
        ''|*[!0-9]*)
            echo "unreadable $label ARM stack usage: $matches" >&2
            exit 8
            ;;
    esac
    if [ "$kind" != "static" ]; then
        echo "$label ARM stack is not static: $matches" >&2
        exit 9
    fi
    printf '%s\n' "$bytes"
}

common_stack=$(stack_record 'vita_crt_stdio_common_vfprintf' 'vfprintf')
vsprintf_stack=$(stack_record 'vita_crt_stdio_common_vsprintf[[:space:]]' 'vsprintf')
vsprintf_s_stack=$(stack_record 'vita_crt_stdio_common_vsprintf_s[[:space:]]' 'vsprintf_s')
format_stack=$(stack_record 'vita_crt_stdio_format' 'format')
double_stack=$(stack_record 'vita_crt_stdio_put_double' 'put_double')
formatter_stack=$(stack_record 'isaac_vita_musl_format_double' 'formatter')
vfprintf_deep=$((common_stack + format_stack + double_stack + formatter_stack))
vsprintf_deep=$((vsprintf_stack + format_stack + double_stack + formatter_stack))
vsprintf_s_deep=$((vsprintf_s_stack + format_stack + double_stack + formatter_stack))
deep_stack=$vfprintf_deep
if [ "$vsprintf_deep" -gt "$deep_stack" ]; then deep_stack=$vsprintf_deep; fi
if [ "$vsprintf_s_deep" -gt "$deep_stack" ]; then deep_stack=$vsprintf_s_deep; fi
if [ "$deep_stack" -gt 2048 ]; then
    echo "deep vfprintf float path exceeds 2048 ARM stack bytes: $deep_stack" >&2
    exit 9
fi

sha256sum \
    "$root/runtime/host_vita_crt.c" \
    "$root/runtime/vita_crt_vfprintf_oracle.c" \
    "$root/vita/test_crt_vfprintf.sh" \
    "$work/vita-crt-vfprintf-host" \
    "$work/vita-crt-vfprintf-host-log-batch" \
    "$work/vita-crt-vfprintf-softfp.elf" \
    "$work/host_vita_crt.production.o"
echo "Vita CRT vfprintf/vsprintf + exact INFO batching + sanitizer + allocator/printf/dtoa-import + softfp/deep-static-stack gate: PASS (ARM ELF was not executed; max project-owned static float-frame subtotal=$deep_stack bytes; vfprintf=$vfprintf_deep, vsprintf=$vsprintf_deep, vsprintf_s=$vsprintf_s_deep, shared=$format_stack+$double_stack+$formatter_stack; external frexp/libgcc and fwrite libc frames are outside this source/object claim)"
