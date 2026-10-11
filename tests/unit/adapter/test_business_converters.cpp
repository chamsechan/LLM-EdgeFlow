#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_converter_registry.h"
#include "adapter/operator/mock/platform_value_binding.h"
#include "adapter/shared_algorithm_runtime.h"
#include "contracts/inference_payloads.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "platform_mock/error_codes.h"
#include "platform_mock/operator_data_types.h"
#include "tests/support/adapter_harness.h"
#include "tests/support/adapter_test_views.h"

namespace llm_edgeflow {
namespace {
constexpr auto kAnswerText = MakeBlackboardKey<TextBatch>("answer_text");
constexpr auto kIntent = MakeBlackboardKey<RuleMatchBatch>("intent");
constexpr auto kChunkCount = MakeBlackboardKey<Int32Batch>("chunk_count");
}  // namespace
}  // namespace llm_edgeflow

namespace llm_edgeflow {

namespace {
nlohmann::json OutputJson(const CompanyOperatorOutput& value) {
  return nlohmann::json::parse(
      std::string(value.result_json->data, value.result_json->length));
}
}  // namespace

class BusinessConvertersTest : public ::testing::Test {
 protected:
  void SetUp() override {
    SharedAlgorithmRuntime::GlobalInit();
    operator_api::Get_LLM_EDGEFLOW_OperatorTable().Init();
  }
  void TearDown() override {
    operator_api::Get_LLM_EDGEFLOW_OperatorTable().DeInit();
  }
};

struct JsonOutputFixture {
  char json[8192] = {};
  CompanyString text{0, json};
  CompanyOperatorOutput out{0, &text, 0};
  std::map<std::string, size_t> Capacities() const {
    return {{"result_json", sizeof(json) - 1}};
  }
};

// =========================================================================
// 业务 Converter 的字段转换；翻译协议由 TextConvertersTest 负责。
// =========================================================================

TEST_F(BusinessConvertersTest, DocQaInputFields) {
  const auto* in_conv =
      IoConverterRegistry::Instance().FindInputConverter("doc_in", "doc_qa");
  ASSERT_NE(in_conv, nullptr);

  test::AdapterHarness harness(in_conv);

  std::string doc_str = "Doc Content";
  std::string query_str = "Query Question";
  CompanyString cs_doc{static_cast<int32_t>(doc_str.size()), doc_str.data()};
  CompanyString cs_query{static_cast<int32_t>(query_str.size()),
                         query_str.data()};
  CompanyOperatorDocInput in{kMockServiceDocQa, &cs_doc, &cs_query};

  ASSERT_EQ(harness.DecodeOperator({&in}), 0);

  const auto* docs = harness.Context().Read<TextBatch>("doc_text");
  const auto* queries = harness.Context().Read<TextBatch>("query_text");
  ASSERT_NE(docs, nullptr);
  ASSERT_NE(queries, nullptr);
  EXPECT_EQ((*docs)[0].data, "Doc Content");
  EXPECT_EQ((*queries)[0].data, "Query Question");
}

TEST_F(BusinessConvertersTest, KeywordMatchFields) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "keyword_in", "keyword_match");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "json_out", "keyword_match");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(in_conv, out_conv);

  std::string text_str = "Some text";
  CompanyString cs_text{static_cast<int32_t>(text_str.size()), text_str.data()};
  CompanyOperatorKeywordInput in{kMockServiceKeywordMatch, &cs_text};
  ASSERT_EQ(harness.DecodeOperator({&in}), 0);

  RuleMatchBatch matches;
  RuleMatchItem match(1, "TEST_CAT", "测试", 0.5f);
  match.slots["flag"] = "urgent";
  matches.emplace_back(0, 0, std::move(match));
  harness.Publish("matches", std::move(matches));

  JsonOutputFixture kw_fix;
  std::vector<CompanyOperatorOutput> outputs = {kw_fix.out};
  ASSERT_EQ(harness.EncodeOperator(&outputs, kw_fix.Capacities()), 0);
  ASSERT_EQ(outputs.size(), 1U);

  EXPECT_EQ(OutputJson(outputs[0]).at("is_hit").get<int>(), 1);
  ASSERT_NE(outputs[0].result_json, nullptr);
  EXPECT_STREQ(outputs[0].result_json->data,
               "{\"confidence\":0.5,\"intent\":\"TEST_CAT\",\"is_hit\":1,"
               "\"matched_word\":"
               "\"测试\",\"matches\":[],\"slots\":{\"flag\":\"urgent\"}}");
}

TEST_F(BusinessConvertersTest, EntityExtractFields) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "entity_in", "entity_extract");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "json_out", "entity_extract");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(in_conv, out_conv);

  std::string text_str = "Entity text";
  CompanyString cs_text{static_cast<int32_t>(text_str.size()), text_str.data()};
  CompanyOperatorEntityInput in{kMockServiceEntityExtract, &cs_text};
  ASSERT_EQ(harness.DecodeOperator({&in}), 0);

  for (const std::string payload : {"[\"E1\"]", "{\"summary\":\"ok\"}"}) {
    SCOPED_TRACE(payload);
    test::AdapterHarness output_harness(out_conv);
    StructuredDocumentBatch entities;
    entities.emplace_back(
        0, 0, JsonDocumentItem(payload, true, JsonParseStatus::kOk));
    output_harness.Publish("entities", std::move(entities));

    JsonOutputFixture ent_fix;
    std::vector<CompanyOperatorOutput> outputs = {ent_fix.out};
    ASSERT_EQ(output_harness.EncodeOperator(&outputs, ent_fix.Capacities()), 0);
    ASSERT_EQ(outputs.size(), 1U);

    EXPECT_EQ(outputs[0].status_code, 0);
    ASSERT_NE(outputs[0].result_json, nullptr);
    EXPECT_EQ(std::string(outputs[0].result_json->data,
                          outputs[0].result_json->length),
              payload);
  }
}

TEST_F(BusinessConvertersTest, DialogueAuditFields) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "audit_in", "dialogue_audit");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "json_out", "dialogue_audit");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(in_conv, out_conv);

  std::string text_str = "audit sentence";
  std::string chan_str = "channel_vip";
  CompanyString cs_text{static_cast<int32_t>(text_str.size()), text_str.data()};
  CompanyString cs_chan{static_cast<int32_t>(chan_str.size()), chan_str.data()};
  CompanyOperatorAuditInput in{kMockServiceDialogueAudit, &cs_text, &cs_chan};
  ASSERT_EQ(harness.DecodeOperator({&in}), 0);

  StructuredDocumentBatch verdicts;
  verdicts.emplace_back(
      0, 0,
      JsonDocumentItem("{\"raw_verdict\":\"accepted\"}", true,
                       JsonParseStatus::kOk, "",
                       {{"risk_level", "SAFE"}, {"risk_score", 0.1f}}));
  harness.Publish("verdict", std::move(verdicts));

  RankedTextBatch policies;
  policies.emplace_back(0, 0, RankedCandidate("Clause 1", 1.0f, 1, 0));
  harness.Publish("matched_policy", std::move(policies));

  JsonOutputFixture audit_fix;
  std::vector<CompanyOperatorOutput> outputs = {audit_fix.out};
  ASSERT_EQ(harness.EncodeOperator(&outputs, audit_fix.Capacities()), 0);
  ASSERT_EQ(outputs.size(), 1U);

  EXPECT_EQ(OutputJson(outputs[0]).at("audit_verdict"),
            nlohmann::json({{"raw_verdict", "accepted"}}));
  EXPECT_FLOAT_EQ(OutputJson(outputs[0]).at("risk_score").get<float>(), 0.1f);
  ASSERT_NE(outputs[0].result_json, nullptr);
  EXPECT_STREQ(
      OutputJson(outputs[0]).at("risk_level").get<std::string>().c_str(),
      "SAFE");
  ASSERT_NE(outputs[0].result_json, nullptr);
  EXPECT_STREQ(OutputJson(outputs[0])
                   .at("matched_policy_clause")
                   .get<std::string>()
                   .c_str(),
               "Clause 1");
}

TEST_F(BusinessConvertersTest, OcrImageOwnershipAndFields) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "frame", "ocr_invoice_qa");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "json_out", "ocr_invoice_qa");
  ASSERT_NE(out_conv, nullptr);

  std::vector<uint8_t> pixels{255, 0, 0, 0, 255, 0, 99, 88};
  std::string query_str = "Total amount?";
  CompanyString cs_query{static_cast<int32_t>(query_str.size()),
                         query_str.data()};
  CompanyFrame frame{kMockServiceOcrInvoiceQa, 1, 2, 8, pixels.data(), nullptr};

  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("frame");
  ASSERT_NE(binding, nullptr);
  InputLimits limits;
  EXPECT_EQ(limits.max_image_bytes, 48U * 1024U * 1024U);
  limits.max_image_bytes = pixels.size();
  std::string error;
  EXPECT_EQ(binding->validate_external(&frame, limits, &error), 0);
  --limits.max_image_bytes;
  EXPECT_EQ(binding->validate_external(&frame, limits, &error),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(error, "CompanyFrame pixels exceed max_image_bytes");

  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["frame"] = llm_edgeflow::BorrowInputForTest({&frame});
  in_view.slots["string"] = llm_edgeflow::BorrowInputForTest({&cs_query});
  in_view.slot_types["frame"] = "CompanyFrame";
  in_view.slot_types["string"] = "CompanyString";

  test::ParsedInputOptions in_options(*in_conv);

  AlgContext ctx;
  AdapterStatus status;
  ASSERT_EQ(::llm_edgeflow::test::DecodeForTest(*in_conv, in_view, in_options,
                                                &ctx, &status),
            0);

  std::fill(pixels.begin(), pixels.end(), 0);
  std::vector<uint8_t>().swap(pixels);
  frame.data = nullptr;
  const auto* images = ctx.Read<ImageFrameBatch>("image");
  ASSERT_NE(images, nullptr);
  ASSERT_EQ(images->size(), 1U);
  EXPECT_EQ(images->front().req_id, 0U);
  EXPECT_EQ(images->front().data.width, 2);
  EXPECT_EQ(images->front().data.height, 1);
  EXPECT_EQ(images->front().data.stride, 8U);
  EXPECT_EQ(images->front().data.data,
            (std::vector<uint8_t>{255, 0, 0, 0, 255, 0, 99, 88}));

  const auto* query_converter =
      IoConverterRegistry::Instance().FindInputConverter("string",
                                                         "ocr_invoice_qa");
  ASSERT_NE(query_converter, nullptr);
  test::ParsedInputOptions query_options(*query_converter);

  ASSERT_EQ(::llm_edgeflow::test::DecodeForTest(*query_converter, in_view,
                                                query_options, &ctx, &status),
            0);
  ASSERT_NE(ctx.Read<TextBatch>("question"), nullptr);
  EXPECT_EQ(ctx.Read<TextBatch>("question")->front().data, "Total amount?");

  StructuredDocumentBatch invoices;
  invoices.emplace_back(
      0, 0, JsonDocumentItem("{\"total\":99.9}", true, JsonParseStatus::kOk));
  ctx.Publish("result", std::move(invoices));

  OcrDocumentBatch ocr_docs;
  OcrDocumentItem ocr_item;
  ocr_item.boxes.push_back({0, 0, 10, 10, "Total", 0.99f});
  ocr_docs.emplace_back(0, 0, std::move(ocr_item));
  ctx.Publish("document", std::move(ocr_docs));

  JsonOutputFixture od_fix;
  TestOutputBatchView out_view;
  out_view.count = 1;
  out_view.leased_slots["json_out"] = {&od_fix.out};
  out_view.slot_types["json_out"] = "CompanyOperatorOutput";
  out_view.SetCapacity("json_out", "result_json", sizeof(od_fix.json) - 1);

  test::ParsedOutputOptions out_options(*out_conv);

  size_t written = 0;
  ASSERT_EQ(::llm_edgeflow::test::EncodeForTest(*out_conv, &ctx, out_options,
                                                &out_view, &written, &status),
            0);

  EXPECT_EQ(written, 1U);
  EXPECT_EQ(OutputJson(od_fix.out).at("detected_box_count").get<int>(), 1);
  ASSERT_NE(od_fix.out.result_json, nullptr);
  EXPECT_EQ(OutputJson(od_fix.out).at("result"),
            nlohmann::json({{"total", 99.9}}));
}

TEST_F(BusinessConvertersTest, AudioIntentFields) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "audio_in", "audio_asr_intent");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "json_out", "audio_asr_intent");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(in_conv, out_conv);

  std::vector<float> pcm(1600, 0.05f);
  CompanyOperatorAudioInput in{kMockServiceAudioAsrIntent, pcm.data(),
                               static_cast<int>(pcm.size()), 16000};
  ASSERT_EQ(harness.DecodeOperator({&in}), 0);

  TextBatch transcripts;
  transcripts.emplace_back(0, 0, "turn left");
  harness.Publish("transcribed_text", std::move(transcripts));

  RuleMatchBatch intent_slots;
  RuleMatchItem match(1, "NAV", "left", 0.5f, "r1");
  match.matches.push_back({RuleMatchSource::kRule, "NAV", "r1", "left", 0.5f});
  match.slots["target"] = "front";
  intent_slots.emplace_back(0, 0, std::move(match));
  harness.Publish("intent_slot", std::move(intent_slots));

  JsonOutputFixture audio_fix;
  std::vector<CompanyOperatorOutput> outputs = {audio_fix.out};
  ASSERT_EQ(harness.EncodeOperator(&outputs, audio_fix.Capacities()), 0);
  ASSERT_EQ(outputs.size(), 1U);

  ASSERT_NE(outputs[0].result_json, nullptr);
  EXPECT_STREQ(
      OutputJson(outputs[0]).at("transcribed_text").get<std::string>().c_str(),
      "turn left");
  ASSERT_NE(outputs[0].result_json, nullptr);
  EXPECT_EQ(OutputJson(outputs[0]).at("intent_slot"),
            nlohmann::json({{"confidence", 0.5},
                            {"intent", "NAV"},
                            {"matched_word", "left"},
                            {"matches",
                             {{{"category", "NAV"},
                               {"pattern", "left"},
                               {"rule_id", "r1"},
                               {"score", 0.5}}}},
                            {"slots", {{"target", "front"}}}}));
}

TEST_F(BusinessConvertersTest, RankedCandidateFields) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "rerank_in", "cross_rerank");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "json_out", "cross_rerank");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(in_conv, out_conv);

  std::string q_str = "query";
  std::string cand0_str = "cand0";
  std::string cand1_str = "cand1";
  CompanyString cs_q{static_cast<int32_t>(q_str.size()), q_str.data()};
  CompanyString cs_cand0{static_cast<int32_t>(cand0_str.size()),
                         cand0_str.data()};
  CompanyString cs_cand1{static_cast<int32_t>(cand1_str.size()),
                         cand1_str.data()};

  CompanyOperatorRerankInput in{kMockServiceCrossRerank, nullptr, {}, 0};
  in.query_text = &cs_q;
  in.candidate_passages[0] = &cs_cand0;
  in.candidate_passages[1] = &cs_cand1;
  in.candidate_count = 2;

  ASSERT_EQ(harness.DecodeOperator({&in}), 0);

  RankedTextBatch ranked;
  ranked.emplace_back(0, 0, RankedCandidate("cand1", 0.85f, 1, 1));
  ranked.emplace_back(0, 1, RankedCandidate("cand0", 0.45f, 2, 0));
  harness.Publish("ranked", std::move(ranked));

  JsonOutputFixture fix;
  std::vector<CompanyOperatorOutput> outputs{fix.out};
  ASSERT_EQ(harness.EncodeOperator(&outputs, fix.Capacities()), 0);
  ASSERT_EQ(outputs.size(), 1U);

  EXPECT_EQ(OutputJson(outputs[0]).at("count").get<int>(), 2);
  EXPECT_FLOAT_EQ(OutputJson(outputs[0]).at("scores").at(0).get<float>(),
                  0.85f);
  EXPECT_EQ(OutputJson(outputs[0]).at("sorted_indices").at(0).get<int>(), 1);
  EXPECT_FLOAT_EQ(OutputJson(outputs[0]).at("scores").at(1).get<float>(),
                  0.45f);
  EXPECT_EQ(OutputJson(outputs[0]).at("sorted_indices").at(1).get<int>(), 0);
}

// =========================================================================
// 2. 契约不变量与安全边界情形
// =========================================================================

TEST_F(BusinessConvertersTest, DocQaAdapter_FailClosedWhenMissingOutputs) {
  const auto* out_conv =
      IoConverterRegistry::Instance().FindOutputConverter("json_out", "doc_qa");
  ASSERT_NE(out_conv, nullptr);

  // 情形 1：缺少 answer_text
  {
    test::AdapterHarness harness(out_conv);

    JsonOutputFixture fix;
    std::vector<CompanyOperatorOutput> outputs = {fix.out};
    EXPECT_NE(harness.EncodeOperator(&outputs, fix.Capacities()), 0);
  }

  // 情形 2：有 answer_text 但缺少 intent -> 必须 fail-closed
  {
    test::AdapterHarness harness(out_conv);

    TextBatch answers;
    answers.emplace_back(0, 0, "Some answer");
    harness.Publish("answer_text", std::move(answers));
    JsonOutputFixture fix;
    std::vector<CompanyOperatorOutput> outputs = {fix.out};
    EXPECT_EQ(harness.EncodeOperator(&outputs, fix.Capacities()),
              COMPANY_ALG_ERR_INVALID_INPUT);
  }

  // 情形 3：有 intent 但缺少显式分块计数 -> 必须 fail-closed
  {
    test::AdapterHarness harness(out_conv);

    TextBatch answers;
    answers.emplace_back(0, 0, "Some answer");
    harness.Publish("answer_text", std::move(answers));
    RuleMatchBatch intents;
    intents.emplace_back(0, 0, RuleMatchItem(1, "QA", "", 0.9f));
    harness.Publish("intent", std::move(intents));
    JsonOutputFixture fix;
    std::vector<CompanyOperatorOutput> outputs = {fix.out};
    EXPECT_EQ(harness.EncodeOperator(&outputs, fix.Capacities()),
              COMPANY_ALG_ERR_INVALID_INPUT);
  }

  // 情形 4：所有输出都存在，但批次行数与结果数量不一致
  {
    test::AdapterHarness harness(out_conv);
    TextBatch answers;
    answers.emplace_back(0, 0, "Some answer");
    harness.Publish("answer_text", std::move(answers));
    RuleMatchBatch intents;
    intents.emplace_back(0, 0, RuleMatchItem(1, "QA", "", 0.9f));
    harness.Publish("intent", std::move(intents));
    Int32Batch chunk_counts;
    chunk_counts.emplace_back(0, 0, 1);
    harness.Publish("chunk_count", std::move(chunk_counts));
    JsonOutputFixture fix;
    TestOutputBatchView destination;
    destination.count = 2;
    destination.leased_slots["json_out"] = {&fix.out};
    destination.slot_types["json_out"] = "CompanyOperatorOutput";
    destination.SetCapacity("json_out", "result_json",
                            fix.Capacities().at("result_json"));

    test::ParsedOutputOptions options(*out_conv);

    AdapterStatus status;
    size_t written = 0;
    EXPECT_EQ(::llm_edgeflow::test::EncodeForTest(*out_conv, &harness.Context(),
                                                  options, &destination,
                                                  &written, &status),
              COMPANY_ALG_ERR_INVALID_INPUT);
    EXPECT_EQ(status.FieldPath(), "answers");
    EXPECT_EQ(written, 0U);
  }
}

TEST_F(BusinessConvertersTest,
       DialogueAuditAdapter_FailClosedWhenMissingStructuredFields) {
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "json_out", "dialogue_audit");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(out_conv);

  // structured_verdicts 缺少必填字段 'risk_level' -> 必须 fail-closed
  StructuredDocumentBatch verdicts;
  nlohmann::json incomplete_obj = {{"only_verdict", "合规"}};
  verdicts.emplace_back(
      0, 0,
      JsonDocumentItem("{}", true, JsonParseStatus::kOk, "", incomplete_obj));
  harness.Publish("verdict", std::move(verdicts));

  RankedTextBatch policies;
  policies.emplace_back(0, 0, RankedCandidate("Clause", 1.0f, 1));
  harness.Publish("matched_policy", std::move(policies));

  JsonOutputFixture fix;
  std::vector<CompanyOperatorOutput> outputs = {fix.out};
  EXPECT_EQ(harness.EncodeOperator(&outputs, fix.Capacities()),
            COMPANY_ALG_ERR_INVALID_INPUT);
}

TEST_F(BusinessConvertersTest, AuditJoinsRankOneByRequestAndRejectsFallback) {
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "json_out", "dialogue_audit");
  ASSERT_NE(out_conv, nullptr);

  for (const auto parse_status :
       {JsonParseStatus::kOk, JsonParseStatus::kFailed,
        JsonParseStatus::kFallbackApplied}) {
    test::AdapterHarness harness(out_conv);

    StructuredDocumentBatch verdicts;
    for (uint32_t id : {1u, 0u}) {
      verdicts.emplace_back(
          id, 0,
          JsonDocumentItem("{}", true, parse_status, "",
                           {{"risk_level", "SAFE"}, {"risk_score", 0.1f}}));
    }
    harness.Publish("verdict", std::move(verdicts));
    harness.Publish("matched_policy",
                    RankedTextBatch{{0, 0, {"req0 first", 1.0f, 1, 1}},
                                    {0, 1, {"req0 second", 0.5f, 2, 2}},
                                    {1, 0, {"req1 first", 1.0f, 1, 1}}});

    JsonOutputFixture fix0, fix1;
    std::vector<CompanyOperatorOutput> outputs = {fix0.out, fix1.out};
    const int ret = harness.EncodeOperator(&outputs, fix0.Capacities());
    if (parse_status == JsonParseStatus::kOk) {
      ASSERT_EQ(ret, 0) << harness.Status().ToString();
      ASSERT_NE(outputs[1].result_json, nullptr);
      EXPECT_STREQ(OutputJson(outputs[1])
                       .at("matched_policy_clause")
                       .get<std::string>()
                       .c_str(),
                   "req1 first");
    } else {
      EXPECT_NE(ret, 0);
    }
  }
}

TEST_F(BusinessConvertersTest, OneToOneResultsRejectDuplicateAndOutOfRangeIds) {
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "json_out", "keyword_match");
  ASSERT_NE(out_conv, nullptr);

  for (const auto& ids :
       {std::vector<uint32_t>{0, 0}, std::vector<uint32_t>{0, 2}}) {
    test::AdapterHarness harness(out_conv);

    RuleMatchBatch matches;
    for (auto id : ids) matches.emplace_back(id, 0, RuleMatchItem{});
    harness.Publish("matches", std::move(matches));

    JsonOutputFixture fix0, fix1;
    std::vector<CompanyOperatorOutput> outputs = {fix0.out, fix1.out};
    EXPECT_EQ(harness.EncodeOperator(&outputs, fix0.Capacities()),
              COMPANY_ALG_ERR_INVALID_INPUT);
  }
}

TEST_F(BusinessConvertersTest,
       DialogueAuditAdapter_RejectsOversizedChannelName) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "audit_in", "dialogue_audit");
  ASSERT_NE(in_conv, nullptr);

  const std::string valid_channel(256, 'c');
  const std::string oversized_channel(257, 'c');
  std::string query_str = "test query";
  CompanyString cs_query{static_cast<int32_t>(query_str.size()),
                         query_str.data()};

  // 合法长度 <= 256
  {
    test::AdapterHarness harness(in_conv);
    CompanyString cs_chan{static_cast<int32_t>(valid_channel.size()),
                          const_cast<char*>(valid_channel.data())};
    CompanyOperatorAuditInput in{kMockServiceDialogueAudit, &cs_query,
                                 &cs_chan};
    EXPECT_EQ(harness.DecodeOperator({&in}), COMPANY_ALG_SUCCESS);
  }

  // 超长 > 256
  {
    test::AdapterHarness harness(in_conv);
    CompanyString cs_chan{static_cast<int32_t>(oversized_channel.size()),
                          const_cast<char*>(oversized_channel.data())};
    CompanyOperatorAuditInput in{kMockServiceDialogueAudit, &cs_query,
                                 &cs_chan};
    EXPECT_EQ(harness.DecodeOperator({&in}), COMPANY_ALG_ERR_INVALID_INPUT);
  }
}

TEST_F(BusinessConvertersTest,
       VariableDocResultPreservesLongAnswerAndOperatorCapacity) {
  const std::string long_answer(5000, 'a');

  AlgContext ctx;

  ctx.Publish("answer_text", TextBatch{{0, 0, long_answer}});
  ctx.Publish("intent",
              RuleMatchBatch{{0, 0, RuleMatchItem(1, "QA", "", 0.9f)}});
  ctx.Publish("chunk_count", Int32Batch{{0, 0, 1}});

  const auto* op_conv =
      IoConverterRegistry::Instance().FindOutputConverter("json_out", "doc_qa");
  ASSERT_NE(op_conv, nullptr);

  test::ParsedOutputOptions options(*op_conv);

  // 1. Operator 缓冲区容量较小 (500) -> BUFFER_TOO_SMALL
  {
    CompanyOperatorOutput small_out{kMockServiceDocQa, nullptr, 0};
    std::vector<char> ans_buf(500);
    CompanyString cs_ans{0, ans_buf.data()};
    small_out.result_json = &cs_ans;

    TestOutputBatchView small_dest;
    small_dest.leased_slots["json_out"].push_back(&small_out);
    small_dest.slot_types["json_out"] = "CompanyOperatorOutput";
    small_dest.SetCapacity("json_out", "result_json", 499);

    small_dest.count = 1;

    size_t written = 0;
    AdapterStatus status;
    int ret = ::llm_edgeflow::test::EncodeForTest(
        *op_conv, &ctx, options, &small_dest, &written, &status);
    EXPECT_EQ(ret, COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  }

  // 2. Operator 可变缓冲区：6000 字节存储，保留完整答案
  {
    CompanyOperatorOutput op_out{kMockServiceDocQa, nullptr, 0};
    std::vector<char> ans_buf(6000);
    CompanyString cs_ans{0, ans_buf.data()};
    op_out.result_json = &cs_ans;

    TestOutputBatchView op_dest;
    op_dest.leased_slots["json_out"].push_back(&op_out);
    op_dest.slot_types["json_out"] = "CompanyOperatorOutput";
    op_dest.SetCapacity("json_out", "result_json", ans_buf.size() - 1);

    op_dest.count = 1;

    size_t written = 0;
    AdapterStatus status;
    int ret = ::llm_edgeflow::test::EncodeForTest(*op_conv, &ctx, options,
                                                  &op_dest, &written, &status);
    EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
    EXPECT_EQ(written, 1U);
    EXPECT_EQ(
        std::string(
            OutputJson(op_out).at("answer_text").get<std::string>().c_str()),
        long_answer);
  }
}

TEST_F(BusinessConvertersTest, DocAnswerExactCapacityAndOneByteOverflow) {
  const auto* converter =
      IoConverterRegistry::Instance().FindOutputConverter("json_out", "doc_qa");
  ASSERT_NE(converter, nullptr);

  test::ParsedOutputOptions options(*converter);

  for (bool overflow : {false, true}) {
    SCOPED_TRACE(overflow);
    AlgContext context;
    context.Publish(kAnswerText,
                    TextBatch{{0, 0, overflow ? "12345" : "1234"}});
    context.Publish(kIntent,
                    RuleMatchBatch{{0, 0, RuleMatchItem(1, "QA", "", 0.9f)}});
    context.Publish(kChunkCount, Int32Batch{{0, 0, 3}});

    const auto expected = nlohmann::json({{"intent_name", "QA"},
                                          {"confidence", 0.9f},
                                          {"answer_text", "1234"},
                                          {"chunk_count", 3}})
                              .dump();
    std::vector<char> bytes(expected.size() + 2, '!');
    std::memcpy(bytes.data(), "old", 4);
    CompanyString text{3, bytes.data()};
    CompanyOperatorOutput output{kMockServiceDocQa, &text, 0};
    TestOutputBatchView view;
    view.count = 1;
    view.leased_slots["json_out"] = {&output};
    view.slot_types["json_out"] = "CompanyOperatorOutput";
    view.SetCapacity("json_out", "result_json", expected.size());
    size_t written = 0;
    AdapterStatus status;
    EXPECT_EQ(
        ::llm_edgeflow::test::EncodeForTest(*converter, &context, options,
                                            &view, &written, &status),
        overflow ? COMPANY_ALG_ERR_BUFFER_TOO_SMALL : COMPANY_ALG_SUCCESS);
    EXPECT_EQ(written, overflow ? 0U : 1U);
    EXPECT_EQ(bytes.back(), '!');
    if (overflow) {
      EXPECT_EQ(status.FieldPath(), "result_json");
      EXPECT_EQ(status.SampleIndex(), 0);
      EXPECT_STREQ(bytes.data(), "old");
      EXPECT_EQ(text.length, 3);
    } else {
      EXPECT_EQ(OutputJson(output).at("answer_text"), "1234");
      EXPECT_EQ(text.length, expected.size());
      EXPECT_EQ(bytes[expected.size()], '\0');
    }
  }
}

TEST_F(BusinessConvertersTest,
       DocQaAdapter_MultiWayResultsReorderedAndPerturbed) {
  const auto* out_conv =
      IoConverterRegistry::Instance().FindOutputConverter("json_out", "doc_qa");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(out_conv);

  // 打乱顺序：索引 1 先于索引 0 发布
  TextBatch answers{{1, 0, "Answer 1"}, {0, 0, "Answer 0"}};
  RuleMatchBatch intents{{0, 0, RuleMatchItem(1, "INTENT_0", "", 0.9f)},
                         {1, 0, RuleMatchItem(2, "INTENT_1", "", 0.8f)}};
  Int32Batch chunks{{0, 0, 3}, {1, 0, 5}};

  harness.Publish("answer_text", std::move(answers));
  harness.Publish("intent", std::move(intents));
  harness.Publish("chunk_count", std::move(chunks));

  JsonOutputFixture fix0, fix1;
  std::vector<CompanyOperatorOutput> outputs = {fix0.out, fix1.out};
  ASSERT_EQ(harness.EncodeOperator(&outputs, fix0.Capacities()), 0);
  ASSERT_EQ(outputs.size(), 2U);
  ASSERT_NE(outputs[0].result_json, nullptr);
  EXPECT_STREQ(
      OutputJson(outputs[0]).at("answer_text").get<std::string>().c_str(),
      "Answer 0");
  ASSERT_NE(outputs[0].result_json, nullptr);
  EXPECT_STREQ(
      OutputJson(outputs[0]).at("intent_name").get<std::string>().c_str(),
      "INTENT_0");
  EXPECT_EQ(OutputJson(outputs[0]).at("chunk_count").get<int>(), 3);
  EXPECT_FLOAT_EQ(OutputJson(outputs[0]).at("confidence").get<float>(), 0.9f);

  ASSERT_NE(outputs[1].result_json, nullptr);
  EXPECT_STREQ(
      OutputJson(outputs[1]).at("answer_text").get<std::string>().c_str(),
      "Answer 1");
  ASSERT_NE(outputs[1].result_json, nullptr);
  EXPECT_STREQ(
      OutputJson(outputs[1]).at("intent_name").get<std::string>().c_str(),
      "INTENT_1");
  EXPECT_EQ(OutputJson(outputs[1]).at("chunk_count").get<int>(), 5);
  EXPECT_FLOAT_EQ(OutputJson(outputs[1]).at("confidence").get<float>(), 0.8f);
}

// =========================================================================
// 不同宿主输入的语义
// =========================================================================

// 证明 1：输入 Converter 与业务声明的宿主类型匹配，
// 并通过测试 binding 证明可复用
TEST_F(BusinessConvertersTest, HostSpecificInputsShareLogicalPayload) {
  const auto* entity_conv = IoConverterRegistry::Instance().FindInputConverter(
      "entity_in", "entity_extract");
  ASSERT_NE(entity_conv, nullptr);
  EXPECT_EQ(OperatorValueTypeRegistry::Instance()
                .GetBindingBySuffix(entity_conv->type)
                ->external_c_type_name,
            "CompanyOperatorEntityInput");

  const auto* keyword_conv = IoConverterRegistry::Instance().FindInputConverter(
      "keyword_in", "keyword_match");
  ASSERT_NE(keyword_conv, nullptr);
  EXPECT_EQ(OperatorValueTypeRegistry::Instance()
                .GetBindingBySuffix(keyword_conv->type)
                ->external_c_type_name,
            "CompanyOperatorKeywordInput");

  // 用 entity converter 解码输入
  {
    test::AdapterHarness harness(entity_conv);
    std::string text_str = "entity sentence";
    CompanyString cs_text{static_cast<int32_t>(text_str.size()),
                          text_str.data()};
    CompanyOperatorEntityInput in{kMockServiceEntityExtract, &cs_text};
    EXPECT_EQ(harness.DecodeOperator({&in}), 0);
    const auto* sentences = harness.Context().Read<TextBatch>("sentence_text");
    ASSERT_NE(sentences, nullptr);
    EXPECT_EQ((*sentences)[0].data, "entity sentence");
  }

  // 用 keyword converter 解码输入
  {
    test::AdapterHarness harness(keyword_conv);
    std::string text_str = "keyword sentence";
    CompanyString cs_text{static_cast<int32_t>(text_str.size()),
                          text_str.data()};
    CompanyOperatorKeywordInput in{kMockServiceKeywordMatch, &cs_text};
    EXPECT_EQ(harness.DecodeOperator({&in}), 0);
    const auto* sentences = harness.Context().Read<TextBatch>("sentence_text");
    ASSERT_NE(sentences, nullptr);
    EXPECT_EQ((*sentences)[0].data, "keyword sentence");
  }
}

TEST_F(BusinessConvertersTest,
       DocQaOutputPreservesEmbeddedNullAndRejectsSmallPool) {
  const auto* converter =
      IoConverterRegistry::Instance().FindOutputConverter("json_out", "doc_qa");
  ASSERT_NE(converter, nullptr);
  const std::string answer("a\0b", 3);
  AlgContext ctx;

  ctx.Publish("answer_text", TextBatch{{0, 0, answer}});
  ctx.Publish("intent", RuleMatchBatch{{0, 0, RuleMatchItem{}}});
  ctx.Publish("chunk_count", Int32Batch{{0, 0, 1}});
  test::ParsedOutputOptions options(*converter);

  char bytes[256] = {};
  CompanyString answer_out{0, bytes};
  CompanyOperatorOutput output{kMockServiceDocQa, nullptr, 0};
  output.result_json = &answer_out;

  TestOutputBatchView view;
  view.count = 1;
  view.leased_slots["json_out"] = {&output};
  view.slot_types["json_out"] = "CompanyOperatorOutput";
  view.SetCapacity("json_out", "result_json", 255);

  size_t written = 0;
  AdapterStatus status;
  ASSERT_EQ(::llm_edgeflow::test::EncodeForTest(*converter, &ctx, options,
                                                &view, &written, &status),
            COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(OutputJson(output).at("answer_text").get<std::string>(), answer);
  EXPECT_EQ(bytes[answer_out.length], '\0');

  view.SetCapacity("json_out", "result_json", 1);
  written = 0;
  EXPECT_NE(::llm_edgeflow::test::EncodeForTest(*converter, &ctx, options,
                                                &view, &written, &status),
            COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 0U);
}

}  // namespace llm_edgeflow
