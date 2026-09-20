#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_binding_registry.h"
#include "adapter/io_converter.h"
#include "adapter/io_converter_registry.h"
#include "contracts/inference_payloads.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/pipeline_catalog.h"
#include "edgeflow/operator/types.h"
#include "platform_mock/operator_data_types.h"
#include "tests/support/adapter_test_views.h"

namespace llm_edgeflow {

class ComplexConvertersTest : public ::testing::Test {};

// ==================== DocQA ====================
TEST_F(ComplexConvertersTest, DocQaOperatorInputAndOutput) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "doc_query.plain.operator.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "doc_answer.plain.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  std::string doc_text = "Sample document text";
  std::string query_text = "What is sample?";
  CompanyString cs_doc{static_cast<int32_t>(doc_text.size()),
                       const_cast<char*>(doc_text.data())};
  CompanyString cs_query{static_cast<int32_t>(query_text.size()),
                         const_cast<char*>(query_text.data())};

  CompanyOperatorDocInput doc_in{2001, &cs_doc, &cs_query};
  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["doc_in"] = llm_edgeflow::BorrowInputForTest({&doc_in});
  in_view.slot_types["doc_in"] = "CompanyOperatorDocInput";

  InputPortBindings in_bindings({{"raw_request_ids", "raw_request_ids"},
                                 {"raw_docs", "raw_docs"},
                                 {"raw_queries", "raw_queries"}});
  InputDecodeOptions in_options;
  in_options.converter_id = in_conv->converter_id;

  AlgContext ctx;
  AdapterStatus status;
  int ret = in_conv->decode_fn(in_view, in_options, in_bindings, &ctx, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);

  // Populate answer context
  TextBatch answers;
  answers.emplace_back(0, 0, "This is the answer.");
  ctx.Publish("llm_answers", std::move(answers));

  RuleMatchBatch intents;
  RuleMatchItem match;
  match.category = "general_faq";
  match.score = 0.95f;
  match.status_code = 0;
  intents.emplace_back(0, 0, match);
  ctx.Publish("intent_matches", std::move(intents));

  Int32Batch chunk_counts;
  chunk_counts.emplace_back(0, 0, 1);
  ctx.Publish("doc_chunk_counts", std::move(chunk_counts));

  char ans_buf[256] = {0};
  CompanyString cs_ans{255, ans_buf};
  char int_buf[64] = {0};
  CompanyString cs_int{63, int_buf};
  CompanyOperatorDocOutput doc_out{};
  doc_out.answer_text = &cs_ans;
  doc_out.intent_name = &cs_int;

  TestOutputBatchView out_view;
  out_view.count = 1;
  out_view.leased_slots["doc_out"] = {&doc_out};
  out_view.slot_types["doc_out"] = "CompanyOperatorDocOutput";
  out_view.SetCapacity("doc_out", "answer_text", 255);
  out_view.SetCapacity("doc_out", "intent_name", 63);

  OutputPortBindings out_bindings({{"raw_request_ids", "raw_request_ids"},
                                   {"llm_answers", "llm_answers"},
                                   {"intent_matches", "intent_matches"},
                                   {"doc_chunk_counts", "doc_chunk_counts"}});
  OutputEncodeOptions out_options;
  out_options.converter_id = out_conv->converter_id;

  size_t written = 0;
  ret = out_conv->encode_fn(&ctx, out_bindings, out_options, &out_view,
                            &written, &status);
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

// ==================== CrossRerank ====================
TEST_F(ComplexConvertersTest, CrossRerankOperatorInputAndOutput) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "rerank.plain.operator.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "rerank_result.plain.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  std::string query_text = "how to rerank?";
  std::string passage_text = "Reranking is a scoring step.";
  CompanyString cs_q{static_cast<int32_t>(query_text.size()),
                     const_cast<char*>(query_text.data())};
  CompanyString cs_p{static_cast<int32_t>(passage_text.size()),
                     const_cast<char*>(passage_text.data())};

  CompanyOperatorRerankInput rerank_in{};
  rerank_in.request_id = 3001;
  rerank_in.query_text = &cs_q;
  rerank_in.candidate_passages[0] = &cs_p;
  rerank_in.candidate_count = 1;

  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["rerank_in"] = llm_edgeflow::BorrowInputForTest({&rerank_in});
  in_view.slot_types["rerank_in"] = "CompanyOperatorRerankInput";

  InputPortBindings in_bindings({{"raw_request_ids", "raw_request_ids"},
                                 {"rerank_queries", "rerank_queries"},
                                 {"rerank_candidates", "rerank_candidates"},
                                 {"rerank_pairs", "rerank_pairs"}});
  InputDecodeOptions in_options;
  in_options.converter_id = in_conv->converter_id;

  AlgContext ctx;
  AdapterStatus status;
  int ret = in_conv->decode_fn(in_view, in_options, in_bindings, &ctx, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);

  // Setup ranked_results in context
  RankedTextBatch ranked;
  RankedCandidate cand;
  cand.rank = 1;
  cand.score = 0.98f;
  cand.original_sub_id = 0;
  cand.text = "Reranking is a scoring step.";
  ranked.emplace_back(0, 0, cand);
  ctx.Publish("ranked_results", std::move(ranked));

  CompanyOperatorRerankOutput rerank_out{};
  TestOutputBatchView out_view;
  out_view.count = 1;
  out_view.leased_slots["rerank_out"] = {&rerank_out};
  out_view.slot_types["rerank_out"] = "CompanyOperatorRerankOutput";

  OutputPortBindings out_bindings({{"raw_request_ids", "raw_request_ids"},
                                   {"ranked_results", "ranked_results"}});
  OutputEncodeOptions out_options;
  out_options.converter_id = out_conv->converter_id;

  size_t written = 0;
  ret = out_conv->encode_fn(&ctx, out_bindings, out_options, &out_view,
                            &written, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(rerank_out.request_id, 3001U);
  EXPECT_EQ(rerank_out.count, 1);
  EXPECT_FLOAT_EQ(rerank_out.scores[0], 0.98f);
  EXPECT_EQ(rerank_out.sorted_indices[0], 0);
}

// ==================== ComplianceAudit ====================
TEST_F(ComplexConvertersTest, DialogueAuditOperatorInputAndOutput) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "audit.plain.operator.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "audit_result.plain.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  std::string dialogue = "User text violating rules";
  std::string channel = "customer_service";
  CompanyString cs_dia{static_cast<int32_t>(dialogue.size()),
                       const_cast<char*>(dialogue.data())};
  CompanyString cs_chan{static_cast<int32_t>(channel.size()),
                        const_cast<char*>(channel.data())};

  CompanyOperatorAuditInput audit_in{4001, &cs_dia, &cs_chan};
  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["audit_in"] = llm_edgeflow::BorrowInputForTest({&audit_in});
  in_view.slot_types["audit_in"] = "CompanyOperatorAuditInput";

  InputPortBindings in_bindings({{"raw_request_ids", "raw_request_ids"},
                                 {"user_texts", "user_texts"},
                                 {"channel_names", "channel_names"}});
  InputDecodeOptions in_options;
  in_options.converter_id = in_conv->converter_id;

  AlgContext ctx;
  AdapterStatus status;
  int ret = in_conv->decode_fn(in_view, in_options, in_bindings, &ctx, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);

  // Setup audit results in context
  StructuredDocumentBatch verdicts;
  verdicts.emplace_back(
      0, 0,
      JsonDocumentItem("{\"risk\":\"high\"}", true, JsonParseStatus::kOk, "",
                       {{"risk_level", "HIGH_RISK"}, {"risk_score", 0.88f}}));
  ctx.Publish("structured_verdicts", std::move(verdicts));

  RankedTextBatch policies;
  policies.emplace_back(0, 0, RankedCandidate("Rule 12.3", 0.88f, 1, 0));
  ctx.Publish("matched_policies", std::move(policies));

  char risk_buf[32] = {0};
  CompanyString cs_risk{31, risk_buf};
  char clause_buf[256] = {0};
  CompanyString cs_clause{255, clause_buf};
  char verdict_buf[1024] = {0};
  CompanyString cs_verdict{1023, verdict_buf};

  CompanyOperatorAuditOutput audit_out{};
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

  OutputPortBindings out_bindings(
      {{"raw_request_ids", "raw_request_ids"},
       {"structured_verdicts", "structured_verdicts"},
       {"matched_policies", "matched_policies"}});
  OutputEncodeOptions out_options;
  out_options.converter_id = out_conv->converter_id;

  size_t written = 0;
  ret = out_conv->encode_fn(&ctx, out_bindings, out_options, &out_view,
                            &written, &status);
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

// ==================== AudioAsr ====================
TEST_F(ComplexConvertersTest, AudioAsrOperatorInputAndOutput) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "audio.pcm.operator.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "audio_result.plain.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  std::vector<float> pcm = {0.1f, 0.2f, -0.1f};
  CompanyOperatorAudioInput audio_in{5001, pcm.data(),
                                     static_cast<int32_t>(pcm.size()), 16000};
  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["audio_in"] = llm_edgeflow::BorrowInputForTest({&audio_in});
  in_view.slot_types["audio_in"] = "CompanyOperatorAudioInput";

  InputPortBindings in_bindings({{"raw_request_ids", "raw_request_ids"},
                                 {"audio_inputs", "audio_inputs"}});
  InputDecodeOptions in_options;
  in_options.converter_id = in_conv->converter_id;

  AlgContext ctx;
  AdapterStatus status;
  int ret = in_conv->decode_fn(in_view, in_options, in_bindings, &ctx, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);

  TextBatch transcripts;
  transcripts.emplace_back(0, 0, "open the front door");
  ctx.Publish("transcripts", std::move(transcripts));

  RuleMatchBatch slots;
  RuleMatchItem m;
  m.status_code = 0;
  m.match_result_json =
      "{\"intent\":\"open_door\",\"slot\":{\"target\":\"front\"}}";
  slots.emplace_back(0, 0, m);
  ctx.Publish("intent_slots", std::move(slots));

  char trans_buf[512] = {0};
  CompanyString cs_trans{511, trans_buf};
  char slot_buf[1024] = {0};
  CompanyString cs_slot{1023, slot_buf};

  CompanyOperatorAudioOutput audio_out{};
  audio_out.transcribed_text = &cs_trans;
  audio_out.intent_slot_json = &cs_slot;

  TestOutputBatchView out_view;
  out_view.count = 1;
  out_view.leased_slots["audio_out"] = {&audio_out};
  out_view.slot_types["audio_out"] = "CompanyOperatorAudioOutput";
  out_view.SetCapacity("audio_out", "transcribed_text", 511);
  out_view.SetCapacity("audio_out", "intent_slot_json", 1023);

  OutputPortBindings out_bindings({{"raw_request_ids", "raw_request_ids"},
                                   {"transcripts", "transcripts"},
                                   {"intent_slots", "intent_slots"}});
  OutputEncodeOptions out_options;
  out_options.converter_id = out_conv->converter_id;

  size_t written = 0;
  ret = out_conv->encode_fn(&ctx, out_bindings, out_options, &out_view,
                            &written, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(audio_out.request_id, 5001U);
  ASSERT_NE(audio_out.transcribed_text, nullptr);
  EXPECT_STREQ(audio_out.transcribed_text->data, "open the front door");
  ASSERT_NE(audio_out.intent_slot_json, nullptr);
  EXPECT_STREQ(audio_out.intent_slot_json->data,
               "{\"intent\":\"open_door\",\"slot\":{\"target\":\"front\"}}");
}

// ==================== OcrDocQa ====================
TEST_F(ComplexConvertersTest, OcrDocQaOperatorInputAndOutput) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "image_query.plain.operator.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "invoice_result.plain.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  // Setup operator input: frame and string
  std::string uri_str = "/path/to/invoice.jpg";
  CompanyString uri{static_cast<int32_t>(uri_str.size()),
                    const_cast<char*>(uri_str.data())};
  CompanyFrame frame{};
  frame.request_id = 6001;
  frame.image_uri = &uri;

  std::string q_str = "Total amount?";
  CompanyString query{static_cast<int32_t>(q_str.size()),
                      const_cast<char*>(q_str.data())};

  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["frame"] = llm_edgeflow::BorrowInputForTest({&frame});
  in_view.slots["string"] = llm_edgeflow::BorrowInputForTest({&query});
  in_view.slot_types["frame"] = "CompanyFrame";
  in_view.slot_types["string"] = "CompanyString";

  InputPortBindings in_bindings({{"raw_request_ids", "raw_request_ids"},
                                 {"image_paths", "image_paths"},
                                 {"user_queries", "user_queries"}});
  InputDecodeOptions in_options;
  in_options.converter_id = in_conv->converter_id;

  AlgContext ctx;
  AdapterStatus status;
  int ret = in_conv->decode_fn(in_view, in_options, in_bindings, &ctx, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);

  // Setup invoice output in AlgContext
  StructuredDocumentBatch invoices;
  JsonDocumentItem doc_item;
  doc_item.is_valid = true;
  doc_item.parse_status = JsonParseStatus::kOk;
  doc_item.json_payload = "{\"total\":123.45}";
  invoices.emplace_back(0, 0, doc_item);
  ctx.Publish("extracted_invoice_json", std::move(invoices));

  OcrDocumentBatch ocr_docs;
  OcrDocumentItem ocr_doc;
  ocr_doc.boxes.push_back({0, 0, 10, 10, "Invoice", 0.99f});
  ocr_docs.emplace_back(0, 0, ocr_doc);
  ctx.Publish("ocr_docs", std::move(ocr_docs));

  // Destination operator od_out
  CompanyOdOutput od_out{};
  std::vector<char> buf(256);
  CompanyString res_str{255, buf.data()};
  od_out.result_json = &res_str;

  TestOutputBatchView out_view;
  out_view.leased_slots["od_out"] = {&od_out};
  out_view.slot_types["od_out"] = "CompanyOdOutput";
  out_view.SetCapacity("od_out", "result_json", 255);
  out_view.count = 1;

  OutputPortBindings out_bindings(
      {{"raw_request_ids", "raw_request_ids"},
       {"extracted_invoice_json", "extracted_invoice_json"},
       {"ocr_docs", "ocr_docs"}});
  OutputEncodeOptions out_options;
  out_options.converter_id = out_conv->converter_id;

  size_t written = 0;
  ret = out_conv->encode_fn(&ctx, out_bindings, out_options, &out_view,
                            &written, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(od_out.request_id, 6001U);
  EXPECT_EQ(od_out.detected_box_count, 1);
  ASSERT_NE(od_out.result_json, nullptr);
  EXPECT_STREQ(od_out.result_json->data, "{\"total\":123.45}");
}

// ==================== All 8 Businesses Bound ====================
TEST_F(ComplexConvertersTest, AllEightBusinessesRegistered) {
  const std::vector<std::string> expected_biz = {
      "translate_v1",
      "entity_extract_v1",
      "keyword_match_v1",
      "smart_doc_qa_v1",
      "dense_cross_rerank_scoring",
      "dialogue_compliance_audit_v1",
      "speech_audio_asr_intent_slot",
      "multimodal_ocr_invoice_qa",
  };

  for (const auto& biz : expected_biz) {
    const auto* desc = IoBindingRegistry::Instance().FindExposure(biz);
    ASSERT_NE(desc, nullptr) << "Missing biz exposure: " << biz;
    auto biz_def = PipelineCatalog::FindBiz(biz);
    ASSERT_TRUE(biz_def.has_value()) << "Missing biz in catalog: " << biz;
  }
}

}  // namespace llm_edgeflow
