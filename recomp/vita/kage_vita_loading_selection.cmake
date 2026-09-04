include_guard(GLOBAL)

# Select the loading-screen implementation without changing the target-wide
# command line.  Toggling the Specialist Dance animation therefore recompiles
# only kage_vita_loading.c; generated guest translation units remain untouched.
function(isaac_configure_vita_loading_source
         loading_source fallback_include specialist_include)
  get_filename_component(_loading_source "${loading_source}" ABSOLUTE
                         BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  get_filename_component(_fallback_include "${fallback_include}" ABSOLUTE
                         BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  get_filename_component(_specialist_include "${specialist_include}" ABSOLUTE
                         BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

  if(NOT EXISTS "${_loading_source}")
    message(FATAL_ERROR "Vita loading source is missing: ${_loading_source}")
  endif()
  if(NOT EXISTS "${_fallback_include}")
    message(FATAL_ERROR
      "Redistributable Vita loading fallback is missing: ${_fallback_include}")
  endif()

  if(ISAAC_VITA_LOADING_SPECIALIST)
    if(NOT EXISTS "${_specialist_include}")
      message(FATAL_ERROR
        "ISAAC_VITA_LOADING_SPECIALIST=ON requires ${_specialist_include}")
    endif()
    set_property(SOURCE "${_loading_source}" APPEND PROPERTY
      COMPILE_DEFINITIONS ISAAC_KAGE_VITA_LOADING_SPECIALIST=1)
    set(_loading_mode "local Specialist override")
  else()
    set(_loading_mode "redistributable procedural fallback")
  endif()

  if(NOT DEFINED ISAAC_VITA_LOADING_PRESENTATION)
    set(ISAAC_VITA_LOADING_PRESENTATION ON)
  endif()
  if(ISAAC_VITA_LOADING_PRESENTATION)
    set(_presentation_value 1)
    set(_presentation_mode "enabled")
  else()
    set(_presentation_value 0)
    set(_presentation_mode "disabled")
  endif()
  set_property(SOURCE "${_loading_source}" APPEND PROPERTY
    COMPILE_DEFINITIONS
      ISAAC_KAGE_VITA_LOADING_PRESENTATION=${_presentation_value})

  set(ISAAC_VITA_LOADING_MODE "${_loading_mode}" PARENT_SCOPE)
  set(ISAAC_VITA_LOADING_PRESENTATION_MODE
      "${_presentation_mode}" PARENT_SCOPE)
endfunction()
