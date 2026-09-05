# Sharded Google Test runners and label-driven development test matrix.

include(${PROJECT_SOURCE_DIR}/cmake/TestInventory.cmake)

option(LLM_EDGEFLOW_TEST_PCH "Enable precompiled headers for test runners" ON)

add_test(NAME ThirdPartyCacheMetadataTest
  COMMAND ${CMAKE_COMMAND}
          -DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}
          -DTEST_ROOT=${PROJECT_BINARY_DIR}/third_party_cache_metadata_test
          -P ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_third_party_cache_metadata.cmake)
set_tests_properties(ThirdPartyCacheMetadataTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "tier1;static-gate;dev-fast;sanitizer-compatible")

if(NOT LLM_EDGEFLOW_SHARDED_TEST_RUNNERS)
  include(${PROJECT_SOURCE_DIR}/cmake/IndividualTests.cmake)
  # The opt-in real Kite deployment suite loads text, ONNX and vision models.
  set_tests_properties(DemoRunnerTest PROPERTIES TIMEOUT 300)
  edgeflow_assert_required_test_inventory()
  return()
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

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

set(EDGEFLOW_TEST_CORE_SRCS
  ${EDGEFLOW_SOURCE_test_batch_executor}
  ${EDGEFLOW_SOURCE_test_company_alg_log}
  ${EDGEFLOW_SOURCE_test_company_alg_log_name_override}
  ${EDGEFLOW_SOURCE_test_framework_core}
  ${EDGEFLOW_SOURCE_test_dag_pipeline}
  ${EDGEFLOW_SOURCE_test_engine_fault_tolerance_and_lifecycle}
  ${EDGEFLOW_SOURCE_test_pipeline_config}
  ${EDGEFLOW_SOURCE_test_registry_reentrant}
  ${EDGEFLOW_SOURCE_test_typed_blackboard_contracts}
  ${EDGEFLOW_SOURCE_test_validated_pipeline_plan}
  ${EDGEFLOW_SOURCE_test_node_base_contracts}
  ${EDGEFLOW_SOURCE_test_node_ownership_and_reuse}
  ${EDGEFLOW_SOURCE_test_definition_schema_validation}
  ${EDGEFLOW_SOURCE_test_model_backend_decoupling}
  ${EDGEFLOW_SOURCE_test_model_backend_pipeline}
  ${EDGEFLOW_SOURCE_test_onnx_and_embedding_model}
  ${EDGEFLOW_SOURCE_test_onnx_and_reranker_model}
  ${EDGEFLOW_SOURCE_test_qwen_causal_lm_model}
  ${EDGEFLOW_SOURCE_test_llama_cpp_backend})
add_executable(edgeflow_test_core_runner
  ${EDGEFLOW_TEST_CORE_SRCS}
  $<TARGET_OBJECTS:edgeflow_test_backend_fixtures>
  $<TARGET_OBJECTS:edgeflow_test_business_model_fixtures>)
target_link_libraries(edgeflow_test_core_runner PRIVATE
  llm_edgeflow::internal_runtime GTest::gtest GTest::gtest_main)
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
    "${PROJECT_SOURCE_DIR}/tests/fixtures/pipelines/cross_rerank/pipeline_cross_rerank_missing_model.conf"
    "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}/pipeline_cross_rerank_missing_model.conf"
    COPYONLY)
endif()
edgeflow_enable_test_pch(edgeflow_test_core_runner)

set(EDGEFLOW_TEST_NODE_SRCS
  ${EDGEFLOW_SOURCE_test_text_chunk_node}
  ${EDGEFLOW_SOURCE_test_text_embedding_node}
  ${EDGEFLOW_SOURCE_test_vector_top_k_node}
  ${EDGEFLOW_SOURCE_test_text_rerank_node}
  ${EDGEFLOW_SOURCE_test_text_template_node}
  ${EDGEFLOW_SOURCE_test_llm_generate_node}
  ${EDGEFLOW_SOURCE_test_asr_transcribe_node}
  ${EDGEFLOW_SOURCE_test_ocr_detect_node}
  ${EDGEFLOW_SOURCE_test_text_rule_match_node}
  ${EDGEFLOW_SOURCE_test_structured_json_parse_node}
  ${EDGEFLOW_SOURCE_test_text_corpus_source_node}
  ${EDGEFLOW_SOURCE_test_common_nodes})
add_executable(edgeflow_test_nodes_runner
  ${EDGEFLOW_TEST_NODE_SRCS}
  $<TARGET_OBJECTS:edgeflow_test_backend_fixtures>
  $<TARGET_OBJECTS:edgeflow_test_business_model_fixtures>)
target_link_libraries(edgeflow_test_nodes_runner PRIVATE
  llm_edgeflow::internal_runtime GTest::gtest GTest::gtest_main)
edgeflow_enable_test_pch(edgeflow_test_nodes_runner)

set(EDGEFLOW_TEST_ADAPTER_SRCS
  ${EDGEFLOW_SOURCE_test_c_abi_safety}
  ${EDGEFLOW_SOURCE_test_different_io_modalities}
  ${EDGEFLOW_SOURCE_test_all_biz_pipelines}
  ${EDGEFLOW_SOURCE_test_concurrency_and_edge_cases}
  ${EDGEFLOW_SOURCE_test_runtime_control_and_hot_swap}
  ${EDGEFLOW_SOURCE_test_adapter_contract_security}
  ${EDGEFLOW_SOURCE_test_operator_api}
  ${EDGEFLOW_SOURCE_test_operator_output_pool}
  ${EDGEFLOW_SOURCE_test_operator_value_registry}
  ${EDGEFLOW_SOURCE_test_operator_biz_bridge_registry}
  ${EDGEFLOW_SOURCE_test_operator_golden}
  ${EDGEFLOW_SOURCE_test_adapter_purity})
add_executable(edgeflow_test_adapter_runner
  ${EDGEFLOW_TEST_ADAPTER_SRCS}
  $<TARGET_OBJECTS:edgeflow_test_backend_fixtures>
  $<TARGET_OBJECTS:edgeflow_test_business_model_fixtures>)
target_link_libraries(edgeflow_test_adapter_runner PRIVATE
  llm_edgeflow::internal_runtime GTest::gtest GTest::gtest_main)
edgeflow_enable_test_pch(edgeflow_test_adapter_runner)

set(EDGEFLOW_TEST_TOOLING_SRCS
  ${EDGEFLOW_SOURCE_test_doc_qa_rerank}
  ${EDGEFLOW_SOURCE_test_rerank_refine_node}
  ${EDGEFLOW_SOURCE_test_pipeline_catalog_validator}
  ${EDGEFLOW_SOURCE_test_demo_runner})
add_executable(edgeflow_test_tooling_runner
  ${EDGEFLOW_TEST_TOOLING_SRCS}
  $<TARGET_OBJECTS:edgeflow_demo_objects>
  $<TARGET_OBJECTS:edgeflow_test_backend_fixtures>
  $<TARGET_OBJECTS:edgeflow_test_business_model_fixtures>)
target_link_libraries(edgeflow_test_tooling_runner PRIVATE
  llm_edgeflow::internal_runtime GTest::gtest GTest::gtest_main)
edgeflow_enable_test_pch(edgeflow_test_tooling_runner)

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

# Process-isolated targets. Registry conflict intentionally runs each dirty
# singleton scenario in its own process.
add_executable(test_c11_abi_compliance ${EDGEFLOW_SOURCE_test_c11_abi_compliance})
target_link_libraries(test_c11_abi_compliance PRIVATE llm_edgeflow::sdk)

add_executable(test_registry_conflict ${EDGEFLOW_SOURCE_test_registry_conflict})
target_link_libraries(test_registry_conflict PRIVATE
  llm_edgeflow::internal_runtime GTest::gtest GTest::gtest_main)

add_executable(test_model_backend_registry_conflict
  ${EDGEFLOW_SOURCE_test_model_backend_registry_conflict})
target_link_libraries(test_model_backend_registry_conflict PRIVATE
  llm_edgeflow::internal_runtime GTest::gtest GTest::gtest_main)

add_executable(test_catalog_contract_ssot
  ${EDGEFLOW_SOURCE_test_catalog_contract_ssot})
target_link_libraries(test_catalog_contract_ssot PRIVATE
  llm_edgeflow::internal_runtime GTest::gtest GTest::gtest_main)

set(_edgeflow_tier1 "tier1;dev-fast;sanitizer-compatible")
set(_edgeflow_tier2 "tier2;dev-fast;sanitizer-compatible")
set(_edgeflow_tier3 "tier3;integration;dev-fast;sanitizer-compatible")
set(_edgeflow_tier4 "tier4;tooling;dev-fast;sanitizer-compatible")

edgeflow_add_runner_test(BatchExecutorTest edgeflow_test_core_runner
  "FixedBatchExecutorTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(FrameworkCoreTest edgeflow_test_core_runner
  "AlgContextTest.*:TraceableItemTest.*:NodeRegistryTest.*:ModelManagerTest.*:PipelineTest.*"
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
  "CatalogContractSsotTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(TypedBlackboardContractsTest edgeflow_test_core_runner
  "TypedBlackboardContractsTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(ValidatedPipelinePlanTest edgeflow_test_core_runner
  "ValidatedPipelinePlanTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(NodeBaseContractsTest edgeflow_test_core_runner
  "NodeBaseContractsTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(NodeOwnershipAndReuseTest edgeflow_test_core_runner
  "NodeOwnershipAndReuseTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(DefinitionSchemaValidationTest edgeflow_test_core_runner
  "DefinitionSchemaValidationTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(ModelBackendDecouplingTest edgeflow_test_core_runner
  "ModelBackendDecouplingTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(ModelBackendPipelineTest edgeflow_test_core_runner
  "ModelBackendPipelineTest.*" "${_edgeflow_tier1}")
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
  "TextRerankNodeTest.*" "${_edgeflow_tier1}")
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
  "CommonNodesTest.*" "${_edgeflow_tier1}")

add_test(NAME C11AbiComplianceTest COMMAND test_c11_abi_compliance)
set_tests_properties(C11AbiComplianceTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "${_edgeflow_tier2}")
edgeflow_add_runner_test(CAbiSafetyTest edgeflow_test_adapter_runner
  "CAbiSafetyTest.*" "${_edgeflow_tier2}")
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
  "OperatorApiTest.*" "${_edgeflow_tier2}")
edgeflow_add_runner_test(OperatorOutputPoolTest edgeflow_test_adapter_runner
  "OperatorOutputPoolTest.*" "${_edgeflow_tier2}")
edgeflow_add_runner_test(OperatorValueRegistryTest edgeflow_test_adapter_runner
  "OperatorValueRegistryTest.*" "${_edgeflow_tier2}")
edgeflow_add_runner_test(OperatorBizBridgeRegistryTest
  edgeflow_test_adapter_runner "OperatorBizBridgeRegistryTest.*"
  "${_edgeflow_tier2}")
edgeflow_add_runner_test(OperatorGoldenTest edgeflow_test_adapter_runner
  "OperatorGoldenTest.*" "${_edgeflow_tier2}")
edgeflow_add_runner_test(AdapterPurityTest edgeflow_test_adapter_runner
  "AdapterPurityTest.*" "${_edgeflow_tier2}")

edgeflow_add_runner_test(DocQaRerankTest edgeflow_test_tooling_runner
  "DocQaRerankPipelineTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(RerankRefineNodeTest edgeflow_test_tooling_runner
  "TextRerankNodeTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(PipelineStudioTest edgeflow_test_tooling_runner
  "BlackboardKeyTest.*:PipelineCatalogTest.*:PipelineValidatorTest.*"
  "${_edgeflow_tier4}")
edgeflow_add_runner_test(DemoRunnerTest edgeflow_test_tooling_runner
  "DemoRunnerTest.*" "${_edgeflow_tier3}")

add_test(NAME RegistryConflictNodeTest COMMAND test_registry_conflict
  --gtest_filter=RegistryConflictNodeTest.*)
add_test(NAME RegistryConflictModelTest COMMAND test_registry_conflict
  --gtest_filter=RegistryConflictModelTest.*)
set_tests_properties(RegistryConflictNodeTest RegistryConflictModelTest
  PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "${_edgeflow_tier1}" TIMEOUT 5)

edgeflow_add_runner_test(ModelBackendRegistryConflictTest
  test_model_backend_registry_conflict "ModelBackendRegistryConflictTest.*"
  "${_edgeflow_tier1}")

edgeflow_add_runner_test(QwenCausalLmModelTest edgeflow_test_core_runner
  "QwenCausalLmModelTest.*:CommonAutoregressiveGeneratorTest.*"
  "${_edgeflow_tier1}")
edgeflow_add_runner_test(LlamaCppBackendTest edgeflow_test_core_runner
  "LlamaCppBackendTest.*" "${_edgeflow_tier1}")

# Architecture and source-governance gates.
add_test(NAME LayerGuardTest
  COMMAND ${PROJECT_SOURCE_DIR}/scripts/check_layer_isolation.sh)
add_test(NAME LayerGuardSelfTest
  COMMAND ${PROJECT_SOURCE_DIR}/scripts/check_layer_isolation.sh --self-test)
add_test(NAME ArchitectureDocsDriftTest
  COMMAND ${PROJECT_SOURCE_DIR}/scripts/check_architecture_docs.sh)
add_test(NAME ArchitectureDocsDriftGateSelfTest
  COMMAND ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_architecture_docs_drift_gate.sh)
add_test(NAME GovernanceConsistencyTest
  COMMAND ${PROJECT_SOURCE_DIR}/scripts/check_governance.sh)
add_test(NAME DiagramAssetsCheckTest
  COMMAND ${PROJECT_SOURCE_DIR}/scripts/render_architecture_diagrams.sh --check)
add_test(NAME DiagramRenderGateSelfTest
  COMMAND ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_diagram_render_gate.sh)
add_test(NAME ScriptGeneratorDetectionTest
  COMMAND ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_script_generator_detection.sh)
add_test(NAME SanitizerCcacheContractTest
  COMMAND ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_sanitizer_ccache_contract.sh)
set_tests_properties(
  LayerGuardTest LayerGuardSelfTest ArchitectureDocsDriftTest
  ArchitectureDocsDriftGateSelfTest GovernanceConsistencyTest
  ScriptGeneratorDetectionTest SanitizerCcacheContractTest
  PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "tier1;static-gate;dev-fast;sanitizer-compatible")
set_tests_properties(DiagramAssetsCheckTest DiagramRenderGateSelfTest
  PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "tier4;tooling;static-gate;slow")

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME PipelineStudioServerTest
  COMMAND ${Python3_EXECUTABLE}
          ${PROJECT_SOURCE_DIR}/tests/tooling/test_pipeline_studio.py)
set_tests_properties(PipelineStudioServerTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "${_edgeflow_tier4}"
  ENVIRONMENT
    "LLM_EDGEFLOW_PIPELINE_TOOL=$<TARGET_FILE:alg_pipeline_tool_test>;LLM_EDGEFLOW_DEMO_BINARY=$<TARGET_FILE:alg_demo>;LLM_EDGEFLOW_ALG_SHOW=$<TARGET_FILE:alg_show>")

add_test(NAME DemoSmokeTest COMMAND $<TARGET_FILE:alg_demo> --suite smoke)
set_tests_properties(DemoSmokeTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "${_edgeflow_tier3}")

if(LLM_EDGEFLOW_HAS_ONNXRUNTIME)
  add_test(
    NAME CrossRerankDemoFixtureTest
    COMMAND $<TARGET_FILE:alg_demo>
            --biz cross_rerank
            --config
            "${EDGEFLOW_GENERATED_MODEL_FIXTURE_DIR}/pipeline_cross_rerank_fixture.conf"
            --dataset
            "${PROJECT_SOURCE_DIR}/data/corpus_cross_rerank.txt"
            --output-dir
            "${CMAKE_CURRENT_BINARY_DIR}/test-results/cross-rerank-demo")
  add_test(
    NAME CrossRerankDemoMissingModelFailsClosedTest
    COMMAND $<TARGET_FILE:alg_demo>
            --biz cross_rerank
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
  configs/pipeline_keyword_match.json
  configs/pipeline_entity_extract.json
  configs/pipeline_doc_qa.json
  configs/pipeline_dialogue_audit.json
  configs/pipeline_doc_qa_onnx.json
  configs/pipeline_doc_qa_rerank.json
  configs/pipeline_doc_qa_rerank_real.json
  configs/pipeline_entity_extract_llamacpp.json
  demo/fixtures/mock/pipeline_ocr_doc_qa.json
  demo/fixtures/mock/pipeline_audio_asr_intent.json
  configs/pipeline_cross_rerank.json)
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
      COMMAND ${PROJECT_SOURCE_DIR}/show
              ${config_path})
    set_tests_properties(PythonCli_${config_stem}
      PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      LABELS "${_edgeflow_tier4}")
  endif()
endforeach()

add_test(NAME PipelineToolCatalogTest COMMAND $<TARGET_FILE:alg_pipeline_tool>
  catalog --biz keyword_match_v1)
add_test(NAME PipelineToolValidateTest COMMAND $<TARGET_FILE:alg_pipeline_tool>
  validate ${PROJECT_SOURCE_DIR}/configs/pipeline_keyword_match.json)
set_tests_properties(PipelineToolCatalogTest PipelineToolValidateTest
  PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "${_edgeflow_tier4}")

add_custom_target(edgeflow_dev_tests DEPENDS
  alg_demo alg_pipeline_tool alg_pipeline_tool_test alg_show
  test_c11_abi_compliance
  test_registry_conflict test_model_backend_registry_conflict
  test_catalog_contract_ssot edgeflow_test_core_runner
  edgeflow_test_nodes_runner edgeflow_test_adapter_runner
  edgeflow_test_tooling_runner)

get_property(_edgeflow_registered_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${_edgeflow_registered_tests} PROPERTIES TIMEOUT 120)
set_tests_properties(RegistryConflictNodeTest RegistryConflictModelTest
  PROPERTIES TIMEOUT 5)
# The opt-in real Kite deployment suite loads text, ONNX and vision models.
set_tests_properties(DemoRunnerTest PROPERTIES TIMEOUT 300)
edgeflow_assert_required_test_inventory()
