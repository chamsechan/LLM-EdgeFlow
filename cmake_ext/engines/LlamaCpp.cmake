include(FetchContent)
include(cmake_ext/third_party/ThirdPartyDownload.cmake)

option(LLM_EDGEFLOW_LLAMACPP_METAL
       "Enable llama.cpp Metal backend (requires a usable Metal device)" OFF)
set(LLM_EDGEFLOW_HAS_LLAMACPP OFF)

if(ENABLE_LLAMACPP)
  message(STATUS "[Model Execution] Enabling llama.cpp Backend support...")

  set(LLM_EDGEFLOW_LLAMACPP_COMMIT
      "70adb1b4cea5ee39f867792c78dc59320921eda7")
  # 平台对应的 GGML 默认值：macOS 使用 Apple Accelerate 并关闭 OpenMP。
  if(LLM_EDGEFLOW_TARGET_OS STREQUAL "macos")
    set(_llama_blas_default ON)
    set(_llama_blas_vendor_default Apple)
    set(_llama_openmp_default OFF)
  else()
    set(_llama_blas_default OFF)
    set(_llama_blas_vendor_default Generic)
    set(_llama_openmp_default ON)
  endif()
  option(GGML_BLAS "ggml: use BLAS" ${_llama_blas_default})
  set(GGML_BLAS_VENDOR "${_llama_blas_vendor_default}" CACHE STRING
      "ggml: BLAS library vendor")
  # 在源码子工程之前固定这些选项，不让宿主探测改变目标 CPU。
  set(GGML_NATIVE OFF CACHE BOOL "Build with native CPU flags" FORCE)
  set(LLAMA_NATIVE OFF CACHE BOOL "Build with native CPU flags" FORCE)
  set(GGML_METAL ${LLM_EDGEFLOW_LLAMACPP_METAL} CACHE BOOL "Enable Metal" FORCE)
  set(GGML_CCACHE OFF CACHE BOOL "Use ccache for ggml" FORCE)
  foreach(feature AVX512 AVX512_VBMI AVX512_VNNI AVX512_BF16)
    set(GGML_${feature} OFF CACHE BOOL "Enable ${feature}" FORCE)
  endforeach()
  option(GGML_OPENMP "ggml: use OpenMP" ${_llama_openmp_default})
  option(GGML_ACCELERATE "ggml: use Accelerate" ${_llama_blas_default})
  set(GGML_CPU_ARM_ARCH "" CACHE STRING "ggml: target ARM architecture")

  # 配置 llama.cpp 极简构建选项；静态与 PIC 策略由 ProjectSetup 统一确定。
  set(LLAMA_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
  set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
  set(LLAMA_BUILD_SERVER OFF CACHE BOOL "Build server" FORCE)

  set(LLAMA_BUILD_COMMIT "${LLM_EDGEFLOW_LLAMACPP_COMMIT}"
      CACHE STRING "Pinned llama.cpp source commit" FORCE)
  FetchContent_Declare(
    llama_cpp_source
    URL https://github.com/ggml-org/llama.cpp/archive/${LLM_EDGEFLOW_LLAMACPP_COMMIT}.tar.gz
    ${EDGEFLOW_FETCHCONTENT_TIMESTAMP_ARGS}
  )
  FetchContent_MakeAvailable(llama_cpp_source)
  if(NOT TARGET llama)
    message(FATAL_ERROR "Pinned llama.cpp source did not define target 'llama'")
  endif()

  set(LLM_EDGEFLOW_HAS_LLAMACPP ON)
  list(APPEND LLM_EDGEFLOW_BACKEND_INCLUDE_DIRS
       "${llama_cpp_source_SOURCE_DIR}/include"
       "${llama_cpp_source_SOURCE_DIR}/ggml/include")
  list(APPEND LLM_EDGEFLOW_BACKEND_COMPILE_DEFINITIONS HAVE_LLAMACPP=1)
  list(APPEND LLM_EDGEFLOW_BACKEND_LIBRARIES llama)
endif()
