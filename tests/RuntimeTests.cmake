# 分片的 Google Test runner 与按标签驱动的开发测试矩阵。

include(${CMAKE_CURRENT_LIST_DIR}/ScaffoldFixtures.cmake)

option(LLM_EDGEFLOW_TEST_PCH "Enable precompiled headers for test runners" OFF)

add_test(NAME ThirdPartyCacheMetadataTest
  COMMAND ${CMAKE_COMMAND}
          -DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}
          -DTEST_ROOT=${PROJECT_BINARY_DIR}/third_party_cache_metadata_test
          -P ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_third_party_cache_metadata.cmake)

# Runtime runners share only their test entrypoint, runtime link and optional PCH.
# Fixture sources, allocation instrumentation and component libraries stay local.
function(edgeflow_configure_gtest_target target_name)
  target_sources(${target_name} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support/gtest_main.cpp")
  target_link_libraries(${target_name} PRIVATE
    llm_edgeflow::internal_runtime GTest::gtest)
  if(LLM_EDGEFLOW_TEST_PCH)
    target_precompile_headers(${target_name} PRIVATE
      <gtest/gtest.h>
      <nlohmann/json.hpp>
      <atomic>
      <memory>
      <string>
      <vector>)
  endif()
endfunction()

# 只扫描每个 runner 自有的测试目录。生成的源码、进程隔离的契约测试和
# 需显式开启的 E2E 目标保持显式列出。CONFIGURE_DEPENDS 使增删文件在下次
# 构建时触发重新生成 (CMake 3.19)。
file(GLOB EDGEFLOW_TEST_CORE_SRCS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/core/test_*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/engine/test_*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/logging/test_*.cpp")
add_executable(edgeflow_test_core_runner
  ${EDGEFLOW_TEST_CORE_SRCS}
)
edgeflow_enable_aligned_allocation_failure(edgeflow_test_core_runner)
edgeflow_configure_gtest_target(edgeflow_test_core_runner)
target_link_libraries(edgeflow_test_core_runner PRIVATE edgeflow_test_allocation_failure)

# BPE 分词器的合成 fixture：确定性生成，只依赖 Python 标准库，不需要下载。
set(EDGEFLOW_BPE_TOKENIZER_FIXTURE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures/bpe_tokenizer")
set(EDGEFLOW_BPE_METASPACE_FIXTURE
    "${EDGEFLOW_BPE_TOKENIZER_FIXTURE_DIR}/metaspace_fixture.eftok")
set(EDGEFLOW_BPE_BYTE_LEVEL_FIXTURE
    "${EDGEFLOW_BPE_TOKENIZER_FIXTURE_DIR}/byte_level_fixture.eftok")
add_custom_command(
  OUTPUT
    "${EDGEFLOW_BPE_METASPACE_FIXTURE}"
    "${EDGEFLOW_BPE_BYTE_LEVEL_FIXTURE}"
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tools/prepare_bpe_tokenizer.py"
          fixture metaspace --output "${EDGEFLOW_BPE_METASPACE_FIXTURE}"
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tools/prepare_bpe_tokenizer.py"
          fixture byte_level --output "${EDGEFLOW_BPE_BYTE_LEVEL_FIXTURE}"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/prepare_bpe_tokenizer.py"
  COMMENT "Generating synthetic BPE tokenizer fixtures"
  VERBATIM)
add_custom_target(edgeflow_bpe_tokenizer_fixtures
  DEPENDS
    "${EDGEFLOW_BPE_METASPACE_FIXTURE}"
    "${EDGEFLOW_BPE_BYTE_LEVEL_FIXTURE}")
add_dependencies(edgeflow_test_core_runner edgeflow_bpe_tokenizer_fixtures)
target_compile_definitions(edgeflow_test_core_runner PRIVATE
  EDGEFLOW_BPE_METASPACE_FIXTURE="${EDGEFLOW_BPE_METASPACE_FIXTURE}"
  EDGEFLOW_BPE_BYTE_LEVEL_FIXTURE="${EDGEFLOW_BPE_BYTE_LEVEL_FIXTURE}"
  EDGEFLOW_BPE_TOKENIZER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bpe_tokenizer")

# tools/prepare_bpe_tokenizer.py verify 调用的驱动，不是 CTest 用例。
add_executable(edgeflow_bpe_tokenizer_driver
  "${PROJECT_SOURCE_DIR}/tests/support/bpe_tokenizer_driver.cpp")
target_link_libraries(edgeflow_bpe_tokenizer_driver PRIVATE
  llm_edgeflow::internal_runtime)
if(LLM_EDGEFLOW_HAS_ONNXRUNTIME)
  set(EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures/models")
  set(EDGEFLOW_VOCAB_FIXTURE
      "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}/vocab.txt")
  set(EDGEFLOW_RERANK_ONNX_FIXTURE
      "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}/rerank_fixture.onnx")
  set(EDGEFLOW_NON_TENSOR_ONNX_FIXTURE
      "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}/non_tensor_output_fixture.onnx")
  file(MAKE_DIRECTORY "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}")
  add_custom_command(
    OUTPUT
      "${EDGEFLOW_RERANK_ONNX_FIXTURE}"
      "${EDGEFLOW_NON_TENSOR_ONNX_FIXTURE}"
      "${EDGEFLOW_VOCAB_FIXTURE}"
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/scripts/generate_test_onnx_model.py"
            --output-dir "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}"
    DEPENDS "${PROJECT_SOURCE_DIR}/scripts/generate_test_onnx_model.py"
    COMMENT "Generating small rerank and non-tensor ONNX Runtime fixtures"
    VERBATIM)
  add_custom_target(edgeflow_generated_model_fixtures
    DEPENDS
      "${EDGEFLOW_RERANK_ONNX_FIXTURE}"
      "${EDGEFLOW_NON_TENSOR_ONNX_FIXTURE}"
      "${EDGEFLOW_VOCAB_FIXTURE}")
  configure_file(
    "${PROJECT_SOURCE_DIR}/tests/fixtures/pipelines/cross_rerank/pipeline_cross_rerank_fixture.json"
    "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}/pipeline_cross_rerank_fixture.json"
    COPYONLY)
  configure_file(
    "${PROJECT_SOURCE_DIR}/tests/fixtures/pipelines/cross_rerank/pipeline_cross_rerank_fixture.conf"
    "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}/pipeline_cross_rerank_fixture.conf"
    COPYONLY)
  file(READ "${PROJECT_SOURCE_DIR}/tests/fixtures/pipelines/cross_rerank/pipeline_cross_rerank_fixture.json" _rerank_json)
  string(JSON _missing_rerank SET "${_rerank_json}" models 0 file "\"missing-fixture.onnx\"")
  file(WRITE "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}/pipeline_cross_rerank_missing_model.json" "${_missing_rerank}\n")
  file(WRITE "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}/pipeline_cross_rerank_missing_model.conf"
    "{\"pipe_path\":\"pipeline_cross_rerank_missing_model.json\"}\n")
endif()

file(GLOB EDGEFLOW_TEST_NODE_SRCS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/nodes/test_*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/contracts/test_*.cpp")
add_executable(edgeflow_test_nodes_runner
  ${EDGEFLOW_SCAFFOLD_FIXTURE_SOURCE}
  ${EDGEFLOW_TEST_NODE_SRCS}
)
edgeflow_configure_gtest_target(edgeflow_test_nodes_runner)
target_link_libraries(edgeflow_test_nodes_runner PRIVATE edgeflow_test_allocation_failure)

# 壳无关一致性套件位于 contract/operator_shell/；具体壳的驱动、清单及依赖其
# 载体的测试位于各 runner 目录下的 operator_shell/<shell>/，只收集选中的壳。
set(EDGEFLOW_TEST_SHELL "operator_shell/${EDGEFLOW_OPERATOR_SHELL}")
file(GLOB EDGEFLOW_TEST_ADAPTER_SRCS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/adapter/test_*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/operator/test_*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/operator/test_*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/contract/operator_shell/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/contract/${EDGEFLOW_TEST_SHELL}/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/operator/${EDGEFLOW_TEST_SHELL}/test_*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/operator/${EDGEFLOW_TEST_SHELL}/test_*.cpp"
)
list(APPEND EDGEFLOW_TEST_ADAPTER_SRCS "${EDGEFLOW_CONTROL_FIXTURE_SOURCE}")
add_executable(edgeflow_test_adapter_runner
  ${EDGEFLOW_TEST_ADAPTER_SRCS}
)
edgeflow_configure_gtest_target(edgeflow_test_adapter_runner)
target_link_libraries(edgeflow_test_adapter_runner PRIVATE edgeflow_test_allocation_failure)

file(GLOB EDGEFLOW_TEST_TOOLING_SRCS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/demo/test_*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/demo/${EDGEFLOW_TEST_SHELL}/test_*.cpp")
list(APPEND EDGEFLOW_TEST_TOOLING_SRCS
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/pipeline/test_pipeline_catalog_validator.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/pipeline/test_pipeline_authoring.cpp"
  "${EDGEFLOW_CONTROL_FIXTURE_SOURCE}")
add_executable(edgeflow_test_tooling_runner
  ${EDGEFLOW_TEST_TOOLING_SRCS}
  $<TARGET_OBJECTS:edgeflow_demo_objects>
)
edgeflow_configure_gtest_target(edgeflow_test_tooling_runner)
target_link_libraries(edgeflow_test_tooling_runner PRIVATE edgeflow_pipeline_tooling)
target_compile_definitions(edgeflow_test_tooling_runner PRIVATE
  EDGEFLOW_DEMO_BINARY="$<TARGET_FILE:alg_demo>")
add_dependencies(edgeflow_test_tooling_runner alg_demo)

if(LLM_EDGEFLOW_HAS_WHISPERCPP)
  target_compile_definitions(edgeflow_test_core_runner PRIVATE HAVE_WHISPERCPP=1)
  target_compile_definitions(edgeflow_test_tooling_runner PRIVATE HAVE_WHISPERCPP=1)
endif()

if(LLM_EDGEFLOW_HAS_LLAMACPP)
  target_compile_definitions(edgeflow_test_core_runner PRIVATE HAVE_LLAMACPP=1)
endif()
if(LLM_EDGEFLOW_HAS_KITELLM)
  target_compile_definitions(edgeflow_test_core_runner PRIVATE HAVE_KITELLM=1)
endif()
if(LLM_EDGEFLOW_HAS_INFERLET)
  target_compile_definitions(edgeflow_test_core_runner PRIVATE HAVE_INFERLET=1)
endif()

if(LLM_EDGEFLOW_HAS_ONNXRUNTIME)
  foreach(target
      edgeflow_test_core_runner
      edgeflow_test_nodes_runner
      edgeflow_test_adapter_runner
      edgeflow_test_tooling_runner)
    add_dependencies(${target} edgeflow_generated_model_fixtures)
    target_compile_definitions(${target} PRIVATE
      HAVE_ONNXRUNTIME=1
      EDGEFLOW_RERANK_ONNX_FIXTURE="${EDGEFLOW_RERANK_ONNX_FIXTURE}"
      EDGEFLOW_NON_TENSOR_ONNX_FIXTURE="${EDGEFLOW_NON_TENSOR_ONNX_FIXTURE}"
      EDGEFLOW_VOCAB_FIXTURE="${EDGEFLOW_VOCAB_FIXTURE}")
  endforeach()
  add_dependencies(alg_demo edgeflow_generated_model_fixtures)
endif()

# 进程隔离的目标。注册表冲突测试有意让每个污染单例的场景运行在独立进程中。
add_executable(test_cpp_operator_sdk "${PROJECT_SOURCE_DIR}/tests/contract/abi/test_cpp_operator_sdk.cpp")
set_target_properties(test_cpp_operator_sdk PROPERTIES LINK_LIBRARIES "llm_edgeflow::sdk")

add_executable(test_registry_conflict "${PROJECT_SOURCE_DIR}/tests/contract/catalog/test_registry_conflict.cpp")
edgeflow_configure_gtest_target(test_registry_conflict)

add_executable(test_model_backend_registry_conflict
  "${PROJECT_SOURCE_DIR}/tests/contract/catalog/test_model_backend_registry_conflict.cpp")
edgeflow_configure_gtest_target(test_model_backend_registry_conflict)

add_executable(test_catalog_contract_ssot
  "${PROJECT_SOURCE_DIR}/tests/contract/catalog/test_catalog_contract_ssot.cpp")
edgeflow_configure_gtest_target(test_catalog_contract_ssot)
target_link_libraries(test_catalog_contract_ssot PRIVATE edgeflow_pipeline_tooling)
foreach(backend ONNXRUNTIME LLAMACPP WHISPERCPP KITELLM INFERLET)
  target_compile_definitions(test_catalog_contract_ssot PRIVATE
    EDGEFLOW_EXPECT_${backend}=$<BOOL:${LLM_EDGEFLOW_HAS_${backend}}>)
endforeach()

# GoogleTest discovers the compiled cases, including generated tests. There is
# no historical suite allowlist; each case gets its own actual execution filter.
include(GoogleTest)
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/SafeGtestDiscovery.cmake")
set(_edgeflow_tier2 "tier2;dev-fast;sanitizer-compatible;sanitizer-runtime")
set(_edgeflow_tier3 "tier3;integration;dev-fast;sanitizer-compatible;sanitizer-runtime")
set(_edgeflow_tier4 "tier4;tooling;dev-fast;sanitizer-compatible")
function(edgeflow_discover target)
  gtest_discover_tests(${target}
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    TEST_LIST ${target}_cases
    PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endfunction()
edgeflow_discover(edgeflow_test_core_runner)
edgeflow_discover(edgeflow_test_nodes_runner)
edgeflow_discover(edgeflow_test_adapter_runner)
edgeflow_discover(edgeflow_test_tooling_runner)
edgeflow_discover(test_registry_conflict)
edgeflow_discover(test_model_backend_registry_conflict)
edgeflow_discover(test_catalog_contract_ssot)
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/RuntimeLabels.cmake")
add_test(NAME CppOperatorSdkTest COMMAND test_cpp_operator_sdk)
set_tests_properties(CppOperatorSdkTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "${_edgeflow_tier2}")

# 架构与源码治理门禁。
add_test(NAME CMakeStructureTest
  COMMAND ${CMAKE_COMMAND}
          -DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}
          -P ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_cmake_structure.cmake)
# 记录各目标实际源码，供壳选择检查确认生产目标不含测试驱动与清单。
set(_edgeflow_shell_manifest "set(operator_shell [==[${EDGEFLOW_OPERATOR_SHELL}]==])\n")
set(_edgeflow_shell_production_targets
  edgeflow_model_execution_objects edgeflow_model_execution_backends_objects
  edgeflow_capability_nodes_objects edgeflow_orchestration_objects
  edgeflow_integration_objects edgeflow_composition_objects
  edgeflow_demo_objects edgeflow_pipeline_tooling)
set(_edgeflow_shell_test_targets
  edgeflow_test_adapter_runner edgeflow_test_tooling_runner)
string(APPEND _edgeflow_shell_manifest
  "set(production_targets [==[${_edgeflow_shell_production_targets}]==])\n"
  "set(test_targets [==[${_edgeflow_shell_test_targets}]==])\n")
foreach(target IN LISTS _edgeflow_shell_production_targets _edgeflow_shell_test_targets)
  string(APPEND _edgeflow_shell_manifest
    "set(${target}_sources [==[$<TARGET_PROPERTY:${target},SOURCES>]==])\n")
endforeach()
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/operator_shell_sources_$<CONFIG>.cmake"
     CONTENT "${_edgeflow_shell_manifest}")
add_test(NAME OperatorShellSelectionTest
  COMMAND ${CMAKE_COMMAND}
          -DTEST_ROOT=${PROJECT_BINARY_DIR}/operator_shell_selection_test
          "-DSOURCE_MANIFEST=${PROJECT_BINARY_DIR}/operator_shell_sources_$<CONFIG>.cmake"
          -P ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_operator_shell_selection.cmake)
add_test(NAME LayerGuardTest
  COMMAND ${CMAKE_COMMAND} -E env
          "LLM_EDGEFLOW_LAYER_COMPILE_MANIFEST=${PROJECT_BINARY_DIR}/layer_includes/compile_checks_$<CONFIG>.cmake"
          ${PROJECT_SOURCE_DIR}/scripts/check_layer_isolation.sh)
add_test(NAME LayerGuardSelfTest
  COMMAND ${Python3_EXECUTABLE}
          ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_layer_guard_contract.py)
add_test(NAME SpecSignatureDiagnosticsTest
  COMMAND ${CMAKE_COMMAND}
          "-DLAYER_COMPILE_MANIFEST=${PROJECT_BINARY_DIR}/layer_includes/compile_checks_$<CONFIG>.cmake"
          -P "${PROJECT_SOURCE_DIR}/tests/contract/authoring/test_spec_signature_diagnostics.cmake")
add_test(NAME ArchitectureDocsDriftTest
  COMMAND ${PROJECT_SOURCE_DIR}/scripts/check_architecture_docs.sh)
add_test(NAME ArchitectureDocsDriftGateSelfTest
  COMMAND ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_architecture_docs_drift_gate.sh)
add_test(NAME GovernanceConsistencyTest
  COMMAND ${PROJECT_SOURCE_DIR}/scripts/check_governance.sh)
add_test(NAME DocLinksTest
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_doc_links.py)
add_test(NAME DocLinksSelfTest
  COMMAND ${Python3_EXECUTABLE}
          ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_doc_links_contract.py)
add_test(NAME DiagramAssetsCheckTest
  COMMAND ${PROJECT_SOURCE_DIR}/scripts/render_architecture_diagrams.sh --check)
add_test(NAME DiagramRenderGateSelfTest
  COMMAND ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_diagram_render_gate.sh)
add_test(NAME ScriptGeneratorDetectionTest
  COMMAND ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_script_generator_detection.sh)
set_tests_properties(
  CMakeStructureTest ThirdPartyCacheMetadataTest SpecSignatureDiagnosticsTest
  OperatorShellSelectionTest
  LayerGuardTest LayerGuardSelfTest ArchitectureDocsDriftTest
  ArchitectureDocsDriftGateSelfTest GovernanceConsistencyTest
  DocLinksTest DocLinksSelfTest ScriptGeneratorDetectionTest
  PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "tier1;static-gate;dev-fast;sanitizer-compatible")
set_tests_properties(DiagramAssetsCheckTest DiagramRenderGateSelfTest
  PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "tier4;tooling;static-gate;slow")

add_test(NAME PipelineStudioServerTest
  COMMAND ${Python3_EXECUTABLE}
          ${PROJECT_SOURCE_DIR}/tests/tooling/test_pipeline_studio.py)
set_tests_properties(PipelineStudioServerTest PROPERTIES
  ENVIRONMENT
    "LLM_EDGEFLOW_PIPELINE_TOOL=$<TARGET_FILE:alg_pipeline_tool_test>;LLM_EDGEFLOW_SELECTION_TOOL=$<TARGET_FILE:alg_pipeline_tool>;LLM_EDGEFLOW_DEMO_BINARY=$<TARGET_FILE:alg_demo>;LLM_EDGEFLOW_ALG_SHOW=$<TARGET_FILE:alg_show>")

add_test(NAME CustomNodeScaffoldTest
  COMMAND ${Python3_EXECUTABLE}
          ${PROJECT_SOURCE_DIR}/tests/tooling/test_scaffold_custom_node.py)

add_test(NAME BpeTokenizerToolTest
  COMMAND ${Python3_EXECUTABLE}
          ${PROJECT_SOURCE_DIR}/tests/tooling/test_prepare_bpe_tokenizer.py)

add_test(NAME DevRecipeTest
  COMMAND ${Python3_EXECUTABLE}
          ${PROJECT_SOURCE_DIR}/tests/tooling/test_dev_recipe.py)
set_tests_properties(DevRecipeTest PROPERTIES
  ENVIRONMENT
    "LLM_EDGEFLOW_PIPELINE_TOOL=$<TARGET_FILE:alg_pipeline_tool_test>;LLM_EDGEFLOW_SELECTION_TOOL=$<TARGET_FILE:alg_pipeline_tool>;LLM_EDGEFLOW_DEMO_BINARY=$<TARGET_FILE:alg_demo>")

add_test(NAME DemoSmokeTest COMMAND $<TARGET_FILE:alg_demo> --suite smoke)
set_tests_properties(DemoSmokeTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "${_edgeflow_tier3}")

if(LLM_EDGEFLOW_HAS_ONNXRUNTIME)
  add_test(
    NAME CrossRerankDemoFixtureTest
    COMMAND $<TARGET_FILE:alg_demo>
            --config
            "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}/pipeline_cross_rerank_fixture.conf"
            --dataset
            "${PROJECT_SOURCE_DIR}/data/corpus_cross_rerank.txt"
            --output-dir
            "${CMAKE_CURRENT_BINARY_DIR}/test-results/cross-rerank-demo")
  add_test(
    NAME CrossRerankDemoMissingModelFailsClosedTest
    COMMAND $<TARGET_FILE:alg_demo>
            --config
            "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}/pipeline_cross_rerank_missing_model.conf"
            --dataset
            "${PROJECT_SOURCE_DIR}/data/corpus_cross_rerank.txt"
            --allow-fallback-sample
            --output-dir
            "${CMAKE_CURRENT_BINARY_DIR}/test-results/cross-rerank-missing")
  set_tests_properties(
    CrossRerankDemoFixtureTest
    CrossRerankDemoMissingModelFailsClosedTest
    PROPERTIES
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      LABELS "${_edgeflow_tier3}")
  set_tests_properties(
    CrossRerankDemoMissingModelFailsClosedTest
    PROPERTIES WILL_FAIL TRUE)
endif()

add_test(NAME PipelineToolCatalogTest COMMAND $<TARGET_FILE:alg_pipeline_tool>
  catalog)
add_test(NAME PipelineToolValidateTest COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tooling/test_pipeline_cli.py
  --tool $<TARGET_FILE:alg_pipeline_tool>
  --test-tool $<TARGET_FILE:alg_pipeline_tool_test>
  --repo ${PROJECT_SOURCE_DIR})
set_tests_properties(
  PipelineStudioServerTest CustomNodeScaffoldTest BpeTokenizerToolTest DevRecipeTest
  PipelineToolCatalogTest PipelineToolValidateTest
  PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "${_edgeflow_tier4}")
set_tests_properties(PipelineToolValidateTest PROPERTIES
  ENVIRONMENT "LLM_EDGEFLOW_ALG_SHOW=$<TARGET_FILE:alg_show>")

add_custom_target(edgeflow_dev_tests DEPENDS
  alg_demo alg_pipeline_tool alg_pipeline_tool_test alg_show
  test_cpp_operator_sdk
  test_registry_conflict test_model_backend_registry_conflict
  test_catalog_contract_ssot edgeflow_bpe_tokenizer_driver edgeflow_test_core_runner
  edgeflow_test_nodes_runner edgeflow_test_adapter_runner
  edgeflow_test_tooling_runner)

get_property(_edgeflow_registered_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${_edgeflow_registered_tests} PROPERTIES TIMEOUT 120)
