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
#include "tests/support/node_test_utils.h"

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
    if (return_wrong_count && !output_embeddings->empty()) {
      output_embeddings->pop_back();
    }
    if (corrupt_provenance && output_embeddings->size() > 1) {
      ++(*output_embeddings)[1].sub_id;
    }
    return 0;
  }

  bool return_wrong_count = false;
  bool corrupt_provenance = false;
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
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch inputs;
  inputs.emplace_back(1, 0, "Query 1");
  inputs.emplace_back(1, 1, "Query 2");
  ctx.Publish("text", inputs);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* embeddings = ctx.Read<EmbeddingBatch>("embedding");
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
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

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
      ctx.Publish("text", corpus);

      int ret = node->Process(&ctx);
      if (ret == 0) {
        const auto* out = ctx.Read<EmbeddingBatch>("embedding");
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
    ctx.Publish("text", updated_corpus);

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
    ctx.Publish("text", original_corpus);
    EXPECT_EQ(node->Process(&ctx), 0);
    EXPECT_EQ(counting_model_->infer_calls.load(), 3);
  }
}

// 3. Missing Input Fails Closed
TEST_F(TextEmbeddingNodeTest, MissingInputFailsClosed) {
  auto node = NodeFactory::Instance().Create("TextEmbeddingNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"bind_model", "embed_model_v1"}},
                              session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_EQ(node->Process(&empty_ctx), -4101);
}

TEST_F(TextEmbeddingNodeTest, EmptyBatchSkipsInference) {
  auto node = NodeFactory::Instance().Create("TextEmbeddingNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"bind_model", "embed_model_v1"}},
                              session_ctx_.get()));

  AlgContext ctx;
  ctx.Publish("text", TextBatch{});
  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* output = ctx.Read<EmbeddingBatch>("embedding");
  ASSERT_NE(output, nullptr);
  EXPECT_TRUE(output->empty());
  EXPECT_EQ(counting_model_->infer_calls.load(), 0);
}

TEST_F(TextEmbeddingNodeTest, InvalidRequestOutputFailsClosed) {
  auto node = NodeFactory::Instance().Create("TextEmbeddingNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"bind_model", "embed_model_v1"}},
                              session_ctx_.get()));

  TextBatch inputs = {{8, 0, "first"}, {8, 1, "second"}};

  counting_model_->return_wrong_count = true;
  AlgContext count_ctx;
  count_ctx.Publish("text", inputs);
  EXPECT_EQ(node->Process(&count_ctx), -4102);
  EXPECT_EQ(count_ctx.Read<EmbeddingBatch>("embedding"), nullptr);

  counting_model_->return_wrong_count = false;
  counting_model_->corrupt_provenance = true;
  AlgContext provenance_ctx;
  provenance_ctx.Publish("text", inputs);
  EXPECT_EQ(node->Process(&provenance_ctx), -4103);
  EXPECT_EQ(provenance_ctx.Read<EmbeddingBatch>("embedding"), nullptr);
}

TEST_F(TextEmbeddingNodeTest, InvalidSessionOutputIsNotCached) {
  auto node = NodeFactory::Instance().Create("TextEmbeddingNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(
      *node, {{"bind_model", "embed_model_v1"}, {"lifetime", "session"}},
      session_ctx_.get()));

  TextBatch inputs = {{9, 0, "static first"}, {9, 1, "static second"}};
  counting_model_->return_wrong_count = true;

  AlgContext invalid_ctx;
  invalid_ctx.Publish("text", inputs);
  EXPECT_EQ(node->Process(&invalid_ctx), -4102);
  EXPECT_EQ(counting_model_->infer_calls.load(), 1);

  counting_model_->return_wrong_count = false;
  AlgContext retry_ctx;
  retry_ctx.Publish("text", inputs);
  EXPECT_EQ(node->Process(&retry_ctx), 0);
  EXPECT_NE(retry_ctx.Read<EmbeddingBatch>("embedding"), nullptr);
  EXPECT_EQ(counting_model_->infer_calls.load(), 2);
}

}  // namespace alg_framework
