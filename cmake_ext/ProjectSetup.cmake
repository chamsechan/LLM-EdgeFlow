# 编译策略集中在依赖与业务目标创建之前。
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# 第三方源码依赖统一构建为静态归档，与缓存导入规则一致；alg_sdk 显式声明 SHARED。
# 使用缓存项，旧 CMake 策略下上游的 option(BUILD_SHARED_LIBS) 也不会改回共享库。
if(BUILD_SHARED_LIBS)
  message(FATAL_ERROR "BUILD_SHARED_LIBS=ON is unsupported; third-party dependencies are linked statically")
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build third-party source dependencies as shared libraries")
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()
include(cmake_ext/support/BuildAcceleration.cmake)
add_compile_options(-Wall -Wextra)

# 与固定 GGML 上游的默认值一致，在配置任何依赖前确定。
if(ENABLE_LLAMACPP AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
set(LLM_EDGEFLOW_SANITIZERS "address,undefined" CACHE STRING
    "Comma-separated Clang/GCC sanitizer set used when ENABLE_SANITIZERS=ON")
if(ENABLE_SANITIZERS)
  if(NOT LLM_EDGEFLOW_SANITIZERS MATCHES "^(address|undefined|address,undefined|undefined,address|thread)$")
    message(FATAL_ERROR
      "Unsupported LLM_EDGEFLOW_SANITIZERS='${LLM_EDGEFLOW_SANITIZERS}'. "
      "Use address, undefined, address,undefined, or thread.")
  endif()
  message(STATUS "Enabling sanitizers (-fsanitize=${LLM_EDGEFLOW_SANITIZERS})")
  add_compile_options(
      -O1 -g -fsanitize=${LLM_EDGEFLOW_SANITIZERS}
      -fno-omit-frame-pointer -fno-optimize-sibling-calls)
  set(CMAKE_EXE_LINKER_FLAGS
      "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=${LLM_EDGEFLOW_SANITIZERS}")
  set(CMAKE_SHARED_LINKER_FLAGS
      "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=${LLM_EDGEFLOW_SANITIZERS}")
endif()

# 直接调用编译器的分层契约探针也使用相同的 Apple SDK/架构。
set(edgeflow_cxx_platform_flags "")
if(LLM_EDGEFLOW_TARGET_OS STREQUAL "macos")
  if(CMAKE_OSX_SYSROOT)
    set(_edgeflow_sdk_path "${CMAKE_OSX_SYSROOT}")
    if(NOT IS_ABSOLUTE "${_edgeflow_sdk_path}")
      execute_process(COMMAND xcrun --sdk "${CMAKE_OSX_SYSROOT}" --show-sdk-path
          OUTPUT_VARIABLE _edgeflow_sdk_path OUTPUT_STRIP_TRAILING_WHITESPACE
          RESULT_VARIABLE _edgeflow_sdk_result)
      if(NOT _edgeflow_sdk_result EQUAL 0)
        message(FATAL_ERROR "Cannot resolve the selected macOS SDK: ${CMAKE_OSX_SYSROOT}")
      endif()
    endif()
    set(LLM_EDGEFLOW_APPLE_SDK_PATH "${_edgeflow_sdk_path}")
    execute_process(COMMAND xcrun --sdk "${_edgeflow_sdk_path}" --show-sdk-version
        OUTPUT_VARIABLE LLM_EDGEFLOW_APPLE_SDK_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _edgeflow_sdk_result)
    if(NOT _edgeflow_sdk_result EQUAL 0)
      message(FATAL_ERROR "Cannot identify the selected macOS SDK: ${_edgeflow_sdk_path}")
    endif()
    list(APPEND edgeflow_cxx_platform_flags -isysroot "${_edgeflow_sdk_path}")
  endif()
  list(APPEND edgeflow_cxx_platform_flags -arch "${LLM_EDGEFLOW_TARGET_ARCH}")
  if(CMAKE_OSX_DEPLOYMENT_TARGET)
    list(APPEND edgeflow_cxx_platform_flags "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
  endif()
endif()

# 同一份公开符号清单生成 ELF / Mach-O 的链接限制。
set(_edgeflow_exports "${PROJECT_SOURCE_DIR}/cmake_ext/templates/edgeflow_sdk.exports")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_edgeflow_exports}")
file(STRINGS "${_edgeflow_exports}" _edgeflow_sdk_symbols)
set(LLM_EDGEFLOW_SDK_EXPORT_FILE "${PROJECT_BINARY_DIR}/edgeflow_sdk.exports")
if(LLM_EDGEFLOW_BINARY_FORMAT STREQUAL "MachO")
  set(_edgeflow_export_content "")
  foreach(symbol IN LISTS _edgeflow_sdk_symbols)
    string(APPEND _edgeflow_export_content "_${symbol}\n")
  endforeach()
  set(LLM_EDGEFLOW_SDK_LINK_OPTIONS "LINKER:-exported_symbols_list,${LLM_EDGEFLOW_SDK_EXPORT_FILE}")
else()
  set(_edgeflow_export_content "{\n  global:\n")
  foreach(symbol IN LISTS _edgeflow_sdk_symbols)
    string(APPEND _edgeflow_export_content "    ${symbol};\n")
  endforeach()
  string(APPEND _edgeflow_export_content "  local: *;\n};\n")
  set(LLM_EDGEFLOW_SDK_LINK_OPTIONS "LINKER:--version-script=${LLM_EDGEFLOW_SDK_EXPORT_FILE}")
endif()
# 内容不变时保留时间戳，避免 LINK_DEPENDS 在每次配置后触发 SDK 重链接。
file(GENERATE OUTPUT "${LLM_EDGEFLOW_SDK_EXPORT_FILE}" CONTENT "${_edgeflow_export_content}")

include(cmake_ext/support/OperatorShell.cmake)
include(cmake_ext/support/LayerHeaderViews.cmake)
add_library(edgeflow_public_headers INTERFACE)
target_include_directories(edgeflow_public_headers INTERFACE
    $<BUILD_INTERFACE:${edgeflow_public_include_dir}>)

# 源码扩展使用编写契约，不属于动态 SDK ABI。
add_library(edgeflow_extension_headers INTERFACE)
target_include_directories(edgeflow_extension_headers INTERFACE
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(edgeflow_extension_headers INTERFACE
    edgeflow_public_headers nlohmann_json::nlohmann_json)

# 仓库内工具和测试可访问源码；公开 SDK 目标只传播明确列出的调用头。
add_library(edgeflow_internal_headers INTERFACE)
target_include_directories(edgeflow_internal_headers INTERFACE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(edgeflow_internal_headers INTERFACE
    edgeflow_extension_headers)
