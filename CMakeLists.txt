cmake_minimum_required(VERSION 3.16)
project(AlgSdkFramework LANGUAGES C CXX)

# 自动检测并启用 ccache 编译加速 (如果存在则无缝加速，不存在则平滑降级)
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  message(STATUS "Found ccache: ${CCACHE_PROGRAM}, compiler caching enabled!")
  set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 编译选项
add_compile_options(-Wall -Wextra -O2 -fPIC)

option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(ENABLE_SANITIZERS)
  message(STATUS "Enabling AddressSanitizer and UndefinedBehaviorSanitizer (-fsanitize=address,undefined)")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address,undefined")
endif()

# 头文件搜索路径
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(APPLE)
  if(EXISTS "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1")
    include_directories(SYSTEM "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1")
  endif()
endif()

# 引入第三方 JSON 库 (nlohmann/json) 自动下载配置
include(cmake/NlohmannJson.cmake)

# 引入第三方开源推理引擎 (ONNX Runtime & llama.cpp) 自动下载配置
include(cmake/ThirdPartyEngines.cmake)

# 核心框架与业务算子源文件列表
set(FRAMEWORK_SRCS
    src/core/pipeline_config.cpp
    src/core/pipeline.cpp
    src/engine/mock_npu/mock_npu_embedding_engine.cpp
    src/engine/mock_npu/mock_npu_llm_engine.cpp
    src/engine/mock_npu/mock_npu_rerank_engine.cpp
    src/engine/mock_npu/mock_npu_ocr_engine.cpp
    src/engine/mock_npu/mock_npu_asr_engine.cpp
    src/engine/onnx/onnx_embedding_engine.cpp
    src/engine/onnx/onnx_rerank_engine.cpp
    src/engine/llama_cpp/llama_cpp_engine.cpp
    src/common_nodes/prompt_builder_node.cpp
    src/common_nodes/vector_search_node.cpp
    src/common_nodes/rerank_refine_node.cpp
    src/business/doc_qa/doc_chunk_pre_node.cpp
    src/business/doc_qa/doc_embedding_node.cpp
    src/business/doc_qa/intent_rule_node.cpp
    src/business/doc_qa/llm_generate_node.cpp
    src/business/doc_qa/doc_qa_post_node.cpp
    src/business/keyword_match/keyword_matcher_node.cpp
    src/business/entity_extract/entity_extract_pre_node.cpp
    src/business/entity_extract/entity_extract_post_node.cpp
    src/business/dialogue_audit/safety_rule_pre_node.cpp
    src/business/dialogue_audit/dense_retrieval_node.cpp
    src/business/dialogue_audit/cross_rerank_node.cpp
    src/business/dialogue_audit/risk_prompt_node.cpp
    src/business/dialogue_audit/llm_audit_node.cpp
    src/business/dialogue_audit/audit_post_node.cpp
    src/business/ocr_doc_qa/image_pre_node.cpp
    src/business/ocr_doc_qa/ocr_infer_node.cpp
    src/business/ocr_doc_qa/ocr_doc_post_node.cpp
    src/business/audio_asr/audio_feature_pre_node.cpp
    src/business/audio_asr/asr_infer_node.cpp
    src/business/audio_asr/slot_extract_node.cpp
    src/business/audio_asr/audio_post_node.cpp
    src/business/cross_rerank/rerank_pair_builder_node.cpp
    src/business/cross_rerank/cross_rerank_batch_node.cpp
    src/business/cross_rerank/rerank_sort_post_node.cpp
    src/adapter/adapters/keyword_match_adapter.cpp
    src/adapter/adapters/entity_extract_adapter.cpp
    src/adapter/adapters/doc_qa_adapter.cpp
    src/adapter/adapters/compliance_audit_adapter.cpp
    src/adapter/adapters/ocr_doc_qa_adapter.cpp
    src/adapter/adapters/audio_asr_intent_adapter.cpp
    src/adapter/adapters/cross_rerank_adapter.cpp
    src/adapter/shared_algorithm_runtime.cpp
    src/adapter/platform/company_conf_resolver.cpp
    src/adapter/platform/platform_io_registry.cpp
    src/adapter/platform/platform_control_registry.cpp
    src/adapter/platform_operator_adapter.cpp
    src/adapter/company_c_adapter.cpp
)

# 1. 生成对外交付的动态算法库 (.so)
add_library(alg_sdk SHARED ${FRAMEWORK_SRCS})
target_link_libraries(alg_sdk PUBLIC nlohmann_json::nlohmann_json)
if(THIRD_PARTY_ENGINE_LIBS)
  target_link_libraries(alg_sdk PRIVATE ${THIRD_PARTY_ENGINE_LIBS})
endif()
set_target_properties(alg_sdk PROPERTIES
    OUTPUT_NAME "company_alg_sdk"
    VERSION 2.0.0
    SOVERSION 2
)

# 引入 Google Test 框架 (自动下载或使用系统库)
include(cmake/GoogleTest.cmake)

# 2. 生成下游部门集成测试 Demo 可执行程序
add_executable(alg_demo demo/main.cpp)
target_link_libraries(alg_demo PRIVATE alg_sdk)

# 3. 纯 C11 ABI 兼容性测试 (确保头文件为纯 C 且可被标准 C 编译器直接编译)
add_executable(test_c11_abi_compliance tests/test_c11_abi_compliance.c)
target_link_libraries(test_c11_abi_compliance PRIVATE alg_sdk)
add_test(NAME C11AbiComplianceTest COMMAND test_c11_abi_compliance)

# 4. 架构分层防腐隔离测试 (LayerGuard)
add_test(NAME LayerGuardTest COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_layer_isolation.sh)

# 5. 单元测试与安全性测试集 (基于 Google Test & CTest)
enable_testing()

add_executable(test_batch_executor tests/test_batch_executor.cpp)
target_link_libraries(test_batch_executor PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME BatchExecutorTest COMMAND test_batch_executor)

add_executable(test_framework_core tests/test_framework_core.cpp)
target_link_libraries(test_framework_core PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME FrameworkCoreTest COMMAND test_framework_core)

add_executable(test_c_abi_safety tests/test_c_abi_safety.cpp)
target_link_libraries(test_c_abi_safety PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME CAbiSafetyTest COMMAND test_c_abi_safety)

add_executable(test_qwen_engines_comparison tests/test_qwen_engines_comparison.cpp)
target_link_libraries(test_qwen_engines_comparison PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME QwenEnginesComparisonTest COMMAND test_qwen_engines_comparison)

add_executable(test_different_io_modalities tests/test_different_io_modalities.cpp)
target_link_libraries(test_different_io_modalities PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME DifferentIoModalitiesTest COMMAND test_different_io_modalities)

add_executable(test_all_business_pipelines tests/test_all_business_pipelines.cpp)
target_link_libraries(test_all_business_pipelines PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME AllBusinessPipelinesTest COMMAND test_all_business_pipelines)

add_executable(test_concurrency_and_edge_cases tests/test_concurrency_and_edge_cases.cpp)
target_link_libraries(test_concurrency_and_edge_cases PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME ConcurrencyAndEdgeCasesTest COMMAND test_concurrency_and_edge_cases)

add_executable(test_dag_pipeline tests/test_dag_pipeline.cpp)
target_link_libraries(test_dag_pipeline PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME DagPipelineTest COMMAND test_dag_pipeline)

add_executable(test_runtime_control_and_hot_swap tests/test_runtime_control_and_hot_swap.cpp)
target_link_libraries(test_runtime_control_and_hot_swap PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME RuntimeControlAndHotSwapTest COMMAND test_runtime_control_and_hot_swap)

add_executable(test_engine_fault_tolerance_and_lifecycle tests/test_engine_fault_tolerance_and_lifecycle.cpp)
target_link_libraries(test_engine_fault_tolerance_and_lifecycle PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME EngineFaultToleranceAndLifecycleTest COMMAND test_engine_fault_tolerance_and_lifecycle)

add_executable(test_adapter_contract_security tests/test_adapter_contract_security.cpp)
target_link_libraries(test_adapter_contract_security PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME AdapterContractSecurityTest COMMAND test_adapter_contract_security)

add_executable(test_pipeline_config tests/test_pipeline_config.cpp)
target_link_libraries(test_pipeline_config PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME PipelineConfigTest COMMAND test_pipeline_config)

add_executable(test_registry_conflict tests/test_registry_conflict.cpp)
target_link_libraries(test_registry_conflict PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME RegistryConflictNodeTest COMMAND test_registry_conflict --gtest_filter=RegistryConflictNodeTest.*)
set_tests_properties(RegistryConflictNodeTest PROPERTIES TIMEOUT 5)
add_test(NAME RegistryConflictEngineTest COMMAND test_registry_conflict --gtest_filter=RegistryConflictEngineTest.*)
set_tests_properties(RegistryConflictEngineTest PROPERTIES TIMEOUT 5)

add_executable(test_registry_reentrant tests/test_registry_reentrant.cpp)
target_link_libraries(test_registry_reentrant PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME RegistryReentrantTest COMMAND test_registry_reentrant)
set_tests_properties(RegistryReentrantTest PROPERTIES TIMEOUT 5)

add_executable(test_platform_operator tests/test_platform_operator.cpp)
target_link_libraries(test_platform_operator PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME PlatformOperatorTest COMMAND test_platform_operator)

add_executable(test_doc_qa_rerank tests/test_doc_qa_rerank.cpp)
target_link_libraries(test_doc_qa_rerank PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME DocQaRerankTest COMMAND test_doc_qa_rerank)

add_executable(test_rerank_refine_node tests/test_rerank_refine_node.cpp)
target_link_libraries(test_rerank_refine_node PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
add_test(NAME RerankRefineNodeTest COMMAND test_rerank_refine_node)

# 6. 纯 C++ 零依赖原生 DAG 可视化工具 (专为无 Python 的极简嵌入式/边缘环境设计)
add_executable(alg_show src/tools/alg_show.cpp)
target_link_libraries(alg_show PRIVATE nlohmann_json::nlohmann_json)

# 7. 独立物理真实模型端到端测试套件 (阶段性/物理硬件压测，隔离于默认日常 CTest)
option(ENABLE_REAL_MODEL_TESTS "Enable real model weights E2E test suite" OFF)
add_executable(test_real_models_e2e tests/e2e_real_models/test_real_models_e2e.cpp)
target_link_libraries(test_real_models_e2e PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
if(ENABLE_REAL_MODEL_TESTS)
  add_test(NAME RealModelE2ETest COMMAND test_real_models_e2e)
endif()
