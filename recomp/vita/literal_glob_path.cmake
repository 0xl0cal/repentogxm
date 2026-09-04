#[=======================================================================[.rst:
literal_glob_path
-----------------

``file(GLOB)`` parses every path component as a glob expression.  These
helpers quote metacharacters in the one generated-directory exception before
appending the intentional ``/*`` wildcard.  The unescaped directory remains
the ``RELATIVE`` base.  All other CMake-visible build inputs use the strict
portable absolute-path policy because VitaSDK's non-``VERBATIM`` package
commands cannot safely carry shell/Ninja metacharacters.
#]=======================================================================]

function(_isaac_absolute_path_tail input_path description output_variable)
  if(NOT ARGC EQUAL 3)
    message(FATAL_ERROR
      "_isaac_absolute_path_tail expects a path, description, and output")
  endif()
  if("${input_path}" STREQUAL "")
    message(FATAL_ERROR "${description} must not be empty")
  endif()
  string(FIND "${input_path}" ";" _isaac_semicolon)
  if(NOT _isaac_semicolon EQUAL -1)
    message(FATAL_ERROR
      "${description} contains forbidden CMake list separator ';'")
  endif()
  if("${input_path}" MATCHES "^//" OR "${input_path}" MATCHES "^\\\\")
    message(FATAL_ERROR "${description} must not be a UNC path")
  endif()
  string(LENGTH "${input_path}" _isaac_length)
  math(EXPR _isaac_last "${_isaac_length} - 1")
  string(SUBSTRING "${input_path}" ${_isaac_last} 1 _isaac_last_byte)
  if(_isaac_last_byte STREQUAL "/" OR _isaac_last_byte STREQUAL "\\")
    message(FATAL_ERROR "${description} must not end in a path separator")
  endif()
  # These are host filesystem paths even under a cross toolchain.  WIN32 is
  # the target-system predicate and is unset before project(); never use it
  # to interpret drive prefixes.
  if(CMAKE_HOST_WIN32)
    if(NOT "${input_path}" MATCHES "^[A-Za-z]:/")
      message(FATAL_ERROR
        "${description} must be canonical absolute with an X:/ prefix")
    endif()
    string(SUBSTRING "${input_path}" 3 -1 _isaac_tail)
  else()
    if(NOT "${input_path}" MATCHES "^/" OR "${input_path}" MATCHES "^//")
      message(FATAL_ERROR
        "${description} must be canonical absolute with a '/' prefix")
    endif()
    string(SUBSTRING "${input_path}" 1 -1 _isaac_tail)
  endif()
  if("${_isaac_tail}" STREQUAL "")
    message(FATAL_ERROR "${description} must contain a non-root path segment")
  endif()
  string(FIND "${_isaac_tail}" "//" _isaac_empty_segment)
  if(NOT _isaac_empty_segment EQUAL -1)
    message(FATAL_ERROR "${description} contains an empty path segment")
  endif()
  set(${output_variable} "${_isaac_tail}" PARENT_SCOPE)
endfunction()

function(_isaac_path_segments input_path description output_variable)
  if(NOT ARGC EQUAL 3)
    message(FATAL_ERROR
      "_isaac_path_segments expects a path, description, and output")
  endif()
  _isaac_absolute_path_tail(
    "${input_path}" "${description}" _isaac_tail)
  string(REPLACE "/" ";" _isaac_segments "${_isaac_tail}")
  foreach(_isaac_segment IN LISTS _isaac_segments)
    if("${_isaac_segment}" STREQUAL "")
      message(FATAL_ERROR "${description} contains an empty path segment")
    endif()
    if(_isaac_segment STREQUAL "." OR _isaac_segment STREQUAL "..")
      message(FATAL_ERROR
        "${description} contains forbidden '${_isaac_segment}' path segment")
    endif()
  endforeach()
  set(${output_variable} "${_isaac_segments}" PARENT_SCOPE)
endfunction()

function(isaac_validate_strict_absolute_path input_path description)
  if(NOT ARGC EQUAL 2)
    message(FATAL_ERROR
      "isaac_validate_strict_absolute_path expects a path and description")
  endif()
  _isaac_path_segments("${input_path}" "${description}" _isaac_segments)
  foreach(_isaac_segment IN LISTS _isaac_segments)
    if(NOT "${_isaac_segment}" MATCHES "^[A-Za-z0-9._-]+$")
      message(FATAL_ERROR
        "${description} has a non-portable strict path segment: "
        "'${_isaac_segment}'")
    endif()
  endforeach()
endfunction()

function(isaac_validate_generated_absolute_path input_path description)
  if(NOT ARGC EQUAL 2)
    message(FATAL_ERROR
      "isaac_validate_generated_absolute_path expects a path and description")
  endif()
  _isaac_path_segments("${input_path}" "${description}" _isaac_segments)
  foreach(_isaac_segment IN LISTS _isaac_segments)
    string(LENGTH "${_isaac_segment}" _isaac_segment_length)
    math(EXPR _isaac_segment_last "${_isaac_segment_length} - 1")
    set(_isaac_generated_allowed
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-*?[]")
    set(_isaac_open_count 0)
    set(_isaac_close_count 0)
    foreach(_isaac_index RANGE 0 ${_isaac_segment_last})
      string(SUBSTRING "${_isaac_segment}" ${_isaac_index} 1 _isaac_byte)
      string(FIND "${_isaac_generated_allowed}" "${_isaac_byte}"
        _isaac_allowed_index)
      if(_isaac_allowed_index EQUAL -1)
        message(FATAL_ERROR
          "${description} has a non-portable generated path segment: "
          "'${_isaac_segment}'")
      endif()
      if(_isaac_byte STREQUAL "[")
        math(EXPR _isaac_open_count "${_isaac_open_count} + 1")
      elseif(_isaac_byte STREQUAL "]")
        math(EXPR _isaac_close_count "${_isaac_close_count} + 1")
      endif()
    endforeach()
    if(NOT _isaac_open_count EQUAL _isaac_close_count)
      message(FATAL_ERROR
        "${description} has unequal '[' and ']' counts in generated path "
        "segment '${_isaac_segment}' "
        "(${_isaac_open_count} != ${_isaac_close_count})")
    endif()
  endforeach()
endfunction()

function(isaac_escape_literal_glob_path input_path output_variable)
  if(NOT ARGC EQUAL 2)
    message(FATAL_ERROR
      "isaac_escape_literal_glob_path expects an input and output variable")
  endif()
  isaac_validate_generated_absolute_path("${input_path}" "literal glob path")

  # Order matters: the '[' replacement introduces brackets which must not be
  # visited again.  A ']' outside a character class is already literal.
  string(REPLACE "[" "[[]" _isaac_escaped "${input_path}")
  string(REPLACE "*" "[*]" _isaac_escaped "${_isaac_escaped}")
  string(REPLACE "?" "[?]" _isaac_escaped "${_isaac_escaped}")
  set(${output_variable} "${_isaac_escaped}" PARENT_SCOPE)
endfunction()

function(isaac_literal_directory_files input_directory output_variable)
  if(NOT ARGC EQUAL 2)
    message(FATAL_ERROR
      "isaac_literal_directory_files expects a directory and output variable")
  endif()
  if(NOT IS_DIRECTORY "${input_directory}")
    message(FATAL_ERROR
      "literal glob input is not a directory: ${input_directory}")
  endif()
  isaac_escape_literal_glob_path(
    "${input_directory}" _isaac_literal_directory_pattern)
  file(GLOB _isaac_literal_directory_entries
       LIST_DIRECTORIES FALSE RELATIVE "${input_directory}"
       "${_isaac_literal_directory_pattern}/*")
  list(SORT _isaac_literal_directory_entries)
  set(${output_variable} "${_isaac_literal_directory_entries}" PARENT_SCOPE)
endfunction()
