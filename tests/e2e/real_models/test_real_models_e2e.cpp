#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "company_alg_cpp.hpp"
#include "company_alg_interface.h"
#include "engine/model_interface.h"
#include "engine/model_runtime_factory.h"

namespace llm_edgeflow {

class RealModelE2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    project_root_ =
        std::filesystem::weakly_canonical(std::filesystem::path(__FILE__)
                                              .parent_path()
                                              .parent_path()
                                              .parent_path());
    model_root_ = project_root_ / "models";
    model_path_ = model_root_ / "qwen2.5-0.5b-instruct-q4_k_m.gguf";
    ASSERT_TRUE(std::filesystem::is_regular_file(model_path_))
        << "ENABLE_REAL_MODEL_TESTS requires pinned artifacts; run "
           "./scripts/fetch_real_test_models.sh --gguf-only";
  }

  std::filesystem::path project_root_;
  std::filesystem::path model_root_;
  std::filesystem::path model_path_;

  std::shared_ptr<ILlmModel> CreateModel() const {
    ModelLoadSpec spec;
    spec.model_type = "qwen_causal_lm";
    spec.backend_type = "llama_cpp";
    spec.model_path = model_path_.string();
    spec.model_config = {{"chat_template", "qwen_chatml"},
                         {"add_bos", false},
                         {"random_seed", 17}};
    spec.backend_config = {
        {"context_size", 512}, {"decode_batch_size", 512}, {"n_gpu_layers", 0}};
    std::string diagnostic;
    auto model = ModelRuntimeFactory::Create(spec, &diagnostic);
    EXPECT_NE(model, nullptr) << diagnostic;
    return std::dynamic_pointer_cast<ILlmModel>(model);
  }
};

// 1. 真实 Qwen GGUF 物理前向与自回归 Token 生成测试
TEST_F(RealModelE2ETest, RealQwenGgufTextGeneration) {
  auto model = CreateModel();
  ASSERT_NE(model, nullptr);

  std::string prompt = "你好，请用一句话告诉我什么是人工智能？";
  GenerateOptions opt;
  opt.max_tokens = 64;
  opt.temperature = 0.7f;

  TextBatch output;
  auto t_start = std::chrono::high_resolution_clock::now();
  int ret = model->Generate({{1, 0, prompt}}, opt, &output);
  auto t_end = std::chrono::high_resolution_clock::now();

  double elapsed_ms =
      std::chrono::duration<double, std::milli>(t_end - t_start).count();

  EXPECT_EQ(ret, 0);
  ASSERT_EQ(output.size(), 1U);
  EXPECT_FALSE(output[0].data.empty());
  std::cout << "\n=================================================="
            << std::endl;
  std::cout << "  [Real Model E2E] Prompt : " << prompt << std::endl;
  std::cout << "  [Real Model E2E] Output : " << output[0].data << std::endl;
  std::cout << "  [Real Model E2E] Latency: " << elapsed_ms << " ms"
            << std::endl;
  std::cout << "=================================================="
            << std::endl;
}

// 2. 真实 Qwen 模型在 FixedBatchExecutor 定长对齐批推理压测
TEST_F(RealModelE2ETest, RealQwenBatchExecutionWithPadding) {
  auto model = CreateModel();
  ASSERT_NE(model, nullptr);

  // 构造 3 条请求，验证独立 sequence 与 provenance。
  std::vector<TraceableItem<std::string>> input_batch = {
      {101, 0, "中国的首都是哪里？"},
      {102, 0, "1+1等于几？"},
      {103, 0, "请用一句话介绍机器学习。"},
  };

  std::vector<TraceableItem<std::string>> output_batch;
  GenerateOptions opt;
  opt.max_tokens = 32;
  opt.temperature = 0.1f;

  int ret = model->Generate(input_batch, opt, &output_batch);
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

// 3. 真实模型接入 C ABI 全链路端到端验证
TEST_F(RealModelE2ETest, RealModelCAbiEndToEnd) {
  ASSERT_EQ(Alg_Init(), 0);

  const std::string cfg_path =
      (project_root_ / "configs/pipeline_entity_extract_llamacpp.json")
          .string();
  const std::string model_root = model_root_.string();

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

}  // namespace llm_edgeflow
