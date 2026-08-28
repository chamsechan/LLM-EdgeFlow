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
#include "engine/engine_interface.h"
#include "engine/engine_registry.h"

namespace alg_framework {

class AsrTranscribeNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SharedAlgorithmRuntime::GlobalInit(), 0);
    session_ctx_ = std::make_unique<SessionContext>();

    auto asr_engine = EngineFactory::Instance().Create("mock_npu_asr");
    ASSERT_NE(asr_engine, nullptr);
    asr_engine->Load("./models/paraformer.bin", {{"max_batch_size", 2}});
    ASSERT_TRUE(session_ctx_->GetModelManager().RegisterModel(
        "mock_asr_model", std::move(asr_engine)));
  }
  std::unique_ptr<SessionContext> session_ctx_;
};

// 1. Process Audio Transcription
TEST_F(AsrTranscribeNodeTest, ProcessAudioTranscription) {
  auto node = NodeFactory::Instance().Create("AsrTranscribeNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "mock_asr_model"}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  AudioPcmBatch audio;
  audio.emplace_back(1, 0,
                     AudioPcmPayload(std::vector<float>(16000, 0.1f), 16000));
  ctx.Set("audio", audio);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Get<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 1u);
  EXPECT_FALSE((*out)[0].data.empty());
}

// 2. Empty Audio Yields Empty Transcript
TEST_F(AsrTranscribeNodeTest, EmptyAudioInput) {
  auto node = NodeFactory::Instance().Create("AsrTranscribeNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      node->Init({{"bind_model", "mock_asr_model"}}, session_ctx_.get()));

  AlgContext ctx;
  ctx.Set("audio", AudioPcmBatch{});

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Get<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  EXPECT_TRUE(out->empty());
}

// 3. Missing Audio Fails Closed
TEST_F(AsrTranscribeNodeTest, MissingInputFailsClosed) {
  auto node = NodeFactory::Instance().Create("AsrTranscribeNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      node->Init({{"bind_model", "mock_asr_model"}}, session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_NE(node->Process(&empty_ctx), 0);
}

}  // namespace alg_framework
