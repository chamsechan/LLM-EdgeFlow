#include <gtest/gtest.h>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "tests/support/node_probe.h"

namespace llm_edgeflow {
namespace {
using test_support::NodeProbe;

TEST(TextCorpusSourceNodeTest,
     PublishesLiteralCorpusInConfiguredOrderWithSessionIdentity) {
  SessionContext session;
  NodeProbe node;
  ASSERT_TRUE(node.Init("text_corpus_source",
                        {{"corpus", {"second", "第一", "second", ""}}}, session,
                        {}, {{"corpus", "corpus", ""}}))
      << node.diagnostic;
  ASSERT_NE(node.plan.FindPort("corpus", PortDirection::kOutput), nullptr);
  EXPECT_EQ(node.plan.FindPort("corpus", PortDirection::kOutput)->lifetime,
            "session");
  AlgContext first;
  ASSERT_EQ(node.node->Process(&first), 0);
  const auto* output = first.Read<TextBatch>("corpus");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), 4u);
  const std::vector<std::string> expected = {"second", "第一", "second", ""};
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ((*output)[i].req_id, 0u);
    EXPECT_EQ((*output)[i].sub_id, i);
    EXPECT_EQ((*output)[i].data, expected[i]);
  }
  AlgContext another;
  ASSERT_EQ(node.node->Process(&another), 0);
  ASSERT_NE(another.Read<TextBatch>("corpus"), nullptr);
  EXPECT_EQ(another.Read<TextBatch>("corpus")->at(1).data, "第一");
}

TEST(TextCorpusSourceNodeTest, EmptyCorpusPublishesAnEmptyBatch) {
  SessionContext session;
  NodeProbe node;
  ASSERT_TRUE(node.Init("text_corpus_source",
                        {{"corpus", nlohmann::json::array()}}, session, {},
                        {{"corpus", "corpus", ""}}));
  AlgContext context;
  ASSERT_EQ(node.node->Process(&context), 0);
  ASSERT_NE(context.Read<TextBatch>("corpus"), nullptr);
  EXPECT_TRUE(context.Read<TextBatch>("corpus")->empty());
}
}  // namespace
}  // namespace llm_edgeflow
