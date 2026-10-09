#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline_validator.h"
#include "core/session_context.h"
#include "engine/model_interface.h"
#include "nodes/node_error_codes.h"
#include "tests/support/model_registration.h"
#include "tests/support/node_test_utils.h"
#include "tests/support/pipeline_test_utils.h"

namespace llm_edgeflow {

class CountingEmbeddingModel final : public IEmbeddingModel {
 public:
  std::atomic<int> infer_calls{0};
  const std::string& ImplName() const noexcept override {
    static const std::string type = "counting_embedding";
    return type;
  }
  const std::string& ModelType() const noexcept override {
    static const std::string model_type = "embedding";
    return model_type;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }

  int Embed(const TextBatch& input_texts, EmbeddingBatch* output_embeddings,
            std::string* diagnostic = nullptr) noexcept override {
    if (diagnostic) diagnostic->clear();
    infer_calls++;
    output_embeddings->clear();
    if (fail_inference) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if (diagnostic) *diagnostic = "embedding backend unavailable";
      return -731;
    }
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

  bool fail_inference = false;
  bool return_wrong_count = false;
  bool corrupt_provenance = false;
};

class TextEmbeddingNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    session_ctx_ = std::make_unique<SessionContext>();
    counting_model_ = std::make_shared<CountingEmbeddingModel>();
    ASSERT_TRUE(RegisterTestModel(session_ctx_->GetModelManager(),
                                  "embed_model", counting_model_,
                                  "revision-1"));
  }
  std::unique_ptr<SessionContext> session_ctx_;
  std::shared_ptr<CountingEmbeddingModel> counting_model_;
};

TEST_F(TextEmbeddingNodeTest, ProcessRequestLifetime) {
  auto node = NodeRegistry::Instance().Create("text_embedding");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "embed_model"}};
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

TEST_F(TextEmbeddingNodeTest, SessionCachingSingleFlightAndInvalidation) {
  auto node = NodeRegistry::Instance().Create("text_embedding");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "embed_model"}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get(), nullptr, {},
                              {{"text", "session"}}));

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

  // 失效测试：更换语料会触发重新计算
  {
    AlgContext ctx;
    TextBatch updated_corpus;
    updated_corpus.emplace_back(100, 0, "Brand new updated policy text");
    ctx.Publish("text", updated_corpus);

    int ret = node->Process(&ctx);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(counting_model_->infer_calls.load(), 2);
  }

  // 模型热更新会改变 revision，使其余完全相同的会话缓存条目失效。
  ASSERT_TRUE(session_ctx_->GetModelManager().UpdateModelRevision(
      "embed_model", "revision-2"));
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

TEST_F(TextEmbeddingNodeTest, MissingInputFailsClosed) {
  auto node = NodeRegistry::Instance().Create("text_embedding");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"bind_model", "embed_model"}},
                              session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_EQ(node->Process(&empty_ctx), node_error::author_node::kMissingInput);
}

TEST_F(TextEmbeddingNodeTest, EmptyBatchSkipsInference) {
  auto node = NodeRegistry::Instance().Create("text_embedding");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"bind_model", "embed_model"}},
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
  auto node = NodeRegistry::Instance().Create("text_embedding");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"bind_model", "embed_model"}},
                              session_ctx_.get()));

  TextBatch inputs = {{8, 0, "first"}, {8, 1, "second"}};

  counting_model_->return_wrong_count = true;
  AlgContext count_ctx;
  count_ctx.Publish("text", inputs);
  EXPECT_EQ(node->Process(&count_ctx),
            node_error::author_node::kOutputCountMismatch);
  EXPECT_EQ(count_ctx.Read<EmbeddingBatch>("embedding"), nullptr);

  counting_model_->return_wrong_count = false;
  counting_model_->corrupt_provenance = true;
  AlgContext provenance_ctx;
  provenance_ctx.Publish("text", inputs);
  EXPECT_EQ(node->Process(&provenance_ctx),
            node_error::author_node::kOutputProvenanceMismatch);
  EXPECT_EQ(provenance_ctx.Read<EmbeddingBatch>("embedding"), nullptr);
}

TEST_F(TextEmbeddingNodeTest, InvalidSessionOutputIsNotCached) {
  auto node = NodeRegistry::Instance().Create("text_embedding");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"bind_model", "embed_model"}},
                              session_ctx_.get(), nullptr, {},
                              {{"text", "session"}}));

  TextBatch inputs = {{9, 0, "static first"}, {9, 1, "static second"}};
  counting_model_->return_wrong_count = true;

  AlgContext invalid_ctx;
  invalid_ctx.Publish("text", inputs);
  EXPECT_EQ(node->Process(&invalid_ctx),
            node_error::author_node::kOutputCountMismatch);
  EXPECT_EQ(counting_model_->infer_calls.load(), 1);

  counting_model_->return_wrong_count = false;
  AlgContext retry_ctx;
  retry_ctx.Publish("text", inputs);
  EXPECT_EQ(node->Process(&retry_ctx), 0);
  EXPECT_NE(retry_ctx.Read<EmbeddingBatch>("embedding"), nullptr);
  EXPECT_EQ(counting_model_->infer_calls.load(), 2);
}

TEST_F(TextEmbeddingNodeTest,
       ConcurrentModelFailuresReachEveryRequestAndRetry) {
  for (const char* lifetime : {"request", "session"}) {
    SCOPED_TRACE(lifetime);
    auto node = NodeRegistry::Instance().Create("text_embedding");
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(InitNodeForTest(*node, {{"bind_model", "embed_model"}},
                                session_ctx_.get(), nullptr, {},
                                {{"text", lifetime}}));
    counting_model_->fail_inference = true;
    const int calls_before = counting_model_->infer_calls.load();
    std::promise<void> start;
    const auto ready = start.get_future().share();
    struct Result {
      int code;
      int context_code;
      std::string message;
      bool has_output;
    };
    std::vector<std::future<Result>> workers;
    for (int i = 0; i < 8; ++i) {
      workers.push_back(std::async(std::launch::async, [&] {
        if (ready.wait_for(std::chrono::seconds(5)) !=
            std::future_status::ready)
          return Result{-1, -1, "start timeout", false};
        AlgContext context;
        context.Publish("text", TextBatch{{9, 0, "same session corpus"}});
        const int code = node->Process(&context);
        return Result{code, context.GetErrorCode(), context.GetErrorMessage(),
                      context.Read<EmbeddingBatch>("embedding") != nullptr};
      }));
    }
    start.set_value();
    for (auto& worker : workers) {
      ASSERT_EQ(worker.wait_for(std::chrono::seconds(5)),
                std::future_status::ready);
      const auto result = worker.get();
      EXPECT_EQ(result.code, -731);
      EXPECT_EQ(result.context_code, -731);
      EXPECT_NE(result.message.find("embedding backend unavailable"),
                std::string::npos);
      EXPECT_FALSE(result.has_output);
    }
    if (std::string(lifetime) == "session") {
      EXPECT_LT(counting_model_->infer_calls.load() - calls_before, 8);
    }
    // 修改模型行为前，所有 worker 都已 join。
    counting_model_->fail_inference = false;
    const int calls_before_retry = counting_model_->infer_calls.load();
    AlgContext retry;
    retry.Publish("text", TextBatch{{9, 0, "same session corpus"}});
    ASSERT_EQ(node->Process(&retry), 0);
    EXPECT_NE(retry.Read<EmbeddingBatch>("embedding"), nullptr);
    EXPECT_EQ(counting_model_->infer_calls.load(), calls_before_retry + 1);
    if (std::string(lifetime) == "session") {
      AlgContext cached;
      cached.Publish("text", TextBatch{{9, 0, "same session corpus"}});
      ASSERT_EQ(node->Process(&cached), 0);
      EXPECT_NE(cached.Read<EmbeddingBatch>("embedding"), nullptr);
      EXPECT_EQ(counting_model_->infer_calls.load(), calls_before_retry + 1);
    }
  }
}

TEST_F(TextEmbeddingNodeTest, SessionCacheCollisionReproductionDefeated) {
  auto node_a = NodeRegistry::Instance().Create("text_embedding");
  auto node_b = NodeRegistry::Instance().Create("text_embedding");
  ASSERT_NE(node_a, nullptr);
  ASSERT_NE(node_b, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node_a, {{"bind_model", "embed_model"}},
                              session_ctx_.get(), nullptr, {},
                              {{"text", "session"}}));
  ASSERT_TRUE(InitNodeForTest(*node_b, {{"bind_model", "embed_model"}},
                              session_ctx_.get(), nullptr, {},
                              {{"text", "session"}}));

  // 语料 A：["a", "b\0\1c"]
  // 语料 B：["a\0\1b", "c"]
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
  // 旧实现中 corpus_b 会与 corpus_a 冲突，返回缓存的 [1.0f, 4.0f]，
  // infer_calls 仍为 1。修复后 infer_calls 必须为 2，out_b 为 [4.0f, 1.0f]！
  EXPECT_EQ(counting_model_->infer_calls.load(), 2);
  const auto* out_b = ctx_b.Read<EmbeddingBatch>("embedding");
  ASSERT_NE(out_b, nullptr);
  ASSERT_EQ(out_b->size(), 2u);
  EXPECT_FLOAT_EQ((*out_b)[0].data[0], 4.0f);
  EXPECT_FLOAT_EQ((*out_b)[1].data[0], 1.0f);

  // 随后用 corpus_a 调用应命中缓存 (infer_calls 仍为 2)
  AlgContext ctx_a2;
  ctx_a2.Publish("text", corpus_a);
  EXPECT_EQ(node_a->Process(&ctx_a2), 0);
  EXPECT_EQ(counting_model_->infer_calls.load(), 2);
  const auto* out_a2 = ctx_a2.Read<EmbeddingBatch>("embedding");
  ASSERT_NE(out_a2, nullptr);
  EXPECT_FLOAT_EQ((*out_a2)[0].data[0], 1.0f);
  EXPECT_FLOAT_EQ((*out_a2)[1].data[0], 4.0f);

  // 改变顺序、sub_id 或 req_id 都会产生不同的条目
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

  corpus_a[0].req_id = 7;
  AlgContext changed_request;
  changed_request.Publish("text", corpus_a);
  ASSERT_EQ(node_a->Process(&changed_request), 0);
  EXPECT_EQ(counting_model_->infer_calls.load(), 5);
  EXPECT_EQ(changed_request.Read<EmbeddingBatch>("embedding")->front().req_id,
            7U);
}

TEST_F(TextEmbeddingNodeTest, StrictPlanKeepsDistinctCorpusCacheIdentities) {
#ifndef HAVE_ONNXRUNTIME
  GTEST_SKIP() << "ONNX Runtime disabled in this build";
#endif
  const auto config = nlohmann::json::parse(R"json({
  "models": [
    {
      "name": "embed_model",
      "type": "embedding",
      "file": "neutral-embedding.fixture",
      "params": {
        "tokenizer_file": "vocab.fixture",
        "embedding_dim": 1
      },
      "backend": {
        "type": "onnxruntime"
      }
    }
  ],
  "pipeline": [
    {
      "type": "text_corpus_source",
      "name": "source_a",
      "params": {
        "corpus": [
          "a",
          "b\u0000\u0001c",
          "",
          "中文"
        ]
      }
    },
    {
      "type": "text_embedding",
      "name": "embed_a",
      "params": {
        "bind_model": "embed_model"
      },
      "inputs": {
        "text": "source_a.corpus"
      }
    },
    {
      "type": "text_corpus_source",
      "name": "source_b",
      "params": {
        "corpus": [
          "a\u0000\u0001b",
          "c",
          "",
          "中文"
        ]
      }
    },
    {
      "type": "text_embedding",
      "name": "embed_b",
      "params": {
        "bind_model": "embed_model"
      },
      "inputs": {
        "text": "source_b.corpus"
      }
    },
    {
      "type": "text_rule_match",
      "name": "rules",
      "params": {},
      "inputs": {
        "text": "input.input_sentences"
      }
    }
  ]
})json");
  auto plan = PipelineValidator::ValidateAndPlan(
      config,
      MakeTestBoundary({{"input.input_sentences", "TextBatch"}},
                       {{"rules.matches", "RuleMatchBatch"},
                        {"embed_a.embedding", "EmbeddingBatch", true, "N:M"},
                        {"embed_b.embedding", "EmbeddingBatch", true, "N:M"}}));
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump();
  for (const char* name : {"embed_a", "embed_b"}) {
    const auto& node_plan = plan.node_plans.at(name);
    const auto* input = node_plan.FindPort("text", PortDirection::kInput);
    const auto* output =
        node_plan.FindPort("embedding", PortDirection::kOutput);
    ASSERT_NE(input, nullptr);
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(input->lifetime, "session");
    EXPECT_EQ(output->lifetime, "session");
  }
  for (int request = 0; request < 2; ++request) {
    AlgContext ctx;
    ctx.Publish("input.input_sentences", TextBatch{{1, 0, "probe"}});
    for (const auto& id : plan.report.topological_order) {
      const auto& node_plan = plan.node_plans.at(id);
      auto node = NodeRegistry::Instance().Create(node_plan.node.node_type);
      ASSERT_NE(node, nullptr);
      ASSERT_TRUE(node->Init({&node_plan, session_ctx_.get()}));
      ASSERT_EQ(node->Process(&ctx), 0);
    }
    for (const char* key : {"a", "b"}) {
      const auto* corpus =
          ctx.Read<TextBatch>(std::string("source_") + key + ".corpus");
      const auto* vectors =
          ctx.Read<EmbeddingBatch>(std::string("embed_") + key + ".embedding");
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

TEST_F(TextEmbeddingNodeTest,
       RequestInputRecomputesEvenWhenPayloadIsUnchanged) {
  auto node = NodeRegistry::Instance().Create("text_embedding");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"bind_model", "embed_model"}},
                              session_ctx_.get(), nullptr, {},
                              {{"text", "request"}}));
  for (int request = 0; request < 2; ++request) {
    AlgContext context;
    context.Publish("text", TextBatch{{43, 7, "same payload"}});
    ASSERT_EQ(node->Process(&context), 0) << context.GetErrorMessage();
    const auto* output = context.Read<EmbeddingBatch>("embedding");
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->size(), 1U);
    EXPECT_EQ(output->front().req_id, 43U);
    EXPECT_EQ(output->front().sub_id, 7U);
  }
  EXPECT_EQ(counting_model_->infer_calls.load(), 2);
}

}  // namespace llm_edgeflow
