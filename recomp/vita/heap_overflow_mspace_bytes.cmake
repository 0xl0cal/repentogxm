include_guard(GLOBAL)

# Second-tier guest-heap pool size (ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES).
# The USER_RW SceClibMspace served after newlib exhaustion is one memblock
# whose request must keep 0x1000 as its lowest set bit: Vita3K's USER_RW
# allocator retains one extra low-bit quantum (measured), so its retained
# span is request + 4 KiB; whether hardware retains it is unmeasured.
# RETAINED, the combined ledger+pool budgets and the RoomConfig slab
# remainder derive from this one value in the runtime headers; CMake emits a
# definition only for a non-default value so the default build's compile
# commands and objects stay byte-identical.
#
# isaac_validate_heap_overflow_mspace_bytes(<value> <feature_on>) sets in the
# caller's scope:
#   ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_NORMALIZED  lower-case 0x + 8 hex digits
#   ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_IS_DEFAULT  TRUE/FALSE
#   ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_DECIMAL     byte count
set(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_DEFAULT "0x007d5000")
# Never below the shipped, device-proven pool: the RoomConfig slab needs its
# whole 6 MiB raw backing plus a remainder.  The ceiling is a 64 MiB class;
# every byte here is USER_RW taken before vglInit from the vitaGL RAM pool.
set(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_MIN "0x007d5000")
set(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_MAX "0x03ffd000")

function(isaac_validate_heap_overflow_mspace_bytes value feature_on)
  string(LENGTH "${value}" _len)
  if(NOT value MATCHES "^0x[0-9a-fA-F]+$" OR NOT _len EQUAL 10)
    message(FATAL_ERROR
      "ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES must be 0x followed by exactly "
      "8 hex digits (got '${value}')")
  endif()
  string(TOLOWER "${value}" _norm)
  math(EXPR _bytes "${_norm}")
  math(EXPR _low "${_bytes} & (~${_bytes} + 1)")
  if(NOT _low EQUAL 4096)
    message(FATAL_ERROR
      "ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES must have 0x1000 as its lowest "
      "set bit (one retained 4 KiB quantum); got ${_norm}, low bit ${_low}")
  endif()
  math(EXPR _min "${ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_MIN}")
  math(EXPR _max "${ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_MAX}")
  if(_bytes LESS _min OR _bytes GREATER _max)
    message(FATAL_ERROR
      "ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES must lie in "
      "${ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_MIN}.."
      "${ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_MAX} (got ${_norm})")
  endif()
  if(_norm STREQUAL ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_DEFAULT)
    set(_is_default TRUE)
  else()
    set(_is_default FALSE)
    if(NOT feature_on)
      message(FATAL_ERROR
        "ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES=${_norm} needs "
        "ISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON; only the default "
        "${ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_DEFAULT} is accepted while "
        "the pool is OFF")
    endif()
  endif()
  set(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_NORMALIZED "${_norm}" PARENT_SCOPE)
  set(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_IS_DEFAULT ${_is_default} PARENT_SCOPE)
  set(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES_DECIMAL ${_bytes} PARENT_SCOPE)
endfunction()
