#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "company_alg_interface.h"
#include "engine/engine_interface.h"
#include "engine/engine_registry.h"
#include "engine/llama_cpp/llama_cpp_engine.h"

namespace alg_framework {

class RealModelE2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    // 验证真实模型文件是否存在
    model_path_ = "./models/qwen2.5-0.5b-instruct-q4_k_m.gguf";
    FILE* fp = fopen(model_path_.c_str(), "rb");
    if (!fp) {
      model_path_ = "../models/qwen2.5-0.5b-instruct-q4_k_m.gguf";
      fp = fopen(model_path_.c_str(), "rb");
    }
    if (fp) {
      fclose(fp);
      model_available_ = true;
    } else {
      model_available_ = false;
    }
  }

  std::string model_path_;
  bool model_available_ = false;
};

// 1. 真实 Qwen GGUF 物理前向与自回归 Token 生成测试
TEST_F(RealModelE2ETest, RealQwenGgufTextGeneration) {
  if (!model_available_) {
    std::cout << "[SKIPPED] Real GGUF model file not found at " << model_path_
              << ", run ./scripts/fetch_real_test_models.sh first."
              << std::endl;
    GTEST_SKIP();
  }

  LlamaCppEngine engine;
  nlohmann::json cfg = {{"max_batch_size", 1}, {"max_seq_len", 512}};
  ASSERT_TRUE(engine.Load(model_path_, cfg));

  std::string prompt = "你好，请用一句话告诉我什么是人工智能？";
  ILlmEngine::GenerateOption opt;
  opt.max_tokens = 64;
  opt.temperature = 0.7f;

  std::string output;
  auto t_start = std::chrono::high_resolution_clock::now();
  int ret = engine.Generate(prompt, opt, &output);
  auto t_end = std::chrono::high_resolution_clock::now();

  double elapsed_ms =
      std::chrono::duration<double, std::milli>(t_end - t_start).count();

  EXPECT_EQ(ret, 0);
  EXPECT_FALSE(output.empty());
  std::cout << "\n=================================================="
            << std::endl;
  std::cout << "  [Real Model E2E] Prompt : " << prompt << std::endl;
  std::cout << "  [Real Model E2E] Output : " << output << std::endl;
  std::cout << "  [Real Model E2E] Latency: " << elapsed_ms << " ms"
            << std::endl;
  std::cout << "=================================================="
            << std::endl;
}

// 2. 真实 Qwen 模型在 FixedBatchExecutor 定长对齐批推理压测
TEST_F(RealModelE2ETest, RealQwenBatchExecutionWithPadding) {
  if (!model_available_) {
    GTEST_SKIP();
  }

  LlamaCppEngine engine;
  nlohmann::json cfg = {{"max_batch_size", 2}, {"max_seq_len", 512}};
  ASSERT_TRUE(engine.Load(model_path_, cfg));

  // 构造 3 条请求，Fixed Batch = 2 (触发 2 个硬件 Batch，最后 1 个 Batch 包含 1
  // 个 Dummy Pad)
  std::vector<TraceableItem<std::string>> input_batch = {
      {101, 0, "中国的首都是哪里？"},
      {102, 0, "1+1等于几？"},
      {103, 0, "请用一句话介绍机器学习。"},
  };

  std::vector<TraceableItem<std::string>> output_batch;
  ILlmEngine::GenerateOption opt;
  opt.max_tokens = 32;
  opt.temperature = 0.1f;

  int ret = engine.InferTraceableBatch(input_batch, opt, &output_batch);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(output_batch.size(), 3);

  // 严格校验 Provenance ID 追溯性与非空真实生成
  for (size_t i = 0; i < output_batch.size(); ++i) {
    EXPECT_EQ(output_batch[i].req_id, input_batch[i].req_id);
    EXPECT_FALSE(output_batch[i].data.empty());
    std::cout << "  [Batch Item #" << i << "] ReqID: " << output_batch[i].req_id
              << " | Output: " << output_batch[i].data << std::endl;
  }
}

static std::string GetConfigPath(const std::string& rel_path) {
  FILE* fp = fopen(rel_path.c_str(), "r");
  if (fp) {
    fclose(fp);
    return rel_path;
  }
  return "../" + rel_path;
}

// 3. 真实模型接入 C ABI 全链路端到端验证
TEST_F(RealModelE2ETest, RealModelCAbiEndToEnd) {
  if (!model_available_) {
    GTEST_SKIP();
  }

  ASSERT_EQ(Alg_Init(), 0);

  std::string cfg_path = GetConfigPath("configs/pipeline_entity_extract.json");
  std::string model_root = GetConfigPath("models");

  CompanyAlgParamCreate create_param;
  create_param.config_file_path = cfg_path.c_str();
  create_param.model_root_dir = model_root.c_str();
  create_param.device_id = 0;
  create_param.biz_type = ALG_BIZ_TYPE_ENTITY_EXTRACT;

  void* handle = nullptr;
  ASSERT_EQ(Alg_Create(&handle, &create_param), 0);
  ASSERT_NE(handle, nullptr);

  CompanyEntityInputStruct req{99001,
                               "李雷在微软北京研发中心负责AI大模型芯片开发。"};
  std::vector<void*> inputs = {&req};

  CompanyEntityOutputStruct out;
  std::vector<void*> outputs = {&out};

  int ret = Alg_Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out.request_id, 99001);

  std::cout << "  [C ABI Real Model Output] " << out.entities_json << std::endl;

  EXPECT_EQ(Alg_Destroy(handle), 0);
  EXPECT_EQ(Alg_DeInit(), 0);
}

}  // namespace alg_framework
