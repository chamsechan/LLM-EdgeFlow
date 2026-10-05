#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/deployment_io_config.h"
#include "adapter/io_binding_registry.h"
#include "adapter/io_binding_resolver.h"
#include "adapter/io_converter.h"
#include "adapter/io_converter_registry.h"
#include "adapter/shared_algorithm_runtime.h"
#include "contracts/inference_payloads.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/pipeline_catalog.h"
#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"
#include "platform_mock/error_codes.h"
#include "platform_mock/operator_data_types.h"
#include "tests/support/adapter_harness.h"
#include "tests/support/adapter_test_views.h"

namespace llm_edgeflow {

class AdapterPurityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    SharedAlgorithmRuntime::GlobalInit();
    operator_api::Get_LLM_EDGEFLOW_OperatorTable().Init();
  }
  void TearDown() override {
    operator_api::Get_LLM_EDGEFLOW_OperatorTable().DeInit();
  }
};

struct CustomMultiFieldInput {
  uint64_t req_id;
  const char* topic;
  const char* content;
};

DECLARE_EXTERNAL_TYPE_TRAITS(CustomMultiFieldInput, "CustomMultiFieldInput");

struct DocOutputFixture {
  char ans[512] = {0};
  CompanyString cs_ans{511, ans};
  char intent[128] = {0};
  CompanyString cs_intent{127, intent};
  CompanyOperatorDocOutput out{};
  DocOutputFixture() {
    out.answer_text = &cs_ans;
    out.intent_name = &cs_intent;
  }
  std::map<std::string, size_t> Capacities() const {
    return {{"answer_text", sizeof(ans) - 1},
            {"intent_name", sizeof(intent) - 1}};
  }
};

struct KeywordOutputFixture {
  char match[2048] = {0};
  CompanyString cs_match{2047, match};
  CompanyOperatorKeywordOutput out{};
  KeywordOutputFixture() { out.match_result_json = &cs_match; }
  std::map<std::string, size_t> Capacities() const {
    return {{"match_result_json", sizeof(match) - 1}};
  }
};

struct EntityOutputFixture {
  char entities[2048] = {0};
  CompanyString cs_entities{2047, entities};
  CompanyOperatorEntityOutput out{};
  EntityOutputFixture() { out.entities_json = &cs_entities; }
  std::map<std::string, size_t> Capacities() const {
    return {{"entities_json", sizeof(entities) - 1}};
  }
};

struct AuditOutputFixture {
  char risk[64] = {0};
  CompanyString cs_risk{63, risk};
  char clause[512] = {0};
  CompanyString cs_clause{511, clause};
  char verdict[2048] = {0};
  CompanyString cs_verdict{2047, verdict};
  CompanyOperatorAuditOutput out{};
  AuditOutputFixture() {
    out.risk_level = &cs_risk;
    out.matched_policy_clause = &cs_clause;
    out.audit_verdict_json = &cs_verdict;
  }
  std::map<std::string, size_t> Capacities() const {
    return {{"risk_level", sizeof(risk) - 1},
            {"matched_policy_clause", sizeof(clause) - 1},
            {"audit_verdict_json", sizeof(verdict) - 1}};
  }
};

struct OdOutputFixture {
  char json[2048] = {0};
  CompanyString cs_json{2047, json};
  CompanyOdOutput out{};
  OdOutputFixture() { out.result_json = &cs_json; }
  std::map<std::string, size_t> Capacities() const {
    return {{"result_json", sizeof(json) - 1}};
  }
};

struct AudioOutputFixture {
  char text[512] = {0};
  CompanyString cs_text{511, text};
  char slot[2048] = {0};
  CompanyString cs_slot{2047, slot};
  CompanyOperatorAudioOutput out{};
  AudioOutputFixture() {
    out.transcribed_text = &cs_text;
    out.intent_slot_json = &cs_slot;
  }
  std::map<std::string, size_t> Capacities() const {
    return {{"transcribed_text", sizeof(text) - 1},
            {"intent_slot_json", sizeof(slot) - 1}};
  }
};

// =========================================================================
// 1. 全部 8 个业务的 Converter 纯净性
// =========================================================================

TEST_F(AdapterPurityTest, DocQaAdapterPurity) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "doc_query.plain.operator.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "doc_answer.plain.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(in_conv, out_conv);

  std::string doc_str = "Doc Content";
  std::string query_str = "Query Question";
  CompanyString cs_doc{static_cast<int32_t>(doc_str.size()), doc_str.data()};
  CompanyString cs_query{static_cast<int32_t>(query_str.size()),
                         query_str.data()};
  CompanyOperatorDocInput in{1001, &cs_doc, &cs_query};

  ASSERT_EQ(harness.DecodeOperator({&in}), 0);

  const auto& req_ids = harness.RequestIds();
  const auto* docs = harness.Context().Read<TextBatch>("raw_docs");
  const auto* queries = harness.Context().Read<TextBatch>("raw_queries");
  ASSERT_EQ(req_ids.size(), 1U);
  ASSERT_NE(docs, nullptr);
  ASSERT_NE(queries, nullptr);
  EXPECT_EQ(req_ids[0], 1001u);
  EXPECT_EQ((*docs)[0].data, "Doc Content");
  EXPECT_EQ((*queries)[0].data, "Query Question");

  // 输出编码
  TextBatch answers;
  answers.emplace_back(0, 0, "Model Generated Answer");
  harness.Publish("llm_answers", std::move(answers));

  RuleMatchBatch intents;
  intents.emplace_back(0, 0, RuleMatchItem(1, "GENERAL_QA", "query", 0.95f));
  harness.Publish("intent_matches", std::move(intents));

  Int32Batch chunk_counts;
  chunk_counts.emplace_back(0, 0, 1);
  harness.Publish("doc_chunk_counts", std::move(chunk_counts));

  DocOutputFixture doc_fix;
  std::vector<CompanyOperatorDocOutput> outputs = {doc_fix.out};
  ASSERT_EQ(harness.EncodeOperator(&outputs, doc_fix.Capacities()), 0);

  EXPECT_EQ(outputs[0].request_id, 1001u);
  EXPECT_EQ(outputs[0].chunk_count, 1);
  ASSERT_NE(outputs[0].intent_name, nullptr);
  EXPECT_STREQ(outputs[0].intent_name->data, "GENERAL_QA");
  EXPECT_FLOAT_EQ(outputs[0].confidence, 0.95f);
  ASSERT_NE(outputs[0].answer_text, nullptr);
  EXPECT_STREQ(outputs[0].answer_text->data, "Model Generated Answer");
}

TEST_F(AdapterPurityTest, KeywordMatchAdapterPurity) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "text.plain.operator.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "keyword.result.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(in_conv, out_conv);

  std::string text_str = "Some text";
  CompanyString cs_text{static_cast<int32_t>(text_str.size()), text_str.data()};
  CompanyOperatorEntityInput in{1002, &cs_text};
  ASSERT_EQ(harness.DecodeOperator({&in}), 0);

  RuleMatchBatch matches;
  matches.emplace_back(0, 0, RuleMatchItem(1, "TEST_CAT", "测试", 0.5f));
  harness.Publish("rule_matches", std::move(matches));

  KeywordOutputFixture kw_fix;
  std::vector<CompanyOperatorKeywordOutput> outputs = {kw_fix.out};
  ASSERT_EQ(harness.EncodeOperator(&outputs, kw_fix.Capacities()), 0);

  EXPECT_EQ(outputs[0].request_id, 1002u);
  EXPECT_EQ(outputs[0].is_hit, 1);
  ASSERT_NE(outputs[0].match_result_json, nullptr);
  EXPECT_STREQ(outputs[0].match_result_json->data,
               "{\"confidence\":0.5,\"intent\":\"TEST_CAT\",\"matched_word\":"
               "\"测试\",\"matches\":[],\"slots\":{}}");
}

TEST_F(AdapterPurityTest, EntityExtractAdapterPurity) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "text.plain.operator.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "document.structured.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(in_conv, out_conv);

  std::string text_str = "Entity text";
  CompanyString cs_text{static_cast<int32_t>(text_str.size()), text_str.data()};
  CompanyOperatorEntityInput in{1003, &cs_text};
  ASSERT_EQ(harness.DecodeOperator({&in}), 0);

  StructuredDocumentBatch entities;
  entities.emplace_back(
      0, 0, JsonDocumentItem("[\"E1\"]", true, JsonParseStatus::kOk));
  harness.Publish("extracted_entities", std::move(entities));

  EntityOutputFixture ent_fix;
  std::vector<CompanyOperatorEntityOutput> outputs = {ent_fix.out};
  ASSERT_EQ(harness.EncodeOperator(&outputs, ent_fix.Capacities()), 0);

  EXPECT_EQ(outputs[0].request_id, 1003u);
  EXPECT_EQ(outputs[0].status_code, 0);
  ASSERT_NE(outputs[0].entities_json, nullptr);
  EXPECT_STREQ(outputs[0].entities_json->data, "[\"E1\"]");
}

TEST_F(AdapterPurityTest, DialogueAuditAdapterPurity) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "audit.plain.operator.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "audit_result.plain.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(in_conv, out_conv);

  std::string text_str = "audit sentence";
  std::string chan_str = "channel_vip";
  CompanyString cs_text{static_cast<int32_t>(text_str.size()), text_str.data()};
  CompanyString cs_chan{static_cast<int32_t>(chan_str.size()), chan_str.data()};
  CompanyOperatorAuditInput in{1004, &cs_text, &cs_chan};
  ASSERT_EQ(harness.DecodeOperator({&in}), 0);

  StructuredDocumentBatch verdicts;
  verdicts.emplace_back(
      0, 0,
      JsonDocumentItem("{\"risk_level\":\"SAFE\",\"risk_score\":0.1}", true,
                       JsonParseStatus::kOk, "",
                       {{"risk_level", "SAFE"}, {"risk_score", 0.1f}}));
  harness.Publish("structured_verdicts", std::move(verdicts));

  RankedTextBatch policies;
  policies.emplace_back(0, 0, RankedCandidate("Clause 1", 1.0f, 1, 0));
  harness.Publish("matched_policy", std::move(policies));

  AuditOutputFixture audit_fix;
  std::vector<CompanyOperatorAuditOutput> outputs = {audit_fix.out};
  ASSERT_EQ(harness.EncodeOperator(&outputs, audit_fix.Capacities()), 0);

  EXPECT_EQ(outputs[0].request_id, 1004u);
  EXPECT_FLOAT_EQ(outputs[0].risk_score, 0.1f);
  ASSERT_NE(outputs[0].risk_level, nullptr);
  EXPECT_STREQ(outputs[0].risk_level->data, "SAFE");
  ASSERT_NE(outputs[0].matched_policy_clause, nullptr);
  EXPECT_STREQ(outputs[0].matched_policy_clause->data, "Clause 1");
}

TEST_F(AdapterPurityTest, OcrInvoiceQaAdapterPurity) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "image_query.plain.operator.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "invoice_result.plain.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  std::string path_str = "/path/invoice.jpg";
  std::string query_str = "Total amount?";
  CompanyString cs_path{static_cast<int32_t>(path_str.size()), path_str.data()};
  CompanyString cs_query{static_cast<int32_t>(query_str.size()),
                         query_str.data()};
  CompanyFrame frame{1005, &cs_path, nullptr};

  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["frame"] = llm_edgeflow::BorrowInputForTest({&frame});
  in_view.slots["string"] = llm_edgeflow::BorrowInputForTest({&cs_query});
  in_view.slot_types["frame"] = "CompanyFrame";
  in_view.slot_types["string"] = "CompanyString";

  std::vector<uint64_t> request_ids;
  InputDecodeOptions in_options;
  in_options.request_ids = &request_ids;
  in_options.converter_id = in_conv->converter_id;

  AlgContext ctx;
  AdapterStatus status;
  ASSERT_EQ(in_conv->decode_fn(in_view, in_options, &ctx, &status), 0);

  StructuredDocumentBatch invoices;
  invoices.emplace_back(
      0, 0, JsonDocumentItem("{\"total\":99.9}", true, JsonParseStatus::kOk));
  ctx.Publish("extracted_invoice_json", std::move(invoices));

  OcrDocumentBatch ocr_docs;
  OcrDocumentItem ocr_item;
  ocr_item.boxes.push_back({0, 0, 10, 10, "Total", 0.99f});
  ocr_docs.emplace_back(0, 0, std::move(ocr_item));
  ctx.Publish("ocr_docs", std::move(ocr_docs));

  OdOutputFixture od_fix;
  TestOutputBatchView out_view;
  out_view.count = 1;
  out_view.leased_slots["od_out"] = {&od_fix.out};
  out_view.slot_types["od_out"] = "CompanyOdOutput";
  out_view.SetCapacity("od_out", "result_json", sizeof(od_fix.json) - 1);

  OutputEncodeOptions out_options;
  out_options.request_ids = &request_ids;
  out_options.converter_id = out_conv->converter_id;

  size_t written = 0;
  ASSERT_EQ(
      out_conv->encode_fn(&ctx, out_options, &out_view, &written, &status), 0);

  EXPECT_EQ(od_fix.out.request_id, 1005u);
  EXPECT_EQ(od_fix.out.detected_box_count, 1);
  ASSERT_NE(od_fix.out.result_json, nullptr);
  EXPECT_STREQ(od_fix.out.result_json->data, "{\"total\":99.9}");
}

TEST_F(AdapterPurityTest, AudioAsrIntentAdapterPurity) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "audio.pcm.operator.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "audio_result.plain.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(in_conv, out_conv);

  std::vector<float> pcm(1600, 0.05f);
  CompanyOperatorAudioInput in{1006, pcm.data(), static_cast<int>(pcm.size()),
                               16000};
  ASSERT_EQ(harness.DecodeOperator({&in}), 0);

  TextBatch transcripts;
  transcripts.emplace_back(0, 0, "turn left");
  harness.Publish("transcripts", std::move(transcripts));

  RuleMatchBatch intent_slots;
  intent_slots.emplace_back(0, 0, RuleMatchItem(1, "NAV", "", 0.5f));
  harness.Publish("intent_slots", std::move(intent_slots));

  AudioOutputFixture audio_fix;
  std::vector<CompanyOperatorAudioOutput> outputs = {audio_fix.out};
  ASSERT_EQ(harness.EncodeOperator(&outputs, audio_fix.Capacities()), 0);

  EXPECT_EQ(outputs[0].request_id, 1006u);
  ASSERT_NE(outputs[0].transcribed_text, nullptr);
  EXPECT_STREQ(outputs[0].transcribed_text->data, "turn left");
  ASSERT_NE(outputs[0].intent_slot_json, nullptr);
  EXPECT_STREQ(outputs[0].intent_slot_json->data,
               "{\"confidence\":0.5,\"intent\":\"NAV\",\"matched_word\":\"\","
               "\"matches\":[],\"slots\":{}}");
}

TEST_F(AdapterPurityTest, CrossRerankAdapterPurity) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "rerank.plain.operator.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "rerank_result.plain.operator.v1");
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

  CompanyOperatorRerankInput in{};
  in.request_id = 1007;
  in.query_text = &cs_q;
  in.candidate_passages[0] = &cs_cand0;
  in.candidate_passages[1] = &cs_cand1;
  in.candidate_count = 2;

  ASSERT_EQ(harness.DecodeOperator({&in}), 0);

  RankedTextBatch ranked;
  ranked.emplace_back(0, 0, RankedCandidate("cand1", 0.85f, 1, 1));
  ranked.emplace_back(0, 1, RankedCandidate("cand0", 0.45f, 2, 0));
  harness.Publish("ranked_results", std::move(ranked));

  std::vector<CompanyOperatorRerankOutput> outputs(1);
  ASSERT_EQ(harness.EncodeOperator(&outputs), 0);

  EXPECT_EQ(outputs[0].request_id, 1007u);
  EXPECT_EQ(outputs[0].count, 2);
  EXPECT_FLOAT_EQ(outputs[0].scores[0], 0.85f);
  EXPECT_EQ(outputs[0].sorted_indices[0], 1);
  EXPECT_FLOAT_EQ(outputs[0].scores[1], 0.45f);
  EXPECT_EQ(outputs[0].sorted_indices[1], 0);
}

TEST_F(AdapterPurityTest, TranslateAdapterPurity) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "translate.json.operator.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "translate.json.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(in_conv, out_conv);

  std::string json_query = "{\"query\":\"Hello\"}";
  CompanyString cs_text{static_cast<int32_t>(json_query.size()),
                        json_query.data()};
  CompanyOperatorEntityInput in{1008, &cs_text};
  ASSERT_EQ(harness.DecodeOperator({&in}), 0);

  TextBatch answers;
  answers.emplace_back(0, 0, "Bonjour");
  harness.Publish("llm_answers", std::move(answers));

  EntityOutputFixture ent_fix;
  std::vector<CompanyOperatorEntityOutput> outputs = {ent_fix.out};
  ASSERT_EQ(harness.EncodeOperator(&outputs, ent_fix.Capacities()), 0);

  EXPECT_EQ(outputs[0].request_id, 1008u);
  EXPECT_EQ(outputs[0].status_code, 0);
  ASSERT_NE(outputs[0].entities_json, nullptr);
  auto parsed = nlohmann::json::parse(outputs[0].entities_json->data);
  EXPECT_EQ(parsed["translated"], "Bonjour");
}

// =========================================================================
// 2. 契约不变量与安全边界情形
// =========================================================================

TEST_F(AdapterPurityTest, DocQaAdapter_FailClosedWhenMissingOutputs) {
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "doc_answer.plain.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  // 情形 1：缺少 llm_answers
  {
    test::AdapterHarness harness(out_conv);
    harness.SetRequestIds(std::vector<uint64_t>{1001});
    DocOutputFixture fix;
    std::vector<CompanyOperatorDocOutput> outputs = {fix.out};
    EXPECT_NE(harness.EncodeOperator(&outputs, fix.Capacities()), 0);
  }

  // 情形 2：有 llm_answers 但缺少 intent_matches -> 必须 fail-closed
  {
    test::AdapterHarness harness(out_conv);
    harness.SetRequestIds(std::vector<uint64_t>{1001});
    TextBatch answers;
    answers.emplace_back(0, 0, "Some answer");
    harness.Publish("llm_answers", std::move(answers));
    DocOutputFixture fix;
    std::vector<CompanyOperatorDocOutput> outputs = {fix.out};
    EXPECT_EQ(harness.EncodeOperator(&outputs, fix.Capacities()),
              COMPANY_ALG_ERR_INVALID_INPUT);
  }

  // 情形 3：有 intent_matches 但缺少显式分块计数 -> 必须 fail-closed
  {
    test::AdapterHarness harness(out_conv);
    harness.SetRequestIds(std::vector<uint64_t>{1001});
    TextBatch answers;
    answers.emplace_back(0, 0, "Some answer");
    harness.Publish("llm_answers", std::move(answers));
    RuleMatchBatch intents;
    intents.emplace_back(0, 0, RuleMatchItem(1, "QA", "", 0.9f));
    harness.Publish("intent_matches", std::move(intents));
    DocOutputFixture fix;
    std::vector<CompanyOperatorDocOutput> outputs = {fix.out};
    EXPECT_EQ(harness.EncodeOperator(&outputs, fix.Capacities()),
              COMPANY_ALG_ERR_INVALID_INPUT);
  }

  // 情形 4：所有输出都存在，但缺少请求 ID 表
  {
    test::AdapterHarness harness(out_conv);
    TextBatch answers;
    answers.emplace_back(0, 0, "Some answer");
    harness.Publish("llm_answers", std::move(answers));
    RuleMatchBatch intents;
    intents.emplace_back(0, 0, RuleMatchItem(1, "QA", "", 0.9f));
    harness.Publish("intent_matches", std::move(intents));
    Int32Batch chunk_counts;
    chunk_counts.emplace_back(0, 0, 1);
    harness.Publish("doc_chunk_counts", std::move(chunk_counts));
    DocOutputFixture fix;
    TestOutputBatchView destination;
    destination.count = 1;
    destination.leased_slots["doc_out"] = {&fix.out};
    destination.slot_types["doc_out"] = "CompanyOperatorDocOutput";
    destination.SetCapacity("doc_out", "answer_text",
                            fix.Capacities().at("answer_text"));
    destination.SetCapacity("doc_out", "intent_name",
                            fix.Capacities().at("intent_name"));
    OutputEncodeOptions options;
    options.converter_id = out_conv->converter_id;
    AdapterStatus status;
    size_t written = 0;
    EXPECT_EQ(out_conv->encode_fn(&harness.Context(), options, &destination,
                                  &written, &status),
              COMPANY_ALG_ERR_INVALID_INPUT);
    EXPECT_EQ(status.FieldPath(), "request_ids");
  }
}

TEST_F(AdapterPurityTest,
       DialogueAuditAdapter_FailClosedWhenMissingStructuredFields) {
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "audit_result.plain.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(out_conv);
  harness.SetRequestIds(std::vector<uint64_t>{1001});

  // structured_verdicts 缺少必填字段 'risk_level' -> 必须 fail-closed
  StructuredDocumentBatch verdicts;
  nlohmann::json incomplete_obj = {{"only_verdict", "合规"}};
  verdicts.emplace_back(
      0, 0,
      JsonDocumentItem("{}", true, JsonParseStatus::kOk, "", incomplete_obj));
  harness.Publish("structured_verdicts", std::move(verdicts));

  RankedTextBatch policies;
  policies.emplace_back(0, 0, RankedCandidate("Clause", 1.0f, 1));
  harness.Publish("matched_policy", std::move(policies));

  AuditOutputFixture fix;
  std::vector<CompanyOperatorAuditOutput> outputs = {fix.out};
  EXPECT_EQ(harness.EncodeOperator(&outputs, fix.Capacities()),
            COMPANY_ALG_ERR_INVALID_INPUT);
}

TEST_F(AdapterPurityTest, AuditJoinsRankOneByRequestAndRejectsFallback) {
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "audit_result.plain.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  for (const auto parse_status :
       {JsonParseStatus::kOk, JsonParseStatus::kFailed,
        JsonParseStatus::kFallbackApplied}) {
    test::AdapterHarness harness(out_conv);
    harness.SetRequestIds(std::vector<uint64_t>{100, 200});

    StructuredDocumentBatch verdicts;
    for (uint32_t id : {1u, 0u}) {
      verdicts.emplace_back(
          id, 0,
          JsonDocumentItem("{}", true, parse_status, "",
                           {{"risk_level", "SAFE"}, {"risk_score", 0.1f}}));
    }
    harness.Publish("structured_verdicts", std::move(verdicts));
    harness.Publish("matched_policy",
                    RankedTextBatch{{0, 0, {"req0 first", 1.0f, 1, 1}},
                                    {0, 1, {"req0 second", 0.5f, 2, 2}},
                                    {1, 0, {"req1 first", 1.0f, 1, 1}}});

    AuditOutputFixture fix0, fix1;
    std::vector<CompanyOperatorAuditOutput> outputs = {fix0.out, fix1.out};
    const int ret = harness.EncodeOperator(&outputs, fix0.Capacities());
    if (parse_status == JsonParseStatus::kOk) {
      ASSERT_EQ(ret, 0) << harness.Status().ToString();
      EXPECT_EQ(outputs[1].request_id, 200u);
      ASSERT_NE(outputs[1].matched_policy_clause, nullptr);
      EXPECT_STREQ(outputs[1].matched_policy_clause->data, "req1 first");
    } else {
      EXPECT_NE(ret, 0);
    }
  }
}

TEST_F(AdapterPurityTest, OneToOneResultsRejectDuplicateAndOutOfRangeIds) {
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "keyword.result.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  for (const auto& ids :
       {std::vector<uint32_t>{0, 0}, std::vector<uint32_t>{0, 2}}) {
    test::AdapterHarness harness(out_conv);
    harness.SetRequestIds(std::vector<uint64_t>{100, 200});
    RuleMatchBatch matches;
    for (auto id : ids) matches.emplace_back(id, 0, RuleMatchItem{});
    harness.Publish("rule_matches", std::move(matches));

    KeywordOutputFixture fix0, fix1;
    std::vector<CompanyOperatorKeywordOutput> outputs = {fix0.out, fix1.out};
    EXPECT_EQ(harness.EncodeOperator(&outputs, fix0.Capacities()),
              COMPANY_ALG_ERR_INVALID_INPUT);
  }
}

TEST_F(AdapterPurityTest, DialogueAuditAdapter_RejectsOversizedChannelName) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "audit.plain.operator.v1");
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
    CompanyOperatorAuditInput in{5001, &cs_query, &cs_chan};
    EXPECT_EQ(harness.DecodeOperator({&in}), COMPANY_ALG_SUCCESS);
  }

  // 超长 > 256
  {
    test::AdapterHarness harness(in_conv);
    CompanyString cs_chan{static_cast<int32_t>(oversized_channel.size()),
                          const_cast<char*>(oversized_channel.data())};
    CompanyOperatorAuditInput in{5002, &cs_query, &cs_chan};
    EXPECT_EQ(harness.DecodeOperator({&in}), COMPANY_ALG_ERR_INVALID_INPUT);
  }
}

TEST_F(AdapterPurityTest,
       VariableDocResultPreservesLongAnswerAndOperatorCapacity) {
  const std::string long_answer(5000, 'a');

  AlgContext ctx;
  const std::vector<uint64_t> request_ids{10};
  ctx.Publish("llm_answers", TextBatch{{0, 0, long_answer}});
  ctx.Publish("intent_matches",
              RuleMatchBatch{{0, 0, RuleMatchItem(1, "QA", "", 0.9f)}});
  ctx.Publish("doc_chunk_counts", Int32Batch{{0, 0, 1}});

  const auto* op_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "doc_answer.plain.operator.v1");
  ASSERT_NE(op_conv, nullptr);

  OutputEncodeOptions options;
  options.request_ids = &request_ids;
  options.converter_id = op_conv->converter_id;

  // 1. Operator 缓冲区容量较小 (500) -> BUFFER_TOO_SMALL
  {
    CompanyOperatorDocOutput small_out{};
    std::vector<char> ans_buf(500);
    CompanyString cs_ans{0, ans_buf.data()};
    small_out.answer_text = &cs_ans;
    std::vector<char> int_buf(128);
    CompanyString cs_int{0, int_buf.data()};
    small_out.intent_name = &cs_int;

    TestOutputBatchView small_dest;
    small_dest.leased_slots["doc_out"].push_back(&small_out);
    small_dest.slot_types["doc_out"] = "CompanyOperatorDocOutput";
    small_dest.SetCapacity("doc_out", "answer_text", 499);
    small_dest.SetCapacity("doc_out", "intent_name", 127);
    small_dest.count = 1;

    size_t written = 0;
    AdapterStatus status;
    int ret = op_conv->encode_fn(&ctx, options, &small_dest, &written, &status);
    EXPECT_EQ(ret, COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  }

  // 2. Operator 可变缓冲区：6000 字节存储，保留完整答案
  {
    CompanyOperatorDocOutput op_out{};
    std::vector<char> ans_buf(6000);
    CompanyString cs_ans{0, ans_buf.data()};
    op_out.answer_text = &cs_ans;
    std::vector<char> int_buf(128);
    CompanyString cs_int{0, int_buf.data()};
    op_out.intent_name = &cs_int;

    TestOutputBatchView op_dest;
    op_dest.leased_slots["doc_out"].push_back(&op_out);
    op_dest.slot_types["doc_out"] = "CompanyOperatorDocOutput";
    op_dest.SetCapacity("doc_out", "answer_text", ans_buf.size() - 1);
    op_dest.SetCapacity("doc_out", "intent_name", int_buf.size() - 1);
    op_dest.count = 1;

    size_t written = 0;
    AdapterStatus status;
    int ret = op_conv->encode_fn(&ctx, options, &op_dest, &written, &status);
    EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
    EXPECT_EQ(written, 1U);
    EXPECT_EQ(op_out.request_id, 10U);
    EXPECT_EQ(std::string(op_out.answer_text->data), long_answer);
  }
}

TEST_F(AdapterPurityTest, DocAnswerExactCapacityAndOneByteOverflow) {
  const auto* converter = IoConverterRegistry::Instance().FindOutputConverter(
      "doc_answer.plain.operator.v1");
  ASSERT_NE(converter, nullptr);
  const std::vector<uint64_t> request_ids{42};
  OutputEncodeOptions options;
  options.request_ids = &request_ids;
  options.converter_id = converter->converter_id;

  for (bool overflow : {false, true}) {
    SCOPED_TRACE(overflow);
    AlgContext context;
    context.Publish(kLlmAnswers,
                    TextBatch{{0, 0, overflow ? "12345" : "1234"}});
    context.Publish(kIntentMatches,
                    RuleMatchBatch{{0, 0, RuleMatchItem(1, "QA", "", 0.9f)}});
    context.Publish(kDocChunkCounts, Int32Batch{{0, 0, 3}});

    // 容量按 payload 字节计；存储还需为结束符预留空间。
    char answer[6] = {'o', 'l', 'd', '\0', '#', '!'};
    char intent[4] = {'?', '?', '?', '!'};
    CompanyString answer_string{3, answer};
    CompanyString intent_string{0, intent};
    CompanyOperatorDocOutput output{};
    output.answer_text = &answer_string;
    output.intent_name = &intent_string;
    TestOutputBatchView view;
    view.count = 1;
    view.leased_slots["doc_out"] = {&output};
    view.slot_types["doc_out"] = "CompanyOperatorDocOutput";
    view.SetCapacity("doc_out", "answer_text", 4);
    view.SetCapacity("doc_out", "intent_name", 2);
    size_t written = 0;
    AdapterStatus status;
    EXPECT_EQ(
        converter->encode_fn(&context, options, &view, &written, &status),
        overflow ? COMPANY_ALG_ERR_BUFFER_TOO_SMALL : COMPANY_ALG_SUCCESS);
    EXPECT_EQ(written, overflow ? 0U : 1U);
    EXPECT_STREQ(intent, "QA");
    EXPECT_EQ(intent_string.length, 2);
    EXPECT_EQ(intent[3], '!');
    EXPECT_EQ(answer[5], '!');
    if (overflow) {
      EXPECT_EQ(status.FieldPath(), "answer_text");
      EXPECT_EQ(status.SampleIndex(), 0);
      EXPECT_STREQ(answer, "old");
      EXPECT_EQ(answer_string.length, 3);
      EXPECT_EQ(answer[4], '#');
    } else {
      EXPECT_STREQ(answer, "1234");
      EXPECT_EQ(answer_string.length, 4);
      EXPECT_EQ(answer[4], '\0');
    }
  }
}

TEST_F(AdapterPurityTest, InputBatchSkeleton_CopyInPurity) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "translate.json.operator.v1");
  ASSERT_NE(in_conv, nullptr);

  test::AdapterHarness harness(in_conv);

  std::string buffer = "{\"query\":\"original query\"}";
  CompanyString cs_buf{static_cast<int32_t>(buffer.size()), buffer.data()};
  CompanyOperatorEntityInput in{5555, &cs_buf};

  ASSERT_EQ(harness.DecodeOperator({&in}), 0);

  // 覆写外部缓冲区
  buffer[11] = 'X';
  buffer[12] = 'X';

  const auto* sentences = harness.Context().Read<TextBatch>("input_sentences");
  ASSERT_NE(sentences, nullptr);
  EXPECT_EQ((*sentences)[0].data, "original query");
}

TEST_F(AdapterPurityTest, InputBatchSkeleton_ExternalDuplicateIdsAllowed) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "translate.json.operator.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "translate.json.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(in_conv, out_conv);

  std::string q0 = "{\"query\":\"q0\"}";
  std::string q1 = "{\"query\":\"q1\"}";
  CompanyString cs_q0{static_cast<int32_t>(q0.size()), q0.data()};
  CompanyString cs_q1{static_cast<int32_t>(q1.size()), q1.data()};
  CompanyOperatorEntityInput in0{1234, &cs_q0};
  CompanyOperatorEntityInput in1{1234, &cs_q1};

  ASSERT_EQ(harness.DecodeOperator({&in0, &in1}), 0);

  const auto& req_ids = harness.RequestIds();
  const auto* sentences = harness.Context().Read<TextBatch>("input_sentences");
  ASSERT_EQ(req_ids.size(), 2U);
  ASSERT_NE(sentences, nullptr);
  EXPECT_EQ(req_ids[0], 1234u);
  EXPECT_EQ(req_ids[1], 1234u);
  EXPECT_EQ((*sentences)[0].req_id, 0u);
  EXPECT_EQ((*sentences)[1].req_id, 1u);

  TextBatch answers{{0, 0, "ans0"}, {1, 0, "ans1"}};
  harness.Publish("llm_answers", std::move(answers));

  EntityOutputFixture fix0, fix1;
  std::vector<CompanyOperatorEntityOutput> outputs = {fix0.out, fix1.out};
  ASSERT_EQ(harness.EncodeOperator(&outputs, fix0.Capacities()), 0);
  EXPECT_EQ(outputs[0].request_id, 1234u);
  EXPECT_EQ(outputs[1].request_id, 1234u);
}

TEST_F(AdapterPurityTest, InputBatchSkeleton_AllSamplesValidatedBeforePublish) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "translate.json.operator.v1");
  ASSERT_NE(in_conv, nullptr);

  test::AdapterHarness harness(in_conv);

  std::string valid_q = "{\"query\":\"valid\"}";
  std::string invalid_q = "invalid json";
  CompanyString cs_valid{static_cast<int32_t>(valid_q.size()), valid_q.data()};
  CompanyString cs_invalid{static_cast<int32_t>(invalid_q.size()),
                           invalid_q.data()};
  CompanyOperatorEntityInput in0{1, &cs_valid};
  CompanyOperatorEntityInput in1{2, &cs_invalid};

  EXPECT_EQ(harness.DecodeOperator({&in0, &in1}),
            COMPANY_ALG_ERR_INVALID_INPUT);

  // 请求 ID 表和业务值都不得发布。
  EXPECT_TRUE(harness.RequestIds().empty());
  EXPECT_EQ(harness.Context().Read<TextBatch>("input_sentences"), nullptr);
}

TEST_F(AdapterPurityTest, DocQaAdapter_MultiWayResultsReorderedAndPerturbed) {
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "doc_answer.plain.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(out_conv);

  harness.SetRequestIds(std::vector<uint64_t>{1001, 2002});

  // 打乱顺序：索引 1 先于索引 0 发布
  TextBatch answers{{1, 0, "Answer 1"}, {0, 0, "Answer 0"}};
  RuleMatchBatch intents{{0, 0, RuleMatchItem(1, "INTENT_0", "", 0.9f)},
                         {1, 0, RuleMatchItem(2, "INTENT_1", "", 0.8f)}};
  Int32Batch chunks{{0, 0, 3}, {1, 0, 5}};

  harness.Publish("llm_answers", std::move(answers));
  harness.Publish("intent_matches", std::move(intents));
  harness.Publish("doc_chunk_counts", std::move(chunks));

  DocOutputFixture fix0, fix1;
  std::vector<CompanyOperatorDocOutput> outputs = {fix0.out, fix1.out};
  ASSERT_EQ(harness.EncodeOperator(&outputs, fix0.Capacities()), 0);
  EXPECT_EQ(outputs[0].request_id, 1001u);
  ASSERT_NE(outputs[0].answer_text, nullptr);
  EXPECT_STREQ(outputs[0].answer_text->data, "Answer 0");
  ASSERT_NE(outputs[0].intent_name, nullptr);
  EXPECT_STREQ(outputs[0].intent_name->data, "INTENT_0");
  EXPECT_EQ(outputs[0].chunk_count, 3);

  EXPECT_EQ(outputs[1].request_id, 2002u);
  ASSERT_NE(outputs[1].answer_text, nullptr);
  EXPECT_STREQ(outputs[1].answer_text->data, "Answer 1");
  ASSERT_NE(outputs[1].intent_name, nullptr);
  EXPECT_STREQ(outputs[1].intent_name->data, "INTENT_1");
  EXPECT_EQ(outputs[1].chunk_count, 5);
}

// =========================================================================
// 3. 独立复用证明 (第 13.1 节)
// =========================================================================

// 证明 1：输入 Converter 与业务声明的宿主类型匹配，
// 并通过测试 binding 证明可复用
TEST_F(AdapterPurityTest, ReuseProof_1_InputConverterReusedAcrossBindings) {
  const auto* entity_binding =
      IoBindingRegistry::Instance().FindBinding("entity_extract.operator.v1");
  ASSERT_NE(entity_binding, nullptr);
  const auto* keyword_binding =
      IoBindingRegistry::Instance().FindBinding("keyword_match.operator.v1");
  ASSERT_NE(keyword_binding, nullptr);

  EXPECT_EQ(entity_binding->input_converter_id, "text.plain.operator.v1");
  EXPECT_EQ(keyword_binding->input_converter_id, "keyword.plain.operator.v1");

  const auto* entity_conv = IoConverterRegistry::Instance().FindInputConverter(
      "text.plain.operator.v1");
  ASSERT_NE(entity_conv, nullptr);
  EXPECT_EQ(ExternalType(entity_conv->external_slots),
            "CompanyOperatorEntityInput");

  const auto* keyword_conv = IoConverterRegistry::Instance().FindInputConverter(
      "keyword.plain.operator.v1");
  ASSERT_NE(keyword_conv, nullptr);
  EXPECT_EQ(ExternalType(keyword_conv->external_slots),
            "CompanyOperatorKeywordInput");

  // 用 entity binding 解码输入
  {
    test::AdapterHarness harness(entity_conv);
    std::string text_str = "entity sentence";
    CompanyString cs_text{static_cast<int32_t>(text_str.size()),
                          text_str.data()};
    CompanyOperatorEntityInput in{8001, &cs_text};
    EXPECT_EQ(harness.DecodeOperator({&in}), 0);
    const auto* sentences =
        harness.Context().Read<TextBatch>("input_sentences");
    ASSERT_NE(sentences, nullptr);
    EXPECT_EQ((*sentences)[0].data, "entity sentence");
  }

  // 用 keyword binding 解码输入
  {
    test::AdapterHarness harness(keyword_conv);
    std::string text_str = "keyword sentence";
    CompanyString cs_text{static_cast<int32_t>(text_str.size()),
                          text_str.data()};
    CompanyOperatorKeywordInput in{8002, &cs_text};
    EXPECT_EQ(harness.DecodeOperator({&in}), 0);
    const auto* sentences =
        harness.Context().Read<TextBatch>("input_sentences");
    ASSERT_NE(sentences, nullptr);
    EXPECT_EQ((*sentences)[0].data, "keyword sentence");
  }

  // 跨业务复用证明：在测试专用绑定中复用 text.plain.operator.v1
  {
    IoBindingDefinition test_reuse_binding;
    test_reuse_binding.binding_id = "test_purity_reuse.operator.v1";
    test_reuse_binding.biz_name = "entity_extract";

    test_reuse_binding.input_converter_id = "text.plain.operator.v1";
    test_reuse_binding.output_converter_id = "document.structured.operator.v1";
    test_reuse_binding.max_batch_size = 64;
    IoBindingRegistry::Instance().RegisterBinding(test_reuse_binding);

    const auto* b_test = IoBindingRegistry::Instance().FindBinding(
        "test_purity_reuse.operator.v1");
    ASSERT_NE(b_test, nullptr);
    EXPECT_EQ(b_test->input_converter_id, entity_binding->input_converter_id);
  }
}

// 证明 2：输出 Converter 可跨 Pipeline 复用
TEST_F(AdapterPurityTest, ReuseProof_2_OutputConverterReusedAcrossPipelines) {
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "document.structured.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  // 上下文 A：实体抽取 Pipeline 的输出
  {
    test::AdapterHarness harness(out_conv);
    harness.SetRequestIds(std::vector<uint64_t>{9001});
    StructuredDocumentBatch batch;
    batch.emplace_back(
        0, 0,
        JsonDocumentItem("[\"PERSON: Alice\"]", true, JsonParseStatus::kOk));
    harness.Publish("extracted_entities", std::move(batch));

    EntityOutputFixture ent_fix;
    std::vector<CompanyOperatorEntityOutput> outputs = {ent_fix.out};
    ASSERT_EQ(harness.EncodeOperator(&outputs, ent_fix.Capacities()), 0);
    EXPECT_EQ(outputs[0].request_id, 9001U);
    ASSERT_NE(outputs[0].entities_json, nullptr);
    EXPECT_STREQ(outputs[0].entities_json->data, "[\"PERSON: Alice\"]");
  }

  // 上下文 B：产出相同 schema 的通用结构化 JSON Pipeline 输出
  {
    test::AdapterHarness harness(out_conv);
    harness.SetRequestIds(std::vector<uint64_t>{9002});
    StructuredDocumentBatch batch;
    batch.emplace_back(
        0, 0,
        JsonDocumentItem("{\"summary\":\"ok\"}", true, JsonParseStatus::kOk));
    harness.Publish("extracted_entities", std::move(batch));

    EntityOutputFixture ent_fix;
    std::vector<CompanyOperatorEntityOutput> outputs = {ent_fix.out};
    ASSERT_EQ(harness.EncodeOperator(&outputs, ent_fix.Capacities()), 0);
    EXPECT_EQ(outputs[0].request_id, 9002U);
    ASSERT_NE(outputs[0].entities_json, nullptr);
    EXPECT_STREQ(outputs[0].entities_json->data, "{\"summary\":\"ok\"}");
  }
}

// 证明 3：多种外部输入格式驱动同一 Pipeline
TEST_F(AdapterPurityTest,
       ReuseProof_3_MultipleExternalInputFormatsForSamePipeline) {
  InputConverterDefinition custom_in_def;
  custom_in_def.converter_id = "test.multi_field.operator.v1";

  custom_in_def.schema_id = "multi_field.request";
  custom_in_def.external_slots = {
      ExternalSlotDefinition("inputs", "CustomMultiFieldInput",
                             PortDirection::kInput, true, "custom_input")};
  custom_in_def.max_batch_size = 64;
  custom_in_def.logical_ports = {OutputPort(kInputSentences)};
  custom_in_def.decode_fn = [](const ExternalInputBatchView& src,
                               const InputDecodeOptions& options,
                               AlgContext* ctx, AdapterStatus* status) -> int {
    std::vector<uint64_t> ids;
    TextBatch texts;
    for (size_t i = 0; i < src.count; ++i) {
      const auto* item = src.GetSlot<CustomMultiFieldInput>("inputs", i);
      if (!item) return -3;
      ids.push_back(item->req_id);
      std::string combined =
          std::string(item->topic) + ": " + std::string(item->content);
      texts.emplace_back(static_cast<uint32_t>(i), 0, combined);
    }
    if (!PublishRequestIds(options, std::move(ids), status)) {
      return status->Code();
    }
    ctx->Publish(kInputSentences, texts);
    return 0;
  };

  EXPECT_TRUE(
      IoConverterRegistry::Instance().RegisterInputConverter(custom_in_def));

  // 格式 A：经 text.plain.operator.v1 的 CompanyOperatorEntityInput
  AlgContext ctx_a;
  {
    const auto* in_a = IoConverterRegistry::Instance().FindInputConverter(
        "text.plain.operator.v1");
    ASSERT_NE(in_a, nullptr);
    std::string text_str = "AI: Revolution in robotics";
    CompanyString cs_text{static_cast<int32_t>(text_str.size()),
                          text_str.data()};
    CompanyOperatorEntityInput req_a{777, &cs_text};
    ExternalInputBatchView view;
    view.slots["entity_in"] = llm_edgeflow::BorrowInputForTest({&req_a});
    view.slot_types["entity_in"] = "CompanyOperatorEntityInput";
    view.count = 1;

    std::vector<uint64_t> request_ids;
    InputDecodeOptions opts;
    opts.request_ids = &request_ids;
    opts.converter_id = in_a->converter_id;

    AdapterStatus st;
    ASSERT_EQ(in_a->decode_fn(view, opts, &ctx_a, &st), 0);
  }

  // 格式 B：经 test.multi_field.operator.v1 的 CustomMultiFieldInput
  AlgContext ctx_b;
  {
    const auto* in_b = IoConverterRegistry::Instance().FindInputConverter(
        "test.multi_field.operator.v1");
    ASSERT_NE(in_b, nullptr);
    CustomMultiFieldInput req_b{777, "AI", "Revolution in robotics"};
    ExternalInputBatchView view;
    view.slots["inputs"] = llm_edgeflow::BorrowInputForTest({&req_b});
    view.slot_types["inputs"] = "CustomMultiFieldInput";
    view.count = 1;

    std::vector<uint64_t> request_ids;
    InputDecodeOptions opts;
    opts.request_ids = &request_ids;
    opts.converter_id = in_b->converter_id;

    AdapterStatus st;
    ASSERT_EQ(in_b->decode_fn(view, opts, &ctx_b, &st), 0);
  }

  const auto* texts_a = ctx_a.Read<TextBatch>("input_sentences");
  const auto* texts_b = ctx_b.Read<TextBatch>("input_sentences");
  ASSERT_NE(texts_a, nullptr);
  ASSERT_NE(texts_b, nullptr);
  EXPECT_EQ((*texts_a)[0].data, (*texts_b)[0].data);
  EXPECT_EQ((*texts_a)[0].data, "AI: Revolution in robotics");
}

// 证明 4：同一 Pipeline 可独立切换输出格式
TEST_F(AdapterPurityTest, ReuseProof_4_IndependentlySwitchOutputFormat) {
  // 输出 binding A：document.structured.operator.v1 ->
  // CompanyOperatorEntityOutput
  {
    const auto* out_a = IoConverterRegistry::Instance().FindOutputConverter(
        "document.structured.operator.v1");
    ASSERT_NE(out_a, nullptr);
    EntityOutputFixture fix;
    std::vector<CompanyOperatorEntityOutput> outputs = {fix.out};
    test::AdapterHarness harness(out_a);
    harness.SetRequestIds(std::vector<uint64_t>{5001});
    harness.Publish(
        "extracted_entities",
        StructuredDocumentBatch{
            {0, 0,
             JsonDocumentItem("[\"item_1\"]", true, JsonParseStatus::kOk)}});
    ASSERT_EQ(harness.EncodeOperator(&outputs, fix.Capacities()), 0);
    EXPECT_EQ(outputs[0].request_id, 5001U);
    ASSERT_NE(outputs[0].entities_json, nullptr);
    EXPECT_STREQ(outputs[0].entities_json->data, "[\"item_1\"]");
  }

  // 输出 binding B：keyword.result.operator.v1 ->
  // CompanyOperatorKeywordOutput
  {
    const auto* out_b = IoConverterRegistry::Instance().FindOutputConverter(
        "keyword.result.operator.v1");
    ASSERT_NE(out_b, nullptr);
    KeywordOutputFixture fix;
    std::vector<CompanyOperatorKeywordOutput> outputs = {fix.out};
    test::AdapterHarness harness(out_b);
    harness.SetRequestIds(std::vector<uint64_t>{5001});
    RuleMatchItem urgent(1, "URGENT", "急", 1.0f);
    urgent.slots["flag"] = "urgent";
    harness.Publish("rule_matches", RuleMatchBatch{{0, 0, urgent}});
    ASSERT_EQ(harness.EncodeOperator(&outputs, fix.Capacities()), 0);
    EXPECT_EQ(outputs[0].request_id, 5001U);
    EXPECT_EQ(outputs[0].is_hit, 1);
    ASSERT_NE(outputs[0].match_result_json, nullptr);
    EXPECT_STREQ(outputs[0].match_result_json->data,
                 "{\"confidence\":1.0,\"intent\":\"URGENT\",\"matched_word\":"
                 "\"急\",\"matches\":[],\"slots\":{\"flag\":\"urgent\"}}");
  }
}

// 证明 5：同一载体承载不同 schema
TEST_F(AdapterPurityTest, ReuseProof_5_SameCarrierDifferentSchema) {
  const auto* plain_conv = IoConverterRegistry::Instance().FindInputConverter(
      "text.plain.operator.v1");
  ASSERT_NE(plain_conv, nullptr);
  const auto* json_conv = IoConverterRegistry::Instance().FindInputConverter(
      "translate.json.operator.v1");
  ASSERT_NE(json_conv, nullptr);

  // Payload 1：纯文本 "Hello plain text"
  std::string plain_str = "Hello plain text";
  CompanyString cs_plain{static_cast<int32_t>(plain_str.size()),
                         plain_str.data()};
  CompanyOperatorEntityInput plain_req{101, &cs_plain};
  ExternalInputBatchView plain_view;
  plain_view.slots["entity_in"] =
      llm_edgeflow::BorrowInputForTest({&plain_req});
  plain_view.slot_types["entity_in"] = "CompanyOperatorEntityInput";
  plain_view.count = 1;

  std::vector<uint64_t> request_ids;
  InputDecodeOptions opts;
  opts.request_ids = &request_ids;

  // text.plain.operator.v1 将其作为纯文本接受
  {
    AlgContext ctx;
    AdapterStatus st;
    opts.converter_id = plain_conv->converter_id;
    EXPECT_EQ(plain_conv->decode_fn(plain_view, opts, &ctx, &st), 0);
    const auto* s = ctx.Read<TextBatch>("input_sentences");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ((*s)[0].data, "Hello plain text");
  }

  // translate.json.operator.v1 因其不是 JSON 而拒绝
  {
    AlgContext ctx;
    AdapterStatus st;
    opts.converter_id = json_conv->converter_id;
    EXPECT_EQ(json_conv->decode_fn(plain_view, opts, &ctx, &st),
              COMPANY_ALG_ERR_INVALID_INPUT);
    EXPECT_EQ(st.FieldPath(), "json");
  }

  // Payload 2：JSON 格式字符串 "{\"query\": \"Hello JSON\"}"
  std::string json_str = "{\"query\": \"Hello JSON\"}";
  CompanyString cs_json{static_cast<int32_t>(json_str.size()), json_str.data()};
  CompanyOperatorEntityInput json_req{102, &cs_json};
  ExternalInputBatchView json_view;
  json_view.slots["entity_in"] = llm_edgeflow::BorrowInputForTest({&json_req});
  json_view.slot_types["entity_in"] = "CompanyOperatorEntityInput";
  json_view.count = 1;

  // translate.json.operator.v1 解析成功并提取 "query"
  {
    AlgContext ctx;
    AdapterStatus st;
    opts.converter_id = json_conv->converter_id;
    EXPECT_EQ(json_conv->decode_fn(json_view, opts, &ctx, &st), 0);
    const auto* s = ctx.Read<TextBatch>("input_sentences");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ((*s)[0].data, "Hello JSON");
  }
}

// 证明 6：拒绝非法组合
TEST_F(AdapterPurityTest, ReuseProof_6_NegativeCombinations) {
  nlohmann::json valid_pipeline = nlohmann::json::array(
      {{{"id", "node_0_TextRuleMatchNode"},
        {"node_type", "TextRuleMatchNode"},
        {"depends_on", nlohmann::json::array()},
        {"inputs", {{"text", "input_sentences"}}},
        {"outputs", {{"matches", "rule_matches"}}},
        {"config",
         {{"categories",
           {{"SYSTEM_INIT", nlohmann::json::array({"init"})}}}}}}});

  // 1. 未知或未注册的 io_binding
  nlohmann::json bad_binding_json = {
      {"deployment",
       {{"io",
         {{"io_binding", "non_existent.binding.v999"},
          {"out_mem",
           {{"keyword_out",
             {{"meta_num", 0},
              {"metadata_type_id", 0},
              {"capacities", {{"match_result_json", 2047}}}}}}}}}}},
      {"models", nlohmann::json::array()},
      {"pipeline", valid_pipeline}};
  std::unique_ptr<ValidatedIoPlan> plan;
  std::string error;
  int ret = IoBindingResolver::ResolveFromPipelineJson(
      bad_binding_json, "./models", &plan, &error);
  EXPECT_EQ(ret, -2);
  EXPECT_NE(error.find("Unknown or unregistered io_binding"),
            std::string::npos);

  // 3. DeploymentIoConfig schema 校验拒绝非法/旧格式
  nlohmann::json invalid_version_json = {
      {"schema_version", 999},
      {"data",
       {{"pipe_path", "test.json"},
        {"io_binding", "keyword_match.operator.v1"}}}};
  DeploymentIoConfig parsed_cfg;
  EXPECT_FALSE(DeploymentIoConfig::Parse(invalid_version_json, ".", &parsed_cfg,
                                         &error));
  EXPECT_NE(error.find("Unknown field"), std::string::npos);

  // 4. 一致性检查拒绝含未知输出槽位的 Operator 配置
  nlohmann::json unknown_out_json = {
      {"deployment",
       {{"io",
         {{"io_binding", "keyword_match.operator.v1"},
          {"out_mem", {{"unknown_slot", nlohmann::json::object()}}}}}}},
      {"models", nlohmann::json::array()},
      {"pipeline", valid_pipeline}};
  ret = IoBindingResolver::ResolveFromPipelineJson(unknown_out_json, "./models",
                                                   &plan, &error);
  EXPECT_EQ(ret, -2);
  EXPECT_NE(error.find("Unknown configured output slot: unknown_slot"),
            std::string::npos);

  // 5. 已移除的部署字段 model_paths 作为未知字段被拒绝
  nlohmann::json unknown_mid_json = {
      {"deployment",
       {{"model_paths", {{"non_existent_model", "dummy_path"}}},
        {"io",
         {{"io_binding", "keyword_match.operator.v1"},
          {"out_mem",
           {{"keyword_out",
             {{"meta_num", 0},
              {"metadata_type_id", 0},
              {"capacities", {{"match_result_json", 2047}}}}}}}}}}},
      {"models", nlohmann::json::array()},
      {"pipeline", valid_pipeline}};
  ret = IoBindingResolver::ResolveFromPipelineJson(unknown_mid_json, "./models",
                                                   &plan, &error);
  EXPECT_EQ(ret, -2);
  EXPECT_NE(error.find("Unknown field at /deployment/model_paths"),
            std::string::npos)
      << "actual error was: " << error;
}

// 证明 7：初始化前校验 (binding 非法时不加载探测模型)
TEST_F(AdapterPurityTest, ReuseProof_7_ValidationBeforeInitialization) {
  operator_api::CreateParam param{};
  param.cfg_file_name = "non_existent_path.conf";
  param.model_path = "./models";
  param.device_id = 0;

  void* handle = nullptr;
  int ret =
      operator_api::Get_LLM_EDGEFLOW_OperatorTable().Create(&handle, &param);
  EXPECT_NE(ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(handle, nullptr);
}

}  // namespace llm_edgeflow
