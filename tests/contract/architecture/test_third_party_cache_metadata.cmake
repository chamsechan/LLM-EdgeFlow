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

# Dependency failures must be diagnosed before package loading or network I/O.
function(expect_kite_failure expected)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -DENABLE_KITELLM=ON
            -DENABLE_LLAMACPP=OFF -DCMAKE_SYSTEM_NAME=Linux
            -DCMAKE_SYSTEM_PROCESSOR=x86_64
            "-DLLM_EDGEFLOW_3RDPARTY_DIR=${TEST_ROOT}/deps"
            ${ARGN} -P "${PROJECT_SOURCE_DIR}/cmake/KiteLlm.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${expected}")
    message(FATAL_ERROR "Expected kiteLLM failure '${expected}': ${output}${error}")
  endif()
endfunction()
expect_kite_failure("symbol collisions" -DENABLE_LLAMACPP=ON)
expect_kite_failure("KITELLM_ROOT is obsolete" -DKITELLM_ROOT=/obsolete/sdk)
expect_kite_failure("supports Linux" -DCMAKE_SYSTEM_NAME=UnsupportedOS)
file(MAKE_DIRECTORY "${TEST_ROOT}/deps/kite_llm/v0.1.0/x64")
file(WRITE "${TEST_ROOT}/deps/kite_llm/v0.1.0/x64/kiteLLM-x64.tar.gz" "tampered")
expect_kite_failure("SHA-256 mismatch")

file(REMOVE_RECURSE "${TEST_ROOT}")
