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
#include "core/pipeline_validator.h"
#include "core/session_context.h"
#include "engine/model_interface.h"
#include "tests/support/node_test_utils.h"

namespace llm_edgeflow {

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
      output_embeddings->emplace_back(
          in.req_id, in.sub_id,
          std::vector<float>(384, static_cast<float>(in.data.size())));
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
  auto node = NodeRegistry::Instance().Create("TextEmbeddingNode");
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
  auto node = NodeRegistry::Instance().Create("TextEmbeddingNode");
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
  auto node = NodeRegistry::Instance().Create("TextEmbeddingNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"bind_model", "embed_model_v1"}},
                              session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_EQ(node->Process(&empty_ctx), -4101);
}

TEST_F(TextEmbeddingNodeTest, EmptyBatchSkipsInference) {
  auto node = NodeRegistry::Instance().Create("TextEmbeddingNode");
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
  auto node = NodeRegistry::Instance().Create("TextEmbeddingNode");
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
  auto node = NodeRegistry::Instance().Create("TextEmbeddingNode");
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

// 4. Session Cache Collision Reproduction Defeated (C01)
TEST_F(TextEmbeddingNodeTest, SessionCacheCollisionReproductionDefeated) {
  auto node_a = NodeRegistry::Instance().Create("TextEmbeddingNode");
  auto node_b = NodeRegistry::Instance().Create("TextEmbeddingNode");
  ASSERT_NE(node_a, nullptr);
  ASSERT_NE(node_b, nullptr);
  ASSERT_TRUE(InitNodeForTest(
      *node_a, {{"bind_model", "embed_model_v1"}, {"lifetime", "session"}},
      session_ctx_.get()));
  ASSERT_TRUE(InitNodeForTest(
      *node_b, {{"bind_model", "embed_model_v1"}, {"lifetime", "session"}},
      session_ctx_.get()));

  // Corpus A: ["a", "b\0\1c"]
  // Corpus B: ["a\0\1b", "c"]
  std::string s_b_nul_c = std::string("b") + '\0' + '\1' + "c";
  std::string s_a_nul_b = std::string("a") + '\0' + '\1' + "b";
  ASSERT_EQ(s_b_nul_c.size(), 4u);
  ASSERT_EQ(s_a_nul_b.size(), 4u);

  TextBatch corpus_a;
  corpus_a.emplace_back(0, 0, "a");
  corpus_a.emplace_back(0, 1, s_b_nul_c);

  TextBatch corpus_b;
  corpus_b.emplace_back(0, 0, s_a_nul_b);
  corpus_b.emplace_back(0, 1, "c");

  AlgContext ctx_a;
  ctx_a.Publish("text", corpus_a);
  EXPECT_EQ(node_a->Process(&ctx_a), 0);
  EXPECT_EQ(counting_model_->infer_calls.load(), 1);
  const auto* out_a = ctx_a.Read<EmbeddingBatch>("embedding");
  ASSERT_NE(out_a, nullptr);
  ASSERT_EQ(out_a->size(), 2u);
  EXPECT_FLOAT_EQ((*out_a)[0].data[0], 1.0f);
  EXPECT_FLOAT_EQ((*out_a)[1].data[0], 4.0f);

  AlgContext ctx_b;
  ctx_b.Publish("text", corpus_b);
  EXPECT_EQ(node_b->Process(&ctx_b), 0);
  // In the old implementation, corpus_b collided with corpus_a, returning
  // cached [1.0f, 4.0f] with infer_calls staying at 1. With the fix,
  // infer_calls must be 2 and out_b has [4.0f, 1.0f]!
  EXPECT_EQ(counting_model_->infer_calls.load(), 2);
  const auto* out_b = ctx_b.Read<EmbeddingBatch>("embedding");
  ASSERT_NE(out_b, nullptr);
  ASSERT_EQ(out_b->size(), 2u);
  EXPECT_FLOAT_EQ((*out_b)[0].data[0], 4.0f);
  EXPECT_FLOAT_EQ((*out_b)[1].data[0], 1.0f);

  // Subsequent call with corpus_a should hit cache (infer_calls remains 2)
  AlgContext ctx_a2;
  ctx_a2.Publish("text", corpus_a);
  EXPECT_EQ(node_a->Process(&ctx_a2), 0);
  EXPECT_EQ(counting_model_->infer_calls.load(), 2);
  const auto* out_a2 = ctx_a2.Read<EmbeddingBatch>("embedding");
  ASSERT_NE(out_a2, nullptr);
  EXPECT_FLOAT_EQ((*out_a2)[0].data[0], 1.0f);
  EXPECT_FLOAT_EQ((*out_a2)[1].data[0], 4.0f);

  // Changing order, sub_id, req_id, or normalize option creates distinct
  // entries
  TextBatch corpus_a_reordered;
  corpus_a_reordered.emplace_back(0, 0, s_b_nul_c);
  corpus_a_reordered.emplace_back(0, 1, "a");
  AlgContext ctx_reorder;
  ctx_reorder.Publish("text", corpus_a_reordered);
  EXPECT_EQ(node_a->Process(&ctx_reorder), 0);
  EXPECT_EQ(counting_model_->infer_calls.load(), 3);

  TextBatch corpus_a_sub_id;
  corpus_a_sub_id.emplace_back(0, 5, "a");
  corpus_a_sub_id.emplace_back(0, 6, s_b_nul_c);
  AlgContext ctx_sub_id;
  ctx_sub_id.Publish("text", corpus_a_sub_id);
  EXPECT_EQ(node_a->Process(&ctx_sub_id), 0);
  EXPECT_EQ(counting_model_->infer_calls.load(), 4);

  // Different normalize option creates distinct cache entry
  auto node_no_norm = NodeRegistry::Instance().Create("TextEmbeddingNode");
  ASSERT_TRUE(InitNodeForTest(*node_no_norm,
                              {{"bind_model", "embed_model_v1"},
                               {"lifetime", "session"},
                               {"normalize", false}},
                              session_ctx_.get()));
  AlgContext ctx_no_norm;
  ctx_no_norm.Publish("text", corpus_a);
  EXPECT_EQ(node_no_norm->Process(&ctx_no_norm), 0);
  EXPECT_EQ(counting_model_->infer_calls.load(), 5);

  corpus_a[0].req_id = 7;
  AlgContext changed_request;
  changed_request.Publish("text", corpus_a);
  ASSERT_EQ(node_a->Process(&changed_request), 0);
  EXPECT_EQ(counting_model_->infer_calls.load(), 6);
  EXPECT_EQ(changed_request.Read<EmbeddingBatch>("embedding")->front().req_id,
            7U);
}

TEST_F(TextEmbeddingNodeTest, StrictPlanKeepsDistinctCorpusCacheIdentities) {
  const auto config = nlohmann::json::parse(R"json({
  "biz_name": "keyword_match_v1",
  "models": [
    {
      "model_id": "embed_model_v1",
      "model_type": "bge_embedding",
      "capability": "embedding",
      "backend": "onnxruntime",
      "model_path": "demo/fixtures/mock/artifacts/neutral-embedding.fixture",
      "model_config": {
        "embedding_dim": 1
      },
      "backend_config": {}
    }
  ],
  "pipeline": [
    {
      "id": "source_a",
      "node_type": "TextCorpusSourceNode",
      "depends_on": [],
      "ports": {
        "outputs": {
          "corpus": "corpus_a"
        }
      },
      "config": {
        "corpus": [
          "a",
          "b\u0000\u0001c",
          "",
          "中文"
        ]
      }
    },
    {
      "id": "embed_a",
      "node_type": "TextEmbeddingNode",
      "depends_on": [
        "source_a"
      ],
      "ports": {
        "inputs": {
          "text": "corpus_a"
        },
        "outputs": {
          "embedding": "vectors_a"
        }
      },
      "config": {
        "bind_model": "embed_model_v1",
        "lifetime": "session"
      }
    },
    {
      "id": "source_b",
      "node_type": "TextCorpusSourceNode",
      "depends_on": [],
      "ports": {
        "outputs": {
          "corpus": "corpus_b"
        }
      },
      "config": {
        "corpus": [
          "a\u0000\u0001b",
          "c",
          "",
          "中文"
        ]
      }
    },
    {
      "id": "embed_b",
      "node_type": "TextEmbeddingNode",
      "depends_on": [
        "source_b"
      ],
      "ports": {
        "inputs": {
          "text": "corpus_b"
        },
        "outputs": {
          "embedding": "vectors_b"
        }
      },
      "config": {
        "bind_model": "embed_model_v1",
        "lifetime": "session"
      }
    },
    {
      "id": "rules",
      "node_type": "TextRuleMatchNode",
      "depends_on": [],
      "ports": {
        "inputs": {
          "text": "input_sentences"
        },
        "outputs": {
          "matches": "rule_matches"
        }
      },
      "config": {}
    }
  ]
})json");
  auto plan = PipelineValidator::ValidateAndPlan(config);
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump();
  for (int request = 0; request < 2; ++request) {
    AlgContext ctx;
    ctx.Publish("input_sentences", TextBatch{{1, 0, "probe"}});
    for (const auto& id : plan.topological_order) {
      const auto& node_plan = plan.node_plans.at(id);
      auto node = NodeRegistry::Instance().Create(node_plan.node.node_type);
      ASSERT_NE(node, nullptr);
      ASSERT_TRUE(node->Init(
          {&node_plan, &node_plan.normalized_config, session_ctx_.get()}));
      ASSERT_EQ(node->Process(&ctx), 0);
    }
    for (const char* key : {"a", "b"}) {
      const auto* corpus = ctx.Read<TextBatch>(std::string("corpus_") + key);
      const auto* vectors =
          ctx.Read<EmbeddingBatch>(std::string("vectors_") + key);
      ASSERT_NE(corpus, nullptr);
      ASSERT_NE(vectors, nullptr);
      ASSERT_EQ(vectors->size(), corpus->size());
      for (size_t i = 0; i < corpus->size(); ++i) {
        EXPECT_FLOAT_EQ((*vectors)[i].data[0], (*corpus)[i].data.size());
        EXPECT_EQ((*vectors)[i].sub_id, (*corpus)[i].sub_id);
      }
    }
    EXPECT_EQ(counting_model_->infer_calls.load(), 2);
  }
}

}  // namespace llm_edgeflow
