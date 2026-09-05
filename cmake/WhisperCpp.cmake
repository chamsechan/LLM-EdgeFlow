# cmake/WhisperCpp.cmake
# whisper.cpp 开源语音转写引擎配置 (用于 float32 16kHz PCM 音频转写)

option(ENABLE_WHISPERCPP "Enable whisper.cpp ASR engine (reuses shared GGML provider)" OFF)
option(WHISPER_CPP_FORCE_REBUILD "Force rebuilding whisper.cpp from source even if cached in 3rdparty" OFF)
set(LLM_EDGEFLOW_HAS_WHISPERCPP OFF)

if(NOT ENABLE_WHISPERCPP)
  return()
endif()

if(NOT ENABLE_LLAMACPP)
  message(FATAL_ERROR
    "ENABLE_WHISPERCPP=ON requires ENABLE_LLAMACPP=ON to reuse the shared GGML runtime provider.")
endif()

if(ENABLE_KITELLM)
  message(FATAL_ERROR
    "ENABLE_WHISPERCPP=ON cannot coexist with ENABLE_KITELLM=ON in the current architecture.")
endif()

message(STATUS "[Engine Layer] Enabling whisper.cpp engine support...")

set(WHISPER_3RDPARTY_DIR "${LLM_EDGEFLOW_3RDPARTY_DIR}/whisper_cpp")
set(_WHISPER_FOUND OFF)
set(LLM_EDGEFLOW_WHISPERCPP_COMMIT "371b5a7561823ab2bb32142d2751e35e7534727b")
set(_WHISPER_SOURCE_SHA256 "89051d8fca516a3ad1f5c2f8f9d2fccb089afbaec338fca3f8731999babc6f81")

edgeflow_prepare_third_party_cache(
  NAME whisper_cpp
  VERSION "${LLM_EDGEFLOW_WHISPERCPP_COMMIT}"
  SOURCE_SHA256 "${_WHISPER_SOURCE_SHA256}"
  CACHE_DIR "${WHISPER_3RDPARTY_DIR}"
  KIND STATIC
  ABI_OPTIONS GGML_PROVIDER=llama_cpp BUILD_SHARED_LIBS=OFF
  OUT_VALID _WHISPER_CACHE_VALID
  OUT_MARKER _WHISPER_CACHE_MARKER)

if(_WHISPER_CACHE_VALID AND
   EXISTS "${WHISPER_3RDPARTY_DIR}/lib/libwhisper.a" AND
   EXISTS "${WHISPER_3RDPARTY_DIR}/include/whisper.h" AND
   NOT WHISPER_CPP_FORCE_REBUILD)
  set(_WHISPER_FOUND ON)
  message(STATUS "[Engine Layer] Using prebuilt whisper.cpp from 3rdparty: ${WHISPER_3RDPARTY_DIR} (skipping compilation)")

  if(NOT TARGET whisper)
    add_library(whisper STATIC IMPORTED GLOBAL)
    set_target_properties(whisper PROPERTIES
      IMPORTED_LOCATION "${WHISPER_3RDPARTY_DIR}/lib/libwhisper.a"
      INTERFACE_INCLUDE_DIRECTORIES "${WHISPER_3RDPARTY_DIR}/include"
      INTERFACE_LINK_LIBRARIES ggml
    )
  endif()

  set(THIRD_PARTY_ENGINE_LIBS ${THIRD_PARTY_ENGINE_LIBS} whisper)
  set(LLM_EDGEFLOW_HAS_WHISPERCPP ON)
  set(WHISPERCPP_INCLUDE_DIRS "${WHISPER_3RDPARTY_DIR}/include")
  message(STATUS "[Engine Layer] Prebuilt whisper.cpp target imported successfully.")
else()
  set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "Position independent code" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
  set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
  set(WHISPER_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
  set(WHISPER_BUILD_SERVER OFF CACHE BOOL "Build server" FORCE)
  set(WHISPER_CURL OFF CACHE BOOL "Enable CURL support" FORCE)
  set(WHISPER_ALL_WARNINGS OFF CACHE BOOL "All warnings" FORCE)
  set(WHISPER_BUILD_IS_DEV OFF CACHE BOOL "Dev build" FORCE)

  FetchContent_Declare(
    whisper_cpp_source
    URL https://github.com/ggml-org/whisper.cpp/archive/${LLM_EDGEFLOW_WHISPERCPP_COMMIT}.tar.gz
    URL_HASH SHA256=${_WHISPER_SOURCE_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_GetProperties(whisper_cpp_source)
  if(NOT whisper_cpp_source_POPULATED)
    FetchContent_Populate(whisper_cpp_source)
    add_subdirectory(${whisper_cpp_source_SOURCE_DIR} ${whisper_cpp_source_BINARY_DIR} EXCLUDE_FROM_ALL)
  endif()

  if(TARGET whisper)
    set(THIRD_PARTY_ENGINE_LIBS ${THIRD_PARTY_ENGINE_LIBS} whisper)
    set(LLM_EDGEFLOW_HAS_WHISPERCPP ON)
    set(WHISPERCPP_INCLUDE_DIRS
        ${whisper_cpp_source_SOURCE_DIR}/include)
    message(STATUS "[Engine Layer] whisper.cpp target configured successfully.")

    add_custom_target(archive_whisper_cpp_to_3rdparty ALL
      COMMAND ${CMAKE_COMMAND} -E make_directory "${WHISPER_3RDPARTY_DIR}/lib" "${WHISPER_3RDPARTY_DIR}/include"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:whisper>" "${WHISPER_3RDPARTY_DIR}/lib/"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${whisper_cpp_source_SOURCE_DIR}/include" "${WHISPER_3RDPARTY_DIR}/include"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_WHISPER_CACHE_MARKER}" "${WHISPER_3RDPARTY_DIR}/.edgeflow-cache-fingerprint"
      DEPENDS whisper
      COMMENT "[3rdparty] Archiving whisper.cpp static library and headers to ${WHISPER_3RDPARTY_DIR}"
    )
  else()
    message(WARNING "[Engine Layer] whisper.cpp target 'whisper' was not created.")
  endif()
endif()
