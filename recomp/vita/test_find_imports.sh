#!/usr/bin/env bash
set -eu

host_only=${ISAAC_FIND_HOST_ONLY:-0}
if [ "$host_only" != 0 ] && [ "$host_only" != 1 ]; then
    echo "ISAAC_FIND_HOST_ONLY must be 0 or 1" >&2
    exit 2
fi
if [ "$host_only" = 0 ] && [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_FIND_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_FIND_TEST_OUT:-}" ]; then
    work=$ISAAC_FIND_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-find-imports.XXXXXX")
fi

host_cc=${CC:-cc}

common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-maybe-uninitialized"

mkdir -p "$work/fake/psp2/io" "$work/fake/psp2/kernel"
cat > "$work/fake/psp2/io/dirent.h" <<'EOF'
#ifndef ORACLE_PSP2_IO_DIRENT_H
#define ORACLE_PSP2_IO_DIRENT_H
#include <stdint.h>
typedef int32_t SceUID;
typedef struct SceDateTime {
    uint16_t year;
    uint16_t month;
    uint16_t day;
    uint16_t hour;
    uint16_t minute;
    uint16_t second;
    uint32_t microsecond;
} SceDateTime;
typedef struct SceIoStat {
    uint32_t st_mode;
    uint32_t st_attr;
    int64_t st_size;
    SceDateTime st_ctime;
    SceDateTime st_atime;
    SceDateTime st_mtime;
    uint32_t st_private[6];
} SceIoStat;
typedef struct SceIoDirent {
    SceIoStat d_stat;
    char d_name[256];
    uint32_t d_private;
    int dummy;
} SceIoDirent;
#define SCE_S_IFREG UINT32_C(0020000)
#define SCE_S_IFDIR UINT32_C(0010000)
#define SCE_S_IFMT  UINT32_C(0170000)
#define SCE_S_ISDIR(mode) (((mode) & SCE_S_IFMT) == SCE_S_IFDIR)
_Static_assert(sizeof(SceIoStat) == 0x58U,
               "fake SceIoStat must match VitaSDK ABI");
_Static_assert(sizeof(SceIoDirent) == 0x160U,
               "fake SceIoDirent must match VitaSDK ABI");
SceUID sceIoDopen(const char *path);
int sceIoDread(SceUID descriptor, SceIoDirent *entry);
int sceIoDclose(SceUID descriptor);
#endif
EOF
cat > "$work/fake/psp2/io/stat.h" <<'EOF'
#ifndef ORACLE_PSP2_IO_STAT_H
#define ORACLE_PSP2_IO_STAT_H
#include <psp2/io/dirent.h>
int sceIoGetstat(const char *path, SceIoStat *status);
#endif
EOF
cat > "$work/fake/psp2/kernel/threadmgr.h" <<'EOF'
#ifndef ORACLE_PSP2_KERNEL_THREADMGR_H
#define ORACLE_PSP2_KERNEL_THREADMGR_H
int sceKernelDelayThread(unsigned int delay);
#endif
EOF

# GCC cannot prove that the legacy guest-stack helper initializes its local
# `guarded` flag before the inline fast path.  Production uses the required
# out-of-line stack guard; suppress only that inherited header warning here.
"$host_cc" -std=gnu11 -O2 -Wall -Wextra -Werror \
    -Wno-maybe-uninitialized \
    -DGUEST_IMAGE_BASE=0x10000000u \
    -I"$work/fake" -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_find.c" \
    "$root/runtime/vita_find_semantics_oracle.c" \
    -o "$work/vita-find-semantics-oracle"
"$work/vita-find-semantics-oracle"

if [ "$host_only" = 1 ]; then
    echo "Vita FindFile host semantics check: PASS"
    exit 0
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
strings="$VITASDK/bin/arm-vita-eabi-strings"

"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/host_vita_find.c" \
    "$root/runtime/vita_find_import_test.c" \
    -o "$work/vita-find-import-oracle.elf" \
    -lSceIofilemgr_stub \
    -lSceKernelThreadMgr_stub

"$readelf" -h "$work/vita-find-import-oracle.elf"
"$readelf" -A "$work/vita-find-import-oracle.elf"
"$nm" "$work/vita-find-import-oracle.elf" > \
    "$work/vita-find-import-oracle.symbols"
"$strings" "$work/vita-find-import-oracle.elf" > \
    "$work/vita-find-import-oracle.strings"

for symbol in \
    isaac_vita_find_import \
    isaac_vita_find_import_counted \
    sceIoDopen \
    sceIoDread \
    sceIoDclose \
    sceIoGetstat \
    sceKernelDelayThread
do
    if ! grep -q "[[:space:]]$symbol$" \
        "$work/vita-find-import-oracle.symbols"
    then
        echo "missing linked FindFile symbol: $symbol" >&2
        exit 3
    fi
done

for evidence in \
    "FindFirstFileW native directory open failed" \
    "FindFile native directory read failed" \
    "FindFirstFileW native directory close failed" \
    "FindClose native directory close failed"
do
    if ! grep -Fqx "$evidence" \
        "$work/vita-find-import-oracle.strings"
    then
        echo "missing loud FindFile policy evidence: $evidence" >&2
        exit 4
    fi
done

for obsolete in \
    "FindNextFileW received an unknown search handle" \
    "FindClose received an unknown search handle"
do
    if grep -Fqx "$obsolete" \
        "$work/vita-find-import-oracle.strings"
    then
        echo "stale-token path is still loud: $obsolete" >&2
        exit 5
    fi
done

sha256sum \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/host_vita_find.h" \
    "$root/runtime/host_vita_find.c" \
    "$root/runtime/vita_find_import_test.c" \
    "$root/runtime/vita_find_semantics_oracle.c" \
    "$root/vita/test_find_imports.sh" \
    "$work/vita-find-import-oracle.elf"
echo "Vita FindFile host semantics + compile/link/static ABI check: PASS (no synthetic dot entries; exact-file stat path; 3 locked handlers; expected Win32 failures publish LastError; unexpected Sony I/O errors loud)"
