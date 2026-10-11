#include <gtest/gtest.h>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "engine/model_interface.h"
#include "tests/support/node_probe.h"

namespace llm_edgeflow {
namespace {
class RecordingEmbedding final : public IEmbeddingModel {
 public:
  const std::string& ImplName() const noexcept override { return impl; }
  const std::string& ModelType() const noexcept override { return type; }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  int Embed(const TextBatch& inputs, EmbeddingBatch* output,
            std::string* diagnostic) noexcept override {
    if (diagnostic) diagnostic->clear();
    calls.push_back(inputs);
    output->clear();
    if (failures_remaining > 0) {
      --failures_remaining;
      if (diagnostic) *diagnostic = "temporary encoder failure";
      return -51;
    }
    for (size_t i = 0; i < inputs.size(); ++i)
      output->emplace_back(inputs[i].req_id, inputs[i].sub_id, replies.at(i));
    return 0;
  }
  std::string impl = "node_recording_embedding", type = "embedding";
  int failures_remaining = 0;
  std::vector<std::vector<float>> replies = {{1, 2}, {3, 4}};
  std::vector<TextBatch> calls;
};

TEST(TextEmbeddingNodeTest,
     TwoNodesShareSessionCacheAndPublishIndependentOutputValues) {
  SessionContext session;
  auto model = std::make_shared<RecordingEmbedding>();
  ASSERT_TRUE(
      test_support::RegisterNodeModel(session, "encoder", model, "weights-a"));
  test_support::NodeProbe first, second;
  ASSERT_TRUE(first.Init("text_embedding", {{"bind_model", "encoder"}}, session,
                         {{"text", "corpus_a", "session"}},
                         {{"embedding", "first", ""}}));
  ASSERT_TRUE(second.Init("text_embedding", {{"bind_model", "encoder"}},
                          session, {{"text", "corpus_b", "session"}},
                          {{"embedding", "second", ""}}));
  const TextBatch input = {{0, 7, "alpha"}, {0, 2, "beta"}};
  AlgContext initial;
  ASSERT_TRUE(initial.Publish("corpus_a", input));
  ASSERT_EQ(first.node->Process(&initial), 0);
  ASSERT_EQ(model->calls.size(), 1u);
  const auto* published = initial.Read<EmbeddingBatch>("first");
  ASSERT_NE(published, nullptr);
  ASSERT_EQ(published->size(), 2u);
  EXPECT_EQ((*published)[0].data, (std::vector<float>{1, 2}));
  EXPECT_EQ((*published)[1].data, (std::vector<float>{3, 4}));
  auto editable_copy = *published;
  editable_copy[0].data[0] = 99;
  editable_copy[1].data.clear();
  AlgContext cached;
  ASSERT_TRUE(cached.Publish("corpus_b", input));
  ASSERT_EQ(second.node->Process(&cached), 0);
  EXPECT_EQ(model->calls.size(), 1u);
  const auto* result = cached.Read<EmbeddingBatch>("second");
  ASSERT_NE(result, nullptr);
  ASSERT_EQ(result->size(), 2u);
  EXPECT_EQ((*result)[0].req_id, 0u);
  EXPECT_EQ((*result)[0].sub_id, 7u);
  EXPECT_EQ((*result)[0].data, (std::vector<float>{1, 2}));
  EXPECT_EQ((*result)[1].data, (std::vector<float>{3, 4}));
  EXPECT_EQ((*published)[0].data[0], 1);
  EXPECT_EQ(first.plan.FindPort("embedding", PortDirection::kOutput)->lifetime,
            "session");
}

TEST(TextEmbeddingNodeTest,
     CacheIdentityIncludesTextRequestSubItemAndModelRevision) {
  SessionContext session;
  auto model = std::make_shared<RecordingEmbedding>();
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "encoder", model, "r1"));
  test_support::NodeProbe node;
  ASSERT_TRUE(node.Init("text_embedding", {{"bind_model", "encoder"}}, session,
                        {{"text", "input", "session"}},
                        {{"embedding", "output", ""}}));
  const std::vector<TextBatch> distinct = {{{0, 1, "alpha"}},
                                           {{0, 1, "beta"}},
                                           {{0, 2, "alpha"}},
                                           {{7, 1, "alpha"}}};
  for (size_t i = 0; i < distinct.size(); ++i) {
    SCOPED_TRACE(i);
    for (int repeat = 0; repeat < 2; ++repeat) {
      AlgContext context;
      ASSERT_TRUE(context.Publish("input", distinct[i]));
      ASSERT_EQ(node.node->Process(&context), 0);
      EXPECT_EQ(model->calls.size(), i + 1);
      const auto* output = context.Read<EmbeddingBatch>("output");
      ASSERT_NE(output, nullptr);
      EXPECT_EQ(output->front().req_id, distinct[i].front().req_id);
      EXPECT_EQ(output->front().sub_id, distinct[i].front().sub_id);
    }
  }
  ASSERT_TRUE(session.GetModelManager().UpdateModelRevision("encoder", "r2"));
  AlgContext changed;
  ASSERT_TRUE(changed.Publish("input", distinct.front()));
  ASSERT_EQ(node.node->Process(&changed), 0);
  EXPECT_EQ(model->calls.size(), 5u);
  EXPECT_EQ(model->calls.back().front().data, "alpha");
}

TEST(TextEmbeddingNodeTest,
     RequestInputsAlwaysInferEvenWhenTwoNodesShareAModel) {
  SessionContext session;
  auto model = std::make_shared<RecordingEmbedding>();
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "encoder", model));
  test_support::NodeProbe first, second;
  ASSERT_TRUE(first.Init("text_embedding", {{"bind_model", "encoder"}}, session,
                         {{"text", "input", "request"}},
                         {{"embedding", "first", ""}}));
  ASSERT_TRUE(second.Init("text_embedding", {{"bind_model", "encoder"}},
                          session, {{"text", "input", "request"}},
                          {{"embedding", "second", ""}}));
  AlgContext context;
  ASSERT_TRUE(context.Publish("input", TextBatch{{5, 9, "same"}}));
  ASSERT_EQ(first.node->Process(&context), 0);
  ASSERT_EQ(second.node->Process(&context), 0);
  EXPECT_EQ(model->calls.size(), 2u);
  EXPECT_EQ(first.plan.FindPort("embedding", PortDirection::kOutput)->lifetime,
            "request");
  EXPECT_EQ(context.Read<EmbeddingBatch>("second")->front().data,
            (std::vector<float>{1, 2}));
}

TEST(TextEmbeddingNodeTest,
     ModelNamesPartitionCachesWithIdenticalRevisionsAndInputs) {
  SessionContext session;
  auto model_a = std::make_shared<RecordingEmbedding>();
  auto model_b = std::make_shared<RecordingEmbedding>();
  model_a->replies = {{1, 2}};
  model_b->replies = {{8, 9}};
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "encoder_a", model_a,
                                              "same_revision"));
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "encoder_b", model_b,
                                              "same_revision"));
  test_support::NodeProbe first, second;
  ASSERT_TRUE(first.Init("text_embedding", {{"bind_model", "encoder_a"}},
                         session, {{"text", "input", "session"}},
                         {{"embedding", "first", ""}}));
  ASSERT_TRUE(second.Init("text_embedding", {{"bind_model", "encoder_b"}},
                          session, {{"text", "input", "session"}},
                          {{"embedding", "second", ""}}));
  for (int repeat = 0; repeat < 2; ++repeat) {
    AlgContext context;
    ASSERT_TRUE(
        context.Publish("input", TextBatch{{0, 3, "identical corpus"}}));
    ASSERT_EQ(first.node->Process(&context), 0);
    ASSERT_EQ(second.node->Process(&context), 0);
    const auto* output_a = context.Read<EmbeddingBatch>("first");
    const auto* output_b = context.Read<EmbeddingBatch>("second");
    ASSERT_NE(output_a, nullptr);
    ASSERT_NE(output_b, nullptr);
    EXPECT_EQ(output_a->front().data, (std::vector<float>{1, 2}));
    EXPECT_EQ(output_b->front().data, (std::vector<float>{8, 9}));
    EXPECT_EQ(model_a->calls.size(), 1u);
    EXPECT_EQ(model_b->calls.size(), 1u);
  }
}

TEST(TextEmbeddingNodeTest,
     FailedInferenceIsNotCachedAndTheNextAttemptCanSucceed) {
  SessionContext session;
  auto model = std::make_shared<RecordingEmbedding>();
  model->failures_remaining = 1;
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "encoder", model));
  test_support::NodeProbe node;
  ASSERT_TRUE(node.Init("text_embedding", {{"bind_model", "encoder"}}, session,
                        {{"text", "input", "session"}},
                        {{"embedding", "output", ""}}));
  for (int attempt = 0; attempt < 3; ++attempt) {
    AlgContext context;
    ASSERT_TRUE(context.Publish("input", TextBatch{{0, 6, "corpus"}}));
    const int code = node.node->Process(&context);
    if (attempt == 0) {
      EXPECT_EQ(code, -51);
      EXPECT_FALSE(context.Has("output"));
      EXPECT_NE(context.GetErrorMessage().find("temporary encoder failure"),
                std::string::npos);
    } else {
      ASSERT_EQ(code, 0);
      ASSERT_NE(context.Read<EmbeddingBatch>("output"), nullptr);
      EXPECT_EQ(context.Read<EmbeddingBatch>("output")->front().data,
                (std::vector<float>{1, 2}));
    }
    EXPECT_EQ(model->calls.size(), attempt == 0 ? 1u : 2u);
  }
}
}  // namespace
}  // namespace llm_edgeflow
