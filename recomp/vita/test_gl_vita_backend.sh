#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_RBO_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_RBO_TEST_OUT:-}" ]; then
    work=$ISAAC_RBO_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-rbo-oracle.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
# Optional: the pinned patched vitaGL include (vitagl-stock-reference/0002
# adds vglIsaacDrawCanonicalQuads).  Without it the production object of a
# prod-* mode is compiled without CANONICAL_QUAD_ZERO_COPY; the oracle ELF
# always has it (the fake vitaGL declares the symbol).
vitagl_include=${ISAAC_RBO_VITAGL_INCLUDE:-}
strings="$VITASDK/bin/arm-vita-eabi-strings"
common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
production_flags="$common_flags -DGUEST_STACK_REQUIRED=1"
oracle_flags="$common_flags -Wno-error=maybe-uninitialized -DISAAC_VITA_IO_PROFILE=1"

for name in \
    glBindTextureUnit glBindTextures \
    glCopyTexImage2D glCopyTexSubImage2D \
    glCompressedTexImage2D glCompressedTexSubImage2D \
    glGenerateMipmap glPixelStorei glTexStorage2D \
    glTextureStorage2D glTextureSubImage2D glTextureView \
    glFramebufferTextureLayer glGetTexLevelParameteriv
do
    if grep -Fq "\"$name\"" "$root/runtime/gl_surface_generated.inc"; then
        echo "GL backend surface gained an unmodelled texture mutator: $name" >&2
        exit 3
    fi
done

for mode in off on fxray texture texture-fxray fbo fbo720 time time-fbo720 \
    prod-elision prod-elision-drop prod-loc prod-loc-verify \
    prod-elision-loc-verify prod-fill prod-elision-drop-fill
do
    if [ "$mode" = on ] || [ "$mode" = fbo720 ] || [ "$mode" = time-fbo720 ]; then
        display_define=-DISAAC_VITA_DISPLAY_RASTER_720=1
    else
        display_define=
    fi
    if [ "$mode" = time ] || [ "$mode" = time-fbo720 ]; then
        # GL time profile: clock brackets around every native boundary call.
        time_define="-DISAAC_VITA_GL_TIME_PROFILE=1 -DISAAC_VITA_PHASE_PROFILE=1"
    else
        time_define=
    fi
    if [ "$mode" = fbo ] || [ "$mode" = fbo720 ] || [ "$mode" = time-fbo720 ]; then
        fbo_define="-DISAAC_VITA_FBO_CLEAR_ELISION=1 -DISAAC_VITA_FBO_RASTER_SCALE=1 -DISAAC_VITA_FBO_RASTER_SCALE_NUM=1 -DISAAC_VITA_FBO_RASTER_SCALE_DEN=2 -DISAAC_VITA_PHASE_PROFILE=1"
    else
        fbo_define=
    fi
    if [ "$mode" = fxray ] || [ "$mode" = texture-fxray ]; then
        fxray_define=-DISAAC_VITA_FXRAY_ALPHA_MASK=1
    else
        fxray_define=
    fi
    if [ "$mode" = texture ] || [ "$mode" = texture-fxray ]; then
        texture_define="-DISAAC_VITA_TEXTURE_CHURN_PROFILE=1 -DISAAC_VITA_PHASE_PROFILE=1"
    else
        texture_define=
    fi
    # prod-*: the production option set that reaches gl_vita_backend.c
    # (CMakeLists.txt GL_REDUNDANCY_CACHE / GL_TYPED_STATE_CACHE /
    # GL_SHIM_FASTDISPATCH / CANONICAL_QUAD_ZERO_COPY scopes plus
    # PHASE_PROFILE) with the device-gated options on top: clear elision alone
    # (no raster scale), the location cache with and without its VERIFY
    # shadow, and both together.
    case $mode in
    prod-*)
        prod_define="-DISAAC_VITA_GL_REDUNDANCY_CACHE=1 -DISAAC_VITA_GL_TYPED_STATE_CACHE=1 -DISAAC_VITA_GL_SHIM_FASTDISPATCH=1 -DISAAC_VITA_CANONICAL_QUAD_ZERO_COPY=1 -DISAAC_VITA_PHASE_PROFILE=1"
        ;;
    *)
        prod_define=
        ;;
    esac
    # prod-fill / prod-elision-drop-fill: the GL fill census with its
    # one-frame draw dump, alone and on top of the depth-drop elision (whose
    # absorbed clears the census must not count).
    if [ "$mode" = prod-fill ] || [ "$mode" = prod-elision-drop-fill ]; then
        fill_define="-DISAAC_VITA_GL_FILL_CENSUS=1 -DISAAC_VITA_GL_FILL_CENSUS_DUMP=1"
    else
        fill_define=
    fi
    if [ "$mode" = prod-elision-drop ] || [ "$mode" = prod-elision-drop-fill ]; then
        elision_define="-DISAAC_VITA_FBO_CLEAR_ELISION=1 -DISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP=1"
    elif [ "$mode" = prod-elision ] || [ "$mode" = prod-elision-loc-verify ]; then
        elision_define=-DISAAC_VITA_FBO_CLEAR_ELISION=1
    else
        elision_define=
    fi
    if [ "$mode" = prod-loc ] || [ "$mode" = prod-loc-verify ] ||
            [ "$mode" = prod-elision-loc-verify ]; then
        location_define=-DISAAC_VITA_GL_LOCATION_CACHE=1
    else
        location_define=
    fi
    if [ "$mode" = prod-loc-verify ] || [ "$mode" = prod-elision-loc-verify ]; then
        verify_define=-DISAAC_VITA_GL_LOCATION_CACHE_VERIFY=1
    else
        verify_define=
    fi
    production_prod_define=$prod_define
    production_include=
    if [ -n "$vitagl_include" ]; then
        production_include="-isystem $vitagl_include"
    else
        production_prod_define=${prod_define//-DISAAC_VITA_CANONICAL_QUAD_ZERO_COPY=1/}
    fi
    "$cc" $oracle_flags $display_define $fxray_define $texture_define \
        $fbo_define $time_define \
        $prod_define $elision_define $location_define $verify_define \
        $fill_define \
        -DISAAC_GL_VITA_BACKEND_ORACLE=1 \
        -DGUEST_IMAGE_BASE=0x98000000u \
        -I"$root/runtime" \
        "$root/runtime/gl_bridge.c" \
        "$root/runtime/gl_vita_backend.c" \
        "$root/runtime/guest_stack_legacy_oracle_stub.c" \
        "$root/runtime/gl_vita_backend_oracle.c" \
        -Wl,--gc-sections \
        -o "$work/gl-vita-rbo-oracle.$mode.elf"

    "$cc" $production_flags $display_define $fxray_define $texture_define \
        $fbo_define $time_define \
        $production_prod_define $elision_define $location_define $verify_define \
        $fill_define \
        $production_include \
        -DGUEST_IMAGE_BASE=0x98000000u \
        -I"$root/runtime" -I"$root/vita" \
        -c "$root/runtime/gl_vita_backend.c" \
        -o "$work/gl_vita_backend.production.$mode.o"

    "$readelf" -h "$work/gl-vita-rbo-oracle.$mode.elf"
    "$readelf" -A "$work/gl-vita-rbo-oracle.$mode.elf"
    "$readelf" -A "$work/gl_vita_backend.production.$mode.o"
    "$nm" -u "$work/gl-vita-rbo-oracle.$mode.elf" \
        > "$work/oracle.$mode.undefined"
    "$nm" -u "$work/gl_vita_backend.production.$mode.o" \
        > "$work/production.$mode.undefined"
    cat "$work/oracle.$mode.undefined"

    if grep -Eq '(__atomic|libatomic)' "$work/oracle.$mode.undefined"; then
        echo "display-$mode oracle unexpectedly depends on libatomic" >&2
        exit 3
    fi
    if grep -Eq '[[:space:]]U[[:space:]]+kage_vita_backend_memory_snapshot_at_first_fbo$' \
            "$work/production.$mode.undefined"; then
        echo "display-$mode GL backend retained default-OFF memory snapshot edge" >&2
        exit 3
    fi
done

"$strings" "$work/gl-vita-rbo-oracle.off.elf" > "$work/oracle.strings"

for evidence in \
    "Vita GL backend: 62/73 typed symbols installed; 11 remain loud" \
    "Vita GL glGetRenderbufferParameteriv has no bound renderbuffer" \
    "Vita GL glGetRenderbufferParameteriv received an invalid target" \
    "Vita GL renderbuffer registry exhausted" \
    "KAGE VITA GXM RT %s: profile=gxm-io-v1 site=%.42s f=0x%08x/0x%08x r=0x%08x/0x%08x fin=0x%08x fc=%u del=0x%08x t=%u full=%u dup=%u inv=%u p=%u,%u,%u,%u drain=%u live=%u refs=%u wh=%ux%u frame=%u" \
    "vitaGL render-target acquire failed" \
    "vitaGL render-target telemetry was null" \
    "nested or concurrent guest GL dispatch"
do
    if ! grep -Fqx "$evidence" "$work/oracle.strings"; then
        echo "missing RBO/backend policy evidence: $evidence" >&2
        exit 4
    fi
done

sha256sum \
    "$root/runtime/gl_bridge.h" \
    "$root/runtime/gl_bridge.c" \
    "$root/runtime/gl_vita_backend.h" \
    "$root/runtime/gl_vita_backend.c" \
    "$root/runtime/manual_kage_vita.c" \
    "$root/runtime/gl_vita_backend_oracle.c" \
    "$root/runtime/gl_vita_backend_test_vitagl.h" \
    "$root/runtime/gl_vita_backend_test_vitagl_undef.h" \
    "$root/vita/test_gl_vita_backend.sh" \
    "$work/gl_vita_backend.production.off.o" \
    "$work/gl_vita_backend.production.on.o" \
    "$work/gl_vita_backend.production.fxray.o" \
    "$work/gl-vita-rbo-oracle.off.elf" \
    "$work/gl-vita-rbo-oracle.on.elf" \
    "$work/gl-vita-rbo-oracle.fxray.elf"
echo "Vita GL RBO/display-raster OFF+ON plus FXLayers-ray softfp compile/link/static ABI check: PASS (ELFs are not executed; behavior runs in the x86 oracle)"
