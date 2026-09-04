#!/usr/bin/env bash
set -eu

root=${ISAAC_FLOAT_FORMAT_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_FLOAT_FORMAT_TEST_OUT:-}" ]; then
    work=$ISAAC_FLOAT_FORMAT_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-float-format.XXXXXX")
fi

source_file="$root/runtime/vita_float_format_oracle.c"
header="$root/runtime/third_party/musl_fmt_fp/musl_fmt_fp.h"
common="-std=gnu11 -O2 -Wall -Wextra -Werror"
wraps="-Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free"

run_oracle() {
    compiler=$1
    suffix=$2
    sanitizer=${3:-}
    "$compiler" $common $sanitizer \
        -DISAAC_VITA_FLOAT_FORMAT_ALLOC_WRAP=1 \
        "$source_file" $wraps -lm $sanitizer \
        -o "$work/vita-float-format-$suffix"
    if command -v timeout >/dev/null 2>&1; then
        timeout 90s "$work/vita-float-format-$suffix"
    else
        "$work/vita-float-format-$suffix"
    fi
}

host_cc=${CC:-cc}
run_oracle "$host_cc" host
if command -v gcc >/dev/null 2>&1; then
    run_oracle gcc gcc-sanitize \
        "-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined"
fi
if command -v clang >/dev/null 2>&1; then
    run_oracle clang clang-sanitize \
        "-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined"
fi

"$host_cc" $common -fno-builtin -fstack-usage \
    -DISAAC_VITA_FLOAT_FORMAT_IMPORT_PROBE=1 \
    -c "$source_file" -o "$work/vita-float-format-import-probe.o"
nm -u "$work/vita-float-format-import-probe.o" \
    > "$work/vita-float-format-import-probe.undefined"
nm --defined-only "$work/vita-float-format-import-probe.o" \
    > "$work/vita-float-format-import-probe.defined"
if grep -Eq \
        '[[:space:]]U[[:space:]]+([^[:space:]]*dtoa[^[:space:]]*|malloc|calloc|realloc|free|memalign|aligned_alloc|_malloc_r|_calloc_r|_realloc_r|_free_r|_Balloc|_Bfree|__d2b|snprintf|sprintf|vsnprintf|vsprintf)$' \
        "$work/vita-float-format-import-probe.undefined"; then
    cat "$work/vita-float-format-import-probe.undefined" >&2
    echo "float formatter retains a forbidden allocator/printf/dtoa import" >&2
    exit 3
fi
if grep -Eq 'isaac_vita_musl_fp_(write|repeat|u32)$' \
        "$work/vita-float-format-import-probe.defined"; then
    cat "$work/vita-float-format-import-probe.defined" >&2
    echo "float formatter retained a supposedly always-inline helper" >&2
    exit 3
fi

stack_files=$(find "$work" -maxdepth 1 -name '*.su' -print)
stack_file=$(printf '%s\n' "$stack_files" | head -n 1)
if [ -z "$stack_file" ] || [ "$(printf '%s\n' "$stack_files" | sed '/^$/d' | wc -l)" -ne 1 ]; then
    echo "expected exactly one float formatter stack report" >&2
    exit 4
fi
stack_line=$(grep 'isaac_vita_musl_format_double' "$stack_file")
stack_bytes=$(printf '%s\n' "$stack_line" | awk -F '\t' '{print $2}')
stack_kind=$(printf '%s\n' "$stack_line" | awk -F '\t' '{print $3}')
case "$stack_bytes" in
    ''|*[!0-9]*)
        echo "unreadable float formatter stack usage: $stack_line" >&2
        exit 5
        ;;
esac
if [ "$stack_kind" != "static" ] || [ "$stack_bytes" -gt 768 ]; then
    echo "float formatter stack is not bounded static <=768 bytes: $stack_line" >&2
    exit 6
fi

sha256sum "$source_file" "$header" \
    "$work/vita-float-format-host" \
    "$work/vita-float-format-import-probe.o"
echo "Vita allocation-free binary64 formatter differential/sanitizer/import/stack oracle: PASS (formatter project-owned static frame=$stack_bytes host bytes)"
