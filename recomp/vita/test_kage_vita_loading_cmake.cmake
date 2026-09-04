cmake_minimum_required(VERSION 3.19)

foreach(_required ISAAC_LOADING_CMAKE_ROOT ISAAC_LOADING_CMAKE_OUT)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "${_required} is required")
  endif()
endforeach()

get_filename_component(ISAAC_LOADING_CMAKE_ROOT
  "${ISAAC_LOADING_CMAKE_ROOT}" ABSOLUTE)
get_filename_component(ISAAC_LOADING_CMAKE_OUT
  "${ISAAC_LOADING_CMAKE_OUT}" ABSOLUTE)
set(_module
  "${ISAAC_LOADING_CMAKE_ROOT}/vita/kage_vita_loading_selection.cmake")
set(_production "${ISAAC_LOADING_CMAKE_ROOT}/vita/CMakeLists.txt")
if(NOT EXISTS "${_module}" OR NOT EXISTS "${_production}")
  message(FATAL_ERROR "loading selection module or production CMake is missing")
endif()

file(READ "${_module}" _module_text)
file(READ "${_production}" _production_text)
foreach(_needle
    "set_property(SOURCE"
    "ISAAC_KAGE_VITA_LOADING_SPECIALIST=1"
    "ISAAC_KAGE_VITA_LOADING_PRESENTATION=\${_presentation_value}"
    "Redistributable Vita loading fallback is missing")
  string(FIND "${_module_text}" "${_needle}" _found)
  if(_found EQUAL -1)
    message(FATAL_ERROR "loading selection module lost: ${_needle}")
  endif()
endforeach()
string(FIND "${_module_text}" "target_compile_definitions" _target_define)
if(NOT _target_define EQUAL -1)
  message(FATAL_ERROR "loading selection must remain source-scoped")
endif()
foreach(_needle
    "option(ISAAC_VITA_LOADING_SPECIALIST"
    "option(ISAAC_VITA_LOADING_PRESENTATION"
    "include(\"\${ISAAC_VITA_LOADING_SELECTION_MODULE}\")"
    "isaac_configure_vita_loading_source(")
  string(FIND "${_production_text}" "${_needle}" _found)
  if(_found EQUAL -1)
    message(FATAL_ERROR "production CMake lost loading selection: ${_needle}")
  endif()
endforeach()

set(_source_dir "${ISAAC_LOADING_CMAKE_OUT}/source")
file(MAKE_DIRECTORY "${_source_dir}")
file(WRITE "${_source_dir}/loading.c" "void loading_probe(void) {}\n")
file(WRITE "${_source_dir}/other.c" "void other_probe(void) {}\n")
file(WRITE "${_source_dir}/fallback.inc" "/* redistributable test fallback */\n")
file(WRITE "${_source_dir}/specialist.inc" "/* local test override, no art */\n")
file(WRITE "${_source_dir}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.19)
project(isaac_loading_selection_probe LANGUAGES C)
add_library(selection_probe OBJECT "${LOADING_SOURCE}" "${OTHER_SOURCE}")
include("${SELECTION_MODULE}")
isaac_configure_vita_loading_source(
  "${LOADING_SOURCE}" "${FALLBACK_INCLUDE}" "${SPECIALIST_INCLUDE}")
get_source_file_property(_loading_defs "${LOADING_SOURCE}" COMPILE_DEFINITIONS)
get_source_file_property(_other_defs "${OTHER_SOURCE}" COMPILE_DEFINITIONS)
if(_loading_defs STREQUAL "NOTFOUND")
  set(_loading_defs "")
endif()
if(_other_defs STREQUAL "NOTFOUND")
  set(_other_defs "")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/selection.txt"
  "mode=${ISAAC_VITA_LOADING_MODE}\n"
  "presentation=${ISAAC_VITA_LOADING_PRESENTATION_MODE}\n"
  "loading=${_loading_defs}\n"
  "other=${_other_defs}\n"
  "direct=${ISAAC_VITA_DIRECT_DEFAULT}\n"
  "raw_gxm=${ISAAC_VITA_RAW_GXM_PROBE}\n")
]=])

function(_configure_probe name specialist specialist_include presentation
         expect_success)
  set(_build "${ISAAC_LOADING_CMAKE_OUT}/${name}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      -S "${_source_dir}" -B "${_build}"
      "-DSELECTION_MODULE=${_module}"
      "-DLOADING_SOURCE=${_source_dir}/loading.c"
      "-DOTHER_SOURCE=${_source_dir}/other.c"
      "-DFALLBACK_INCLUDE=${_source_dir}/fallback.inc"
      "-DSPECIALIST_INCLUDE=${specialist_include}"
      "-DISAAC_VITA_LOADING_SPECIALIST=${specialist}"
      "-DISAAC_VITA_LOADING_PRESENTATION=${presentation}"
      "-DISAAC_VITA_DIRECT_DEFAULT=ON"
      "-DISAAC_VITA_RAW_GXM_PROBE=ON"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
  if(expect_success)
    if(NOT _result EQUAL 0)
      message(FATAL_ERROR
        "${name} CMake selection failed:\n${_stdout}\n${_stderr}")
    endif()
    file(READ "${_build}/selection.txt" _selection)
    if(specialist)
      set(_mode "local Specialist override")
      set(_loading_defs "ISAAC_KAGE_VITA_LOADING_SPECIALIST=1;")
    else()
      set(_mode "redistributable procedural fallback")
      set(_loading_defs "")
    endif()
    if(presentation)
      set(_presentation_mode "enabled")
      string(APPEND _loading_defs
        "ISAAC_KAGE_VITA_LOADING_PRESENTATION=1")
    else()
      set(_presentation_mode "disabled")
      string(APPEND _loading_defs
        "ISAAC_KAGE_VITA_LOADING_PRESENTATION=0")
    endif()
    string(CONCAT _expected
      "mode=${_mode}\n"
      "presentation=${_presentation_mode}\n"
      "loading=${_loading_defs}\n"
      "other=\n"
      "direct=ON\n"
      "raw_gxm=ON\n")
    if(NOT _selection STREQUAL _expected)
      message(FATAL_ERROR
        "${name} source selection changed:\n${_selection}")
    endif()
  else()
    if(_result EQUAL 0)
      message(FATAL_ERROR "${name} unexpectedly accepted a missing override")
    endif()
    set(_failure "${_stdout}\n${_stderr}")
    string(FIND "${_failure}"
      "ISAAC_VITA_LOADING_SPECIALIST=ON requires" _found_failure)
    if(_found_failure EQUAL -1)
      message(FATAL_ERROR "${name} failed for the wrong reason:\n${_failure}")
    endif()
  endif()
endfunction()

_configure_probe(generic OFF "${_source_dir}/absent.inc" ON TRUE)
_configure_probe(generic_disabled OFF "${_source_dir}/absent.inc" OFF TRUE)
_configure_probe(local_override ON "${_source_dir}/specialist.inc" ON TRUE)
_configure_probe(local_override_disabled ON
  "${_source_dir}/specialist.inc" OFF TRUE)
_configure_probe(missing_override ON "${_source_dir}/absent.inc" ON FALSE)

message(STATUS
  "Vita loading CMake selection oracle: presentation on/off + "
  "direct/raw-GXM compatibility + local/missing PASS")
