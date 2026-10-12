#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "adapter/io_values.h"
#include "core/common_contracts.h"
#include "tests/support/converter_probe.h"

namespace llm_edgeflow {
namespace {

using test_support::DecodeConverter;
using test_support::EncodeJsonConverter;

TEST(TextConverterTest, EntityAndKeywordKeepWholeTextAndOwnEachRequestRow) {
  struct Case {
    const char* type;
    const char* name;
  };
  const std::vector<Case> cases = {{"entity_in", "entity_extract"},
                                   {"keyword_in", "keyword_match"}};
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    auto first = std::make_shared<TextInputValue>();
    first->sentence_text = R"({"query":"Hi","src_lan":"en"})";
    auto second = std::make_shared<TextInputValue>();
    second->sentence_text = "第二行\nraw text";
    AlgContext context;
    AdapterStatus status;
    ASSERT_EQ(DecodeConverter<TextInputValue>(
                  test.type, test.name, {first, second}, &context, &status),
              0)
        << status.ToString();
    first->sentence_text = "mutated";
    second.reset();
    const auto* texts = context.Read<TextBatch>("decoded.sentence_text");
    ASSERT_NE(texts, nullptr);
    ASSERT_EQ(texts->size(), 2U);
    EXPECT_EQ((*texts)[0].data, R"({"query":"Hi","src_lan":"en"})");
    EXPECT_EQ((*texts)[0].req_id, 0U);
    EXPECT_EQ((*texts)[0].sub_id, 0U);
    EXPECT_EQ((*texts)[1].data, "第二行\nraw text");
    EXPECT_EQ((*texts)[1].req_id, 1U);
    EXPECT_EQ((*texts)[1].sub_id, 0U);
  }
}

TEST(TextConverterTest, TranslationSelectsStringQueryFromCompleteJsonRequest) {
  auto first = std::make_shared<TextInputValue>();
  first->sentence_text =
      R"({"query":"Hi\n你好","src_lan":"en","dst_lan":"zh","extra":7})";
  auto second = std::make_shared<TextInputValue>();
  second->sentence_text = R"({"query":"","src_lan":false})";
  AlgContext context;
  AdapterStatus status;
  ASSERT_EQ(DecodeConverter<TextInputValue>("entity_in", "translate",
                                            {first, second}, &context, &status),
            0)
      << status.ToString();
  first->sentence_text.clear();
  const auto* queries = context.Read<TextBatch>("decoded.query");
  ASSERT_NE(queries, nullptr);
  ASSERT_EQ(queries->size(), 2U);
  EXPECT_EQ((*queries)[0].data, "Hi\n你好");
  EXPECT_EQ((*queries)[0].req_id, 0U);
  EXPECT_EQ((*queries)[1].data, "");
  EXPECT_EQ((*queries)[1].req_id, 1U);
}

TEST(TextConverterTest, TranslationRejectsMalformedOrNonStringQuery) {
  const std::vector<std::string> requests = {
      "{", "[]", "null", "{}", R"({"query":5})", R"({"query":null})"};
  for (const auto& request : requests) {
    SCOPED_TRACE(request);
    auto row = std::make_shared<TextInputValue>();
    row->sentence_text = request;
    AlgContext context;
    AdapterStatus status;
    EXPECT_EQ(DecodeConverter<TextInputValue>("entity_in", "translate", {row},
                                              &context, &status),
              COMPANY_ALG_ERR_INVALID_INPUT);
    EXPECT_EQ(status.FieldPath(), "json");
    EXPECT_NE(status.Message().find("string field query"), std::string::npos);
    EXPECT_FALSE(context.Has("decoded.query"));
  }
}

TEST(TextConverterTest, TranslationResponseContainsOnlyTranslatedText) {
  AlgContext context;
  ASSERT_TRUE(context.Publish("result.translation",
                              TextBatch{{0, 0, "Bonjour \"世界\"\n"}}));
  std::vector<JsonOutputValue> rows(1);
  AdapterStatus status;
  ASSERT_EQ(EncodeJsonConverter("translate", &context, &rows, &status), 0)
      << status.ToString();
  EXPECT_EQ(rows[0].status_code, 0);
  EXPECT_EQ(nlohmann::json::parse(rows[0].result_json),
            nlohmann::json::parse(R"({"translated":"Bonjour \"世界\"\n"})"));
}

TEST(TextConverterTest, EntityResponsePreservesDocumentBytesAndAcceptsRepairs) {
  for (const auto parse_status :
       {JsonParseStatus::kOk, JsonParseStatus::kExtractedFromMarkdown,
        JsonParseStatus::kAutoClosed}) {
    AlgContext context;
    const std::string payload = "{ \"entities\" : [\"北京\"], \"n\" : 1 }";
    ASSERT_TRUE(context.Publish(
        "result.entities",
        StructuredDocumentBatch{
            {0, 0, JsonDocumentItem(payload, true, parse_status)}}));
    std::vector<JsonOutputValue> rows(1);
    AdapterStatus status;
    ASSERT_EQ(EncodeJsonConverter("entity_extract", &context, &rows, &status),
              0);
    EXPECT_EQ(rows[0].status_code, 0);
    EXPECT_EQ(rows[0].result_json, payload);
  }
}

TEST(TextConverterTest,
     EntityResponseRejectsInvalidFailedAndFallbackDocuments) {
  struct Case {
    bool valid;
    JsonParseStatus parse_status;
  };
  const std::vector<Case> cases = {{false, JsonParseStatus::kOk},
                                   {true, JsonParseStatus::kFailed},
                                   {true, JsonParseStatus::kFallbackApplied}};
  for (const auto& test : cases) {
    AlgContext context;
    ASSERT_TRUE(context.Publish(
        "result.entities",
        StructuredDocumentBatch{
            {0, 0, JsonDocumentItem("{}", test.valid, test.parse_status)}}));
    std::vector<JsonOutputValue> rows(1);
    AdapterStatus status;
    EXPECT_EQ(EncodeJsonConverter("entity_extract", &context, &rows, &status),
              COMPANY_ALG_ERR_INVALID_INPUT);
    EXPECT_EQ(status.Message(), "Structured result failed or used fallback");
    EXPECT_TRUE(rows[0].result_json.empty());
  }
}

TEST(TextConverterTest, KeywordResponseDistinguishesRuleAndKeywordHitFields) {
  RuleMatchItem match(1, "BOOK", "book", 0.75f, "r1");
  match.status_code = 7;
  match.slots = {{"quantity", 2}, {"confirmed", true}};
  match.matches = {{RuleMatchSource::kKeyword, "BOOK", "ignored", "book", 0.5f},
                   {RuleMatchSource::kRule, "BOOK", "r1", "book (.+)", 0.75f}};
  AlgContext context;
  ASSERT_TRUE(context.Publish("result.matches", RuleMatchBatch{{0, 0, match}}));
  std::vector<JsonOutputValue> rows(1);
  AdapterStatus status;
  ASSERT_EQ(EncodeJsonConverter("keyword_match", &context, &rows, &status), 0);
  EXPECT_EQ(rows[0].status_code, 7);
  EXPECT_EQ(nlohmann::json::parse(rows[0].result_json),
            nlohmann::json::parse(R"json({
    "is_hit":1,"intent":"BOOK","matched_word":"book","confidence":0.75,
    "slots":{"quantity":2,"confirmed":true},
    "matches":[{"category":"BOOK","matched_word":"book"},
      {"category":"BOOK","rule_id":"r1","pattern":"book (.+)","score":0.75}]
  })json"));
}

TEST(TextConverterTest, KeywordNoHitOmitsIntentAndSlots) {
  RuleMatchItem match;
  match.status_code = 9;
  match.category = "ignored";
  match.slots = {{"ignored", true}};
  AlgContext context;
  ASSERT_TRUE(context.Publish("result.matches", RuleMatchBatch{{0, 0, match}}));
  std::vector<JsonOutputValue> rows(1);
  AdapterStatus status;
  ASSERT_EQ(EncodeJsonConverter("keyword_match", &context, &rows, &status), 0);
  EXPECT_EQ(rows[0].status_code, 9);
  EXPECT_EQ(nlohmann::json::parse(rows[0].result_json),
            nlohmann::json::parse(R"({"is_hit":0,"matches":[]})"));
}

}  // namespace
}  // namespace llm_edgeflow
