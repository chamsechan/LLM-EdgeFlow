# Exercise the production configure/import/archive paths with tiny local targets.
# No llama.cpp download or target-platform compiler is needed for these checks.
set(_llama_fixture "${TEST_ROOT}/llama_fixture")
file(MAKE_DIRECTORY "${_llama_fixture}/upstream/include"
                    "${_llama_fixture}/upstream/ggml/include")
file(WRITE "${_llama_fixture}/upstream/include/llama.h" "// fixture\n")
file(WRITE "${_llama_fixture}/upstream/ggml/include/ggml.h" "// fixture\n")
file(WRITE "${_llama_fixture}/upstream/stub.c" "int cache_fixture(void) { return 0; }\n")
file(WRITE "${_llama_fixture}/upstream/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.16)
project(llama_cache_upstream C)
option(GGML_BLAS "ggml: use BLAS" ${APPLE})
foreach(lib llama ggml ggml-base ggml-cpu)
  add_library(${lib} STATIC stub.c)
endforeach()
if(GGML_BLAS)
  add_library(ggml-blas STATIC stub.c)
endif()
]=])
file(WRITE "${_llama_fixture}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.16)
project(llama_cache_fixture C CXX)
# Simulate platform defaults only; compilation still uses the host toolchain.
set(APPLE ${TEST_APPLE})
include("${EDGEFLOW_SOURCE_DIR}/cmake_ext/ThirdPartyEngines.cmake")
if(NOT "${_LLAMA_FOUND}" STREQUAL "${EXPECT_FOUND}")
  message(FATAL_ERROR "Expected cache hit=${EXPECT_FOUND}, got ${_LLAMA_FOUND}")
endif()
if(EXPECT_BLAS AND NOT TARGET ggml-blas)
  message(FATAL_ERROR "BLAS-enabled configuration must provide ggml-blas")
elseif(NOT EXPECT_BLAS AND TARGET ggml-blas)
  message(FATAL_ERROR "BLAS-disabled configuration must not import stale ggml-blas")
endif()
]=])

function(configure_llama_case step expected_found expected_blas)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${_case_source}" -B "${_case_root}/${step}"
            "-DEDGEFLOW_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DFETCHCONTENT_SOURCE_DIR_LLAMA_CPP_SOURCE=${_llama_fixture}/upstream"
            -DENABLE_ONNXRUNTIME=OFF -DENABLE_KITELLM=OFF -DENABLE_WHISPERCPP=OFF
            "-DTEST_APPLE=${_apple}" "-DEXPECT_FOUND=${expected_found}"
            "-DEXPECT_BLAS=${expected_blas}" ${ARGN}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "llama cache ${_case_name}/${step}: ${output}${error}")
  endif()
endfunction()

function(build_llama_case step)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_case_root}/${step}"
            --target archive_llama_cpp_to_3rdparty --config Release
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "llama archive ${_case_name}/${step}: ${output}${error}")
  endif()
endfunction()

function(check_llama_cache _case_name _apple _blas)
  set(_case_root "${TEST_ROOT}/${_case_name}")
  set(_case_source "${_case_root}/source")
  file(MAKE_DIRECTORY "${_case_source}/cmake_ext")
  file(COPY "${_llama_fixture}/CMakeLists.txt" DESTINATION "${_case_source}")
  file(COPY "${PROJECT_SOURCE_DIR}/cmake_ext/ThirdPartyCacheMetadata.cmake"
            "${PROJECT_SOURCE_DIR}/cmake_ext/KiteLlm.cmake"
            "${PROJECT_SOURCE_DIR}/cmake_ext/WhisperCpp.cmake"
       DESTINATION "${_case_source}/cmake_ext")
  set(_cache "${_case_source}/3rdparty/llama_cpp")

  configure_llama_case(fresh OFF ${_blas} ${ARGN})
  build_llama_case(fresh)
  configure_llama_case(reuse ON ${_blas} ${ARGN})

  if(_blas)
    file(REMOVE "${_cache}/lib/libggml-blas.a")
    configure_llama_case(missing_blas OFF ON ${ARGN})
    build_llama_case(missing_blas)
    configure_llama_case(repaired ON ON ${ARGN})
  else()
    file(WRITE "${_cache}/lib/libggml-blas.a" "stale archive")
    configure_llama_case(stale_blas ON OFF ${ARGN})
  endif()

  file(REMOVE "${_cache}/lib/libggml-base.a")
  configure_llama_case(missing_base OFF ${_blas} ${ARGN})
  build_llama_case(missing_base)

  if(_blas)
    set(_opposite OFF)
  else()
    set(_opposite ON)
  endif()
  configure_llama_case(changed_option OFF ${_opposite} ${ARGN}
                       "-DGGML_BLAS=${_opposite}")
  configure_llama_case(changed_vendor OFF ${_blas} ${ARGN}
                       -DGGML_BLAS_VENDOR=FixtureVendor)

  # Recreate a parent-revision marker: identical source/toolchain, no BLAS fields.
  # Keep every archive present so rejection depends on the metadata alone.
  set(_marker "${_cache}/.edgeflow-cache-fingerprint")
  file(READ "${_marker}" _legacy)
  string(REGEX REPLACE "GGML_BLAS=[^,\n]*," "" _legacy "${_legacy}")
  string(REGEX REPLACE "GGML_BLAS_VENDOR=[^,\n]*," "" _legacy "${_legacy}")
  string(REGEX REPLACE "fingerprint=[^\n]*\n" "" _legacy "${_legacy}")
  string(SHA256 _legacy_hash "${_legacy}")
  file(WRITE "${_marker}" "${_legacy}fingerprint=${_legacy_hash}\n")
  configure_llama_case(legacy OFF ${_blas} ${ARGN})
endfunction()

check_llama_cache(apple_default ON ON)
check_llama_cache(apple_disabled ON OFF -DGGML_BLAS=OFF)
check_llama_cache(nonapple_enabled OFF ON -DGGML_BLAS=ON)
check_llama_cache(nonapple_default OFF OFF)
