include(FetchContent)
include(cmake_ext/third_party/ThirdPartyCacheMetadata.cmake)

# whisper.cpp 开源语音转写引擎配置（用于 float32 16kHz PCM 音频转写）

option(WHISPER_CPP_FORCE_REBUILD "Force rebuilding whisper.cpp from source even if cached in 3rdparty" OFF)
set(LLM_EDGEFLOW_HAS_WHISPERCPP OFF)

if(NOT ENABLE_WHISPERCPP)
  return()
endif()

message(STATUS "[Model Execution] Enabling whisper.cpp Backend support...")

set(WHISPER_3RDPARTY_DIR "${CMAKE_SOURCE_DIR}/3rdparty/whisper_cpp")
set(LLM_EDGEFLOW_WHISPERCPP_COMMIT "371b5a7561823ab2bb32142d2751e35e7534727b")

edgeflow_prepare_third_party_cache(
  NAME whisper_cpp
  VERSION "${LLM_EDGEFLOW_WHISPERCPP_COMMIT}"
  CACHE_DIR "${WHISPER_3RDPARTY_DIR}"
  KIND STATIC
  ABI_OPTIONS GGML_PROVIDER=llama_cpp GGML_SOURCE=${LLM_EDGEFLOW_LLAMACPP_COMMIT}
              GGML_CACHE=${LLAMA_3RDPARTY_DIR} BUILD_SHARED_LIBS=OFF
  REQUIRED_FILES lib/libwhisper.a include/whisper.h
  OUT_CACHE_DIR WHISPER_3RDPARTY_DIR
  OUT_VALID _WHISPER_CACHE_VALID
  OUT_MARKER _WHISPER_CACHE_MARKER)

if(LLM_EDGEFLOW_LLAMACPP_CACHEABLE AND _WHISPER_CACHE_VALID AND NOT WHISPER_CPP_FORCE_REBUILD)
  message(STATUS "[Model Execution] Using prebuilt whisper.cpp from 3rdparty: ${WHISPER_3RDPARTY_DIR} (skipping compilation)")
  add_library(whisper STATIC IMPORTED GLOBAL)
  set_target_properties(whisper PROPERTIES
    IMPORTED_LOCATION "${WHISPER_3RDPARTY_DIR}/lib/libwhisper.a"
    INTERFACE_INCLUDE_DIRECTORIES "${WHISPER_3RDPARTY_DIR}/include"
    INTERFACE_LINK_LIBRARIES ggml)
  set(_whisper_include_dir "${WHISPER_3RDPARTY_DIR}/include")
else()
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

  if(LLM_EDGEFLOW_LLAMACPP_CACHEABLE)
    edgeflow_publish_third_party_cache(whisper_cpp
      CACHE_DIR "${WHISPER_3RDPARTY_DIR}" MARKER "${_WHISPER_CACHE_MARKER}"
      DEPENDS whisper LIBRARIES "$<TARGET_FILE:whisper>"
      HEADER_DIRS "${_whisper_include_dir}")
  endif()
endif()

set(LLM_EDGEFLOW_HAS_WHISPERCPP ON)
list(APPEND LLM_EDGEFLOW_BACKEND_INCLUDE_DIRS "${_whisper_include_dir}")
list(APPEND LLM_EDGEFLOW_BACKEND_COMPILE_DEFINITIONS HAVE_WHISPERCPP=1)
list(APPEND LLM_EDGEFLOW_BACKEND_LIBRARIES whisper)
