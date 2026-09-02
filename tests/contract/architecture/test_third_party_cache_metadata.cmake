cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED PROJECT_SOURCE_DIR OR NOT DEFINED TEST_ROOT)
  message(FATAL_ERROR "PROJECT_SOURCE_DIR and TEST_ROOT are required")
endif()

include("${PROJECT_SOURCE_DIR}/cmake/ThirdPartyCacheMetadata.cmake")

set(_cache_dir "${TEST_ROOT}/sample_cache")
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${_cache_dir}")

edgeflow_prepare_third_party_cache(
  NAME sample_header
  VERSION 1.2.3
  SOURCE_SHA256 abcdef
  CACHE_DIR "${_cache_dir}"
  KIND HEADER_ONLY
  MARKER_ROOT "${TEST_ROOT}/expected"
  OUT_VALID _valid
  OUT_MARKER _expected_marker)
if(_valid)
  message(FATAL_ERROR "A cache without a marker must not be accepted")
endif()

file(READ "${_expected_marker}" _expected_metadata)
file(WRITE "${_cache_dir}/.edgeflow-cache-fingerprint"
           "${_expected_metadata}")
edgeflow_prepare_third_party_cache(
  NAME sample_header
  VERSION 1.2.3
  SOURCE_SHA256 abcdef
  CACHE_DIR "${_cache_dir}"
  KIND HEADER_ONLY
  MARKER_ROOT "${TEST_ROOT}/expected"
  OUT_VALID _valid
  OUT_MARKER _unused_marker)
if(NOT _valid)
  message(FATAL_ERROR "A matching cache marker must be accepted")
endif()

file(APPEND "${_cache_dir}/.edgeflow-cache-fingerprint" "tampered=true\n")
edgeflow_prepare_third_party_cache(
  NAME sample_header
  VERSION 1.2.3
  SOURCE_SHA256 abcdef
  CACHE_DIR "${_cache_dir}"
  KIND HEADER_ONLY
  MARKER_ROOT "${TEST_ROOT}/expected"
  OUT_VALID _valid
  OUT_MARKER _unused_marker)
if(_valid)
  message(FATAL_ERROR "A modified cache marker must not be accepted")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
