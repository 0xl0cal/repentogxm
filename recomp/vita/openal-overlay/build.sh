#!/usr/bin/env bash
set -euo pipefail

version=1.19.1
source_sha=9f3536ab2bb7781dbafabc6a61e0b34b17edd16bd6c2eaf2ae71bc63078f98c7
vita_patch_commit=27239f784496a5188fc5350c8c3c74a30ec98b98
vita_patch_sha=a31ebc39966298e31d2d77d43e9ac4352bd1b0a0f6d9f0f0e372a849bca349bf

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
voice_patch="$script_dir/0001-limit-initial-voices.patch"
redirect_patch="$script_dir/0002-route-direct-allocators.patch"
redirect_header="$script_dir/openal_pool_redirect.h"
archive_verifier="$script_dir/verify_deterministic_archive.py"
runtime_dir=$(CDPATH= cd -- "$script_dir/../../runtime" && pwd)
pool_header="$runtime_dir/host_vita_openal_pool.h"

for required in \
    "$voice_patch" "$redirect_patch" "$redirect_header" \
    "$archive_verifier" "$pool_header"; do
    if [ ! -f "$required" ]; then
        echo "required OpenAL overlay input is missing: $required" >&2
        exit 2
    fi
done

if [ "$#" -ne 1 ]; then
    echo "usage: $0 OUTPUT_ROOT" >&2
    exit 2
fi
if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

case "$1" in
    /*) output_root=$1 ;;
    *) output_root=$(pwd)/$1 ;;
esac
case "$output_root" in
    /|/home|/home/*/..)
        echo "refusing unsafe OpenAL overlay output root: $output_root" >&2
        exit 2
        ;;
esac

source_url="https://github.com/kcat/openal-soft/archive/refs/tags/openal-soft-$version.tar.gz"
vita_patch_url="https://raw.githubusercontent.com/isage/openal-soft/$vita_patch_commit/openal-soft-$version-vita-1.patch"
downloads="$output_root/downloads"
work="$output_root/work"
prefix="$output_root/prefix"
source_archive="$downloads/openal-soft-$version.tar.gz"
vita_patch="$downloads/openal-soft-$version-vita-1.patch"

mkdir -p "$downloads" "$work" "$prefix"

verify_sha()
{
    actual=$(sha256sum "$1" | awk '{print $1}')
    if [ "$actual" != "$2" ]; then
        echo "SHA-256 mismatch for $1: $actual != $2" >&2
        exit 3
    fi
}

download()
{
    url=$1
    destination=$2
    expected=$3
    if [ -f "$destination" ]; then
        verify_sha "$destination" "$expected"
        return
    fi

    partial="$destination.part.$$"
    trap 'rm -f -- "$partial"' EXIT HUP INT TERM
    curl --fail --location --retry 3 --output "$partial" "$url"
    verify_sha "$partial" "$expected"
    mv -- "$partial" "$destination"
    trap - EXIT HUP INT TERM
}

download "$source_url" "$source_archive" "$source_sha"
download "$vita_patch_url" "$vita_patch" "$vita_patch_sha"

voice_patch_sha=$(sha256sum "$voice_patch" | awk '{print $1}')
redirect_patch_sha=$(sha256sum "$redirect_patch" | awk '{print $1}')
redirect_header_sha=$(sha256sum "$redirect_header" | awk '{print $1}')
archive_verifier_sha=$(sha256sum "$archive_verifier" | awk '{print $1}')
pool_header_sha=$(sha256sum "$pool_header" | awk '{print $1}')
allocator_contract=direct-openal-allocators-v1
archive_contract=gnu-ar-deterministic-v1
recipe=$(printf '%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n' \
    "$source_sha" "$vita_patch_sha" "$voice_patch_sha" \
    "$redirect_patch_sha" "$redirect_header_sha" "$archive_verifier_sha" \
    "$pool_header_sha" "$allocator_contract" "$archive_contract" | sha256sum | \
    awk '{print $1}')
source_parent="$work/source-$recipe"
source_dir="$source_parent/openal-soft-openal-soft-$version"
build_dir="$work/build-$recipe"
marker="$source_parent/.isaac-openal-recipe"

if [ ! -f "$marker" ]; then
    if [ -e "$source_parent" ]; then
        echo "unmarked OpenAL source directory already exists: $source_parent" >&2
        exit 4
    fi
    mkdir -p "$source_parent"
    tar -xf "$source_archive" -C "$source_parent"
    patch --directory="$source_dir" --strip=1 --input="$vita_patch"
    patch --directory="$source_dir" --strip=1 --input="$voice_patch"
    patch --fuzz=0 --directory="$source_dir" --strip=1 \
        --input="$redirect_patch"
    printf '%s\n' "$recipe" > "$marker"
fi
if [ "$(sed -n '1p' "$marker")" != "$recipe" ]; then
    echo "OpenAL source recipe marker mismatch: $marker" >&2
    exit 4
fi
if ! grep -Fq \
        'AllocateVoices(ALContext, mini(256, device->SourcesMax),' \
        "$source_dir/Alc/ALc.c"; then
    echo "bounded initial voice-pool patch is absent" >&2
    exit 5
fi

archive_create_rule='<CMAKE_AR> qcD <TARGET> <LINK_FLAGS> <OBJECTS>'
archive_append_rule='<CMAKE_AR> qD <TARGET> <LINK_FLAGS> <OBJECTS>'
archive_finish_rule='<CMAKE_RANLIB> -D <TARGET>'
cmake -S "$source_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    "-DCMAKE_C_ARCHIVE_CREATE:STRING=$archive_create_rule" \
    "-DCMAKE_C_ARCHIVE_APPEND:STRING=$archive_append_rule" \
    "-DCMAKE_C_ARCHIVE_FINISH:STRING=$archive_finish_rule" \
    -DCMAKE_C_FLAGS="-std=gnu11 -Wno-error=implicit-function-declaration -Wno-error=int-conversion" \
    -DCMAKE_CXX_FLAGS="-std=gnu++11 -Wno-error=implicit-function-declaration -Wno-error=int-conversion" \
    -DISAAC_VITA_OPENAL_POOL_REDIRECT_HEADER:FILEPATH="$redirect_header" \
    -DISAAC_VITA_OPENAL_POOL_RUNTIME_DIR:PATH="$runtime_dir" \
    -DLIBTYPE=STATIC \
    -DALSOFT_UTILS=OFF \
    -DALSOFT_EXAMPLES=OFF \
    -DALSOFT_TESTS=OFF \
    -DALSOFT_CONFIG=OFF \
    -DALSOFT_HRTF_DEFS=OFF \
    -DALSOFT_AMBDEC_PRESETS=OFF

for cache_entry in \
    "CMAKE_C_ARCHIVE_CREATE:STRING=$archive_create_rule" \
    "CMAKE_C_ARCHIVE_APPEND:STRING=$archive_append_rule" \
    "CMAKE_C_ARCHIVE_FINISH:STRING=$archive_finish_rule"; do
    if ! grep -Fxq "$cache_entry" "$build_dir/CMakeCache.txt"; then
        echo "OpenAL deterministic archive rule is absent: $cache_entry" >&2
        exit 5
    fi
done
python3 -B - \
    "$build_dir/CMakeFiles/rules.ninja" \
    "$VITASDK/bin/arm-vita-eabi-ar" \
    "$VITASDK/bin/arm-vita-eabi-ranlib" <<'PY'
from pathlib import Path
import sys

rules_path = Path(sys.argv[1])
ar, ranlib = sys.argv[2:]
rules = rules_path.read_text(encoding="utf-8")
marker = "rule C_STATIC_LIBRARY_LINKER__OpenAL_Release\n"
if rules.count(marker) != 1:
    raise SystemExit(
        f"expected exactly one generated OpenAL archive rule in {rules_path}"
    )
block = rules.split(marker, 1)[1].split("\n\n", 1)[0]
expected = (
    "  command = $PRE_LINK && /usr/bin/cmake -E rm -f $TARGET_FILE && "
    f"{ar} qcD $TARGET_FILE $LINK_FLAGS $in && "
    f"{ranlib} -D $TARGET_FILE && $POST_BUILD"
)
commands = [line for line in block.splitlines() if line.startswith("  command = ")]
if commands != [expected]:
    raise SystemExit(
        "generated OpenAL archive command is not exact deterministic "
        f"CREATE/FINISH rule: {commands!r}"
    )
print("OpenAL generated archive rule: PASS (qcD; ranlib -D)")
PY
if ! grep -Fxq 'HAVE_ALIGNED_ALLOC:INTERNAL=1' "$build_dir/CMakeCache.txt"; then
    echo "OpenAL aligned_alloc feature probe was changed by the redirect" >&2
    exit 5
fi
python3 - "$build_dir/compile_commands.json" "$redirect_header" <<'PY'
import json
import shlex
import sys

commands_path, redirect_header = sys.argv[1:]
with open(commands_path, "r", encoding="utf-8") as stream:
    entries = json.load(stream)

openal_count = 0
for entry in entries:
    tokens = entry.get("arguments")
    if tokens is None:
        tokens = shlex.split(entry["command"])
    is_openal = any("CMakeFiles/OpenAL.dir" in token for token in tokens)
    forced = [
        index for index, token in enumerate(tokens)
        if token == "-include" and index + 1 < len(tokens) and
        tokens[index + 1] == redirect_header
    ]
    mentions = [token for token in tokens if token == redirect_header]
    if is_openal:
        openal_count += 1
        if len(forced) != 1 or len(mentions) != 1:
            raise SystemExit(
                f"OpenAL compile does not force exactly one redirect: {entry['file']}")
    elif forced or mentions:
        raise SystemExit(
            f"allocator redirect leaked outside OpenAL target: {entry['file']}")
if openal_count < 50:
    raise SystemExit(f"unexpected OpenAL compile-command count: {openal_count}")
print(f"OpenAL allocator redirect scope: PASS ({openal_count} target sources)")
PY

jobs=${ISAAC_OPENAL_JOBS:-}
if [ -z "$jobs" ]; then
    jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
fi
cmake --build "$build_dir" --target install --parallel "$jobs"

library="$prefix/lib/libopenal.a"
if [ ! -s "$library" ]; then
    echo "patched OpenAL archive was not installed: $library" >&2
    exit 6
fi

undefined="$output_root/libopenal.undefined"
undefined_names="$output_root/libopenal.undefined.names"
nm_tool="$VITASDK/bin/arm-vita-eabi-nm"
"$nm_tool" -A -u "$library" > "$undefined"
awk '{ print $NF }' "$undefined" > "$undefined_names"
if grep -Eq '^(malloc|calloc|realloc|free|aligned_alloc|strdup)$' \
        "$undefined_names"; then
    echo "patched OpenAL archive retained a raw direct allocator import" >&2
    grep -E ' (malloc|calloc|realloc|free|aligned_alloc|strdup)$' \
        "$undefined" >&2 || true
    exit 7
fi
for symbol in \
    isaac_vita_openal_pool_malloc \
    isaac_vita_openal_pool_calloc \
    isaac_vita_openal_pool_realloc \
    isaac_vita_openal_pool_free \
    isaac_vita_openal_pool_aligned_alloc \
    isaac_vita_openal_pool_strdup; do
    if ! grep -Fxq "$symbol" "$undefined_names"; then
        echo "patched OpenAL archive does not import $symbol" >&2
        exit 7
    fi
done
archive_receipt="$output_root/libopenal.archive.json"
archive_receipt_partial="$archive_receipt.part.$$"
trap 'rm -f -- "$archive_receipt_partial"' EXIT HUP INT TERM
python3 -B "$archive_verifier" --expected-regular-members 53 "$library" \
    > "$archive_receipt_partial"
mv -- "$archive_receipt_partial" "$archive_receipt"
trap - EXIT HUP INT TERM
printf '%s\n' "$recipe" > "$output_root/recipe.txt"
sha256sum "$library" > "$output_root/libopenal.sha256"
echo "Patched Vita OpenAL overlay: $library"
