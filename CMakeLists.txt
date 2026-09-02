cmake_minimum_required(VERSION 3.16)
project(LLMEdgeFlow VERSION 10.0.0 LANGUAGES C CXX)

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
include(cmake/BuildAcceleration.cmake)
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

# 仓库内工具、Demo 与测试可以包含私有头；对外 SDK 只传播 include/。
add_library(edgeflow_internal_headers INTERFACE)
target_include_directories(edgeflow_internal_headers INTERFACE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src)

if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  if(EXISTS "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1")
    include_directories(SYSTEM "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1")
  endif()
endif()

# 引入第三方 JSON 库 (nlohmann/json) 自动下载配置
include(cmake/NlohmannJson.cmake)

# 引入 Unicode 正则库（TextRuleMatchNode lookbehind / 命名捕获）
include(cmake/Pcre2.cmake)

# 引入第三方开源推理引擎 (ONNX Runtime & llama.cpp) 自动下载配置
include(cmake/ThirdPartyEngines.cmake)

# Keep repository executables at build/ even though their CMake ownership is
# delegated to source subdirectories. Public commands and CI scripts rely on
# this stable artifact layout.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# 1. 生成对外交付的动态算法库 (.so)
add_library(alg_sdk SHARED)
add_library(llm_edgeflow::sdk ALIAS alg_sdk)
target_link_libraries(alg_sdk PRIVATE edgeflow_internal_headers)
add_subdirectory(src)
target_link_libraries(alg_sdk
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE edgeflow_pcre2)
if(LLM_EDGEFLOW_HAS_ONNXRUNTIME)
  target_include_directories(alg_sdk PRIVATE SYSTEM "${ONNXRUNTIME_INCLUDE_DIR}")
  target_compile_definitions(alg_sdk PRIVATE HAVE_ONNXRUNTIME=1)
  target_link_libraries(alg_sdk PRIVATE "${ONNXRUNTIME_LIB}")
endif()
if(LLM_EDGEFLOW_HAS_KITELLM)
  target_include_directories(alg_sdk SYSTEM PRIVATE "${KITELLM_INCLUDE_DIR}")
  target_compile_definitions(alg_sdk PRIVATE HAVE_KITELLM=1)
  target_link_libraries(alg_sdk PRIVATE "${KITELLM_LIBRARY}")
endif()
if(LLM_EDGEFLOW_HAS_LLAMACPP)
  target_include_directories(alg_sdk SYSTEM PRIVATE ${LLAMACPP_INCLUDE_DIRS})
  target_compile_definitions(alg_sdk PRIVATE HAVE_LLAMACPP=1)
endif()
if(THIRD_PARTY_ENGINE_LIBS)
  target_link_libraries(alg_sdk PRIVATE ${THIRD_PARTY_ENGINE_LIBS})
endif()
set_target_properties(alg_sdk PROPERTIES
    OUTPUT_NAME "company_alg_sdk"
    VERSION ${PROJECT_VERSION}
    SOVERSION 5
)

add_subdirectory(dev_support)
add_subdirectory(demo)
add_subdirectory(src/tools)

include(CTest)
if(BUILD_TESTING)
  include(cmake/GoogleTest.cmake)
  option(LLM_EDGEFLOW_SHARDED_TEST_RUNNERS
         "Build PCH-enabled sharded Google Test runners" ON)
  add_subdirectory(tests)
endif()
