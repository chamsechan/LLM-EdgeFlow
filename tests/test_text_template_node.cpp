#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapter/shared_algorithm_runtime.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/session_context.h"

namespace alg_framework {

class TextTemplateNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SharedAlgorithmRuntime::GlobalInit(), 0);
    session_ctx_ = std::make_unique<SessionContext>();
  }
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
  auto node = NodeFactory::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {
      {"template",
       "Query: {{primary}}\nContext: {{context}}\nCategory: {{matches}}"},
      {"separator", " | "}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch primary;
  primary.emplace_back(1, 0, "How do I upgrade?");

  RankedTextBatch context;
  context.emplace_back(1, 0, RankedCandidate("Step 1: Go to settings", 0.9f));
  context.emplace_back(1, 1, RankedCandidate("Step 2: Click upgrade", 0.8f));

  RuleMatchBatch matches;
  matches.emplace_back(1, 0, RuleMatchItem(1, "ACCOUNT_UPGRADE", "upgrade"));

  ctx.Set("primary", primary);
  ctx.Set("context", context);
  ctx.Set("matches", matches);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Get<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 1u);
  EXPECT_NE((*out)[0].data.find("Go to settings | Step 2: Click upgrade"),
            std::string::npos);
  EXPECT_NE((*out)[0].data.find("ACCOUNT_UPGRADE"), std::string::npos);
}

// 2. Missing Required Variable Fails Closed
TEST_F(TextTemplateNodeTest, MissingRequiredVariableFailsClosed) {
  auto node = NodeFactory::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"template", "Hello {user_name}, welcome!"},
                        {"allow_dynamic_attributes", true},
                        {"missing_variable_policy", "fail"}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  // Attributes missing required variable 'user_name'
  AlgContext ctx;
  TextAttributesBatch attrs;
  attrs.emplace_back(
      1, 0,
      std::unordered_map<std::string, std::string>{{"other_key", "value"}});
  ctx.Set("attributes", attrs);

  EXPECT_NE(node->Process(&ctx), 0);
}

// 3. Dynamic Attribute Successfully Rendered
TEST_F(TextTemplateNodeTest, DynamicAttributeRendered) {
  auto node = NodeFactory::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"template", "Hello {user_name}, welcome!"},
                        {"allow_dynamic_attributes", true}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  TextAttributesBatch attrs;
  attrs.emplace_back(
      1, 0,
      std::unordered_map<std::string, std::string>{{"user_name", "Alice"}});
  ctx.Set("attributes", attrs);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Get<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  EXPECT_EQ((*out)[0].data, "Hello Alice, welcome!");
}

// 4. Control Command Hot-Swap & Bogus Rejection
TEST_F(TextTemplateNodeTest, ControlCommandHotSwapAndBogusRejection) {
  auto node = NodeFactory::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->Init({{"template", "{{primary}}"}}, session_ctx_.get()));

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

  NodeControlResult invalid_policy_res = node->Control(
      kControlCmdUpdatePrompt,
      nlohmann::json{{"missing_variable_policy", "invent"}}.dump());
  EXPECT_EQ(invalid_policy_res.status, NodeControlStatus::kFailed);

  // Bogus update with no valid fields -> Rejected
  nlohmann::json bogus_update = {{"bogus_field", 123}};
  NodeControlResult bogus_res =
      node->Control(kControlCmdUpdatePrompt, bogus_update.dump());
  EXPECT_EQ(bogus_res.status, NodeControlStatus::kFailed);
}

TEST_F(TextTemplateNodeTest, PipelineEnforcesPublishedControlSchema) {
  Pipeline pipeline;
  PipelineDiagnostic diagnostic;
  ASSERT_TRUE(pipeline.BuildFromConfigFile(
      ResolveConfigPath("tests/fixtures/stage7/smoke/pipeline_doc_qa.json"),
      &diagnostic))
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

}  // namespace alg_framework
