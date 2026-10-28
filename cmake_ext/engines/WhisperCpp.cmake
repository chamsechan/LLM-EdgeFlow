include(FetchContent)
include(cmake_ext/third_party/ThirdPartyDownload.cmake)

# whisper.cpp 开源语音转写引擎配置（用于 float32 16kHz PCM 音频转写）

set(LLM_EDGEFLOW_HAS_WHISPERCPP OFF)

if(NOT ENABLE_WHISPERCPP)
  return()
endif()

message(STATUS "[Model Execution] Enabling whisper.cpp Backend support...")

set(LLM_EDGEFLOW_WHISPERCPP_COMMIT "371b5a7561823ab2bb32142d2751e35e7534727b")

set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "Build server" FORCE)
set(WHISPER_CURL OFF CACHE BOOL "Enable CURL support" FORCE)
set(WHISPER_ALL_WARNINGS OFF CACHE BOOL "All warnings" FORCE)
set(WHISPER_BUILD_IS_DEV OFF CACHE BOOL "Dev build" FORCE)

FetchContent_Declare(
  whisper_cpp_source
  URL https://github.com/ggml-org/whisper.cpp/archive/${LLM_EDGEFLOW_WHISPERCPP_COMMIT}.tar.gz
  ${EDGEFLOW_FETCHCONTENT_TIMESTAMP_ARGS}
)
# MakeAvailable 在 CMake 3.28 前不支持 EXCLUDE_FROM_ALL，此处只构建被链接的 whisper。
FetchContent_GetProperties(whisper_cpp_source)
if(NOT whisper_cpp_source_POPULATED)
  FetchContent_Populate(whisper_cpp_source)
  add_subdirectory(${whisper_cpp_source_SOURCE_DIR} ${whisper_cpp_source_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET whisper)
  message(FATAL_ERROR "Pinned whisper.cpp source did not define target 'whisper'")
endif()
set(_whisper_include_dir "${whisper_cpp_source_SOURCE_DIR}/include")

set(LLM_EDGEFLOW_HAS_WHISPERCPP ON)
list(APPEND LLM_EDGEFLOW_BACKEND_INCLUDE_DIRS "${_whisper_include_dir}")
list(APPEND LLM_EDGEFLOW_BACKEND_COMPILE_DEFINITIONS HAVE_WHISPERCPP=1)
list(APPEND LLM_EDGEFLOW_BACKEND_LIBRARIES whisper)
