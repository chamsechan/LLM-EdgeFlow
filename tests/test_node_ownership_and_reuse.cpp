#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "biz/dialogue_audit/dialogue_audit_contract.h"
#include "core/alg_context.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "core/session_context.h"
#include "engine/engine_interface.h"

namespace alg_framework {

TEST(NodeOwnershipAndReuseTest, CatalogCategoriesAndOwnership) {
  // 1. LlmGenerateNode is a common node
  const auto* llm_gen = PipelineCatalog::FindNode("LlmGenerateNode");
  ASSERT_NE(llm_gen, nullptr);
  EXPECT_EQ(llm_gen->category, "common");

  // 2. PromptBuilderNode, VectorSearchNode, RerankRefineNode are biz nodes
  const auto* prompt_bld = PipelineCatalog::FindNode("PromptBuilderNode");
  ASSERT_NE(prompt_bld, nullptr);
  EXPECT_EQ(prompt_bld->category, "biz");

  const auto* vec_search = PipelineCatalog::FindNode("VectorSearchNode");
  ASSERT_NE(vec_search, nullptr);
  EXPECT_EQ(vec_search->category, "biz");

  const auto* rerank_refine = PipelineCatalog::FindNode("RerankRefineNode");
  ASSERT_NE(rerank_refine, nullptr);
  EXPECT_EQ(rerank_refine->category, "biz");
}

// Mock Embedding Engine that computes distinct vector based on string hash /
// features
class DistinctMockEmbeddingEngine : public IEmbeddingEngine {
 public:
  bool Load(const std::string&, const nlohmann::json&) override { return true; }
  size_t GetMaxBatchSize() const override { return 16; }
  const std::string& EngineType() const override {
    static const std::string t = "mock_embedding";
    return t;
  }

  int InferTraceableBatch(
      const std::vector<TraceableItem<std::string>>& inputs,
      std::vector<TraceableItem<std::vector<float>>>* outputs) override {
    if (!outputs) return -1;
    outputs->clear();
    for (const auto& item : inputs) {
      std::vector<float> vec(4, 0.0f);
      if (item.data.find("101") != std::string::npos ||
          item.data.find("私下转账") != std::string::npos) {
        vec = {1.0f, 0.0f, 0.0f, 0.0f};
      } else if (item.data.find("102") != std::string::npos ||
                 item.data.find("银行卡密码") != std::string::npos) {
        vec = {0.0f, 1.0f, 0.0f, 0.0f};
      } else if (item.data.find("103") != std::string::npos ||
                 item.data.find("侮辱") != std::string::npos) {
        vec = {0.0f, 0.0f, 1.0f, 0.0f};
      } else {
        vec = {0.0f, 0.0f, 0.0f, 1.0f};
      }
      outputs->emplace_back(item.req_id, item.sub_id, std::move(vec));
    }
    return 0;
  }
};

TEST(NodeOwnershipAndReuseTest, DenseRetrievalEmbeddingCosineRanking) {
  SessionContext session_ctx;
  session_ctx.GetModelManager().RegisterModel(
      "embed_model_v2", std::make_shared<DistinctMockEmbeddingEngine>());

  auto node = NodeFactory::Instance().Create("DenseRetrievalNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json node_cfg = {{"bind_model", "embed_model_v2"}, {"top_k", 2}};
  ASSERT_TRUE(node->Init(node_cfg, &session_ctx));

  AlgContext ctx;
  std::vector<std::string> user_texts = {"请把钱私下转账给我"};
  ctx.Set(kUserTexts, user_texts);

  int ret = node->Process(&ctx);
  EXPECT_EQ(ret, 0);

  const auto* candidates = ctx.Get(kCandidatePolicies);
  ASSERT_NE(candidates, nullptr);
  ASSERT_EQ(candidates->size(), 1u);
  // Top 1 retrieved policy should be clause 101 regarding 私下转账
  ASSERT_GE((*candidates)[0].data.size(), 1u);
  EXPECT_TRUE((*candidates)[0].data[0].find("101") != std::string::npos);
}

}  // namespace alg_framework
