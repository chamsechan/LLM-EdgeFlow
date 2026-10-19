# 编译策略、生成文件与公共/扩展/内部头文件目标。
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)
# 编译与链接加速配置
include(cmake_ext/BuildAcceleration.cmake)
add_compile_options(-Wall -Wextra -fPIC)

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

# 产品版本由 CMake 生成，公共 C 头不再维护副本。
set(LLM_EDGEFLOW_GENERATED_INCLUDE_DIR
    "${PROJECT_BINARY_DIR}/generated/include")
file(MAKE_DIRECTORY "${LLM_EDGEFLOW_GENERATED_INCLUDE_DIR}/edgeflow")
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake_ext/edgeflow_version.h.in"
  "${LLM_EDGEFLOW_GENERATED_INCLUDE_DIR}/edgeflow/version.h"
  @ONLY)
set(LLM_EDGEFLOW_SDK_VERSION_SCRIPT
    "${PROJECT_BINARY_DIR}/edgeflow_sdk.map")
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake_ext/edgeflow_sdk.map.in"
  "${LLM_EDGEFLOW_SDK_VERSION_SCRIPT}"
  @ONLY)

include(cmake_ext/OperatorShell.cmake)
include(cmake_ext/LayerHeaderViews.cmake)
add_library(edgeflow_public_headers INTERFACE)
target_include_directories(edgeflow_public_headers INTERFACE
    $<BUILD_INTERFACE:${LLM_EDGEFLOW_GENERATED_INCLUDE_DIR}>
    $<BUILD_INTERFACE:${edgeflow_public_include_dir}>)

# 源码扩展使用编写契约，不属于动态 SDK ABI。
add_library(edgeflow_extension_headers INTERFACE)
target_include_directories(edgeflow_extension_headers INTERFACE
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(edgeflow_extension_headers INTERFACE
    edgeflow_public_headers nlohmann_json::nlohmann_json)

# 仓库内工具和测试可访问源码；公开 SDK 目标只传播明确列出的调用头。
add_library(edgeflow_internal_headers INTERFACE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/demo/common/profile_fields.json")
file(READ "${PROJECT_SOURCE_DIR}/demo/common/profile_fields.json"
    LLM_EDGEFLOW_DEMO_PROFILE_FIELDS)
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake_ext/demo_profile_fields.h.in"
    "${PROJECT_BINARY_DIR}/generated/internal/demo/common/demo_profile_fields.h"
    @ONLY)
target_include_directories(edgeflow_internal_headers INTERFACE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(edgeflow_internal_headers INTERFACE
    edgeflow_extension_headers)

set(edgeflow_cxx_system_include_dirs "")
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  if(EXISTS "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1")
    list(APPEND edgeflow_cxx_system_include_dirs
        "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1")
    include_directories(SYSTEM ${edgeflow_cxx_system_include_dirs})
  endif()
endif()
