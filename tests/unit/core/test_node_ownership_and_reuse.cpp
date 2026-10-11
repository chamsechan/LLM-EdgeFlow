#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_interface.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "core/session_context.h"
#include "engine/model_interface.h"
#include "tests/support/model_registration.h"
#include "tests/support/node_test_utils.h"

namespace llm_edgeflow {

class RetainedPlanProbeNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "retained_plan_probe";
  bool Init(const NodeInitContext& init) override {
    plan_ = init.plan;
    return plan_ != nullptr;
  }
  int Process(AlgContext*) override {
    return plan_->normalized_params.at("value").get<int>();
  }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }

 private:
  const ValidatedNodePlan* plan_ = nullptr;
};

TEST(NodeOwnershipAndReuseTest, TestFixtureRetainsPlanForNodeLifetime) {
  NodeDefinition definition;
  definition.node_type = RetainedPlanProbeNode::kNodeType;
  definition.config_fields = {{"value", ConfigValueKind::kInteger, false, 17}};
  ASSERT_TRUE(NodeRegistry::Instance().Register(
      definition.node_type,
      [] { return std::make_unique<RetainedPlanProbeNode>(); }, definition));
  SessionContext session;
  RetainedPlanProbeNode node;
  ASSERT_TRUE(InitNodeForTest(node, nlohmann::json::object(), &session));
  EXPECT_EQ(node.Process(nullptr), 17);
  // 第二次初始化不能使第一个 Node 保存的指针失效。
  RetainedPlanProbeNode second;
  ASSERT_TRUE(InitNodeForTest(second, {{"value", 29}}, &session));
  EXPECT_EQ(second.Process(nullptr), 29);
  EXPECT_EQ(node.Process(nullptr), 17);
}

TEST(NodeOwnershipAndReuseTest, CatalogCategoriesAndOwnership) {
  // 通用 Node
  const auto llm_gen = PipelineCatalog::FindNode("llm_generate");
  ASSERT_TRUE(llm_gen.has_value());
  EXPECT_EQ(llm_gen->category, "common");

  const auto text_tmpl = PipelineCatalog::FindNode("text_template");
  ASSERT_TRUE(text_tmpl.has_value());
  EXPECT_EQ(text_tmpl->category, "common");

  const auto text_chunk = PipelineCatalog::FindNode("text_chunk");
  ASSERT_TRUE(text_chunk.has_value());
  EXPECT_EQ(text_chunk->category, "common");

  const auto vec_topk = PipelineCatalog::FindNode("vector_top_k");
  ASSERT_TRUE(vec_topk.has_value());
  EXPECT_EQ(vec_topk->category, "common");

  const auto text_rerank = PipelineCatalog::FindNode("text_rerank");
  ASSERT_TRUE(text_rerank.has_value());
  EXPECT_EQ(text_rerank->category, "common");
}

}  // namespace llm_edgeflow
