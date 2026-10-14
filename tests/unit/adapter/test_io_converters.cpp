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

void ExpectTextRows(const AlgContext& context, const std::string& port,
                    const std::vector<std::string>& expected) {
  const auto* batch = context.Read<TextBatch>("decoded." + port);
  ASSERT_NE(batch, nullptr);
  ASSERT_EQ(batch->size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ((*batch)[i].req_id, i);
    EXPECT_EQ((*batch)[i].sub_id, 0U);
    EXPECT_EQ((*batch)[i].data, expected[i]);
  }
}

TEST(IoConverterTest, DocumentAndQueryStaySeparateOwnedStreams) {
  auto first = std::make_shared<DocumentInputValue>();
  *first = {"完整文档\npage2", "问题一"};
  auto second = std::make_shared<DocumentInputValue>();
  *second = {"other document", "question two"};
  AlgContext context;
  AdapterStatus status;
  ASSERT_EQ(DecodeConverter<DocumentInputValue>(
                "doc_in", "doc_qa", {first, second}, &context, &status),
            0);
  first->doc_text = "mutated";
  first->query_text.clear();
  second.reset();
  ExpectTextRows(context, "doc_text", {"完整文档\npage2", "other document"});
  ExpectTextRows(context, "query_text", {"问题一", "question two"});
}

TEST(IoConverterTest, AuditKeepsChannelMetadataSeparateFromUserText) {
  auto first = std::make_shared<AuditInputValue>();
  *first = {"user says buy", "sales"};
  auto second = std::make_shared<AuditInputValue>();
  *second = {"user asks help", "support"};
  AlgContext context;
  AdapterStatus status;
  ASSERT_EQ(
      DecodeConverter<AuditInputValue>("audit_in", "dialogue_audit",
                                       {first, second}, &context, &status),
      0);
  first->user_text.clear();
  first->channel_name = "changed";
  second.reset();
  ExpectTextRows(context, "user_text", {"user says buy", "user asks help"});
  ExpectTextRows(context, "channel_name", {"sales", "support"});
}

TEST(IoConverterTest, InvoiceFrameAndQuestionKeepCompleteOwnedValues) {
  auto first = std::make_shared<ImageInputValue>();
  first->frame = {2, 1, 8, {10, 20, 30, 40, 50, 60, 99, 88}};
  auto second = std::make_shared<ImageInputValue>();
  second->frame = {1, 1, 3, {70, 80, 90}};
  AlgContext context;
  AdapterStatus status;
  ASSERT_EQ(DecodeConverter<ImageInputValue>(
                "frame", "ocr_invoice_qa", {first, second}, &context, &status),
            0);
  first->frame.data.assign(8, 0);
  second.reset();
  const auto* images = context.Read<ImageFrameBatch>("decoded.image");
  ASSERT_NE(images, nullptr);
  ASSERT_EQ(images->size(), 2U);
  EXPECT_EQ((*images)[0].req_id, 0U);
  EXPECT_EQ((*images)[0].sub_id, 0U);
  EXPECT_EQ((*images)[0].data.width, 2);
  EXPECT_EQ((*images)[0].data.height, 1);
  EXPECT_EQ((*images)[0].data.stride, 8U);
  EXPECT_EQ((*images)[0].data.data,
            (std::vector<uint8_t>{10, 20, 30, 40, 50, 60, 99, 88}));
  EXPECT_EQ((*images)[1].req_id, 1U);
  EXPECT_EQ((*images)[1].data.data, (std::vector<uint8_t>{70, 80, 90}));
  auto question = std::make_shared<std::string>("税额是多少？");
  auto other = std::make_shared<std::string>("invoice number?");
  ASSERT_EQ(DecodeConverter<std::string>("string", "ocr_invoice_qa",
                                         {question, other}, &context, &status),
            0);
  question->clear();
  ExpectTextRows(context, "question", {"税额是多少？", "invoice number?"});
}

TEST(IoConverterTest, AudioCopiesPcmAndPreservesSupportedCarrierSampleRates) {
  for (const int rate : {8000, 44100, 192000}) {
    auto row = std::make_shared<AudioInputValue>();
    *row = {{-0.5f, 0, 0.75f}, rate};
    auto second = std::make_shared<AudioInputValue>();
    *second = {{0.5f}, 16000};
    AlgContext context;
    AdapterStatus status;
    ASSERT_EQ(
        DecodeConverter<AudioInputValue>("audio_in", "audio_asr_intent",
                                         {row, second}, &context, &status),
        0);
    row->pcm.clear();
    row->sample_rate = 0;
    const auto* audio = context.Read<AudioPcmBatch>("decoded.audio");
    ASSERT_NE(audio, nullptr);
    ASSERT_EQ(audio->size(), 2U);
    EXPECT_EQ((*audio)[0].req_id, 0U);
    EXPECT_EQ((*audio)[0].sub_id, 0U);
    EXPECT_EQ((*audio)[1].req_id, 1U);
    EXPECT_EQ((*audio)[1].sub_id, 0U);
    EXPECT_EQ((*audio)[1].data.pcm_data, (std::vector<float>{0.5f}));
    EXPECT_EQ((*audio)[1].data.sample_rate, 16000);
    EXPECT_EQ((*audio)[0].data.pcm_data, (std::vector<float>{-0.5f, 0, 0.75f}));
    EXPECT_EQ((*audio)[0].data.sample_rate, rate);
  }
}

TEST(IoConverterTest, AudioRejectsRatesOutsideConverterRange) {
  for (const int rate : {7999, 192001}) {
    auto row = std::make_shared<AudioInputValue>();
    *row = {{0.25f}, rate};
    AlgContext context;
    AdapterStatus status;
    EXPECT_EQ(DecodeConverter<AudioInputValue>("audio_in", "audio_asr_intent",
                                               {row}, &context, &status),
              COMPANY_ALG_ERR_INVALID_INPUT);
    EXPECT_EQ(status.FieldPath(), "audio_in.sample_rate");
    EXPECT_EQ(status.Message(), "sample_rate out of range");
    EXPECT_FALSE(context.Has("decoded.audio"));
  }
}

TEST(IoConverterTest,
     RerankExpandsCandidatesWithRequestLocalRankAndSourceIndex) {
  auto first = std::make_shared<RerankInputValue>();
  *first = {"Q0", {"first", "second"}};
  auto second = std::make_shared<RerankInputValue>();
  *second = {"Q1", {"third"}};
  AlgContext context;
  AdapterStatus status;
  ASSERT_EQ(
      DecodeConverter<RerankInputValue>("rerank_in", "cross_rerank",
                                        {first, second}, &context, &status),
      0);
  first->query_text.clear();
  first->candidate_passages.clear();
  second.reset();
  ExpectTextRows(context, "query_text", {"Q0", "Q1"});
  const auto* candidates = context.Read<RankedTextBatch>("decoded.candidates");
  ASSERT_NE(candidates, nullptr);
  ASSERT_EQ(candidates->size(), 3U);
  const RankedTextBatch expected = {{0, 0, {"first", 0, 1, 0}},
                                    {0, 1, {"second", 0, 2, 1}},
                                    {1, 0, {"third", 0, 1, 0}}};
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ((*candidates)[i].req_id, expected[i].req_id);
    EXPECT_EQ((*candidates)[i].sub_id, expected[i].sub_id);
    EXPECT_EQ((*candidates)[i].data.text, expected[i].data.text);
    EXPECT_FLOAT_EQ((*candidates)[i].data.score, 0);
    EXPECT_EQ((*candidates)[i].data.rank, expected[i].data.rank);
    EXPECT_EQ((*candidates)[i].data.original_sub_id,
              expected[i].data.original_sub_id);
  }
}

TEST(IoConverterTest, RerankRequiresCandidateListAndBoundsEachPassage) {
  for (const auto& candidates :
       {std::vector<std::string>{},
        std::vector<std::string>{std::string(65537, 'x')}}) {
    auto row = std::make_shared<RerankInputValue>();
    *row = {"query", candidates};
    AlgContext context;
    AdapterStatus status;
    EXPECT_EQ(DecodeConverter<RerankInputValue>("rerank_in", "cross_rerank",
                                                {row}, &context, &status),
              COMPANY_ALG_ERR_INVALID_INPUT);
    EXPECT_EQ(status.FieldPath(), "rerank_in.candidate_passages");
    EXPECT_EQ(status.Message(),
              candidates.empty() ? "At least one candidate passage is required"
                                 : "candidate passage length exceeds limit");
    EXPECT_FALSE(context.Has("decoded.candidates"));
  }
}

TEST(IoConverterTest, DocumentAnswerUsesIntentStatusAndChunkCount) {
  AlgContext context;
  RuleMatchItem intent(1, "REFUND", "refund", 0.75f);
  intent.status_code = 9;
  ASSERT_TRUE(
      context.Publish("result.answer_text", TextBatch{{0, 0, "退款说明"}}));
  ASSERT_TRUE(context.Publish("result.intent", RuleMatchBatch{{0, 0, intent}}));
  ASSERT_TRUE(context.Publish("result.chunk_count", Int32Batch{{0, 0, 3}}));
  std::vector<JsonOutputValue> rows(1);
  AdapterStatus status;
  ASSERT_EQ(EncodeJsonConverter("doc_qa", &context, &rows, &status), 0);
  EXPECT_EQ(rows[0].status_code, 9);
  EXPECT_EQ(nlohmann::json::parse(rows[0].result_json),
            nlohmann::json::parse(R"({
    "intent_name":"REFUND","confidence":0.75,"answer_text":"退款说明","chunk_count":3
  })"));
  AlgContext missing_count;
  ASSERT_TRUE(
      missing_count.Publish("result.answer_text", TextBatch{{0, 0, "text"}}));
  ASSERT_TRUE(
      missing_count.Publish("result.intent", RuleMatchBatch{{0, 0, intent}}));
  EXPECT_EQ(EncodeJsonConverter("doc_qa", &missing_count, &rows, &status),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "chunk_count");
}

TEST(IoConverterTest, AudioResponseNestsIntentWithoutKeywordIsHitField) {
  RuleMatchItem intent(1, "PLAY", "play", 0.5f, "play_rule");
  intent.status_code = 5;
  intent.slots = {{"song", "夜曲"}};
  intent.matches = {
      {RuleMatchSource::kRule, "PLAY", "play_rule", "play song", 0.5f}};
  AlgContext context;
  ASSERT_TRUE(context.Publish("result.transcribed_text",
                              TextBatch{{0, 0, "播放夜曲"}}));
  ASSERT_TRUE(
      context.Publish("result.intent_slot", RuleMatchBatch{{0, 0, intent}}));
  std::vector<JsonOutputValue> rows(1);
  AdapterStatus status;
  ASSERT_EQ(EncodeJsonConverter("audio_asr_intent", &context, &rows, &status),
            0);
  EXPECT_EQ(rows[0].status_code, 5);
  EXPECT_EQ(nlohmann::json::parse(rows[0].result_json),
            nlohmann::json::parse(R"({
    "transcribed_text":"播放夜曲","intent_slot":{"intent":"PLAY","matched_word":"play",
    "confidence":0.5,"slots":{"song":"夜曲"},"matches":[{"category":"PLAY",
    "rule_id":"play_rule","pattern":"play song","score":0.5}]}
  })"));
  AlgContext missing_intent;
  ASSERT_TRUE(missing_intent.Publish("result.transcribed_text",
                                     TextBatch{{0, 0, "text"}}));
  EXPECT_EQ(
      EncodeJsonConverter("audio_asr_intent", &missing_intent, &rows, &status),
      COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "intent_slot");
}

TEST(IoConverterTest, InvoiceResponseNestsParsedResultAndCountsDetectedBoxes) {
  AlgContext context;
  JsonDocumentItem invoice(R"({"invoice_no":"I7","total":12})");
  OcrDocumentItem document;
  document.combined_text = "raw OCR omitted from external response";
  document.boxes.resize(2);
  ASSERT_TRUE(context.Publish("result.result",
                              StructuredDocumentBatch{{0, 0, invoice}}));
  ASSERT_TRUE(
      context.Publish("result.document", OcrDocumentBatch{{0, 0, document}}));
  std::vector<JsonOutputValue> rows(1);
  AdapterStatus status;
  ASSERT_EQ(EncodeJsonConverter("ocr_invoice_qa", &context, &rows, &status), 0);
  EXPECT_EQ(rows[0].status_code, 0);
  EXPECT_EQ(nlohmann::json::parse(rows[0].result_json),
            nlohmann::json::parse(R"({
    "detected_box_count":2,"result":{"invoice_no":"I7","total":12}
  })"));
  AlgContext missing_document;
  ASSERT_TRUE(missing_document.Publish(
      "result.result", StructuredDocumentBatch{{0, 0, invoice}}));
  EXPECT_EQ(
      EncodeJsonConverter("ocr_invoice_qa", &missing_document, &rows, &status),
      COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "document");
}

TEST(IoConverterTest, InvoiceRejectsFallbackEvenWithValidJson) {
  AlgContext context;
  ASSERT_TRUE(context.Publish(
      "result.result",
      StructuredDocumentBatch{
          {0, 0,
           JsonDocumentItem("{}", true, JsonParseStatus::kFallbackApplied)}}));
  ASSERT_TRUE(context.Publish("result.document", OcrDocumentBatch{{0, 0, {}}}));
  std::vector<JsonOutputValue> rows(1);
  AdapterStatus status;
  EXPECT_EQ(EncodeJsonConverter("ocr_invoice_qa", &context, &rows, &status),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "invoice_jsons");
  EXPECT_EQ(status.Message(), "Structured result failed or used fallback");
}

TEST(IoConverterTest, AuditResponseUsesStructuredRiskAndOriginalVerdictJson) {
  JsonDocumentItem verdict(
      R"({"risk_level":"SAFE","risk_score":0.25,"reason":"ok"})");
  verdict.structured_data = {
      {"risk_level", "SAFE"}, {"risk_score", 0.25}, {"reason", "ok"}};
  AlgContext context;
  ASSERT_TRUE(context.Publish("result.verdict",
                              StructuredDocumentBatch{{0, 0, verdict}}));
  ASSERT_TRUE(context.Publish(
      "result.matched_policy",
      RankedTextBatch{{0, 0, {"clause A", 0.75f, 1, 8}},
                      {0, 1, {"not the selected clause", 0.5f, 2, 9}}}));
  std::vector<JsonOutputValue> rows(1);
  AdapterStatus status;
  ASSERT_EQ(EncodeJsonConverter("dialogue_audit", &context, &rows, &status), 0);
  EXPECT_EQ(rows[0].status_code, 0);
  EXPECT_EQ(nlohmann::json::parse(rows[0].result_json),
            nlohmann::json::parse(R"({
    "risk_level":"SAFE","risk_score":0.25,"matched_policy_clause":"clause A",
    "audit_verdict":{"risk_level":"SAFE","risk_score":0.25,"reason":"ok"}
  })"));
  AlgContext missing_policy;
  ASSERT_TRUE(missing_policy.Publish("result.verdict",
                                     StructuredDocumentBatch{{0, 0, verdict}}));
  EXPECT_EQ(
      EncodeJsonConverter("dialogue_audit", &missing_policy, &rows, &status),
      COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "matched_policy");
}

TEST(IoConverterTest, AuditRejectsInvalidRiskSchemaRangeAndPolicyRank) {
  struct Case {
    const char* name;
    nlohmann::json risk;
    int rank;
    const char* detail;
  };
  const std::vector<Case> cases = {
      {"missing risk score", {{"risk_level", "SAFE"}}, 1, "types"},
      {"wrong risk level type",
       {{"risk_level", 1}, {"risk_score", 0.5}},
       1,
       "types"},
      {"unsupported risk level",
       {{"risk_level", "UNKNOWN"}, {"risk_score", 0.5}},
       1,
       "Invalid risk level or score"},
      {"negative score",
       {{"risk_level", "SAFE"}, {"risk_score", -0.1}},
       1,
       "Invalid risk level or score"},
      {"above one score",
       {{"risk_level", "SAFE"}, {"risk_score", 1.1}},
       1,
       "Invalid risk level or score"},
      {"selected policy rank is not one",
       {{"risk_level", "SAFE"}, {"risk_score", 0.5}},
       2,
       "types"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    JsonDocumentItem verdict(test.risk.dump());
    verdict.structured_data = test.risk;
    AlgContext context;
    ASSERT_TRUE(context.Publish("result.verdict",
                                StructuredDocumentBatch{{0, 0, verdict}}));
    ASSERT_TRUE(context.Publish(
        "result.matched_policy",
        RankedTextBatch{{0, 0, {"clause", 0.5f, test.rank, 0}}}));
    std::vector<JsonOutputValue> rows(1);
    AdapterStatus status;
    EXPECT_EQ(EncodeJsonConverter("dialogue_audit", &context, &rows, &status),
              COMPANY_ALG_ERR_INVALID_INPUT);
    EXPECT_EQ(status.FieldPath(), "structured_verdicts");
    EXPECT_NE(status.Message().find(test.detail), std::string::npos);
  }
}

TEST(IoConverterTest, RerankResponseSortsByRankAndReportsOriginalIndicesOnly) {
  AlgContext context;
  ASSERT_TRUE(context.Publish(
      "result.ranked", RankedTextBatch{{0, 1, {"second", 0.25f, 2, 9}},
                                       {0, 0, {"first", 0.75f, 1, 4}},
                                       {1, 0, {"other row", 0.5f, 1, 3}}}));
  std::vector<JsonOutputValue> rows(2);
  AdapterStatus status;
  ASSERT_EQ(EncodeJsonConverter("cross_rerank", &context, &rows, &status), 0);
  EXPECT_EQ(rows[0].status_code, 0);
  EXPECT_EQ(rows[1].status_code, 0);
  EXPECT_EQ(nlohmann::json::parse(rows[0].result_json),
            nlohmann::json::parse(
                R"({"scores":[0.75,0.25],"sorted_indices":[4,9],"count":2})"));
  EXPECT_EQ(nlohmann::json::parse(rows[1].result_json),
            nlohmann::json::parse(
                R"({"scores":[0.5],"sorted_indices":[3],"count":1})"));
}

TEST(IoConverterTest, RerankRequiresRanksStartingAtOneWithoutGapsOrDuplicates) {
  for (const std::vector<int>& ranks :
       {std::vector<int>{0, 1}, std::vector<int>{1, 3},
        std::vector<int>{1, 1}}) {
    AlgContext context;
    ASSERT_TRUE(context.Publish(
        "result.ranked", RankedTextBatch{{0, 0, {"A", 0.5f, ranks[0], 0}},
                                         {0, 1, {"B", 0.25f, ranks[1], 1}}}));
    std::vector<JsonOutputValue> rows(1);
    AdapterStatus status;
    EXPECT_EQ(EncodeJsonConverter("cross_rerank", &context, &rows, &status),
              COMPANY_ALG_ERR_INVALID_INPUT);
    EXPECT_EQ(status.FieldPath(), "ranked_results");
    EXPECT_EQ(status.Message(), "Invalid ranked result");
  }
}

}  // namespace
}  // namespace llm_edgeflow
