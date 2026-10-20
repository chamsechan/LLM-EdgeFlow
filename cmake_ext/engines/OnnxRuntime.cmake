include(FetchContent)
include(cmake_ext/ThirdPartyCacheMetadata.cmake)

set(LLM_EDGEFLOW_HAS_ONNXRUNTIME OFF)

if(ENABLE_ONNXRUNTIME)
  message(STATUS "[Model Execution] Enabling ONNX Runtime Backend support...")

  set(ORT_3RDPARTY_DIR "${CMAKE_SOURCE_DIR}/3rdparty/onnxruntime")
  if(APPLE)
    set(ORT_LIB_NAME "libonnxruntime.dylib")
  else()
    set(ORT_LIB_NAME "libonnxruntime.so")
  endif()

  # 先选定固定的官方包，再检查持久缓存，使缓存标记与具体平台归档对应。
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
    message(STATUS "[Model Execution] Using prebuilt ONNX Runtime from 3rdparty: ${ORT_3RDPARTY_DIR}")
  endif()

  if(NOT _ORT_FOUND)
    FetchContent_Declare(
      onnxruntime_prebuilt
      URL ${ORT_URL}
      URL_HASH SHA256=${ORT_SHA256}
      ${EDGEFLOW_FETCHCONTENT_TIMESTAMP_ARGS}
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
    message(STATUS "[Model Execution] ONNX Runtime successfully loaded from: ${ONNXRUNTIME_INCLUDE_DIR}")
    set(LLM_EDGEFLOW_HAS_ONNXRUNTIME ON)
    set(CMAKE_BUILD_RPATH "${CMAKE_BUILD_RPATH};${ORT_3RDPARTY_DIR}/lib")
    set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_RPATH};${ORT_3RDPARTY_DIR}/lib")
  else()
    message(WARNING "[Model Execution] ONNX Runtime library or headers not found.")
  endif()
endif()
