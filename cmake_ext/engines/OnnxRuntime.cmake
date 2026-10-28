include(FetchContent)
include(cmake_ext/third_party/ThirdPartyDownload.cmake)

set(LLM_EDGEFLOW_HAS_ONNXRUNTIME OFF)
if(NOT ENABLE_ONNXRUNTIME)
  return()
endif()
message(STATUS "[Model Execution] Enabling ONNX Runtime Backend support...")

# 选定与目标平台对应的固定版本官方包。
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
FetchContent_Declare(
  onnxruntime_prebuilt
  URL "https://github.com/microsoft/onnxruntime/releases/download/v${_ort_version}/onnxruntime-${_ort_package}-${_ort_version}.tgz"
  ${EDGEFLOW_FETCHCONTENT_TIMESTAMP_ARGS}
)
FetchContent_MakeAvailable(onnxruntime_prebuilt)
# 运行库与产物同目录，复制构建根目录的二进制和共享库即可运行。
file(COPY "${onnxruntime_prebuilt_SOURCE_DIR}/lib/" DESTINATION "${CMAKE_BINARY_DIR}"
     FILES_MATCHING PATTERN "libonnxruntime*")

add_library(edgeflow_onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(edgeflow_onnxruntime PROPERTIES
    IMPORTED_LOCATION "${CMAKE_BINARY_DIR}/libonnxruntime${CMAKE_SHARED_LIBRARY_SUFFIX}")
set(LLM_EDGEFLOW_HAS_ONNXRUNTIME ON)
list(APPEND LLM_EDGEFLOW_BACKEND_INCLUDE_DIRS "${onnxruntime_prebuilt_SOURCE_DIR}/include")
list(APPEND LLM_EDGEFLOW_BACKEND_COMPILE_DEFINITIONS HAVE_ONNXRUNTIME=1)
list(APPEND LLM_EDGEFLOW_BACKEND_LIBRARIES edgeflow_onnxruntime)
