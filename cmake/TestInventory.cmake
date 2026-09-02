# Required runtime contract suites shared by sharded and individual modes.
# Mode-specific smoke/tooling tests may extend this set, but neither mode may
# silently omit one of these core contracts.
set(EDGEFLOW_REQUIRED_CONTRACT_TESTS
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
  C11AbiComplianceTest
  CAbiSafetyTest
  DifferentIoModalitiesTest
  AllBizPipelinesTest
  ConcurrencyAndEdgeCasesTest
  RuntimeControlAndHotSwapTest
  AdapterContractSecurityTest
  OperatorApiTest
  OperatorOutputPoolTest
  OperatorValueRegistryTest
  OperatorBizBridgeRegistryTest
  OperatorGoldenTest
  AdapterPurityTest
  DocQaRerankTest
  RerankRefineNodeTest
  PipelineStudioTest
  DemoRunnerTest
  RegistryConflictNodeTest
  RegistryConflictModelTest
  ModelBackendRegistryConflictTest
  QwenCausalLmModelTest
  LlamaCppBackendTest)

# Source ownership is shared by sharded and individual runners. Keep each test
# translation unit declared once here; mode-specific files only decide how to
# group processes, filters and labels.
set(EDGEFLOW_SOURCE_test_adapter_contract_security "${PROJECT_SOURCE_DIR}/tests/contract/abi/test_adapter_contract_security.cpp")
set(EDGEFLOW_SOURCE_test_c11_abi_compliance "${PROJECT_SOURCE_DIR}/tests/contract/abi/test_c11_abi_compliance.c")
set(EDGEFLOW_SOURCE_test_c_abi_safety "${PROJECT_SOURCE_DIR}/tests/contract/abi/test_c_abi_safety.cpp")
set(EDGEFLOW_SOURCE_test_catalog_contract_ssot "${PROJECT_SOURCE_DIR}/tests/contract/catalog/test_catalog_contract_ssot.cpp")
set(EDGEFLOW_SOURCE_test_model_backend_registry_conflict "${PROJECT_SOURCE_DIR}/tests/contract/catalog/test_model_backend_registry_conflict.cpp")
set(EDGEFLOW_SOURCE_test_registry_conflict "${PROJECT_SOURCE_DIR}/tests/contract/catalog/test_registry_conflict.cpp")
set(EDGEFLOW_SOURCE_test_real_models_e2e "${PROJECT_SOURCE_DIR}/tests/e2e/real_models/test_real_models_e2e.cpp")
set(EDGEFLOW_SOURCE_test_demo_runner "${PROJECT_SOURCE_DIR}/tests/integration/demo/test_demo_runner.cpp")
set(EDGEFLOW_SOURCE_test_operator_api "${PROJECT_SOURCE_DIR}/tests/integration/operator/test_operator_api.cpp")
set(EDGEFLOW_SOURCE_test_operator_golden "${PROJECT_SOURCE_DIR}/tests/integration/operator/test_operator_golden.cpp")
set(EDGEFLOW_SOURCE_test_doc_qa_rerank "${PROJECT_SOURCE_DIR}/tests/integration/pipeline/test_doc_qa_rerank.cpp")
set(EDGEFLOW_SOURCE_test_pipeline_catalog_validator "${PROJECT_SOURCE_DIR}/tests/integration/pipeline/test_pipeline_catalog_validator.cpp")
set(EDGEFLOW_SOURCE_test_all_biz_pipelines "${PROJECT_SOURCE_DIR}/tests/integration/runtime/test_all_biz_pipelines.cpp")
set(EDGEFLOW_SOURCE_test_concurrency_and_edge_cases "${PROJECT_SOURCE_DIR}/tests/integration/runtime/test_concurrency_and_edge_cases.cpp")
set(EDGEFLOW_SOURCE_test_different_io_modalities "${PROJECT_SOURCE_DIR}/tests/integration/runtime/test_different_io_modalities.cpp")
set(EDGEFLOW_SOURCE_test_runtime_control_and_hot_swap "${PROJECT_SOURCE_DIR}/tests/integration/runtime/test_runtime_control_and_hot_swap.cpp")
set(EDGEFLOW_SOURCE_test_adapter_purity "${PROJECT_SOURCE_DIR}/tests/unit/adapter/test_adapter_purity.cpp")
set(EDGEFLOW_SOURCE_test_dag_pipeline "${PROJECT_SOURCE_DIR}/tests/unit/core/test_dag_pipeline.cpp")
set(EDGEFLOW_SOURCE_test_definition_schema_validation "${PROJECT_SOURCE_DIR}/tests/unit/core/test_definition_schema_validation.cpp")
set(EDGEFLOW_SOURCE_test_framework_core "${PROJECT_SOURCE_DIR}/tests/unit/core/test_framework_core.cpp")
set(EDGEFLOW_SOURCE_test_node_base_contracts "${PROJECT_SOURCE_DIR}/tests/unit/core/test_node_base_contracts.cpp")
set(EDGEFLOW_SOURCE_test_node_ownership_and_reuse "${PROJECT_SOURCE_DIR}/tests/unit/core/test_node_ownership_and_reuse.cpp")
set(EDGEFLOW_SOURCE_test_pipeline_config "${PROJECT_SOURCE_DIR}/tests/unit/core/test_pipeline_config.cpp")
set(EDGEFLOW_SOURCE_test_registry_reentrant "${PROJECT_SOURCE_DIR}/tests/unit/core/test_registry_reentrant.cpp")
set(EDGEFLOW_SOURCE_test_typed_blackboard_contracts "${PROJECT_SOURCE_DIR}/tests/unit/core/test_typed_blackboard_contracts.cpp")
set(EDGEFLOW_SOURCE_test_validated_pipeline_plan "${PROJECT_SOURCE_DIR}/tests/unit/core/test_validated_pipeline_plan.cpp")
set(EDGEFLOW_SOURCE_test_batch_executor "${PROJECT_SOURCE_DIR}/tests/unit/engine/test_batch_executor.cpp")
set(EDGEFLOW_SOURCE_test_engine_fault_tolerance_and_lifecycle "${PROJECT_SOURCE_DIR}/tests/unit/engine/test_engine_fault_tolerance_and_lifecycle.cpp")
set(EDGEFLOW_SOURCE_test_llama_cpp_backend "${PROJECT_SOURCE_DIR}/tests/unit/engine/test_llama_cpp_backend.cpp")
set(EDGEFLOW_SOURCE_test_model_backend_decoupling "${PROJECT_SOURCE_DIR}/tests/unit/engine/test_model_backend_decoupling.cpp")
set(EDGEFLOW_SOURCE_test_model_backend_pipeline "${PROJECT_SOURCE_DIR}/tests/unit/engine/test_model_backend_pipeline.cpp")
set(EDGEFLOW_SOURCE_test_onnx_and_embedding_model "${PROJECT_SOURCE_DIR}/tests/unit/engine/test_onnx_and_embedding_model.cpp")
set(EDGEFLOW_SOURCE_test_onnx_and_reranker_model "${PROJECT_SOURCE_DIR}/tests/unit/engine/test_onnx_and_reranker_model.cpp")
set(EDGEFLOW_SOURCE_test_qwen_causal_lm_model "${PROJECT_SOURCE_DIR}/tests/unit/engine/test_qwen_causal_lm_model.cpp")
set(EDGEFLOW_SOURCE_test_company_alg_log "${PROJECT_SOURCE_DIR}/tests/unit/logging/test_company_alg_log.cpp")
set(EDGEFLOW_SOURCE_test_company_alg_log_name_override "${PROJECT_SOURCE_DIR}/tests/unit/logging/test_company_alg_log_name_override.cpp")
set(EDGEFLOW_SOURCE_test_asr_transcribe_node "${PROJECT_SOURCE_DIR}/tests/unit/nodes/test_asr_transcribe_node.cpp")
set(EDGEFLOW_SOURCE_test_common_nodes "${PROJECT_SOURCE_DIR}/tests/unit/nodes/test_common_nodes.cpp")
set(EDGEFLOW_SOURCE_test_llm_generate_node "${PROJECT_SOURCE_DIR}/tests/unit/nodes/test_llm_generate_node.cpp")
set(EDGEFLOW_SOURCE_test_ocr_detect_node "${PROJECT_SOURCE_DIR}/tests/unit/nodes/test_ocr_detect_node.cpp")
set(EDGEFLOW_SOURCE_test_rerank_refine_node "${PROJECT_SOURCE_DIR}/tests/unit/nodes/test_rerank_refine_node.cpp")
set(EDGEFLOW_SOURCE_test_structured_json_parse_node "${PROJECT_SOURCE_DIR}/tests/unit/nodes/test_structured_json_parse_node.cpp")
set(EDGEFLOW_SOURCE_test_text_chunk_node "${PROJECT_SOURCE_DIR}/tests/unit/nodes/test_text_chunk_node.cpp")
set(EDGEFLOW_SOURCE_test_text_corpus_source_node "${PROJECT_SOURCE_DIR}/tests/unit/nodes/test_text_corpus_source_node.cpp")
set(EDGEFLOW_SOURCE_test_text_embedding_node "${PROJECT_SOURCE_DIR}/tests/unit/nodes/test_text_embedding_node.cpp")
set(EDGEFLOW_SOURCE_test_text_rerank_node "${PROJECT_SOURCE_DIR}/tests/unit/nodes/test_text_rerank_node.cpp")
set(EDGEFLOW_SOURCE_test_text_rule_match_node "${PROJECT_SOURCE_DIR}/tests/unit/nodes/test_text_rule_match_node.cpp")
set(EDGEFLOW_SOURCE_test_text_template_node "${PROJECT_SOURCE_DIR}/tests/unit/nodes/test_text_template_node.cpp")
set(EDGEFLOW_SOURCE_test_vector_top_k_node "${PROJECT_SOURCE_DIR}/tests/unit/nodes/test_vector_top_k_node.cpp")
set(EDGEFLOW_SOURCE_test_operator_biz_bridge_registry "${PROJECT_SOURCE_DIR}/tests/unit/operator/test_operator_biz_bridge_registry.cpp")
set(EDGEFLOW_SOURCE_test_operator_output_pool "${PROJECT_SOURCE_DIR}/tests/unit/operator/test_operator_output_pool.cpp")
set(EDGEFLOW_SOURCE_test_operator_value_registry "${PROJECT_SOURCE_DIR}/tests/unit/operator/test_operator_value_registry.cpp")

function(edgeflow_assert_required_test_inventory)
  get_property(registered_tests DIRECTORY PROPERTY TESTS)
  foreach(required_test IN LISTS EDGEFLOW_REQUIRED_CONTRACT_TESTS)
    if(NOT required_test IN_LIST registered_tests)
      message(FATAL_ERROR
        "Test mode omitted required contract suite: ${required_test}")
    endif()
  endforeach()
endfunction()
