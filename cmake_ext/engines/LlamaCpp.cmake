include(FetchContent)
include(cmake_ext/third_party/ThirdPartyCacheMetadata.cmake)

option(LLAMA_CPP_FORCE_REBUILD "Force rebuilding llama.cpp from source even if cached in 3rdparty" OFF)
option(LLM_EDGEFLOW_LLAMACPP_METAL
       "Enable llama.cpp Metal backend (requires a usable Metal device)" OFF)
set(LLM_EDGEFLOW_HAS_LLAMACPP OFF)
set(LLM_EDGEFLOW_LLAMACPP_CACHEABLE OFF)

if(ENABLE_LLAMACPP)
  message(STATUS "[Model Execution] Enabling llama.cpp Backend support...")

  set(LLAMA_3RDPARTY_DIR "${CMAKE_SOURCE_DIR}/3rdparty/llama_cpp")
  set(LLM_EDGEFLOW_LLAMACPP_COMMIT
      "70adb1b4cea5ee39f867792c78dc59320921eda7")
  # 先解析平台对应的 GGML 默认值，再检查缓存。
  # 这些选项须同时描述源码构建和导入的归档：macOS 使用 Apple Accelerate 并关闭 OpenMP。
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
  # 在缓存查询和源码子工程之前确定同一组选项，不让宿主探测改变目标 CPU。
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
  # 身份只使用进入上游前已确定的选项，避免上游新增默认值使重新配置漂移。
  set(_llama_cache_options GGML_BLAS GGML_BLAS_VENDOR GGML_NATIVE GGML_METAL
      GGML_CCACHE GGML_AVX512 GGML_AVX512_VBMI GGML_AVX512_VNNI GGML_AVX512_BF16
      GGML_OPENMP GGML_ACCELERATE GGML_CPU_ARM_ARCH)
  set(_llama_abi_options BUILD_SHARED_LIBS=OFF)
  foreach(variable IN LISTS _llama_cache_options)
    list(APPEND _llama_abi_options "${variable}=${${variable}}")
  endforeach()
  # 未纳入身份的调优使用源码增量构建，既不导入也不覆盖共享归档。
  # VARIABLES 同时覆盖普通变量及上游会转换的 LLAMA_* 旧选项。
  set(_llama_cache_controls LLAMA_CPP_FORCE_REBUILD LLAMA_NATIVE LLAMA_3RDPARTY_DIR)
  set(LLM_EDGEFLOW_LLAMACPP_CACHEABLE ON)
  set(_llama_uncached_options)
  get_cmake_property(_llama_variables VARIABLES)
  foreach(variable IN LISTS _llama_variables)
    get_property(_type CACHE "${variable}" PROPERTY TYPE)
    get_property(_cached_value CACHE "${variable}" PROPERTY VALUE)
    if(variable MATCHES "^(GGML|LLAMA)_" AND
       NOT variable IN_LIST _llama_cache_options AND NOT variable IN_LIST _llama_cache_controls AND
       NOT (_type STREQUAL "INTERNAL" AND "${${variable}}" STREQUAL "${_cached_value}"))
      set(LLM_EDGEFLOW_LLAMACPP_CACHEABLE OFF)
      list(APPEND _llama_uncached_options "${variable}")
    endif()
  endforeach()
  if(_llama_uncached_options)
    string(JOIN ", " _llama_uncached_options_text ${_llama_uncached_options})
    message(STATUS
      "[3rdparty] llama.cpp/Whisper shared archive cache disabled: untracked build options: ${_llama_uncached_options_text}. Using incremental source builds.")
  endif()
  # 归档导入只覆盖完整的 CPU/Apple BLAS 链。其他组合保留源码目标的传递依赖。
  if(GGML_METAL OR (GGML_BLAS AND NOT GGML_BLAS_VENDOR STREQUAL "Apple"))
    set(LLM_EDGEFLOW_LLAMACPP_CACHEABLE OFF)
    message(STATUS
      "[3rdparty] llama.cpp/Whisper shared archive cache disabled: GGML_METAL=${GGML_METAL}, GGML_BLAS=${GGML_BLAS}, GGML_BLAS_VENDOR=${GGML_BLAS_VENDOR} require source target dependencies.")
  endif()
  set(_llama_archives llama ggml ggml-cpu ggml-base)
  if(GGML_BLAS)
    list(APPEND _llama_archives ggml-blas)
  endif()
  set(_llama_required_files include/llama.h)
  foreach(_archive IN LISTS _llama_archives)
    list(APPEND _llama_required_files "lib/lib${_archive}.a")
  endforeach()
  edgeflow_prepare_third_party_cache(
    NAME llama_cpp
    VERSION "${LLM_EDGEFLOW_LLAMACPP_COMMIT}"
    CACHE_DIR "${LLAMA_3RDPARTY_DIR}"
    KIND STATIC
    ABI_OPTIONS ${_llama_abi_options}
    REQUIRED_FILES ${_llama_required_files}
    OUT_CACHE_DIR LLAMA_3RDPARTY_DIR
    OUT_VALID _LLAMA_CACHE_VALID
    OUT_MARKER _LLAMA_CACHE_MARKER)

  if(LLM_EDGEFLOW_LLAMACPP_CACHEABLE AND _LLAMA_CACHE_VALID AND NOT LLAMA_CPP_FORCE_REBUILD)
    message(STATUS "[Model Execution] Using prebuilt llama.cpp from 3rdparty: ${LLAMA_3RDPARTY_DIR} (skipping compilation)")

    find_package(Threads REQUIRED)
    set(_llama_imported_deps Threads::Threads)
    if(GGML_OPENMP)
      find_package(OpenMP)
      if(OpenMP_FOUND)
        list(APPEND _llama_imported_deps OpenMP::OpenMP_CXX OpenMP::OpenMP_C)
      elseif(LLM_EDGEFLOW_TARGET_OS STREQUAL "linux")
        list(APPEND _llama_imported_deps gomp)
      endif()
    endif()
    list(APPEND _llama_imported_deps ${CMAKE_DL_LIBS})
    if(LLM_EDGEFLOW_TARGET_OS STREQUAL "linux")
      list(APPEND _llama_imported_deps m)
    else()
      find_library(ACCELERATE_FRAMEWORK Accelerate REQUIRED)
      list(APPEND _llama_imported_deps ${ACCELERATE_FRAMEWORK})
    endif()

    foreach(_archive IN LISTS _llama_archives)
      add_library(${_archive} STATIC IMPORTED GLOBAL)
      set_target_properties(${_archive} PROPERTIES
        IMPORTED_LOCATION "${LLAMA_3RDPARTY_DIR}/lib/lib${_archive}.a"
        INTERFACE_INCLUDE_DIRECTORIES "${LLAMA_3RDPARTY_DIR}/include")
    endforeach()
    # GGML 静态库相互引用；Linux 链接器需要分组解析，Whisper 也通过 ggml 获得整组依赖。
    list(REMOVE_ITEM _llama_archives llama)
    if(LLM_EDGEFLOW_TARGET_OS STREQUAL "linux")
      set(_llama_link_group "-Wl,--start-group" ${_llama_archives} "-Wl,--end-group")
    else()
      set(_llama_link_group ${_llama_archives})
    endif()
    foreach(_archive IN LISTS _llama_archives)
      set_target_properties(${_archive} PROPERTIES INTERFACE_LINK_LIBRARIES "${_llama_imported_deps}")
    endforeach()
    set_target_properties(ggml llama PROPERTIES
      INTERFACE_LINK_LIBRARIES "${_llama_link_group};${_llama_imported_deps}")
    set(_llama_include_dirs "${LLAMA_3RDPARTY_DIR}/include")
  else()
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
    set(_llama_include_dirs
        "${llama_cpp_source_SOURCE_DIR}/include"
        "${llama_cpp_source_SOURCE_DIR}/ggml/include")

    if(LLM_EDGEFLOW_LLAMACPP_CACHEABLE)
      # 上游未生成的可选库（如未找到 BLAS）不发布，缓存因缺文件而不会被导入。
      set(_llama_built_archives)
      set(_llama_archive_files)
      foreach(_archive IN LISTS _llama_archives)
        if(TARGET ${_archive})
          list(APPEND _llama_built_archives ${_archive})
          list(APPEND _llama_archive_files "$<TARGET_FILE:${_archive}>")
        endif()
      endforeach()
      edgeflow_publish_third_party_cache(llama_cpp
        CACHE_DIR "${LLAMA_3RDPARTY_DIR}" MARKER "${_LLAMA_CACHE_MARKER}"
        DEPENDS ${_llama_built_archives} LIBRARIES ${_llama_archive_files}
        HEADER_DIRS ${_llama_include_dirs})
    endif()
  endif()

  set(LLM_EDGEFLOW_HAS_LLAMACPP ON)
  list(APPEND LLM_EDGEFLOW_BACKEND_INCLUDE_DIRS ${_llama_include_dirs})
  list(APPEND LLM_EDGEFLOW_BACKEND_COMPILE_DEFINITIONS HAVE_LLAMACPP=1)
  list(APPEND LLM_EDGEFLOW_BACKEND_LIBRARIES llama)
endif()
