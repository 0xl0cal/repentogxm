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
    "option(ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY"
    "option(ISAAC_VITA_GXM_STATE_SHADOW"
    "option(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS"
    "option(ISAAC_VITA_PHASE_PROFILE"
    "option(ISAAC_VITA_FULLSPEED_SCHEDULER"
    "option(ISAAC_VITA_GUEST_LOOKUP_CACHE"
    "option(ISAAC_VITA_DISPLAY_RASTER_720"
    "ISAAC_VITA_DISPLAY_RASTER_720 requires the translated KAGE runtime"
    "ISAAC_VITA_DISPLAY_RASTER_720 requires LOADING_PRESENTATION=OFF"
    "ISAAC_VITA_DISPLAY_RASTER_720 requires FIRST_FRAME, RAW_GXM,"
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

message(STATUS
  "Vita stock-reference CMake profile matrix: production-audio/texture-churn-audio-off/required/forbidden/OFF PASS")
