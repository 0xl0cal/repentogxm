cmake_minimum_required(VERSION 3.19)

if(NOT DEFINED ISAAC_LITERAL_GLOB_HELPER OR
   "${ISAAC_LITERAL_GLOB_HELPER}" STREQUAL "")
  message(FATAL_ERROR "ISAAC_LITERAL_GLOB_HELPER is required")
endif()

include("${ISAAC_LITERAL_GLOB_HELPER}")

if(DEFINED ISAAC_PATH_POLICY_ROLE)
  if(ISAAC_PATH_POLICY_USE_ENV)
    set(ISAAC_PATH_POLICY_INPUT "$ENV{ISAAC_PATH_POLICY_INPUT}")
  elseif(NOT DEFINED ISAAC_PATH_POLICY_INPUT)
    message(FATAL_ERROR
      "ISAAC_PATH_POLICY_INPUT or ISAAC_PATH_POLICY_USE_ENV is required")
  endif()
  if(ISAAC_PATH_POLICY_ROLE STREQUAL "strict")
    isaac_validate_strict_absolute_path(
      "${ISAAC_PATH_POLICY_INPUT}" "strict policy probe")
  elseif(ISAAC_PATH_POLICY_ROLE STREQUAL "generated")
    isaac_validate_generated_absolute_path(
      "${ISAAC_PATH_POLICY_INPUT}" "generated policy probe")
  else()
    message(FATAL_ERROR
      "unknown ISAAC_PATH_POLICY_ROLE='${ISAAC_PATH_POLICY_ROLE}'")
  endif()
  message(STATUS "${ISAAC_PATH_POLICY_ROLE} path policy probe: PASS")
  return()
endif()

foreach(_required ISAAC_LITERAL_GLOB_ROOT ISAAC_LITERAL_GLOB_EXPECTED)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "${_required} is required")
  endif()
endforeach()

if(NOT DEFINED ISAAC_LITERAL_GLOB_EXPECT_ESCAPED)
  message(FATAL_ERROR "ISAAC_LITERAL_GLOB_EXPECT_ESCAPED is required")
endif()

isaac_escape_literal_glob_path(
  "${ISAAC_LITERAL_GLOB_ROOT}" _escaped_root)

if(DEFINED ISAAC_LITERAL_GLOB_EXPECT_ESCAPED AND
   NOT "${_escaped_root}" STREQUAL "${ISAAC_LITERAL_GLOB_EXPECT_ESCAPED}")
  message(FATAL_ERROR
    "literal glob escaping mismatch: '${_escaped_root}' != "
    "'${ISAAC_LITERAL_GLOB_EXPECT_ESCAPED}'")
endif()

isaac_literal_directory_files(
  "${ISAAC_LITERAL_GLOB_ROOT}" _actual_files)
list(SORT ISAAC_LITERAL_GLOB_EXPECTED)
if(NOT "${_actual_files}" STREQUAL "${ISAAC_LITERAL_GLOB_EXPECTED}")
  message(FATAL_ERROR
    "literal directory set mismatch: actual='${_actual_files}' "
    "expected='${ISAAC_LITERAL_GLOB_EXPECTED}'")
endif()

# Production turns each manifest-relative name into an absolute path before it
# enters CMAKE_CONFIGURE_DEPENDS and the target SOURCES property.  Exercise the
# same list boundary: a relative-only glob test would miss CMake's documented
# unequal-bracket/trailing-backslash restrictions on absolute list elements.
set(_absolute_files)
foreach(_relative_file IN LISTS _actual_files)
  list(APPEND _absolute_files
    "${ISAAC_LITERAL_GLOB_ROOT}/${_relative_file}")
endforeach()
list(LENGTH _actual_files _relative_count)
list(LENGTH _absolute_files _absolute_count)
if(NOT _absolute_count EQUAL _relative_count)
  message(FATAL_ERROR
    "production-shaped absolute path list changed length: "
    "${_absolute_count} != ${_relative_count}")
endif()
message(STATUS
  "literal directory set probe: PASS absolute_count=${_absolute_count}")
