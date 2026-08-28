#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapter/shared_algorithm_runtime.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline_validator.h"
#include "core/session_context.h"
#include "engine/engine_interface.h"
#include "engine/engine_registry.h"

namespace alg_framework {

class TextRerankNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SharedAlgorithmRuntime::GlobalInit(), 0);
    session_ctx_ = std::make_unique<SessionContext>();

    auto rerank_engine = EngineFactory::Instance().Create("mock_npu_rerank");
    ASSERT_NE(rerank_engine, nullptr);
    rerank_engine->Load("./models/bge_reranker.bin", {{"max_batch_size", 4}});
    ASSERT_TRUE(session_ctx_->GetModelManager().RegisterModel(
        "rerank_model_v1", std::move(rerank_engine)));
  }
  std::unique_ptr<SessionContext> session_ctx_;
};

// 1. Process Queries + Candidates (Group 2)
TEST_F(TextRerankNodeTest, ProcessQueriesAndCandidates) {
  auto node = NodeFactory::Instance().Create("TextRerankNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "rerank_model_v1"}, {"top_k", 2}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch queries;
  queries.emplace_back(1, 0, "How to refund?");

  RankedTextBatch candidates;
  candidates.emplace_back(1, 0,
                          RankedCandidate("Refund Policy Guide", 0.8f, 1, 0));
  candidates.emplace_back(1, 1,
                          RankedCandidate("Company About Us", 0.2f, 2, 1));
  candidates.emplace_back(1, 2, RankedCandidate("Contact Support", 0.5f, 3, 2));

  ctx.Set("queries", queries);
  ctx.Set("candidates", candidates);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* ranked = ctx.Get<RankedTextBatch>("ranked");
  ASSERT_NE(ranked, nullptr);
  ASSERT_EQ(ranked->size(), 2u);
  EXPECT_EQ((*ranked)[0].data.text, "Refund Policy Guide");
}

// 2. Process Pairs (Group 1)
TEST_F(TextRerankNodeTest, ProcessPairsInput) {
  auto node = NodeFactory::Instance().Create("TextRerankNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "rerank_model_v1"}, {"top_k", 1}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  QueryCandidatesBatch pairs;
  pairs.emplace_back(1, 0, QueryCandidatePair("Query A", "Candidate A1"));
  pairs.emplace_back(1, 1, QueryCandidatePair("Query A", "Candidate A2"));
  ctx.Set("pairs", pairs);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* ranked = ctx.Get<RankedTextBatch>("ranked");
  ASSERT_NE(ranked, nullptr);
  ASSERT_EQ(ranked->size(), 1u);
}

// 3. Port Constraints Validation Check
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
       {{{"engine_type", "mock_npu_rerank"},
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
