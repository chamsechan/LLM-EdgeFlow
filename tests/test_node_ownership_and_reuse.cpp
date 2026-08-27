#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "core/session_context.h"
#include "engine/engine_interface.h"

namespace alg_framework {

TEST(NodeOwnershipAndReuseTest, CatalogCategoriesAndOwnership) {
  // Common nodes in Phase 1
  const auto* llm_gen = PipelineCatalog::FindNode("LlmGenerateNode");
  ASSERT_NE(llm_gen, nullptr);
  EXPECT_EQ(llm_gen->category, "common");

  const auto* text_tmpl = PipelineCatalog::FindNode("TextTemplateNode");
  ASSERT_NE(text_tmpl, nullptr);
  EXPECT_EQ(text_tmpl->category, "common");

  const auto* text_chunk = PipelineCatalog::FindNode("TextChunkNode");
  ASSERT_NE(text_chunk, nullptr);
  EXPECT_EQ(text_chunk->category, "common");

  const auto* vec_topk = PipelineCatalog::FindNode("VectorTopKNode");
  ASSERT_NE(vec_topk, nullptr);
  EXPECT_EQ(vec_topk->category, "common");

  const auto* text_rerank = PipelineCatalog::FindNode("TextRerankNode");
  ASSERT_NE(text_rerank, nullptr);
  EXPECT_EQ(text_rerank->category, "common");
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

TEST(NodeOwnershipAndReuseTest, CommonEmbeddingAndVectorTopKExecution) {
  SessionContext session_ctx;
  session_ctx.GetModelManager().RegisterModel(
      "embed_model_v2", std::make_shared<DistinctMockEmbeddingEngine>());

  auto embed_node = NodeFactory::Instance().Create("TextEmbeddingNode");
  ASSERT_NE(embed_node, nullptr);

  nlohmann::json node_cfg = {{"bind_model", "embed_model_v2"},
                             {"normalize", true}};
  ASSERT_TRUE(embed_node->Init(node_cfg, &session_ctx));

  AlgContext ctx;
  TextBatch query_batch = {
      TraceableItem<std::string>{0, 0, "请把钱私下转账给我"}};
  ctx.Set(BlackboardKey<TextBatch>{"text", "TextBatch"}, query_batch);

  int ret = embed_node->Process(&ctx);
  EXPECT_EQ(ret, 0);

  const auto* q_embed =
      ctx.Get(BlackboardKey<EmbeddingBatch>{"embedding", "EmbeddingBatch"});
  ASSERT_NE(q_embed, nullptr);
  ASSERT_EQ(q_embed->size(), 1u);
  EXPECT_GT((*q_embed)[0].data[0], 0.5f);
}

}  // namespace alg_framework
