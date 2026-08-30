cmake_minimum_required(VERSION 3.16)
project(AlgSdkFramework LANGUAGES C CXX)

# 严格禁止 In-Source 构建 (防止误用 cmake . 污染源码目录并触发重复下载阻塞)
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
  message(FATAL_ERROR "
  [ERROR] In-source builds are strictly forbidden!
  Building directly inside the source directory with 'cmake .' causes pollution and hangs on network dependencies.
  Please use out-of-source build instead:
    mkdir -p build && cd build && cmake .. && make -j4
  Or:
    cmake -B build -S . && cmake --build build -j4
  ")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)
# 编译与链接加速配置
include(cmake/BuildAcceleration.cmake)
add_compile_options(-Wall -Wextra -fPIC)

# 确保 mock npu 模型的占位文件存在

option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
set(LLM_EDGEFLOW_SANITIZERS "address,undefined" CACHE STRING
    "Comma-separated Clang/GCC sanitizer set used when ENABLE_SANITIZERS=ON")
if(ENABLE_SANITIZERS)
  if(NOT LLM_EDGEFLOW_SANITIZERS MATCHES "^(address|undefined|address,undefined|undefined,address|thread)$")
    message(FATAL_ERROR
      "Unsupported LLM_EDGEFLOW_SANITIZERS='${LLM_EDGEFLOW_SANITIZERS}'. "
      "Use address, undefined, address,undefined, or thread.")
  endif()
  message(STATUS "Enabling sanitizers (-fsanitize=${LLM_EDGEFLOW_SANITIZERS})")
  add_compile_options(
      -O1 -g -fsanitize=${LLM_EDGEFLOW_SANITIZERS}
      -fno-omit-frame-pointer -fno-optimize-sibling-calls)
  set(CMAKE_EXE_LINKER_FLAGS
      "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=${LLM_EDGEFLOW_SANITIZERS}")
  set(CMAKE_SHARED_LINKER_FLAGS
      "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=${LLM_EDGEFLOW_SANITIZERS}")
endif()

# 头文件搜索路径
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  if(EXISTS "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1")
    include_directories(SYSTEM "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1")
  endif()
endif()

# 引入第三方 JSON 库 (nlohmann/json) 自动下载配置
include(cmake/NlohmannJson.cmake)

# 引入第三方开源推理引擎 (ONNX Runtime & llama.cpp) 自动下载配置
include(cmake/ThirdPartyEngines.cmake)

# 核心框架与公共算子源文件列表
set(FRAMEWORK_SRCS
    src/company_alg_log.cpp
    src/core/pipeline_catalog.cpp
    src/core/pipeline_config.cpp
    src/core/pipeline_validator.cpp
    src/core/pipeline.cpp
    src/engine/runtime/model_registry.cpp
    src/engine/runtime/backend_registry.cpp
    src/engine/runtime/model_runtime_factory.cpp
    src/engine/backends/onnxruntime/onnxruntime_backend.cpp
    src/engine/backends/llama_cpp/llama_cpp_backend.cpp
    src/engine/models/bge_embedding/bert_wordpiece_tokenizer.cpp
    src/engine/models/bge_common/bert_model_support.cpp
    src/engine/models/bge_embedding/bge_embedding_model.cpp
    src/engine/models/bge_reranker/bge_reranker_model.cpp
    src/engine/models/qwen_causal_lm/qwen_causal_lm_model.cpp
    src/common_nodes/text_template_node.cpp
    src/common_nodes/text_chunk_node.cpp
    src/common_nodes/text_rule_match_node.cpp
    src/common_nodes/structured_json_parse_node.cpp
    src/common_nodes/text_embedding_node.cpp
    src/common_nodes/vector_top_k_node.cpp
    src/common_nodes/text_rerank_node.cpp
    src/common_nodes/llm_generate_node.cpp
    src/common_nodes/asr_transcribe_node.cpp
    src/common_nodes/ocr_detect_node.cpp
    src/common_nodes/text_corpus_source_node.cpp
    src/adapter/adapters/keyword_match_adapter.cpp
    src/adapter/adapters/entity_extract_adapter.cpp
    src/adapter/adapters/doc_qa_adapter.cpp
    src/adapter/adapters/compliance_audit_adapter.cpp
    src/adapter/adapters/ocr_doc_qa_adapter.cpp
    src/adapter/adapters/audio_asr_intent_adapter.cpp
    src/adapter/adapters/cross_rerank_adapter.cpp
    src/adapter/shared_algorithm_runtime.cpp
    src/adapter/operator/company_conf_resolver.cpp
    src/adapter/operator/operator_value_type_registry.cpp
    src/adapter/operator/operator_biz_bridge_registry.cpp
    src/adapter/operator/operator_output_pool.cpp
    src/adapter/operator/biz_bridges/keyword_match_bridge.cpp
    src/adapter/operator/biz_bridges/entity_extract_bridge.cpp
    src/adapter/operator/biz_bridges/doc_qa_bridge.cpp
    src/adapter/operator/biz_bridges/compliance_audit_bridge.cpp
    src/adapter/operator/biz_bridges/ocr_doc_qa_bridge.cpp
    src/adapter/operator/biz_bridges/audio_asr_intent_bridge.cpp
    src/adapter/operator/biz_bridges/cross_rerank_bridge.cpp
    src/adapter/operator/operator_control_registry.cpp
    src/adapter/operator/operator_process_binding.cpp
    src/adapter/operator/operator_adapter.cpp
    src/adapter/company_c_adapter.cpp
)

# 1. 生成对外交付的动态算法库 (.so)
add_library(alg_sdk SHARED ${FRAMEWORK_SRCS})
target_link_libraries(alg_sdk PUBLIC nlohmann_json::nlohmann_json)
if(LLM_EDGEFLOW_HAS_ONNXRUNTIME)
  target_include_directories(alg_sdk PRIVATE SYSTEM "${ONNXRUNTIME_INCLUDE_DIR}")
  target_compile_definitions(alg_sdk PRIVATE HAVE_ONNXRUNTIME=1)
  target_link_libraries(alg_sdk PRIVATE "${ONNXRUNTIME_LIB}")
endif()
if(LLM_EDGEFLOW_HAS_LLAMACPP)
  target_include_directories(alg_sdk SYSTEM PRIVATE ${LLAMACPP_INCLUDE_DIRS})
  target_compile_definitions(alg_sdk PRIVATE HAVE_LLAMACPP=1)
endif()
if(THIRD_PARTY_ENGINE_LIBS)
  target_link_libraries(alg_sdk PRIVATE ${THIRD_PARTY_ENGINE_LIBS})
endif()
set_target_properties(alg_sdk PROPERTIES
    OUTPUT_NAME "company_alg_sdk"
    VERSION 4.3.0
    SOVERSION 4
)

# 引入 Google Test 框架 (自动下载或使用系统库)
include(cmake/GoogleTest.cmake)
enable_testing()

option(LLM_EDGEFLOW_SHARDED_TEST_RUNNERS
       "Build PCH-enabled sharded Google Test runners" ON)

# 2. 生成下游部门集成测试 Demo 可执行程序 (参数化多业务模块化 Runner)
set(ALG_DEMO_SRCS
    demo/main.cpp
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
    demo/biz/cross_rerank_demo.cpp
    tests/support/inference/test_tensor_backend.cpp
    tests/support/inference/test_causal_lm_backend.cpp
    tests/support/inference/test_business_models.cpp
)
add_executable(alg_demo ${ALG_DEMO_SRCS})
target_link_libraries(alg_demo PRIVATE alg_sdk)

# Test target organization for both sharded and individual modes lives in one
# module so the project entry point does not duplicate test metadata.
include(cmake/Tests.cmake)

# 6. 纯 C++ 零依赖原生 DAG 可视化工具 (专为无 Python 的极简嵌入式/边缘环境设计)
add_executable(alg_show src/tools/alg_show.cpp)
target_link_libraries(alg_show PRIVATE nlohmann_json::nlohmann_json)

# 6.1 Pipeline Catalog / Validator / Normalizer machine-readable CLI
add_executable(alg_pipeline_tool src/tools/alg_pipeline_tool.cpp)
target_link_libraries(alg_pipeline_tool PRIVATE alg_sdk)

# Validation parity tests exercise deterministic test-only Model/Backend
# definitions. Keep those registrations out of the production CLI and SDK by
# linking them into a dedicated test executable.
add_executable(alg_pipeline_tool_test
    src/tools/alg_pipeline_tool.cpp
    tests/support/inference/test_tensor_backend.cpp
    tests/support/inference/test_causal_lm_backend.cpp
    tests/support/inference/test_business_models.cpp)
target_link_libraries(alg_pipeline_tool_test PRIVATE alg_sdk)

# 7. 独立物理真实模型端到端测试套件 (阶段性/物理硬件压测，隔离于默认日常 CTest)
option(ENABLE_REAL_MODEL_TESTS "Enable real model weights E2E test suite" OFF)
if(ENABLE_REAL_MODEL_TESTS)
  add_executable(test_real_models_e2e tests/e2e_real_models/test_real_models_e2e.cpp)
  target_link_libraries(test_real_models_e2e PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
  add_test(NAME RealModelE2ETest COMMAND test_real_models_e2e)
  set_tests_properties(RealModelE2ETest PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    LABELS "tier3;slow;real-model")
endif()
