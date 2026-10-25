#include <gtest/gtest.h>

#include "contracts/error_codes.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "tests/support/node_probe.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;
using test_support::NodeProbe;

TEST(StructuredJsonParseNodeTest,
     ParsesWholeValuesFencesAndCompleteOuterContainers) {
  SessionContext session;
  NodeProbe node;
  ASSERT_TRUE(node.Init("structured_json_parse", nlohmann::json::object(),
                        session, {{"text", "input", ""}},
                        {{"document", "result", ""}}));
  AlgContext context;
  ASSERT_TRUE(context.Publish(
      "input",
      TextBatch{
          {7, 9, R"({"answer":42})"},
          {2, 5, "before ```json\n{\"answer\":true}\n``` after"},
          {7, 3, R"(preface [{"brackets":"[}]","quote":"\""},2] trailing)"},
          {4, 0, "123"}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  const auto* docs = context.Read<StructuredDocumentBatch>("result");
  ASSERT_NE(docs, nullptr);
  ASSERT_EQ(docs->size(), 4u);
  EXPECT_EQ((*docs)[0].data.structured_data, Json({{"answer", 42}}));
  EXPECT_EQ((*docs)[0].data.json_payload, R"({"answer":42})");
  EXPECT_EQ((*docs)[1].data.parse_status,
            JsonParseStatus::kExtractedFromMarkdown);
  EXPECT_EQ((*docs)[1].data.structured_data, Json({{"answer", true}}));
  EXPECT_EQ((*docs)[2].data.structured_data,
            Json::array({Json({{"brackets", "[}]"}, {"quote", "\""}}), 2}));
  EXPECT_EQ((*docs)[2].req_id, 7u);
  EXPECT_EQ((*docs)[2].sub_id, 3u);
  EXPECT_EQ((*docs)[3].data.structured_data, 123);
  for (const auto& doc : *docs) {
    EXPECT_TRUE(doc.data.is_valid);
    EXPECT_TRUE(doc.data.diagnostic.empty());
  }
}

TEST(StructuredJsonParseNodeTest,
     NeverStealsValidChildrenFromMalformedParentContainers) {
  const std::vector<std::string> invalid = {
      R"(prefix [{"valid":1},)",
      R"(prefix {"child":{"valid":1}, broken})",
      R"(prefix [{"valid":1} trailing])",
      "```json\n[{\"valid\":1},\n```",
      "```json\n{\"valid\":1}",
      ""};
  SessionContext session;
  NodeProbe node;
  ASSERT_TRUE(node.Init("structured_json_parse",
                        {{"failure_policy", "emit_diagnostic"}}, session,
                        {{"text", "input", ""}}, {{"document", "result", ""}}));
  for (const auto& text : invalid) {
    SCOPED_TRACE(text);
    AlgContext context;
    ASSERT_TRUE(context.Publish("input", TextBatch{{1, 8, text}}));
    ASSERT_EQ(node.node->Process(&context), 0);
    const auto* docs = context.Read<StructuredDocumentBatch>("result");
    ASSERT_NE(docs, nullptr);
    ASSERT_EQ(docs->size(), 1u);
    EXPECT_FALSE(docs->front().data.is_valid);
    EXPECT_EQ(docs->front().data.parse_status, JsonParseStatus::kFailed);
    EXPECT_EQ(docs->front().data.structured_data, Json::object());
    EXPECT_FALSE(docs->front().data.diagnostic.empty());
  }
}

TEST(StructuredJsonParseNodeTest,
     FieldChecksSelectConfiguredFallbackOrAbortWholeBatch) {
  SessionContext session;
  const Json fields = {
      {"required_fields", {"name"}},
      {"field_types", {{"name", "string"}, {"score", "number"}}},
      {"fallback", {{"name", "unknown"}, {"score", 0}}}};
  NodeProbe fallback;
  ASSERT_TRUE(fallback.Init("structured_json_parse", fields, session,
                            {{"text", "input", ""}},
                            {{"document", "result", ""}}));
  AlgContext context;
  ASSERT_TRUE(context.Publish(
      "input",
      TextBatch{{3, 2, R"({"name":1,"score":9})"}, {3, 4, R"({"score":9})"}}));
  ASSERT_EQ(fallback.node->Process(&context), 0);
  const auto* docs = context.Read<StructuredDocumentBatch>("result");
  ASSERT_NE(docs, nullptr);
  ASSERT_EQ(docs->size(), 2u);
  for (const auto& doc : *docs) {
    EXPECT_TRUE(doc.data.is_valid);
    EXPECT_EQ(doc.data.parse_status, JsonParseStatus::kFallbackApplied);
    EXPECT_EQ(doc.data.structured_data,
              Json({{"name", "unknown"}, {"score", 0}}));
    EXPECT_EQ(doc.data.json_payload, R"({"name":"unknown","score":0})");
    EXPECT_FALSE(doc.data.diagnostic.empty());
  }
  auto fail_params = fields;
  fail_params["failure_policy"] = "fail";
  NodeProbe fail;
  ASSERT_TRUE(fail.Init("structured_json_parse", fail_params, session,
                        {{"text", "input", ""}}, {{"document", "result", ""}}));
  AlgContext failed;
  ASSERT_TRUE(failed.Publish("input",
                             TextBatch{{1, 0, R"({"name":"ok","score":1})"},
                                       {1, 1, R"({"name":false,"score":1})"}}));
  EXPECT_EQ(fail.node->Process(&failed), error::kJsonParse);
  EXPECT_FALSE(failed.Has("result"));
  const std::string message = failed.GetErrorMessage();
  EXPECT_NE(message.find("JSON parse failed for sample: Field 'name' type "
                         "mismatch, expected string"),
            std::string::npos)
      << message;
  const std::string source = "req_id=1, sub_id=1";
  const auto first = message.find(source);
  ASSERT_NE(first, std::string::npos) << message;
  EXPECT_EQ(message.find(source, first + 1), std::string::npos) << message;
}

TEST(StructuredJsonParseNodeTest,
     DisabledExtractionAndInvalidFallbackRespectConfiguration) {
  SessionContext session;
  NodeProbe node;
  ASSERT_TRUE(node.Init(
      "structured_json_parse",
      {{"extract_json_block", false}, {"failure_policy", "emit_diagnostic"}},
      session, {{"text", "input", ""}}, {{"document", "result", ""}}));
  AlgContext context;
  ASSERT_TRUE(context.Publish("input", TextBatch{{1, 0, "prefix {\"ok\":1}"}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  EXPECT_FALSE(
      context.Read<StructuredDocumentBatch>("result")->front().data.is_valid);
  NodeProbe invalid;
  EXPECT_FALSE(invalid.Init(
      "structured_json_parse", {{"required_fields", {"name"}}}, session,
      {{"text", "input", ""}}, {{"document", "result", ""}}));
  EXPECT_NE(invalid.diagnostic.find("name"), std::string::npos);
}
}  // namespace
}  // namespace llm_edgeflow
