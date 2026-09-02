link_libraries(edgeflow_internal_headers)

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
  $<TARGET_OBJECTS:edgeflow_test_business_model_fixtures>)
target_link_libraries(alg_pipeline_tool_test PRIVATE llm_edgeflow::internal_runtime)

include(${PROJECT_SOURCE_DIR}/cmake/Tests.cmake)

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
