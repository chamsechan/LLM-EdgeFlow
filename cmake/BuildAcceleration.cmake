# Developer build acceleration options shared by first-party and fetched targets.

set(LLM_EDGEFLOW_LINKER "auto" CACHE STRING
    "Linker selection: auto, mold, lld, or system")
set_property(CACHE LLM_EDGEFLOW_LINKER PROPERTY STRINGS auto mold lld system)

set(_edgeflow_supported_linkers auto mold lld system)
if(NOT LLM_EDGEFLOW_LINKER IN_LIST _edgeflow_supported_linkers)
  message(FATAL_ERROR
    "Unsupported LLM_EDGEFLOW_LINKER='${LLM_EDGEFLOW_LINKER}'. "
    "Use auto, mold, lld, or system.")
endif()

function(edgeflow_probe_linker linker_name linker_flag output_variable)
  string(TOUPPER "${linker_name}" linker_name_upper)
  set(probe_variable "LLM_EDGEFLOW_${linker_name_upper}_LINKER_WORKS")
  set(saved_required_link_options "${CMAKE_REQUIRED_LINK_OPTIONS}")
  set(CMAKE_REQUIRED_LINK_OPTIONS "${linker_flag}")
  include(CheckCXXSourceCompiles)
  check_cxx_source_compiles("int main() { return 0; }" ${probe_variable})
  set(CMAKE_REQUIRED_LINK_OPTIONS "${saved_required_link_options}")
  set(${output_variable} "${${probe_variable}}" PARENT_SCOPE)
endfunction()

set(_edgeflow_selected_linker system)
set(_edgeflow_linker_flag "")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  find_program(LLM_EDGEFLOW_MOLD_EXECUTABLE mold)
  find_program(LLM_EDGEFLOW_LLD_EXECUTABLE ld.lld)

  if(LLM_EDGEFLOW_LINKER STREQUAL "auto" OR
     LLM_EDGEFLOW_LINKER STREQUAL "mold")
    if(LLM_EDGEFLOW_MOLD_EXECUTABLE)
      edgeflow_probe_linker(mold "-fuse-ld=mold" _edgeflow_mold_works)
      if(_edgeflow_mold_works)
        set(_edgeflow_selected_linker mold)
        set(_edgeflow_linker_flag "-fuse-ld=mold")
      endif()
    endif()
    if(LLM_EDGEFLOW_LINKER STREQUAL "mold" AND
       NOT _edgeflow_selected_linker STREQUAL "mold")
      message(FATAL_ERROR
        "LLM_EDGEFLOW_LINKER=mold was requested, but mold is unavailable or "
        "the compiler-driver link probe failed.")
    endif()
  endif()

  if(_edgeflow_selected_linker STREQUAL "system" AND
     (LLM_EDGEFLOW_LINKER STREQUAL "auto" OR
      LLM_EDGEFLOW_LINKER STREQUAL "lld"))
    if(LLM_EDGEFLOW_LLD_EXECUTABLE)
      edgeflow_probe_linker(lld "-fuse-ld=lld" _edgeflow_lld_works)
      if(_edgeflow_lld_works)
        set(_edgeflow_selected_linker lld)
        set(_edgeflow_linker_flag "-fuse-ld=lld")
      endif()
    endif()
    if(LLM_EDGEFLOW_LINKER STREQUAL "lld" AND
       NOT _edgeflow_selected_linker STREQUAL "lld")
      message(FATAL_ERROR
        "LLM_EDGEFLOW_LINKER=lld was requested, but lld is unavailable or "
        "the compiler-driver link probe failed.")
    endif()
  endif()
elseif(NOT LLM_EDGEFLOW_LINKER STREQUAL "auto" AND
       NOT LLM_EDGEFLOW_LINKER STREQUAL "system")
  message(FATAL_ERROR
    "LLM_EDGEFLOW_LINKER=${LLM_EDGEFLOW_LINKER} requires a GNU- or "
    "Clang-compatible compiler driver.")
endif()

if(_edgeflow_linker_flag)
  add_link_options("${_edgeflow_linker_flag}")
endif()

set(LLM_EDGEFLOW_SELECTED_LINKER "${_edgeflow_selected_linker}" CACHE INTERNAL
    "Resolved linker used by LLM-EdgeFlow targets" FORCE)
message(STATUS "LLM-EdgeFlow linker: ${LLM_EDGEFLOW_SELECTED_LINKER}")
