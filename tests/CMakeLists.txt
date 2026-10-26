link_libraries(edgeflow_internal_headers)
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  # 与 std::any 类型的 Blackboard payload 保持一致的运行时类型可见性。使用默认
  # 可见性时，TextTemplateNodeTest/PromptGuidedLlmNodeTest 无法读取属性。
  set(CMAKE_CXX_VISIBILITY_PRESET hidden)
endif()

# 分配替换仅限于相关的测试可执行文件，
# 绝不链接进运行时对象、SDK、Demo 或仓库工具。
add_library(edgeflow_test_allocation_failure OBJECT
  support/scoped_allocation_failure.cpp)
target_include_directories(edgeflow_test_allocation_failure PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/support")

add_test(
  NAME SdkExportSurfaceTest
  COMMAND ${PROJECT_SOURCE_DIR}/scripts/check_sdk_exports.sh
          $<TARGET_FILE:alg_sdk> ${LLM_EDGEFLOW_BINARY_FORMAT} ${CMAKE_NM})
set_tests_properties(SdkExportSurfaceTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "tier2;static-gate;dev-fast;sanitizer-compatible")

# 校验一致性测试使用确定性注册，不污染生产 CLI 或共享 SDK。
add_executable(alg_pipeline_tool_test
  ${PROJECT_SOURCE_DIR}/src/cli/alg_pipeline_tool.cpp
  $<TARGET_OBJECTS:edgeflow_demo_fixture_models>)
target_link_libraries(alg_pipeline_tool_test PRIVATE llm_edgeflow::internal_runtime
  edgeflow_pipeline_tooling edgeflow_demo_profile_fields)
target_compile_definitions(alg_pipeline_tool_test PRIVATE
  LLM_EDGEFLOW_TOOL_HAS_TEST_REGISTRATIONS=1)

# GNU 兼容的链接包装仅限于现有的模型测试 runner。
function(edgeflow_enable_aligned_allocation_failure target_name)
  if(UNIX AND NOT APPLE)
    target_compile_definitions(${target_name} PRIVATE
      LLM_EDGEFLOW_TEST_WRAP_POSIX_MEMALIGN=1)
    target_link_options(${target_name} PRIVATE "LINKER:--wrap=posix_memalign")
  endif()
endfunction()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(_edgeflow_guard_sanitizers "")
if(ENABLE_SANITIZERS)
  list(APPEND _edgeflow_guard_sanitizers --sanitizers "${LLM_EDGEFLOW_SANITIZERS}")
endif()
add_test(NAME QualityGateScriptsContractTest
  COMMAND ${Python3_EXECUTABLE}
          ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_quality_gate_contract.py
          --cxx ${CMAKE_CXX_COMPILER}
          ${_edgeflow_guard_sanitizers}
          --gtest-include $<TARGET_PROPERTY:GTest::gtest,INTERFACE_INCLUDE_DIRECTORIES>
          --gtest-library $<TARGET_FILE:GTest::gtest>
          --gtest-main ${PROJECT_SOURCE_DIR}/tests/support/gtest_main.cpp
          --ctest-policy ${PROJECT_SOURCE_DIR}/tests/SafeGtestDiscovery.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/RuntimeTests.cmake)

add_test(NAME JsonPromptDemoTest
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/tooling/test_json_prompt_demo.py)
set_tests_properties(JsonPromptDemoTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "tier3;tooling;dev-fast;sanitizer-compatible"
  ENVIRONMENT "LLM_EDGEFLOW_DEMO_BINARY=$<TARGET_FILE:alg_demo>")

add_test(NAME TestLabelsContractTest
  COMMAND ${Python3_EXECUTABLE}
          ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_test_labels_contract.py
          --build-dir ${PROJECT_BINARY_DIR})
set_tests_properties(QualityGateScriptsContractTest TestLabelsContractTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "tier1;static-gate;dev-fast;sanitizer-compatible")

option(ENABLE_REAL_MODEL_TESTS "Enable real model weights E2E test suite" OFF)
if(ENABLE_REAL_MODEL_TESTS)
  add_executable(test_real_models_e2e
    "${PROJECT_SOURCE_DIR}/tests/e2e/real_models/test_real_models_e2e.cpp")
  if(LLM_EDGEFLOW_HAS_WHISPERCPP)
    target_compile_definitions(test_real_models_e2e PRIVATE HAVE_WHISPERCPP=1)
  endif()
  target_compile_definitions(test_real_models_e2e PRIVATE
    LLM_EDGEFLOW_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  target_sources(test_real_models_e2e PRIVATE support/gtest_main.cpp)
  target_link_libraries(test_real_models_e2e PRIVATE
    llm_edgeflow::internal_runtime GTest::gtest)
  add_test(NAME RealModelE2ETest COMMAND test_real_models_e2e --gtest_filter=*)
  set_tests_properties(RealModelE2ETest PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    LABELS "tier3;slow;real-model"
    SKIP_RETURN_CODE 77
    TIMEOUT 1800)
endif()
