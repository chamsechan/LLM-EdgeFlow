link_libraries(edgeflow_internal_headers)

# Validation parity uses deterministic registrations without contaminating the
# production CLI or shared SDK.
add_executable(alg_pipeline_tool_test
  ${PROJECT_SOURCE_DIR}/src/tools/alg_pipeline_tool.cpp
  $<TARGET_OBJECTS:edgeflow_test_backend_fixtures>
  $<TARGET_OBJECTS:edgeflow_test_business_model_fixtures>)
target_link_libraries(alg_pipeline_tool_test PRIVATE llm_edgeflow::sdk)

include(${PROJECT_SOURCE_DIR}/cmake/Tests.cmake)

option(ENABLE_REAL_MODEL_TESTS "Enable real model weights E2E test suite" OFF)
if(ENABLE_REAL_MODEL_TESTS)
  add_executable(test_real_models_e2e
    ${EDGEFLOW_SOURCE_test_real_models_e2e})
  target_link_libraries(test_real_models_e2e PRIVATE
    llm_edgeflow::sdk GTest::gtest GTest::gtest_main)
  add_test(NAME RealModelE2ETest COMMAND test_real_models_e2e)
  set_tests_properties(RealModelE2ETest PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    LABELS "tier3;slow;real-model"
    TIMEOUT 1800)
endif()
