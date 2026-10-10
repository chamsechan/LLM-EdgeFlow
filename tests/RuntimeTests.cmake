# 分片的 Google Test runner 与按标签驱动的开发测试矩阵。

include(${CMAKE_CURRENT_LIST_DIR}/ScaffoldFixtures.cmake)

option(LLM_EDGEFLOW_TEST_PCH "Enable precompiled headers for test runners" OFF)

add_test(NAME ThirdPartyCacheMetadataTest
  COMMAND ${CMAKE_COMMAND}
          -DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}
          -DTEST_ROOT=${PROJECT_BINARY_DIR}/third_party_cache_metadata_test
          -P ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_third_party_cache_metadata.cmake)
set_tests_properties(ThirdPartyCacheMetadataTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "tier1;static-gate;dev-fast;sanitizer-compatible")

function(edgeflow_enable_test_pch target_name)
  if(NOT LLM_EDGEFLOW_TEST_PCH)
    return()
  endif()
  target_precompile_headers(${target_name} PRIVATE
    <gtest/gtest.h>
    <nlohmann/json.hpp>
    <atomic>
    <memory>
    <string>
    <vector>)
endfunction()

function(edgeflow_add_runner_test test_name runner_name gtest_filter labels)
  add_test(
    NAME ${test_name}
    COMMAND $<TARGET_FILE:${runner_name}> --gtest_filter=${gtest_filter})
  set_tests_properties(${test_name} PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    LABELS "${labels}")
endfunction()

# 只扫描每个 runner 自有的测试目录。生成的源码、进程隔离的契约测试和
# 需显式开启的 E2E 目标保持显式列出。CONFIGURE_DEPENDS 使增删文件在下次
# 构建时触发重新生成 (CMake 3.19)。
file(GLOB EDGEFLOW_TEST_CORE_SRCS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/core/test_*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/engine/test_*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/logging/test_*.cpp")
# Pipeline 集成测试的 runner 依赖不同。
list(APPEND EDGEFLOW_TEST_CORE_SRCS
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/pipeline/test_model_backend_pipeline.cpp")
add_executable(edgeflow_test_core_runner
  ${EDGEFLOW_TEST_CORE_SRCS}
  $<TARGET_OBJECTS:edgeflow_test_backend_fixtures>
  $<TARGET_OBJECTS:edgeflow_test_biz_model_fixtures>)
edgeflow_enable_aligned_allocation_failure(edgeflow_test_core_runner)
target_link_libraries(edgeflow_test_core_runner PRIVATE
  llm_edgeflow::internal_runtime GTest::gtest GTest::gtest_main
  edgeflow_test_allocation_failure)
if(LLM_EDGEFLOW_HAS_ONNXRUNTIME)
  set(EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures/models")
  set(EDGEFLOW_EMBEDDING_ONNX_FIXTURE
      "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}/embedding_fixture.onnx")
  set(EDGEFLOW_VOCAB_FIXTURE
      "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}/vocab.txt")
  set(EDGEFLOW_RERANK_ONNX_FIXTURE
      "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}/rerank_fixture.onnx")
  set(EDGEFLOW_NON_TENSOR_ONNX_FIXTURE
      "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}/non_tensor_output_fixture.onnx")
  file(MAKE_DIRECTORY "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}")
  add_custom_command(
    OUTPUT
      "${EDGEFLOW_EMBEDDING_ONNX_FIXTURE}"
      "${EDGEFLOW_RERANK_ONNX_FIXTURE}"
      "${EDGEFLOW_NON_TENSOR_ONNX_FIXTURE}"
      "${EDGEFLOW_VOCAB_FIXTURE}"
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/scripts/generate_test_onnx_model.py"
            --output-dir "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}"
    DEPENDS "${PROJECT_SOURCE_DIR}/scripts/generate_test_onnx_model.py"
    COMMENT "Generating deterministic generated embedding and rerank ONNX Runtime fixtures"
    VERBATIM)
  add_custom_target(edgeflow_generated_model_fixtures
    DEPENDS
      "${EDGEFLOW_EMBEDDING_ONNX_FIXTURE}"
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
  configure_file(
    "${PROJECT_SOURCE_DIR}/tests/fixtures/pipelines/cross_rerank/pipeline_cross_rerank_missing_model.json"
    "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}/pipeline_cross_rerank_missing_model.json"
    COPYONLY)
  configure_file(
    "${PROJECT_SOURCE_DIR}/tests/fixtures/pipelines/cross_rerank/pipeline_cross_rerank_missing_model.conf"
    "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}/pipeline_cross_rerank_missing_model.conf"
    COPYONLY)
endif()
edgeflow_enable_test_pch(edgeflow_test_core_runner)

file(GLOB EDGEFLOW_TEST_NODE_SRCS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/nodes/test_*.cpp")
add_executable(edgeflow_test_nodes_runner
  ${EDGEFLOW_SCAFFOLD_FIXTURE_SOURCE}
  ${EDGEFLOW_TEST_NODE_SRCS}
  $<TARGET_OBJECTS:edgeflow_test_backend_fixtures>
  $<TARGET_OBJECTS:edgeflow_test_biz_model_fixtures>)
target_link_libraries(edgeflow_test_nodes_runner PRIVATE
  llm_edgeflow::internal_runtime GTest::gtest GTest::gtest_main
  edgeflow_test_allocation_failure)
edgeflow_enable_test_pch(edgeflow_test_nodes_runner)

file(GLOB EDGEFLOW_TEST_ADAPTER_SRCS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/adapter/test_*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/operator/test_*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/operator/test_*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/runtime/test_*.cpp")
list(APPEND EDGEFLOW_TEST_ADAPTER_SRCS
  "${CMAKE_CURRENT_SOURCE_DIR}/contract/abi/test_operator_safety.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/contract/abi/test_adapter_contract_security.cpp"
  "${EDGEFLOW_CONTROL_FIXTURE_SOURCE}")
add_executable(edgeflow_test_adapter_runner
  ${EDGEFLOW_TEST_ADAPTER_SRCS}
  $<TARGET_OBJECTS:edgeflow_test_backend_fixtures>
  $<TARGET_OBJECTS:edgeflow_test_biz_model_fixtures>)
target_link_libraries(edgeflow_test_adapter_runner PRIVATE
  llm_edgeflow::internal_runtime GTest::gtest GTest::gtest_main
  edgeflow_test_allocation_failure)
edgeflow_enable_test_pch(edgeflow_test_adapter_runner)

file(GLOB EDGEFLOW_TEST_TOOLING_SRCS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/demo/test_*.cpp")
list(APPEND EDGEFLOW_TEST_TOOLING_SRCS
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/pipeline/test_doc_qa_rerank.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/pipeline/test_pipeline_catalog_validator.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/pipeline/test_pipeline_authoring.cpp"
  "${EDGEFLOW_CONTROL_FIXTURE_SOURCE}")
add_executable(edgeflow_test_tooling_runner
  ${EDGEFLOW_TEST_TOOLING_SRCS}
  $<TARGET_OBJECTS:edgeflow_demo_objects>
  $<TARGET_OBJECTS:edgeflow_test_backend_fixtures>
  $<TARGET_OBJECTS:edgeflow_test_biz_model_fixtures>)
target_link_libraries(edgeflow_test_tooling_runner PRIVATE
  llm_edgeflow::internal_runtime edgeflow_pipeline_tooling GTest::gtest
  GTest::gtest_main)
edgeflow_enable_test_pch(edgeflow_test_tooling_runner)
target_compile_definitions(edgeflow_test_tooling_runner PRIVATE
  EDGEFLOW_DEMO_BINARY="$<TARGET_FILE:alg_demo>")
add_dependencies(edgeflow_test_tooling_runner alg_demo)

if(LLM_EDGEFLOW_HAS_WHISPERCPP)
  target_compile_definitions(edgeflow_test_core_runner PRIVATE HAVE_WHISPERCPP=1)
  target_compile_definitions(edgeflow_test_tooling_runner PRIVATE HAVE_WHISPERCPP=1)
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
      EDGEFLOW_EMBEDDING_ONNX_FIXTURE="${EDGEFLOW_EMBEDDING_ONNX_FIXTURE}"
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
target_link_libraries(test_registry_conflict PRIVATE
  llm_edgeflow::internal_runtime GTest::gtest GTest::gtest_main)

add_executable(test_model_backend_registry_conflict
  "${PROJECT_SOURCE_DIR}/tests/contract/catalog/test_model_backend_registry_conflict.cpp")
target_link_libraries(test_model_backend_registry_conflict PRIVATE
  llm_edgeflow::internal_runtime GTest::gtest GTest::gtest_main)

add_executable(test_catalog_contract_ssot
  "${PROJECT_SOURCE_DIR}/tests/contract/catalog/test_catalog_contract_ssot.cpp")
target_link_libraries(test_catalog_contract_ssot PRIVATE
  llm_edgeflow::internal_runtime GTest::gtest GTest::gtest_main)

set(_edgeflow_tier1 "tier1;dev-fast;sanitizer-compatible;sanitizer-runtime")
set(_edgeflow_tier2 "tier2;dev-fast;sanitizer-compatible;sanitizer-runtime")
set(_edgeflow_tier3 "tier3;integration;dev-fast;sanitizer-compatible;sanitizer-runtime")
set(_edgeflow_tier4 "tier4;tooling;dev-fast;sanitizer-compatible")

edgeflow_add_runner_test(BatchExecutorTest edgeflow_test_core_runner
  "FixedBatchExecutorTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(FrameworkCoreTest edgeflow_test_core_runner
  "AlgContextTest.*:TraceableItemTest.*:ModelManagerTest.*:PipelineTest.*:SessionContextTest.*"
  "${_edgeflow_tier1}")
edgeflow_add_runner_test(CompanyAlgLogTest edgeflow_test_core_runner
  "CompanyAlgLogTest.*:CompanyAlgLogNameOverrideTest.*"
  "${_edgeflow_tier1}")
edgeflow_add_runner_test(DagPipelineTest edgeflow_test_core_runner
  "DagPipelineTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(EngineFaultToleranceAndLifecycleTest
  edgeflow_test_core_runner "EngineFaultToleranceAndLifecycleTest.*"
  "${_edgeflow_tier1}")
edgeflow_add_runner_test(PipelineConfigTest edgeflow_test_core_runner
  "PipelineConfigTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(RegistryReentrantTest edgeflow_test_core_runner
  "RegistryReentrantTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(CatalogContractSsotTest test_catalog_contract_ssot
  "CatalogContractSsotTest.*" "${_edgeflow_tier1};kite")
edgeflow_add_runner_test(TypedBlackboardContractsTest edgeflow_test_core_runner
  "TypedBlackboardContractsTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(ValidatedPipelinePlanTest edgeflow_test_core_runner
  "ValidatedPipelinePlanTest.*:PortShapeTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(NodeBaseContractsTest edgeflow_test_core_runner
  "NodeBaseContractsTest.*:NodeErrorCodesTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(NodeOwnershipAndReuseTest edgeflow_test_core_runner
  "NodeOwnershipAndReuseTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(DefinitionSchemaValidationTest edgeflow_test_core_runner
  "DefinitionSchemaValidationTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(ModelBackendDecouplingTest edgeflow_test_core_runner
  "ModelBackendDecouplingTest.*:ModelConfigValidationTest.*" "${_edgeflow_tier1};kite")
edgeflow_add_runner_test(ModelBackendPipelineTest edgeflow_test_core_runner
  "ModelBackendPipelineTest.*" "${_edgeflow_tier1};kite")
edgeflow_add_runner_test(OnnxAndEmbeddingModelTest edgeflow_test_core_runner
  "OnnxAndEmbeddingModelTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(OnnxAndRerankerModelTest edgeflow_test_core_runner
  "OnnxAndRerankerModelTest.*" "${_edgeflow_tier1}")

edgeflow_add_runner_test(TextChunkNodeTest edgeflow_test_nodes_runner
  "TextChunkNodeTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(TextEmbeddingNodeTest edgeflow_test_nodes_runner
  "TextEmbeddingNodeTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(VectorTopKNodeTest edgeflow_test_nodes_runner
  "VectorTopKNodeTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(TextRerankNodeTest edgeflow_test_nodes_runner
  "TextRerankNodeTest.*:TextRerankRankingTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(TextTemplateNodeTest edgeflow_test_nodes_runner
  "TextTemplateNodeTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(LlmGenerateNodeTest edgeflow_test_nodes_runner
  "LlmGenerateNodeTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(AsrTranscribeNodeTest edgeflow_test_nodes_runner
  "AsrTranscribeNodeTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(OcrDetectNodeTest edgeflow_test_nodes_runner
  "OcrDetectNodeTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(TextRuleMatchNodeTest edgeflow_test_nodes_runner
  "TextRuleMatchNodeTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(StructuredJsonParseNodeTest edgeflow_test_nodes_runner
  "StructuredJsonParseNodeTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(TextCorpusSourceNodeTest edgeflow_test_nodes_runner
  "TextCorpusSourceNodeTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(CommonNodesTest edgeflow_test_nodes_runner
  "NodeAuthoringExamplesTest.*:PromptGuidedLlmNodeTest.*:CustomNodeCatalogTest.*"
  "${_edgeflow_tier1}")
edgeflow_add_runner_test(FunctionNodeTest edgeflow_test_nodes_runner
  "FunctionNodeTest.*:ConfigurationSnapshotTest.*:TraceableBatchOperationsTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(ParameterBindingTest edgeflow_test_nodes_runner
  "ParameterBindingTest.*" "${_edgeflow_tier1}")

add_test(NAME CppOperatorSdkTest COMMAND test_cpp_operator_sdk)
set_tests_properties(CppOperatorSdkTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "${_edgeflow_tier2}")
edgeflow_add_runner_test(OperatorSafetyTest edgeflow_test_adapter_runner
  "OperatorSafetyTest.*" "${_edgeflow_tier2}")
edgeflow_add_runner_test(DifferentIoModalitiesTest edgeflow_test_adapter_runner
  "DifferentIoModalitiesTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(AllBizPipelinesTest edgeflow_test_adapter_runner
  "AllBizPipelinesTest.*" "${_edgeflow_tier3}")
edgeflow_add_runner_test(ConcurrencyAndEdgeCasesTest edgeflow_test_adapter_runner
  "ConcurrencyAndEdgeCasesTest.*" "${_edgeflow_tier2}")
edgeflow_add_runner_test(RuntimeControlAndHotSwapTest edgeflow_test_adapter_runner
  "RuntimeControlAndHotSwapTest.*" "${_edgeflow_tier2}")
edgeflow_add_runner_test(AdapterContractSecurityTest edgeflow_test_adapter_runner
  "AdapterContractSecurityTest.*" "${_edgeflow_tier2}")
edgeflow_add_runner_test(OperatorApiTest edgeflow_test_adapter_runner
  "OperatorApiTest.*:IoParametersTest.*" "${_edgeflow_tier2}")
edgeflow_add_runner_test(RuntimeFacadeTest edgeflow_test_adapter_runner
  "RuntimeFacadeTest.*:RuntimeFacadeOptionsTest.*" "${_edgeflow_tier2}")
edgeflow_add_runner_test(OperatorOutputPoolTest edgeflow_test_adapter_runner
  "OperatorOutputPoolTest.*" "${_edgeflow_tier2}")
edgeflow_add_runner_test(OperatorValueRegistryTest edgeflow_test_adapter_runner
  "OperatorValueRegistryTest.*" "${_edgeflow_tier2}")
edgeflow_add_runner_test(OperatorGoldenTest edgeflow_test_adapter_runner
  "OperatorGoldenTest.*:ScopedTestOperatorTest.*" "${_edgeflow_tier2}")
edgeflow_add_runner_test(AdapterPurityTest edgeflow_test_adapter_runner
  "AdapterPurityTest.*" "${_edgeflow_tier2}")
edgeflow_add_runner_test(IoConverterTest edgeflow_test_adapter_runner
  "IoConverterTest.*:IoConverterProcessTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(IoConverterRegistryTest edgeflow_test_adapter_runner
  "IoConverterRegistryTest.*:ConverterContractsTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(TextConvertersTest edgeflow_test_adapter_runner
  "TextConvertersTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(ComplexConvertersTest edgeflow_test_adapter_runner
  "ComplexConvertersTest.*" "${_edgeflow_tier1}")

edgeflow_add_runner_test(DocQaRerankTest edgeflow_test_tooling_runner
  "DocQaRerankPipelineTest.*" "${_edgeflow_tier1}")
# Catalog、Validator 与 Authoring 的原生契约使用带工具依赖的 runner。
edgeflow_add_runner_test(PipelineContractsTest edgeflow_test_tooling_runner
  "PipelineCatalogTest.*:PipelineValidatorTest.*:PipelineAuthoringTest.*"
  "${_edgeflow_tier3}")
edgeflow_add_runner_test(DemoRunnerTest edgeflow_test_tooling_runner
  "DemoRunnerTest.*" "${_edgeflow_tier3};kite;kite-real")

add_test(NAME RegistryConflictNodeTest COMMAND test_registry_conflict
  --gtest_filter=RegistryConflictNodeTest.*)
add_test(NAME RegistryConflictModelTest COMMAND test_registry_conflict
  --gtest_filter=RegistryConflictModelTest.*)
set_tests_properties(RegistryConflictNodeTest RegistryConflictModelTest
  PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "${_edgeflow_tier1}" TIMEOUT 5)
foreach(_authoring_case invalid_default duplicate_member factory_exception)
  add_test(NAME RegistryAuthoringStartup_${_authoring_case}
    COMMAND ${CMAKE_COMMAND} -E env
      "EDGEFLOW_BAD_AUTHORING_CASE=${_authoring_case}"
      $<TARGET_FILE:test_registry_conflict>
      --gtest_filter=RegistryAuthoringStartupTest.*)
  set_tests_properties(RegistryAuthoringStartup_${_authoring_case}
    PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    LABELS "${_edgeflow_tier1}" TIMEOUT 5)
endforeach()

edgeflow_add_runner_test(ModelBackendRegistryConflictTest
  test_model_backend_registry_conflict "ModelBackendRegistryConflictTest.*"
  "${_edgeflow_tier1}")

edgeflow_add_runner_test(QwenCausalLmModelTest edgeflow_test_core_runner
  "QwenCausalLmModelTest.*:CommonAutoregressiveGeneratorTest.*"
  "${_edgeflow_tier1}")
edgeflow_add_runner_test(LlamaCppBackendTest edgeflow_test_core_runner
  "LlamaCppBackendTest.*" "${_edgeflow_tier1};kite;kite-real")
edgeflow_add_runner_test(WhisperCppBackendTest edgeflow_test_core_runner
  "WhisperCppBackendTest.*" "${_edgeflow_tier1}")

# 架构与源码治理门禁。
add_test(NAME LayerGuardTest
  COMMAND ${CMAKE_COMMAND} -E env
          "LLM_EDGEFLOW_LAYER_COMPILE_MANIFEST=${PROJECT_BINARY_DIR}/layer_includes/compile_checks_$<CONFIG>.cmake"
          ${PROJECT_SOURCE_DIR}/scripts/check_layer_isolation.sh)
add_test(NAME LayerGuardSelfTest
  COMMAND ${PROJECT_SOURCE_DIR}/scripts/check_layer_isolation.sh --self-test)
add_test(NAME SpecSignatureDiagnosticsTest
  COMMAND ${CMAKE_COMMAND}
          "-DLAYER_COMPILE_MANIFEST=${PROJECT_BINARY_DIR}/layer_includes/compile_checks_$<CONFIG>.cmake"
          "-DFIXTURE_DIR=${PROJECT_SOURCE_DIR}/tests/fixtures/spec_signatures"
          "-DFUNCTION_NODE_HEADER=${PROJECT_SOURCE_DIR}/include/nodes/function_node.h"
          "-DCONCEPTS_DOC=${PROJECT_SOURCE_DIR}/doc/dev_guide/custom_node_concepts.md"
          -P "${PROJECT_SOURCE_DIR}/tests/contract/authoring/test_spec_signature_diagnostics.cmake")
set_tests_properties(SpecSignatureDiagnosticsTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "tier1;static-gate;dev-fast;sanitizer-compatible")
add_test(NAME ArchitectureDocsDriftTest
  COMMAND ${PROJECT_SOURCE_DIR}/scripts/check_architecture_docs.sh)
add_test(NAME ArchitectureDocsDriftGateSelfTest
  COMMAND ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_architecture_docs_drift_gate.sh)
add_test(NAME GovernanceConsistencyTest
  COMMAND ${PROJECT_SOURCE_DIR}/scripts/check_governance.sh)
add_test(NAME DocLinksTest
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_doc_links.py)
add_test(NAME DocLinksSelfTest
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_doc_links.py --self-test)
add_test(NAME DiagramAssetsCheckTest
  COMMAND ${PROJECT_SOURCE_DIR}/scripts/render_architecture_diagrams.sh --check)
add_test(NAME DiagramRenderGateSelfTest
  COMMAND ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_diagram_render_gate.sh)
add_test(NAME ScriptGeneratorDetectionTest
  COMMAND ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_script_generator_detection.sh)
set_tests_properties(
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
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "${_edgeflow_tier4}"
  ENVIRONMENT
    "LLM_EDGEFLOW_PIPELINE_TOOL=$<TARGET_FILE:alg_pipeline_tool_test>;LLM_EDGEFLOW_SELECTION_TOOL=$<TARGET_FILE:alg_pipeline_tool>;LLM_EDGEFLOW_DEMO_BINARY=$<TARGET_FILE:alg_demo>;LLM_EDGEFLOW_ALG_SHOW=$<TARGET_FILE:alg_show>")

add_test(NAME CustomNodeScaffoldTest
  COMMAND ${Python3_EXECUTABLE}
          ${PROJECT_SOURCE_DIR}/tests/tooling/test_scaffold_custom_node.py)
set_tests_properties(CustomNodeScaffoldTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "${_edgeflow_tier4}")

add_test(NAME DevRecipeTest
  COMMAND ${Python3_EXECUTABLE}
          ${PROJECT_SOURCE_DIR}/tests/tooling/test_dev_recipe.py)
set_tests_properties(DevRecipeTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "${_edgeflow_tier4}"
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

set(EDGEFLOW_PIPELINE_CONFIGS
  configs/pipeline_keyword_match_rules.json
  configs/pipeline_entity_extract_default.json
  configs/pipeline_doc_qa_default.json
  configs/pipeline_dialogue_audit_default.json
  configs/pipeline_doc_qa_cpu.json
  configs/pipeline_doc_qa_rerank_default.json
  configs/pipeline_doc_qa_rerank_cpu.json
  configs/pipeline_entity_extract_cpu.json
  demo/fixtures/mock/pipeline_ocr_invoice_qa.json
  demo/fixtures/mock/pipeline_audio_asr_intent.json
  configs/pipeline_cross_rerank_cpu.json)
foreach(config_path IN LISTS EDGEFLOW_PIPELINE_CONFIGS)
  get_filename_component(config_stem "${config_path}" NAME_WE)
  add_test(NAME NativeCli_${config_stem}
    COMMAND $<TARGET_FILE:alg_show>
            ${PROJECT_SOURCE_DIR}/${config_path})
  set_tests_properties(NativeCli_${config_stem}
    PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    LABELS "${_edgeflow_tier4}")
  if(config_path MATCHES "^configs/")
    add_test(NAME PythonCli_${config_stem}
      COMMAND ${PROJECT_SOURCE_DIR}/tools/pipeline_studio/server.py
              ${config_path})
    set_tests_properties(PythonCli_${config_stem}
      PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      LABELS "${_edgeflow_tier4}")
  endif()
endforeach()

add_test(NAME PipelineToolCatalogTest COMMAND $<TARGET_FILE:alg_pipeline_tool>
  catalog)
add_test(NAME PipelineToolValidateTest COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tooling/test_pipeline_cli.py
  --tool $<TARGET_FILE:alg_pipeline_tool>
  --test-tool $<TARGET_FILE:alg_pipeline_tool_test>
  --repo ${PROJECT_SOURCE_DIR})
set_tests_properties(PipelineToolCatalogTest PipelineToolValidateTest
  PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "${_edgeflow_tier4}")
set_tests_properties(PipelineToolValidateTest PROPERTIES
  ENVIRONMENT "LLM_EDGEFLOW_ALG_SHOW=$<TARGET_FILE:alg_show>")

add_custom_target(edgeflow_dev_tests DEPENDS
  alg_demo alg_pipeline_tool alg_pipeline_tool_test alg_show
  test_cpp_operator_sdk
  test_registry_conflict test_model_backend_registry_conflict
  test_catalog_contract_ssot edgeflow_test_core_runner
  edgeflow_test_nodes_runner edgeflow_test_adapter_runner
  edgeflow_test_tooling_runner)

get_property(_edgeflow_registered_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${_edgeflow_registered_tests} PROPERTIES TIMEOUT 120)
set_tests_properties(RegistryConflictNodeTest RegistryConflictModelTest
  RegistryAuthoringStartup_invalid_default
  RegistryAuthoringStartup_duplicate_member
  RegistryAuthoringStartup_factory_exception
  PROPERTIES TIMEOUT 5)
# 需显式开启的真实 Kite 部署测试会加载文本、ONNX 和视觉模型。
set_tests_properties(DemoRunnerTest PROPERTIES TIMEOUT 300)

# 规范 runner 组装必须包含的运行时契约测试。
# 冒烟测试和工具测试可以扩展此集合，但组装时不得静默遗漏其中任何一项。
set(EDGEFLOW_REQUIRED_CONTRACT_TESTS
  QualityGateScriptsContractTest
  BatchExecutorTest
  FrameworkCoreTest
  CompanyAlgLogTest
  DagPipelineTest
  EngineFaultToleranceAndLifecycleTest
  PipelineConfigTest
  RegistryReentrantTest
  CatalogContractSsotTest
  TypedBlackboardContractsTest
  ValidatedPipelinePlanTest
  NodeBaseContractsTest
  NodeOwnershipAndReuseTest
  DefinitionSchemaValidationTest
  ModelBackendDecouplingTest
  ModelBackendPipelineTest
  OnnxAndEmbeddingModelTest
  OnnxAndRerankerModelTest
  TextChunkNodeTest
  TextEmbeddingNodeTest
  VectorTopKNodeTest
  TextRerankNodeTest
  TextTemplateNodeTest
  LlmGenerateNodeTest
  AsrTranscribeNodeTest
  OcrDetectNodeTest
  TextRuleMatchNodeTest
  StructuredJsonParseNodeTest
  TextCorpusSourceNodeTest
  CommonNodesTest
  FunctionNodeTest
  SpecSignatureDiagnosticsTest
  ParameterBindingTest
  CppOperatorSdkTest
  OperatorSafetyTest
  DifferentIoModalitiesTest
  AllBizPipelinesTest
  ConcurrencyAndEdgeCasesTest
  RuntimeControlAndHotSwapTest
  AdapterContractSecurityTest
  OperatorApiTest
  OperatorOutputPoolTest
  RuntimeFacadeTest
  OperatorValueRegistryTest
  OperatorGoldenTest
  AdapterPurityTest
  IoConverterTest
  IoConverterRegistryTest
  TextConvertersTest
  ComplexConvertersTest
  DocQaRerankTest
  PipelineContractsTest
  DemoRunnerTest
  RegistryConflictNodeTest
  RegistryConflictModelTest
  ModelBackendRegistryConflictTest
  QwenCausalLmModelTest
  LlamaCppBackendTest
  WhisperCppBackendTest)

function(edgeflow_assert_required_test_inventory)
  get_property(registered_tests DIRECTORY PROPERTY TESTS)
  foreach(required_test IN LISTS EDGEFLOW_REQUIRED_CONTRACT_TESTS)
    if(NOT required_test IN_LIST registered_tests)
      message(FATAL_ERROR
        "Test mode omitted required contract suite: ${required_test}")
    endif()
  endforeach()
endfunction()
edgeflow_assert_required_test_inventory()
