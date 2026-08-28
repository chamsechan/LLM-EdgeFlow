# cmake/ThirdPartyEngines.cmake
# 自动从 GitHub 下载与集成第三方开源推理引擎 (ONNX Runtime & llama.cpp)

include(FetchContent)

# ------------------------------------------------------------------------------
# 1. ONNX Runtime 开源推理引擎配置 (用于特征向量与精排打分模型)
# ------------------------------------------------------------------------------
option(ENABLE_ONNXRUNTIME "Enable ONNX Runtime engine (auto-download from GitHub)" ON)

if(ENABLE_ONNXRUNTIME)
  message(STATUS "[Engine Layer] Enabling ONNX Runtime engine support...")

  # 根据目标系统与架构选择对应的官方 Release 包
  if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
      set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v1.17.3/onnxruntime-osx-arm64-1.17.3.tgz")
    else()
      set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v1.17.3/onnxruntime-osx-x86_64-1.17.3.tgz")
    endif()
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v1.17.3/onnxruntime-linux-aarch64-1.17.3.tgz")
  else()
    set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v1.17.3/onnxruntime-linux-x64-1.17.3.tgz")
  endif()

  FetchContent_Declare(
    onnxruntime_prebuilt
    URL ${ORT_URL}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )

  FetchContent_GetProperties(onnxruntime_prebuilt)
  if(NOT onnxruntime_prebuilt_POPULATED)
    message(STATUS "[Engine Layer] Downloading ONNX Runtime C/C++ release from GitHub: ${ORT_URL} ...")
    FetchContent_Populate(onnxruntime_prebuilt)
  endif()

  set(ONNXRUNTIME_INCLUDE_DIR "${onnxruntime_prebuilt_SOURCE_DIR}/include")
  if(APPLE)
    set(ONNXRUNTIME_LIB "${onnxruntime_prebuilt_SOURCE_DIR}/lib/libonnxruntime.dylib")
    if(NOT EXISTS "${ONNXRUNTIME_LIB}")
      set(ONNXRUNTIME_LIB "${onnxruntime_prebuilt_SOURCE_DIR}/lib/libonnxruntime.1.17.3.dylib")
    endif()
  else()
    set(ONNXRUNTIME_LIB "${onnxruntime_prebuilt_SOURCE_DIR}/lib/libonnxruntime.so")
    if(NOT EXISTS "${ONNXRUNTIME_LIB}")
      set(ONNXRUNTIME_LIB "${onnxruntime_prebuilt_SOURCE_DIR}/lib/libonnxruntime.so.1.17.3")
    endif()
  endif()

  if(EXISTS "${ONNXRUNTIME_INCLUDE_DIR}" AND EXISTS "${ONNXRUNTIME_LIB}")
    message(STATUS "[Engine Layer] ONNX Runtime successfully loaded from: ${onnxruntime_prebuilt_SOURCE_DIR}")
    include_directories(SYSTEM ${ONNXRUNTIME_INCLUDE_DIR})
    link_directories("${onnxruntime_prebuilt_SOURCE_DIR}/lib")
    set(CMAKE_BUILD_RPATH "${CMAKE_BUILD_RPATH};${onnxruntime_prebuilt_SOURCE_DIR}/lib")
    set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_RPATH};${onnxruntime_prebuilt_SOURCE_DIR}/lib")
    set(THIRD_PARTY_ENGINE_LIBS ${THIRD_PARTY_ENGINE_LIBS} ${ONNXRUNTIME_LIB})
    add_definitions(-DHAVE_ONNXRUNTIME=1)
  else()
    message(WARNING "[Engine Layer] ONNX Runtime library or headers not found in downloaded package, falling back to stub.")
  endif()
endif()

# ------------------------------------------------------------------------------
# 2. llama.cpp 开源大语言模型推理引擎配置 (用于 GGUF 自回归 LLM 生成)
# ------------------------------------------------------------------------------
option(ENABLE_LLAMACPP "Enable llama.cpp LLM engine (auto-download from GitHub)" ON)

if(ENABLE_LLAMACPP)
  message(STATUS "[Engine Layer] Enabling llama.cpp engine support...")

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
  # ggml 有独立的 ccache 开关。顶层明确关闭编译缓存时必须同步关闭，
  # 避免子工程绕过 LLM_EDGEFLOW_USE_CCACHE 并访问不可写的默认缓存目录。
  if(DEFINED LLM_EDGEFLOW_USE_CCACHE AND NOT LLM_EDGEFLOW_USE_CCACHE)
    set(GGML_CCACHE OFF CACHE BOOL "Use ccache for ggml" FORCE)
  endif()

  FetchContent_Declare(
    llama_cpp_source
    URL https://github.com/ggerganov/llama.cpp/archive/refs/heads/master.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )

  FetchContent_GetProperties(llama_cpp_source)
  if(NOT llama_cpp_source_POPULATED)
    message(STATUS "[Engine Layer] Downloading llama.cpp source code from GitHub: https://github.com/ggerganov/llama.cpp ...")
    FetchContent_Populate(llama_cpp_source)
  endif()

  # 将 llama.cpp 作为三方子工程引入
  if(EXISTS "${llama_cpp_source_SOURCE_DIR}/CMakeLists.txt")
    add_subdirectory(${llama_cpp_source_SOURCE_DIR} ${llama_cpp_source_BINARY_DIR} EXCLUDE_FROM_ALL)
    include_directories(SYSTEM ${llama_cpp_source_SOURCE_DIR}/include ${llama_cpp_source_SOURCE_DIR}/ggml/include)
    
    # 链接 llama 静态库
    if(TARGET llama)
      set(THIRD_PARTY_ENGINE_LIBS ${THIRD_PARTY_ENGINE_LIBS} llama)
      add_definitions(-DHAVE_LLAMACPP=1)
      message(STATUS "[Engine Layer] llama.cpp target configured successfully.")
    elseif(TARGET ggml)
      set(THIRD_PARTY_ENGINE_LIBS ${THIRD_PARTY_ENGINE_LIBS} ggml)
      add_definitions(-DHAVE_LLAMACPP=1)
    endif()
  else()
    message(WARNING "[Engine Layer] llama.cpp CMakeLists.txt not found, falling back to stub.")
  endif()
endif()
