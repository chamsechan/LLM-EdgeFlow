#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/common_contracts.h"
#include "dev_support/inference/test_capability_models.h"
#include "tests/support/node_harness.h"

namespace llm_edgeflow {

TEST(AsrTranscribeNodeTest, ProcessAudioTranscription) {
  auto model = std::make_shared<test::TestAsrModel>();
  NodeHarness harness("asr_transcribe");
  harness.Config({{"bind_model", "model"}}).BindModel("model", model);
  AudioPcmBatch audio;
  audio.emplace_back(1, 0,
                     AudioPcmPayload(std::vector<float>(16000, 0.1f), 16000));
  harness.CustomInput("audio", audio);

  const auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  const auto* out = result.Output<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 1u);
  EXPECT_EQ((*out)[0].req_id, 1u);
  EXPECT_EQ((*out)[0].sub_id, 0u);
  EXPECT_EQ((*out)[0].data, "transcript:16000:16000");
}

}  // namespace llm_edgeflow
