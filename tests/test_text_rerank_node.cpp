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

namespace alg_framework {

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
  auto node = NodeFactory::Instance().Create("TextRerankNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "fake_rerank_model"}, {"top_k", 2}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

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

  ctx.Set("queries", queries);
  ctx.Set("candidates", candidates);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* ranked = ctx.Get<RankedTextBatch>("ranked");
  ASSERT_NE(ranked, nullptr);
  ASSERT_EQ(ranked->size(), 2u);
  EXPECT_EQ((*ranked)[0].data.text, "Refund Policy Guide HIGH");
  EXPECT_EQ((*ranked)[0].data.original_sub_id, 0u);
  EXPECT_EQ((*ranked)[1].data.text, "Contact Support MID");
  EXPECT_EQ((*ranked)[1].data.original_sub_id, 2u);
}

// 2. Process Pairs (Group 1)
TEST_F(TextRerankNodeTest, ProcessPairsInput) {
  auto node = NodeFactory::Instance().Create("TextRerankNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "fake_rerank_model"}, {"top_k", 1}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  QueryCandidatesBatch pairs;
  pairs.emplace_back(1, 10, QueryCandidatePair("Query A", "Candidate LOW"));
  pairs.emplace_back(1, 20, QueryCandidatePair("Query A", "Candidate HIGH"));
  ctx.Set("pairs", pairs);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* ranked = ctx.Get<RankedTextBatch>("ranked");
  ASSERT_NE(ranked, nullptr);
  ASSERT_EQ(ranked->size(), 1u);
  EXPECT_EQ((*ranked)[0].data.text, "Candidate HIGH");
  EXPECT_EQ((*ranked)[0].data.original_sub_id, 20u);
}

// 3. Process Queries + CandidateTexts (Group 3)
TEST_F(TextRerankNodeTest, ProcessQueriesAndCandidateTexts) {
  auto node = NodeFactory::Instance().Create("TextRerankNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "fake_rerank_model"}, {"top_k", 2}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch queries = {{10, 0, "Query 10"}};
  TextBatch candidate_texts = {
      {10, 5, "Doc LOW"},
      {10, 6, "Doc MID"},
      {10, 7, "Doc HIGH"},
  };

  ctx.Set("queries", queries);
  ctx.Set("candidate_texts", candidate_texts);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* ranked = ctx.Get<RankedTextBatch>("ranked");
  ASSERT_NE(ranked, nullptr);
  ASSERT_EQ(ranked->size(), 2u);
  EXPECT_EQ((*ranked)[0].data.text, "Doc HIGH");
  EXPECT_EQ((*ranked)[0].data.original_sub_id, 7u);
  EXPECT_EQ((*ranked)[1].data.text, "Doc MID");
  EXPECT_EQ((*ranked)[1].data.original_sub_id, 6u);
}

// 4. Multi Request Grouping
TEST_F(TextRerankNodeTest, MultiRequestGrouping) {
  auto node = NodeFactory::Instance().Create("TextRerankNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "fake_rerank_model"}, {"top_k", 1}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch queries = {{1, 0, "Q1"}, {2, 0, "Q2"}};
  TextBatch cand_texts = {
      {1, 0, "Q1 LOW"},
      {1, 1, "Q1 HIGH"},
      {2, 0, "Q2 HIGH"},
      {2, 1, "Q2 MID"},
  };
  ctx.Set("queries", queries);
  ctx.Set("candidate_texts", cand_texts);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* ranked = ctx.Get<RankedTextBatch>("ranked");
  ASSERT_NE(ranked, nullptr);
  ASSERT_EQ(ranked->size(), 2u);
  EXPECT_EQ((*ranked)[0].req_id, 1u);
  EXPECT_EQ((*ranked)[0].data.text, "Q1 HIGH");
  EXPECT_EQ((*ranked)[1].req_id, 2u);
  EXPECT_EQ((*ranked)[1].data.text, "Q2 HIGH");
}

// 5. Typed Model pair-input path
TEST_F(TextRerankNodeTest, TypedModelPairInputPath) {
  auto node = NodeFactory::Instance().Create("TextRerankNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "fake_rerank_model"}, {"top_k", 1}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  QueryCandidatesBatch pairs;
  pairs.emplace_back(1, 0, QueryCandidatePair("Query", "Passage"));
  ctx.Set("pairs", pairs);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* ranked = ctx.Get<RankedTextBatch>("ranked");
  ASSERT_NE(ranked, nullptr);
  ASSERT_EQ(ranked->size(), 1u);
}

// 6. Failures and Error Handling
TEST_F(TextRerankNodeTest, FailuresAndProvenanceMismatch) {
  auto node = NodeFactory::Instance().Create("TextRerankNode");
  ASSERT_NE(node, nullptr);
  nlohmann::json cfg = {{"bind_model", "fake_rerank_model"}, {"top_k", 1}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  QueryCandidatesBatch pairs = {
      {1, 0, QueryCandidatePair("Q", "HIGH")},
      {1, 1, QueryCandidatePair("Q", "LOW")},
  };
  ctx.Set("pairs", pairs);

  // Score error
  fake_model_->fail_score_ = true;
  EXPECT_EQ(node->Process(&ctx), -1);

  // Score count mismatch
  fake_model_->fail_score_ = false;
  fake_model_->return_wrong_count_ = true;
  EXPECT_EQ(node->Process(&ctx), -1);

  // Score provenance mismatch
  fake_model_->return_wrong_count_ = false;
  fake_model_->corrupt_provenance_ = true;
  EXPECT_EQ(node->Process(&ctx), -1);
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
         {"model_type", "test_business_rerank"},
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

}  // namespace alg_framework
