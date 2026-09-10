cmake_minimum_required(VERSION 3.16)
project(LLMEdgeFlow VERSION 10.0.0 LANGUAGES C CXX)

set(LLM_EDGEFLOW_ABI_VERSION "5.0.0")
set(LLM_EDGEFLOW_ABI_VERSION_MAJOR 5)

# 严格禁止 In-Source 构建 (防止误用 cmake . 污染源码目录并触发重复下载阻塞)
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
  message(FATAL_ERROR "
  [ERROR] In-source builds are strictly forbidden!
  Building directly inside the source directory with 'cmake .' causes pollution and hangs on network dependencies.
  Please use out-of-source build instead:
    mkdir -p build && cd build && cmake .. && make -j4
  Or:
    cmake -B build -S . && cmake --build build -j4
  ")
endif()

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

# 确保 mock npu 模型的占位文件存在

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

# 产品版本和 ABI 版本均由 CMake 生成，公共 C 头不再维护副本。
set(LLM_EDGEFLOW_GENERATED_INCLUDE_DIR
    "${PROJECT_BINARY_DIR}/generated/include")
file(MAKE_DIRECTORY "${LLM_EDGEFLOW_GENERATED_INCLUDE_DIR}/edgeflow")
# An earlier build generated this name; it must not shadow the compatibility
# forwarding header after the canonical version changes.
file(REMOVE "${LLM_EDGEFLOW_GENERATED_INCLUDE_DIR}/company_alg_version.h")
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

include(cmake_ext/LayerHeaderViews.cmake)
add_library(edgeflow_public_headers INTERFACE)
target_include_directories(edgeflow_public_headers INTERFACE
    $<BUILD_INTERFACE:${LLM_EDGEFLOW_GENERATED_INCLUDE_DIR}>
    $<BUILD_INTERFACE:${edgeflow_public_include_dir}>)

# Source extensions use authoring contracts; they are not the dynamic SDK ABI.
add_library(edgeflow_extension_headers INTERFACE)
target_include_directories(edgeflow_extension_headers INTERFACE
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(edgeflow_extension_headers INTERFACE edgeflow_public_headers)

# 仓库内工具和测试可访问源码；公开 SDK 目标只传播明确列出的调用头。
add_library(edgeflow_internal_headers INTERFACE)
target_include_directories(edgeflow_internal_headers INTERFACE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(edgeflow_internal_headers INTERFACE
    edgeflow_extension_headers)

if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  if(EXISTS "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1")
    include_directories(SYSTEM "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1")
  endif()
endif()

# 引入第三方 JSON 库 (nlohmann/json) 自动下载配置
include(cmake_ext/NlohmannJson.cmake)
target_link_libraries(edgeflow_extension_headers INTERFACE
    nlohmann_json::nlohmann_json)
include(cmake_ext/StbImage.cmake)

# 引入 Unicode 正则库（TextRuleMatchNode lookbehind / 命名捕获）
include(cmake_ext/Pcre2.cmake)

# 引入第三方开源推理引擎 (ONNX Runtime & llama.cpp) 自动下载配置
include(cmake_ext/ThirdPartyEngines.cmake)

# Keep repository executables at build/ even though their CMake ownership is
# delegated to source subdirectories. Public commands and CI scripts rely on
# this stable artifact layout.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# 轻量运行时契约位于所有实现层下方；各层依赖 interface 只向下传播。
add_library(edgeflow_runtime_contracts INTERFACE)
target_include_directories(edgeflow_runtime_contracts INTERFACE
    "${edgeflow_contracts_include_dir}"
    "${LLM_EDGEFLOW_GENERATED_INCLUDE_DIR}")
target_link_libraries(edgeflow_runtime_contracts INTERFACE
    nlohmann_json::nlohmann_json)

add_library(edgeflow_model_execution_dependencies INTERFACE)
target_link_libraries(edgeflow_model_execution_dependencies INTERFACE
    edgeflow_runtime_contracts)

add_library(edgeflow_capability_nodes_dependencies INTERFACE)
target_link_libraries(edgeflow_capability_nodes_dependencies INTERFACE
    edgeflow_runtime_contracts
    edgeflow_model_execution_dependencies
    edgeflow_pcre2)

add_library(edgeflow_orchestration_dependencies INTERFACE)
target_link_libraries(edgeflow_orchestration_dependencies INTERFACE
    edgeflow_runtime_contracts
    edgeflow_capability_nodes_dependencies)

add_library(edgeflow_integration_dependencies INTERFACE)
target_link_libraries(edgeflow_integration_dependencies INTERFACE
    edgeflow_runtime_contracts
    edgeflow_orchestration_dependencies)

add_library(edgeflow_runtime_dependencies INTERFACE)
target_link_libraries(edgeflow_runtime_dependencies INTERFACE
    edgeflow_integration_dependencies)

function(edgeflow_add_runtime_object_target target_name dependency_target)
  add_library(${target_name} OBJECT)
  target_link_libraries(${target_name} PUBLIC ${dependency_target})
  target_compile_definitions(${target_name} PRIVATE
      COMPANY_ALG_BUILDING_SDK=1)
  target_compile_definitions(${target_name} INTERFACE
      COMPANY_ALG_STATIC_LINK=1)
  set_target_properties(${target_name} PROPERTIES
      C_VISIBILITY_PRESET hidden
      CXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN YES)
endfunction()

edgeflow_add_runtime_object_target(edgeflow_model_execution_objects
    edgeflow_model_execution_dependencies)
edgeflow_add_runtime_object_target(edgeflow_capability_nodes_objects
    edgeflow_capability_nodes_dependencies)
edgeflow_add_runtime_object_target(edgeflow_orchestration_objects
    edgeflow_orchestration_dependencies)
edgeflow_add_runtime_object_target(edgeflow_integration_objects
    edgeflow_integration_dependencies)
edgeflow_add_runtime_object_target(edgeflow_composition_objects
    edgeflow_runtime_dependencies)

foreach(layer model_execution capability_nodes orchestration integration)
  target_include_directories(edgeflow_${layer}_objects PRIVATE
      "${edgeflow_${layer}_include_dir}")
endforeach()
target_include_directories(edgeflow_composition_objects PRIVATE
    "${edgeflow_integration_include_dir}")
edgeflow_generate_layer_compile_manifest()

# 仓库工具和测试继续链接一个聚合 INTERFACE target，不依赖公共共享库的
# 内部 C++ 动态符号。该 target 自身不拥有层内源码。
add_library(edgeflow_runtime_objects INTERFACE)
add_library(llm_edgeflow::internal_runtime ALIAS edgeflow_runtime_objects)
target_link_libraries(edgeflow_runtime_objects INTERFACE
    edgeflow_runtime_dependencies)
target_compile_definitions(edgeflow_runtime_objects INTERFACE
    COMPANY_ALG_STATIC_LINK=1)

if(LLM_EDGEFLOW_HAS_ONNXRUNTIME)
  target_include_directories(edgeflow_model_execution_dependencies SYSTEM INTERFACE
      "${ONNXRUNTIME_INCLUDE_DIR}")
  target_compile_definitions(edgeflow_model_execution_dependencies INTERFACE
      HAVE_ONNXRUNTIME=1)
  target_link_libraries(edgeflow_model_execution_dependencies INTERFACE
      "${ONNXRUNTIME_LIB}")
endif()
if(LLM_EDGEFLOW_HAS_KITELLM)
  target_compile_definitions(edgeflow_model_execution_dependencies INTERFACE
      HAVE_KITELLM=1)
  target_link_libraries(edgeflow_model_execution_dependencies INTERFACE
      kiteLLM::kiteLLM)
endif()
if(LLM_EDGEFLOW_HAS_LLAMACPP)
  target_include_directories(edgeflow_model_execution_dependencies SYSTEM INTERFACE
      ${LLAMACPP_INCLUDE_DIRS})
  target_compile_definitions(edgeflow_model_execution_dependencies INTERFACE
      HAVE_LLAMACPP=1)
endif()
if(LLM_EDGEFLOW_HAS_WHISPERCPP)
  target_include_directories(edgeflow_model_execution_dependencies SYSTEM INTERFACE
      ${WHISPERCPP_INCLUDE_DIRS})
  target_compile_definitions(edgeflow_model_execution_dependencies INTERFACE
      HAVE_WHISPERCPP=1)
endif()
if(THIRD_PARTY_ENGINE_LIBS)
  target_link_libraries(edgeflow_model_execution_dependencies INTERFACE
      ${THIRD_PARTY_ENGINE_LIBS})
endif()

add_subdirectory(src)

target_sources(edgeflow_runtime_objects INTERFACE
    $<TARGET_OBJECTS:edgeflow_model_execution_objects>
    $<TARGET_OBJECTS:edgeflow_capability_nodes_objects>
    $<TARGET_OBJECTS:edgeflow_orchestration_objects>
    $<TARGET_OBJECTS:edgeflow_integration_objects>
    $<TARGET_OBJECTS:edgeflow_composition_objects>)

# 1. 生成对外交付的动态算法库 (.so)
add_library(alg_sdk SHARED
    $<TARGET_OBJECTS:edgeflow_model_execution_objects>
    $<TARGET_OBJECTS:edgeflow_capability_nodes_objects>
    $<TARGET_OBJECTS:edgeflow_orchestration_objects>
    $<TARGET_OBJECTS:edgeflow_integration_objects>
    $<TARGET_OBJECTS:edgeflow_composition_objects>)
add_library(llm_edgeflow::sdk ALIAS alg_sdk)
target_link_libraries(alg_sdk
    PUBLIC edgeflow_public_headers
    PRIVATE edgeflow_runtime_dependencies)
if(UNIX AND NOT APPLE)
  target_link_options(alg_sdk PRIVATE
      "LINKER:--version-script=${LLM_EDGEFLOW_SDK_VERSION_SCRIPT}")
  set_target_properties(alg_sdk PROPERTIES
      LINK_DEPENDS "${LLM_EDGEFLOW_SDK_VERSION_SCRIPT}")
endif()
set_target_properties(alg_sdk PROPERTIES
    OUTPUT_NAME "company_alg_sdk"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${LLM_EDGEFLOW_ABI_VERSION_MAJOR}
)

add_subdirectory(dev_support)
add_subdirectory(demo)
add_subdirectory(src/tools)

include(CTest)
if(BUILD_TESTING)
  include(cmake_ext/GoogleTest.cmake)
  option(LLM_EDGEFLOW_SHARDED_TEST_RUNNERS
         "Build PCH-enabled sharded Google Test runners" ON)
  add_subdirectory(tests)
endif()
