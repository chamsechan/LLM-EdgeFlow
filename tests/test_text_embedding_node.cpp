#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "adapter/shared_algorithm_runtime.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/session_context.h"
#include "engine/model_interface.h"

namespace alg_framework {

class CountingEmbeddingModel final : public IEmbeddingModel {
 public:
  std::atomic<int> infer_calls{0};
  const std::string& ModelType() const noexcept override {
    static const std::string type = "counting_embedding";
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string capability = "embedding";
    return capability;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  size_t GetMaxBatchSize() const noexcept override { return 4; }

  int Embed(const TextBatch& input_texts, const EmbeddingOptions&,
            EmbeddingBatch* output_embeddings) noexcept override {
    infer_calls++;
    output_embeddings->clear();
    for (const auto& in : input_texts) {
      output_embeddings->emplace_back(in.req_id, in.sub_id,
                                      std::vector<float>(384, 0.1f));
    }
    return 0;
  }
};

class TextEmbeddingNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SharedAlgorithmRuntime::GlobalInit(), 0);
    session_ctx_ = std::make_unique<SessionContext>();
    counting_model_ = std::make_shared<CountingEmbeddingModel>();
    ASSERT_TRUE(session_ctx_->GetModelManager().RegisterModel(
        "embed_model_v1", counting_model_, "revision-1"));
  }
  std::unique_ptr<SessionContext> session_ctx_;
  std::shared_ptr<CountingEmbeddingModel> counting_model_;
};

// 1. Init & Process Request Lifetime
TEST_F(TextEmbeddingNodeTest, ProcessRequestLifetime) {
  auto node = NodeFactory::Instance().Create("TextEmbeddingNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "embed_model_v1"}, {"normalize", true}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch inputs;
  inputs.emplace_back(1, 0, "Query 1");
  inputs.emplace_back(1, 1, "Query 2");
  ctx.Set("text", inputs);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* embeddings = ctx.Get<EmbeddingBatch>("embedding");
  ASSERT_NE(embeddings, nullptr);
  EXPECT_EQ(embeddings->size(), 2u);
  EXPECT_EQ(counting_model_->infer_calls.load(), 1);
}

// 2. Session Caching Single-Flight & Invalidation
TEST_F(TextEmbeddingNodeTest, SessionCachingSingleFlightAndInvalidation) {
  auto node = NodeFactory::Instance().Create("TextEmbeddingNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "embed_model_v1"},
                        {"normalize", true},
                        {"lifetime", "session"}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  constexpr int kNumThreads = 8;
  std::vector<std::thread> threads;
  std::atomic<int> success_count{0};

  for (int i = 0; i < kNumThreads; ++i) {
    (void)i;
    threads.emplace_back([&]() {
      AlgContext ctx;
      TextBatch corpus;
      corpus.emplace_back(100, 0, "Static policy clause 1");
      corpus.emplace_back(100, 1, "Static policy clause 2");
      ctx.Set("text", corpus);

      int ret = node->Process(&ctx);
      if (ret == 0) {
        const auto* out = ctx.Get<EmbeddingBatch>("embedding");
        if (out && out->size() == 2u) {
          success_count++;
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(success_count.load(), kNumThreads);
  EXPECT_EQ(counting_model_->infer_calls.load(), 1);

  // Invalidation test: changing corpus triggers recomputation
  {
    AlgContext ctx;
    TextBatch updated_corpus;
    updated_corpus.emplace_back(100, 0, "Brand new updated policy text");
    ctx.Set("text", updated_corpus);

    int ret = node->Process(&ctx);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(counting_model_->infer_calls.load(), 2);
  }

  // A model hot update changes the revision and invalidates otherwise
  // identical session cache entries.
  ASSERT_TRUE(session_ctx_->GetModelManager().UpdateModelRevision(
      "embed_model_v1", "revision-2"));
  {
    AlgContext ctx;
    TextBatch original_corpus;
    original_corpus.emplace_back(100, 0, "Static policy clause 1");
    original_corpus.emplace_back(100, 1, "Static policy clause 2");
    ctx.Set("text", original_corpus);
    EXPECT_EQ(node->Process(&ctx), 0);
    EXPECT_EQ(counting_model_->infer_calls.load(), 3);
  }
}

// 3. Missing Input Fails Closed
TEST_F(TextEmbeddingNodeTest, MissingInputFailsClosed) {
  auto node = NodeFactory::Instance().Create("TextEmbeddingNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      node->Init({{"bind_model", "embed_model_v1"}}, session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_EQ(node->Process(&empty_ctx), -4101);
}

}  // namespace alg_framework
