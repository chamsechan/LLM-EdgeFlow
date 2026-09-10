#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapter/shared_algorithm_runtime.h"
#include "contracts/inference_payloads.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline_validator.h"
#include "core/session_context.h"
#include "engine/model_interface.h"
#include "nodes/node_error_codes.h"
#include "tests/support/node_test_utils.h"

namespace llm_edgeflow {

namespace {

class FakeRerankModel : public IRerankModel {
 public:
  const std::string& ModelType() const noexcept override {
    static const std::string type = "fake_reranker";
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string cap = "rerank";
    return cap;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  size_t GetMaxBatchSize() const noexcept override { return 16; }

  int Score(const QueryCandidatesBatch& inputs,
            ScoreBatch* outputs) noexcept override {
    if (!outputs) return -1;
    outputs->clear();
    if (fail_score_) return -1;

    if (return_wrong_count_) {
      outputs->emplace_back(0, 0, 1.0f);
      return 0;
    }

    for (size_t i = 0; i < inputs.size(); ++i) {
      const auto& item = inputs[i];
      uint32_t r_id = item.req_id;
      uint32_t s_id = item.sub_id;
      if (corrupt_provenance_ && i == 0) {
        s_id += 999;
      }
      float score = 0.5f;
      if (item.data.candidate.find("HIGH") != std::string::npos) {
        score = 0.95f;
      } else if (item.data.candidate.find("MID") != std::string::npos) {
        score = 0.75f;
      } else if (item.data.candidate.find("LOW") != std::string::npos) {
        score = 0.15f;
      }
      outputs->emplace_back(r_id, s_id, score);
    }
    return 0;
  }

  bool fail_score_ = false;
  bool return_wrong_count_ = false;
  bool corrupt_provenance_ = false;
};

}  // namespace

class TextRerankNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SharedAlgorithmRuntime::GlobalInit(), 0);
    session_ctx_ = std::make_unique<SessionContext>();

    // 注册 Fake IRerankModel
    fake_model_ = std::make_shared<FakeRerankModel>();
    ASSERT_TRUE(session_ctx_->GetModelManager().RegisterModel(
        "fake_rerank_model", fake_model_, "v1", "fake_reranker", "rerank",
        "mock"));
  }

  std::shared_ptr<FakeRerankModel> fake_model_;
  std::unique_ptr<SessionContext> session_ctx_;
};

// 1. Process Queries + Candidates (Group 2)
TEST_F(TextRerankNodeTest, ProcessQueriesAndCandidates) {
  auto node = NodeRegistry::Instance().Create("TextRerankNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "fake_rerank_model"}, {"top_k", 2}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch queries;
  queries.emplace_back(1, 0, "How to refund?");

  RankedTextBatch candidates;
  candidates.emplace_back(
      1, 0, RankedCandidate("Refund Policy Guide HIGH", 0.8f, 1, 0));
  candidates.emplace_back(1, 1,
                          RankedCandidate("Company About Us LOW", 0.2f, 2, 1));
  candidates.emplace_back(1, 2,
                          RankedCandidate("Contact Support MID", 0.5f, 3, 2));

  ctx.Publish("queries", queries);
  ctx.Publish("candidates", candidates);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* ranked = ctx.Read<RankedTextBatch>("ranked");
  ASSERT_NE(ranked, nullptr);
  ASSERT_EQ(ranked->size(), 2u);
  EXPECT_EQ((*ranked)[0].data.text, "Refund Policy Guide HIGH");
  EXPECT_EQ((*ranked)[0].data.original_sub_id, 0u);
  EXPECT_EQ((*ranked)[1].data.text, "Contact Support MID");
  EXPECT_EQ((*ranked)[1].data.original_sub_id, 2u);
}

// 2. Process Pairs (Group 1)
TEST_F(TextRerankNodeTest, ProcessPairsInput) {
  auto node = NodeRegistry::Instance().Create("TextRerankNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "fake_rerank_model"}, {"top_k", 1}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  QueryCandidatesBatch pairs;
  pairs.emplace_back(1, 10, QueryCandidatePair("Query A", "Candidate LOW"));
  pairs.emplace_back(1, 20, QueryCandidatePair("Query A", "Candidate HIGH"));
  ctx.Publish("pairs", pairs);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* ranked = ctx.Read<RankedTextBatch>("ranked");
  ASSERT_NE(ranked, nullptr);
  ASSERT_EQ(ranked->size(), 1u);
  EXPECT_EQ((*ranked)[0].data.text, "Candidate HIGH");
  EXPECT_EQ((*ranked)[0].data.original_sub_id, 20u);
}

// 3. Process Queries + CandidateTexts (Group 3)
TEST_F(TextRerankNodeTest, ProcessQueriesAndCandidateTexts) {
  auto node = NodeRegistry::Instance().Create("TextRerankNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "fake_rerank_model"}, {"top_k", 2}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch queries = {{10, 0, "Query 10"}};
  TextBatch candidate_texts = {
      {10, 5, "Doc LOW"},
      {10, 6, "Doc MID"},
      {10, 7, "Doc HIGH"},
  };

  ctx.Publish("queries", queries);
  ctx.Publish("candidate_texts", candidate_texts);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* ranked = ctx.Read<RankedTextBatch>("ranked");
  ASSERT_NE(ranked, nullptr);
  ASSERT_EQ(ranked->size(), 2u);
  EXPECT_EQ((*ranked)[0].data.text, "Doc HIGH");
  EXPECT_EQ((*ranked)[0].data.original_sub_id, 7u);
  EXPECT_EQ((*ranked)[1].data.text, "Doc MID");
  EXPECT_EQ((*ranked)[1].data.original_sub_id, 6u);
}

// 4. Multi Request Grouping
TEST_F(TextRerankNodeTest, MultiRequestGrouping) {
  auto node = NodeRegistry::Instance().Create("TextRerankNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "fake_rerank_model"}, {"top_k", 1}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch queries = {{1, 0, "Q1"}, {2, 0, "Q2"}};
  TextBatch cand_texts = {
      {1, 0, "Q1 LOW"},
      {1, 1, "Q1 HIGH"},
      {2, 0, "Q2 HIGH"},
      {2, 1, "Q2 MID"},
  };
  ctx.Publish("queries", queries);
  ctx.Publish("candidate_texts", cand_texts);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* ranked = ctx.Read<RankedTextBatch>("ranked");
  ASSERT_NE(ranked, nullptr);
  ASSERT_EQ(ranked->size(), 2u);
  EXPECT_EQ((*ranked)[0].req_id, 1u);
  EXPECT_EQ((*ranked)[0].data.text, "Q1 HIGH");
  EXPECT_EQ((*ranked)[1].req_id, 2u);
  EXPECT_EQ((*ranked)[1].data.text, "Q2 HIGH");
}

// 5. Typed Model pair-input path
TEST_F(TextRerankNodeTest, TypedModelPairInputPath) {
  auto node = NodeRegistry::Instance().Create("TextRerankNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "fake_rerank_model"}, {"top_k", 1}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  QueryCandidatesBatch pairs;
  pairs.emplace_back(1, 0, QueryCandidatePair("Query", "Passage"));
  ctx.Publish("pairs", pairs);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* ranked = ctx.Read<RankedTextBatch>("ranked");
  ASSERT_NE(ranked, nullptr);
  ASSERT_EQ(ranked->size(), 1u);
}

// 6. Failures and Error Handling
TEST_F(TextRerankNodeTest, FailuresAndProvenanceMismatch) {
  auto node = NodeRegistry::Instance().Create("TextRerankNode");
  ASSERT_NE(node, nullptr);
  nlohmann::json cfg = {{"bind_model", "fake_rerank_model"}, {"top_k", 1}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  QueryCandidatesBatch pairs = {
      {1, 0, QueryCandidatePair("Q", "HIGH")},
      {1, 1, QueryCandidatePair("Q", "LOW")},
  };
  ctx.Publish("pairs", pairs);

  // Score error
  fake_model_->fail_score_ = true;
  EXPECT_EQ(node->Process(&ctx), -1);

  // Score count mismatch
  fake_model_->fail_score_ = false;
  fake_model_->return_wrong_count_ = true;
  EXPECT_EQ(node->Process(&ctx), node_error::text_rerank::kModelOutputMismatch);

  // Score provenance mismatch
  fake_model_->return_wrong_count_ = false;
  fake_model_->corrupt_provenance_ = true;
  EXPECT_EQ(node->Process(&ctx), node_error::text_rerank::kModelOutputMismatch);
}

// 7. Port Constraints Validation Check
TEST_F(TextRerankNodeTest, PortConstraintsValidation) {
  auto has_constraint_err = [](const ValidationReport& r) {
    return std::any_of(r.diagnostics.begin(), r.diagnostics.end(),
                       [](const auto& d) {
                         return d.code == DiagnosticCode::kInvalidCombination;
                       });
  };

  // Missing query when candidates is bound -> Fail
  nlohmann::json bad_pipeline = {
      {"biz_name", "cross_rerank_matrix_v1"},
      {"models",
       {{{"capability", "rerank"},
         {"model_type", "test_biz_rerank"},
         {"backend", "test_tensor_backend"},
         {"model_id", "rerank_model_v1"},
         {"model_path", "./models/rerank.bin"}}}},
      {"pipeline",
       {{{"id", "node_0_TextRerankNode"},
         {"node_type", "TextRerankNode"},
         {"depends_on", nlohmann::json::array()},
         {"ports",
          {{"inputs", {{"candidates", "doc_candidates"}}},
           {"outputs", {{"ranked", "ranked_results"}}}}},
         {"config", {{"bind_model", "rerank_model_v1"}}}}}}};

  auto plan = PipelineValidator::ValidateAndPlan(
      bad_pipeline, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_FALSE(plan.report.ok);
  EXPECT_TRUE(has_constraint_err(plan.report));
}

/**
 * @brief 专用于测试 TextRerankNode 重排序与 Top-K 行为的可控 Mock 引擎
 */
class ControllableMockRerankModel : public IRerankModel {
 public:
  const std::string& ModelType() const noexcept override {
    static const std::string type = "controllable_mock_rerank";
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string capability = "rerank";
    return capability;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  size_t GetMaxBatchSize() const noexcept override { return 16; }

  int Score(const QueryCandidatesBatch& input_pairs,
            ScoreBatch* output_scores) noexcept override {
    if (!output_scores) return -1;
    output_scores->clear();
    for (const auto& item : input_pairs) {
      float score = 0.5f;
      if (item.data.candidate.find("SCORE_0.9") != std::string::npos) {
        score = 0.9f;
      } else if (item.data.candidate.find("SCORE_0.7") != std::string::npos) {
        score = 0.7f;
      } else if (item.data.candidate.find("SCORE_0.3") != std::string::npos) {
        score = 0.3f;
      } else if (item.data.candidate.find("SCORE_0.1") != std::string::npos) {
        score = 0.1f;
      }
      output_scores->emplace_back(item.req_id, item.sub_id, score);
    }
    return 0;
  }
};

class TextRerankRankingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Alg_Init();
    mock_model_ = std::make_shared<ControllableMockRerankModel>();
    session_ctx_.GetModelManager().RegisterModel("test_rerank_model",
                                                 mock_model_, "test-v1");
    node_ = NodeRegistry::Instance().Create("TextRerankNode");
    ASSERT_NE(node_, nullptr);
  }

  void TearDown() override { Alg_DeInit(); }

  std::shared_ptr<ControllableMockRerankModel> mock_model_;
  SessionContext session_ctx_;
  std::unique_ptr<INode> node_;
};

// 1. 验证重排序与 Top-K 选精逻辑 (打分最高的候选排在前面)
TEST_F(TextRerankRankingTest, ReorderAndTopKFiltering) {
  nlohmann::json cfg = {
      {"bind_model", "test_rerank_model"},
      {"top_k", 2},
  };
  ASSERT_TRUE(InitNodeForTest(*node_, cfg, &session_ctx_));

  AlgContext ctx;
  TextBatch raw_queries = {
      TraceableItem<std::string>{0, 0, "什么是深度学习？"},
      TraceableItem<std::string>{1, 0, "退款流程？"},
  };
  ctx.Publish(BlackboardKey<TextBatch>{"queries", "TextBatch"}, raw_queries);

  // 故意将低分样本放在前面，高分样本放在后面，测试重排序
  RankedTextBatch input_candidates = {
      // Request 0 的 3 个候选 (期望排序: sub_id=2 (0.9) -> sub_id=1 (0.7))
      {0, 0, RankedCandidate("候选 A (SCORE_0.1)", 0.1f, 1, 0)},
      {0, 1, RankedCandidate("候选 B (SCORE_0.7)", 0.7f, 2, 1)},
      {0, 2, RankedCandidate("候选 C (SCORE_0.9)", 0.9f, 3, 2)},
      // Request 1 的 2 个候选 (期望排序: sub_id=1 (0.9) -> sub_id=0 (0.3))
      {1, 0, RankedCandidate("候选 D (SCORE_0.3)", 0.3f, 1, 0)},
      {1, 1, RankedCandidate("候选 E (SCORE_0.9)", 0.9f, 2, 1)},
  };
  ctx.Publish(BlackboardKey<RankedTextBatch>{"candidates", "RankedTextBatch"},
              input_candidates);

  int ret = node_->Process(&ctx);
  EXPECT_EQ(ret, 0);

  auto* result =
      ctx.Read(BlackboardKey<RankedTextBatch>{"ranked", "RankedTextBatch"});
  ASSERT_NE(result, nullptr);
  // 每个请求保留 top_k=2，共 4 个输出
  ASSERT_EQ(result->size(), 4U);

  // Request 0: 验证 Top-1 是 SCORE_0.9, Top-2 是 SCORE_0.7
  EXPECT_EQ((*result)[0].req_id, 0U);
  EXPECT_EQ((*result)[0].data.original_sub_id, 2U);
  EXPECT_EQ((*result)[0].data.text, "候选 C (SCORE_0.9)");

  EXPECT_EQ((*result)[1].req_id, 0U);
  EXPECT_EQ((*result)[1].data.original_sub_id, 1U);
  EXPECT_EQ((*result)[1].data.text, "候选 B (SCORE_0.7)");

  // Request 1: 验证 Top-1 是 SCORE_0.9, Top-2 是 SCORE_0.3
  EXPECT_EQ((*result)[2].req_id, 1U);
  EXPECT_EQ((*result)[2].data.original_sub_id, 1U);
  EXPECT_EQ((*result)[2].data.text, "候选 E (SCORE_0.9)");

  EXPECT_EQ((*result)[3].req_id, 1U);
  EXPECT_EQ((*result)[3].data.original_sub_id, 0U);
  EXPECT_EQ((*result)[3].data.text, "候选 D (SCORE_0.3)");
}

// 2. 验证空候选集鲁棒性
TEST_F(TextRerankRankingTest, EmptyCandidatesHandling) {
  nlohmann::json cfg = {{"bind_model", "test_rerank_model"}, {"top_k", 1}};
  ASSERT_TRUE(InitNodeForTest(*node_, cfg, &session_ctx_));

  AlgContext ctx;
  TextBatch raw_queries = {TraceableItem<std::string>{0, 0, "Query"}};
  ctx.Publish(BlackboardKey<TextBatch>{"queries", "TextBatch"}, raw_queries);
  ctx.Publish(BlackboardKey<RankedTextBatch>{"candidates", "RankedTextBatch"},
              RankedTextBatch{});

  int ret = node_->Process(&ctx);
  EXPECT_EQ(ret, 0);
}

// 3. 验证缺失黑板 Key 拦截
TEST_F(TextRerankRankingTest, MissingContextKeyHandling) {
  nlohmann::json cfg = {{"bind_model", "test_rerank_model"}};
  ASSERT_TRUE(InitNodeForTest(*node_, cfg, &session_ctx_));

  AlgContext ctx;  // 空黑板
  int ret = node_->Process(&ctx);
  EXPECT_EQ(ret, -7001);
}

}  // namespace llm_edgeflow
