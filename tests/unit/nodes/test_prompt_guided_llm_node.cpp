#include <gtest/gtest.h>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "tests/support/node_probe.h"
#include "tests/support/recording_models.h"

namespace llm_edgeflow {
namespace {
using test_support::RecordingLlm;

TEST(PromptGuidedLlmNodeTest,
     AggregatesContextsByRequestRendersPromptsAndStripsOuterFences) {
  SessionContext session;
  auto model = std::make_shared<RecordingLlm>();
  model->replies = {
      {" \n```json\n{\"ok\":1}\n```\t", " plain \t", "```text\nlast\n```"}};
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "writer", model));
  test_support::NodeProbe node;
  ASSERT_TRUE(node.Init("prompt_guided_llm",
                        {{"bind_model", "writer"},
                         {"prompt_template", "{{context}}|{{input}}"},
                         {"strip_markdown", true},
                         {"system_prompt", "guide"},
                         {"max_tokens", 19},
                         {"temperature", 0.4},
                         {"top_k", 3},
                         {"top_p", 0.7},
                         {"repetition_penalty", 1.3},
                         {"stop_words", {"stop"}},
                         {"random_seed", 2}},
                        session,
                        {{"input", "input", ""}, {"context", "context", ""}},
                        {{"output", "output", ""}}));
  AlgContext context;
  ASSERT_TRUE(context.Publish(
      "input", TextBatch{{9, 4, "one"}, {2, 7, "two"}, {9, 8, "three"}}));
  ASSERT_TRUE(context.Publish("context", TextBatch{{9, 10, "first"},
                                                   {2, 5, "other"},
                                                   {88, 0, "unrelated"},
                                                   {9, 2, "second"}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  ASSERT_EQ(model->calls.size(), 1u);
  const auto& prompts = model->calls[0];
  ASSERT_EQ(prompts.size(), 3u);
  EXPECT_EQ(prompts[0].data, "first\nsecond|one");
  EXPECT_EQ(prompts[1].data, "other|two");
  EXPECT_EQ(prompts[2].data, "first\nsecond|three");
  EXPECT_EQ(prompts[0].sub_id, 4u);
  EXPECT_EQ(prompts[1].req_id, 2u);
  const auto& options = model->call_options[0];
  EXPECT_EQ(options.system_prompt, "guide");
  EXPECT_EQ(options.max_tokens, 19);
  EXPECT_FLOAT_EQ(options.temperature, 0.4f);
  EXPECT_EQ(options.top_k, 3);
  EXPECT_FLOAT_EQ(options.top_p, 0.7f);
  EXPECT_FLOAT_EQ(options.repetition_penalty, 1.3f);
  EXPECT_EQ(options.stop_words, std::vector<std::string>{"stop"});
  EXPECT_EQ(options.random_seed, 2);
  const auto* output = context.Read<TextBatch>("output");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), 3u);
  EXPECT_EQ((*output)[0].data, R"({"ok":1})");
  EXPECT_EQ((*output)[1].data, "plain");
  EXPECT_EQ((*output)[2].data, "last");
  EXPECT_EQ((*output)[2].req_id, 9u);
  EXPECT_EQ((*output)[2].sub_id, 8u);
}

TEST(PromptGuidedLlmNodeTest,
     DefaultPostprocessingPreservesRawResponseAndLiteralTemplateText) {
  SessionContext session;
  auto model = std::make_shared<RecordingLlm>();
  model->replies = {{"  ```text\nraw\n```  "}};
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "writer", model));
  test_support::NodeProbe node;
  ASSERT_TRUE(
      node.Init("prompt_guided_llm",
                {{"bind_model", "writer"},
                 {"prompt_template", "{literal} {{input}} {\"key\":1}"}},
                session, {{"input", "input", ""}}, {{"output", "output", ""}}));
  AlgContext context;
  ASSERT_TRUE(context.Publish("input", TextBatch{{1, 3, "Q {{context}}"}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  EXPECT_EQ(model->calls.front().front().data,
            "{literal} Q {{context}} {\"key\":1}");
  EXPECT_EQ(context.Read<TextBatch>("output")->front().data,
            "  ```text\nraw\n```  ");
}

TEST(PromptGuidedLlmNodeTest,
     UsedContextRequiresABindingAndARequestValueBeforeGeneration) {
  SessionContext session;
  auto model = std::make_shared<RecordingLlm>();
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "writer", model));
  const nlohmann::json params = {{"bind_model", "writer"},
                                 {"prompt_template", "{{input}}|{{context}}"}};
  test_support::NodeProbe unconnected;
  EXPECT_FALSE(unconnected.Init("prompt_guided_llm", params, session,
                                {{"input", "input", ""}},
                                {{"output", "output", ""}}));
  EXPECT_NE(unconnected.diagnostic.find("context"), std::string::npos);
  test_support::NodeProbe connected;
  ASSERT_TRUE(
      connected.Init("prompt_guided_llm", params, session,
                     {{"input", "input", ""}, {"context", "context", ""}},
                     {{"output", "output", ""}}));
  AlgContext missing;
  ASSERT_TRUE(missing.Publish("input", TextBatch{{1, 0, "Q"}}));
  EXPECT_NE(connected.node->Process(&missing), 0);
  EXPECT_FALSE(missing.Has("output"));
  EXPECT_TRUE(model->calls.empty());
  EXPECT_NE(missing.GetErrorMessage().find("context"), std::string::npos);
}

TEST(PromptGuidedLlmNodeTest,
     PresentContextWithoutMatchingRequestRendersAnEmptyContext) {
  SessionContext session;
  auto model = std::make_shared<RecordingLlm>();
  model->replies = {{"answer"}};
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "writer", model));
  test_support::NodeProbe node;
  ASSERT_TRUE(node.Init(
      "prompt_guided_llm",
      {{"bind_model", "writer"}, {"prompt_template", "{{input}}|{{context}}"}},
      session, {{"input", "input", ""}, {"context", "context", ""}},
      {{"output", "output", ""}}));
  AlgContext context;
  ASSERT_TRUE(context.Publish("input", TextBatch{{7, 6, "Q"}}));
  ASSERT_TRUE(
      context.Publish("context", TextBatch{{99, 0, "not for request seven"}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  EXPECT_EQ(model->calls.front().front().data, "Q|");
  EXPECT_EQ(context.Read<TextBatch>("output")->front().data, "answer");
}
}  // namespace
}  // namespace llm_edgeflow
