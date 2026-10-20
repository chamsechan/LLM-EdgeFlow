# 仓库可执行文件统一输出到 build/，保持脚本和 CLI 的产物路径。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# 对外交付的动态算法库。
set(LLM_EDGEFLOW_SDK_OUTPUT_NAME "llm_edgeflow_sdk" CACHE STRING
    "SDK library base name, without platform prefix or extension")
if(NOT LLM_EDGEFLOW_SDK_OUTPUT_NAME MATCHES "^[A-Za-z0-9_][A-Za-z0-9_.-]*$")
  message(FATAL_ERROR
      "LLM_EDGEFLOW_SDK_OUTPUT_NAME must be a nonempty library base name, not a path")
endif()
add_library(alg_sdk SHARED
    $<TARGET_OBJECTS:edgeflow_model_execution_objects>
    $<TARGET_OBJECTS:edgeflow_model_execution_backends_objects>
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
    OUTPUT_NAME "${LLM_EDGEFLOW_SDK_OUTPUT_NAME}"
)

add_subdirectory(dev_support)
add_subdirectory(demo)
add_subdirectory(src/cli)

include(CTest)
if(BUILD_TESTING)
  include(cmake_ext/GoogleTest.cmake)
  add_subdirectory(tests)
endif()
