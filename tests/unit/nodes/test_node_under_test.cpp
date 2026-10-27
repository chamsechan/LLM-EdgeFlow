#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "core/common_contracts.h"
#include "tests/support/node_probe.h"
#include "tests/support/recording_models.h"

namespace llm_edgeflow {
namespace {
using test_support::NodeUnderTest;

TEST(NodeUnderTestTest, FailedRunPublishesNothingAndNextRunSucceeds) {
  NodeUnderTest node;
  auto model = std::make_shared<test_support::RecordingLlm>();
  model->replies = {{std::string("unused")}, {std::string("answer")}};
  model->fail_call = 1;
  ASSERT_TRUE(node.AddModel("fixture", model));
  ASSERT_TRUE(node.Init("prompt_guided_llm", {{"bind_model", "fixture"}},
                        {"input"}, {"output"}))
      << node.diagnostic;
  const TextBatch inputs{{3, 4, "question"}};

  const auto failed = node.Process({{"input", inputs}});
  EXPECT_NE(failed.status, 0);
  EXPECT_FALSE(failed.diagnostic.empty());
  EXPECT_FALSE(failed.Published("output"));
  EXPECT_EQ(failed.Output<TextBatch>("output"), nullptr);

  const auto next = node.Process({{"input", inputs}});
  ASSERT_EQ(next.status, 0) << next.diagnostic;
  const auto* result = next.Output<TextBatch>("output");
  ASSERT_NE(result, nullptr);
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(result->front().req_id, 3u);
  EXPECT_EQ(result->front().sub_id, 4u);
  EXPECT_EQ(result->front().data, "answer");
  EXPECT_EQ(model->calls.size(), 2u);
}

TEST(NodeUnderTestTest, ReportsInvalidInitAndUnconnectedInputs) {
  NodeUnderTest unregistered;
  EXPECT_FALSE(unregistered.Init("missing_node", nlohmann::json::object(),
                                 {"input"}, {"output"}));
  EXPECT_NE(unregistered.diagnostic.find("missing_node"), std::string::npos);
  EXPECT_NONFATAL_FAILURE(unregistered.Process({}), "successful Init");

  // 模型绑定失败的已注册 Node 同样不得运行，否则空批次会在不调用模型的
  // 情况下成功。
  NodeUnderTest unbound;
  EXPECT_FALSE(unbound.Init("prompt_guided_llm", {{"bind_model", "absent"}},
                            {"input"}, {"output"}));
  EXPECT_NE(unbound.diagnostic.find("absent"), std::string::npos);
  EXPECT_NONFATAL_FAILURE(unbound.Process({{"input", TextBatch{}}}),
                          "successful Init");
  EXPECT_NONFATAL_FAILURE(unbound.Control(1, "{}"), "successful Init");

  NodeUnderTest node;
  auto model = std::make_shared<test_support::RecordingLlm>();
  ASSERT_TRUE(node.AddModel("fixture", model));
  ASSERT_TRUE(node.Init("prompt_guided_llm", {{"bind_model", "fixture"}},
                        {"input"}, {"output"}))
      << node.diagnostic;
  const TextBatch inputs{{1, 0, "question"}};
  EXPECT_NONFATAL_FAILURE(node.Process({{"context", inputs}}), "context");
  EXPECT_NONFATAL_FAILURE(node.Process({{"input", inputs}, {"input", inputs}}),
                          "given twice");
  EXPECT_TRUE(model->calls.empty());
}

}  // namespace
}  // namespace llm_edgeflow
