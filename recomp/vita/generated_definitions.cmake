include_guard(GLOBAL)

# A configure-local index of public definitions in the immutable, manifest-
# checked corpus. Callers still apply their original exact regex and count
# limits, including duplicate-definition detection. No result is persisted
# between configure runs or shared between paths/canonical/derived sources.
function(isaac_generated_definition_lines source output)
  cmake_parse_arguments(PARSE_ARGV 2 QUERY "" "LIMIT_COUNT;REGEX" "")
  if(QUERY_UNPARSED_ARGUMENTS OR QUERY_KEYWORDS_MISSING_VALUES OR
     NOT DEFINED QUERY_REGEX OR NOT QUERY_REGEX MATCHES "^\\^void sub_")
    message(FATAL_ERROR "unsupported generated-definition query for ${source}")
  endif()
  if(DEFINED QUERY_LIMIT_COUNT AND
     NOT QUERY_LIMIT_COUNT MATCHES "^[0-9]+$")
    message(FATAL_ERROR "invalid generated-definition count limit")
  endif()
  # Definitions have no semicolons. Keeping the exact signature contract also
  # avoids list(FILTER)'s normalization of escaped semicolons in prototypes.
  set(signature [=[\(CPU \*__restrict c\)$]=])
  string(LENGTH "${QUERY_REGEX}" query_length)
  string(LENGTH "${signature}" signature_length)
  math(EXPR signature_offset "${query_length} - ${signature_length}")
  if(signature_offset LESS 0)
    message(FATAL_ERROR "generated-definition query lacks the exact signature")
  endif()
  string(SUBSTRING "${QUERY_REGEX}" ${signature_offset} -1 query_signature)
  if(NOT query_signature STREQUAL signature)
    message(FATAL_ERROR "generated-definition query lacks the exact signature")
  endif()
  string(SHA256 key "${source}")
  get_property(ready GLOBAL PROPERTY "ISAAC_DEFINITIONS_${key}" SET)
  if(NOT ready)
    # Exact definitions are a superset of every accepted anchored caller query.
    # Keep duplicates and source order; do not cache a unique symbol set.
    file(STRINGS "${source}" definitions
      REGEX "^void sub_[0-9a-f]+\\(CPU \\*__restrict c\\)$")
    set_property(GLOBAL PROPERTY "ISAAC_DEFINITIONS_${key}" "${definitions}")
  else()
    get_property(definitions GLOBAL PROPERTY "ISAAC_DEFINITIONS_${key}")
  endif()
  list(FILTER definitions INCLUDE REGEX "${QUERY_REGEX}")
  if(DEFINED QUERY_LIMIT_COUNT AND QUERY_LIMIT_COUNT GREATER 0)
    list(LENGTH definitions count)
    if(count GREATER QUERY_LIMIT_COUNT)
      list(SUBLIST definitions 0 ${QUERY_LIMIT_COUNT} definitions)
    endif()
  endif()
  set(${output} "${definitions}" PARENT_SCOPE)
endfunction()
