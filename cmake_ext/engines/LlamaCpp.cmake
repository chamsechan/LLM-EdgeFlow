include(FetchContent)
include(cmake_ext/ThirdPartyCacheMetadata.cmake)

option(LLAMA_CPP_FORCE_REBUILD "Force rebuilding llama.cpp from source even if cached in 3rdparty" OFF)
option(LLM_EDGEFLOW_LLAMACPP_METAL
       "Enable llama.cpp Metal backend (requires a usable Metal device)" OFF)
set(LLM_EDGEFLOW_HAS_LLAMACPP OFF)

if(ENABLE_LLAMACPP)
  message(STATUS "[Model Execution] Enabling llama.cpp Backend support...")

  set(LLAMA_3RDPARTY_DIR "${CMAKE_SOURCE_DIR}/3rdparty/llama_cpp")
  set(_LLAMA_FOUND OFF)
  set(LLM_EDGEFLOW_LLAMACPP_COMMIT
      "70adb1b4cea5ee39f867792c78dc59320921eda7")
  set(_LLAMA_SOURCE_SHA256
      "94d215f1fd85ded40f4674eccdbd3caf4a9b0daa00b6d72255efec922c6d94a4")
  # 先解析固定上游版本的 BLAS 默认值，再检查缓存。
  # 这些选项须同时描述源码构建和导入的归档。
  if(APPLE)
    set(_llama_blas_default ON)
    set(_llama_blas_vendor_default Apple)
  else()
    set(_llama_blas_default OFF)
    set(_llama_blas_vendor_default Generic)
  endif()
  option(GGML_BLAS "ggml: use BLAS" ${_llama_blas_default})
  set(GGML_BLAS_VENDOR "${_llama_blas_vendor_default}" CACHE STRING
      "ggml: BLAS library vendor")
  edgeflow_prepare_third_party_cache(
    NAME llama_cpp
    VERSION "${LLM_EDGEFLOW_LLAMACPP_COMMIT}"
    SOURCE_SHA256 "${_LLAMA_SOURCE_SHA256}"
    CACHE_DIR "${LLAMA_3RDPARTY_DIR}"
    KIND STATIC
    ABI_OPTIONS GGML_NATIVE=OFF GGML_METAL=${LLM_EDGEFLOW_LLAMACPP_METAL}
                GGML_BLAS=${GGML_BLAS} GGML_BLAS_VENDOR=${GGML_BLAS_VENDOR}
                BUILD_SHARED_LIBS=OFF
    OUT_VALID _LLAMA_CACHE_VALID
    OUT_MARKER _LLAMA_CACHE_MARKER)

  set(_llama_required_sublibs ggml ggml-cpu ggml-base)
  if(GGML_BLAS)
    list(APPEND _llama_required_sublibs ggml-blas)
  endif()
  if(_LLAMA_CACHE_VALID)
    foreach(_sublib IN LISTS _llama_required_sublibs)
      if(NOT EXISTS "${LLAMA_3RDPARTY_DIR}/lib/lib${_sublib}.a")
        message(STATUS "[3rdparty] Ignoring incomplete llama_cpp cache: missing lib${_sublib}.a")
        set(_LLAMA_CACHE_VALID OFF)
      endif()
    endforeach()
  endif()

  if(_LLAMA_CACHE_VALID AND
     EXISTS "${LLAMA_3RDPARTY_DIR}/lib/libllama.a" AND
     EXISTS "${LLAMA_3RDPARTY_DIR}/include/llama.h" AND
     NOT LLAMA_CPP_FORCE_REBUILD)
    set(_LLAMA_FOUND ON)
    message(STATUS "[Model Execution] Using prebuilt llama.cpp from 3rdparty: ${LLAMA_3RDPARTY_DIR} (skipping compilation)")

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
    if(APPLE)
      find_library(ACCELERATE_FRAMEWORK Accelerate)
      if(ACCELERATE_FRAMEWORK)
        list(APPEND _llama_imported_deps ${ACCELERATE_FRAMEWORK})
      endif()
    endif()

    set(_llama_sublibs "")
    foreach(_sublib IN LISTS _llama_required_sublibs)
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
    message(STATUS "[Model Execution] Prebuilt llama.cpp targets imported successfully.")
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
      ${EDGEFLOW_FETCHCONTENT_TIMESTAMP_ARGS}
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
        message(STATUS "[Model Execution] llama.cpp target configured successfully.")

        set(_llama_archive_deps llama ggml ggml-base ggml-cpu)
        set(_llama_archive_commands
          COMMAND ${CMAKE_COMMAND} -E make_directory "${LLAMA_3RDPARTY_DIR}/lib" "${LLAMA_3RDPARTY_DIR}/include"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:llama>" "${LLAMA_3RDPARTY_DIR}/lib/"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ggml>" "${LLAMA_3RDPARTY_DIR}/lib/"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ggml-base>" "${LLAMA_3RDPARTY_DIR}/lib/"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ggml-cpu>" "${LLAMA_3RDPARTY_DIR}/lib/"
        )
        if(TARGET ggml-blas)
          list(APPEND _llama_archive_deps ggml-blas)
          list(APPEND _llama_archive_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ggml-blas>" "${LLAMA_3RDPARTY_DIR}/lib/"
          )
        endif()
        list(APPEND _llama_archive_commands
          COMMAND ${CMAKE_COMMAND} -E copy_directory "${llama_cpp_source_SOURCE_DIR}/include" "${LLAMA_3RDPARTY_DIR}/include"
          COMMAND ${CMAKE_COMMAND} -E copy_directory "${llama_cpp_source_SOURCE_DIR}/ggml/include" "${LLAMA_3RDPARTY_DIR}/include"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_LLAMA_CACHE_MARKER}" "${LLAMA_3RDPARTY_DIR}/.edgeflow-cache-fingerprint"
        )
        add_custom_target(archive_llama_cpp_to_3rdparty ALL
          ${_llama_archive_commands}
          DEPENDS ${_llama_archive_deps}
          COMMENT "[3rdparty] Archiving llama.cpp static libraries and headers to ${LLAMA_3RDPARTY_DIR}"
        )
      endif()
    else()
      message(WARNING "[Model Execution] llama.cpp CMakeLists.txt not found, falling back to stub.")
    endif()
  endif()
endif()
