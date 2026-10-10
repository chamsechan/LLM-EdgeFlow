#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_converter_registry.h"
#include "adapter/platform_value_binding.h"
#include "contracts/inference_payloads.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/pipeline_catalog.h"
#include "edgeflow/operator/types.h"
#include "platform_mock/operator_data_types.h"
#include "tests/support/adapter_harness.h"
#include "tests/support/adapter_test_views.h"

namespace llm_edgeflow {

class ComplexConvertersTest : public ::testing::Test {};

// ==================== DocQA ====================
TEST_F(ComplexConvertersTest, DocQaOperatorInputAndOutput) {
  const auto* in_conv =
      IoConverterRegistry::Instance().FindInputConverter("doc_in", "doc_qa");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv =
      IoConverterRegistry::Instance().FindOutputConverter("doc_out", "doc_qa");
  ASSERT_NE(out_conv, nullptr);

  std::string doc_text = "Sample document text";
  std::string query_text = "What is sample?";
  CompanyString cs_doc{static_cast<int32_t>(doc_text.size()),
                       const_cast<char*>(doc_text.data())};
  CompanyString cs_query{static_cast<int32_t>(query_text.size()),
                         const_cast<char*>(query_text.data())};

  CompanyOperatorDocInput doc_in{2001, kMockServiceDocQa, &cs_doc, &cs_query};
  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["doc_in"] = llm_edgeflow::BorrowInputForTest({&doc_in});
  in_view.slot_types["doc_in"] = "CompanyOperatorDocInput";

  const std::vector<uint64_t> request_ids{doc_in.request_id};
  test::ParsedInputOptions in_options(*in_conv);
  in_options.request_ids = &request_ids;

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
  char int_buf[64] = {0};
  CompanyString cs_int{63, int_buf};
  CompanyOperatorDocOutput doc_out{
      0, kMockServiceDocQa, nullptr, 0.0f, nullptr, 0, 0};
  doc_out.answer_text = &cs_ans;
  doc_out.intent_name = &cs_int;

  TestOutputBatchView out_view;
  out_view.count = 1;
  out_view.leased_slots["doc_out"] = {&doc_out};
  out_view.slot_types["doc_out"] = "CompanyOperatorDocOutput";
  out_view.SetCapacity("doc_out", "answer_text", 255);
  out_view.SetCapacity("doc_out", "intent_name", 63);

  test::ParsedOutputOptions out_options(*out_conv);
  out_options.request_ids = &request_ids;

  size_t written = 0;
  ret = ::llm_edgeflow::test::EncodeForTest(*out_conv, &ctx, out_options,
                                            &out_view, &written, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(doc_out.request_id, 2001U);
  EXPECT_EQ(doc_out.chunk_count, 1);
  ASSERT_NE(doc_out.intent_name, nullptr);
  EXPECT_STREQ(doc_out.intent_name->data, "general_faq");
  EXPECT_FLOAT_EQ(doc_out.confidence, 0.95f);
  ASSERT_NE(doc_out.answer_text, nullptr);
  EXPECT_STREQ(doc_out.answer_text->data, "This is the answer.");
}

TEST_F(ComplexConvertersTest,
       DocQaOutputPreservesEmbeddedNullAndRejectsSmallPool) {
  const auto* converter =
      IoConverterRegistry::Instance().FindOutputConverter("doc_out", "doc_qa");
  ASSERT_NE(converter, nullptr);
  const std::string answer("a\0b", 3);
  AlgContext ctx;
  std::vector<uint64_t> request_ids{2001};
  ctx.Publish("answer_text", TextBatch{{0, 0, answer}});
  ctx.Publish("intent", RuleMatchBatch{{0, 0, RuleMatchItem{}}});
  ctx.Publish("chunk_count", Int32Batch{{0, 0, 1}});
  test::ParsedOutputOptions options(*converter);
  options.request_ids = &request_ids;

  char bytes[4] = {};
  char intent_bytes[1] = {};
  CompanyString answer_out{0, bytes};
  CompanyString intent_out{0, intent_bytes};
  CompanyOperatorDocOutput output{
      0, kMockServiceDocQa, nullptr, 0.0f, nullptr, 0, 0};
  output.answer_text = &answer_out;
  output.intent_name = &intent_out;
  TestOutputBatchView view;
  view.count = 1;
  view.leased_slots["doc_out"] = {&output};
  view.slot_types["doc_out"] = "CompanyOperatorDocOutput";
  view.SetCapacity("doc_out", "answer_text", 3);
  view.SetCapacity("doc_out", "intent_name", 0);
  size_t written = 0;
  AdapterStatus status;
  ASSERT_EQ(::llm_edgeflow::test::EncodeForTest(*converter, &ctx, options,
                                                &view, &written, &status),
            COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 1U);
  ASSERT_EQ(answer_out.length, 3);
  EXPECT_EQ(std::string(answer_out.data, answer_out.length), answer);
  EXPECT_EQ(bytes[3], '\0');

  view.SetCapacity("doc_out", "answer_text", 1);
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
      "rerank_out", "cross_rerank");
  ASSERT_NE(out_conv, nullptr);

  std::string query_text = "how to rerank?";
  std::string passage_text = "Reranking is a scoring step.";
  CompanyString cs_q{static_cast<int32_t>(query_text.size()),
                     const_cast<char*>(query_text.data())};
  CompanyString cs_p{static_cast<int32_t>(passage_text.size()),
                     const_cast<char*>(passage_text.data())};

  CompanyOperatorRerankInput rerank_in{
      0, kMockServiceCrossRerank, nullptr, {}, 0};
  rerank_in.request_id = 3001;
  rerank_in.query_text = &cs_q;
  rerank_in.candidate_passages[0] = &cs_p;
  rerank_in.candidate_count = 1;

  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["rerank_in"] = llm_edgeflow::BorrowInputForTest({&rerank_in});
  in_view.slot_types["rerank_in"] = "CompanyOperatorRerankInput";

  const std::vector<uint64_t> request_ids{rerank_in.request_id};
  test::ParsedInputOptions in_options(*in_conv);
  in_options.request_ids = &request_ids;

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

  CompanyOperatorRerankOutput rerank_out{0, kMockServiceCrossRerank, {}, {}, 0,
                                         0};
  TestOutputBatchView out_view;
  out_view.count = 1;
  out_view.leased_slots["rerank_out"] = {&rerank_out};
  out_view.slot_types["rerank_out"] = "CompanyOperatorRerankOutput";
  ResolvedOutputPoolSpec output_spec;
  out_view.pool_specs["rerank_out"] = &output_spec;

  test::ParsedOutputOptions out_options(*out_conv);
  out_options.request_ids = &request_ids;

  size_t written = 0;
  ret = ::llm_edgeflow::test::EncodeForTest(*out_conv, &ctx, out_options,
                                            &out_view, &written, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(rerank_out.request_id, 3001U);
  EXPECT_EQ(rerank_out.count, 1);
  EXPECT_FLOAT_EQ(rerank_out.scores[0], 0.98f);
  EXPECT_EQ(rerank_out.sorted_indices[0], 0);
}

// ==================== DialogueAudit ====================
TEST_F(ComplexConvertersTest, DialogueAuditOperatorInputAndOutput) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "audit_in", "dialogue_audit");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "audit_out", "dialogue_audit");
  ASSERT_NE(out_conv, nullptr);

  std::string dialogue = "User text violating rules";
  std::string channel = "customer_service";
  CompanyString cs_dia{static_cast<int32_t>(dialogue.size()),
                       const_cast<char*>(dialogue.data())};
  CompanyString cs_chan{static_cast<int32_t>(channel.size()),
                        const_cast<char*>(channel.data())};

  CompanyOperatorAuditInput audit_in{4001, kMockServiceDialogueAudit, &cs_dia,
                                     &cs_chan};
  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["audit_in"] = llm_edgeflow::BorrowInputForTest({&audit_in});
  in_view.slot_types["audit_in"] = "CompanyOperatorAuditInput";

  const std::vector<uint64_t> request_ids{audit_in.request_id};
  test::ParsedInputOptions in_options(*in_conv);
  in_options.request_ids = &request_ids;

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

  char risk_buf[32] = {0};
  CompanyString cs_risk{31, risk_buf};
  char clause_buf[256] = {0};
  CompanyString cs_clause{255, clause_buf};
  char verdict_buf[1024] = {0};
  CompanyString cs_verdict{1023, verdict_buf};

  CompanyOperatorAuditOutput audit_out{
      0, kMockServiceDialogueAudit, nullptr, 0.0f, nullptr, nullptr, 0};
  audit_out.risk_level = &cs_risk;
  audit_out.matched_policy_clause = &cs_clause;
  audit_out.audit_verdict_json = &cs_verdict;

  TestOutputBatchView out_view;
  out_view.count = 1;
  out_view.leased_slots["audit_out"] = {&audit_out};
  out_view.slot_types["audit_out"] = "CompanyOperatorAuditOutput";
  out_view.SetCapacity("audit_out", "risk_level", 31);
  out_view.SetCapacity("audit_out", "matched_policy_clause", 255);
  out_view.SetCapacity("audit_out", "audit_verdict_json", 1023);

  test::ParsedOutputOptions out_options(*out_conv);
  out_options.request_ids = &request_ids;

  size_t written = 0;
  ret = ::llm_edgeflow::test::EncodeForTest(*out_conv, &ctx, out_options,
                                            &out_view, &written, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(audit_out.request_id, 4001U);
  ASSERT_NE(audit_out.risk_level, nullptr);
  EXPECT_STREQ(audit_out.risk_level->data, "HIGH_RISK");
  EXPECT_FLOAT_EQ(audit_out.risk_score, 0.88f);
  ASSERT_NE(audit_out.matched_policy_clause, nullptr);
  EXPECT_STREQ(audit_out.matched_policy_clause->data, "Rule 12.3");
  ASSERT_NE(audit_out.audit_verdict_json, nullptr);
  EXPECT_STREQ(audit_out.audit_verdict_json->data, "{\"risk\":\"high\"}");
}

// ==================== AudioAsrIntent ====================
TEST_F(ComplexConvertersTest, AudioAsrIntentOperatorInputAndOutput) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "audio_in", "audio_asr_intent");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "audio_out", "audio_asr_intent");
  ASSERT_NE(out_conv, nullptr);

  std::vector<float> pcm = {0.1f, 0.2f, -0.1f};
  CompanyOperatorAudioInput audio_in{5001, kMockServiceAudioAsrIntent,
                                     pcm.data(),
                                     static_cast<int32_t>(pcm.size()), 16000};
  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["audio_in"] = llm_edgeflow::BorrowInputForTest({&audio_in});
  in_view.slot_types["audio_in"] = "CompanyOperatorAudioInput";

  const std::vector<uint64_t> request_ids{audio_in.request_id};
  test::ParsedInputOptions in_options(*in_conv);
  in_options.request_ids = &request_ids;

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

  char trans_buf[512] = {0};
  CompanyString cs_trans{511, trans_buf};
  char slot_buf[1024] = {0};
  CompanyString cs_slot{1023, slot_buf};

  CompanyOperatorAudioOutput audio_out{0, kMockServiceAudioAsrIntent, nullptr,
                                       nullptr, 0};
  audio_out.transcribed_text = &cs_trans;
  audio_out.intent_slot_json = &cs_slot;

  TestOutputBatchView out_view;
  out_view.count = 1;
  out_view.leased_slots["audio_out"] = {&audio_out};
  out_view.slot_types["audio_out"] = "CompanyOperatorAudioOutput";
  out_view.SetCapacity("audio_out", "transcribed_text", 511);
  out_view.SetCapacity("audio_out", "intent_slot_json", 1023);

  test::ParsedOutputOptions out_options(*out_conv);
  out_options.request_ids = &request_ids;

  size_t written = 0;
  ret = ::llm_edgeflow::test::EncodeForTest(*out_conv, &ctx, out_options,
                                            &out_view, &written, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(audio_out.request_id, 5001U);
  ASSERT_NE(audio_out.transcribed_text, nullptr);
  EXPECT_STREQ(audio_out.transcribed_text->data, "open the front door");
  ASSERT_NE(audio_out.intent_slot_json, nullptr);
  EXPECT_STREQ(audio_out.intent_slot_json->data,
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
      "od_out", "ocr_invoice_qa");
  ASSERT_NE(out_conv, nullptr);

  // 准备 Operator 输入：frame 和 string
  std::vector<uint8_t> pixels{255, 0, 0, 0, 255, 0};
  CompanyFrame frame{6001,   kMockServiceOcrInvoiceQa, 1, 2, 6, pixels.data(),
                     nullptr};

  std::string q_str = "Total amount?";
  CompanyString query{static_cast<int32_t>(q_str.size()),
                      const_cast<char*>(q_str.data())};

  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["frame"] = llm_edgeflow::BorrowInputForTest({&frame});
  in_view.slots["string"] = llm_edgeflow::BorrowInputForTest({&query});
  in_view.slot_types["frame"] = "CompanyFrame";
  in_view.slot_types["string"] = "CompanyString";

  const std::vector<uint64_t> request_ids{frame.request_id};
  test::ParsedInputOptions in_options(*in_conv);
  in_options.request_ids = &request_ids;

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
  query_options.request_ids = &request_ids;
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

  // 目标 Operator od_out
  CompanyOdOutput od_out{0, kMockServiceOcrInvoiceQa, 0, nullptr, nullptr, 0};
  std::vector<char> buf(256);
  CompanyString res_str{255, buf.data()};
  od_out.result_json = &res_str;

  TestOutputBatchView out_view;
  out_view.leased_slots["od_out"] = {&od_out};
  out_view.slot_types["od_out"] = "CompanyOdOutput";
  out_view.SetCapacity("od_out", "result_json", 255);
  out_view.count = 1;

  test::ParsedOutputOptions out_options(*out_conv);
  out_options.request_ids = &request_ids;

  size_t written = 0;
  ret = ::llm_edgeflow::test::EncodeForTest(*out_conv, &ctx, out_options,
                                            &out_view, &written, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(od_out.request_id, 6001U);
  EXPECT_EQ(od_out.detected_box_count, 1);
  ASSERT_NE(od_out.result_json, nullptr);
  EXPECT_STREQ(od_out.result_json->data, "{\"total\":123.45}");
}

}  // namespace llm_edgeflow
