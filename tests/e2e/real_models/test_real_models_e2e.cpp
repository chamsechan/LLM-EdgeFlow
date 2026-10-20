#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "edgeflow/operator/interface.h"
#include "engine/backend_registry.h"
#include "nlohmann/json.hpp"
#include "tests/support/mock_text_carrier.h"

namespace llm_edgeflow {
namespace {
namespace fs = std::filesystem;
namespace api = operator_api;
using Json = nlohmann::json;

class OwnedRealOperator {
 public:
  explicit OwnedRealOperator(api::OperatorFunc table) : table_(table) {}
  OwnedRealOperator(const OwnedRealOperator&) = delete;
  OwnedRealOperator& operator=(const OwnedRealOperator&) = delete;
  ~OwnedRealOperator() {
    if (handle_) {
      EXPECT_EQ(Close(), 0) << api::GetOperatorLastError();
    }
  }
  int Open(const std::string& config, const fs::path& root) {
    const auto directory = root.string();
    api::CreateParam params;
    params.cfg_file_name = config.c_str();
    params.model_path = directory.c_str();
    params.compute_platform = api::ComputePlatform::kCpu;
    params.max_frame_depth = 1;
    return table_.Create(&handle_, &params);
  }
  int Close() {
    void* consumed = std::exchange(handle_, nullptr);
    return consumed ? table_.Destroy(consumed) : 0;
  }
  void* get() const { return handle_; }

 private:
  api::OperatorFunc table_;
  void* handle_ = nullptr;
};

class RealModelExecutionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ops_ = api::Get_LLM_EDGEFLOW_OperatorTable();
    ASSERT_EQ(ops_.Init(), 0) << api::GetOperatorLastError();
    initialized_ = true;
  }
  void TearDown() override {
    if (initialized_) {
      EXPECT_EQ(ops_.DeInit(), 0) << api::GetOperatorLastError();
    }
  }
  api::OperatorFunc ops_{};
  bool initialized_ = false;
  fs::path root = LLM_EDGEFLOW_PROJECT_SOURCE_DIR;

  static Json CopyResponse(const api::NamedIoBatch& outputs) {
    const auto* output =
        static_cast<const MockOutput*>(outputs.at(0).at("real.json_out").get());
    EXPECT_NE(output, nullptr);
    if (!output || !output->text || !output->text->value) return Json();
    return Json::parse(output->text->value,
                       output->text->value + output->text->length);
  }

  static std::vector<float> ReadPcm(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(stream)),
                                     std::istreambuf_iterator<char>());
    if (bytes.size() % 4 != 0) return {};
    std::vector<float> pcm;
    pcm.reserve(bytes.size() / 4);
    for (size_t index = 0; index < bytes.size(); index += 4) {
      const uint32_t bits = static_cast<uint32_t>(bytes[index]) |
                            (static_cast<uint32_t>(bytes[index + 1]) << 8) |
                            (static_cast<uint32_t>(bytes[index + 2]) << 16) |
                            (static_cast<uint32_t>(bytes[index + 3]) << 24);
      float sample = 0;
      std::memcpy(&sample, &bits, sizeof(sample));
      pcm.push_back(sample);
    }
    return pcm;
  }
};

TEST_F(RealModelExecutionTest,
       GgufTranslationTraversesPublicOperatorWithActualWeights) {
  if (!BackendRegistry::Instance().Find("llama_cpp")) {
    RecordProperty("optional_runtime_asset", "selected build lacks llama_cpp");
    GTEST_SKIP() << "Selected build does not register llama_cpp";
  }
  const auto weight = root / "configs/qwen2.5-0.5b-instruct-q4_k_m.gguf";
  if (!fs::is_regular_file(weight)) {
    RecordProperty("optional_runtime_asset", weight.string());
    GTEST_SKIP() << "Missing pinned GGUF; use "
                    "scripts/fetch_real_test_models.sh --gguf-only: "
                 << weight;
  }
  OwnedRealOperator instance(ops_);
  ASSERT_EQ(instance.Open("configs/pipeline_translate_cpu.conf", root), 0)
      << api::GetOperatorLastError();
  std::string request = R"({"query":"Hello world."})";
  test_support::MockTextCarrier carrier(request);
  api::NamedIoBatch inputs(1), outputs(1);
  inputs[0]["real.entity_in"] = api::MakeBorrowedOperatorInput(&carrier.root);
  outputs[0]["real.json_out"] = {};
  ASSERT_EQ(ops_.Process(instance.get(), inputs, outputs), 0)
      << api::GetOperatorLastError();
  const auto response = CopyResponse(outputs);
  ASSERT_TRUE(response.is_object());
  ASSERT_TRUE(response.contains("translated"));
  ASSERT_TRUE(response["translated"].is_string());
  EXPECT_FALSE(response["translated"].get<std::string>().empty());
  // This proves physical execution and response transport, not translation
  // quality.
  outputs.clear();
  EXPECT_EQ(instance.Close(), 0) << api::GetOperatorLastError();
}

TEST_F(RealModelExecutionTest,
       WhisperSpeechTraversesPublicOperatorWithActualPcm) {
  if (!BackendRegistry::Instance().Find("whisper_cpp")) {
    RecordProperty("optional_runtime_asset",
                   "selected build lacks whisper_cpp");
    GTEST_SKIP() << "Selected build does not register whisper_cpp";
  }
  const auto weight = root / "configs/ggml-base.bin";
  const auto audio = root / "data/audio/nav_001.f32";
  if (!fs::is_regular_file(weight) || !fs::is_regular_file(audio)) {
    RecordProperty("optional_runtime_asset",
                   weight.string() + ";" + audio.string());
    GTEST_SKIP() << "Missing pinned Whisper weight or speech fixture; use "
                    "scripts/fetch_real_test_models.sh --whisper-e2e";
  }
  const auto pcm = ReadPcm(audio);
  ASSERT_FALSE(pcm.empty());
  OwnedRealOperator instance(ops_);
  ASSERT_EQ(instance.Open("configs/pipeline_audio_asr_intent_cpu.conf", root),
            0)
      << api::GetOperatorLastError();
  MockAudioInput carrier{kMockServiceAudioAsrIntent, pcm.data(),
                         static_cast<int32_t>(pcm.size()), 16000};
  api::NamedIoBatch inputs(1), outputs(1);
  inputs[0]["real.audio_in"] = api::MakeBorrowedOperatorInput(&carrier);
  outputs[0]["real.json_out"] = {};
  ASSERT_EQ(ops_.Process(instance.get(), inputs, outputs), 0)
      << api::GetOperatorLastError();
  const auto response = CopyResponse(outputs);
  ASSERT_TRUE(response.is_object());
  ASSERT_TRUE(response["transcribed_text"].is_string());
  EXPECT_FALSE(response["transcribed_text"].get<std::string>().empty());
  EXPECT_TRUE(response["intent_slot"].is_object());
  outputs.clear();
  EXPECT_EQ(instance.Close(), 0) << api::GetOperatorLastError();
}
}  // namespace
}  // namespace llm_edgeflow
