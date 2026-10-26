# 仓库可执行文件统一输出到 build/，保持脚本和 CLI 的产物路径。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# 对外交付的动态算法库。
set(LLM_EDGEFLOW_SDK_OUTPUT_NAME "llm_edgeflow_sdk" CACHE STRING
    "SDK library base name, without platform prefix or extension")
if(NOT LLM_EDGEFLOW_SDK_OUTPUT_NAME MATCHES "^[A-Za-z0-9_][A-Za-z0-9_.-]*$")
  message(FATAL_ERROR
      "LLM_EDGEFLOW_SDK_OUTPUT_NAME must be a nonempty library base name, not a path")
endif()
add_library(alg_sdk SHARED ${edgeflow_runtime_object_files})
add_library(llm_edgeflow::sdk ALIAS alg_sdk)
target_link_libraries(alg_sdk
    PUBLIC edgeflow_public_headers
    PRIVATE edgeflow_runtime_dependencies)
target_link_options(alg_sdk PRIVATE ${LLM_EDGEFLOW_SDK_LINK_OPTIONS})
set_target_properties(alg_sdk PROPERTIES
    OUTPUT_NAME "${LLM_EDGEFLOW_SDK_OUTPUT_NAME}"
    LINK_DEPENDS "${LLM_EDGEFLOW_SDK_EXPORT_FILE}"
)

# Demo、CLI 和测试共享的 Profile 字段头。
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/demo/common/profile_fields.json")
file(READ "${PROJECT_SOURCE_DIR}/demo/common/profile_fields.json"
    LLM_EDGEFLOW_DEMO_PROFILE_FIELDS)
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake_ext/templates/demo_profile_fields.h.in"
    "${PROJECT_BINARY_DIR}/generated/internal/demo/common/demo_profile_fields.h"
    @ONLY)
add_library(edgeflow_demo_profile_fields INTERFACE)
target_include_directories(edgeflow_demo_profile_fields INTERFACE
    "${PROJECT_BINARY_DIR}/generated/internal")

add_subdirectory(dev_support)
add_subdirectory(demo)
add_subdirectory(src/cli)

include(CTest)
if(BUILD_TESTING)
  include(cmake_ext/third_party/GoogleTest.cmake)
  add_subdirectory(tests)
endif()
