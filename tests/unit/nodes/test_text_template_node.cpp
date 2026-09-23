#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/pipeline_catalog.h"
#include "core/session_context.h"
#include "tests/support/node_process_pause.h"
#include "tests/support/node_test_utils.h"
#include "tests/support/pipeline_test_utils.h"
#include "tests/support/scoped_allocation_failure.h"

namespace llm_edgeflow {

class TextTemplateNodeTest : public ::testing::Test {
 protected:
  void SetUp() override { session_ctx_ = std::make_unique<SessionContext>(); }
  std::unique_ptr<SessionContext> session_ctx_;
};

std::string ResolveConfigPath(const std::string& relative) {
  FILE* file = fopen(relative.c_str(), "r");
  if (file) {
    fclose(file);
    return relative;
  }
  return "../" + relative;
}

// 1. Process Multi-Input Aggregation
TEST_F(TextTemplateNodeTest, ProcessMultiInputAggregation) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {
      {"template",
       "Query: {{primary}}\nContext: {{context}}\nCategory: {{matches}}"},
      {"separator", " | "}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch primary;
  primary.emplace_back(1, 0, "How do I upgrade?");

  RankedTextBatch context;
  context.emplace_back(1, 0, RankedCandidate("Step 1: Go to settings", 0.9f));
  context.emplace_back(1, 1, RankedCandidate("Step 2: Click upgrade", 0.8f));

  RuleMatchBatch matches;
  matches.emplace_back(1, 0, RuleMatchItem(1, "ACCOUNT_UPGRADE", "upgrade"));

  ctx.Publish("primary", primary);
  ctx.Publish("context", context);
  ctx.Publish("matches", matches);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Read<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 1u);
  EXPECT_NE((*out)[0].data.find("Go to settings | Step 2: Click upgrade"),
            std::string::npos);
  EXPECT_NE((*out)[0].data.find("ACCOUNT_UPGRADE"), std::string::npos);
}

// 2. Missing Required Variable Fails Closed
TEST_F(TextTemplateNodeTest, MissingRequiredVariableFailsClosed) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"template", "Hello {{user_name}}, welcome!"},
                        {"allow_dynamic_attributes", true},
                        {"missing_variable_policy", "fail"}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  // Attributes missing required variable 'user_name'
  AlgContext ctx;
  TextAttributesBatch attrs;
  attrs.emplace_back(
      1, 0,
      std::unordered_map<std::string, std::string>{{"other_key", "value"}});
  ctx.Publish("attributes", attrs);

  EXPECT_EQ(node->Process(&ctx), -6202);
}

// 3. Dynamic Attribute Successfully Rendered
TEST_F(TextTemplateNodeTest, DynamicAttributeRendered) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"template", "Hello {{user_name}}, welcome!"},
                        {"allow_dynamic_attributes", true}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  TextAttributesBatch attrs;
  attrs.emplace_back(
      1, 0,
      std::unordered_map<std::string, std::string>{{"user_name", "Alice"}});
  ctx.Publish("attributes", attrs);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Read<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  EXPECT_EQ((*out)[0].data, "Hello Alice, welcome!");
}

// 4. Single Braces Treated As Literal Text
TEST_F(TextTemplateNodeTest, SingleBraceTreatedAsLiteral) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);

  // Single braces like {user_name} and JSON objects {"k": 1} must remain
  // literal text. Only double braces {{var}} are treated as template variables.
  nlohmann::json cfg = {
      {"template",
       "Literal: {user_name}, JSON: {\"key\": 1}, Var: {{user_name}}"},
      {"allow_dynamic_attributes", true}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  TextAttributesBatch attrs;
  attrs.emplace_back(
      1, 0,
      std::unordered_map<std::string, std::string>{{"user_name", "Alice"}});
  ctx.Publish("attributes", attrs);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Read<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  EXPECT_EQ((*out)[0].data,
            "Literal: {user_name}, JSON: {\"key\": 1}, Var: Alice");
}

// 5. Malformed Placeholders Rejected At Initialization
TEST_F(TextTemplateNodeTest, MalformedPlaceholderFailsInit) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);

  for (const std::string bad_tmpl :
       {"Hello {{unclosed", "Hello {{}}", "Hello {{invalid name}}"}) {
    nlohmann::json cfg = {{"template", bad_tmpl},
                          {"allow_dynamic_attributes", true}};
    EXPECT_FALSE(InitNodeForTest(*node, cfg, session_ctx_.get()));
  }
}

TEST_F(TextTemplateNodeTest, TruncatePreservesUtf8CodePointBoundaries) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node,
                              {{"template", "{{primary}}"},
                               {"max_length", 4},
                               {"overflow_policy", "truncate"}},
                              session_ctx_.get()));

  AlgContext ctx;
  TextBatch primary;
  primary.emplace_back(1, 0, u8"中文");
  primary.emplace_back(2, 0, u8"A🙂B");
  ctx.Publish("primary", std::move(primary));

  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Read<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 2u);
  EXPECT_EQ((*out)[0].data, u8"中");
  EXPECT_EQ((*out)[1].data, "A");
}

TEST_F(TextTemplateNodeTest, TruncateRejectsInvalidUtf8) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node,
                              {{"template", "{{primary}}"},
                               {"max_length", 2},
                               {"overflow_policy", "truncate"}},
                              session_ctx_.get()));

  AlgContext ctx;
  TextBatch primary;
  primary.emplace_back(1, 0, std::string("A") + "\xF0\x9F");
  ctx.Publish("primary", std::move(primary));

  EXPECT_EQ(node->Process(&ctx), -6203);
  EXPECT_EQ(ctx.Read<TextBatch>("text"), nullptr);
}

// 4. Control Command Hot-Swap & Bogus Rejection
TEST_F(TextTemplateNodeTest, ControlCommandHotSwapAndBogusRejection) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"template", "{{primary}}"}},
                              session_ctx_.get()));

  // Valid update
  nlohmann::json valid_update = {
      {"template", "Updated: {{primary}} [{{context}}]"}};
  NodeControlResult res =
      node->Control(kControlCmdUpdatePrompt, valid_update.dump());
  EXPECT_EQ(res.status, NodeControlStatus::kHandled);

  // Operator metadata is accepted independently of a template change.
  NodeControlResult prompt_id_res = node->Control(
      kControlCmdUpdatePrompt, nlohmann::json{{"prompt_id", "qa-v2"}}.dump());
  EXPECT_EQ(prompt_id_res.status, NodeControlStatus::kHandled);

  NodeControlResult removed_field_res =
      node->Control(kControlCmdUpdatePrompt,
                    nlohmann::json{{"prompt_template", "removed"}}.dump());
  EXPECT_EQ(removed_field_res.status, NodeControlStatus::kFailed);

  NodeControlResult invalid_policy_res = node->Control(
      kControlCmdUpdatePrompt,
      nlohmann::json{{"missing_variable_policy", "invent"}}.dump());
  EXPECT_EQ(invalid_policy_res.status, NodeControlStatus::kFailed);

  // Bogus update with no valid fields -> Rejected
  nlohmann::json bogus_update = {{"bogus_field", 123}};
  NodeControlResult bogus_res =
      node->Control(kControlCmdUpdatePrompt, bogus_update.dump());
  EXPECT_EQ(bogus_res.status, NodeControlStatus::kFailed);

  EXPECT_EQ(node->Control(kControlCmdUpdatePrompt,
                          nlohmann::json{{"template", "{{unclosed"}}.dump())
                .status,
            NodeControlStatus::kFailed);
  AlgContext after_failure;
  after_failure.Publish("primary", TextBatch{{17, 4, "{{context}}"}});
  after_failure.Publish("context", RankedTextBatch{});
  ASSERT_EQ(node->Process(&after_failure), 0);
  ASSERT_NE(after_failure.Read<TextBatch>("text"), nullptr);
  EXPECT_EQ(after_failure.Read<TextBatch>("text")->front().data,
            "Updated: {{context}} []");
}

TEST_F(TextTemplateNodeTest, PipelineEnforcesPublishedControlSchema) {
  Pipeline pipeline;
  PipelineDiagnostic diagnostic;
  std::ifstream cfg_in(
      ResolveConfigPath("demo/fixtures/mock/pipeline_doc_qa.json"));
  ASSERT_TRUE(cfg_in.is_open());
  nlohmann::json pipe_json;
  cfg_in >> pipe_json;
  pipe_json.erase("deployment");
  ASSERT_TRUE(BuildTestPipeline(pipeline, pipe_json, &diagnostic))
      << diagnostic.message;

  EXPECT_NE(pipeline.Control(kControlCmdUpdatePrompt, "{}"), 0);
  EXPECT_NE(pipeline.Control(
                kControlCmdUpdatePrompt,
                nlohmann::json{{"template", "{{primary}}"}, {"unknown", true}}
                    .dump()),
            0);
  EXPECT_NE(pipeline.Control(
                kControlCmdUpdatePrompt,
                nlohmann::json{{"missing_variable_policy", "invent"}}.dump()),
            0);
  EXPECT_EQ(pipeline.Control(kControlCmdUpdatePrompt,
                             nlohmann::json{{"prompt_id", "doc-qa-v2"}}.dump()),
            0);
}

namespace {
nlohmann::json TemplatePipeline(const nlohmann::json& config) {
  auto root = nlohmann::json::parse(R"({
  "biz_name": "keyword_match_v1",
  "models": [],
  "pipeline": [
    {
      "id": "template",
      "node_type": "TextTemplateNode",
      "config": {},
      "inputs": {
        "primary": "input_sentences"
      },
      "outputs": {
        "text": "rendered_text"
      }
    },
    {
      "id": "rules",
      "node_type": "TextRuleMatchNode",
      "config": {},
      "inputs": {
        "text": "rendered_text"
      },
      "outputs": {
        "matches": "rule_matches"
      }
    }
  ]
})");
  root["pipeline"][0]["config"] = config;
  return root;
}
}  // namespace

TEST_F(TextTemplateNodeTest, UnconnectedBuiltinUsesDeclaredMissingPolicy) {
  for (const std::string variable :
       {"context", "context_text", "matches", "document", "document_text"}) {
    SCOPED_TRACE(variable);
    const std::string pattern = "Q={{primary}}|V={{" + variable + "}}";
    auto root = TemplatePipeline({{"template", pattern}});
    const auto invalid = PipelineValidator::ValidateAndPlan(root);
    ASSERT_FALSE(invalid.report.ok);
    EXPECT_TRUE(
        std::any_of(invalid.report.diagnostics.begin(),
                    invalid.report.diagnostics.end(), [&](const auto& d) {
                      return d.path == "/pipeline/0/config" &&
                             d.message.find(variable) != std::string::npos;
                    }));
    for (const std::string policy : {"empty", "preserve"}) {
      root["pipeline"][0]["config"]["missing_variable_policy"] = policy;
      Pipeline pipeline;
      PipelineDiagnostic diagnostic;
      ASSERT_TRUE(BuildTestPipeline(pipeline, root, &diagnostic))
          << diagnostic.message;
      AlgContext ctx;
      ctx.Publish("input_sentences", TextBatch{{1, 3, "hello"}});
      ASSERT_EQ(pipeline.Execute(&ctx), 0);
      ASSERT_NE(ctx.Read<TextBatch>("rendered_text"), nullptr);
      EXPECT_EQ(
          ctx.Read<TextBatch>("rendered_text")->front().data,
          "Q=hello|V=" + (policy == "empty" ? "" : "{{" + variable + "}}"));
    }
  }
}

TEST_F(TextTemplateNodeTest,
       MissingRuntimeBuiltinFailsButEmptyAggregateIsValid) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_TRUE(InitNodeForTest(*node, {{"template", "{{primary}}|{{context}}"}},
                              session_ctx_.get()));
  AlgContext missing;
  missing.Publish("primary", TextBatch{{1, 0, "Q"}});
  EXPECT_EQ(node->Process(&missing), -6202);
  EXPECT_FALSE(missing.Has("text"));

  AlgContext empty;
  empty.Publish("primary", TextBatch{{1, 0, "Q"}});
  empty.Publish("context_text", TextBatch{});
  ASSERT_EQ(node->Process(&empty), 0);
  ASSERT_NE(empty.Read<TextBatch>("text"), nullptr);
  EXPECT_EQ(empty.Read<TextBatch>("text")->front().data, "Q|");

  AlgContext other_request;
  other_request.Publish("primary", TextBatch{{1, 0, "Q"}});
  other_request.Publish(
      "context", RankedTextBatch{{2, 0, RankedCandidate("other", 1.0f)}});
  ASSERT_EQ(node->Process(&other_request), 0);
  EXPECT_EQ(other_request.Read<TextBatch>("text")->front().data, "Q|");
}

TEST_F(TextTemplateNodeTest, MissingPrimarySampleDoesNotPublishPartialOutput) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_TRUE(InitNodeForTest(*node, {{"template", "{{primary}}"}},
                              session_ctx_.get()));
  AlgContext ctx;
  ctx.Publish("primary", TextBatch{{1, 0, ""}});
  ctx.Publish("attributes", TextAttributesBatch{{2, 0, {}}});
  EXPECT_EQ(node->Process(&ctx), -6202);
  EXPECT_FALSE(ctx.Has("text"));

  AlgContext valid_empty;
  valid_empty.Publish("primary", TextBatch{{1, 0, ""}});
  ASSERT_EQ(node->Process(&valid_empty), 0);
  EXPECT_EQ(valid_empty.Read<TextBatch>("text")->front().data, "");
}

TEST_F(TextTemplateNodeTest,
       ControlRejectsUnconnectedBuiltinAndRetainsConfiguration) {
  Pipeline pipeline;
  ASSERT_TRUE(BuildTestPipeline(
      pipeline, TemplatePipeline({{"template", "{{primary}}"}})));
  EXPECT_NE(pipeline.Control(kControlCmdUpdatePrompt,
                             R"({"template":"{{context}}"})"),
            0);
  AlgContext original;
  original.Publish("input_sentences", TextBatch{{1, 0, "Q"}});
  ASSERT_EQ(pipeline.Execute(&original), 0);
  EXPECT_EQ(original.Read<TextBatch>("rendered_text")->front().data, "Q");
  EXPECT_EQ(
      pipeline.Control(
          kControlCmdUpdatePrompt,
          R"({"template":"{{context}}","missing_variable_policy":"empty"})"),
      0);
  EXPECT_NE(pipeline.Control(kControlCmdUpdatePrompt,
                             R"({"missing_variable_policy":"fail"})"),
            0);
  AlgContext after_failure;
  after_failure.Publish("input_sentences", TextBatch{{2, 0, "Q"}});
  ASSERT_EQ(pipeline.Execute(&after_failure), 0);
  EXPECT_EQ(after_failure.Read<TextBatch>("rendered_text")->front().data, "");
}

TEST_F(TextTemplateNodeTest, ConnectedAttributesRemainAvailableAcrossControl) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  std::string diagnostic;
  auto plan = PrepareNodePlanForTest(
      "TextTemplateNode",
      {{"template", "{{name}}"}, {"allow_dynamic_attributes", false}},
      {"primary", "context", "context_text", "matches", "document",
       "document_text"},
      "attrs_", "", &diagnostic);
  ASSERT_NE(plan, nullptr) << diagnostic;
  ASSERT_TRUE(node->Init({plan.get(), session_ctx_.get(), &diagnostic}))
      << diagnostic;
  EXPECT_EQ(node->Control(kControlCmdUpdatePrompt,
                          R"({"allow_dynamic_attributes":false})")
                .status,
            NodeControlStatus::kHandled);
  AlgContext ctx;
  ctx.Publish("attrs_attributes",
              TextAttributesBatch{{1, 0, {{"name", "Alice"}}}});
  ASSERT_EQ(node->Process(&ctx), 0);
  EXPECT_EQ(ctx.Read<TextBatch>("text")->front().data, "Alice");
}

TEST_F(TextTemplateNodeTest, DirectConcurrentProcessAndControl) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"template", "OLD: {{primary}}"}},
                              session_ctx_.get()));
  const nlohmann::json update = {{"template", "NEW: {{primary}}"}};
  const TextBatch inputs{
      {101, 2, "first"}, {101, 7, "second"}, {202, 3, "third"}};
  AlgContext in_flight;
  in_flight.Publish("primary", inputs);
  test_support::NodeProcessPause pause;
  int process_result = -1;
  std::exception_ptr reader_error;
  // On this valid Process path, the first heap allocation follows
  // snapshot.Read: the template grouping container. Pause with that old
  // snapshot retained.
  std::thread reader([&] {
    try {
      test_support::ScopedNextAllocationCallback callback(
          &test_support::NodeProcessPause::OnAllocation, &pause);
      process_result = node->Process(&in_flight);
    } catch (...) {
      reader_error = std::current_exception();
    }
  });
  const bool paused = pause.WaitUntilPaused();
  NodeControlStatus control_status = NodeControlStatus::kFailed;
  std::exception_ptr control_error;
  if (paused) {
    try {
      control_status =
          node->Control(kControlCmdUpdatePrompt, update.dump()).status;
    } catch (...) {
      control_error = std::current_exception();
    }
  }
  pause.Resume();
  reader.join();
  ASSERT_TRUE(paused) << "Process never reached the snapshot pause";
  ASSERT_EQ(reader_error, nullptr);
  ASSERT_EQ(control_error, nullptr);
  ASSERT_EQ(control_status, NodeControlStatus::kHandled);
  ASSERT_EQ(process_result, 0);

  auto expect_batch = [&](const AlgContext& ctx, const std::string& version) {
    const auto* output = ctx.Read<TextBatch>("text");
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->size(), inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
      EXPECT_EQ((*output)[i].req_id, inputs[i].req_id);
      EXPECT_EQ((*output)[i].sub_id, inputs[i].sub_id);
      EXPECT_EQ((*output)[i].data, version + ": " + inputs[i].data);
    }
  };
  expect_batch(in_flight, "OLD");
  AlgContext subsequent;
  subsequent.Publish("primary", inputs);
  ASSERT_EQ(node->Process(&subsequent), 0);
  expect_batch(subsequent, "NEW");
}

TEST_F(TextTemplateNodeTest,
       CatalogDefinitionStatesOnlyDoubleBracesSubstitute) {
  auto def_opt = PipelineCatalog::FindNode("TextTemplateNode");
  ASSERT_TRUE(def_opt.has_value());
  const auto& def = *def_opt;
  EXPECT_NE(def.description.find("{{name}} substitutes variables"),
            std::string::npos);
  EXPECT_NE(
      def.description.find("single {name} and JSON braces remain literal"),
      std::string::npos);
  EXPECT_EQ(def.description.find("and {name} substitute"), std::string::npos);
}

}  // namespace llm_edgeflow
