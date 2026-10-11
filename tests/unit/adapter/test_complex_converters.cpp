#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_converter_registry.h"
#include "adapter/operator/mock/platform_value_binding.h"
#include "contracts/inference_payloads.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/pipeline_catalog.h"
#include "edgeflow/operator/types.h"
#include "platform_mock/operator_data_types.h"
#include "tests/support/adapter_harness.h"
#include "tests/support/adapter_test_views.h"

namespace llm_edgeflow {

namespace {
nlohmann::json OutputJson(const CompanyOperatorOutput& value) {
  return nlohmann::json::parse(
      std::string(value.result_json->data, value.result_json->length));
}
}  // namespace

class ComplexConvertersTest : public ::testing::Test {};

// ==================== DocQA ====================
TEST_F(ComplexConvertersTest, DocQaOperatorInputAndOutput) {
  const auto* in_conv =
      IoConverterRegistry::Instance().FindInputConverter("doc_in", "doc_qa");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv =
      IoConverterRegistry::Instance().FindOutputConverter("json_out", "doc_qa");
  ASSERT_NE(out_conv, nullptr);

  std::string doc_text = "Sample document text";
  std::string query_text = "What is sample?";
  CompanyString cs_doc{static_cast<int32_t>(doc_text.size()),
                       const_cast<char*>(doc_text.data())};
  CompanyString cs_query{static_cast<int32_t>(query_text.size()),
                         const_cast<char*>(query_text.data())};

  CompanyOperatorDocInput doc_in{kMockServiceDocQa, &cs_doc, &cs_query};
  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["doc_in"] = llm_edgeflow::BorrowInputForTest({&doc_in});
  in_view.slot_types["doc_in"] = "CompanyOperatorDocInput";

  test::ParsedInputOptions in_options(*in_conv);

  AlgContext ctx;
  AdapterStatus status;
  int ret = ::llm_edgeflow::test::DecodeForTest(*in_conv, in_view, in_options,
                                                &ctx, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);

  // 填充答案上下文
  TextBatch answers;
  answers.emplace_back(0, 0, "This is the answer.");
  ctx.Publish("answer_text", std::move(answers));

  RuleMatchBatch intents;
  RuleMatchItem match;
  match.category = "general_faq";
  match.score = 0.95f;
  match.status_code = 0;
  intents.emplace_back(0, 0, match);
  ctx.Publish("intent", std::move(intents));

  Int32Batch chunk_counts;
  chunk_counts.emplace_back(0, 0, 1);
  ctx.Publish("chunk_count", std::move(chunk_counts));

  char ans_buf[256] = {0};
  CompanyString cs_ans{255, ans_buf};
  CompanyOperatorOutput json_out{kMockServiceDocQa, nullptr, 0};
  json_out.result_json = &cs_ans;

  TestOutputBatchView out_view;
  out_view.count = 1;
  out_view.leased_slots["json_out"] = {&json_out};
  out_view.slot_types["json_out"] = "CompanyOperatorOutput";
  out_view.SetCapacity("json_out", "result_json", 255);

  test::ParsedOutputOptions out_options(*out_conv);

  size_t written = 0;
  ret = ::llm_edgeflow::test::EncodeForTest(*out_conv, &ctx, out_options,
                                            &out_view, &written, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(OutputJson(json_out).at("chunk_count").get<int>(), 1);
  ASSERT_NE(json_out.result_json, nullptr);
  EXPECT_STREQ(
      OutputJson(json_out).at("intent_name").get<std::string>().c_str(),
      "general_faq");
  EXPECT_FLOAT_EQ(OutputJson(json_out).at("confidence").get<float>(), 0.95f);
  ASSERT_NE(json_out.result_json, nullptr);
  EXPECT_STREQ(
      OutputJson(json_out).at("answer_text").get<std::string>().c_str(),
      "This is the answer.");
}

TEST_F(ComplexConvertersTest,
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

// ==================== CrossRerank ====================
TEST_F(ComplexConvertersTest, CrossRerankOperatorInputAndOutput) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "rerank_in", "cross_rerank");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "json_out", "cross_rerank");
  ASSERT_NE(out_conv, nullptr);

  std::string query_text = "how to rerank?";
  std::string passage_text = "Reranking is a scoring step.";
  CompanyString cs_q{static_cast<int32_t>(query_text.size()),
                     const_cast<char*>(query_text.data())};
  CompanyString cs_p{static_cast<int32_t>(passage_text.size()),
                     const_cast<char*>(passage_text.data())};

  CompanyOperatorRerankInput rerank_in{kMockServiceCrossRerank, nullptr, {}, 0};
  rerank_in.query_text = &cs_q;
  rerank_in.candidate_passages[0] = &cs_p;
  rerank_in.candidate_count = 1;

  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["rerank_in"] = llm_edgeflow::BorrowInputForTest({&rerank_in});
  in_view.slot_types["rerank_in"] = "CompanyOperatorRerankInput";

  test::ParsedInputOptions in_options(*in_conv);

  AlgContext ctx;
  AdapterStatus status;
  int ret = ::llm_edgeflow::test::DecodeForTest(*in_conv, in_view, in_options,
                                                &ctx, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);

  // 在上下文中准备 ranked_results
  RankedTextBatch ranked;
  RankedCandidate cand;
  cand.rank = 1;
  cand.score = 0.98f;
  cand.original_sub_id = 0;
  cand.text = "Reranking is a scoring step.";
  ranked.emplace_back(0, 0, cand);
  ctx.Publish("ranked", std::move(ranked));

  char bytes[256] = {};
  CompanyString result{0, bytes};
  CompanyOperatorOutput json_out{kMockServiceCrossRerank, &result, 0};
  TestOutputBatchView out_view;
  out_view.count = 1;
  out_view.leased_slots["json_out"] = {&json_out};
  out_view.slot_types["json_out"] = "CompanyOperatorOutput";
  out_view.SetCapacity("json_out", "result_json", 255);

  test::ParsedOutputOptions out_options(*out_conv);

  size_t written = 0;
  ret = ::llm_edgeflow::test::EncodeForTest(*out_conv, &ctx, out_options,
                                            &out_view, &written, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(OutputJson(json_out).at("count").get<int>(), 1);
  EXPECT_FLOAT_EQ(OutputJson(json_out).at("scores").at(0).get<float>(), 0.98f);
  EXPECT_EQ(OutputJson(json_out).at("sorted_indices").at(0).get<int>(), 0);
}

// ==================== DialogueAudit ====================
TEST_F(ComplexConvertersTest, DialogueAuditOperatorInputAndOutput) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "audit_in", "dialogue_audit");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "json_out", "dialogue_audit");
  ASSERT_NE(out_conv, nullptr);

  std::string dialogue = "User text violating rules";
  std::string channel = "customer_service";
  CompanyString cs_dia{static_cast<int32_t>(dialogue.size()),
                       const_cast<char*>(dialogue.data())};
  CompanyString cs_chan{static_cast<int32_t>(channel.size()),
                        const_cast<char*>(channel.data())};

  CompanyOperatorAuditInput audit_in{kMockServiceDialogueAudit, &cs_dia,
                                     &cs_chan};
  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["audit_in"] = llm_edgeflow::BorrowInputForTest({&audit_in});
  in_view.slot_types["audit_in"] = "CompanyOperatorAuditInput";

  test::ParsedInputOptions in_options(*in_conv);

  AlgContext ctx;
  AdapterStatus status;
  int ret = ::llm_edgeflow::test::DecodeForTest(*in_conv, in_view, in_options,
                                                &ctx, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);

  // 在上下文中准备审核结果
  StructuredDocumentBatch verdicts;
  verdicts.emplace_back(
      0, 0,
      JsonDocumentItem("{\"risk\":\"high\"}", true, JsonParseStatus::kOk, "",
                       {{"risk_level", "HIGH_RISK"}, {"risk_score", 0.88f}}));
  ctx.Publish("verdict", std::move(verdicts));

  RankedTextBatch policies;
  policies.emplace_back(0, 0, RankedCandidate("Rule 12.3", 0.88f, 1, 0));
  ctx.Publish("matched_policy", std::move(policies));

  char verdict_buf[1024] = {0};
  CompanyString cs_verdict{1023, verdict_buf};

  CompanyOperatorOutput json_out{kMockServiceDialogueAudit, nullptr, 0};

  json_out.result_json = &cs_verdict;

  TestOutputBatchView out_view;
  out_view.count = 1;
  out_view.leased_slots["json_out"] = {&json_out};
  out_view.slot_types["json_out"] = "CompanyOperatorOutput";

  out_view.SetCapacity("json_out", "result_json", 1023);

  test::ParsedOutputOptions out_options(*out_conv);

  size_t written = 0;
  ret = ::llm_edgeflow::test::EncodeForTest(*out_conv, &ctx, out_options,
                                            &out_view, &written, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 1U);
  ASSERT_NE(json_out.result_json, nullptr);
  EXPECT_STREQ(OutputJson(json_out).at("risk_level").get<std::string>().c_str(),
               "HIGH_RISK");
  EXPECT_FLOAT_EQ(OutputJson(json_out).at("risk_score").get<float>(), 0.88f);
  ASSERT_NE(json_out.result_json, nullptr);
  EXPECT_STREQ(OutputJson(json_out)
                   .at("matched_policy_clause")
                   .get<std::string>()
                   .c_str(),
               "Rule 12.3");
  ASSERT_NE(json_out.result_json, nullptr);
  EXPECT_STREQ(OutputJson(json_out).at("audit_verdict").dump().c_str(),
               "{\"risk\":\"high\"}");
}

// ==================== AudioAsrIntent ====================
TEST_F(ComplexConvertersTest, AudioAsrIntentOperatorInputAndOutput) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "audio_in", "audio_asr_intent");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "json_out", "audio_asr_intent");
  ASSERT_NE(out_conv, nullptr);

  std::vector<float> pcm = {0.1f, 0.2f, -0.1f};
  CompanyOperatorAudioInput audio_in{kMockServiceAudioAsrIntent, pcm.data(),
                                     static_cast<int32_t>(pcm.size()), 16000};
  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["audio_in"] = llm_edgeflow::BorrowInputForTest({&audio_in});
  in_view.slot_types["audio_in"] = "CompanyOperatorAudioInput";

  test::ParsedInputOptions in_options(*in_conv);

  AlgContext ctx;
  AdapterStatus status;
  int ret = ::llm_edgeflow::test::DecodeForTest(*in_conv, in_view, in_options,
                                                &ctx, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);

  TextBatch transcripts;
  transcripts.emplace_back(0, 0, "open the front door");
  ctx.Publish("transcribed_text", std::move(transcripts));

  RuleMatchBatch slots;
  RuleMatchItem m(1, "open_door", "open", 1.0f, "r1");
  m.matches.push_back(
      {RuleMatchSource::kRule, "open_door", "r1", "open", 0.5f});
  m.slots["target"] = "front";
  slots.emplace_back(0, 0, m);
  ctx.Publish("intent_slot", std::move(slots));

  char slot_buf[1024] = {0};
  CompanyString cs_slot{1023, slot_buf};

  CompanyOperatorOutput json_out{kMockServiceAudioAsrIntent, nullptr, 0};

  json_out.result_json = &cs_slot;

  TestOutputBatchView out_view;
  out_view.count = 1;
  out_view.leased_slots["json_out"] = {&json_out};
  out_view.slot_types["json_out"] = "CompanyOperatorOutput";

  out_view.SetCapacity("json_out", "result_json", 1023);

  test::ParsedOutputOptions out_options(*out_conv);

  size_t written = 0;
  ret = ::llm_edgeflow::test::EncodeForTest(*out_conv, &ctx, out_options,
                                            &out_view, &written, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 1U);
  ASSERT_NE(json_out.result_json, nullptr);
  EXPECT_STREQ(
      OutputJson(json_out).at("transcribed_text").get<std::string>().c_str(),
      "open the front door");
  ASSERT_NE(json_out.result_json, nullptr);
  EXPECT_STREQ(OutputJson(json_out).at("intent_slot").dump().c_str(),
               "{\"confidence\":1.0,\"intent\":\"open_door\","
               "\"matched_word\":\"open\",\"matches\":[{\"category\":"
               "\"open_door\",\"pattern\":\"open\",\"rule_id\":\"r1\","
               "\"score\":0.5}],\"slots\":{\"target\":\"front\"}}");
}

// ==================== OcrInvoiceQa ====================
TEST_F(ComplexConvertersTest, OcrInvoiceQaOperatorInputAndOutput) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "frame", "ocr_invoice_qa");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "json_out", "ocr_invoice_qa");
  ASSERT_NE(out_conv, nullptr);

  // 准备 Operator 输入：frame 和 string
  std::vector<uint8_t> pixels{255, 0, 0, 0, 255, 0};
  CompanyFrame frame{kMockServiceOcrInvoiceQa, 1, 2, 6, pixels.data(), nullptr};

  std::string q_str = "Total amount?";
  CompanyString query{static_cast<int32_t>(q_str.size()),
                      const_cast<char*>(q_str.data())};

  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["frame"] = llm_edgeflow::BorrowInputForTest({&frame});
  in_view.slots["string"] = llm_edgeflow::BorrowInputForTest({&query});
  in_view.slot_types["frame"] = "CompanyFrame";
  in_view.slot_types["string"] = "CompanyString";

  test::ParsedInputOptions in_options(*in_conv);

  AlgContext ctx;
  AdapterStatus status;
  int ret = ::llm_edgeflow::test::DecodeForTest(*in_conv, in_view, in_options,
                                                &ctx, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);

  // 在 AlgContext 中准备发票输出

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
  JsonDocumentItem doc_item;
  doc_item.is_valid = true;
  doc_item.parse_status = JsonParseStatus::kOk;
  doc_item.json_payload = "{\"total\":123.45}";
  invoices.emplace_back(0, 0, doc_item);
  ctx.Publish("result", std::move(invoices));

  OcrDocumentBatch ocr_docs;
  OcrDocumentItem ocr_doc;
  ocr_doc.boxes.push_back({0, 0, 10, 10, "Invoice", 0.99f});
  ocr_docs.emplace_back(0, 0, ocr_doc);
  ctx.Publish("document", std::move(ocr_docs));

  // 目标 Operator json_out
  CompanyOperatorOutput json_out{kMockServiceOcrInvoiceQa, nullptr, 0};
  std::vector<char> buf(256);
  CompanyString res_str{255, buf.data()};
  json_out.result_json = &res_str;

  TestOutputBatchView out_view;
  out_view.leased_slots["json_out"] = {&json_out};
  out_view.slot_types["json_out"] = "CompanyOperatorOutput";
  out_view.SetCapacity("json_out", "result_json", 255);
  out_view.count = 1;

  test::ParsedOutputOptions out_options(*out_conv);

  size_t written = 0;
  ret = ::llm_edgeflow::test::EncodeForTest(*out_conv, &ctx, out_options,
                                            &out_view, &written, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(OutputJson(json_out).at("detected_box_count").get<int>(), 1);
  ASSERT_NE(json_out.result_json, nullptr);
  EXPECT_EQ(OutputJson(json_out).at("result"),
            nlohmann::json({{"total", 123.45}}));
}

}  // namespace llm_edgeflow
