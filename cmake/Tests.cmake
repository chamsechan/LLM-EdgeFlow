# Sharded Google Test runners and label-driven development test matrix.

find_package(Python3 COMPONENTS Interpreter REQUIRED)

function(edgeflow_enable_test_pch target_name)
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
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    LABELS "${labels}")
endfunction()

set(EDGEFLOW_TEST_CORE_SRCS
  tests/test_batch_executor.cpp
  tests/test_company_alg_log.cpp
  tests/test_company_alg_log_name_override.cpp
  tests/test_framework_core.cpp
  tests/test_dag_pipeline.cpp
  tests/test_engine_fault_tolerance_and_lifecycle.cpp
  tests/test_pipeline_config.cpp
  tests/test_registry_reentrant.cpp
  tests/test_typed_blackboard_contracts.cpp
  tests/test_validated_pipeline_plan.cpp
  tests/test_node_base_contracts.cpp
  tests/test_node_ownership_and_reuse.cpp
  tests/test_definition_schema_validation.cpp
  tests/test_model_backend_decoupling.cpp
  tests/test_model_backend_pipeline.cpp
  tests/test_onnx_and_embedding_model.cpp
  tests/test_onnx_and_reranker_model.cpp
  tests/support/inference/test_tensor_backend.cpp
  tests/support/inference/test_causal_lm_backend.cpp)
add_executable(edgeflow_test_core_runner ${EDGEFLOW_TEST_CORE_SRCS})
target_link_libraries(edgeflow_test_core_runner PRIVATE
  alg_sdk GTest::gtest GTest::gtest_main)
if(LLM_EDGEFLOW_HAS_ONNXRUNTIME)
  set(EDGEFLOW_STAGE3_FIXTURE_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures/stage3")
  set(EDGEFLOW_STAGE3_ONNX_FIXTURE
      "${EDGEFLOW_STAGE3_FIXTURE_DIR}/embedding_fixture.onnx")
  set(EDGEFLOW_STAGE3_VOCAB_FIXTURE
      "${EDGEFLOW_STAGE3_FIXTURE_DIR}/vocab.txt")
  set(EDGEFLOW_STAGE4_RERANK_ONNX_FIXTURE
      "${EDGEFLOW_STAGE3_FIXTURE_DIR}/rerank_fixture.onnx")
  file(MAKE_DIRECTORY "${EDGEFLOW_STAGE3_FIXTURE_DIR}")
  add_custom_command(
    OUTPUT
      "${EDGEFLOW_STAGE3_ONNX_FIXTURE}"
      "${EDGEFLOW_STAGE4_RERANK_ONNX_FIXTURE}"
      "${EDGEFLOW_STAGE3_VOCAB_FIXTURE}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_test_onnx_model.py"
            --output-dir "${EDGEFLOW_STAGE3_FIXTURE_DIR}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_test_onnx_model.py"
    COMMENT "Generating deterministic stage-3 and stage-4 ONNX Runtime fixtures"
    VERBATIM)
  add_custom_target(edgeflow_stage3_onnx_fixture
    DEPENDS
      "${EDGEFLOW_STAGE3_ONNX_FIXTURE}"
      "${EDGEFLOW_STAGE4_RERANK_ONNX_FIXTURE}"
      "${EDGEFLOW_STAGE3_VOCAB_FIXTURE}")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/stage4/pipeline_cross_rerank_fixture.json"
    "${EDGEFLOW_STAGE3_FIXTURE_DIR}/pipeline_cross_rerank_fixture.json"
    COPYONLY)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/stage4/pipeline_cross_rerank_fixture.conf"
    "${EDGEFLOW_STAGE3_FIXTURE_DIR}/pipeline_cross_rerank_fixture.conf"
    COPYONLY)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/stage4/pipeline_cross_rerank_missing_model.conf"
    "${EDGEFLOW_STAGE3_FIXTURE_DIR}/pipeline_cross_rerank_missing_model.conf"
    COPYONLY)
endif()
edgeflow_enable_test_pch(edgeflow_test_core_runner)

set(EDGEFLOW_TEST_NODE_SRCS
  tests/test_text_chunk_node.cpp
  tests/test_text_embedding_node.cpp
  tests/test_vector_top_k_node.cpp
  tests/test_text_rerank_node.cpp
  tests/test_text_template_node.cpp
  tests/test_llm_generate_node.cpp
  tests/test_asr_transcribe_node.cpp
  tests/test_ocr_detect_node.cpp
  tests/test_text_rule_match_node.cpp
  tests/test_structured_json_parse_node.cpp
  tests/test_text_corpus_source_node.cpp
  tests/test_common_nodes.cpp)
add_executable(edgeflow_test_nodes_runner ${EDGEFLOW_TEST_NODE_SRCS})
target_link_libraries(edgeflow_test_nodes_runner PRIVATE
  alg_sdk GTest::gtest GTest::gtest_main)
edgeflow_enable_test_pch(edgeflow_test_nodes_runner)

set(EDGEFLOW_TEST_ADAPTER_SRCS
  tests/test_c_abi_safety.cpp
  tests/test_different_io_modalities.cpp
  tests/test_all_biz_pipelines.cpp
  tests/test_concurrency_and_edge_cases.cpp
  tests/test_runtime_control_and_hot_swap.cpp
  tests/test_adapter_contract_security.cpp
  tests/test_operator_api.cpp
  tests/test_operator_output_pool.cpp
  tests/test_operator_value_registry.cpp
  tests/test_operator_biz_bridge_registry.cpp
  tests/test_operator_golden.cpp
  tests/test_adapter_purity.cpp)
add_executable(edgeflow_test_adapter_runner ${EDGEFLOW_TEST_ADAPTER_SRCS})
target_link_libraries(edgeflow_test_adapter_runner PRIVATE
  alg_sdk GTest::gtest GTest::gtest_main)
edgeflow_enable_test_pch(edgeflow_test_adapter_runner)

set(EDGEFLOW_TEST_TOOLING_SRCS
  tests/test_doc_qa_rerank.cpp
  tests/test_rerank_refine_node.cpp
  tests/test_pipeline_studio.cpp
  tests/test_demo_runner.cpp
  demo/common/demo_options.cpp
  demo/common/demo_registry.cpp
  demo/common/dataset_reader.cpp
  demo/common/result_writer.cpp
  demo/biz/entity_extract_demo.cpp
  demo/biz/keyword_match_demo.cpp
  demo/biz/doc_qa_demo.cpp
  demo/biz/dialogue_audit_demo.cpp
  demo/biz/ocr_doc_qa_demo.cpp
  demo/biz/audio_asr_demo.cpp
  demo/biz/cross_rerank_demo.cpp)
add_executable(edgeflow_test_tooling_runner ${EDGEFLOW_TEST_TOOLING_SRCS})
target_link_libraries(edgeflow_test_tooling_runner PRIVATE
  alg_sdk GTest::gtest GTest::gtest_main)
edgeflow_enable_test_pch(edgeflow_test_tooling_runner)

if(LLM_EDGEFLOW_HAS_ONNXRUNTIME)
  foreach(target
      edgeflow_test_core_runner
      edgeflow_test_nodes_runner
      edgeflow_test_adapter_runner
      edgeflow_test_tooling_runner)
    add_dependencies(${target} edgeflow_stage3_onnx_fixture)
    target_compile_definitions(${target} PRIVATE
      HAVE_ONNXRUNTIME=1
      EDGEFLOW_STAGE3_ONNX_FIXTURE="${EDGEFLOW_STAGE3_ONNX_FIXTURE}"
      EDGEFLOW_STAGE4_RERANK_ONNX_FIXTURE="${EDGEFLOW_STAGE4_RERANK_ONNX_FIXTURE}"
      EDGEFLOW_STAGE3_VOCAB_FIXTURE="${EDGEFLOW_STAGE3_VOCAB_FIXTURE}")
  endforeach()
  add_dependencies(alg_demo edgeflow_stage3_onnx_fixture)
endif()

# Process-isolated targets. Registry conflict intentionally runs each dirty
# singleton scenario in its own process.
add_executable(test_c11_abi_compliance tests/test_c11_abi_compliance.c)
target_link_libraries(test_c11_abi_compliance PRIVATE alg_sdk)

add_executable(test_registry_conflict tests/test_registry_conflict.cpp)
target_link_libraries(test_registry_conflict PRIVATE
  alg_sdk GTest::gtest GTest::gtest_main)

add_executable(test_model_backend_registry_conflict
  tests/test_model_backend_registry_conflict.cpp)
target_link_libraries(test_model_backend_registry_conflict PRIVATE
  alg_sdk GTest::gtest GTest::gtest_main)

add_executable(test_qwen_engines_comparison
  tests/test_qwen_engines_comparison.cpp)
target_link_libraries(test_qwen_engines_comparison PRIVATE
  alg_sdk GTest::gtest GTest::gtest_main)

add_executable(test_catalog_contract_ssot
  tests/test_catalog_contract_ssot.cpp)
target_link_libraries(test_catalog_contract_ssot PRIVATE
  alg_sdk GTest::gtest GTest::gtest_main)

set(_edgeflow_tier1 "tier1;dev-fast;sanitizer-compatible")
set(_edgeflow_tier2 "tier2;dev-fast;sanitizer-compatible")
set(_edgeflow_tier3 "tier3;integration;dev-fast;sanitizer-compatible")
set(_edgeflow_tier4 "tier4;tooling;dev-fast;sanitizer-compatible")

edgeflow_add_runner_test(BatchExecutorTest edgeflow_test_core_runner
  "FixedBatchExecutorTest.*" "${_edgeflow_tier1}")
edgeflow_add_runner_test(FrameworkCoreTest edgeflow_test_core_runner
  "AlgContextTest.*:TraceableItemTest.*:NodeRegistryTest.*:EngineRegistryTest.*:PipelineTest.*"
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
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" LABELS "${_edgeflow_tier2}")
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
add_test(NAME RegistryConflictEngineTest COMMAND test_registry_conflict
  --gtest_filter=RegistryConflictEngineTest.*)
set_tests_properties(RegistryConflictNodeTest RegistryConflictEngineTest
  PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  LABELS "${_edgeflow_tier1}" TIMEOUT 5)

edgeflow_add_runner_test(ModelBackendRegistryConflictTest
  test_model_backend_registry_conflict "ModelBackendRegistryConflictTest.*"
  "${_edgeflow_tier1}")

add_test(NAME QwenEnginesComparisonTest COMMAND test_qwen_engines_comparison)
set_tests_properties(QwenEnginesComparisonTest PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  LABELS "tier3;integration;slow;external-engine")

# Architecture and source-governance gates.
add_test(NAME LayerGuardTest
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_layer_isolation.sh)
add_test(NAME LayerGuardSelfTest
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_layer_isolation.sh --self-test)
add_test(NAME ArchitectureDocsDriftTest
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_architecture_docs.sh)
add_test(NAME ArchitectureDocsDriftGateSelfTest
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_architecture_docs_drift_gate.sh)
add_test(NAME DiagramAssetsCheckTest
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/render_architecture_diagrams.sh --check)
add_test(NAME DiagramRenderGateSelfTest
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_diagram_render_gate.sh)
add_test(NAME ScriptGeneratorDetectionTest
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_script_generator_detection.sh)
set_tests_properties(
  LayerGuardTest LayerGuardSelfTest ArchitectureDocsDriftTest
  ArchitectureDocsDriftGateSelfTest ScriptGeneratorDetectionTest
  PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  LABELS "tier1;static-gate;dev-fast;sanitizer-compatible")
set_tests_properties(DiagramAssetsCheckTest DiagramRenderGateSelfTest
  PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  LABELS "tier4;tooling;static-gate;slow")

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME VisualizerServerTest
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_visualizer_server.py)
set_tests_properties(VisualizerServerTest PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  LABELS "${_edgeflow_tier4}"
  ENVIRONMENT
    "LLM_EDGEFLOW_PIPELINE_TOOL=$<TARGET_FILE:alg_pipeline_tool>;LLM_EDGEFLOW_DEMO_BINARY=$<TARGET_FILE:alg_demo>")

add_test(NAME DemoSmokeTest COMMAND $<TARGET_FILE:alg_demo> --suite smoke)
set_tests_properties(DemoSmokeTest PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  LABELS "${_edgeflow_tier3}")

if(LLM_EDGEFLOW_HAS_ONNXRUNTIME)
  add_test(
    NAME CrossRerankDemoFixtureTest
    COMMAND $<TARGET_FILE:alg_demo>
            --business cross_rerank
            --config
            "${EDGEFLOW_STAGE3_FIXTURE_DIR}/pipeline_cross_rerank_fixture.conf"
            --dataset
            "${CMAKE_CURRENT_SOURCE_DIR}/data/corpus_cross_rerank.txt"
            --output-dir
            "${CMAKE_CURRENT_BINARY_DIR}/test-results/stage4-rerank-demo")
  add_test(
    NAME CrossRerankDemoMissingModelFailsClosedTest
    COMMAND $<TARGET_FILE:alg_demo>
            --business cross_rerank
            --config
            "${EDGEFLOW_STAGE3_FIXTURE_DIR}/pipeline_cross_rerank_missing_model.conf"
            --dataset
            "${CMAKE_CURRENT_SOURCE_DIR}/data/corpus_cross_rerank.txt"
            --allow-fallback-sample
            --output-dir
            "${CMAKE_CURRENT_BINARY_DIR}/test-results/stage4-rerank-missing")
  set_tests_properties(
    CrossRerankDemoFixtureTest
    CrossRerankDemoMissingModelFailsClosedTest
    PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      LABELS "${_edgeflow_tier3}")
  set_tests_properties(
    CrossRerankDemoMissingModelFailsClosedTest
    PROPERTIES WILL_FAIL TRUE)
endif()

set(EDGEFLOW_PIPELINE_CONFIGS
  pipeline_keyword_match.json
  pipeline_entity_extract.json
  pipeline_doc_qa.json
  pipeline_dialogue_audit.json
  pipeline_doc_qa_onnx.json
  pipeline_doc_qa_rerank.json
  pipeline_doc_qa_rerank_real.json
  pipeline_entity_extract_llamacpp.json
  pipeline_ocr_doc_qa.json
  pipeline_audio_asr_intent.json
  pipeline_cross_rerank.json)
foreach(config_name IN LISTS EDGEFLOW_PIPELINE_CONFIGS)
  string(REPLACE ".json" "" config_stem "${config_name}")
  add_test(NAME PythonCli_${config_stem}
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/show
            configs/${config_name})
  add_test(NAME NativeCli_${config_stem}
    COMMAND $<TARGET_FILE:alg_show>
            ${CMAKE_CURRENT_SOURCE_DIR}/configs/${config_name})
  set_tests_properties(PythonCli_${config_stem} NativeCli_${config_stem}
    PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    LABELS "${_edgeflow_tier4}")
endforeach()

add_test(NAME PipelineToolCatalogTest COMMAND $<TARGET_FILE:alg_pipeline_tool>
  catalog --business keyword_match_v1)
add_test(NAME PipelineToolValidateTest COMMAND $<TARGET_FILE:alg_pipeline_tool>
  validate ${CMAKE_CURRENT_SOURCE_DIR}/configs/pipeline_keyword_match.json)
set_tests_properties(PipelineToolCatalogTest PipelineToolValidateTest
  PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  LABELS "${_edgeflow_tier4}")

add_custom_target(edgeflow_dev_tests DEPENDS
  alg_demo alg_pipeline_tool alg_show test_c11_abi_compliance
  test_registry_conflict test_model_backend_registry_conflict
  test_catalog_contract_ssot edgeflow_test_core_runner
  edgeflow_test_nodes_runner edgeflow_test_adapter_runner
  edgeflow_test_tooling_runner)
