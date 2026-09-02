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
#include "engine/model_interface.h"
#include "tests/support/node_test_utils.h"

namespace alg_framework {

namespace {

class ContractLlmModel final : public ILlmModel {
 public:
  const std::string& ModelType() const noexcept override {
    static const std::string type = "contract_llm";
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string capability = "llm";
    return capability;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  size_t GetMaxBatchSize() const noexcept override { return 4; }

  int Generate(const TextBatch& prompts, const GenerateOptions& options,
               TextBatch* outputs) noexcept override {
    if (!outputs) return -1;
    ++infer_calls;
    last_options = options;
    outputs->clear();
    for (const auto& prompt : prompts) {
      outputs->emplace_back(prompt.req_id, prompt.sub_id,
                            "generated:" + prompt.data);
    }
    if (return_wrong_count && !outputs->empty()) {
      outputs->pop_back();
    }
    if (corrupt_provenance && outputs->size() > 1) {
      ++(*outputs)[1].sub_id;
    }
    return 0;
  }

  int infer_calls = 0;
  GenerateOptions last_options;
  bool return_wrong_count = false;
  bool corrupt_provenance = false;
};

}  // namespace

class LlmGenerateNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SharedAlgorithmRuntime::GlobalInit(), 0);
    session_ctx_ = std::make_unique<SessionContext>();

    model_ = std::make_shared<ContractLlmModel>();
    ASSERT_TRUE(session_ctx_->GetModelManager().RegisterModel(
        "llm_model_v1", model_, "test-v1"));
  }
  std::unique_ptr<SessionContext> session_ctx_;
  std::shared_ptr<ContractLlmModel> model_;
};

// 1. Process Batch Prompt Inference
TEST_F(LlmGenerateNodeTest, ProcessBatchPromptInference) {
  auto node = NodeFactory::Instance().Create("LlmGenerateNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "llm_model_v1"},
                        {"temperature", 0.7},
                        {"max_tokens", 128},
                        {"top_k", 32},
                        {"top_p", 0.8},
                        {"repetition_penalty", 1.15},
                        {"stop_words", {"END"}}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch prompts;
  prompts.emplace_back(1, 0, "Explain quantum physics");
  prompts.emplace_back(2, 0, "Summarize article");
  ctx.Publish("prompt", prompts);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Read<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 2u);
  EXPECT_FALSE((*out)[0].data.empty());
  EXPECT_FALSE((*out)[1].data.empty());
  EXPECT_EQ((*out)[0].req_id, 1U);
  EXPECT_EQ((*out)[1].req_id, 2U);
  EXPECT_EQ(model_->last_options.top_k, 32);
  EXPECT_FLOAT_EQ(model_->last_options.top_p, 0.8f);
  EXPECT_FLOAT_EQ(model_->last_options.repetition_penalty, 1.15f);
  EXPECT_EQ(model_->last_options.stop_words, std::vector<std::string>({"END"}));
}

TEST_F(LlmGenerateNodeTest, RejectsInvalidUnifiedGenerationOptions) {
  auto node = NodeFactory::Instance().Create("LlmGenerateNode");
  ASSERT_NE(node, nullptr);
  EXPECT_FALSE(InitNodeForTest(*node,
                               {{"bind_model", "llm_model_v1"}, {"top_k", -1}},
                               session_ctx_.get()));

  node = NodeFactory::Instance().Create("LlmGenerateNode");
  ASSERT_NE(node, nullptr);
  EXPECT_FALSE(InitNodeForTest(
      *node, {{"bind_model", "llm_model_v1"}, {"repetition_penalty", 0.0}},
      session_ctx_.get()));
}

// 2. Missing Prompt Fails Closed
TEST_F(LlmGenerateNodeTest, MissingInputFailsClosed) {
  auto node = NodeFactory::Instance().Create("LlmGenerateNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"bind_model", "llm_model_v1"}},
                              session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_EQ(node->Process(&empty_ctx), -4301);
}

TEST_F(LlmGenerateNodeTest, EmptyBatchSkipsInference) {
  auto node = NodeFactory::Instance().Create("LlmGenerateNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"bind_model", "llm_model_v1"}},
                              session_ctx_.get()));

  AlgContext ctx;
  ctx.Publish("prompt", TextBatch{});
  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* output = ctx.Read<TextBatch>("text");
  ASSERT_NE(output, nullptr);
  EXPECT_TRUE(output->empty());
  EXPECT_EQ(model_->infer_calls, 0);
}

TEST_F(LlmGenerateNodeTest, InvalidModelOutputFailsClosed) {
  auto node = NodeFactory::Instance().Create("LlmGenerateNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"bind_model", "llm_model_v1"}},
                              session_ctx_.get()));

  TextBatch prompts = {{3, 0, "first"}, {3, 1, "second"}};

  model_->return_wrong_count = true;
  AlgContext count_ctx;
  count_ctx.Publish("prompt", prompts);
  EXPECT_EQ(node->Process(&count_ctx), -4302);
  EXPECT_EQ(count_ctx.Read<TextBatch>("text"), nullptr);

  model_->return_wrong_count = false;
  model_->corrupt_provenance = true;
  AlgContext provenance_ctx;
  provenance_ctx.Publish("prompt", prompts);
  EXPECT_EQ(node->Process(&provenance_ctx), -4303);
  EXPECT_EQ(provenance_ctx.Read<TextBatch>("text"), nullptr);
}

}  // namespace alg_framework
