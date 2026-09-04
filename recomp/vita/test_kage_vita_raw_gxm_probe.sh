#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_RAW_GXM_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_RAW_GXM_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-raw-gxm.XXXXXX")}
host_cc=${CC:-cc}
python_cmd=${PYTHON:-python3}
cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
objdump="$VITASDK/bin/arm-vita-eabi-objdump"
probe="$root/runtime/kage_vita_raw_gxm_probe.c"
build_id=0123456789abcdef0123456789abcdef01234567
# Keep the ordinary unset production-ELF path safe under set -u.  The optional
# final gate below uses the identical defaulted expansion.
(
    unset ISAAC_RAW_GXM_FINAL_ELF
    test -z "${ISAAC_RAW_GXM_FINAL_ELF:-}"
) || {
    echo "unset production final-ELF selection is not inert" >&2
    exit 2
}
symbols="
sceGxmColorSurfaceInit
sceGxmBeginScene
sceGxmDraw
sceGxmEndScene
sceGxmDisplayQueueAddEntry
"
mkdir -p "$work/fake/psp2"

cat > "$work/fake/psp2/gxm.h" <<'EOF'
#ifndef TEST_PSP2_GXM_H
#define TEST_PSP2_GXM_H
#include <stdint.h>
typedef struct SceGxmColorSurface { uint32_t value; } SceGxmColorSurface;
typedef struct SceGxmContext { uint32_t value; } SceGxmContext;
typedef struct SceGxmRenderTarget { uint32_t value; } SceGxmRenderTarget;
typedef struct SceGxmValidRegion { uint32_t value; } SceGxmValidRegion;
typedef struct SceGxmSyncObject { uint32_t value; } SceGxmSyncObject;
typedef struct SceGxmDepthStencilSurface {
    uint32_t value;
} SceGxmDepthStencilSurface;
typedef struct SceGxmNotification { uint32_t value; } SceGxmNotification;
typedef uint32_t SceGxmColorFormat;
typedef uint32_t SceGxmColorSurfaceType;
typedef uint32_t SceGxmColorSurfaceScaleMode;
typedef uint32_t SceGxmOutputRegisterSize;
typedef uint32_t SceGxmPrimitiveType;
typedef uint32_t SceGxmIndexFormat;
#endif
EOF

cat > "$work/arm_reals.c" <<'EOF'
#include <psp2/gxm.h>

void isaac_vita_log(const char *format, ...)
{
    (void)format;
}

__attribute__((noinline))
int sceGxmColorSurfaceInit(
    SceGxmColorSurface *surface, SceGxmColorFormat color_format,
    SceGxmColorSurfaceType surface_type,
    SceGxmColorSurfaceScaleMode scale_mode,
    SceGxmOutputRegisterSize output_register_size,
    unsigned int width, unsigned int height, unsigned int stride_in_pixels,
    void *data)
{
    (void)surface; (void)color_format; (void)surface_type; (void)scale_mode;
    (void)output_register_size; (void)width; (void)height;
    (void)stride_in_pixels; (void)data;
    return 11;
}

__attribute__((noinline))
int sceGxmBeginScene(
    SceGxmContext *context, unsigned int flags,
    const SceGxmRenderTarget *render_target,
    const SceGxmValidRegion *valid_region,
    SceGxmSyncObject *vertex_sync_object,
    SceGxmSyncObject *fragment_sync_object,
    const SceGxmColorSurface *color_surface,
    const SceGxmDepthStencilSurface *depth_stencil)
{
    (void)context; (void)flags; (void)render_target; (void)valid_region;
    (void)vertex_sync_object; (void)fragment_sync_object;
    (void)color_surface; (void)depth_stencil;
    return 12;
}

__attribute__((noinline))
int sceGxmDraw(
    SceGxmContext *context, SceGxmPrimitiveType primitive_type,
    SceGxmIndexFormat index_type, const void *index_data,
    unsigned int index_count)
{
    (void)context; (void)primitive_type; (void)index_type;
    (void)index_data; (void)index_count;
    return 13;
}

__attribute__((noinline))
int sceGxmEndScene(
    SceGxmContext *context,
    const SceGxmNotification *vertex_notification,
    const SceGxmNotification *fragment_notification)
{
    (void)context; (void)vertex_notification; (void)fragment_notification;
    return 14;
}

__attribute__((noinline))
int sceGxmDisplayQueueAddEntry(
    SceGxmSyncObject *old_buffer, SceGxmSyncObject *new_buffer,
    const void *callback_data)
{
    (void)old_buffer; (void)new_buffer; (void)callback_data;
    return 15;
}
EOF

run_arm_checks()
{
arm_flags="-std=c11 -O2 -Wall -Wextra -Werror -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns -fno-unwind-tables -fno-asynchronous-unwind-tables -I$root/runtime"
"$cc" $arm_flags \
    -DISAAC_VITA_RAW_GXM_BUILD_ID=\"$build_id\" \
    -c "$probe" -o "$work/probe.o"
"$cc" $arm_flags -c "$work/arm_callers.c" -o "$work/callers.o"
"$cc" $arm_flags -c "$work/arm_reals.c" -o "$work/reals.o"

wrap_flags=
for symbol in $symbols; do
    wrap_flags="$wrap_flags -Wl,--wrap=$symbol"
done
"$cc" -nostdlib -Wl,-e,raw_gxm_link_entry -Wl,--gc-sections \
    $wrap_flags "$work/probe.o" "$work/callers.o" "$work/reals.o" \
    -o "$work/raw-gxm-softfp.elf"

"$readelf" -h "$work/raw-gxm-softfp.elf" > "$work/header"
"$readelf" -A "$work/raw-gxm-softfp.elf" > "$work/attributes"
"$readelf" -rW "$work/probe.o" > "$work/probe.relocations"
"$nm" -u "$work/probe.o" > "$work/probe.undefined"
"$nm" -u "$work/raw-gxm-softfp.elf" > "$work/final.undefined"
"$nm" "$work/raw-gxm-softfp.elf" > "$work/final.symbols"
"$objdump" -d "$work/raw-gxm-softfp.elf" > "$work/final.disassembly"

if ! grep -q "soft-float ABI" "$work/header"; then
    echo "raw GXM link fixture is not marked soft-float ABI" >&2
    exit 3
fi
if grep -q "Tag_ABI_VFP_args" "$work/attributes"; then
    echo "raw GXM link fixture unexpectedly advertises hardfp arguments" >&2
    exit 4
fi
if [ -s "$work/final.undefined" ]; then
    cat "$work/final.undefined" >&2
    echo "raw GXM link fixture has unresolved symbols" >&2
    exit 5
fi
for symbol in $symbols; do
    if ! grep -Eq "[[:space:]]T[[:space:]]+__wrap_$symbol$" \
            "$work/final.symbols"; then
        echo "final fixture is missing wrapper definition: $symbol" >&2
        exit 6
    fi
    if ! grep -Eq "[[:space:]]T[[:space:]]+$symbol$" \
            "$work/final.symbols"; then
        echo "final fixture is missing real definition: $symbol" >&2
        exit 7
    fi
done

"$python_cmd" - "$work/probe.relocations" "$work/probe.undefined" $symbols <<'PY'
import pathlib
import re
import sys

relocations = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
undefined = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
for symbol in sys.argv[3:]:
    real = "__real_" + symbol
    relocation_count = sum(
        bool(re.search(r"\b" + re.escape(real) + r"\b", line))
        for line in relocations.splitlines()
    )
    undefined_count = sum(
        line.split()[-1:] == [real] for line in undefined.splitlines()
    )
    if relocation_count != 1 or undefined_count != 1:
        raise SystemExit(
            f"{symbol}: expected one real relocation/symbol, got "
            f"{relocation_count}/{undefined_count}"
        )
print("Raw GXM pre-link exactly-one-real-relocation oracle: PASS")
PY

cat > "$work/verify_interception.py" <<'PY'
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
fixture = len(sys.argv) > 2 and sys.argv[2] == "fixture"
symbols = sys.argv[3:] if fixture else sys.argv[2:]
text = path.read_text(encoding="utf-8")
headers = list(re.finditer(
    r"(?m)^\s*([0-9a-f]+) <([^>]+)>:\s*$", text
))
functions = {}
addresses = {}
for index, match in enumerate(headers):
    end = headers[index + 1].start() if index + 1 < len(headers) else len(text)
    name = match.group(2)
    functions[name] = text[match.end():end]
    addresses[name] = int(match.group(1), 16)
if not functions:
    raise SystemExit("no functions parsed from final ELF")

branch = re.compile(
    r"\b(?:b|b\.w|bl|bl\.w|blx)\s+[^<\n]*<([^>]+)>"
)
targets = {
    name: [match.group(1) for match in branch.finditer(body)]
    for name, body in functions.items()
}

def require_vitasdk_from_thumb_seam(stub, raw):
    body = functions.get(stub)
    if body is None:
        raise SystemExit("VitaSDK from-thumb stub body missing: " + stub)
    if raw not in addresses:
        raise SystemExit("raw import body missing: " + raw)

    stub_address = addresses[stub]
    lines = [line.strip() for line in body.splitlines() if line.strip()]
    if len(lines) != 3:
        raise SystemExit("unexpected VitaSDK from-thumb stub size: " + stub)
    if not re.fullmatch(
            rf"{stub_address:x}:\s+e59fc000\s+ldr\s+ip,\s*\[pc\](?:\s*;.*)?",
            lines[0], re.IGNORECASE):
        raise SystemExit("unexpected VitaSDK from-thumb load: " + stub)
    if not re.fullmatch(
            rf"{stub_address + 4:x}:\s+e08ff00c\s+add\s+pc,\s*pc,\s*ip",
            lines[1], re.IGNORECASE):
        raise SystemExit("unexpected VitaSDK from-thumb jump: " + stub)
    literal = re.fullmatch(
        rf"{stub_address + 8:x}:\s+([0-9a-f]{{8}})\s+\.word\s+"
        r"0x([0-9a-f]{8})",
        lines[2], re.IGNORECASE
    )
    if literal is None or literal.group(1).lower() != literal.group(2).lower():
        raise SystemExit("unexpected VitaSDK from-thumb literal: " + stub)
    destination = (stub_address + 12 + int(literal.group(1), 16)) & 0xffffffff
    if destination != addresses[raw]:
        raise SystemExit(
            f"VitaSDK from-thumb stub misses raw import {raw}: "
            f"0x{destination:08x} != 0x{addresses[raw]:08x}"
        )

for symbol in symbols:
    wrapper = "__wrap_" + symbol
    wrapper_veneer = "____wrap_" + symbol + "_veneer"
    raw_veneer = "__" + symbol + "_veneer"
    from_thumb = "__" + symbol + "_from_thumb"
    wrapped_targets = {wrapper, wrapper_veneer}
    raw_targets = {symbol, raw_veneer, from_thumb}
    if wrapper not in functions:
        raise SystemExit("wrapper body missing: " + wrapper)

    wrapped_users = [
        caller for caller, called in targets.items()
        if wrapped_targets.intersection(called)
    ]
    if not wrapped_users:
        raise SystemExit("no final-ELF caller reaches wrapper: " + symbol)
    bypasses = [
        f"{caller}->{target}"
        for caller, called in targets.items()
        for target in sorted(raw_targets.intersection(called))
        if caller not in {
            symbol: {wrapper, raw_veneer},
            raw_veneer: {wrapper},
            from_thumb: {wrapper},
        }[target]
    ]
    if bypasses:
        raise SystemExit(
            f"raw final-ELF bypass for {symbol}: {sorted(bypasses)}"
        )
    wrapper_raw_targets = raw_targets.intersection(targets.get(wrapper, ()))
    if not wrapper_raw_targets:
        raise SystemExit(
            "wrapper has no direct/veneer real edge: " + symbol
        )
    if from_thumb in wrapper_raw_targets:
        require_vitasdk_from_thumb_seam(from_thumb, symbol)
    if fixture:
        caller = "raw_gxm_call_" + symbol
        called = targets.get(caller)
        if called is None:
            raise SystemExit("fixture caller missing: " + caller)
        if not wrapped_targets.intersection(called):
            raise SystemExit("fixture caller bypassed wrapper: " + symbol)
        if raw_targets.intersection(called):
            raise SystemExit("fixture caller retained raw edge: " + symbol)

print(
    "Raw GXM final-ELF direct/veneer interception: PASS "
    f"({path.name})"
)
PY

cat > "$work/veneer.disassembly" <<'EOF'
00000000 <raw_gxm_call_sceGxmDraw>:
   0: f000 b800  b.w 10 <____wrap_sceGxmDraw_veneer>
00000010 <____wrap_sceGxmDraw_veneer>:
  10: f000 b800  b.w 20 <__wrap_sceGxmDraw>
00000020 <__wrap_sceGxmDraw>:
  20: f000 f800  bl 30 <__sceGxmDraw_veneer>
00000030 <__sceGxmDraw_veneer>:
  30: f000 b800  b.w 40 <sceGxmDraw>
00000040 <sceGxmDraw>:
  40: 4770       bx lr
EOF
"$python_cmd" "$work/verify_interception.py" \
    "$work/veneer.disassembly" fixture sceGxmDraw
cp "$work/veneer.disassembly" "$work/bypass.disassembly"
cat >> "$work/bypass.disassembly" <<'EOF'
00000050 <hostile_raw_sceGxmDraw_user>:
  50: f000 f800  bl 40 <sceGxmDraw>
EOF
if "$python_cmd" "$work/verify_interception.py" \
        "$work/bypass.disassembly" fixture sceGxmDraw \
        > "$work/bypass.stdout" 2> "$work/bypass.stderr"; then
    echo "hostile final-ELF raw bypass unexpectedly passed" >&2
    exit 8
fi
if ! grep -Eq '^raw final-ELF bypass for sceGxmDraw:' \
        "$work/bypass.stderr"; then
    cat "$work/bypass.stderr" >&2
    echo "hostile final-ELF bypass failed for the wrong reason" >&2
    exit 8
fi
echo "Raw GXM final-ELF veneer acceptance/raw-bypass rejection: PASS"

cat > "$work/from-thumb.disassembly" <<'EOF'
00000000 <raw_gxm_call_sceGxmDraw>:
   0: f000 b800  b.w 10 <____wrap_sceGxmDraw_veneer>
00000010 <____wrap_sceGxmDraw_veneer>:
  10: f000 b800  b.w 20 <__wrap_sceGxmDraw>
00000020 <__wrap_sceGxmDraw>:
  20: f000 f806  bl 30 <__sceGxmDraw_from_thumb>
00000030 <__sceGxmDraw_from_thumb>:
  30: e59fc000  ldr ip, [pc] ; 38 <__sceGxmDraw_from_thumb+0x8>
  34: e08ff00c  add pc, pc, ip
  38: 00000004  .word 0x00000004
00000040 <sceGxmDraw>:
  40: 4770       bx lr
EOF
"$python_cmd" "$work/verify_interception.py" \
    "$work/from-thumb.disassembly" fixture sceGxmDraw
cp "$work/from-thumb.disassembly" "$work/from-thumb-bypass.disassembly"
cat >> "$work/from-thumb-bypass.disassembly" <<'EOF'
00000050 <hostile_from_thumb_sceGxmDraw_user>:
  50: f000 f800  bl 30 <__sceGxmDraw_from_thumb>
EOF
if "$python_cmd" "$work/verify_interception.py" \
        "$work/from-thumb-bypass.disassembly" fixture sceGxmDraw \
        > "$work/from-thumb-bypass.stdout" \
        2> "$work/from-thumb-bypass.stderr"; then
    echo "hostile final-ELF from-thumb bypass unexpectedly passed" >&2
    exit 8
fi
if ! grep -Eq '^raw final-ELF bypass for sceGxmDraw:' \
        "$work/from-thumb-bypass.stderr"; then
    cat "$work/from-thumb-bypass.stderr" >&2
    echo "hostile final-ELF from-thumb bypass failed for the wrong reason" >&2
    exit 8
fi
echo "Raw GXM VitaSDK from-thumb seam acceptance/raw-bypass rejection: PASS"

cp "$work/from-thumb.disassembly" \
    "$work/from-thumb-trampoline-bypass.disassembly"
cat >> "$work/from-thumb-trampoline-bypass.disassembly" <<'EOF'
00000060 <__sceGxmDraw_veneer>:
  60: f000 f800  bl 30 <__sceGxmDraw_from_thumb>
EOF
if "$python_cmd" "$work/verify_interception.py" \
        "$work/from-thumb-trampoline-bypass.disassembly" fixture sceGxmDraw \
        > "$work/from-thumb-trampoline-bypass.stdout" \
        2> "$work/from-thumb-trampoline-bypass.stderr"; then
    echo "non-wrapper final-ELF from-thumb trampoline unexpectedly passed" >&2
    exit 8
fi
if ! grep -Eq '^raw final-ELF bypass for sceGxmDraw:' \
        "$work/from-thumb-trampoline-bypass.stderr"; then
    cat "$work/from-thumb-trampoline-bypass.stderr" >&2
    echo "non-wrapper final-ELF from-thumb trampoline failed for the wrong reason" >&2
    exit 8
fi
echo "Raw GXM VitaSDK from-thumb wrapper-only ownership: PASS"

"$python_cmd" "$work/verify_interception.py" \
    "$work/final.disassembly" fixture $symbols

production_final=SKIP
if [ -n "${ISAAC_RAW_GXM_FINAL_ELF:-}" ]; then
    if [ ! -f "$ISAAC_RAW_GXM_FINAL_ELF" ]; then
        echo "production final ELF is missing: $ISAAC_RAW_GXM_FINAL_ELF" >&2
        exit 8
    fi
    "$objdump" -d "$ISAAC_RAW_GXM_FINAL_ELF" \
        > "$work/production-final.disassembly"
    "$python_cmd" "$work/verify_interception.py" \
        "$work/production-final.disassembly" $symbols
    production_final=PASS
fi

sha256sum \
    "$root/runtime/kage_vita_raw_gxm_probe.h" \
    "$root/runtime/kage_vita_raw_gxm_probe.c" \
    "$root/vita/test_kage_vita_raw_gxm_probe.sh" \
    "$work/host_oracle" \
    "$work/probe.o" \
    "$work/raw-gxm-softfp.elf"
echo "Vita raw GXM bounded-RLE + exactly-once wrappers + ARM softfp + final-ELF interception + OFF static gate: PASS (ARM ELF not executed; production-final=$production_final)"
}

cat > "$work/host_oracle.c" <<'EOF'
#include <psp2/gxm.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kage_vita_raw_gxm_probe.h"

#define ERROR_VALUE(v) ((int32_t)UINT32_C(v))

static char s_logs[64][512];
static unsigned int s_log_count;
static unsigned int s_surface_real_calls;
static unsigned int s_begin_real_calls;
static unsigned int s_draw_real_calls;
static unsigned int s_end_real_calls;
static unsigned int s_queue_real_calls;
static int s_surface_result;
static int s_begin_result;
static int s_draw_result;
static int s_end_result;
static int s_queue_result;

static void fail(const char *message)
{
    fprintf(stderr, "raw GXM host oracle: %s\n", message);
    exit(1);
}

static void require_true(int condition, const char *message)
{
    if (!condition)
        fail(message);
}

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;
    int length;

    if (s_log_count >= 64u)
        fail("log storage overflow");
    va_start(arguments, format);
    length = vsnprintf(
        s_logs[s_log_count], sizeof(s_logs[s_log_count]), format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t)length >= sizeof(s_logs[s_log_count]))
        fail("truncated log record");
    if (length > 383)
        fail("log record exceeded bounded production logger body");
    ++s_log_count;
}

static const char *find_log(const char *needle)
{
    unsigned int index;
    const char *result = NULL;

    for (index = 0u; index < s_log_count; ++index) {
        if (strstr(s_logs[index], needle)) {
            if (result)
                fail("log selector is not unique");
            result = s_logs[index];
        }
    }
    if (!result)
        fail(needle);
    return result;
}

static void require_field(const char *line, const char *field)
{
    if (!strstr(line, field))
        fail(field);
}

static uint32_t hash_results(const int32_t *values, unsigned int count)
{
    uint32_t hash = UINT32_C(2166136261);
    unsigned int index;

    for (index = 0u; index < count; ++index) {
        uint32_t value = (uint32_t)values[index];
        unsigned int byte;
        for (byte = 0u; byte < 4u; ++byte) {
            hash ^= value & UINT32_C(0xff);
            hash *= UINT32_C(16777619);
            value >>= 8u;
        }
    }
    return hash;
}

int __real_sceGxmColorSurfaceInit(
    SceGxmColorSurface *surface, SceGxmColorFormat color_format,
    SceGxmColorSurfaceType surface_type,
    SceGxmColorSurfaceScaleMode scale_mode,
    SceGxmOutputRegisterSize output_register_size,
    unsigned int width, unsigned int height, unsigned int stride_in_pixels,
    void *data)
{
    (void)surface; (void)color_format; (void)surface_type; (void)scale_mode;
    (void)output_register_size; (void)width; (void)height;
    (void)stride_in_pixels; (void)data;
    ++s_surface_real_calls;
    return s_surface_result;
}

int __real_sceGxmBeginScene(
    SceGxmContext *context, unsigned int flags,
    const SceGxmRenderTarget *render_target,
    const SceGxmValidRegion *valid_region,
    SceGxmSyncObject *vertex_sync_object,
    SceGxmSyncObject *fragment_sync_object,
    const SceGxmColorSurface *color_surface,
    const SceGxmDepthStencilSurface *depth_stencil)
{
    (void)context; (void)flags; (void)render_target; (void)valid_region;
    (void)vertex_sync_object; (void)fragment_sync_object;
    (void)color_surface; (void)depth_stencil;
    ++s_begin_real_calls;
    return s_begin_result;
}

int __real_sceGxmDraw(
    SceGxmContext *context, SceGxmPrimitiveType primitive_type,
    SceGxmIndexFormat index_type, const void *index_data,
    unsigned int index_count)
{
    (void)context; (void)primitive_type; (void)index_type;
    (void)index_data; (void)index_count;
    ++s_draw_real_calls;
    return s_draw_result;
}

int __real_sceGxmEndScene(
    SceGxmContext *context,
    const SceGxmNotification *vertex_notification,
    const SceGxmNotification *fragment_notification)
{
    (void)context; (void)vertex_notification; (void)fragment_notification;
    ++s_end_real_calls;
    return s_end_result;
}

int __real_sceGxmDisplayQueueAddEntry(
    SceGxmSyncObject *old_buffer, SceGxmSyncObject *new_buffer,
    const void *callback_data)
{
    (void)old_buffer; (void)new_buffer; (void)callback_data;
    ++s_queue_real_calls;
    return s_queue_result;
}

int __wrap_sceGxmColorSurfaceInit(
    SceGxmColorSurface *, SceGxmColorFormat, SceGxmColorSurfaceType,
    SceGxmColorSurfaceScaleMode, SceGxmOutputRegisterSize,
    unsigned int, unsigned int, unsigned int, void *);
int __wrap_sceGxmBeginScene(
    SceGxmContext *, unsigned int, const SceGxmRenderTarget *,
    const SceGxmValidRegion *, SceGxmSyncObject *, SceGxmSyncObject *,
    const SceGxmColorSurface *, const SceGxmDepthStencilSurface *);
int __wrap_sceGxmDraw(
    SceGxmContext *, SceGxmPrimitiveType, SceGxmIndexFormat,
    const void *, unsigned int);
int __wrap_sceGxmEndScene(
    SceGxmContext *, const SceGxmNotification *,
    const SceGxmNotification *);
int __wrap_sceGxmDisplayQueueAddEntry(
    SceGxmSyncObject *, SceGxmSyncObject *, const void *);

static void call_surface(
    SceGxmColorSurface *surface, int result, unsigned int width,
    unsigned int height, void *data)
{
    unsigned int before = s_surface_real_calls;
    s_surface_result = result;
    require_true(
        __wrap_sceGxmColorSurfaceInit(
            surface, 0x11u, 0x22u, 0x33u, 0x44u,
            width, height, width, data) == result,
        "ColorSurfaceInit return changed");
    require_true(
        s_surface_real_calls == before + 1u,
        "ColorSurfaceInit did not call real exactly once");
}

static void call_begin(int result)
{
    unsigned int before = s_begin_real_calls;
    s_begin_result = result;
    require_true(
        __wrap_sceGxmBeginScene(
            NULL, 0u, NULL, NULL, NULL, NULL, NULL, NULL) == result,
        "BeginScene return changed");
    require_true(s_begin_real_calls == before + 1u,
                 "BeginScene did not call real exactly once");
}

static void call_draw(int result)
{
    unsigned int before = s_draw_real_calls;
    s_draw_result = result;
    require_true(
        __wrap_sceGxmDraw(NULL, 0u, 0u, NULL, 0u) == result,
        "Draw return changed");
    require_true(s_draw_real_calls == before + 1u,
                 "Draw did not call real exactly once");
}

static void call_end(int result)
{
    unsigned int before = s_end_real_calls;
    s_end_result = result;
    require_true(
        __wrap_sceGxmEndScene(NULL, NULL, NULL) == result,
        "EndScene return changed");
    require_true(s_end_real_calls == before + 1u,
                 "EndScene did not call real exactly once");
}

static void call_queue(int result)
{
    unsigned int before = s_queue_real_calls;
    s_queue_result = result;
    require_true(
        __wrap_sceGxmDisplayQueueAddEntry(NULL, NULL, NULL) == result,
        "DisplayQueueAddEntry return changed");
    require_true(s_queue_real_calls == before + 1u,
                 "DisplayQueueAddEntry did not call real exactly once");
}

int main(void)
{
    SceGxmColorSurface surfaces[5];
    int32_t frame7_draws[] = {
        0, 0, 0, ERROR_VALUE(0x805b0006), ERROR_VALUE(0x805b0006), 0, 0
    };
    int32_t overflow_draws[] = {
        1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2
    };
    unsigned int index;
    unsigned int sealed_log_count;
    char hash_field[32];
    const char *line;

    call_surface(&surfaces[0], 0, 960u, 544u, (void *)(uintptr_t)0x1000u);
    call_surface(
        &surfaces[1], ERROR_VALUE(0x805b0001), 960u, 544u,
        (void *)(uintptr_t)0x2000u);
    call_surface(&surfaces[2], 0, 960u, 544u, (void *)(uintptr_t)0x3000u);

    /* All wrappers remain transparent before the post-loading bracket. */
    call_begin(ERROR_VALUE(0x805b0a01));
    call_draw(ERROR_VALUE(0x805b0a02));
    call_end(ERROR_VALUE(0x805b0a03));
    call_queue(ERROR_VALUE(0x805b0a04));
    require_true(s_log_count == 0u, "a wrapper logged on the call thread");

    kage_vita_raw_gxm_probe_begin_frame(7u);
    kage_vita_raw_gxm_probe_begin_frame(999u);
    call_surface(
        &surfaces[3], ERROR_VALUE(0x805b00aa), 512u, 256u,
        (void *)(uintptr_t)0x4000u);
    call_surface(&surfaces[4], 0, 128u, 64u, (void *)(uintptr_t)0x5000u);
    call_begin(ERROR_VALUE(0x805b1001));
    for (index = 0u; index < 7u; ++index)
        call_draw(frame7_draws[index]);
    call_end(ERROR_VALUE(0x805b1002));
    call_queue(ERROR_VALUE(0x805b1003));
    kage_vita_raw_gxm_probe_end_frame(7u);

    require_field(
        find_log("phase=surface-summary"),
        "phase=surface-summary n=5 keep=4 ov=1");
    line = find_log("phase=surface i=3 ");
    require_field(line, "rc=805b00aa");
    require_field(line, "data=00004000");
    require_field(line, "wh=512/256");
    line = find_log("phase=frame begin=7 ");
    require_field(line, "end=7 match=1 nested=1 ordinal=1/4");
    line = find_log("f=7 api=b ");
    require_field(line, "n=1 keep=1 ov=0/0");
    require_field(line, "runs=1:805b1001*1");
    line = find_log("f=7 api=d ");
    require_field(line, "n=7 keep=7 ov=0/0");
    require_field(
        line,
        "runs=3:00000000*3,805b0006*2,00000000*2");
    line = find_log("f=7 api=e ");
    require_field(line, "runs=1:805b1002*1");
    line = find_log("f=7 api=q ");
    require_field(line, "runs=1:805b1003*1");

    kage_vita_raw_gxm_probe_begin_frame(8u);
    for (index = 0u; index < 12u; ++index)
        call_draw(overflow_draws[index]);
    kage_vita_raw_gxm_probe_end_frame(8u);
    line = find_log("f=8 api=d ");
    require_field(line, "n=12 keep=8 ov=4/4");
    require_field(line, "last=00000002");
    require_field(line, "runs=8:");
    require_field(line, "of=00000001");
    (void)snprintf(
        hash_field, sizeof(hash_field), "hash=%08x",
        (unsigned)hash_results(overflow_draws, 12u));
    require_field(line, hash_field);

    kage_vita_raw_gxm_probe_begin_frame(9u);
    kage_vita_raw_gxm_probe_end_frame(109u);
    require_field(
        find_log("phase=frame begin=9 "),
        "end=109 match=0 nested=0 ordinal=3/4");
    kage_vita_raw_gxm_probe_begin_frame(10u);
    kage_vita_raw_gxm_probe_end_frame(10u);

    require_true(s_log_count == 25u, "bounded four-frame log count changed");
    sealed_log_count = s_log_count;
    kage_vita_raw_gxm_probe_begin_frame(11u);
    call_draw(ERROR_VALUE(0x805bffff));
    kage_vita_raw_gxm_probe_end_frame(11u);
    call_surface(&surfaces[0], 0, 1u, 1u, NULL);
    require_true(s_log_count == sealed_log_count,
                 "frame/surface capture did not seal");
    require_true(
        s_surface_real_calls == 6u && s_begin_real_calls == 2u &&
        s_draw_real_calls == 21u && s_end_real_calls == 2u &&
        s_queue_real_calls == 2u,
        "real-call totals changed");

    puts("Raw GXM host behavior oracle: PASS");
    return 0;
}
EOF

common_flags="-std=c11 -O2 -Wall -Wextra -Werror -I$root/runtime"
"$host_cc" $common_flags -I"$work/fake" \
    -DISAAC_VITA_RAW_GXM_BUILD_ID=\"$build_id\" \
    "$probe" "$work/host_oracle.c" -o "$work/host_oracle"
"$work/host_oracle"

# Fail closed on OFF-by-default/source scope, call-thread purity and the two
# owner-thread lifecycle seams.  This is independent of compiler optimisation.
"$python_cmd" - \
    "$probe" \
    "$root/runtime/kage_vita_raw_gxm_probe.h" \
    "$root/runtime/kage_vita_generated_hooks.c" \
    "$root/runtime/kage_vita_backend.c" \
    "$root/vita/CMakeLists.txt" $symbols <<'PY'
import pathlib
import re
import sys

probe_path, header_path, hooks_path, backend_path, cmake_path = map(
    pathlib.Path, sys.argv[1:6]
)
symbols = sys.argv[6:]
probe = probe_path.read_text(encoding="utf-8")
header = header_path.read_text(encoding="utf-8")
hooks = hooks_path.read_text(encoding="utf-8")
backend = backend_path.read_text(encoding="utf-8")
cmake = cmake_path.read_text(encoding="utf-8")


def function_body(text: str, marker: str) -> str:
    start = text.index(marker)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace + 1:index]
    raise SystemExit("unterminated function: " + marker)


for symbol in symbols:
    body = function_body(probe, "int __wrap_" + symbol + "(")
    if body.count("__real_" + symbol + "(") != 1:
        raise SystemExit("wrapper does not have exactly one real call: " + symbol)
    for forbidden in (
        "isaac_vita_log(", "malloc(", "calloc(", "realloc(", "free(",
        "memalign(", "aligned_alloc(", "sceGxmPadHeartbeat(",
    ):
        if forbidden in body:
            raise SystemExit(
                "wrapper gained logging/allocation/other GXM work: "
                + symbol + " " + forbidden
            )

if re.search(
    r"\b(?:malloc|calloc|realloc|free|memalign|aligned_alloc|strdup)\s*\(",
    probe,
):
    raise SystemExit("probe source gained a raw allocation")
if "sceGxmPadHeartbeat" in probe or "vglSwapBuffers" in probe:
    raise SystemExit("raw return-code probe gained a behavior toggle")
if probe.index("kage_vita_raw_gxm_report_surfaces();") < probe.index(
    "void kage_vita_raw_gxm_probe_end_frame"
):
    raise SystemExit("surface capture seals before first Render completes")

option = re.compile(
    r"option\(ISAAC_VITA_RAW_GXM_PROBE\s+"
    r"\"[^\"]+\"\s+OFF\)"
)
if len(option.findall(cmake)) != 1:
    raise SystemExit("raw GXM option is not exactly once/default OFF")
feature_start = cmake.index("    if(ISAAC_VITA_RAW_GXM_PROBE)\n")
feature_end = cmake.index("\n    endif()", feature_start)
feature = cmake[feature_start:feature_end]
source = '"${ISAAC_RUNTIME}/kage_vita_raw_gxm_probe.c"'
if cmake.count(source) != 2 or feature.count(source) != 2:
    raise SystemExit("probe source escaped or is absent from feature gate")
for owner in (
    '"${ISAAC_RUNTIME}/kage_vita_backend.c"',
    '"${ISAAC_RUNTIME}/kage_vita_generated_hooks.c"',
):
    if feature.count(owner) != 1:
        raise SystemExit("raw probe macro owner set changed: " + owner)
if feature.count("ISAAC_VITA_RAW_GXM_PROBE=1") != 1:
    raise SystemExit("raw probe selection macro scope changed")
if (
    feature.count("ISAAC_VITA_RAW_GXM_BUILD_ID=") != 1
    or 'ISAAC_VITA_RAW_GXM_BUILD_ID=\\"${ISAAC_VITA_GUEST_LINK_ID}\\"'
       not in feature
):
    raise SystemExit("source-scoped raw probe build identity changed")
for symbol in symbols:
    token = "-Wl,--wrap=" + symbol
    if cmake.count(token) != 1 or token not in feature:
        raise SystemExit("wrapper option escaped or is absent: " + symbol)
for forbidden in (
    "sceGxmPadHeartbeat", "NO_LOADING", "SKIP_LOADING", "vglSwapBuffers",
    "ISAAC_VITA_DIRECT_DEFAULT",
):
    if forbidden in feature:
        raise SystemExit("raw probe feature gate gained behavior/policy: " + forbidden)

dependency_start = cmake.index("if(ISAAC_VITA_RAW_GXM_PROBE AND")
dependency_end = cmake.index("\nendif()", dependency_start)
dependency = cmake[dependency_start:dependency_end]
if "ISAAC_VITA_DIRECT_DEFAULT" in dependency:
    raise SystemExit("raw probe was coupled to direct-default")
direct_start = cmake.index("if(ISAAC_VITA_DIRECT_DEFAULT AND")
direct_end = cmake.index("\nendif()", direct_start)
if "ISAAC_VITA_RAW_GXM_PROBE" in cmake[direct_start:direct_end]:
    raise SystemExit("direct-default was coupled to raw probe")

hook_body = function_body(hooks, "void kage_pc_backend_note_loop_head(void)")
if not (
    hook_body.index("kage_vita_loading_finish();")
    < hook_body.index("kage_vita_raw_gxm_probe_begin_frame(")
    < hook_body.index("kage_vita_input_sample()")
):
    raise SystemExit("post-loading begin-frame ownership/order changed")
present_body = function_body(backend, "int kage_vita_backend_present(void)")
if not (
    present_body.index("vglSwapBuffers(GL_FALSE);")
    < present_body.index("kage_vita_raw_gxm_probe_end_frame(")
    < present_body.index("s_present_count++;")
):
    raise SystemExit("presentation-boundary end-frame ownership/order changed")
for text, label in ((hooks, "hooks"), (backend, "backend")):
    if text.count('#if defined(ISAAC_VITA_RAW_GXM_PROBE)') != 2:
        raise SystemExit(label + " raw probe include/call scope changed")

if "#define KAGE_VITA_RAW_GXM_FRAME_LIMIT 4u" not in header:
    raise SystemExit("four-frame bound changed")
if "#define KAGE_VITA_RAW_GXM_RUN_CAPACITY 8u" not in header:
    raise SystemExit("RLE bound changed")
print("Raw GXM source/CMake OFF-scope and lifecycle oracle: PASS")
PY

cat > "$work/arm_callers.c" <<'EOF'
#include <psp2/gxm.h>

__attribute__((noinline))
int raw_gxm_call_sceGxmColorSurfaceInit(void)
{
    return sceGxmColorSurfaceInit(
        (SceGxmColorSurface *)0, (SceGxmColorFormat)0,
        (SceGxmColorSurfaceType)0, (SceGxmColorSurfaceScaleMode)0,
        (SceGxmOutputRegisterSize)0, 1u, 1u, 1u, (void *)0);
}

__attribute__((noinline))
int raw_gxm_call_sceGxmBeginScene(void)
{
    return sceGxmBeginScene(
        (SceGxmContext *)0, 0u, (const SceGxmRenderTarget *)0,
        (const SceGxmValidRegion *)0, (SceGxmSyncObject *)0,
        (SceGxmSyncObject *)0, (const SceGxmColorSurface *)0,
        (const SceGxmDepthStencilSurface *)0);
}

__attribute__((noinline))
int raw_gxm_call_sceGxmDraw(void)
{
    return sceGxmDraw(
        (SceGxmContext *)0, (SceGxmPrimitiveType)0,
        (SceGxmIndexFormat)0, (const void *)0, 0u);
}

__attribute__((noinline))
int raw_gxm_call_sceGxmEndScene(void)
{
    return sceGxmEndScene(
        (SceGxmContext *)0, (const SceGxmNotification *)0,
        (const SceGxmNotification *)0);
}

__attribute__((noinline))
int raw_gxm_call_sceGxmDisplayQueueAddEntry(void)
{
    return sceGxmDisplayQueueAddEntry(
        (SceGxmSyncObject *)0, (SceGxmSyncObject *)0, (const void *)0);
}

volatile int raw_gxm_link_sink;
void raw_gxm_link_entry(void)
{
    raw_gxm_link_sink =
        raw_gxm_call_sceGxmColorSurfaceInit() +
        raw_gxm_call_sceGxmBeginScene() +
        raw_gxm_call_sceGxmDraw() +
        raw_gxm_call_sceGxmEndScene() +
        raw_gxm_call_sceGxmDisplayQueueAddEntry();
}
EOF

run_arm_checks
