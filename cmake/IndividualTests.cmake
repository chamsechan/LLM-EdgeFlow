
# 3. 纯 C11 ABI 兼容性测试 (确保头文件为纯 C 且可被标准 C 编译器直接编译)
add_executable(test_c11_abi_compliance ${EDGEFLOW_SOURCE_test_c11_abi_compliance})
target_link_libraries(test_c11_abi_compliance PRIVATE alg_sdk)
add_test(NAME C11AbiComplianceTest COMMAND test_c11_abi_compliance)

# 4. 架构分层防腐隔离测试 (LayerGuard)
add_test(NAME LayerGuardTest COMMAND ${PROJECT_SOURCE_DIR}/scripts/check_layer_isolation.sh)
add_test(NAME LayerGuardSelfTest COMMAND ${PROJECT_SOURCE_DIR}/scripts/check_layer_isolation.sh --self-test)

# 4.1 架构文档防漂移测试 (ArchitectureDocsDriftTest)
add_test(NAME ArchitectureDocsDriftTest COMMAND ${PROJECT_SOURCE_DIR}/scripts/check_architecture_docs.sh)
add_test(NAME ArchitectureDocsDriftGateSelfTest COMMAND ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_architecture_docs_drift_gate.sh)
add_test(NAME GovernanceConsistencyTest COMMAND ${PROJECT_SOURCE_DIR}/scripts/check_governance.sh)

# 4.2 架构图资产一致性检查测试 (DiagramAssetsCheckTest)
add_test(NAME DiagramAssetsCheckTest COMMAND ${PROJECT_SOURCE_DIR}/scripts/render_architecture_diagrams.sh --check)
add_test(NAME DiagramRenderGateSelfTest COMMAND ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_diagram_render_gate.sh)
add_test(NAME ScriptGeneratorDetectionTest COMMAND ${PROJECT_SOURCE_DIR}/tests/contract/architecture/test_script_generator_detection.sh)

# 5. 单元测试与安全性测试集 (基于 Google Test & CTest)

add_executable(test_batch_executor ${EDGEFLOW_SOURCE_test_batch_executor})
target_link_libraries(test_batch_executor PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME BatchExecutorTest COMMAND test_batch_executor)

add_executable(test_framework_core ${EDGEFLOW_SOURCE_test_framework_core})
target_link_libraries(test_framework_core PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME FrameworkCoreTest COMMAND test_framework_core)

add_executable(test_company_alg_log
    ${EDGEFLOW_SOURCE_test_company_alg_log}
    ${EDGEFLOW_SOURCE_test_company_alg_log_name_override})
target_link_libraries(test_company_alg_log PRIVATE
    alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME CompanyAlgLogTest COMMAND test_company_alg_log)

add_executable(test_c_abi_safety ${EDGEFLOW_SOURCE_test_c_abi_safety})
target_link_libraries(test_c_abi_safety PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME CAbiSafetyTest COMMAND test_c_abi_safety)

add_executable(test_qwen_causal_lm_model ${EDGEFLOW_SOURCE_test_qwen_causal_lm_model})
target_link_libraries(test_qwen_causal_lm_model PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME QwenCausalLmModelTest COMMAND test_qwen_causal_lm_model)

add_executable(test_llama_cpp_backend ${EDGEFLOW_SOURCE_test_llama_cpp_backend})
target_link_libraries(test_llama_cpp_backend PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME LlamaCppBackendTest COMMAND test_llama_cpp_backend)

add_executable(test_different_io_modalities ${EDGEFLOW_SOURCE_test_different_io_modalities})
target_link_libraries(test_different_io_modalities PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME DifferentIoModalitiesTest COMMAND test_different_io_modalities)

add_executable(test_all_biz_pipelines ${EDGEFLOW_SOURCE_test_all_biz_pipelines})
target_link_libraries(test_all_biz_pipelines PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME AllBizPipelinesTest COMMAND test_all_biz_pipelines)

add_executable(test_concurrency_and_edge_cases ${EDGEFLOW_SOURCE_test_concurrency_and_edge_cases})
target_link_libraries(test_concurrency_and_edge_cases PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME ConcurrencyAndEdgeCasesTest COMMAND test_concurrency_and_edge_cases)

add_executable(test_dag_pipeline ${EDGEFLOW_SOURCE_test_dag_pipeline})
target_link_libraries(test_dag_pipeline PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME DagPipelineTest COMMAND test_dag_pipeline)

add_executable(test_runtime_control_and_hot_swap ${EDGEFLOW_SOURCE_test_runtime_control_and_hot_swap})
target_link_libraries(test_runtime_control_and_hot_swap PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME RuntimeControlAndHotSwapTest COMMAND test_runtime_control_and_hot_swap)

add_executable(test_engine_fault_tolerance_and_lifecycle ${EDGEFLOW_SOURCE_test_engine_fault_tolerance_and_lifecycle})
target_link_libraries(test_engine_fault_tolerance_and_lifecycle PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME EngineFaultToleranceAndLifecycleTest COMMAND test_engine_fault_tolerance_and_lifecycle)

add_executable(test_adapter_contract_security ${EDGEFLOW_SOURCE_test_adapter_contract_security})
target_link_libraries(test_adapter_contract_security PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME AdapterContractSecurityTest COMMAND test_adapter_contract_security)

add_executable(test_pipeline_config ${EDGEFLOW_SOURCE_test_pipeline_config})
target_link_libraries(test_pipeline_config PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME PipelineConfigTest COMMAND test_pipeline_config)

add_executable(test_registry_conflict ${EDGEFLOW_SOURCE_test_registry_conflict})
target_link_libraries(test_registry_conflict PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME RegistryConflictNodeTest COMMAND test_registry_conflict --gtest_filter=RegistryConflictNodeTest.*)
set_tests_properties(RegistryConflictNodeTest PROPERTIES TIMEOUT 5)
add_test(NAME RegistryConflictModelTest COMMAND test_registry_conflict --gtest_filter=RegistryConflictModelTest.*)
set_tests_properties(RegistryConflictModelTest PROPERTIES TIMEOUT 5)

add_executable(test_registry_reentrant ${EDGEFLOW_SOURCE_test_registry_reentrant})
target_link_libraries(test_registry_reentrant PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME RegistryReentrantTest COMMAND test_registry_reentrant)
set_tests_properties(RegistryReentrantTest PROPERTIES TIMEOUT 5)

add_executable(test_operator_api ${EDGEFLOW_SOURCE_test_operator_api})
target_link_libraries(test_operator_api PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME OperatorApiTest COMMAND test_operator_api)

add_executable(test_operator_output_pool ${EDGEFLOW_SOURCE_test_operator_output_pool})
target_link_libraries(test_operator_output_pool PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME OperatorOutputPoolTest COMMAND test_operator_output_pool)

add_executable(test_operator_value_registry ${EDGEFLOW_SOURCE_test_operator_value_registry})
target_link_libraries(test_operator_value_registry PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME OperatorValueRegistryTest COMMAND test_operator_value_registry)

add_executable(test_operator_biz_bridge_registry ${EDGEFLOW_SOURCE_test_operator_biz_bridge_registry})
target_link_libraries(test_operator_biz_bridge_registry PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME OperatorBizBridgeRegistryTest COMMAND test_operator_biz_bridge_registry)

add_executable(test_doc_qa_rerank ${EDGEFLOW_SOURCE_test_doc_qa_rerank})
target_link_libraries(test_doc_qa_rerank PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME DocQaRerankTest COMMAND test_doc_qa_rerank)

add_executable(test_rerank_refine_node ${EDGEFLOW_SOURCE_test_rerank_refine_node})
target_link_libraries(test_rerank_refine_node PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME RerankRefineNodeTest COMMAND test_rerank_refine_node)

add_executable(test_pipeline_studio ${EDGEFLOW_SOURCE_test_pipeline_catalog_validator})
target_link_libraries(test_pipeline_studio PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME PipelineStudioTest COMMAND test_pipeline_studio)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(
  NAME VisualizerServerTest
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/tooling/test_visualizer_server.py
)

# Demo Runner 参数化与结果落盘单元测试
add_executable(test_demo_runner
    ${EDGEFLOW_SOURCE_test_demo_runner}
    $<TARGET_OBJECTS:edgeflow_demo_objects>
    $<TARGET_OBJECTS:edgeflow_test_backend_fixtures>
    $<TARGET_OBJECTS:edgeflow_test_business_model_fixtures>)
target_link_libraries(test_demo_runner PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME DemoRunnerTest COMMAND test_demo_runner)

# RFC 0008 SSOT 契约与强类型黑板单测
add_executable(test_catalog_contract_ssot ${EDGEFLOW_SOURCE_test_catalog_contract_ssot})
target_link_libraries(test_catalog_contract_ssot PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME CatalogContractSsotTest COMMAND test_catalog_contract_ssot)

add_executable(test_typed_blackboard_contracts ${EDGEFLOW_SOURCE_test_typed_blackboard_contracts})
target_link_libraries(test_typed_blackboard_contracts PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME TypedBlackboardContractsTest COMMAND test_typed_blackboard_contracts)

add_executable(test_validated_pipeline_plan ${EDGEFLOW_SOURCE_test_validated_pipeline_plan})
target_link_libraries(test_validated_pipeline_plan PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME ValidatedPipelinePlanTest COMMAND test_validated_pipeline_plan)

add_executable(test_node_base_contracts ${EDGEFLOW_SOURCE_test_node_base_contracts})
target_link_libraries(test_node_base_contracts PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME NodeBaseContractsTest COMMAND test_node_base_contracts)

add_executable(test_node_ownership_and_reuse ${EDGEFLOW_SOURCE_test_node_ownership_and_reuse})
target_link_libraries(test_node_ownership_and_reuse PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME NodeOwnershipAndReuseTest COMMAND test_node_ownership_and_reuse)

add_executable(test_definition_schema_validation ${EDGEFLOW_SOURCE_test_definition_schema_validation})
target_link_libraries(test_definition_schema_validation PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME DefinitionSchemaValidationTest COMMAND test_definition_schema_validation)

add_executable(test_model_backend_decoupling
    ${EDGEFLOW_SOURCE_test_model_backend_decoupling}
    $<TARGET_OBJECTS:edgeflow_test_backend_fixtures>)
target_link_libraries(test_model_backend_decoupling PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME ModelBackendDecouplingTest COMMAND test_model_backend_decoupling)

add_executable(test_model_backend_pipeline ${EDGEFLOW_SOURCE_test_model_backend_pipeline})
target_link_libraries(test_model_backend_pipeline PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME ModelBackendPipelineTest COMMAND test_model_backend_pipeline)

add_executable(test_onnx_and_embedding_model
    ${EDGEFLOW_SOURCE_test_onnx_and_embedding_model}
    $<TARGET_OBJECTS:edgeflow_test_backend_fixtures>
    $<TARGET_OBJECTS:edgeflow_test_business_model_fixtures>)
target_link_libraries(test_onnx_and_embedding_model PRIVATE
    alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME OnnxAndEmbeddingModelTest COMMAND test_onnx_and_embedding_model)

add_executable(test_onnx_and_reranker_model
    ${EDGEFLOW_SOURCE_test_onnx_and_reranker_model}
    $<TARGET_OBJECTS:edgeflow_test_backend_fixtures>
    $<TARGET_OBJECTS:edgeflow_test_business_model_fixtures>)
target_link_libraries(test_onnx_and_reranker_model PRIVATE
    alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME OnnxAndRerankerModelTest COMMAND test_onnx_and_reranker_model)

add_executable(test_model_backend_registry_conflict
    ${EDGEFLOW_SOURCE_test_model_backend_registry_conflict})
target_link_libraries(test_model_backend_registry_conflict PRIVATE
    alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME ModelBackendRegistryConflictTest
    COMMAND test_model_backend_registry_conflict)

# RFC 0012 11 类 Common Nodes 独立测试套件与 Operator Golden / Adapter Purity 测试
add_executable(test_text_chunk_node ${EDGEFLOW_SOURCE_test_text_chunk_node})
target_link_libraries(test_text_chunk_node PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME TextChunkNodeTest COMMAND test_text_chunk_node)

add_executable(test_text_embedding_node ${EDGEFLOW_SOURCE_test_text_embedding_node})
target_link_libraries(test_text_embedding_node PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME TextEmbeddingNodeTest COMMAND test_text_embedding_node)

add_executable(test_vector_top_k_node ${EDGEFLOW_SOURCE_test_vector_top_k_node})
target_link_libraries(test_vector_top_k_node PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME VectorTopKNodeTest COMMAND test_vector_top_k_node)

add_executable(test_text_rerank_node ${EDGEFLOW_SOURCE_test_text_rerank_node})
target_link_libraries(test_text_rerank_node PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME TextRerankNodeTest COMMAND test_text_rerank_node)

add_executable(test_text_template_node ${EDGEFLOW_SOURCE_test_text_template_node})
target_link_libraries(test_text_template_node PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME TextTemplateNodeTest COMMAND test_text_template_node)

add_executable(test_llm_generate_node ${EDGEFLOW_SOURCE_test_llm_generate_node})
target_link_libraries(test_llm_generate_node PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME LlmGenerateNodeTest COMMAND test_llm_generate_node)

add_executable(test_asr_transcribe_node ${EDGEFLOW_SOURCE_test_asr_transcribe_node})
target_link_libraries(test_asr_transcribe_node PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME AsrTranscribeNodeTest COMMAND test_asr_transcribe_node)

add_executable(test_ocr_detect_node ${EDGEFLOW_SOURCE_test_ocr_detect_node})
target_link_libraries(test_ocr_detect_node PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME OcrDetectNodeTest COMMAND test_ocr_detect_node)

add_executable(test_text_rule_match_node ${EDGEFLOW_SOURCE_test_text_rule_match_node})
target_link_libraries(test_text_rule_match_node PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME TextRuleMatchNodeTest COMMAND test_text_rule_match_node)

add_executable(test_structured_json_parse_node ${EDGEFLOW_SOURCE_test_structured_json_parse_node})
target_link_libraries(test_structured_json_parse_node PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME StructuredJsonParseNodeTest COMMAND test_structured_json_parse_node)

add_executable(test_text_corpus_source_node ${EDGEFLOW_SOURCE_test_text_corpus_source_node})
target_link_libraries(test_text_corpus_source_node PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME TextCorpusSourceNodeTest COMMAND test_text_corpus_source_node)

add_executable(test_common_nodes ${EDGEFLOW_SOURCE_test_common_nodes})
target_link_libraries(test_common_nodes PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME CommonNodesTest COMMAND test_common_nodes)

add_executable(test_operator_golden ${EDGEFLOW_SOURCE_test_operator_golden})
target_link_libraries(test_operator_golden PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME OperatorGoldenTest COMMAND test_operator_golden)

add_executable(test_adapter_purity ${EDGEFLOW_SOURCE_test_adapter_purity})
target_link_libraries(test_adapter_purity PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME AdapterPurityTest COMMAND test_adapter_purity)

# 设置所有测试工作目录为项目根目录，保证无论从何处运行 CTest，相对路径均一致解析
set_tests_properties(
  C11AbiComplianceTest LayerGuardTest ArchitectureDocsDriftTest
  ArchitectureDocsDriftGateSelfTest DiagramAssetsCheckTest
  DiagramRenderGateSelfTest BatchExecutorTest FrameworkCoreTest
  CAbiSafetyTest CompanyAlgLogTest QwenCausalLmModelTest LlamaCppBackendTest DifferentIoModalitiesTest
  AllBizPipelinesTest ConcurrencyAndEdgeCasesTest DagPipelineTest
  RuntimeControlAndHotSwapTest EngineFaultToleranceAndLifecycleTest
  AdapterContractSecurityTest PipelineConfigTest RegistryConflictNodeTest
  RegistryConflictModelTest RegistryReentrantTest OperatorApiTest
  OperatorOutputPoolTest OperatorValueRegistryTest OperatorBizBridgeRegistryTest
  DocQaRerankTest RerankRefineNodeTest PipelineStudioTest VisualizerServerTest
  DemoRunnerTest CatalogContractSsotTest TypedBlackboardContractsTest
  ValidatedPipelinePlanTest NodeBaseContractsTest NodeOwnershipAndReuseTest
  DefinitionSchemaValidationTest ModelBackendDecouplingTest ModelBackendPipelineTest
  OnnxAndEmbeddingModelTest OnnxAndRerankerModelTest
  ModelBackendRegistryConflictTest TextChunkNodeTest TextEmbeddingNodeTest
  VectorTopKNodeTest TextRerankNodeTest TextTemplateNodeTest
  LlmGenerateNodeTest AsrTranscribeNodeTest OcrDetectNodeTest
  TextRuleMatchNodeTest StructuredJsonParseNodeTest TextCorpusSourceNodeTest
  CommonNodesTest OperatorGoldenTest AdapterPurityTest
  PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
