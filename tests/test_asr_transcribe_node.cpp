#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapter/shared_algorithm_runtime.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/session_context.h"
#include "tests/support/inference/test_capability_models.h"
#include "tests/support/node_test_utils.h"

namespace alg_framework {

class AsrTranscribeNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SharedAlgorithmRuntime::GlobalInit(), 0);
    session_ctx_ = std::make_unique<SessionContext>();

    asr_model_ = std::make_shared<test::TestAsrModel>();
    ASSERT_TRUE(session_ctx_->GetModelManager().RegisterModel(
        "asr_model_v1", asr_model_, "test-revision", "test_asr_model", "asr",
        "test_tensor_backend"));
  }
  std::unique_ptr<SessionContext> session_ctx_;
  std::shared_ptr<test::TestAsrModel> asr_model_;
};

// 1. Process Audio Transcription
TEST_F(AsrTranscribeNodeTest, ProcessAudioTranscription) {
  auto node = NodeFactory::Instance().Create("AsrTranscribeNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "asr_model_v1"}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  AudioPcmBatch audio;
  audio.emplace_back(1, 0,
                     AudioPcmPayload(std::vector<float>(16000, 0.1f), 16000));
  ctx.Publish("audio", audio);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Read<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 1u);
  EXPECT_EQ((*out)[0].req_id, 1u);
  EXPECT_EQ((*out)[0].sub_id, 0u);
  EXPECT_EQ((*out)[0].data, "transcript:16000:16000");
}

// 2. Empty Audio Yields Empty Transcript
TEST_F(AsrTranscribeNodeTest, EmptyAudioInput) {
  auto node = NodeFactory::Instance().Create("AsrTranscribeNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"bind_model", "asr_model_v1"}},
                              session_ctx_.get()));

  AlgContext ctx;
  ctx.Publish("audio", AudioPcmBatch{});

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Read<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  EXPECT_TRUE(out->empty());
}

// 3. Missing Audio Fails Closed
TEST_F(AsrTranscribeNodeTest, MissingInputFailsClosed) {
  auto node = NodeFactory::Instance().Create("AsrTranscribeNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"bind_model", "asr_model_v1"}},
                              session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_EQ(node->Process(&empty_ctx), -7001);
}

TEST_F(AsrTranscribeNodeTest, InvalidModelOutputFailsClosed) {
  auto node = NodeFactory::Instance().Create("AsrTranscribeNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"bind_model", "asr_model_v1"}},
                              session_ctx_.get()));

  AudioPcmBatch audio;
  audio.emplace_back(9, 3, AudioPcmPayload({0.1f, 0.2f}, 8000));

  AlgContext count_ctx;
  count_ctx.Publish("audio", audio);
  asr_model_->return_wrong_count_ = true;
  EXPECT_EQ(node->Process(&count_ctx), -7002);

  asr_model_->return_wrong_count_ = false;
  asr_model_->corrupt_provenance_ = true;
  AlgContext provenance_ctx;
  provenance_ctx.Publish("audio", audio);
  EXPECT_EQ(node->Process(&provenance_ctx), -7003);
}

}  // namespace alg_framework
