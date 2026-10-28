include(FetchContent)
include(cmake_ext/third_party/ThirdPartyCacheMetadata.cmake)

set(LLM_EDGEFLOW_HAS_ONNXRUNTIME OFF)
if(NOT ENABLE_ONNXRUNTIME)
  return()
endif()
message(STATUS "[Model Execution] Enabling ONNX Runtime Backend support...")

# 先选定固定的官方包，再检查持久缓存，使缓存标记与具体平台归档对应。
set(_ort_version 1.17.3)
if(LLM_EDGEFLOW_TARGET_PLATFORM STREQUAL "macos-arm64")
  set(_ort_package osx-arm64)
elseif(LLM_EDGEFLOW_TARGET_PLATFORM STREQUAL "macos-x86_64")
  set(_ort_package osx-x86_64)
elseif(LLM_EDGEFLOW_TARGET_PLATFORM STREQUAL "linux-arm64")
  set(_ort_package linux-aarch64)
else()
  set(_ort_package linux-x64)
endif()
set(_ort_library "lib/libonnxruntime${CMAKE_SHARED_LIBRARY_SUFFIX}")
edgeflow_prepare_third_party_cache(
  NAME onnxruntime
  VERSION ${_ort_version}
  CACHE_DIR "${CMAKE_SOURCE_DIR}/3rdparty/onnxruntime"
  KIND PREBUILT
  REQUIRED_FILES "${_ort_library}" include/onnxruntime_c_api.h
  OUT_CACHE_DIR _ort_dir
  OUT_VALID _ort_cache_valid
  OUT_MARKER _ort_marker)

if(_ort_cache_valid)
  message(STATUS "[Model Execution] Using prebuilt ONNX Runtime from 3rdparty: ${_ort_dir}")
else()
  FetchContent_Declare(
    onnxruntime_prebuilt
    URL "https://github.com/microsoft/onnxruntime/releases/download/v${_ort_version}/onnxruntime-${_ort_package}-${_ort_version}.tgz"
    ${EDGEFLOW_FETCHCONTENT_TIMESTAMP_ARGS}
  )
  FetchContent_MakeAvailable(onnxruntime_prebuilt)
  file(COPY "${onnxruntime_prebuilt_SOURCE_DIR}/include" "${onnxruntime_prebuilt_SOURCE_DIR}/lib"
       DESTINATION "${_ort_dir}")
  configure_file("${_ort_marker}" "${_ort_dir}/.edgeflow-cache-fingerprint" COPYONLY)
endif()

add_library(edgeflow_onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(edgeflow_onnxruntime PROPERTIES
    IMPORTED_LOCATION "${_ort_dir}/${_ort_library}")
set(LLM_EDGEFLOW_HAS_ONNXRUNTIME ON)
list(APPEND LLM_EDGEFLOW_BACKEND_INCLUDE_DIRS "${_ort_dir}/include")
list(APPEND LLM_EDGEFLOW_BACKEND_COMPILE_DEFINITIONS HAVE_ONNXRUNTIME=1)
list(APPEND LLM_EDGEFLOW_BACKEND_LIBRARIES edgeflow_onnxruntime)
