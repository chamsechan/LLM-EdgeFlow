#include <gtest/gtest.h>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "engine/model_interface.h"
#include "tests/support/node_probe.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;
class RecordingLlm final : public ILlmModel {
 public:
  const std::string& ImplName() const noexcept override { return impl; }
  const std::string& ModelType() const noexcept override { return type; }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  bool SupportsRandomSeed() const noexcept override {
    ++seed_checks;
    return supports_seed;
  }
  int Generate(const TextBatch& prompts, const GenerateOptions& options,
               TextBatch* output, std::string* diagnostic) noexcept override {
    if (diagnostic) diagnostic->clear();
    calls.push_back(prompts);
    call_options.push_back(options);
    output->clear();
    if (fail_call == calls.size()) {
      if (diagnostic) *diagnostic = "endpoint generation failed";
      return -61;
    }
    const auto& replies = responses.at(calls.size() - 1);
    for (size_t i = 0; i < prompts.size(); ++i)
      output->emplace_back(prompts[i].req_id, prompts[i].sub_id, replies.at(i));
    return 0;
  }
  std::string impl = "node_recording_llm", type = "llm";
  bool supports_seed = true;
  mutable int seed_checks = 0;
  size_t fail_call = 0;
  std::vector<std::vector<std::string>> responses;
  std::vector<TextBatch> calls;
  std::vector<GenerateOptions> call_options;
};

TEST(LlmGenerateNodeTest,
     SingleEndpointKeepsRawTextAndWrapsItAsAStringInTheDocument) {
  SessionContext session;
  auto model = std::make_shared<RecordingLlm>();
  model->responses = {{"  raw\n答案 ", "{\"number\":7}"}};
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "writer", model));
  test_support::NodeProbe node;
  ASSERT_TRUE(
      node.Init("llm_generate",
                {{"bind_model", "writer"},
                 {"endpoints", {{"answer", {{"prompt", "Q={{input}}"}}}}}},
                session, {{"input", "input", ""}},
                {{"text", "text", ""}, {"document", "docs", ""}}));
  AlgContext context;
  ASSERT_TRUE(
      context.Publish("input", TextBatch{{9, 4, "one"}, {2, 7, "two"}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  ASSERT_EQ(model->calls.size(), 1u);
  EXPECT_EQ(model->calls[0][0].data, "Q=one");
  EXPECT_EQ(model->calls[0][1].data, "Q=two");
  const auto* text = context.Read<TextBatch>("text");
  const auto* docs = context.Read<StructuredDocumentBatch>("docs");
  ASSERT_NE(text, nullptr);
  ASSERT_NE(docs, nullptr);
  ASSERT_EQ(text->size(), 2u);
  ASSERT_EQ(docs->size(), 2u);
  EXPECT_EQ((*text)[0].data, "  raw\n答案 ");
  EXPECT_EQ((*text)[1].data, "{\"number\":7}");
  EXPECT_EQ((*docs)[0].data.structured_data,
            Json({{"answer", "  raw\n答案 "}}));
  EXPECT_EQ((*docs)[1].data.structured_data,
            Json({{"answer", "{\"number\":7}"}}));
  EXPECT_EQ((*docs)[1].data.json_payload, R"({"answer":"{\"number\":7}"})");
  EXPECT_TRUE((*docs)[0].data.is_valid);
  EXPECT_EQ((*docs)[0].req_id, 9u);
  EXPECT_EQ((*text)[1].sub_id, 7u);
}

TEST(LlmGenerateNodeTest,
     MultipleEndpointsPublishCompleteJsonAndForwardEveryCallOption) {
  SessionContext session;
  auto model = std::make_shared<RecordingLlm>();
  model->responses = {{"A猫", "A2"}, {"S9", "S2"}};
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "writer", model));
  test_support::NodeProbe node;
  ASSERT_TRUE(node.Init("llm_generate",
                        {{"bind_model", "writer"},
                         {"endpoints",
                          {{"summary", {{"prompt", "Short/{{input}}"}}},
                           {"answer", {{"prompt", "Answer: {{input}}"}}}}},
                         {"system_prompt", "multi"},
                         {"max_tokens", 37},
                         {"temperature", 0.2},
                         {"top_k", 4},
                         {"top_p", 0.8},
                         {"repetition_penalty", 1.1},
                         {"stop_words", {"<end>"}},
                         {"random_seed", 7}},
                        session, {{"input", "input", ""}},
                        {{"text", "text", ""}, {"document", "docs", ""}}));
  EXPECT_EQ(model->seed_checks, 1);
  AlgContext context;
  ASSERT_TRUE(
      context.Publish("input", TextBatch{{9, 4, "one"}, {2, 7, "two"}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  ASSERT_EQ(model->calls.size(), 2u);
  EXPECT_EQ(model->calls[0][0].data, "Answer: one");
  EXPECT_EQ(model->calls[0][1].data, "Answer: two");
  EXPECT_EQ(model->calls[1][0].data, "Short/one");
  EXPECT_EQ(model->calls[1][1].data, "Short/two");
  for (const auto& options : model->call_options) {
    EXPECT_EQ(options.system_prompt, "multi");
    EXPECT_EQ(options.max_tokens, 37);
    EXPECT_FLOAT_EQ(options.temperature, 0.2f);
    EXPECT_EQ(options.top_k, 4);
    EXPECT_FLOAT_EQ(options.top_p, 0.8f);
    EXPECT_FLOAT_EQ(options.repetition_penalty, 1.1f);
    EXPECT_EQ(options.stop_words, std::vector<std::string>{"<end>"});
    EXPECT_EQ(options.random_seed, 7);
  }
  EXPECT_EQ(model->seed_checks, 1);
  const auto* text = context.Read<TextBatch>("text");
  const auto* docs = context.Read<StructuredDocumentBatch>("docs");
  ASSERT_NE(text, nullptr);
  ASSERT_NE(docs, nullptr);
  ASSERT_EQ(text->size(), 2u);
  ASSERT_EQ(docs->size(), 2u);
  const std::vector<std::string> expected = {
      R"({"answer":"A猫","summary":"S9"})",
      R"({"answer":"A2","summary":"S2"})"};
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ((*text)[i].data, expected[i]);
    EXPECT_EQ((*docs)[i].data.json_payload, expected[i]);
    EXPECT_EQ((*docs)[i].data.structured_data, Json::parse(expected[i]));
    EXPECT_TRUE((*docs)[i].data.is_valid);
  }
}

TEST(LlmGenerateNodeTest,
     SecondEndpointFailurePublishesNeitherPartiallyFilledOutput) {
  SessionContext session;
  auto model = std::make_shared<RecordingLlm>();
  model->responses = {{"first answer"}};
  model->fail_call = 2;
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "writer", model));
  test_support::NodeProbe node;
  ASSERT_TRUE(node.Init("llm_generate",
                        {{"bind_model", "writer"},
                         {"endpoints",
                          {{"a", {{"prompt", "first:{{input}}"}}},
                           {"b", {{"prompt", "second:{{input}}"}}}}}},
                        session, {{"input", "input", ""}},
                        {{"text", "text", ""}, {"document", "docs", ""}}));
  AlgContext context;
  ASSERT_TRUE(context.Publish("input", TextBatch{{1, 8, "question"}}));
  EXPECT_EQ(node.node->Process(&context), -61);
  ASSERT_EQ(model->calls.size(), 2u);
  EXPECT_EQ(model->calls[1][0].data, "second:question");
  EXPECT_FALSE(context.Has("text"));
  EXPECT_FALSE(context.Has("docs"));
  EXPECT_NE(context.GetErrorMessage().find("Endpoint 'b'"), std::string::npos);
}

TEST(LlmGenerateNodeTest,
     InvalidUtf8SingleAnswerKeepsRawTextButMarksDocumentInvalid) {
  SessionContext session;
  auto model = std::make_shared<RecordingLlm>();
  const std::string invalid("\xff", 1);
  model->responses = {{invalid}};
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "writer", model));
  test_support::NodeProbe node;
  ASSERT_TRUE(node.Init(
      "llm_generate",
      {{"bind_model", "writer"}, {"endpoints", {{"answer", Json::object()}}}},
      session, {{"input", "input", ""}},
      {{"text", "text", ""}, {"document", "docs", ""}}));
  AlgContext context;
  ASSERT_TRUE(context.Publish("input", TextBatch{{4, 3, "question"}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  ASSERT_NE(context.Read<TextBatch>("text"), nullptr);
  EXPECT_EQ(context.Read<TextBatch>("text")->front().data, invalid);
  const auto* docs = context.Read<StructuredDocumentBatch>("docs");
  ASSERT_NE(docs, nullptr);
  EXPECT_FALSE(docs->front().data.is_valid);
  EXPECT_EQ(docs->front().data.parse_status, JsonParseStatus::kFailed);
  EXPECT_EQ(docs->front().data.structured_data["answer"], invalid);
  EXPECT_TRUE(docs->front().data.json_payload.empty());
  EXPECT_FALSE(docs->front().data.diagnostic.empty());
}

TEST(LlmGenerateNodeTest,
     SharedModelReceivesDifferentNodeOptionsWithoutSharedMutableSettings) {
  SessionContext session;
  auto model = std::make_shared<RecordingLlm>();
  model->responses = {{"first"}, {"second"}};
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "writer", model));
  test_support::NodeProbe first, second;
  const Json endpoints = {{"answer", Json::object()}};
  ASSERT_TRUE(first.Init("llm_generate",
                         {{"bind_model", "writer"},
                          {"endpoints", endpoints},
                          {"system_prompt", "first system"},
                          {"max_tokens", 11},
                          {"random_seed", 101}},
                         session, {{"input", "input", ""}},
                         {{"text", "first", ""}}));
  ASSERT_TRUE(second.Init("llm_generate",
                          {{"bind_model", "writer"},
                           {"endpoints", endpoints},
                           {"system_prompt", "second system"},
                           {"max_tokens", 22},
                           {"random_seed", 202}},
                          session, {{"input", "input", ""}},
                          {{"text", "second", ""}}));
  AlgContext context;
  ASSERT_TRUE(context.Publish("input", TextBatch{{1, 0, "same input"}}));
  ASSERT_EQ(first.node->Process(&context), 0);
  ASSERT_EQ(second.node->Process(&context), 0);
  ASSERT_EQ(model->call_options.size(), 2u);
  EXPECT_EQ(model->call_options[0].system_prompt, "first system");
  EXPECT_EQ(model->call_options[0].max_tokens, 11);
  EXPECT_EQ(model->call_options[0].random_seed, 101);
  EXPECT_EQ(model->call_options[1].system_prompt, "second system");
  EXPECT_EQ(model->call_options[1].max_tokens, 22);
  EXPECT_EQ(model->call_options[1].random_seed, 202);
  EXPECT_EQ(model->seed_checks, 2);
}

TEST(LlmGenerateNodeTest, UnsupportedExplicitSeedFailsOnlyAtInitialization) {
  SessionContext session;
  auto model = std::make_shared<RecordingLlm>();
  model->supports_seed = false;
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "writer", model));
  test_support::NodeProbe node;
  EXPECT_FALSE(node.Init("llm_generate",
                         {{"bind_model", "writer"},
                          {"random_seed", 3},
                          {"endpoints", {{"answer", Json::object()}}}},
                         session, {{"input", "input", ""}},
                         {{"text", "text", ""}}));
  EXPECT_EQ(model->seed_checks, 1);
  EXPECT_TRUE(model->calls.empty());
  EXPECT_NE(node.diagnostic.find("random_seed"), std::string::npos);
}
}  // namespace
}  // namespace llm_edgeflow
