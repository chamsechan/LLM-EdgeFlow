cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED PROJECT_SOURCE_DIR OR NOT DEFINED TEST_ROOT)
  message(FATAL_ERROR "PROJECT_SOURCE_DIR and TEST_ROOT are required")
endif()

include("${PROJECT_SOURCE_DIR}/cmake_ext/ThirdPartyCacheMetadata.cmake")

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

# 依赖失败必须在加载包或访问网络之前诊断出来。
function(expect_kite_failure expected)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -DENABLE_KITELLM=ON
            -DENABLE_LLAMACPP=OFF -DCMAKE_SYSTEM_NAME=Linux
            -DCMAKE_SYSTEM_PROCESSOR=x86_64
            "-DLLM_EDGEFLOW_3RDPARTY_DIR=${TEST_ROOT}/deps"
            ${ARGN} -P "${PROJECT_SOURCE_DIR}/cmake_ext/KiteLlm.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${expected}")
    message(FATAL_ERROR "Expected kiteLLM failure '${expected}': ${output}${error}")
  endif()
endfunction()
expect_kite_failure("symbol collisions" -DENABLE_LLAMACPP=ON)
expect_kite_failure("supports Linux" -DCMAKE_SYSTEM_NAME=UnsupportedOS)
file(MAKE_DIRECTORY "${TEST_ROOT}/deps/kite_llm/v0.1.0/x64")
file(WRITE "${TEST_ROOT}/deps/kite_llm/v0.1.0/x64/kiteLLM-x64.tar.gz" "tampered")
expect_kite_failure("SHA-256 mismatch")

function(expect_whisper_failure expected)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -DENABLE_WHISPERCPP=ON
            "-DLLM_EDGEFLOW_3RDPARTY_DIR=${TEST_ROOT}/deps"
            ${ARGN} -P "${PROJECT_SOURCE_DIR}/cmake_ext/WhisperCpp.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${expected}")
    message(FATAL_ERROR "Expected whisper.cpp failure '${expected}': ${output}${error}")
  endif()
endfunction()
expect_whisper_failure("requires ENABLE_LLAMACPP=ON" -DENABLE_LLAMACPP=OFF)
expect_whisper_failure("cannot coexist with ENABLE_KITELLM=ON" -DENABLE_LLAMACPP=ON -DENABLE_KITELLM=ON)

include("${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_llama_cache.cmake")

# 在当前运行的 CMake 版本上覆盖无缓存的归档路径。
# 仅复用预构建库无法发现不受支持的 FetchContent 选项。
set(_archive_source "${TEST_ROOT}/archive-source")
set(_archive_project "${TEST_ROOT}/archive-project")
file(MAKE_DIRECTORY "${_archive_source}" "${_archive_project}")
file(WRITE "${_archive_source}/fixture.txt" "verified local archive\n")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E tar czf "${TEST_ROOT}/fixture.tar.gz" fixture.txt
  WORKING_DIRECTORY "${_archive_source}" RESULT_VARIABLE _archive_result)
if(NOT _archive_result EQUAL 0)
  message(FATAL_ERROR "Cannot create local dependency archive")
endif()
file(SHA256 "${TEST_ROOT}/fixture.tar.gz" _archive_sha256)
file(WRITE "${_archive_project}/CMakeLists.txt"
  "cmake_minimum_required(VERSION 3.19)\n"
  "project(ArchiveCompatibility NONE)\n"
  "include(\"${PROJECT_SOURCE_DIR}/cmake_ext/ThirdPartyCacheMetadata.cmake\")\n"
  "include(FetchContent)\n"
  "FetchContent_Declare(local_archive URL \"${TEST_ROOT}/fixture.tar.gz\"\n"
  "  URL_HASH SHA256=${_archive_sha256} \${EDGEFLOW_FETCHCONTENT_TIMESTAMP_ARGS})\n"
  "FetchContent_Populate(local_archive)\n"
  "file(READ \"\${local_archive_SOURCE_DIR}/fixture.txt\" content)\n"
  "if(NOT content STREQUAL \"verified local archive\\n\")\n"
  "  message(FATAL_ERROR \"Fetched archive content does not match\")\n"
  "endif()\n")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${_archive_project}" -B "${TEST_ROOT}/archive-build"
  RESULT_VARIABLE _fetch_result OUTPUT_VARIABLE _fetch_output ERROR_VARIABLE _fetch_error)
if(NOT _fetch_result EQUAL 0)
  message(FATAL_ERROR "Local archive population failed: ${_fetch_output}${_fetch_error}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
