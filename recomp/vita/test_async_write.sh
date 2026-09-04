#!/usr/bin/env bash
set -eu

root=${ISAAC_ASYNC_WRITE_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_ASYNC_WRITE_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-async-write.XXXXXX")}
host_cc=${CC:-cc}
flags="-std=gnu11 -O2 -Wall -Wextra -Werror -DISAAC_VITA_ASYNC_WRITE_ORACLE=1 -DISAAC_VITA_ASYNC_WRITE_BUILD_ID=\"perf:async-write-oracle\""

mkdir -p "$work"

# The writer module alone: image semantics, FIFO, read-your-writes waits,
# lane exhaustion, synchronous fallback and shutdown.
"$host_cc" $flags -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_async_write.c" \
    "$root/runtime/host_vita_async_write_oracle.c" \
    -o "$work/async-write-host-oracle"

if command -v timeout >/dev/null 2>&1; then
    timeout 20s "$work/async-write-host-oracle"
else
    "$work/async-write-host-oracle"
fi

# Byte-fidelity: replay the frozen GameState::Save call shape (16-byte header,
# thousands of 1/2/4-byte field writes, an interior length-patch seek-back and a
# seek-past-EOF sparse gap, trailing checksum) for the reported 65,943-byte save
# and the 64 KiB boundary through the image AND a genuine newlib FILE, and
# assert the two files are byte-identical.  This is the necessary condition for
# the game to accept the async-written run.
"$host_cc" $flags -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_async_write.c" \
    "$root/runtime/host_vita_async_write_fidelity_oracle.c" \
    -o "$work/async-write-fidelity-oracle"
if command -v timeout >/dev/null 2>&1; then
    timeout 20s "$work/async-write-fidelity-oracle"
else
    "$work/async-write-fidelity-oracle"
fi

# The CRT dispatch: include the production CRT TU (as test_crt_raw_archive.sh
# does) and drive fopen/fwrite/fseek/ftell/_fileno/fflush/fread/vfprintf/
# fclose through guest-ABI calls against the writer's oracle configuration.
crt_common="-std=gnu11 -O2 -fno-pie -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-maybe-uninitialized -DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_ASYNC_WRITE_ORACLE=1"
crt_includes="-I$root/vita/qsort_oracle_include -I$root/runtime -I$root/vita"
"$host_cc" $crt_common $crt_includes \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/host_vita_async_write.c" \
    "$root/runtime/host_vita_crt_async_write_oracle.c" \
    -Wl,--gc-sections -no-pie \
    -o "$work/crt-async-write-host-oracle"
if command -v timeout >/dev/null 2>&1; then
    timeout 20s "$work/crt-async-write-host-oracle"
else
    "$work/crt-async-write-host-oracle"
fi

# The production CMake scope: the writer macro reaches exactly the CRT FILE
# owner, the four path-based import owners and the writer itself; no
# generated unit and no GL/KAGE owner sees it.
cmake="$root/vita/CMakeLists.txt"
grep -q '^option(ISAAC_VITA_ASYNC_SAVE_WRITE$' "$cmake"
grep -q 'ISAAC_VITA_ASYNC_SAVE_WRITE requires ISAAC_VITA_ARCHIVE_FILE_CACHE=ON' "$cmake"
block=$(awk '/^  if\(ISAAC_VITA_ASYNC_SAVE_WRITE\)$/,/^  endif\(\)$/' "$cmake")
for owner in host_vita_crt.c host_vita_filesystem.c host_vita_startup.c \
             host_vita_find.c host_vita_async_write.c; do
    printf '%s\n' "$block" | grep -q "\${ISAAC_RUNTIME}/$owner\"" \
        || { echo "async save writer CMake scope lost $owner" >&2; exit 1; }
done
if printf '%s\n' "$block" | grep -q 'ISAAC_GENERATED\|gl_vita_backend\|kage_vita'; then
    echo "async save writer CMake scope leaked beyond the CRT owners" >&2
    exit 1
fi
# The raw allocator gate must know the optional source so the fail-closed
# object closure accepts the ON build.
grep -q '("ISAAC_VITA_ASYNC_SAVE_WRITE", "host_vita_async_write.c")' \
    "$root/vita/vita_raw_allocator_gate.py"

sha256sum \
    "$root/runtime/host_vita_async_write.h" \
    "$root/runtime/host_vita_async_write.c" \
    "$root/runtime/host_vita_async_write_oracle.c" \
    "$root/runtime/host_vita_async_write_fidelity_oracle.c" \
    "$root/runtime/host_vita_crt_async_write_oracle.c" \
    "$root/runtime/host_vita_crt.c" \
    "$root/vita/test_async_write.sh" \
    "$work/async-write-host-oracle" \
    "$work/async-write-fidelity-oracle" \
    "$work/crt-async-write-host-oracle"
echo "Vita asynchronous save writer host behavior: PASS"
