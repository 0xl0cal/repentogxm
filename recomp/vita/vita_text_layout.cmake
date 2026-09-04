# ISAAC_VITA_LAYOUT_HUB: veneer-free .text placement (see vita_text_layout.py).
#
# The 23 MB Thumb-2 image exceeds the +/-16 MB BL/B.W reach, so the default GNU
# ld script (hot region first, libraries last) makes every far call go through
# an ARM-state long-branch veneer.  This function generates a copy of the
# toolchain's own default script whose .text block places the hot region, the
# runtime and the libraries in the middle of the image between two halves of
# cold generated units ordered by a committed order file, links with it, and
# verifies the map after the link.  Objects are untouched; only placement moves.
#
# Growth budget: the hub is in reach of both ends of .text with ~3.2 MB to spare
# (gpr-v3), but cold<->cold edges across the hub are only kept in reach by the
# committed ORDER, derived from one particular corpus.  The post-link check
# therefore also measures every branch edge of the link (readelf -rW) and prints
# cold_cold_headroom / hub_headroom: how far the generated code may drift before
# the first veneer appears and the MAX_VENEERS pin fails closed.  After any
# corpus change (emitter, repack, hot set) the recipe is: link once, then
# `ninja isaac_vita_layout_order` (rewrites ORDER from that link), then build
# again (ORDER is a configure dependency, the script a link dependency).
#
# isaac_vita_layout_hub(<target>
#   PYTHON <python3> SCRIPT <vita_text_layout.py> ORDER <translated_cpu_unit_order.txt>
#   LINKER <arm-vita-eabi-ld> NM <arm-vita-eabi-nm> READELF <arm-vita-eabi-readelf>
#   OBJECT_SUFFIX <.c.obj> MAP <linker map written by the target's -Wl,-Map>
#   ELF <linked ELF path, for the order target> OUTPUT_DIR <dir>
#   MAX_VENEERS <n> UNITS <generated unit sources...>)
# Sets ISAAC_VITA_LAYOUT_HUB_SUMMARY in the caller's scope and defines the
# isaac_vita_layout_order target (not in ALL).
function(isaac_vita_layout_hub target)
  set(one_value PYTHON SCRIPT ORDER LINKER NM READELF OBJECT_SUFFIX MAP ELF OUTPUT_DIR
      MAX_VENEERS)
  set(multi_value UNITS)
  cmake_parse_arguments(ARG "" "${one_value}" "${multi_value}" ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "isaac_vita_layout_hub: unexpected arguments: ${ARG_UNPARSED_ARGUMENTS}")
  endif()
  foreach(required IN LISTS one_value)
    if("${ARG_${required}}" STREQUAL "")
      message(FATAL_ERROR "isaac_vita_layout_hub: ${required} is required")
    endif()
  endforeach()
  if(NOT ARG_UNITS)
    message(FATAL_ERROR "isaac_vita_layout_hub: UNITS is empty")
  endif()
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "isaac_vita_layout_hub: no such target ${target}")
  endif()
  foreach(input IN ITEMS SCRIPT ORDER LINKER NM READELF)
    if(NOT EXISTS "${ARG_${input}}")
      message(FATAL_ERROR
        "isaac_vita_layout_hub: ${input} does not exist: ${ARG_${input}}")
    endif()
  endforeach()
  if(NOT ARG_MAX_VENEERS MATCHES "^[0-9]+$")
    message(FATAL_ERROR
      "isaac_vita_layout_hub: MAX_VENEERS must be a non-negative integer: "
      "${ARG_MAX_VENEERS}")
  endif()

  file(MAKE_DIRECTORY "${ARG_OUTPUT_DIR}")
  set(units_file "${ARG_OUTPUT_DIR}/generated_units.txt")
  set(script_out "${ARG_OUTPUT_DIR}/isaac_text_layout.ld")
  set(halves_out "${ARG_OUTPUT_DIR}/resolved_unit_halves.txt")
  string(REPLACE ";" "\n" units_text "${ARG_UNITS}")
  file(WRITE "${units_file}" "${units_text}\n")

  # Configure time: the script is a pure function of the toolchain's default
  # script, the unit list and the committed order, all configure dependencies.
  execute_process(
    COMMAND "${ARG_PYTHON}" "${ARG_SCRIPT}" script
      --order "${ARG_ORDER}"
      --units-file "${units_file}"
      --ld "${ARG_LINKER}"
      --object-suffix "${ARG_OBJECT_SUFFIX}"
      --output "${script_out}"
      --write-halves "${halves_out}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE summary
    ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "veneer-free .text layout script failed: ${error}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ARG_ORDER}" "${ARG_SCRIPT}")

  target_link_options("${target}" PRIVATE "-Wl,-T,${script_out}")
  set_property(TARGET "${target}" APPEND PROPERTY LINK_DEPENDS "${script_out}")

  # Post link: every .text.sorted.* / non-generated section inside the hub, both
  # cold halves where the script put them, the hub within Thumb-2 reach of both
  # ends of .text, no more long-branch veneers than MAX_VENEERS, and the
  # branch-distance census of the link (cold_cold_headroom / hub_headroom).
  add_custom_command(TARGET "${target}" POST_BUILD
    COMMAND "${ARG_PYTHON}" "${ARG_SCRIPT}" check
      --map "${ARG_MAP}"
      --order "${halves_out}"
      --nm "${ARG_NM}"
      --readelf "${ARG_READELF}"
      --elf "$<TARGET_FILE:${target}>"
      --max-veneers "${ARG_MAX_VENEERS}"
      --order-file "${ARG_ORDER}"
    COMMENT "Verifying the veneer-free .text layout of ${target}"
    VERBATIM)

  # Recovery after a corpus change.  Deliberately not dependent on the target:
  # the link output survives a failed POST_BUILD check, and this must be able to
  # run from exactly that state.  ELF is a plain path for the same reason (a
  # $<TARGET_FILE:> here would add the target-level dependency).
  if(TARGET isaac_vita_layout_order)
    message(FATAL_ERROR "isaac_vita_layout_hub: isaac_vita_layout_order already defined")
  endif()
  add_custom_target(isaac_vita_layout_order
    COMMAND "${ARG_PYTHON}" "${ARG_SCRIPT}" order
      --map "${ARG_MAP}"
      --readelf "${ARG_READELF}"
      --elf "${ARG_ELF}"
      --output "${ARG_ORDER}"
    COMMENT "Deriving ${ARG_ORDER} from the current link of ${target}; build again afterwards (reconfigure + relink)"
    VERBATIM USES_TERMINAL)
  set(ISAAC_VITA_LAYOUT_HUB_SUMMARY "${summary}" PARENT_SCOPE)
endfunction()
