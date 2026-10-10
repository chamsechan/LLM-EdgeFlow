#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline_validator.h"
#include "core/session_context.h"
#include "tests/support/model_registration.h"
#include "tests/support/node_test_utils.h"
#include "tests/support/pipeline_test_utils.h"

namespace llm_edgeflow {

class PromptGuidedLlmNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    session_ctx_ = std::make_unique<SessionContext>();
    RuntimeOptions options;
    options.device_id = 0;
    session_ctx_->SetRuntimeOptions(options);
  }

  std::unique_ptr<SessionContext> session_ctx_;
};

TEST_F(PromptGuidedLlmNodeTest,
       PromptRendersOriginalTemplateAndIsolatesRequests) {
  auto model = std::make_shared<test::PromptCaptureLlmModel>();
  ASSERT_TRUE(RegisterTestModel(session_ctx_->GetModelManager(),
                                "prompt_contract", model, "v1"));
  auto node = NodeRegistry::Instance().Create("prompt_guided_llm");
  const nlohmann::json config = {
      {"bind_model", "prompt_contract"},
      {"prompt_template",
       "system {input}\n{\"key\": \"val\"} <{{input}}>|{{context}}|{{input}}"},
      {"strip_markdown", true},
      {"temperature", 0.25},
      {"top_k", 19},
      {"top_p", 0.6},
      {"repetition_penalty", 1.3},
      {"stop_words", {"END"}},
      {"max_tokens", 23}};
  ASSERT_TRUE(InitNodeForTest(*node, config, session_ctx_.get()));
  AlgContext ctx;
  ctx.Publish("input", TextBatch{{17, 4, "literal {context}"},
                                 {29, 8, "second"},
                                 {17, 6, "last"}});
  ctx.Publish("context", TextBatch{{29, 2, "OTHER"},
                                   {17, 3, "C{input}"},
                                   {17, 9, "TAIL"},
                                   {33, 0, "UNRELATED"}});
  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* output = ctx.Read<TextBatch>("output");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), 3U);
  EXPECT_EQ((*output)[0].data,
            "system {input}\n{\"key\": \"val\"} <literal "
            "{context}>|C{input}\nTAIL|literal {context}");
  EXPECT_EQ((*output)[1].data,
            "system {input}\n{\"key\": \"val\"} <second>|OTHER|second");
  EXPECT_EQ((*output)[2].data,
            "system {input}\n{\"key\": \"val\"} <last>|C{input}\nTAIL|last");
  EXPECT_EQ((*output)[0].req_id, 17U);
  EXPECT_EQ((*output)[2].sub_id, 6U);
  EXPECT_FLOAT_EQ(model->last_options.temperature, 0.25f);
  EXPECT_EQ(model->last_options.top_k, 19);
  EXPECT_FLOAT_EQ(model->last_options.top_p, 0.6f);
  EXPECT_FLOAT_EQ(model->last_options.repetition_penalty, 1.3f);
  EXPECT_EQ(model->last_options.max_tokens, 23);
  EXPECT_EQ(model->last_options.stop_words, std::vector<std::string>{"END"});
  // 用新的请求数据复用同一个 Node；之前的上下文不得残留。
  AlgContext next;
  next.Publish("input", TextBatch{{29, 1, "next"}});
  next.Publish("context", TextBatch{});
  ASSERT_EQ(node->Process(&next), 0);
  EXPECT_EQ(next.Read<TextBatch>("output")->front().data,
            "system {input}\n{\"key\": \"val\"} <next>||next");
}

TEST_F(PromptGuidedLlmNodeTest, PromptDefaultsMatchFixtureAndNativePlans) {
  auto model = std::make_shared<test::PromptCaptureLlmModel>();
  ASSERT_TRUE(RegisterTestModel(session_ctx_->GetModelManager(), "entity_llm",
                                model, "v1"));
  const nlohmann::json config = {{"bind_model", "entity_llm"}};
  auto document = LoadCorePipelineFixture(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  document["pipeline"][0]["params"] = config;
  const auto validated = PipelineValidator::ValidateAndPlan(
      document, MakeTestBoundary(
                    {{"input.sentence_text", "TextBatch"}},
                    {{"parse_entities.document", "StructuredDocumentBatch"}}));
  ASSERT_TRUE(validated.report.ok) << validated.report.ToJson().dump(2);
  const auto& plan = validated.node_plans.at("generate_entities");
  const std::string input = R"({"name":"literal {context}"})";
  EXPECT_EQ(plan.normalized_params["max_tokens"], 128);
  for (bool native_plan : {false, true}) {
    SCOPED_TRACE(native_plan);
    auto node = NodeRegistry::Instance().Create("prompt_guided_llm");
    ASSERT_NE(node, nullptr);
    std::string error = "old error";
    NodeInitContext init;
    init.plan = &plan;
    init.session_ctx = session_ctx_.get();
    init.diagnostic = &error;
    if (native_plan) {
      ASSERT_TRUE(node->Init(init)) << error;
    } else {
      ASSERT_TRUE(InitNodeForTest(*node, config, session_ctx_.get(), &error))
          << error;
    }
    EXPECT_TRUE(error.empty());
    AlgContext context;
    const std::string input_key = native_plan ? "input.sentence_text" : "input";
    const std::string output_key =
        native_plan ? "generate_entities.output" : "output";
    context.Publish(input_key, TextBatch{{77, 4, input}});
    ASSERT_EQ(node->Process(&context), 0) << context.GetErrorMessage();
    ASSERT_EQ(model->prompts.size(), 1u);
    EXPECT_EQ(model->prompts.front().data, input);
    EXPECT_FLOAT_EQ(model->last_options.temperature, 0.7f);
    EXPECT_EQ(model->last_options.max_tokens, 128);
    EXPECT_EQ(model->last_options.top_k, 0);
    EXPECT_FLOAT_EQ(model->last_options.top_p, 0.9f);
    EXPECT_FLOAT_EQ(model->last_options.repetition_penalty, 1.0f);
    EXPECT_TRUE(model->last_options.stop_words.empty());
    const auto* output = context.Read<TextBatch>(output_key);
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->size(), 1u);
    EXPECT_EQ(output->front().data, "```text\n" + input + "\n```");
    EXPECT_EQ(output->front().req_id, 77u);
    EXPECT_EQ(output->front().sub_id, 4u);
  }
}

TEST_F(PromptGuidedLlmNodeTest, PromptStandardSyntaxMatchesTextTemplateNode) {
  auto model = std::make_shared<test::PromptCaptureLlmModel>();
  ASSERT_TRUE(RegisterTestModel(session_ctx_->GetModelManager(),
                                "prompt_contract", model, "v1"));
  const std::string value = "opaque {{input}} {context}";
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"{{context}}|{{ context }}", value + "|" + value},
      {R"({"nested":{"context":"{{ context }}"}})",
       "{\"nested\":{\"context\":\"" + value + "\"}}"}};
  for (const auto& [pattern, expected] : cases) {
    SCOPED_TRACE(pattern);
    auto common = NodeRegistry::Instance().Create("text_template");
    auto custom = NodeRegistry::Instance().Create("prompt_guided_llm");
    ASSERT_TRUE(
        InitNodeForTest(*common, {{"template", pattern}}, session_ctx_.get()));
    ASSERT_TRUE(InitNodeForTest(
        *custom,
        {{"bind_model", "prompt_contract"}, {"prompt_template", pattern}},
        session_ctx_.get()));
    AlgContext common_ctx;
    common_ctx.Publish("context",
                       RankedTextBatch{{17, 0, RankedCandidate(value, 1.0f)}});
    ASSERT_EQ(common->Process(&common_ctx), 0);
    ASSERT_NE(common_ctx.Read<TextBatch>("text"), nullptr);
    ASSERT_EQ(common_ctx.Read<TextBatch>("text")->size(), 1U);
    EXPECT_EQ(common_ctx.Read<TextBatch>("text")->front().data, expected);

    AlgContext custom_ctx;
    custom_ctx.Publish("input", TextBatch{{17, 0, "unused"}});
    custom_ctx.Publish("context", TextBatch{{17, 0, value}});
    ASSERT_EQ(custom->Process(&custom_ctx), 0);
    ASSERT_EQ(model->prompts.size(), 1U);
    EXPECT_EQ(model->prompts.front().data, expected);
  }
}

TEST_F(PromptGuidedLlmNodeTest, PromptTemplatePreservesJsonLiterals) {
  auto model = std::make_shared<test::PromptCaptureLlmModel>();
  ASSERT_TRUE(RegisterTestModel(session_ctx_->GetModelManager(),
                                "prompt_contract", model, "v1"));
  for (const auto& [pattern, expected] :
       std::vector<std::pair<std::string, std::string>>{
           {"{\"text\": \"{{input}}\"}", "{\"text\": \"value\"}"},
           {"{{ input }}|{literal_braces}", "value|{literal_braces}"},
           {"prefix: {{input}} | suffix", "prefix: value | suffix"}}) {
    SCOPED_TRACE(pattern);
    auto node = NodeRegistry::Instance().Create("prompt_guided_llm");
    ASSERT_TRUE(InitNodeForTest(
        *node,
        {{"bind_model", "prompt_contract"}, {"prompt_template", pattern}},
        session_ctx_.get()));
    AlgContext ctx;
    ctx.Publish("input", TextBatch{{17, 4, "value"}});
    ASSERT_EQ(node->Process(&ctx), 0);
    EXPECT_EQ(model->prompts.back().data, expected);
  }
}

TEST_F(PromptGuidedLlmNodeTest, PromptContextIsExplicitAndRequiredWhenUsed) {
  auto node = NodeRegistry::Instance().Create("prompt_guided_llm");
  auto model = std::make_shared<test::PromptCaptureLlmModel>();
  ASSERT_TRUE(RegisterTestModel(session_ctx_->GetModelManager(),
                                "prompt_contract", model, "v1"));
  ASSERT_TRUE(InitNodeForTest(*node, {{"bind_model", "prompt_contract"}},
                              session_ctx_.get()));
  AlgContext implicit;
  implicit.Publish("input", TextBatch{{1, 0, "input only"}});
  implicit.Publish("context", TextBatch{{1, 0, "must not append"}});
  ASSERT_EQ(node->Process(&implicit), 0);
  EXPECT_EQ(model->prompts.front().data, "input only");
  ASSERT_TRUE(InitNodeForTest(*node,
                              {{"bind_model", "prompt_contract"},
                               {"prompt_template", "{{input}}|{{context}}"}},
                              session_ctx_.get()));
  for (bool wrong_type : {false, true}) {
    AlgContext missing;
    missing.Publish("input", TextBatch{{1, 0, "input"}});
    if (wrong_type) missing.Publish("context", Int32Batch{});
    EXPECT_EQ(node->Process(&missing), -8001);
    EXPECT_FALSE(missing.Has("output"));
  }
  EXPECT_EQ(model->calls, 1);
}

TEST_F(PromptGuidedLlmNodeTest,
       PromptConfigurationRejectedBeforeInitialization) {
  const std::vector<nlohmann::json> bad_configs = {
      {{"stop_words", {123, ""}}},
      {{"stop_words", {""}}},
      {{"stop_words", "END"}},
      {{"unknown_field", true}},
      {{"strip_markdown", "yes"}},
      {{"max_tokens", 32769}},
      {{"max_tokens", 2.5}},
      {{"max_tokens", 4294967297ULL}},
      {{"temperature", 2.1}},
      {{"top_k", -1}},
      {{"top_p", 0}},
      {{"repetition_penalty", 101}},
      {{"prompt_template", ""}},
      {{"prompt_template", "{{unknown}}"}},
      {{"prompt_template", "{{input"}},
      {{"prompt_template", "{{}}"}},
      {{"prompt_template", "{{invalid name}}"}}};
  for (const auto& bad : bad_configs) {
    SCOPED_TRACE(bad.dump());
    auto doc = LoadCorePipelineFixture(
        "demo/fixtures/mock/pipeline_entity_extract_custom.json");
    nlohmann::json config = {{"bind_model", "entity_llm"},
                             {"prompt_template", "{{input}}"}};
    config.update(bad);
    doc["pipeline"][0]["params"] = config;
    const auto preflight = PipelineValidator::ValidateAndPlan(
        doc, MakeTestBoundary(
                 {{"input.sentence_text", "TextBatch"}},
                 {{"parse_entities.document", "StructuredDocumentBatch"}}));
    EXPECT_FALSE(preflight.report.ok);
    EXPECT_TRUE(std::any_of(preflight.report.diagnostics.begin(),
                            preflight.report.diagnostics.end(),
                            [](const auto& diagnostic) {
                              return diagnostic.path.rfind("/pipeline/0/params",
                                                           0) == 0;
                            }))
        << preflight.report.ToJson().dump(2);
  }
  auto doc =
      LoadCorePipelineFixture("demo/fixtures/mock/pipeline_doc_qa_custom.json");
  doc["pipeline"][2]["inputs"].erase("context");
  EXPECT_FALSE(
      PipelineValidator::ValidateAndPlan(
          doc, MakeTestBoundary({{"input.doc_text", "TextBatch"},
                                 {"input.query_text", "TextBatch"}},
                                {{"generate_answer.output", "TextBatch"},
                                 {"match_intent.matches", "RuleMatchBatch"},
                                 {"chunk_docs.chunk_counts", "Int32Batch"}}))
          .report.ok);
}

}  // namespace llm_edgeflow
