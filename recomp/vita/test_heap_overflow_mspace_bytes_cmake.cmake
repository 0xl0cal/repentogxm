cmake_minimum_required(VERSION 3.19)

# Knob oracle for ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES: text pins on the
# production CMakeLists (one cache entry, one validation call, one
# source-scoped definition emitted only for a non-default value) and the
# validator's accept/reject vectors run through cmake -P probes.
#   cmake -DISAAC_HEAP_BYTES_CMAKE_ROOT=<recomp> \
#         -DISAAC_HEAP_BYTES_CMAKE_OUT=<scratch dir> \
#         -P recomp/vita/test_heap_overflow_mspace_bytes_cmake.cmake

foreach(_required ISAAC_HEAP_BYTES_CMAKE_ROOT ISAAC_HEAP_BYTES_CMAKE_OUT)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "${_required} is required")
  endif()
endforeach()

get_filename_component(ISAAC_HEAP_BYTES_CMAKE_ROOT
  "${ISAAC_HEAP_BYTES_CMAKE_ROOT}" ABSOLUTE)
get_filename_component(ISAAC_HEAP_BYTES_CMAKE_OUT
  "${ISAAC_HEAP_BYTES_CMAKE_OUT}" ABSOLUTE)
set(_module
  "${ISAAC_HEAP_BYTES_CMAKE_ROOT}/vita/heap_overflow_mspace_bytes.cmake")
set(_production "${ISAAC_HEAP_BYTES_CMAKE_ROOT}/vita/CMakeLists.txt")
if(NOT EXISTS "${_module}" OR NOT EXISTS "${_production}")
  message(FATAL_ERROR "overflow mspace bytes module or production CMake is missing")
endif()

file(READ "${_module}" _module_text)
file(READ "${_production}" _production_text)

function(_count_occurrences text needle out)
  set(_count 0)
  set(_rest "${text}")
  string(LENGTH "${needle}" _needle_len)
  while(TRUE)
    string(FIND "${_rest}" "${needle}" _found)
    if(_found EQUAL -1)
      break()
    endif()
    math(EXPR _count "${_count} + 1")
    math(EXPR _skip "${_found} + ${_needle_len}")
    string(SUBSTRING "${_rest}" ${_skip} -1 _rest)
  endwhile()
  set(${out} ${_count} PARENT_SCOPE)
endfunction()

foreach(_needle
    "set(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_DEFAULT \"0x007d5000\")"
    "set(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_MIN \"0x007d5000\")"
    "set(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_MAX \"0x03ffd000\")"
    "function(isaac_validate_heap_overflow_mspace_bytes value feature_on)"
    "PARENT_SCOPE")
  string(FIND "${_module_text}" "${_needle}" _found)
  if(_found EQUAL -1)
    message(FATAL_ERROR "overflow mspace bytes module lost: ${_needle}")
  endif()
endforeach()
foreach(_forbidden
    "target_compile_definitions"
    "set_property"
    "ISAAC_VITA_HEAP_OVERFLOW_MSPACE requires translated runtime")
  string(FIND "${_module_text}" "${_forbidden}" _found)
  if(NOT _found EQUAL -1)
    message(FATAL_ERROR
      "overflow mspace bytes module must stay a pure validator: ${_forbidden}")
  endif()
endforeach()

foreach(_needle_count
    "set(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES \"0x007d5000\" CACHE STRING|1"
    "include(\"\${ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_MODULE}\")|1"
    "isaac_validate_heap_overflow_mspace_bytes(|1"
    "if(NOT ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_IS_DEFAULT)|1"
    "ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES=\${ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_NORMALIZED}U|1"
    "ISAAC_VITA_HEAP_OVERFLOW_MSPACE=1|1")
  string(REPLACE "|" ";" _pair "${_needle_count}")
  list(GET _pair 0 _needle)
  list(GET _pair 1 _expected)
  _count_occurrences("${_production_text}" "${_needle}" _actual)
  if(NOT _actual EQUAL _expected)
    message(FATAL_ERROR
      "production CMake has ${_actual} of '${_needle}' (expected ${_expected})")
  endif()
endforeach()
# The knob definition names exactly the five owner sources, in this order,
# inside one set_property call.  The block is the text from its if() to the
# first endif() after it, whatever its length (no fixed window).
string(FIND "${_production_text}"
  "if(NOT ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_IS_DEFAULT)" _block_start)
if(_block_start EQUAL -1)
  message(FATAL_ERROR "knob definition block if() is missing")
endif()
string(SUBSTRING "${_production_text}" ${_block_start} -1 _block)
string(FIND "${_block}" "endif()" _block_end)
if(_block_end EQUAL -1)
  message(FATAL_ERROR "knob definition block has no endif()")
endif()
string(SUBSTRING "${_block}" 0 ${_block_end} _block)
set(_owner_offset -1)
foreach(_owner
    "\"\${ISAAC_RUNTIME}/host_vita_heap.c\""
    "\"\${ISAAC_RUNTIME}/entry_vita.c\""
    "\"\${ISAAC_RUNTIME}/host_vita_heap_overflow_mspace.c\""
    "\"\${ISAAC_RUNTIME}/host_vita_room_entry_slab.c\""
    "\"\${ISAAC_RUNTIME}/host_vita_room_entry_external.c\""
    "APPEND PROPERTY COMPILE_DEFINITIONS"
    "ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES=\${ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_NORMALIZED}U")
  string(FIND "${_block}" "${_owner}" _found)
  if(_found EQUAL -1 OR _found LESS _owner_offset)
    message(FATAL_ERROR
      "knob definition block lost or reordered its owner: ${_owner}")
  endif()
  set(_owner_offset ${_found})
endforeach()
_count_occurrences("${_block}" "set_property(SOURCE" _set_property_count)
_count_occurrences("${_block}" "\${ISAAC_RUNTIME}/" _owner_count)
if(NOT _set_property_count EQUAL 1 OR NOT _owner_count EQUAL 5)
  message(FATAL_ERROR
    "knob definition block must be one set_property over five owners "
    "(got ${_set_property_count} / ${_owner_count})")
endif()

set(_probe_script "${ISAAC_HEAP_BYTES_CMAKE_OUT}/probe.cmake")
file(MAKE_DIRECTORY "${ISAAC_HEAP_BYTES_CMAKE_OUT}")
file(WRITE "${_probe_script}" [=[
cmake_minimum_required(VERSION 3.19)
include("${PROBE_MODULE}")
isaac_validate_heap_overflow_mspace_bytes("${PROBE_VALUE}" "${PROBE_FEATURE}")
message(STATUS
  "probe normalized=${ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_NORMALIZED}"
  " is_default=${ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_IS_DEFAULT}"
  " decimal=${ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_DECIMAL}")
]=])

set(_accepted 0)
set(_rejected 0)
function(_expect_accept value feature normalized is_default decimal)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DPROBE_MODULE=${_module}"
      "-DPROBE_VALUE=${value}"
      "-DPROBE_FEATURE=${feature}"
      -P "${_probe_script}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR
      "knob rejected '${value}' with feature ${feature}:\n${_stdout}\n${_stderr}")
  endif()
  set(_expected
    "probe normalized=${normalized} is_default=${is_default} decimal=${decimal}")
  string(FIND "${_stdout}" "${_expected}" _found)
  if(_found EQUAL -1)
    message(FATAL_ERROR
      "knob accepted '${value}' but reported the wrong result; expected "
      "'${_expected}' in:\n${_stdout}")
  endif()
  math(EXPR _accepted "${_accepted} + 1")
  set(_accepted ${_accepted} PARENT_SCOPE)
endfunction()
function(_expect_reject value feature needle)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DPROBE_MODULE=${_module}"
      "-DPROBE_VALUE=${value}"
      "-DPROBE_FEATURE=${feature}"
      -P "${_probe_script}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
  if(_result EQUAL 0)
    message(FATAL_ERROR
      "knob accepted invalid '${value}' with feature ${feature}:\n${_stdout}")
  endif()
  # CMake wraps FATAL_ERROR text at about 80 columns; compare on one line.
  set(_failure "${_stdout}\n${_stderr}")
  string(REGEX REPLACE "[ \t\r\n]+" " " _failure "${_failure}")
  string(FIND "${_failure}" "${needle}" _found)
  if(_found EQUAL -1)
    message(FATAL_ERROR
      "knob rejected '${value}' for the wrong reason (wanted '${needle}'):\n"
      "${_failure}")
  endif()
  string(FIND "${_failure}"
    "ISAAC_VITA_HEAP_OVERFLOW_MSPACE requires translated runtime" _foreign)
  if(NOT _foreign EQUAL -1)
    message(FATAL_ERROR
      "knob rejection reused the feature-dependency needle for '${value}'")
  endif()
  math(EXPR _rejected "${_rejected} + 1")
  set(_rejected ${_rejected} PARENT_SCOPE)
endfunction()

# Default: accepted with the pool OFF and ON, reported as the default.
_expect_accept(0x007d5000 OFF 0x007d5000 TRUE 8212480)
_expect_accept(0x007d5000 ON 0x007d5000 TRUE 8212480)
# The 24 MiB-class value measured for the next release, lower-case normalised.
_expect_accept(0x017fd000 ON 0x017fd000 FALSE 25153536)
_expect_accept(0x017FD000 ON 0x017fd000 FALSE 25153536)
# Range ends.
_expect_accept(0x03ffd000 ON 0x03ffd000 FALSE 67096576)
# A non-default value needs the pool.
_expect_reject(0x017fd000 OFF "needs ISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON")
_expect_reject(0x017fd000 "" "needs ISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON")
# Lowest set bit must be exactly 0x1000.
_expect_reject(0x007d4000 ON "lowest set bit")
_expect_reject(0x00800000 ON "lowest set bit")
_expect_reject(0x007d5800 ON "lowest set bit")
_expect_reject(0x017fe000 ON "lowest set bit")
# Range.
_expect_reject(0x007d3000 ON "must lie in")
_expect_reject(0x00601000 ON "must lie in")
_expect_reject(0x03fff000 ON "must lie in")
_expect_reject(0x04001000 ON "must lie in")
# Form: 0x + exactly 8 hex digits.
_expect_reject(0x7d5000 ON "exactly 8 hex digits")
_expect_reject(0x0007d5000 ON "exactly 8 hex digits")
_expect_reject(8212480 ON "exactly 8 hex digits")
_expect_reject(0x007d500g ON "exactly 8 hex digits")
_expect_reject("" ON "exactly 8 hex digits")
_expect_reject("24m" ON "exactly 8 hex digits")

message(STATUS
  "Vita heap overflow mspace bytes CMake knob oracle: production pins + "
  "${_accepted} accepted / ${_rejected} rejected vectors PASS")
