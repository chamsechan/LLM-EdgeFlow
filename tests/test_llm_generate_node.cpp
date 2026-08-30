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
#include "tests/support/inference/test_business_models.h"

namespace alg_framework {

class LlmGenerateNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SharedAlgorithmRuntime::GlobalInit(), 0);
    session_ctx_ = std::make_unique<SessionContext>();

    ASSERT_TRUE(session_ctx_->GetModelManager().RegisterModel(
        "llm_model_v1", std::make_shared<test::TestBusinessLlmModel>(2),
        "test-v1"));
  }
  std::unique_ptr<SessionContext> session_ctx_;
};

// 1. Process Batch Prompt Inference
TEST_F(LlmGenerateNodeTest, ProcessBatchPromptInference) {
  auto node = NodeFactory::Instance().Create("LlmGenerateNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "llm_model_v1"},
                        {"temperature", 0.7},
                        {"max_tokens", 128}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch prompts;
  prompts.emplace_back(1, 0, "Explain quantum physics");
  prompts.emplace_back(2, 0, "Summarize article");
  ctx.Set("prompt", prompts);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Get<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 2u);
  EXPECT_FALSE((*out)[0].data.empty());
  EXPECT_FALSE((*out)[1].data.empty());
}

// 2. Missing Prompt Fails Closed
TEST_F(LlmGenerateNodeTest, MissingInputFailsClosed) {
  auto node = NodeFactory::Instance().Create("LlmGenerateNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->Init({{"bind_model", "llm_model_v1"}}, session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_EQ(node->Process(&empty_ctx), -4301);
}

}  // namespace alg_framework
