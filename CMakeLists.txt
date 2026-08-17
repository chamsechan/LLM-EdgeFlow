cmake_minimum_required(VERSION 3.16)
project(AlgSdkFramework LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 编译选项
add_compile_options(-Wall -Wextra -O2 -fPIC)

# 头文件搜索路径
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# 引入第三方 JSON 库 (nlohmann/json) 自动下载配置
include(cmake/NlohmannJson.cmake)

# 引入第三方开源推理引擎 (ONNX Runtime & llama.cpp) 自动下载配置
include(cmake/ThirdPartyEngines.cmake)

# 核心框架与业务算子源文件列表
set(FRAMEWORK_SRCS
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
    VERSION 1.0.0
    SOVERSION 1
)

# 引入 Google Test 框架 (自动下载或使用系统库)
include(cmake/GoogleTest.cmake)

# 2. 生成下游部门集成测试 Demo 可执行程序
add_executable(alg_demo demo/main.cpp)
target_link_libraries(alg_demo PRIVATE alg_sdk)

# 3. 单元测试与安全性测试集 (基于 Google Test & CTest)
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

# 4. 纯 C++ 零依赖原生 DAG 可视化工具 (专为无 Python 的极简嵌入式/边缘环境设计)
add_executable(alg_show src/tools/alg_show.cpp)
target_link_libraries(alg_show PRIVATE nlohmann_json::nlohmann_json)
