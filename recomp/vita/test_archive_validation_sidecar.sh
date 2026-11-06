#!/usr/bin/env bash
# Host gate for ISAAC_VITA_ARCHIVE_VALIDATION_SIDECAR: the Python tool writes
# an ARCH000 fixture and its .receipt sidecar (numpy is not needed for that),
# the C module is compiled against POSIX I/O and must trust exactly that pair
# and refuse every mutation at the documented stage.  A second, larger fixture
# exercises the sampled window scheme.  Optionally a real rewritten archive
# with its sidecar can be checked too (ISAAC_ARCHIVE_SIDECAR_REAL=<path.a>).
set -eu

root=${ISAAC_ARCHIVE_SIDECAR_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_ARCHIVE_SIDECAR_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-archive-sidecar.XXXXXX")}
host_cc=${CC:-cc}
python=${PYTHON:-python3}
tool="$root/../tools/isaac_vita_halve_sfx.py"

packed="$work/data/resources/packed"
mkdir -p "$packed"

"$host_cc" -std=c11 -O2 -Wall -Wextra -Werror \
    -DISAAC_VITA_ARCHIVE_SIDECAR_ORACLE=1 \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_archive_sidecar.c" \
    "$root/runtime/host_vita_archive_sidecar_oracle.c" \
    -o "$work/archive-sidecar-host-oracle"

# The Vita build compiles the same module against sceIo; prove that path at
# least parses with stand-in psp2 headers so a typo cannot hide behind the
# oracle macros.
mkdir -p "$work/psp2/io"
cat > "$work/psp2/io/fcntl.h" <<'EOF'
#include <stdint.h>
typedef int SceUID; typedef unsigned int SceSize; typedef int64_t SceOff; typedef unsigned int SceMode;
#define SCE_O_RDONLY 1
SceUID sceIoOpen(const char *file, int flags, SceMode mode);
int sceIoClose(SceUID fd);
int sceIoPread(SceUID fd, void *data, SceSize size, SceOff offset);
EOF
cat > "$work/psp2/io/stat.h" <<'EOF'
#include <stdint.h>
typedef struct { unsigned int st_mode; int64_t st_size; } SceIoStat;
#define SCE_S_ISREG(m) (((m) & 0xF000) == 0x8000)
int sceIoGetstat(const char *file, SceIoStat *stat);
int sceIoGetstatByFd(int fd, SceIoStat *stat);
EOF
"$host_cc" -std=c11 -O2 -Wall -Wextra -Werror -fsyntax-only \
    -D__vita__=1 -DISAAC_VITA_ARCHIVE_VALIDATION_SIDECAR=1 \
    -DISAAC_VITA_ARCHIVE_SIDECAR_BUILD_ID='"gate"' \
    -I"$work" -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_archive_sidecar.c"

# Small fixture: the entry region is below 64 x 4 KiB, hashed completely.
# Large fixture: about 1 MiB of entries, the 64 sampled windows apply.
"$python" - "$tool" "$packed" <<'EOF'
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(sys.argv[1])))
import isaac_vita_halve_sfx as H
packed = sys.argv[2]
small = [{"h1": 1, "h2": 2, "data": b"RIFF" + bytes(range(256)) * 3},
         {"h1": 3, "h2": 4, "data": b"x" * 5000},
         {"h1": 5, "h2": 6, "data": b"y"}]
H.write_archive(os.path.join(packed, "small.a"), 2, small)
print(H.write_sidecar(os.path.join(packed, "small.a")))
big = [{"h1": i, "h2": 1000 + i, "data": bytes((i * 7 + j) & 0xFF for j in range(20000))} for i in range(60)]
H.write_archive(os.path.join(packed, "big.a"), 0, big)
print(H.write_sidecar(os.path.join(packed, "big.a")))
EOF

"$work/archive-sidecar-host-oracle" "$work/data" small.a
"$work/archive-sidecar-host-oracle" "$work/data" big.a

if [ -n "${ISAAC_ARCHIVE_SIDECAR_REAL:-}" ]; then
    real_base=$(basename -- "$ISAAC_ARCHIVE_SIDECAR_REAL")
    cp -- "$ISAAC_ARCHIVE_SIDECAR_REAL" "$packed/$real_base"
    cp -- "$ISAAC_ARCHIVE_SIDECAR_REAL.receipt" "$packed/$real_base.receipt"
    "$work/archive-sidecar-host-oracle" "$work/data" "$real_base"
fi
echo "archive validation sidecar: PASS ($work)"
