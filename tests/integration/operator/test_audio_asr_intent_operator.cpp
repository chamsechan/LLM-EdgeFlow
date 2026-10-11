#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "platform_mock/operator_data_types.h"
#include "tests/integration/json_output_test_utils.h"
#include "tests/support/operator_test_fixture.h"

namespace llm_edgeflow {
using test_support::ParseJsonOutput;
using test_support::ScopedTestOperator;

using AudioAsrIntentOperatorTest = test_support::OperatorTestFixture;

TEST_F(AudioAsrIntentOperatorTest, IntentAndSlots) {
  using namespace llm_edgeflow::operator_api;
  ScopedTestOperator instance(ops_);
  ASSERT_EQ(
      instance.Create("demo/fixtures/mock/pipeline_audio_asr_intent.conf"), 0)
      << instance.create_diagnostic();
  ASSERT_NE(instance.get(), nullptr);

  struct AudioCase {
    float amplitude;
    const char* transcript;
    const char* intent;
    nlohmann::json slots;
  };
  const std::vector<AudioCase> cases = {
      {0.05f,
       "帮我导航到清华科技园，避开拥堵路段。",
       "NAVIGATION",
       {{"destination", "清华科技园"},
        {"avoid_traffic", true},
        {"avoid_toll", false}}},
      {0.001f,
       "把空调温度调到24度，风量开到二档。",
       "VEHICLE_HVAC_CONTROL",
       {{"temperature", 24}, {"fan_speed", 2}}},
      {0.005f,
       "今天北京天气怎么样？",
       "GENERAL_VOICE_CMD",
       {{"raw_query", "今天北京天气怎么样？"}}}};
  // Allocate carriers before binding borrowed pointers; keep every row alive.
  std::vector<std::vector<float>> pcm(cases.size());
  std::vector<CompanyOperatorAudioInput> requests(cases.size());
  NamedIoBatch inputs(cases.size());
  NamedIoBatch outputs(cases.size());
  for (size_t i = 0; i < cases.size(); ++i) {
    pcm[i].assign(16000, cases[i].amplitude);
    requests[i] = {kMockServiceAudioAsrIntent, pcm[i].data(),
                   static_cast<int32_t>(pcm[i].size()), 16000};
    inputs[i]["mic_0.audio_in"] = MakeBorrowedOperatorInput(&requests[i]);
    outputs[i]["mic_0.json_out"] = nullptr;
  }
  ASSERT_EQ(ops_.Process(instance.get(), inputs, outputs), 0)
      << GetOperatorLastError();
  ASSERT_EQ(outputs.size(), cases.size());
  for (size_t i = 0; i < cases.size(); ++i) {
    SCOPED_TRACE(cases[i].intent);
    const auto& lease = outputs[i].at("mic_0.json_out");
    ASSERT_NE(lease, nullptr);
    const auto* output = static_cast<const CompanyOperatorOutput*>(lease.get());
    EXPECT_EQ(output->service_type, kMockServiceAudioAsrIntent);
    EXPECT_EQ(output->status_code, 0);
    const auto response = ParseJsonOutput(output);
    EXPECT_EQ(response.at("transcribed_text"), cases[i].transcript);
    const auto& intent_slot = response.at("intent_slot");
    EXPECT_EQ(intent_slot.at("intent"), cases[i].intent);
    const auto& slots = intent_slot.at("slots");
    for (const auto& expected : cases[i].slots.items())
      EXPECT_EQ(slots.at(expected.key()), expected.value()) << expected.key();
  }
  outputs.clear();
  EXPECT_EQ(instance.Close(), 0) << instance.close_diagnostic();
}

TEST_F(AudioAsrIntentOperatorTest, NormalThenEmptyAudioOnSameHandle) {
  ScopedTestOperator instance(ops_);
  ASSERT_EQ(
      instance.Create("demo/fixtures/mock/pipeline_audio_asr_intent.conf"), 0)
      << instance.create_diagnostic();
  ASSERT_NE(instance.get(), nullptr);

  // 构造两个模拟的音频 PCM 浮点信号
  std::vector<float> pcm1(16000, 0.01f);   // 导航语音 (累计值较大)
  std::vector<float> pcm2(16000, 0.001f);  // 空调车控语音

  CompanyOperatorAudioInput in_audio1{kMockServiceAudioAsrIntent, pcm1.data(),
                                      static_cast<int32_t>(pcm1.size()), 16000};
  CompanyOperatorAudioInput in_audio2{kMockServiceAudioAsrIntent, pcm2.data(),
                                      static_cast<int32_t>(pcm2.size()), 16000};

  operator_api::NamedIoBatch inputs(2);
  inputs[0]["mic_0.audio_in"] =
      operator_api::MakeBorrowedOperatorInput(&in_audio1);
  inputs[1]["mic_0.audio_in"] =
      operator_api::MakeBorrowedOperatorInput(&in_audio2);

  operator_api::NamedIoBatch outputs(2);
  outputs[0]["mic_0.json_out"] = nullptr;
  outputs[1]["mic_0.json_out"] = nullptr;

  const int ret = ops_.Process(instance.get(), inputs, outputs);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(outputs.size(), 2u);

  auto out1_sp = outputs[0]["mic_0.json_out"];
  auto out2_sp = outputs[1]["mic_0.json_out"];
  ASSERT_NE(out1_sp, nullptr);
  ASSERT_NE(out2_sp, nullptr);

  auto* out1 = static_cast<CompanyOperatorOutput*>(out1_sp.get());
  const auto out1_json = ParseJsonOutput(out1);
  auto* out2 = static_cast<CompanyOperatorOutput*>(out2_sp.get());
  const auto out2_json = ParseJsonOutput(out2);

  ASSERT_TRUE(out1_json.contains("intent_slot"));
  const auto& j1 = out1_json.at("intent_slot");

  ASSERT_TRUE(out2_json.contains("intent_slot"));
  const auto& j2 = out2_json.at("intent_slot");

  EXPECT_EQ(j1["intent"], "NAVIGATION");
  EXPECT_EQ(j2["intent"], "VEHICLE_HVAC_CONTROL");

  CompanyOperatorAudioInput empty1{kMockServiceAudioAsrIntent, nullptr, 0,
                                   16000};
  CompanyOperatorAudioInput empty2{kMockServiceAudioAsrIntent, nullptr, 0,
                                   16000};
  inputs[0]["mic_0.audio_in"] =
      operator_api::MakeBorrowedOperatorInput(&empty1);
  inputs[1]["mic_0.audio_in"] =
      operator_api::MakeBorrowedOperatorInput(&empty2);
  outputs[0]["mic_0.json_out"] = nullptr;
  outputs[1]["mic_0.json_out"] = nullptr;

  EXPECT_EQ(ops_.Process(instance.get(), inputs, outputs), 0);
  out1_sp = outputs[0]["mic_0.json_out"];
  out2_sp = outputs[1]["mic_0.json_out"];
  ASSERT_NE(out1_sp, nullptr);
  ASSERT_NE(out2_sp, nullptr);
  out1 = static_cast<CompanyOperatorOutput*>(out1_sp.get());
  out2 = static_cast<CompanyOperatorOutput*>(out2_sp.get());
  EXPECT_EQ(out1->status_code, 0);
  EXPECT_EQ(out2->status_code, 0);

  out1_sp.reset();
  out2_sp.reset();
  outputs.clear();
  EXPECT_EQ(instance.Close(), 0) << instance.close_diagnostic();
}

}  // namespace llm_edgeflow
