# cmake/ThirdPartyEngines.cmake
# 第三方推理引擎与条件 SDK (ONNX Runtime / llama.cpp / kiteLLM)

include(FetchContent)
include(cmake/ThirdPartyCacheMetadata.cmake)

set(LLM_EDGEFLOW_3RDPARTY_DIR "${CMAKE_SOURCE_DIR}/3rdparty")

# ------------------------------------------------------------------------------
# 1. ONNX Runtime 开源推理引擎配置 (用于特征向量与精排打分模型)
# ------------------------------------------------------------------------------
set(LLM_EDGEFLOW_HAS_ONNXRUNTIME OFF)
option(ENABLE_ONNXRUNTIME
       "Enable ONNX Runtime engine (auto-download official release or reuse 3rdparty)" OFF)

if(ENABLE_ONNXRUNTIME)
  message(STATUS "[Engine Layer] Enabling ONNX Runtime engine support...")

  set(ORT_3RDPARTY_DIR "${LLM_EDGEFLOW_3RDPARTY_DIR}/onnxruntime")
  if(APPLE)
    set(ORT_LIB_NAME "libonnxruntime.dylib")
  else()
    set(ORT_LIB_NAME "libonnxruntime.so")
  endif()

  # Select the pinned official package before inspecting the persistent cache,
  # so the cache marker is tied to the exact platform archive.
  if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
      set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v1.17.3/onnxruntime-osx-arm64-1.17.3.tgz")
      set(ORT_SHA256 "236c49c9065213b0ec9dec874e3619da3d01cbc8b984bb24291247293454d0f4")
    else()
      set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v1.17.3/onnxruntime-osx-x86_64-1.17.3.tgz")
      set(ORT_SHA256 "6292ad3d2e095b54b012a9fce7361f39fbac0b75fb6e9b1d9c320874515182e8")
    endif()
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v1.17.3/onnxruntime-linux-aarch64-1.17.3.tgz")
    set(ORT_SHA256 "9f801577bd99676d1d821022e52b1f4554f56339ae3606c7b5ff3155f443c921")
  else()
    set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v1.17.3/onnxruntime-linux-x64-1.17.3.tgz")
    set(ORT_SHA256 "f2f11f9da1e3e19b22a8b378b9af57a58433f40e3db6a803e75c0ec0eba97a20")
  endif()
  edgeflow_prepare_third_party_cache(
    NAME onnxruntime
    VERSION 1.17.3
    SOURCE_SHA256 "${ORT_SHA256}"
    CACHE_DIR "${ORT_3RDPARTY_DIR}"
    KIND PREBUILT
    OUT_VALID _ORT_CACHE_VALID
    OUT_MARKER _ORT_CACHE_MARKER)

  set(_ORT_FOUND OFF)
  if(_ORT_CACHE_VALID AND
     EXISTS "${ORT_3RDPARTY_DIR}/lib/${ORT_LIB_NAME}" AND
     EXISTS "${ORT_3RDPARTY_DIR}/include/onnxruntime_c_api.h")
    set(_ORT_FOUND ON)
    set(ONNXRUNTIME_INCLUDE_DIR "${ORT_3RDPARTY_DIR}/include")
    set(ONNXRUNTIME_LIB "${ORT_3RDPARTY_DIR}/lib/${ORT_LIB_NAME}")
    message(STATUS "[Engine Layer] Using prebuilt ONNX Runtime from 3rdparty: ${ORT_3RDPARTY_DIR}")
  endif()

  if(NOT _ORT_FOUND)
    FetchContent_Declare(
      onnxruntime_prebuilt
      URL ${ORT_URL}
      URL_HASH SHA256=${ORT_SHA256}
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(onnxruntime_prebuilt)

    # 自动规整并同步至 3rdparty/onnxruntime
    file(MAKE_DIRECTORY "${ORT_3RDPARTY_DIR}/include" "${ORT_3RDPARTY_DIR}/lib")
    file(COPY "${onnxruntime_prebuilt_SOURCE_DIR}/include/" DESTINATION "${ORT_3RDPARTY_DIR}/include")
    file(GLOB _ort_downloaded_libs "${onnxruntime_prebuilt_SOURCE_DIR}/lib/*")
    file(COPY ${_ort_downloaded_libs} DESTINATION "${ORT_3RDPARTY_DIR}/lib")
    configure_file("${_ORT_CACHE_MARKER}"
                   "${ORT_3RDPARTY_DIR}/.edgeflow-cache-fingerprint"
                   COPYONLY)

    set(ONNXRUNTIME_INCLUDE_DIR "${ORT_3RDPARTY_DIR}/include")
    if(EXISTS "${ORT_3RDPARTY_DIR}/lib/${ORT_LIB_NAME}")
      set(ONNXRUNTIME_LIB "${ORT_3RDPARTY_DIR}/lib/${ORT_LIB_NAME}")
    elseif(EXISTS "${onnxruntime_prebuilt_SOURCE_DIR}/lib/${ORT_LIB_NAME}")
      set(ONNXRUNTIME_LIB "${onnxruntime_prebuilt_SOURCE_DIR}/lib/${ORT_LIB_NAME}")
    endif()
  endif()

  if(EXISTS "${ONNXRUNTIME_INCLUDE_DIR}" AND EXISTS "${ONNXRUNTIME_LIB}")
    message(STATUS "[Engine Layer] ONNX Runtime successfully loaded from: ${ONNXRUNTIME_INCLUDE_DIR}")
    set(LLM_EDGEFLOW_HAS_ONNXRUNTIME ON)
    set(CMAKE_BUILD_RPATH "${CMAKE_BUILD_RPATH};${ORT_3RDPARTY_DIR}/lib")
    set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_RPATH};${ORT_3RDPARTY_DIR}/lib")
  else()
    message(WARNING "[Engine Layer] ONNX Runtime library or headers not found.")
  endif()
endif()

# ------------------------------------------------------------------------------
# 2. llama.cpp 开源大语言模型推理引擎配置 (用于 GGUF 自回归 LLM 生成)
# ------------------------------------------------------------------------------
option(ENABLE_LLAMACPP "Enable llama.cpp LLM engine (auto-download or reuse 3rdparty)" ON)
option(LLAMA_CPP_FORCE_REBUILD "Force rebuilding llama.cpp from source even if cached in 3rdparty" OFF)
option(LLM_EDGEFLOW_LLAMACPP_METAL
       "Enable llama.cpp Metal backend (requires a usable Metal device)" OFF)
set(LLM_EDGEFLOW_HAS_LLAMACPP OFF)

if(ENABLE_LLAMACPP)
  message(STATUS "[Engine Layer] Enabling llama.cpp engine support...")

  set(LLAMA_3RDPARTY_DIR "${LLM_EDGEFLOW_3RDPARTY_DIR}/llama_cpp")
  set(_LLAMA_FOUND OFF)
  set(LLM_EDGEFLOW_LLAMACPP_COMMIT
      "70adb1b4cea5ee39f867792c78dc59320921eda7")
  set(_LLAMA_SOURCE_SHA256
      "94d215f1fd85ded40f4674eccdbd3caf4a9b0daa00b6d72255efec922c6d94a4")
  edgeflow_prepare_third_party_cache(
    NAME llama_cpp
    VERSION "${LLM_EDGEFLOW_LLAMACPP_COMMIT}"
    SOURCE_SHA256 "${_LLAMA_SOURCE_SHA256}"
    CACHE_DIR "${LLAMA_3RDPARTY_DIR}"
    KIND STATIC
    ABI_OPTIONS GGML_NATIVE=OFF GGML_METAL=${LLM_EDGEFLOW_LLAMACPP_METAL}
                BUILD_SHARED_LIBS=OFF
    OUT_VALID _LLAMA_CACHE_VALID
    OUT_MARKER _LLAMA_CACHE_MARKER)

  if(_LLAMA_CACHE_VALID AND
     EXISTS "${LLAMA_3RDPARTY_DIR}/lib/libllama.a" AND
     EXISTS "${LLAMA_3RDPARTY_DIR}/include/llama.h" AND
     NOT LLAMA_CPP_FORCE_REBUILD)
    set(_LLAMA_FOUND ON)
    message(STATUS "[Engine Layer] Using prebuilt llama.cpp from 3rdparty: ${LLAMA_3RDPARTY_DIR} (skipping compilation)")

    find_package(Threads REQUIRED)
    find_package(OpenMP)
    set(_llama_imported_deps Threads::Threads)
    if(OpenMP_FOUND)
      if(TARGET OpenMP::OpenMP_CXX)
        list(APPEND _llama_imported_deps OpenMP::OpenMP_CXX)
      endif()
      if(TARGET OpenMP::OpenMP_C)
        list(APPEND _llama_imported_deps OpenMP::OpenMP_C)
      endif()
    elseif(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      list(APPEND _llama_imported_deps gomp)
    endif()
    if(CMAKE_DL_LIBS)
      list(APPEND _llama_imported_deps ${CMAKE_DL_LIBS})
    endif()
    if(UNIX AND NOT APPLE)
      list(APPEND _llama_imported_deps m)
    endif()

    set(_llama_sublibs "")
    foreach(_sublib ggml ggml-cpu ggml-base)
      if(EXISTS "${LLAMA_3RDPARTY_DIR}/lib/lib${_sublib}.a")
        if(NOT TARGET ${_sublib})
          add_library(${_sublib} STATIC IMPORTED GLOBAL)
          set_target_properties(${_sublib} PROPERTIES
            IMPORTED_LOCATION "${LLAMA_3RDPARTY_DIR}/lib/lib${_sublib}.a"
            INTERFACE_INCLUDE_DIRECTORIES "${LLAMA_3RDPARTY_DIR}/include"
            INTERFACE_LINK_LIBRARIES "${_llama_imported_deps}"
          )
        endif()
        list(APPEND _llama_sublibs ${_sublib})
      endif()
    endforeach()

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      set(_llama_link_group "-Wl,--start-group" ${_llama_sublibs} "-Wl,--end-group")
    else()
      set(_llama_link_group ${_llama_sublibs})
    endif()

    if(TARGET ggml)
      set_target_properties(ggml PROPERTIES
        INTERFACE_LINK_LIBRARIES "${_llama_link_group};${_llama_imported_deps}")
    endif()

    if(NOT TARGET llama)
      add_library(llama STATIC IMPORTED GLOBAL)
      set_target_properties(llama PROPERTIES
        IMPORTED_LOCATION "${LLAMA_3RDPARTY_DIR}/lib/libllama.a"
        INTERFACE_INCLUDE_DIRECTORIES "${LLAMA_3RDPARTY_DIR}/include"
        INTERFACE_LINK_LIBRARIES "${_llama_link_group};${_llama_imported_deps}"
      )
    endif()

    set(THIRD_PARTY_ENGINE_LIBS ${THIRD_PARTY_ENGINE_LIBS} llama)
    set(LLM_EDGEFLOW_HAS_LLAMACPP ON)
    set(LLAMACPP_INCLUDE_DIRS "${LLAMA_3RDPARTY_DIR}/include")
    message(STATUS "[Engine Layer] Prebuilt llama.cpp targets imported successfully.")
  else()
    # 配置 llama.cpp 极简构建选项，强制开启 -fPIC 以便链接到 .so 中
    set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "Position independent code" FORCE)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "Build server" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
    set(GGML_NATIVE OFF CACHE BOOL "Build with native CPU flags" FORCE)
    set(LLAMA_NATIVE OFF CACHE BOOL "Build with native CPU flags" FORCE)
    set(GGML_AVX512 OFF CACHE BOOL "Enable AVX512" FORCE)
    set(GGML_AVX512_VBMI OFF CACHE BOOL "Enable AVX512_VBMI" FORCE)
    set(GGML_AVX512_VNNI OFF CACHE BOOL "Enable AVX512_VNNI" FORCE)
    set(GGML_AVX512_BF16 OFF CACHE BOOL "Enable AVX512_BF16" FORCE)
    set(GGML_METAL ${LLM_EDGEFLOW_LLAMACPP_METAL} CACHE BOOL
        "Enable Metal backend for llama.cpp" FORCE)
    set(GGML_CCACHE OFF CACHE BOOL "Use ccache for ggml" FORCE)

    set(LLAMA_BUILD_COMMIT "${LLM_EDGEFLOW_LLAMACPP_COMMIT}"
        CACHE STRING "Pinned llama.cpp source commit" FORCE)
    FetchContent_Declare(
      llama_cpp_source
      URL https://github.com/ggml-org/llama.cpp/archive/${LLM_EDGEFLOW_LLAMACPP_COMMIT}.tar.gz
      URL_HASH SHA256=${_LLAMA_SOURCE_SHA256}
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(llama_cpp_source)

    # 将 llama.cpp 作为三方子工程引入
    if(EXISTS "${llama_cpp_source_SOURCE_DIR}/CMakeLists.txt")
      if(TARGET llama)
        set(THIRD_PARTY_ENGINE_LIBS ${THIRD_PARTY_ENGINE_LIBS} llama)
        set(LLM_EDGEFLOW_HAS_LLAMACPP ON)
        set(LLAMACPP_INCLUDE_DIRS
            ${llama_cpp_source_SOURCE_DIR}/include
            ${llama_cpp_source_SOURCE_DIR}/ggml/include)
        message(STATUS "[Engine Layer] llama.cpp target configured successfully.")

        # 编译完成后自动归档静态库与头文件至 3rdparty/llama_cpp
        add_custom_target(archive_llama_cpp_to_3rdparty ALL
          COMMAND ${CMAKE_COMMAND} -E make_directory "${LLAMA_3RDPARTY_DIR}/lib" "${LLAMA_3RDPARTY_DIR}/include"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:llama>" "${LLAMA_3RDPARTY_DIR}/lib/"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ggml>" "${LLAMA_3RDPARTY_DIR}/lib/"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ggml-base>" "${LLAMA_3RDPARTY_DIR}/lib/"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ggml-cpu>" "${LLAMA_3RDPARTY_DIR}/lib/"
          COMMAND ${CMAKE_COMMAND} -E copy_directory "${llama_cpp_source_SOURCE_DIR}/include" "${LLAMA_3RDPARTY_DIR}/include"
          COMMAND ${CMAKE_COMMAND} -E copy_directory "${llama_cpp_source_SOURCE_DIR}/ggml/include" "${LLAMA_3RDPARTY_DIR}/include"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_LLAMA_CACHE_MARKER}" "${LLAMA_3RDPARTY_DIR}/.edgeflow-cache-fingerprint"
          DEPENDS llama ggml ggml-base ggml-cpu
          COMMENT "[3rdparty] Archiving llama.cpp static libraries and headers to ${LLAMA_3RDPARTY_DIR}"
        )
      endif()
    else()
      message(WARNING "[Engine Layer] llama.cpp CMakeLists.txt not found, falling back to stub.")
    endif()
  endif()
endif()

include(cmake/KiteLlm.cmake)
include(cmake/WhisperCpp.cmake)
