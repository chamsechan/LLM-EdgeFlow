include_guard(GLOBAL)

include(CMakeParseArguments)

# Prepare an inspectable cache marker and report whether an existing cache was
# produced for the same pinned source and ABI-relevant build environment.
function(edgeflow_prepare_third_party_cache)
  set(_one_value_args
      NAME VERSION SOURCE_SHA256 CACHE_DIR KIND MARKER_ROOT OUT_VALID OUT_MARKER)
  set(_multi_value_args ABI_OPTIONS)
  cmake_parse_arguments(ARG "" "${_one_value_args}" "${_multi_value_args}"
                        ${ARGN})

  foreach(_required NAME VERSION SOURCE_SHA256 CACHE_DIR KIND OUT_VALID OUT_MARKER)
    if(NOT ARG_${_required})
      message(FATAL_ERROR
        "edgeflow_prepare_third_party_cache requires ${_required}")
    endif()
  endforeach()
  if(NOT ARG_KIND MATCHES "^(HEADER_ONLY|PREBUILT|STATIC)$")
    message(FATAL_ERROR
      "Unsupported third-party cache kind '${ARG_KIND}'")
  endif()

  set(_basis
      "schema=1\nname=${ARG_NAME}\nversion=${ARG_VERSION}\nsource_sha256=${ARG_SOURCE_SHA256}\nkind=${ARG_KIND}\n")
  if(ARG_KIND STREQUAL "PREBUILT" OR ARG_KIND STREQUAL "STATIC")
    string(APPEND _basis
      "system_name=${CMAKE_SYSTEM_NAME}\nsystem_processor=${CMAKE_SYSTEM_PROCESSOR}\npointer_size=${CMAKE_SIZEOF_VOID_P}\n")
  endif()
  if(ARG_KIND STREQUAL "STATIC")
    string(APPEND _basis
      "c_compiler=${CMAKE_C_COMPILER_ID}-${CMAKE_C_COMPILER_VERSION}\n"
      "cxx_compiler=${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}\n"
      "cxx_standard=${CMAKE_CXX_STANDARD}\n")
  endif()
  if(ARG_ABI_OPTIONS)
    list(SORT ARG_ABI_OPTIONS)
    string(JOIN "," _abi_options ${ARG_ABI_OPTIONS})
  else()
    set(_abi_options "none")
  endif()
  string(APPEND _basis "abi_options=${_abi_options}\n")
  string(SHA256 _fingerprint "${_basis}")
  set(_metadata "${_basis}fingerprint=${_fingerprint}\n")

  if(ARG_MARKER_ROOT)
    set(_marker_dir "${ARG_MARKER_ROOT}")
  else()
    set(_marker_dir "${CMAKE_BINARY_DIR}/third_party_cache_markers")
  endif()
  file(MAKE_DIRECTORY "${_marker_dir}")
  set(_expected_marker "${_marker_dir}/${ARG_NAME}.txt")
  file(WRITE "${_expected_marker}" "${_metadata}")

  set(_cache_marker "${ARG_CACHE_DIR}/.edgeflow-cache-fingerprint")
  set(_valid OFF)
  if(EXISTS "${_cache_marker}")
    file(READ "${_cache_marker}" _cached_metadata)
    if(_cached_metadata STREQUAL _metadata)
      set(_valid ON)
    else()
      message(STATUS
        "[3rdparty] Ignoring incompatible ${ARG_NAME} cache: ${ARG_CACHE_DIR}")
    endif()
  elseif(EXISTS "${ARG_CACHE_DIR}")
    message(STATUS
      "[3rdparty] Ignoring unverified ${ARG_NAME} cache: ${ARG_CACHE_DIR}")
  endif()

  set(${ARG_OUT_VALID} "${_valid}" PARENT_SCOPE)
  set(${ARG_OUT_MARKER} "${_expected_marker}" PARENT_SCOPE)
endfunction()
