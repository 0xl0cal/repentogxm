cmake_minimum_required(VERSION 3.19)

set(_module
  "${CMAKE_CURRENT_LIST_DIR}/kage_vita_stock_reference_selection.cmake")
set(_production "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt")
include("${_module}")

macro(_valid_profile)
  set(ISAAC_VITA_VITAGL_STOCK_REFERENCE ON)
  set(ISAAC_VITA_SCAFFOLD_ONLY OFF)
  set(ISAAC_VITA_KAGE ON)
  set(ISAAC_VITA_DIRECT_DEFAULT ON)
  set(ISAAC_VITA_FIRST_FRAME_PROBE OFF)
  set(ISAAC_VITA_RAW_GXM_PROBE OFF)
  set(ISAAC_VITA_KNOWN_COLOR_PROBE OFF)
  set(ISAAC_VITA_RASTER_PROBE OFF)
  set(ISAAC_VITA_SCREENSHOT_PROBE OFF)
  set(ISAAC_VITA_LOADING_PRESENTATION ON)
  set(ISAAC_VITA_AUDIO OFF)
  set(ISAAC_VITA_OPENAL_POOL ON)
  set(ISAAC_VITA_IO_PROFILE OFF)
  set(ISAAC_VITA_STALL_PROBE OFF)
  set(ISAAC_VITA_PHASE_PROFILE OFF)
  set(ISAAC_VITA_TEXTURE_CHURN_PROFILE OFF)
  set(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS OFF)
  set(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS OFF)
  set(ISAAC_VITA_FULLSPEED_SCHEDULER OFF)
  set(ISAAC_VITA_GUEST_LOOKUP_CACHE OFF)
  set(ISAAC_VITA_DISPLAY_RASTER_720 OFF)
  set(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS OFF)
  set(ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY OFF)
  set(ISAAC_VITA_GXM_STATE_SHADOW OFF)
endmacro()

function(_expect label expected)
  isaac_vita_stock_reference_profile_error(_actual)
  if(NOT "${_actual}" STREQUAL "${expected}")
    message(FATAL_ERROR
      "${label}: unexpected stock-reference validation result: "
      "'${_actual}' != '${expected}'")
  endif()
endfunction()

_valid_profile()
_expect(valid "")
set(ISAAC_VITA_AUDIO ON)
_expect(valid_with_production_audio_and_openal_pool "")
_valid_profile()
set(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS ON)
_expect(valid_with_exact_gpu_draw_optimizations "")
set(ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY ON)
set(ISAAC_VITA_GXM_STATE_SHADOW ON)
_expect(valid_with_exact_draw_submission "")
_valid_profile()
set(ISAAC_VITA_PHASE_PROFILE ON)
_expect(valid_with_aggregate_phase_profile "")
set(ISAAC_VITA_FULLSPEED_SCHEDULER ON)
_expect(valid_with_phase_profile_and_fullspeed_scheduler "")
_valid_profile()
set(ISAAC_VITA_PHASE_PROFILE ON)
set(ISAAC_VITA_GUEST_LOOKUP_CACHE ON)
_expect(valid_with_phase_profile_and_guest_lookup_cache "")
set(ISAAC_VITA_FULLSPEED_SCHEDULER ON)
_expect(valid_with_phase_profile_fullspeed_and_guest_lookup_cache "")
set(ISAAC_VITA_DISPLAY_RASTER_720 ON)
_expect(valid_with_phase_profile_fullspeed_cache_and_display_raster "")
unset(ISAAC_VITA_RASTER_PROBE)
_expect(valid_without_raster_option "")

set(_requires
  "ISAAC_VITA_VITAGL_STOCK_REFERENCE requires translated KAGE and ISAAC_VITA_DIRECT_DEFAULT=ON")
foreach(_required_case IN ITEMS
    ISAAC_VITA_SCAFFOLD_ONLY
    ISAAC_VITA_KAGE
    ISAAC_VITA_DIRECT_DEFAULT)
  _valid_profile()
  if(_required_case STREQUAL "ISAAC_VITA_SCAFFOLD_ONLY")
    set(${_required_case} ON)
  else()
    set(${_required_case} OFF)
  endif()
  _expect("${_required_case}" "${_requires}")
endforeach()

foreach(_forbidden IN ITEMS
    ISAAC_VITA_FIRST_FRAME_PROBE
    ISAAC_VITA_RAW_GXM_PROBE
    ISAAC_VITA_KNOWN_COLOR_PROBE
    ISAAC_VITA_RASTER_PROBE
    ISAAC_VITA_SCREENSHOT_PROBE
    ISAAC_VITA_IO_PROFILE
    ISAAC_VITA_STALL_PROBE)
  _valid_profile()
  set(${_forbidden} ON)
  _expect("${_forbidden}"
    "ISAAC_VITA_VITAGL_STOCK_REFERENCE forbids enabled features: ${_forbidden}")
endforeach()

_valid_profile()
set(ISAAC_VITA_AUDIO ON)
set(ISAAC_VITA_PHASE_PROFILE ON)
set(ISAAC_VITA_TEXTURE_CHURN_PROFILE ON)
_expect(texture_churn_with_audio
  "ISAAC_VITA_TEXTURE_CHURN_PROFILE requires ISAAC_VITA_AUDIO=OFF")

_valid_profile()
set(ISAAC_VITA_FIRST_FRAME_PROBE ON)
_expect(multiple_forbidden
  "ISAAC_VITA_VITAGL_STOCK_REFERENCE forbids enabled features: ISAAC_VITA_FIRST_FRAME_PROBE")

# OFF is a no-op and must not change the legacy profile's legal combinations.
_valid_profile()
set(ISAAC_VITA_VITAGL_STOCK_REFERENCE OFF)
set(ISAAC_VITA_FIRST_FRAME_PROBE ON)
set(ISAAC_VITA_RAW_GXM_PROBE ON)
set(ISAAC_VITA_LOADING_PRESENTATION ON)
_expect(reference_off "")

file(READ "${_production}" _production_text)
foreach(_needle IN ITEMS
    "option(ISAAC_VITA_VITAGL_STOCK_REFERENCE"
    "option(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS"
    "option(ISAAC_VITA_VITAGL_SHADER_CACHE"
    "option(ISAAC_VITA_LASER_ATLAS_P8"
    "ISAAC_VITA_LASER_ATLAS_P8 requires ISAAC_VITA_VITAGL_P8_SAFE_UPLOAD=ON"
    "ISAAC_LASER_ATLAS_P8=\${ISAAC_VITA_LASER_ATLAS_P8_MODE}"
    "option(ISAAC_VITA_LASER_P8_SWIZZLE"
    "ISAAC_VITA_LASER_P8_SWIZZLE requires ISAAC_VITA_LASER_ATLAS_P8=ON"
    "ISAAC_LASER_P8_SWIZZLE=\${ISAAC_VITA_LASER_P8_SWIZZLE_MODE}"
    "isaac-laser-p8-swizzle.mode"
    "\${ISAAC_VITA_LASER_P8_SWIZZLE_MODE_FILE}"
    "option(ISAAC_VITA_LASER_LIGHT_NEAREST"
    "option(ISAAC_VITA_LASER_LIGHT_HALO_DEPTH_APPROX"
    "ISAAC_VITA_LASER_LIGHT_HALO_DEPTH_APPROX requires ISAAC_VITA_LASER_LIGHT_HALO_CLIP=ON"
    "ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX=\${ISAAC_VITA_LASER_LIGHT_HALO_DEPTH_APPROX_MODE}"
    "isaac-laser-light-halo-depth-approx.mode"
    "\${ISAAC_VITA_LASER_LIGHT_HALO_DEPTH_APPROX_MODE_FILE}"
    "Isaac Vita light-laser halo: depth approximation ON; faint fringe color and depth coverage removed"
    "ISAAC_VITA_LASER_LIGHT_NEAREST requires ISAAC_VITA_LASER_LIGHT_HALO_CLIP=ON"
    "ISAAC_LASER_LIGHT_NEAREST=\${ISAAC_VITA_LASER_LIGHT_NEAREST_MODE}"
    "isaac-laser-light-nearest.mode"
    "\${ISAAC_VITA_LASER_LIGHT_NEAREST_MODE_FILE}"
    "0017-isaac-laser-light-nearest.patch"
    "isaac_laser_light_nearest.h"
    "option(ISAAC_VITA_LASER_ATLAS_NEAREST"
    "if(ISAAC_VITA_LASER_ATLAS_NEAREST AND\n   (NOT ISAAC_VITA_VITAGL_STOCK_REFERENCE OR\n    NOT ISAAC_VITA_VITAGL_SHADER_CACHE OR"
    "ISAAC_VITA_LASER_ATLAS_NEAREST requires stock vitaGL, SHADER_CACHE, LASER_ATLAS_P8, STAGING_PROOF, NEUTRAL_FASTPATH and PLAIN_VERTEX_PAIR"
    "ISAAC_LASER_ATLAS_NEAREST=\${ISAAC_VITA_LASER_ATLAS_NEAREST_MODE}"
    "isaac-laser-atlas-nearest.mode"
    "\${ISAAC_VITA_LASER_ATLAS_NEAREST_MODE_FILE}"
    "0019-isaac-laser-atlas-nearest.patch"
    "isaac_laser_atlas_nearest.h"
    "0011-isaac-exact-laser-p8.patch"
    "isaac_laser_p8_reference.h"
    "option(ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY"
    "option(ISAAC_VITA_GXM_STATE_SHADOW"
    "option(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS"
    "option(ISAAC_VITA_PHASE_PROFILE"
    "option(ISAAC_VITA_FULLSPEED_SCHEDULER"
    "option(ISAAC_VITA_GUEST_LOOKUP_CACHE"
    "option(ISAAC_VITA_DISPLAY_RASTER_720"
    "ISAAC_VITA_DISPLAY_RASTER=\${ISAAC_VITA_DISPLAY_RASTER} requires the "
    "CONTINUE_PROFILE=OFF; kage_vita_continue_profile.c owns a fixed "
    "FIRST_FRAME, RAW_GXM, KNOWN_COLOR and SCREENSHOT probes to be OFF; "
    "set(ISAAC_VITA_STOCK_FBO_RT_SCENES OFF CACHE STRING"
    "ISAAC_VITA_STOCK_FBO_RT_SCENES must be OFF, ON or an integer 1..8 "
    "ISAAC_VITA_STOCK_FBO_RT_SCENES=\${ISAAC_VITA_STOCK_FBO_RT_SCENES} requires "
    "ISAAC_FBO_RT_SCENES=\${ISAAC_VITA_STOCK_FBO_RT_SCENES_MODE}"
    "ISAAC_VITA_STOCK_FBO_RT_SCENES=\${ISAAC_VITA_STOCK_FBO_RT_SCENES_COUNT})"
    "0005-isaac-scene-timer.patch"
    "0006-isaac-scene-split.patch"
    "0007-isaac-fbo-rt-scenes.patch"
    "set(ISAAC_VITA_COLOROFFSET_FS_PROBE OFF CACHE STRING"
    "ISAAC_VITA_COLOROFFSET_FS_PROBE must be OFF, TRIVIAL or NODISCARD "
    "ISAAC_VITA_COLOROFFSET_FS_PROBE=\${ISAAC_VITA_COLOROFFSET_FS_PROBE} requires "
    "NOT ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS OR"
    "ISAAC_COLOROFFSET_FS_PROBE=\${ISAAC_VITA_COLOROFFSET_FS_PROBE_MODE}"
    "ISAAC_VITA_COLOROFFSET_FS_PROBE=\${ISAAC_VITA_COLOROFFSET_FS_PROBE_MODE})"
    "0008-isaac-coloroffset-fs-probe.patch"
    "set(ISAAC_VITA_STOCK_FBO_VALID_REGION OFF CACHE STRING"
    "ISAAC_VITA_STOCK_FBO_VALID_REGION must be OFF, OBSERVE or ON "
    "ISAAC_VITA_STOCK_FBO_VALID_REGION=\${ISAAC_VITA_STOCK_FBO_VALID_REGION} requires "
    "ISAAC_VITA_STOCK_FBO_VALID_REGION is not supported with ISAAC_VITA_FBO_RASTER_SCALE=ON"
    "ISAAC_FBO_VALID_REGION=\${ISAAC_VITA_STOCK_FBO_VALID_REGION_PATCH_MODE}"
    "ISAAC_VITA_STOCK_FBO_VALID_REGION=\${ISAAC_VITA_STOCK_FBO_VALID_REGION_MODE})"
    "0009-isaac-fbo-valid-region.patch"
    "option(ISAAC_VITA_COLOROFFSET_STAGING_PROOF"
    "option(ISAAC_VITA_COLOROFFSET_SINGLE_FINAL_BIND"
    "ISAAC_VITA_COLOROFFSET_SINGLE_FINAL_BIND requires STAGING_PROOF=ON and NEUTRAL_FASTPATH=ON"
    "ISAAC_COLOROFFSET_SINGLE_FINAL_BIND=\${ISAAC_VITA_COLOROFFSET_SINGLE_FINAL_BIND_MODE}"
    "0014-isaac-single-final-fragment-bind.patch"
    "\${ISAAC_VITA_COLOROFFSET_SINGLE_FINAL_BIND_MODE_FILE}"
    "ISAAC_VITA_COLOROFFSET_STAGING_PROOF requires ISAAC_VITA_COLOROFFSET_NEUTRAL_FASTPATH=ON"
    "ISAAC_COLOROFFSET_STAGING_PROOF=\${ISAAC_VITA_COLOROFFSET_STAGING_PROOF_MODE}"
    "0012-isaac-coloroffset-staging-proof.patch"
    "isaac_coloroffset_staging.h"
    "\${ISAAC_VITA_COLOROFFSET_STAGING_PROOF_MODE_FILE}"
    "option(ISAAC_VITA_COLOROFFSET_PLAIN_FASTPATH"
    "option(ISAAC_VITA_COLOROFFSET_PLAIN_FP16"
    "ISAAC_VITA_COLOROFFSET_PLAIN_FP16 requires ISAAC_VITA_COLOROFFSET_PLAIN_FASTPATH=ON"
    "ISAAC_VITA_COLOROFFSET_PLAIN_FP16 is incompatible with ISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR=ON"
    "ISAAC_COLOROFFSET_PLAIN_FP16=\${ISAAC_VITA_COLOROFFSET_PLAIN_FP16_MODE}"
    "isaac_coloroffset_plain_fp16.h"
    "\${ISAAC_VITA_COLOROFFSET_PLAIN_FP16_MODE_FILE}"
    "option(ISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR"
    "ISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR requires PLAIN_FASTPATH=ON and SINGLE_FINAL_BIND=ON"
    "ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR=\${ISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR_MODE}"
    "0015-isaac-coloroffset-plain-vertex-pair.patch"
    "isaac_coloroffset_plain_vertex_pair.h"
    "\${ISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR_MODE_FILE}"
    "option(ISAAC_VITA_COLOROFFSET_TRANSFORM_UNIFORM"
    "ISAAC_VITA_COLOROFFSET_TRANSFORM_UNIFORM requires VITAGL_STOCK_REFERENCE=ON and COLOROFFSET_PLAIN_VERTEX_PAIR=ON"
    "ISAAC_COLOROFFSET_TRANSFORM_UNIFORM=\${ISAAC_VITA_COLOROFFSET_TRANSFORM_UNIFORM_MODE}"
    "0018-isaac-transform-uniform-noop.patch"
    "isaac_coloroffset_transform_uniform.h"
    "\${ISAAC_VITA_COLOROFFSET_TRANSFORM_UNIFORM_MODE_FILE}"
    "ISAAC_VITA_COLOROFFSET_PLAIN_FASTPATH requires ISAAC_VITA_COLOROFFSET_STAGING_PROOF=ON"
    "ISAAC_COLOROFFSET_PLAIN_FASTPATH=\${ISAAC_VITA_COLOROFFSET_PLAIN_FASTPATH_MODE}"
    "0013-isaac-coloroffset-plain-fastpath.patch"
    "isaac_coloroffset_plain_policy.h"
    "isaac_coloroffset_plain_vitagl.h"
    "\${ISAAC_VITA_COLOROFFSET_PLAIN_FASTPATH_MODE_FILE}"
    "option(ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP"
    "ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP requires ISAAC_VITA_FBO_CLEAR_ELISION=ON"
    "include(\"\${ISAAC_VITA_STOCK_REFERENCE_SELECTION_MODULE}\")"
    "isaac_validate_vita_stock_reference_profile()"
    "NOT ISAAC_VITA_VITAGL_STOCK_REFERENCE"
    "vitagl-stock-reference/build.sh"
    "0002-exact-gpu-draw-optimizations.patch"
    "0003-exact-coloroffset-gpu-optimizations.patch"
    "0004-hardened-custom-shader-cache.patch"
    "isaac_shader_cache_vitagl.h"
    "ISAAC_GPU_DRAW_OPTIMIZATIONS=\${ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS_MODE}"
    "ISAAC_SHADER_CACHE=\${ISAAC_VITA_VITAGL_SHADER_CACHE_MODE}"
    "ISAAC_CANONICAL_QUAD_ZERO_COPY=\${ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY_MODE}"
    "ISAAC_GXM_STATE_SHADOW=\${ISAAC_VITA_GXM_STATE_SHADOW_MODE}"
    "ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS=\${ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS_MODE}"
    "Building pinned stock vitaGL 73dd57a with gpu-draw="
    "kage_vita_phase_profile.c"
    "ISAAC_VITA_PHASE_PROFILE_BUILD_ID"
    "kage_vita_fullspeed_scheduler.c"
    "ISAAC_VITA_GUEST_LOOKUP_CACHE=1"
    "ISAAC_VITA_DISPLAY_RASTER_720=1"
    "ISAAC_VITA_VITAGL_STOCK_REFERENCE=1")
  string(FIND "${_production_text}" "${_needle}" _found)
  if(_found EQUAL -1)
    message(FATAL_ERROR "production CMake lost stock-reference edge: ${_needle}")
  endif()
endforeach()

if(NOT _production_text MATCHES
    "option\\(ISAAC_VITA_LASER_ATLAS_P8[\r\n ]+\"[^\"]+\" OFF\\)")
  message(FATAL_ERROR "exact laser P8 is not default OFF")
endif()
if(NOT _production_text MATCHES
    "option\\(ISAAC_VITA_LASER_P8_SWIZZLE[\r\n ]+\"[^\"]+\" OFF\\)")
  message(FATAL_ERROR "laser P8 swizzle is not default OFF")
endif()
if(NOT _production_text MATCHES
    "option\\(ISAAC_VITA_LASER_LIGHT_NEAREST[\r\n ]+\"[^\"]+\" OFF\\)")
  message(FATAL_ERROR "light laser nearest is not default OFF")
endif()
if(NOT _production_text MATCHES
    "option\\(ISAAC_VITA_LASER_ATLAS_NEAREST[\r\n ]+\"[^\"]+\" OFF\\)")
  message(FATAL_ERROR "laser atlas nearest is not default OFF")
endif()
if(NOT _production_text MATCHES
    "option\\(ISAAC_VITA_LASER_LIGHT_HALO_DEPTH_APPROX[\r\n ]+\"[^\"]+\" OFF\\)")
  message(FATAL_ERROR "light halo depth approximation is not default OFF")
endif()
if(NOT _production_text MATCHES
    "option\\(ISAAC_VITA_COLOROFFSET_TRANSFORM_UNIFORM[\r\n ]+\"[^\"]+\" OFF\\)")
  message(FATAL_ERROR "Transform uniform suppression is not default OFF")
endif()

message(STATUS
  "Vita stock-reference CMake profile matrix: production-audio/texture-churn-audio-off/required/forbidden/OFF PASS")
