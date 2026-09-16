#include <gtest/gtest.h>

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
#include "edgeflow/c_api.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {

class ComplexConvertersTest : public ::testing::Test {};

// ==================== DocQA ====================
TEST_F(ComplexConvertersTest, DocQaCAbiInputAndOutput) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "doc_query.plain.cabi.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "doc_answer.plain.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  CompanyDocInputStruct doc_in{2001, "Sample document text", "What is sample?"};
  const void* in_items[] = {&doc_in};
  ExternalInputBatchView in_view;
  in_view.items = in_items;
  in_view.count = 1;

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
  chunk_counts.emplace_back(0, 0, 3);
  ctx.Publish("doc_chunk_counts", std::move(chunk_counts));

  CompanyDocOutputStruct doc_out{};
  void* out_items[] = {&doc_out};
  ExternalOutputBatchView out_view;
  out_view.items = out_items;
  out_view.count = 1;
  out_view.capacity = 1;

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
  EXPECT_STREQ(doc_out.intent_name, "general_faq");
  EXPECT_STREQ(doc_out.answer_text, "This is the answer.");
  EXPECT_FLOAT_EQ(doc_out.confidence, 0.95f);
  EXPECT_EQ(doc_out.chunk_count, 3);
}

// ==================== CrossRerank ====================
TEST_F(ComplexConvertersTest, CrossRerankCAbiInputAndOutput) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "rerank.plain.cabi.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "rerank_result.plain.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  const char* passages[] = {"passage zero", "passage one", "passage two"};
  CompanyRerankBatchInputStruct rerank_in{};
  rerank_in.request_id = 3001;
  rerank_in.query_text = "what is passage";
  rerank_in.candidate_count = 3;
  rerank_in.candidate_passages[0] = passages[0];
  rerank_in.candidate_passages[1] = passages[1];
  rerank_in.candidate_passages[2] = passages[2];

  const void* in_items[] = {&rerank_in};
  ExternalInputBatchView in_view;
  in_view.items = in_items;
  in_view.count = 1;

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

  // Ranked results
  RankedTextBatch ranked;
  ranked.emplace_back(0, 0, RankedCandidate("passage one", 0.9f, 1, 1));
  ranked.emplace_back(0, 1, RankedCandidate("passage zero", 0.5f, 2, 0));
  ranked.emplace_back(0, 2, RankedCandidate("passage two", 0.1f, 3, 2));
  ctx.Publish("ranked_results", std::move(ranked));

  CompanyRerankBatchOutputStruct rerank_out{};
  void* out_items[] = {&rerank_out};
  ExternalOutputBatchView out_view;
  out_view.items = out_items;
  out_view.count = 1;
  out_view.capacity = 1;

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
  EXPECT_EQ(rerank_out.count, 3);
  EXPECT_FLOAT_EQ(rerank_out.scores[0], 0.9f);
  EXPECT_EQ(rerank_out.sorted_indices[0], 1);
  EXPECT_FLOAT_EQ(rerank_out.scores[1], 0.5f);
  EXPECT_EQ(rerank_out.sorted_indices[1], 0);
}

// ==================== ComplianceAudit ====================
TEST_F(ComplexConvertersTest, ComplianceAuditCAbiInputAndOutput) {
  const auto* in_conv =
      IoConverterRegistry::Instance().FindInputConverter("audit.plain.cabi.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "audit_result.plain.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  CompanyAuditInputStruct audit_in{4001, "some dialogue text", "channel_vip"};
  const void* in_items[] = {&audit_in};
  ExternalInputBatchView in_view;
  in_view.items = in_items;
  in_view.count = 1;

  InputPortBindings in_bindings({{"raw_request_ids", "raw_request_ids"},
                                 {"user_texts", "user_texts"},
                                 {"channel_names", "channel_names"}});
  InputDecodeOptions in_options;
  in_options.converter_id = in_conv->converter_id;

  AlgContext ctx;
  AdapterStatus status;
  int ret = in_conv->decode_fn(in_view, in_options, in_bindings, &ctx, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);

  // Verdict document
  StructuredDocumentBatch verdicts;
  JsonDocumentItem doc_item;
  doc_item.is_valid = true;
  doc_item.parse_status = JsonParseStatus::kOk;
  doc_item.json_payload = "{\"risk_level\":\"SAFE\",\"risk_score\":0.05}";
  doc_item.structured_data = {{"risk_level", "SAFE"}, {"risk_score", 0.05f}};
  verdicts.emplace_back(0, 0, doc_item);
  ctx.Publish("structured_verdicts", std::move(verdicts));

  RankedTextBatch policies;
  policies.emplace_back(0, 0,
                        RankedCandidate("Article 42.1 Policy", 1.0f, 1, 0));
  ctx.Publish("matched_policies", std::move(policies));

  CompanyAuditOutputStruct audit_out{};
  void* out_items[] = {&audit_out};
  ExternalOutputBatchView out_view;
  out_view.items = out_items;
  out_view.count = 1;
  out_view.capacity = 1;

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
  EXPECT_FLOAT_EQ(audit_out.risk_score, 0.05f);
  EXPECT_STREQ(audit_out.risk_level, "SAFE");
  EXPECT_STREQ(audit_out.matched_policy_clause, "Article 42.1 Policy");
}

// ==================== AudioAsrIntent ====================
TEST_F(ComplexConvertersTest, AudioAsrIntentCAbiInputAndOutput) {
  const auto* in_conv =
      IoConverterRegistry::Instance().FindInputConverter("audio.pcm.cabi.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "audio_result.plain.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  std::vector<float> pcm(1600, 0.1f);
  CompanyAudioInputStruct audio_in{5001, pcm.data(),
                                   static_cast<int>(pcm.size()), 16000};
  const void* in_items[] = {&audio_in};
  ExternalInputBatchView in_view;
  in_view.items = in_items;
  in_view.count = 1;

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

  CompanyAudioOutputStruct audio_out{};
  void* out_items[] = {&audio_out};
  ExternalOutputBatchView out_view;
  out_view.items = out_items;
  out_view.count = 1;
  out_view.capacity = 1;

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
  EXPECT_STREQ(audio_out.transcribed_text, "open the front door");
  EXPECT_STREQ(audio_out.intent_slot_json,
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
  auto frame_holder = std::shared_ptr<void>(&frame, [](void*) {});
  auto query_holder = std::shared_ptr<void>(&query, [](void*) {});
  in_view.slots["frame"].push_back(frame_holder);
  in_view.slots["string"].push_back(query_holder);
  in_view.count = 1;

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
  CompanyString res_str{0, buf.data()};
  od_out.result_json = &res_str;

  ExternalOutputBatchView out_view;
  out_view.leased_slots["od_out"].push_back(&od_out);
  out_view.slot_capacities["od_out"]["result_json"] = 256;
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
    EXPECT_GE(desc->required_transports.size(), 2U)
        << "Biz missing transports: " << biz;

    auto biz_def = PipelineCatalog::FindBiz(biz);
    ASSERT_TRUE(biz_def.has_value()) << "Missing biz in catalog: " << biz;
  }
}

}  // namespace llm_edgeflow
