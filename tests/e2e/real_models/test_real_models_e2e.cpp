#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "engine/model_interface.h"
#include "engine/model_runtime_factory.h"
#include "platform_mock/operator_data_types.h"

namespace llm_edgeflow {

class RealModelE2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    project_root_ = std::filesystem::weakly_canonical(
        std::filesystem::path(LLM_EDGEFLOW_PROJECT_SOURCE_DIR));
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
    {"add_bos", false}, { "random_seed", 17 }
  };
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

// 3. 真实模型接入 Operator 全链路端到端验证
TEST_F(RealModelE2ETest, RealModelOperatorEndToEnd) {
  std::vector<std::string> sentences = {
      "李雷在微软北京研发中心负责AI大模型芯片开发。"};
  std::ifstream corpus(project_root_ / "data/corpus_entity_extract.txt");
  ASSERT_TRUE(corpus.good());
  std::string line;
  while (std::getline(corpus, line)) {
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && line[first] != '#')
      sentences.push_back(line);
  }
  ASSERT_GT(sentences.size(), 1u) << "Public Profile corpus must not be empty";

  auto op = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(op.Init(), 0);

  const std::string root_str = project_root_.string();
  operator_api::CreateParam create_param{};
  create_param.model_path = root_str.c_str();
  create_param.cfg_file_name = "configs/pipeline_entity_extract_cpu.conf";
  create_param.device_id = 0;
  create_param.compute_platform = operator_api::ComputePlatform::kCpu;
  create_param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(op.Create(&handle, &create_param), 0);
  ASSERT_NE(handle, nullptr);

  for (size_t i = 0; i < sentences.size(); ++i) {
    SCOPED_TRACE(sentences[i]);
    const uint64_t request_id = i == 0 ? 99001 : 30000 + i;
    CompanyString cs{static_cast<int32_t>(sentences[i].size()),
                     const_cast<char*>(sentences[i].data())};
    CompanyOperatorEntityInput req{request_id, &cs};

    operator_api::NamedIoBatch inputs(1);
    inputs[0]["nlp_node.entity_in"] =
        operator_api::MakeBorrowedOperatorInput(&req);
    operator_api::NamedIoBatch outputs(1);
    outputs[0]["nlp_node.entity_out"] = nullptr;

    const int ret = op.Process(handle, inputs, outputs);
    EXPECT_EQ(ret, 0);
    if (ret != 0) continue;
    ASSERT_EQ(outputs.size(), 1u);
    auto out_sp = outputs[0]["nlp_node.entity_out"];
    ASSERT_NE(out_sp, nullptr);
    auto* out = static_cast<CompanyOperatorEntityOutput*>(out_sp.get());
    EXPECT_EQ(out->request_id, request_id);
    EXPECT_EQ(out->status_code, 0);
    ASSERT_NE(out->entities_json, nullptr);
    std::string json_str(out->entities_json->data, out->entities_json->length);
    const auto entities = nlohmann::json::parse(json_str, nullptr, false);
    EXPECT_TRUE(entities.is_array()) << json_str;
    EXPECT_FALSE(entities.empty()) << json_str;
    std::cout << "  [Operator Real Model Output] " << json_str << std::endl;
  }

  EXPECT_EQ(op.Destroy(handle), 0);
  EXPECT_EQ(op.Deinit(), 0);
}

#ifdef HAVE_WHISPERCPP
// 4. 真实 Whisper 模型音频转录端到端验证
TEST_F(RealModelE2ETest, RealWhisperAsrTranscribe) {
  const auto whisper_path = model_root_ / "ggml-base.bin";
  ASSERT_TRUE(std::filesystem::is_regular_file(whisper_path))
      << "ENABLE_REAL_MODEL_TESTS with HAVE_WHISPERCPP requires pinned "
         "Whisper model; run ./scripts/fetch_real_test_models.sh --whisper";
  const auto audio_path = project_root_ / "data/audio/nav_001.f32";
  ASSERT_TRUE(std::filesystem::is_regular_file(audio_path))
      << "Real audio file not found at " << audio_path;

  ModelLoadSpec spec;
  spec.model_type = "whisper_asr";
  spec.backend_type = "whisper_cpp";
  spec.model_path = whisper_path.string();
  spec.model_config = {
      {"language", "zh"},
      {"max_audio_seconds", 30},
      {"max_output_bytes", 65536},
  };
  spec.backend_config = {{"n_threads", 2}};

  std::string diagnostic;
  auto model = ModelRuntimeFactory::Create(spec, &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  auto asr_model = std::dynamic_pointer_cast<IAsrModel>(model);
  ASSERT_NE(asr_model, nullptr);

  std::ifstream ifs(audio_path, std::ios::binary);
  ASSERT_TRUE(ifs.is_open());
  const auto sz = std::filesystem::file_size(audio_path);
  std::vector<float> pcm(sz / sizeof(float));
  ifs.read(reinterpret_cast<char*>(pcm.data()), sz);

  AudioPcmPayload audio;
  audio.sample_rate = 16000;
  audio.pcm_data = std::move(pcm);

  std::vector<TraceableItem<std::string>> output;
  int ret = asr_model->Transcribe({{5001, 0, std::move(audio)}}, &output);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(output.size(), 1U);
  EXPECT_EQ(output[0].req_id, 5001);
  EXPECT_FALSE(output[0].data.empty());
  EXPECT_TRUE(output[0].data.find("导航") != std::string::npos ||
              output[0].data.find("導航") != std::string::npos);
  std::cout << "\n=================================================="
            << std::endl;
  std::cout << "  [Real Model E2E Whisper] Output: " << output[0].data
            << std::endl;
  std::cout << "=================================================="
            << std::endl;
}
#endif

}  // namespace llm_edgeflow
