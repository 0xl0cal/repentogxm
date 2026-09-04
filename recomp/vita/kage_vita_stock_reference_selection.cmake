include_guard(GLOBAL)

# Keep the hardware control as one fail-closed profile.  The renderer override
# is build-owned generated code.  The CPU loading presenter is compatible: it
# uses this same context's display callback, removes it, then drains the queue
# before the first guest frame.  The CPU-only aggregate phase profiler and
# production OpenAL boundary are also compatible: neither mechanism writes
# vitaGL state.  Only the texture-churn diagnostic keeps its audio-off A/B.
function(isaac_vita_stock_reference_profile_error output_variable)
  if(NOT ISAAC_VITA_VITAGL_STOCK_REFERENCE)
    set(${output_variable} "" PARENT_SCOPE)
    return()
  endif()

  if(ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE OR
     NOT ISAAC_VITA_DIRECT_DEFAULT)
    string(CONCAT _required_error
      "ISAAC_VITA_VITAGL_STOCK_REFERENCE requires translated KAGE and "
      "ISAAC_VITA_DIRECT_DEFAULT=ON")
    set(${output_variable} "${_required_error}" PARENT_SCOPE)
    return()
  endif()

  if(ISAAC_VITA_TEXTURE_CHURN_PROFILE AND ISAAC_VITA_AUDIO)
    set(${output_variable}
      "ISAAC_VITA_TEXTURE_CHURN_PROFILE requires ISAAC_VITA_AUDIO=OFF"
      PARENT_SCOPE)
    return()
  endif()

  set(_incompatible)
  foreach(_feature IN ITEMS
      ISAAC_VITA_FIRST_FRAME_PROBE
      ISAAC_VITA_RAW_GXM_PROBE
      ISAAC_VITA_KNOWN_COLOR_PROBE
      ISAAC_VITA_RASTER_PROBE
      ISAAC_VITA_SCREENSHOT_PROBE
      ISAAC_VITA_IO_PROFILE
      ISAAC_VITA_STALL_PROBE)
    if(DEFINED ${_feature} AND ${_feature})
      list(APPEND _incompatible "${_feature}")
    endif()
  endforeach()
  if(_incompatible)
    list(JOIN _incompatible ", " _incompatible_text)
    string(CONCAT _incompatible_error
      "ISAAC_VITA_VITAGL_STOCK_REFERENCE forbids enabled features: "
      "${_incompatible_text}")
    set(${output_variable} "${_incompatible_error}" PARENT_SCOPE)
    return()
  endif()
  set(${output_variable} "" PARENT_SCOPE)
endfunction()

function(isaac_validate_vita_stock_reference_profile)
  isaac_vita_stock_reference_profile_error(_stock_reference_error)
  if(NOT "${_stock_reference_error}" STREQUAL "")
    message(FATAL_ERROR "${_stock_reference_error}")
  endif()
endfunction()
