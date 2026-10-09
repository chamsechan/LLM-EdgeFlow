#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline_validator.h"
#include "core/session_context.h"
#include "nodes/node_error_codes.h"
#include "tests/support/node_test_utils.h"
#include "tests/support/pipeline_test_utils.h"

namespace llm_edgeflow {

class StructuredJsonParseNodeTest : public ::testing::Test {
 protected:
  void SetUp() override { session_ctx_ = std::make_unique<SessionContext>(); }
  std::unique_ptr<SessionContext> session_ctx_;
};

TEST_F(StructuredJsonParseNodeTest, ProcessMarkdownJsonBlockExtraction) {
  auto node = NodeRegistry::Instance().Create("StructuredJsonParseNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"extract_json_block", true},
                        {"failure_policy", "fail"}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch inputs;
  inputs.emplace_back(1, 0,
                      "Here is the result:\n```json\n{\"verdict\": \"合规\", "
                      "\"risk_level\": \"SAFE\", \"risk_score\": 0.10}\n```");
  ctx.Publish("text", inputs);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* doc = ctx.Read<StructuredDocumentBatch>("document");
  ASSERT_NE(doc, nullptr);
  ASSERT_EQ(doc->size(), 1u);
  EXPECT_TRUE((*doc)[0].data.is_valid);
  EXPECT_EQ((*doc)[0].data.structured_data["risk_level"], "SAFE");
}

TEST_F(StructuredJsonParseNodeTest, PreservesCompleteOuterContainers) {
  auto node = NodeRegistry::Instance().Create("StructuredJsonParseNode");
  ASSERT_TRUE(
      InitNodeForTest(*node, {{"failure_policy", "fail"}}, session_ctx_.get()));
  for (const std::string text :
       {R"([{"x":1},{"x":2}])",
        R"({"text":"braces } [ and \"quotes\" \\","nested":[{"x":1}]})",
        R"(["a","b"])"}) {
    const auto expected = nlohmann::json::parse(text);
    for (const std::string& input :
         {text, text + " Done.", "Result: " + text + " Done.",
          "```json\n" + text + "\n```",
          "[Result]\n```json\n" + text + "\n```"}) {
      SCOPED_TRACE(input);
      AlgContext ctx;
      ctx.Publish("text", TextBatch{{17, 3, input}});
      ASSERT_EQ(node->Process(&ctx), 0);
      const auto* docs = ctx.Read<StructuredDocumentBatch>("document");
      ASSERT_NE(docs, nullptr);
      ASSERT_EQ(docs->size(), 1u);
      EXPECT_EQ(docs->at(0).req_id, 17u);
      EXPECT_EQ(docs->at(0).sub_id, 3u);
      EXPECT_TRUE(docs->at(0).data.is_valid);
      EXPECT_EQ(docs->at(0).data.structured_data, expected);
      EXPECT_EQ(nlohmann::json::parse(docs->at(0).data.json_payload), expected);
      EXPECT_TRUE(docs->at(0).data.diagnostic.empty());
    }
  }
}

TEST_F(StructuredJsonParseNodeTest, RejectsTruncationAndQuotedProse) {
  auto node = NodeRegistry::Instance().Create("StructuredJsonParseNode");
  ASSERT_TRUE(
      InitNodeForTest(*node, {{"failure_policy", "fail"}}, session_ctx_.get()));
  for (const std::string input :
       {R"([{"x":1},{"x":2)", R"(Found entities: ["Alice", "Bob")",
        R"(He mentioned "Alice" and "Bob".)", R"({"x":[1,2])", R"({"x":1,})",
        R"({"x":[1,2})", "```json\n[{\"x\":1},{\"x\":2\n```",
        "```json\n{\"x\":1}"}) {
    SCOPED_TRACE(input);
    AlgContext ctx;
    ctx.Publish("text", TextBatch{{1, 0, input}});
    EXPECT_EQ(node->Process(&ctx), -6102);
    EXPECT_EQ(ctx.Read<StructuredDocumentBatch>("document"), nullptr);
  }
}

TEST_F(StructuredJsonParseNodeTest,
       InvalidDocumentHonorsFallbackAndDiagnostic) {
  for (const std::string policy : {"configured_fallback", "emit_diagnostic"}) {
    SCOPED_TRACE(policy);
    auto node = NodeRegistry::Instance().Create("StructuredJsonParseNode");
    ASSERT_TRUE(InitNodeForTest(
        *node, {{"failure_policy", policy}, {"fallback", {{"fallback", true}}}},
        session_ctx_.get()));
    for (const std::string input :
         {R"([{"x":1},{"x":2)", R"(Result: {"x":[1,2})"}) {
      SCOPED_TRACE(input);
      AlgContext ctx;
      ctx.Publish("text", TextBatch{{7, 2, input}});
      ASSERT_EQ(node->Process(&ctx), 0);
      const auto* docs = ctx.Read<StructuredDocumentBatch>("document");
      ASSERT_NE(docs, nullptr);
      ASSERT_EQ(docs->size(), 1u);
      const bool fallback = policy == "configured_fallback";
      EXPECT_EQ(docs->at(0).data.is_valid, fallback);
      EXPECT_EQ(docs->at(0).data.parse_status,
                fallback ? JsonParseStatus::kFallbackApplied
                         : JsonParseStatus::kFailed);
      EXPECT_EQ(docs->at(0).data.structured_data,
                nlohmann::json({{"fallback", true}}));
      EXPECT_FALSE(docs->at(0).data.diagnostic.empty());
      EXPECT_EQ(docs->at(0).req_id, 7u);
      EXPECT_EQ(docs->at(0).sub_id, 2u);
    }
  }
}

TEST_F(StructuredJsonParseNodeTest,
       JsonFallbackContainersPreserveCompactTextAndProvenance) {
  const std::vector<nlohmann::json> fallbacks = {
      {{"status", "fallback"}, {"values", {1, 2}}},
      nlohmann::json::array({"fallback", {{"value", 7}}, false})};
  for (const auto& fallback : fallbacks) {
    for (const std::string policy :
         {"configured_fallback", "emit_diagnostic"}) {
      SCOPED_TRACE(fallback.dump() + policy);
      auto node = NodeRegistry::Instance().Create("StructuredJsonParseNode");
      ASSERT_TRUE(InitNodeForTest(
          *node, {{"fallback", fallback}, {"failure_policy", policy}},
          session_ctx_.get()));
      AlgContext context;
      context.Publish("text", TextBatch{{21, 3, "not JSON"},
                                        {42, 9, R"({ "valid": true })"}});
      ASSERT_EQ(node->Process(&context), 0);
      const auto* documents = context.Read<StructuredDocumentBatch>("document");
      ASSERT_NE(documents, nullptr);
      ASSERT_EQ(documents->size(), 2u);
      const auto& recovered = documents->at(0);
      EXPECT_EQ(recovered.req_id, 21u);
      EXPECT_EQ(recovered.sub_id, 3u);
      EXPECT_EQ(recovered.data.structured_data, fallback);
      EXPECT_EQ(recovered.data.json_payload, fallback.dump());
      EXPECT_EQ(recovered.data.is_valid, policy == "configured_fallback");
      EXPECT_EQ(recovered.data.parse_status,
                policy == "configured_fallback"
                    ? JsonParseStatus::kFallbackApplied
                    : JsonParseStatus::kFailed);
      EXPECT_FALSE(recovered.data.diagnostic.empty());
      EXPECT_EQ(documents->at(1).req_id, 42u);
      EXPECT_EQ(documents->at(1).sub_id, 9u);
      EXPECT_TRUE(documents->at(1).data.is_valid);
      EXPECT_EQ(documents->at(1).data.json_payload, R"({"valid":true})");
      EXPECT_EQ(documents->at(1).data.structured_data,
                nlohmann::json({{"valid", true}}));
    }
  }
}

TEST_F(StructuredJsonParseNodeTest, ExtractionModeControlsSurroundingText) {
  for (const bool extract : {true, false}) {
    SCOPED_TRACE(extract);
    auto node = NodeRegistry::Instance().Create("StructuredJsonParseNode");
    ASSERT_TRUE(InitNodeForTest(
        *node, {{"extract_json_block", extract}, {"failure_policy", "fail"}},
        session_ctx_.get()));
    AlgContext direct_ctx;
    direct_ctx.Publish("text", TextBatch{{1, 0, R"({"x":1})"}});
    EXPECT_EQ(node->Process(&direct_ctx), 0);
    for (const std::string input :
         {R"(Result: {"x":1})", R"({"x":1} Done.)", R"({"x":1},{"x":2)",
          R"(Result: {"x":1},{"x":2)", R"({"x":1}{"x":2})", R"({"x":1}])"}) {
      SCOPED_TRACE(input);
      AlgContext ctx;
      ctx.Publish("text", TextBatch{{1, 0, input}});
      EXPECT_EQ(node->Process(&ctx), extract ? 0 : -6102);
      const auto* docs = ctx.Read<StructuredDocumentBatch>("document");
      if (extract) {
        ASSERT_NE(docs, nullptr);
        ASSERT_EQ(docs->size(), 1u);
        EXPECT_EQ(docs->at(0).data.structured_data, nlohmann::json({{"x", 1}}));
      } else {
        EXPECT_EQ(docs, nullptr);
      }
    }
  }
}

TEST_F(StructuredJsonParseNodeTest, RequiredFieldsAndFieldTypesValidation) {
  auto node = NodeRegistry::Instance().Create("StructuredJsonParseNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {
      {"required_fields", {"risk_level", "risk_score"}},
      {"field_types", {{"risk_level", "string"}, {"risk_score", "number"}}},
      {"failure_policy", "fail"}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  // 合法样本
  {
    AlgContext ctx;
    TextBatch inputs;
    inputs.emplace_back(1, 0, "{\"risk_level\":\"HIGH\",\"risk_score\":0.95}");
    ctx.Publish("text", inputs);
    EXPECT_EQ(node->Process(&ctx), 0);
  }

  // 类型不匹配：risk_score 是字符串而非数字
  {
    AlgContext ctx;
    TextBatch inputs;
    inputs.emplace_back(
        1, 0, "{\"risk_level\":\"HIGH\",\"risk_score\":\"invalid_number\"}");
    ctx.Publish("text", inputs);
    EXPECT_EQ(node->Process(&ctx), -6102);
  }

  // 缺少必填字段
  {
    AlgContext ctx;
    TextBatch inputs;
    inputs.emplace_back(1, 0, "{\"risk_level\":\"HIGH\"}");
    ctx.Publish("text", inputs);
    EXPECT_EQ(node->Process(&ctx), -6102);
  }
}

TEST_F(StructuredJsonParseNodeTest, MissingInputFailsClosed) {
  auto node = NodeRegistry::Instance().Create("StructuredJsonParseNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node, {{"failure_policy", "fail"}}, session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_EQ(node->Process(&empty_ctx), node_error::author_node::kMissingInput);
}

TEST_F(StructuredJsonParseNodeTest, FallbackPolicyOnMalformedInput) {
  auto node = NodeRegistry::Instance().Create("StructuredJsonParseNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"fallback", {{"status", "FALLBACK"}}},
                        {"failure_policy", "configured_fallback"}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch inputs;
  inputs.emplace_back(1, 0, "not a valid json at all");
  ctx.Publish("text", inputs);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* doc = ctx.Read<StructuredDocumentBatch>("document");
  ASSERT_NE(doc, nullptr);
  ASSERT_EQ(doc->size(), 1u);
  EXPECT_EQ((*doc)[0].data.structured_data["status"], "FALLBACK");
}

TEST_F(StructuredJsonParseNodeTest, RejectsInvalidFieldTypeContracts) {
  for (const nlohmann::json type :
       {nlohmann::json("decimal"), nlohmann::json(7)}) {
    SCOPED_TRACE(type.dump());
    const nlohmann::json config = {{"field_types", {{"risk", type}}}};
    auto node = NodeRegistry::Instance().Create("StructuredJsonParseNode");
    ASSERT_NE(node, nullptr);
    std::string diagnostic;
    EXPECT_FALSE(
        InitNodeForTest(*node, config, session_ctx_.get(), &diagnostic));
    EXPECT_NE(diagnostic.find("/field_types/risk"), std::string::npos)
        << diagnostic;
    const nlohmann::json pipeline = {
        {"models", nlohmann::json::array()},
        {"pipeline",
         nlohmann::json::array({{{"id", "parse"},
                                 {"node_type", "StructuredJsonParseNode"},
                                 {"config", config}}})}};
    const auto report =
        PipelineValidator::Validate(pipeline, MakeTestBoundary());
    EXPECT_FALSE(report.ok);
    EXPECT_TRUE(std::any_of(
        report.diagnostics.begin(), report.diagnostics.end(),
        [&](const auto& error) {
          return error.path == "/pipeline/0/config/field_types/risk" &&
                 error.code == (type.is_string()
                                    ? DiagnosticCode::kConfigFieldEnum
                                    : DiagnosticCode::kConfigFieldType);
        }))
        << report.ToJson().dump(2);
  }

  for (const nlohmann::json fallback :
       {nlohmann::json({{"risk", "not-a-number"}}), nlohmann::json::object()}) {
    for (const std::string policy :
         {"configured_fallback", "emit_diagnostic"}) {
      SCOPED_TRACE(fallback.dump() + policy);
      auto node = NodeRegistry::Instance().Create("StructuredJsonParseNode");
      ASSERT_NE(node, nullptr);
      std::string diagnostic;
      EXPECT_FALSE(InitNodeForTest(*node,
                                   {{"required_fields", {"risk"}},
                                    {"field_types", {{"risk", "number"}}},
                                    {"fallback", fallback},
                                    {"failure_policy", policy}},
                                   session_ctx_.get(), &diagnostic));
      EXPECT_NE(diagnostic.find("risk"), std::string::npos) << diagnostic;
    }
  }
}

}  // namespace llm_edgeflow

namespace llm_edgeflow {
TEST_F(StructuredJsonParseNodeTest, EmptyInputHonorsDiagnosticPolicy) {
  auto node = NodeRegistry::Instance().Create("StructuredJsonParseNode");
  ASSERT_TRUE(InitNodeForTest(*node, {{"failure_policy", "emit_diagnostic"}},
                              session_ctx_.get()));
  AlgContext ctx;
  ctx.Publish("text", TextBatch{{0, 0, ""}});
  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* docs = ctx.Read<StructuredDocumentBatch>("document");
  ASSERT_NE(docs, nullptr);
  EXPECT_FALSE(docs->at(0).data.is_valid);
  EXPECT_EQ(docs->at(0).data.parse_status, JsonParseStatus::kFailed);
}
}  // namespace llm_edgeflow
