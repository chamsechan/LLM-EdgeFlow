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

function(edgeflow_assert_required_test_inventory)
  get_property(registered_tests DIRECTORY PROPERTY TESTS)
  foreach(required_test IN LISTS EDGEFLOW_REQUIRED_CONTRACT_TESTS)
    if(NOT required_test IN_LIST registered_tests)
      message(FATAL_ERROR
        "Test mode omitted required contract suite: ${required_test}")
    endif()
  endforeach()
endfunction()
