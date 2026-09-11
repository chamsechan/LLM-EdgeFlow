link_libraries(edgeflow_internal_headers)
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  # Match runtime type visibility for std::any Blackboard payloads. With default
  # visibility, TextTemplateNodeTest/CommonNodesTest fail to read attributes.
  set(CMAKE_CXX_VISIBILITY_PRESET hidden)
endif()

# Allocation replacement is confined to the focused test executables. It is
# never linked into runtime objects, the SDK, demos or repository tools.
add_library(edgeflow_test_allocation_failure OBJECT
  support/scoped_allocation_failure.cpp)
target_include_directories(edgeflow_test_allocation_failure PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/support")

if(UNIX AND NOT APPLE)
  add_test(
    NAME SdkExportSurfaceTest
    COMMAND ${PROJECT_SOURCE_DIR}/scripts/check_sdk_exports.sh
            $<TARGET_FILE:alg_sdk>)
  set_tests_properties(SdkExportSurfaceTest PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    LABELS "tier2;static-gate;dev-fast;sanitizer-compatible")
endif()

# Validation parity uses deterministic registrations without contaminating the
# production CLI or shared SDK.
add_executable(alg_pipeline_tool_test
  ${PROJECT_SOURCE_DIR}/src/tools/alg_pipeline_tool.cpp
  $<TARGET_OBJECTS:edgeflow_test_backend_fixtures>
  $<TARGET_OBJECTS:edgeflow_test_biz_model_fixtures>)
target_link_libraries(alg_pipeline_tool_test PRIVATE llm_edgeflow::internal_runtime)

# GNU-compatible link wrapping is confined to the existing model test runner.
function(edgeflow_enable_aligned_allocation_failure target_name)
  if(UNIX AND NOT APPLE)
    target_compile_definitions(${target_name} PRIVATE
      LLM_EDGEFLOW_TEST_WRAP_POSIX_MEMALIGN=1)
    target_link_options(${target_name} PRIVATE "LINKER:--wrap=posix_memalign")
  endif()
endfunction()

add_test(NAME QualityGateScriptsContractTest
  COMMAND ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_quality_gate_contract.sh)
set_tests_properties(QualityGateScriptsContractTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "tier1;static-gate;dev-fast;sanitizer-compatible")

include(${PROJECT_SOURCE_DIR}/cmake_ext/Tests.cmake)

add_test(NAME JsonPromptDemoTest
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/tooling/test_json_prompt_demo.py)
set_tests_properties(JsonPromptDemoTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "tier3;tooling;dev-fast;sanitizer-compatible")

add_test(NAME TestLabelsContractTest
  COMMAND ${Python3_EXECUTABLE}
          ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_test_labels_contract.py
          --build-dir ${PROJECT_BINARY_DIR})
set_tests_properties(TestLabelsContractTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "tier1;static-gate;dev-fast;sanitizer-compatible")

option(ENABLE_REAL_MODEL_TESTS "Enable real model weights E2E test suite" OFF)
if(ENABLE_REAL_MODEL_TESTS)
  add_executable(test_real_models_e2e
    ${EDGEFLOW_SOURCE_test_real_models_e2e})
  target_compile_definitions(test_real_models_e2e PRIVATE
    LLM_EDGEFLOW_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  target_link_libraries(test_real_models_e2e PRIVATE
    llm_edgeflow::internal_runtime GTest::gtest GTest::gtest_main)
  add_test(NAME RealModelE2ETest COMMAND test_real_models_e2e)
  set_tests_properties(RealModelE2ETest PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    LABELS "tier3;slow;real-model"
    TIMEOUT 1800)
endif()
