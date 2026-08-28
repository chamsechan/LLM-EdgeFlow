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

namespace alg_framework {

class StructuredJsonParseNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SharedAlgorithmRuntime::GlobalInit(), 0);
    session_ctx_ = std::make_unique<SessionContext>();
  }
  std::unique_ptr<SessionContext> session_ctx_;
};

// 1. Process Markdown JSON Block Extraction
TEST_F(StructuredJsonParseNodeTest, ProcessMarkdownJsonBlockExtraction) {
  auto node = NodeFactory::Instance().Create("StructuredJsonParseNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"extract_json_block", true},
                        {"failure_policy", "fail"}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch inputs;
  inputs.emplace_back(1, 0,
                      "Here is the result:\n```json\n{\"verdict\": \"合规\", "
                      "\"risk_level\": \"SAFE\", \"risk_score\": 0.10}\n```");
  ctx.Set("text", inputs);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* doc = ctx.Get<StructuredDocumentBatch>("document");
  ASSERT_NE(doc, nullptr);
  ASSERT_EQ(doc->size(), 1u);
  EXPECT_TRUE((*doc)[0].data.is_valid);
  EXPECT_EQ((*doc)[0].data.structured_data["risk_level"], "SAFE");
}

// 2. Required Fields and Field Types Validation
TEST_F(StructuredJsonParseNodeTest, RequiredFieldsAndFieldTypesValidation) {
  auto node = NodeFactory::Instance().Create("StructuredJsonParseNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {
      {"required_fields", {"risk_level", "risk_score"}},
      {"field_types", {{"risk_level", "string"}, {"risk_score", "number"}}},
      {"failure_policy", "fail"}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  // Valid sample
  {
    AlgContext ctx;
    TextBatch inputs;
    inputs.emplace_back(1, 0, "{\"risk_level\":\"HIGH\",\"risk_score\":0.95}");
    ctx.Set("text", inputs);
    EXPECT_EQ(node->Process(&ctx), 0);
  }

  // Type mismatch: risk_score is string instead of number
  {
    AlgContext ctx;
    TextBatch inputs;
    inputs.emplace_back(
        1, 0, "{\"risk_level\":\"HIGH\",\"risk_score\":\"invalid_number\"}");
    ctx.Set("text", inputs);
    EXPECT_NE(node->Process(&ctx), 0);
  }

  // Missing required field
  {
    AlgContext ctx;
    TextBatch inputs;
    inputs.emplace_back(1, 0, "{\"risk_level\":\"HIGH\"}");
    ctx.Set("text", inputs);
    EXPECT_NE(node->Process(&ctx), 0);
  }
}

// 3. Fallback Policy on Malformed Input
TEST_F(StructuredJsonParseNodeTest, FallbackPolicyOnMalformedInput) {
  auto node = NodeFactory::Instance().Create("StructuredJsonParseNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"fallback_json", "{\"status\":\"FALLBACK\"}"},
                        {"failure_policy", "configured_fallback"}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch inputs;
  inputs.emplace_back(1, 0, "not a valid json at all");
  ctx.Set("text", inputs);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* doc = ctx.Get<StructuredDocumentBatch>("document");
  ASSERT_NE(doc, nullptr);
  ASSERT_EQ(doc->size(), 1u);
  EXPECT_EQ((*doc)[0].data.structured_data["status"], "FALLBACK");
}

TEST_F(StructuredJsonParseNodeTest, RejectsInvalidFieldTypeContracts) {
  auto unknown_type = NodeFactory::Instance().Create("StructuredJsonParseNode");
  ASSERT_NE(unknown_type, nullptr);
  EXPECT_FALSE(unknown_type->Init({{"field_types", {{"risk", "decimal"}}}},
                                  session_ctx_.get()));

  auto non_string_type =
      NodeFactory::Instance().Create("StructuredJsonParseNode");
  ASSERT_NE(non_string_type, nullptr);
  EXPECT_FALSE(non_string_type->Init({{"field_types", {{"risk", 7}}}},
                                     session_ctx_.get()));

  auto invalid_fallback =
      NodeFactory::Instance().Create("StructuredJsonParseNode");
  ASSERT_NE(invalid_fallback, nullptr);
  EXPECT_FALSE(
      invalid_fallback->Init({{"required_fields", {"risk"}},
                              {"field_types", {{"risk", "number"}}},
                              {"fallback_json", R"({"risk":"not-a-number"})"},
                              {"failure_policy", "configured_fallback"}},
                             session_ctx_.get()));
}

}  // namespace alg_framework
