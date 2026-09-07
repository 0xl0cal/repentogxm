#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_IO_PROFILE_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_IO_PROFILE_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-io-profile.XXXXXX")}
mkdir -p "$work"
host_cc=${CC:-cc}
cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
profile_build_id=profile:oracle:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdefx
if [ "${#profile_build_id}" -ne 96 ]; then
    echo "I/O profile oracle build id is not the intended 96 characters" >&2
    exit 2
fi
flags="-std=gnu11 -O2 -Wall -Wextra -Werror -DISAAC_VITA_IO_PROFILE_ORACLE=1 -DISAAC_VITA_IO_PROFILE=1 -DISAAC_VITA_IO_PROFILE_BUILD_ID=\"$profile_build_id\" -I$root/runtime"
arm_flags="$flags -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -ffunction-sections -fdata-sections"

"$host_cc" $flags \
    "$root/runtime/kage_vita_io_profile.c" \
    "$root/runtime/kage_vita_io_profile_oracle.c" \
    -o "$work/kage-vita-io-profile-host-oracle"
"$work/kage-vita-io-profile-host-oracle"

window_flags="-std=gnu11 -O2 -Wall -Wextra -Werror -DISAAC_VITA_IO_PROFILE_ORACLE=1 -DISAAC_VITA_IO_WINDOW_PROFILE=1 -I$root/runtime"
"$host_cc" $window_flags \
    "$root/runtime/kage_vita_io_profile_oracle.c" \
    -o "$work/kage-vita-io-window-host-oracle"
"$work/kage-vita-io-window-host-oracle"

shadow_flags="-std=gnu11 -O2 -Wall -Wextra -Werror -Wno-maybe-uninitialized -fno-pie -ffunction-sections -fdata-sections -DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_IO_PROFILE=1 -DISAAC_VITA_ARCHIVE_FILE_CACHE=1 -DISAAC_VITA_CRT_RAW_ARCHIVE_ORACLE=1 -DISAAC_VITA_CRT_QSORT_HOST_ORACLE=1 -I$root/vita/qsort_oracle_include -I$root/runtime -I$root/vita"
"$host_cc" $shadow_flags \
    "$root/runtime/host_vita_archive_cache.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" -x c - \
    -Wl,--gc-sections -no-pie \
    -o "$work/kage-vita-io-shadow-host-oracle" <<'EOF'
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host_vita_crt.c"

/* The raw-archive path is not exercised here; ISAAC_VITA_CRT_RAW_ARCHIVE_ORACLE
 * only keeps the Vita I/O headers out of this host build. */
int32_t isaac_vita_crt_raw_archive_oracle_open(const char *path)
{
    (void)path;
    return -1;
}

int32_t isaac_vita_crt_raw_archive_oracle_pread(
    int32_t descriptor, void *buffer, uint32_t size, uint64_t offset)
{
    (void)descriptor;
    (void)buffer;
    (void)size;
    (void)offset;
    return -1;
}

int32_t isaac_vita_crt_raw_archive_oracle_get_size(
    int32_t descriptor, int64_t *size)
{
    (void)descriptor;
    (void)size;
    return -1;
}

int32_t isaac_vita_crt_raw_archive_oracle_close(int32_t descriptor)
{
    (void)descriptor;
    return -1;
}

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Vita I/O shadow oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int counters_equal(uint32_t c8, uint32_t c16,
                          uint32_t c32, uint32_t c64)
{
    return s_io_profile.shadow_read_calls[
               ISAAC_VITA_CRT_IO_SHADOW_8K_INDEX] == c8 &&
           s_io_profile.shadow_read_calls[
               ISAAC_VITA_CRT_IO_SHADOW_16K_INDEX] == c16 &&
           s_io_profile.shadow_read_calls[
               ISAAC_VITA_CRT_IO_SHADOW_32K_INDEX] == c32 &&
           s_io_profile.shadow_read_calls[
               ISAAC_VITA_CRT_IO_SHADOW_64K_INDEX] == c64;
}

int main(void)
{
    vita_crt_io_shadow_file file;
    vita_crt_io_shadow_file retained;
    isaac_vita_crt_io_profile_snapshot snapshot;
    isaac_vita_archive_diag_event archive_event;

    /* Exact narrow lane: full fread plus successful SEEK_SET only. */
    memset(&s_io_profile, 0, sizeof s_io_profile);
    vita_crt_io_shadow_clear(&file);
    vita_crt_io_shadow_open(&file, NULL, 0, 1);
    vita_crt_io_shadow_seek(&file, 1536, SEEK_SET);
    CHECK(counters_equal(0u, 0u, 0u, 0u));
    CHECK(file.seek_optimized == 1u && file.position == 1536u);
    vita_crt_io_shadow_seek(&file, 2560, SEEK_SET);
    CHECK(counters_equal(1u, 1u, 1u, 1u));
    vita_crt_io_shadow_seek(&file, 2600, SEEK_SET);
    CHECK(counters_equal(1u, 1u, 1u, 1u));
    vita_crt_io_shadow_seek(&file, 71680, SEEK_SET);
    CHECK(file.buffer_valid_mask == 0u);
    CHECK(counters_equal(1u, 1u, 1u, 1u));
    vita_crt_io_shadow_read(&file, 8000u, 8000u);
    vita_crt_io_shadow_read(&file, 9000u, 9000u);
    CHECK(counters_equal(4u, 3u, 2u, 2u));
    CHECK(file.position == 88680u && file.model_valid == 1u);
    CHECK(s_io_profile.shadow_fseek_set_calls == 4u);
    CHECK(s_io_profile.shadow_fread_calls == 2u);
    CHECK(s_io_profile.shadow_fread_requested_bytes == 17000u);
    CHECK(s_io_profile.shadow_fread_returned_bytes == 17000u);
    vita_crt_io_shadow_close(&file);

    /* A failed seek may already have disturbed newlib's input buffer.  It is
     * therefore a permanent model boundary even when a later SET succeeds. */
    memset(&s_io_profile, 0, sizeof s_io_profile);
    vita_crt_io_shadow_clear(&file);
    vita_crt_io_shadow_open(&file, NULL, 0, 1);
    vita_crt_io_shadow_read(&file, 100u, 100u);
    vita_crt_io_shadow_seek_failed(&file, SEEK_SET);
    vita_crt_io_shadow_seek(&file, 0, SEEK_SET);
    vita_crt_io_shadow_read(&file, 10u, 10u);
    CHECK(file.model_valid == 0u);
    CHECK(counters_equal(1u, 1u, 1u, 1u));
    CHECK(s_io_profile.shadow_fseek_set_calls == 2u);
    CHECK(s_io_profile.shadow_unmodelled_seeks == 2u);
    CHECK(s_io_profile.shadow_unmodelled_fread_calls == 1u);

    /* Unsupported origins invalidate permanently and stay visible in the
     * unmodelled coverage counters. */
    memset(&s_io_profile, 0, sizeof s_io_profile);
    vita_crt_io_shadow_clear(&file);
    vita_crt_io_shadow_open(&file, NULL, 0, 1);
    vita_crt_io_shadow_read(&file, 100u, 100u);
    vita_crt_io_shadow_seek(&file, 1000, SEEK_CUR);
    vita_crt_io_shadow_read(&file, 50u, 50u);
    vita_crt_io_shadow_seek(&file, 0, SEEK_SET);
    vita_crt_io_shadow_read(&file, 20u, 20u);
    vita_crt_io_shadow_seek(&file, -1, SEEK_END);
    CHECK(file.model_valid == 0u);
    CHECK(counters_equal(1u, 1u, 1u, 1u));
    CHECK(s_io_profile.shadow_fseek_cur_calls == 1u);
    CHECK(s_io_profile.shadow_fseek_set_calls == 1u);
    CHECK(s_io_profile.shadow_fseek_end_calls == 1u);
    CHECK(s_io_profile.shadow_unmodelled_seeks == 3u);
    CHECK(s_io_profile.shadow_unmodelled_fread_calls == 2u);
    CHECK(s_io_profile.shadow_unmodelled_fread_bytes == 70u);

    /* A short/EOF fread cannot reveal how many counterfactual refills were
     * short, so it invalidates instead of guessing and SET cannot recover. */
    memset(&s_io_profile, 0, sizeof s_io_profile);
    vita_crt_io_shadow_clear(&file);
    vita_crt_io_shadow_open(&file, NULL, 0, 1);
    vita_crt_io_shadow_read(&file, 100u, 90u);
    vita_crt_io_shadow_seek(&file, 0, SEEK_SET);
    vita_crt_io_shadow_read(&file, 10u, 10u);
    CHECK(counters_equal(0u, 0u, 0u, 0u));
    CHECK(s_io_profile.shadow_partial_fread_calls == 1u);
    CHECK(s_io_profile.shadow_partial_fread_requested_bytes == 100u);
    CHECK(s_io_profile.shadow_partial_fread_returned_bytes == 90u);
    CHECK(s_io_profile.shadow_unmodelled_seeks == 1u);
    CHECK(s_io_profile.shadow_unmodelled_fread_calls == 2u);
    CHECK(s_io_profile.shadow_unmodelled_fread_bytes == 110u);

    /* A known cache hit replays its internal rewind; missing retained state
     * and a failed 16 KiB setvbuf remain selected but invalid. */
    memset(&s_io_profile, 0, sizeof s_io_profile);
    vita_crt_io_shadow_clear(&file);
    vita_crt_io_shadow_open(&file, NULL, 0, 1);
    vita_crt_io_shadow_seek(&file, 0, SEEK_SET);
    vita_crt_io_shadow_read(&file, 100u, 100u);
    vita_crt_io_shadow_close(&file);
    retained = file;
    vita_crt_io_shadow_open(&file, &retained, 1, 0);
    CHECK(file.position == 0u && file.buffer_valid_mask == 15u);
    vita_crt_io_shadow_read(&file, 100u, 100u);
    vita_crt_io_shadow_close(&file);
    CHECK(counters_equal(1u, 1u, 1u, 1u));
    vita_crt_io_shadow_clear(&retained);
    vita_crt_io_shadow_open(&file, &retained, 1, 0);
    CHECK(file.selected == 1u && file.model_valid == 0u);
    vita_crt_io_shadow_read(&file, 10u, 10u);
    vita_crt_io_shadow_open(&file, NULL, 0, 0);
    vita_crt_io_shadow_read(&file, 10u, 10u);
    CHECK(s_io_profile.shadow_cacheable_fopen_calls == 4u);
    CHECK(s_io_profile.shadow_cacheable_fclose_calls == 2u);
    CHECK(s_io_profile.shadow_invalid_cache_hits == 1u);
    CHECK(s_io_profile.shadow_setvbuf_failures == 1u);
    CHECK(s_io_profile.shadow_unmodelled_fread_calls == 2u);
    CHECK(s_io_profile.shadow_unmodelled_fread_bytes == 20u);
    CHECK(s_io_profile.shadow_read_requested_bytes[
              ISAAC_VITA_CRT_IO_SHADOW_8K_INDEX] == 8192u);
    CHECK(s_io_profile.shadow_read_requested_bytes[
              ISAAC_VITA_CRT_IO_SHADOW_16K_INDEX] == 16384u);
    CHECK(s_io_profile.shadow_read_requested_bytes[
              ISAAC_VITA_CRT_IO_SHADOW_32K_INDEX] == 32768u);
    CHECK(s_io_profile.shadow_read_requested_bytes[
              ISAAC_VITA_CRT_IO_SHADOW_64K_INDEX] == 65536u);

    /* Archive fflush is deliberately outside the exact model. */
    memset(&s_io_profile, 0, sizeof s_io_profile);
    vita_crt_io_shadow_clear(&file);
    vita_crt_io_shadow_open(&file, NULL, 0, 1);
    vita_crt_io_shadow_read(&file, 100u, 100u);
    vita_crt_io_shadow_fflush(&file);
    vita_crt_io_shadow_read(&file, 10u, 10u);
    CHECK(file.model_valid == 0u);
    CHECK(s_io_profile.shadow_fflush_calls == 1u);
    CHECK(s_io_profile.shadow_unmodelled_fflush_calls == 1u);
    CHECK(counters_equal(1u, 1u, 1u, 1u));
    vita_crt_io_shadow_read(&file, 10u, 10u);
    CHECK(counters_equal(1u, 1u, 1u, 1u));
    CHECK(s_io_profile.shadow_unmodelled_fread_calls == 2u);
    s_io_shadow_idle = file;
    vita_crt_io_shadow_fflush_all();
    CHECK(s_io_profile.shadow_fflush_all_calls == 1u);
    CHECK(s_io_profile.shadow_unmodelled_fflush_calls == 2u);

    /* A live snapshot retains the epoch and folds in the newest existing
     * archive-ring record while the same FILE lock serializes both views. */
    memset(&archive_event, 0, sizeof archive_event);
    archive_event.kind = ISAAC_VITA_ARCHIVE_DIAG_FSEEK;
    archive_event.flags = ISAAC_VITA_ARCHIVE_DIAG_CACHE_HIT;
    archive_event.position_before = 123;
    archive_event.position_after = 456;
    (void)strcpy(archive_event.key, "afterbirth.a");
    (void)isaac_vita_archive_diag_record(&archive_event);
    isaac_vita_crt_io_profile_begin();
    s_io_profile.fread_calls = 17u;
    s_io_profile.fread_returned_bytes = 65537u;
    CHECK(isaac_vita_crt_io_profile_get_snapshot(&snapshot) == 1);
    CHECK(s_io_profile_active == 1u);
    CHECK(snapshot.fread_calls == 17u);
    CHECK(snapshot.fread_returned_bytes == 65537u);
    CHECK(snapshot.archive_sequence == archive_event.sequence);
    CHECK(snapshot.archive_kind == ISAAC_VITA_ARCHIVE_DIAG_FSEEK);
    CHECK(snapshot.archive_flags == ISAAC_VITA_ARCHIVE_DIAG_CACHE_HIT);
    CHECK(snapshot.archive_position_before == 123);
    CHECK(snapshot.archive_position_after == 456);
    CHECK(strcmp(snapshot.archive_key, "afterbirth.a") == 0);
    CHECK(isaac_vita_crt_io_profile_stop_and_snapshot(&snapshot) == 1);
    CHECK(s_io_profile_active == 0u);
    puts("Vita packed-archive I/O shadow replay oracle: PASS");
    return 0;
}
EOF
"$work/kage-vita-io-shadow-host-oracle"

"$cc" $arm_flags \
    "$root/runtime/kage_vita_io_profile.c" \
    "$root/runtime/kage_vita_io_profile_oracle.c" \
    -Wl,--gc-sections \
    -o "$work/kage-vita-io-profile-softfp-oracle.elf"

crt_base_flags="-std=gnu11 -O2 -Wall -Wextra -Werror -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -ffunction-sections -fdata-sections -I$root/runtime -I$root/vita"
production_flags="$crt_base_flags -DISAAC_VITA_IO_PROFILE=1"
"$cc" $production_flags -c "$root/runtime/kage_vita_io_profile.c" \
    -o "$work/kage-vita-io-profile.production.o"
"$cc" $production_flags -Wno-maybe-uninitialized \
    -DGUEST_IMAGE_BASE=0x98000000u \
    -DISAAC_VITA_ARCHIVE_FILE_CACHE=1 \
    -c "$root/runtime/host_vita_crt.c" \
    -o "$work/host-vita-crt-io-profile.production.o"
"$cc" $production_flags -Wno-maybe-uninitialized \
    -DGUEST_IMAGE_BASE=0x98000000u \
    -c "$root/runtime/host_vita_crt.c" \
    -o "$work/host-vita-crt-io-profile-no-cache.production.o"
"$cc" $crt_base_flags -Wno-maybe-uninitialized \
    -DGUEST_IMAGE_BASE=0x98000000u \
    -c "$root/runtime/host_vita_crt.c" \
    -o "$work/host-vita-crt-no-profile.production.o"
"$cc" $production_flags -c "$root/runtime/kage_vita_loading.c" \
    -o "$work/kage-vita-loading.production.o"
"$cc" -r \
    "$work/kage-vita-loading.production.o" \
    "$work/kage-vita-io-profile.production.o" \
    -o "$work/kage-vita-io-profile-combined.o"

"$readelf" -h "$work/kage-vita-io-profile-softfp-oracle.elf" > "$work/header"
"$readelf" -A "$work/kage-vita-io-profile-softfp-oracle.elf" > "$work/attributes"
"$nm" -u "$work/kage-vita-io-profile-softfp-oracle.elf" > "$work/undefined"
cat "$work/header"
cat "$work/attributes"
if ! grep -q "soft-float ABI" "$work/header"; then
    echo "I/O profile oracle is not marked soft-float ABI" >&2
    exit 3
fi
if grep -q "Tag_ABI_VFP_args" "$work/attributes"; then
    echo "I/O profile oracle advertises hardfp arguments" >&2
    exit 4
fi
if [ -s "$work/undefined" ]; then
    cat "$work/undefined" >&2
    echo "I/O profile oracle has unresolved symbols" >&2
    exit 5
fi
if "$nm" "$work/kage-vita-io-profile-softfp-oracle.elf" | grep -Eq '__atomic|libatomic'; then
    echo "32-bit I/O counters unexpectedly depend on libatomic" >&2
    exit 6
fi
for symbol in \
    kage_vita_io_profile_begin kage_vita_io_profile_progress \
    kage_vita_io_profile_report
do
    if ! "$nm" "$work/kage-vita-io-profile-combined.o" | \
            grep -Eq "^[[:xdigit:]]+[[:space:]]+T[[:space:]]+$symbol$"; then
        echo "strong profiler did not replace loading weak hook: $symbol" >&2
        exit 7
    fi
done
for symbol in sceIoRead sceIoLseek32 sceIoLseek; do
    if ! "$nm" "$work/kage-vita-io-profile-combined.o" | \
            grep -Eq "^[[:xdigit:]]+[[:space:]]+T[[:space:]]+__wrap_$symbol$" ||
       ! "$nm" "$work/kage-vita-io-profile-combined.o" | \
            grep -Eq "^[[:space:]]+U[[:space:]]+__real_$symbol$"; then
        echo "I/O profiler wrapper/real pair missing: $symbol" >&2
        exit 8
    fi
    if ! grep -Fq -- "-Wl,--wrap=$symbol" "$root/vita/CMakeLists.txt"; then
        echo "I/O profiler CMake wrap option missing: $symbol" >&2
        exit 9
    fi
done
if [ "$(grep -Fc 'kage_vita_io_profile_progress(completed);' \
          "$root/runtime/host_vita_first_fault.c")" -ne 1 ]; then
    echo "logical fread path lost its bounded live-profile checkpoint" >&2
    exit 10
fi

sha256sum \
    "$root/runtime/kage_vita_io_profile.h" \
    "$root/runtime/kage_vita_io_profile.c" \
    "$root/runtime/kage_vita_io_profile_oracle.c" \
    "$root/runtime/host_vita_crt.h" \
    "$root/runtime/host_vita_crt.c" \
    "$root/runtime/host_vita_first_fault.c" \
    "$root/vita/CMakeLists.txt" \
    "$root/vita/test_kage_vita_io_profile.sh" \
    "$work/kage-vita-io-profile-host-oracle" \
    "$work/kage-vita-io-shadow-host-oracle" \
    "$work/kage-vita-io-profile-softfp-oracle.elf" \
    "$work/kage-vita-io-profile-combined.o" \
    "$work/host-vita-crt-io-profile.production.o" \
    "$work/host-vita-crt-io-profile-no-cache.production.o" \
    "$work/host-vita-crt-no-profile.production.o"
echo "Vita native-I/O profile + packed-archive shadow replay + softfp static ABI: PASS (ARM ELF was not executed)"
