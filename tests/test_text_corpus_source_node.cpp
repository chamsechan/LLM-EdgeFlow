#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapter/shared_algorithm_runtime.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/session_context.h"
#include "tests/support/node_test_utils.h"

namespace alg_framework {

class TextCorpusSourceNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SharedAlgorithmRuntime::GlobalInit(), 0);
    session_ctx_ = std::make_unique<SessionContext>();
  }
  std::unique_ptr<SessionContext> session_ctx_;
};

// 1. Process Static Corpus Emission
TEST_F(TextCorpusSourceNodeTest, ProcessStaticCorpusEmission) {
  auto node = NodeFactory::Instance().Create("TextCorpusSourceNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {
      {"corpus", {"Clause 1: Compliance", "Clause 2: Security"}}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Read<TextBatch>("corpus");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 2u);
  EXPECT_EQ((*out)[0].data, "Clause 1: Compliance");
  EXPECT_EQ((*out)[1].data, "Clause 2: Security");
}

// 2. Empty Corpus Config
TEST_F(TextCorpusSourceNodeTest, EmptyCorpusConfig) {
  auto node = NodeFactory::Instance().Create("TextCorpusSourceNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));

  AlgContext ctx;
  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Read<TextBatch>("corpus");
  ASSERT_NE(out, nullptr);
  EXPECT_TRUE(out->empty());
}

}  // namespace alg_framework
