#include <gtest/gtest.h>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "company_alg_interface.h"
#include "core/session_context.h"
#include "engine/engine_interface.h"
#include "engine/engine_registry.h"
#include "third_party/nlohmann/json.hpp"

std::string GetConfigPath(const std::string& rel_path) {
  FILE* fp = fopen(rel_path.c_str(), "r");
  if (fp) {
    fclose(fp);
    return rel_path;
  }
  return "../" + rel_path;
}

namespace alg_framework {

class QwenEnginesComparisonTest : public ::testing::Test {
 protected:
  void SetUp() override { Alg_Init(); }
  void TearDown() override { Alg_DeInit(); }
};

// 1. 测试 Qwen 模型的两个引擎 (Mock NPU Engine vs llama.cpp Engine)
// 的独立接口行为与输出对齐
TEST_F(QwenEnginesComparisonTest, DirectInterfaceComparison) {
  auto& factory = EngineFactory::Instance();

  auto npu_llm = factory.Create("mock_npu_llm");
  auto llama_llm = factory.Create("llama_cpp");

  ASSERT_NE(npu_llm, nullptr);
  ASSERT_NE(llama_llm, nullptr);

  nlohmann::json npu_cfg = {{"max_batch_size", 2}, {"max_seq_len", 1024}};
  nlohmann::json llama_cfg = {{"max_batch_size", 2}, {"max_seq_len", 1024}};

  bool ok_npu = npu_llm->Load("./models/qwen_1.5b_npu.bin", npu_cfg);
  bool ok_llama =
      llama_llm->Load("./models/qwen2.5_1.5b_instruct.gguf", llama_cfg);

  EXPECT_TRUE(ok_npu);
  EXPECT_TRUE(ok_llama);

  auto* npu_ptr = dynamic_cast<ILlmEngine*>(npu_llm.get());
  auto* llama_ptr = dynamic_cast<ILlmEngine*>(llama_llm.get());

  ASSERT_NE(npu_ptr, nullptr);
  ASSERT_NE(llama_ptr, nullptr);

  ILlmEngine::GenerateOption opt;
  opt.temperature = 0.1f;
  opt.max_tokens = 64;

  std::string prompt_entity =
      "你是一个中文实体提取助手。请从以下句子中提取名词：\n"
      "张三在清华大学毕业后加入了一家北京的人工智能公司，作为算法工程师负责芯片"
      "研发。\n"
      "提取结果：";

  std::string npu_out_entity;
  std::string llama_out_entity;

  int ret_npu = npu_ptr->Generate(prompt_entity, opt, &npu_out_entity);
  int ret_llama = llama_ptr->Generate(prompt_entity, opt, &llama_out_entity);

  EXPECT_EQ(ret_npu, 0);
  EXPECT_EQ(ret_llama, 0);

  auto j_npu = nlohmann::json::parse(npu_out_entity);
  auto j_llama = nlohmann::json::parse(llama_out_entity);
  EXPECT_TRUE(j_npu.contains("nouns") && j_npu["nouns"].is_array());
  EXPECT_TRUE(j_llama.contains("nouns") && j_llama["nouns"].is_array());

  // 批量推理带溯源标签
  std::vector<TraceableItem<std::string>> batch_prompts;
  batch_prompts.emplace_back(
      1001, 0, "【用户提问】我想办理退款退货业务，请问规则是什么？");
  batch_prompts.emplace_back(
      1002, 0, "【用户提问】请介绍现代C++算法SDK的核心架构设计？");
  batch_prompts.emplace_back(1003, 0,
                             "【用户提问】请问支持哪些模型格式与引擎？");

  std::vector<TraceableItem<std::string>> npu_batch_out;
  std::vector<TraceableItem<std::string>> llama_batch_out;

  ret_npu = npu_ptr->InferTraceableBatch(batch_prompts, opt, &npu_batch_out);
  ret_llama =
      llama_ptr->InferTraceableBatch(batch_prompts, opt, &llama_batch_out);

  EXPECT_EQ(ret_npu, 0);
  EXPECT_EQ(ret_llama, 0);
  EXPECT_EQ(npu_batch_out.size(), 3);
  EXPECT_EQ(llama_batch_out.size(), 3);

  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(npu_batch_out[i].req_id, batch_prompts[i].req_id);
    EXPECT_EQ(llama_batch_out[i].req_id, batch_prompts[i].req_id);
    EXPECT_FALSE(npu_batch_out[i].data.empty());
    EXPECT_FALSE(llama_batch_out[i].data.empty());
  }
}

// 2. 测试通过标准 C ABI 接口全流程无缝切换 Qwen 引擎（NPU配置 vs
// GGUF/llama.cpp配置）
TEST_F(QwenEnginesComparisonTest, CAbiPipelineSwitching) {
  // Case A: NPU Qwen
  {
    std::string cfg_npu = GetConfigPath("configs/pipeline_entity_extract.json");
    CompanyAlgParamCreate param_npu;
    param_npu.config_file_path = cfg_npu.c_str();
    param_npu.model_root_dir = "./models";
    param_npu.device_id = 0;
    param_npu.biz_type = ALG_BIZ_TYPE_ENTITY_EXTRACT;

    void* handle_npu = nullptr;
    int ret = Alg_Create(&handle_npu, &param_npu);
    ASSERT_EQ(ret, 0);
    ASSERT_NE(handle_npu, nullptr);

    CompanyEntityInputStruct in_item{
        50001,
        "李四在浙江大学毕业后去深圳加入了一家人工智能芯片公司担任算法工程师。"};
    std::vector<void*> inputs = {&in_item};
    CompanyEntityOutputStruct out_item;
    std::vector<void*> outputs = {&out_item};

    ret = Alg_Process(handle_npu, inputs, outputs);
    EXPECT_EQ(ret, 0);
    EXPECT_TRUE(std::string(out_item.entities_json).find("浙江大学") !=
                std::string::npos);

    ret = Alg_Destroy(handle_npu);
    EXPECT_EQ(ret, 0);
  }

  // Case B: llama.cpp Qwen GGUF
  {
    std::string cfg_llama =
        GetConfigPath("configs/pipeline_entity_extract_llamacpp.json");
    CompanyAlgParamCreate param_llama;
    param_llama.config_file_path = cfg_llama.c_str();
    param_llama.model_root_dir = "./models";
    param_llama.device_id = 0;
    param_llama.biz_type = ALG_BIZ_TYPE_ENTITY_EXTRACT;

    void* handle_llama = nullptr;
    int ret = Alg_Create(&handle_llama, &param_llama);
    ASSERT_EQ(ret, 0);
    ASSERT_NE(handle_llama, nullptr);

    CompanyEntityInputStruct in_item{
        50002,
        "李四在浙江大学毕业后去深圳加入了一家人工智能芯片公司担任算法工程师。"};
    std::vector<void*> inputs = {&in_item};
    CompanyEntityOutputStruct out_item;
    std::vector<void*> outputs = {&out_item};

    ret = Alg_Process(handle_llama, inputs, outputs);
    EXPECT_EQ(ret, 0);
    EXPECT_TRUE(std::string(out_item.entities_json).find("浙江大学") !=
                std::string::npos);

    ret = Alg_Destroy(handle_llama);
    EXPECT_EQ(ret, 0);
  }
}

}  // namespace alg_framework
