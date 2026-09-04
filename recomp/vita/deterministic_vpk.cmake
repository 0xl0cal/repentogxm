#[=======================================================================[.rst:
deterministic_vpk
-----------------

Wrap VitaSDK's existing ``vita_create_vpk`` command without adding a packaging
edge.  Inputs are staged by the Python wrapper at one even DOS-safe timestamp;
the original packer remains the only archive producer.
#]=======================================================================]

function(isaac_enable_deterministic_vpk
         real_packer python_executable wrapper source_date_epoch)
  if(NOT ARGC EQUAL 4)
    message(FATAL_ERROR
      "isaac_enable_deterministic_vpk expects packer, Python, wrapper, epoch")
  endif()
  foreach(_path real_packer wrapper)
    set(_name "${_path}")
    set(_value "${${_path}}")
    if(NOT IS_ABSOLUTE "${_value}" OR NOT EXISTS "${_value}" OR
       IS_DIRECTORY "${_value}" OR IS_SYMLINK "${_value}")
      message(FATAL_ERROR
        "deterministic VPK ${_name} must be an absolute regular non-symlink: "
        "${_value}")
    endif()
  endforeach()
  if(NOT IS_ABSOLUTE "${python_executable}" OR
     NOT EXISTS "${python_executable}" OR IS_DIRECTORY "${python_executable}")
    message(FATAL_ERROR
      "deterministic VPK Python must be an absolute executable: "
      "${python_executable}")
  endif()
  if(NOT "${source_date_epoch}" MATCHES "^(0|[1-9][0-9]*)$")
    message(FATAL_ERROR
      "SOURCE_DATE_EPOCH must be canonical decimal")
  endif()
  math(EXPR _even_epoch
       "${source_date_epoch} - (${source_date_epoch} % 2)")
  if(_even_epoch LESS 315532800 OR _even_epoch GREATER 4354819198)
    message(FATAL_ERROR
      "SOURCE_DATE_EPOCH is outside the DOS timestamp range")
  endif()
  set(ISAAC_VITA_REAL_PACK_VPK "${real_packer}" CACHE FILEPATH
      "Unwrapped VitaSDK vita-pack-vpk executable" FORCE)
  set(VITA_PACK_VPK
      "${python_executable};${wrapper};--real-packer;${real_packer};--source-date-epoch;${source_date_epoch};--"
      PARENT_SCOPE)
endfunction()

function(isaac_append_deterministic_vpk_dependencies
         vpk_output wrapper real_packer)
  if(NOT ARGC EQUAL 3)
    message(FATAL_ERROR
      "isaac_append_deterministic_vpk_dependencies expects output/wrapper/packer")
  endif()
  add_custom_command(
    OUTPUT "${vpk_output}"
    APPEND
    DEPENDS "${wrapper}" "${real_packer}")
endfunction()
