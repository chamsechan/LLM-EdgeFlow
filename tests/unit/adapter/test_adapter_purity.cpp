#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "adapter/biz_adapter_registry.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/shared_algorithm_runtime.h"
#include "company_alg_interface.h"
#include "core/alg_context.h"

namespace llm_edgeflow {

class AdapterPurityTest : public ::testing::Test {
 protected:
  void SetUp() override { SharedAlgorithmRuntime::GlobalInit(); }
};

// 1. DocQaAdapter Purity (Biz 1)
TEST_F(AdapterPurityTest, DocQaAdapterPurity) {
  auto adapter = BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_DOC_QA);
  ASSERT_NE(adapter, nullptr);

  CompanyDocInputStruct in{};
  in.request_id = 1001;
  in.doc_text = "Doc Content";
  in.query_text = "Query Question";
  const void* inputs[] = {&in};

  AlgContext ctx;
  AdapterStatus status;
  ASSERT_EQ(adapter->Unpack(inputs, 1, &ctx, &status), 0);

  const auto* req_ids = ctx.Read(kRawRequestIds);
  const auto* docs = ctx.Read(kRawDocs);
  const auto* queries = ctx.Read(kRawQueries);
  ASSERT_NE(req_ids, nullptr);
  ASSERT_NE(docs, nullptr);
  ASSERT_NE(queries, nullptr);
  EXPECT_EQ((*req_ids)[0], 1001u);
  EXPECT_EQ((*docs)[0].data, "Doc Content");
  EXPECT_EQ((*queries)[0].data, "Query Question");

  // Pack check
  TextBatch answers;
  answers.emplace_back(0, 0, "Model Generated Answer");
  ctx.Publish(kLlmAnswers, std::move(answers));

  RuleMatchBatch intents;
  intents.emplace_back(0, 0,
                       RuleMatchItem(1, "GENERAL_QA", "query", "{}", 0.95f));
  ctx.Publish(kIntentMatches, std::move(intents));

  Int32Batch chunk_counts;
  chunk_counts.emplace_back(0, 0, 1);
  ctx.Publish(kDocChunkCounts, std::move(chunk_counts));

  CompanyDocOutputStruct out{};
  void* outputs[] = {&out};
  int num_out = 1;
  ASSERT_EQ(adapter->Pack(&ctx, outputs, &num_out, &status), 0);

  EXPECT_EQ(out.request_id, 1001u);
  EXPECT_EQ(out.chunk_count, 1);
  EXPECT_STREQ(out.intent_name, "GENERAL_QA");
  EXPECT_FLOAT_EQ(out.confidence, 0.95f);
  EXPECT_STREQ(out.answer_text, "Model Generated Answer");
}

// 2. KeywordMatchAdapter Purity (Biz 2)
TEST_F(AdapterPurityTest, KeywordMatchAdapterPurity) {
  auto adapter =
      BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_KEYWORD_MATCH);
  ASSERT_NE(adapter, nullptr);

  // Unpack check: C Struct -> AlgContext
  CompanyKeywordInputStruct input{};
  input.request_id = 12345;
  const char* sentence = "测试输入文本";
  input.sentence_text = sentence;
  const void* inputs[] = {&input};

  AlgContext ctx;
  AdapterStatus status;
  ASSERT_EQ(adapter->Unpack(inputs, 1, &ctx, &status), 0);

  const auto* req_ids = ctx.Read(kRawRequestIds);
  const auto* text_batch = ctx.Read(kInputSentences);
  ASSERT_NE(req_ids, nullptr);
  ASSERT_NE(text_batch, nullptr);
  EXPECT_EQ((*req_ids)[0], 12345u);
  EXPECT_EQ((*text_batch)[0].data, sentence);

  // Pack check: AlgContext -> C Struct
  RuleMatchBatch match_batch;
  RuleMatchItem match_item(1, "TEST_CAT", "测试", "{\"intent\":\"TEST_CAT\"}",
                           1.0f);
  match_batch.emplace_back(0, 0, std::move(match_item));
  ctx.Publish(kRuleMatches, std::move(match_batch));

  CompanyKeywordOutputStruct output{};
  void* outputs[] = {&output};
  int num_outputs = 1;
  ASSERT_EQ(adapter->Pack(&ctx, outputs, &num_outputs, &status), 0);

  EXPECT_EQ(output.request_id, 12345u);
  EXPECT_EQ(output.is_hit, 1);
  EXPECT_STREQ(output.match_result_json, "{\"intent\":\"TEST_CAT\"}");
}

// 3. EntityExtractAdapter Purity (Biz 3)
TEST_F(AdapterPurityTest, EntityExtractAdapterPurity) {
  auto adapter =
      BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_ENTITY_EXTRACT);
  ASSERT_NE(adapter, nullptr);

  CompanyEntityInputStruct in{};
  in.request_id = 3001;
  in.sentence_text = "张三就职于阿里巴巴";
  const void* inputs[] = {&in};

  AlgContext ctx;
  AdapterStatus status;
  ASSERT_EQ(adapter->Unpack(inputs, 1, &ctx, &status), 0);

  StructuredDocumentBatch entities;
  entities.emplace_back(0, 0, JsonDocumentItem("[\"张三\",\"阿里巴巴\"]"));
  ctx.Publish(kExtractedEntities, std::move(entities));

  CompanyEntityOutputStruct out{};
  void* outputs[] = {&out};
  int num_out = 1;
  ASSERT_EQ(adapter->Pack(&ctx, outputs, &num_out, &status), 0);

  EXPECT_EQ(out.request_id, 3001u);
  EXPECT_STREQ(out.entities_json, "[\"张三\",\"阿里巴巴\"]");
}

// 4. ComplianceAuditAdapter Purity (Biz 4)
TEST_F(AdapterPurityTest, ComplianceAuditAdapterPurity) {
  auto adapter =
      BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_COMPLIANCE_AUDIT);
  ASSERT_NE(adapter, nullptr);

  CompanyAuditInputStruct in{};
  in.request_id = 8888;
  in.user_text = "客户投诉退款问题";
  in.channel_name = "VIP_HOTLINE";
  const void* inputs[] = {&in};

  AlgContext ctx;
  AdapterStatus status;
  ASSERT_EQ(adapter->Unpack(inputs, 1, &ctx, &status), 0);

  // Pack structured verdict from AlgContext directly
  StructuredDocumentBatch verdicts;
  nlohmann::json structured_obj = {{"risk_level", "HIGH_RISK"},
                                   {"risk_score", 0.92f}};
  verdicts.emplace_back(
      0, 0,
      JsonDocumentItem("{\"risk_level\":\"HIGH_RISK\",\"risk_score\":0.92}",
                       true, JsonParseStatus::kOk, "", structured_obj));
  ctx.Publish(kStructuredVerdicts, std::move(verdicts));

  RankedTextBatch policies;
  policies.emplace_back(0, 0,
                        RankedCandidate("Clause 9.1 Refund Policy", 0.95f, 1));
  ctx.Publish(kMatchedPolicy, std::move(policies));

  CompanyAuditOutputStruct out{};
  void* outputs[] = {&out};
  int num_out = 1;
  ASSERT_EQ(adapter->Pack(&ctx, outputs, &num_out, &status), 0);

  EXPECT_EQ(out.request_id, 8888u);
  EXPECT_STREQ(out.risk_level, "HIGH_RISK");
  EXPECT_FLOAT_EQ(out.risk_score, 0.92f);
  EXPECT_STREQ(out.matched_policy_clause, "Clause 9.1 Refund Policy");
}

// 5. OcrDocQaAdapter Purity (Biz 5)
TEST_F(AdapterPurityTest, OcrDocQaAdapterPurity) {
  auto adapter =
      BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_OCR_DOC_QA);
  ASSERT_NE(adapter, nullptr);

  CompanyOcrDocInputStruct in{};
  in.request_id = 5001;
  in.image_path = "./data/invoice.png";
  in.query_prompt = "提取发票总额";
  const void* inputs[] = {&in};

  AlgContext ctx;
  AdapterStatus status;
  ASSERT_EQ(adapter->Unpack(inputs, 1, &ctx, &status), 0);

  OcrDocumentBatch ocr_docs;
  OcrDocumentItem doc_item;
  doc_item.boxes.push_back({10, 20, 100, 30, "总计 500 元", 0.99f});
  ocr_docs.emplace_back(0, 0, std::move(doc_item));
  ctx.Publish(kOcrDocs, std::move(ocr_docs));

  StructuredDocumentBatch invoices;
  invoices.emplace_back(0, 0, JsonDocumentItem("{\"total\":500}"));
  ctx.Publish(kExtractedInvoiceJson, std::move(invoices));

  CompanyOcrDocOutputStruct out{};
  void* outputs[] = {&out};
  int num_out = 1;
  ASSERT_EQ(adapter->Pack(&ctx, outputs, &num_out, &status), 0);

  EXPECT_EQ(out.request_id, 5001u);
  EXPECT_EQ(out.detected_box_count, 1);
  EXPECT_STREQ(out.extracted_invoice_json, "{\"total\":500}");
}

// 6. AudioAsrIntentAdapter Purity (Biz 6)
TEST_F(AdapterPurityTest, AudioAsrIntentAdapterPurity) {
  auto adapter =
      BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_AUDIO_ASR_INTENT);
  ASSERT_NE(adapter, nullptr);

  std::vector<float> pcm(160, 0.1f);
  CompanyAudioInputStruct in{};
  in.request_id = 6001;
  in.pcm_buffer = pcm.data();
  in.pcm_length = static_cast<int64_t>(pcm.size());
  in.sample_rate = 16000;
  const void* inputs[] = {&in};

  AlgContext ctx;
  AdapterStatus status;
  ASSERT_EQ(adapter->Unpack(inputs, 1, &ctx, &status), 0);

  TextBatch transcripts;
  transcripts.emplace_back(0, 0, "导航到清华科技园");
  ctx.Publish(kTranscripts, std::move(transcripts));

  RuleMatchBatch slots;
  slots.emplace_back(0, 0,
                     RuleMatchItem(1, "NAVIGATION", "导航到",
                                   "{\"intent\":\"NAVIGATION\",\"slots\":{"
                                   "\"destination\":\"清华科技园\"}}",
                                   1.0f));
  ctx.Publish(kIntentSlots, std::move(slots));

  CompanyAudioOutputStruct out{};
  void* outputs[] = {&out};
  int num_out = 1;
  ASSERT_EQ(adapter->Pack(&ctx, outputs, &num_out, &status), 0);

  EXPECT_EQ(out.request_id, 6001u);
  EXPECT_STREQ(out.transcribed_text, "导航到清华科技园");
  EXPECT_NE(std::string(out.intent_slot_json).find("清华科技园"),
            std::string::npos);
}

// 7. CrossRerankAdapter Purity (Biz 7)
TEST_F(AdapterPurityTest, CrossRerankAdapterPurity) {
  auto adapter =
      BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_CROSS_RERANK);
  ASSERT_NE(adapter, nullptr);

  const char* query = "EdgeFlow 架构";
  const char* p0 = "LLM-EdgeFlow 核心组件";
  const char* p1 = "不相关段落";
  CompanyRerankBatchInputStruct in{};
  in.request_id = 7001;
  in.query_text = query;
  in.candidate_passages[0] = p0;
  in.candidate_passages[1] = p1;
  in.candidate_count = 2;
  const void* inputs[] = {&in};

  AlgContext ctx;
  AdapterStatus status;
  ASSERT_EQ(adapter->Unpack(inputs, 1, &ctx, &status), 0);

  RankedTextBatch results;
  results.emplace_back(0, 0, RankedCandidate(p0, 0.98f, 1, 0));
  results.emplace_back(0, 1, RankedCandidate(p1, 0.12f, 2, 1));
  ctx.Publish(kRankedResults, std::move(results));

  CompanyRerankBatchOutputStruct out{};
  void* outputs[] = {&out};
  int num_out = 1;
  ASSERT_EQ(adapter->Pack(&ctx, outputs, &num_out, &status), 0);

  EXPECT_EQ(out.request_id, 7001u);
  EXPECT_EQ(out.count, 2);
  EXPECT_FLOAT_EQ(out.scores[0], 0.98f);
  EXPECT_EQ(out.sorted_indices[0], 0);
}

// 8. Negative Tests: Fail-Closed Purity Assertions (Zero Fabrication)
TEST_F(AdapterPurityTest, DocQaAdapter_FailClosedWhenMissingOutputs) {
  auto adapter = BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_DOC_QA);
  ASSERT_NE(adapter, nullptr);

  AlgContext ctx;
  AdapterStatus status;
  CompanyDocOutputStruct out{};
  void* outputs[] = {&out};
  int num_out = 1;

  // Case 1: missing llm_answers
  EXPECT_NE(adapter->Pack(&ctx, outputs, &num_out, &status), 0);

  // Case 2: has llm_answers but missing intent_matches -> MUST fail-closed
  TextBatch answers;
  answers.emplace_back(0, 0, "Some answer");
  ctx.Publish(kLlmAnswers, std::move(answers));
  EXPECT_EQ(adapter->Pack(&ctx, outputs, &num_out, &status),
            COMPANY_ALG_ERR_INVALID_INPUT);

  // Case 3: has intent_matches but missing explicit per-request chunk counts
  // -> MUST fail-closed (the adapter may not derive business data).
  RuleMatchBatch intents;
  intents.emplace_back(0, 0, RuleMatchItem(1, "GENERAL_QA", "", "{}", 0.9f));
  ctx.Publish(kIntentMatches, std::move(intents));
  EXPECT_EQ(adapter->Pack(&ctx, outputs, &num_out, &status),
            COMPANY_ALG_ERR_INVALID_INPUT);

  // Case 4: all business outputs exist but input request provenance is absent.
  Int32Batch chunk_counts;
  chunk_counts.emplace_back(0, 0, 1);
  ctx.Publish(kDocChunkCounts, std::move(chunk_counts));
  EXPECT_EQ(adapter->Pack(&ctx, outputs, &num_out, &status),
            COMPANY_ALG_ERR_INVALID_INPUT);
}

TEST_F(AdapterPurityTest,
       ComplianceAuditAdapter_FailClosedWhenMissingStructuredFields) {
  auto adapter =
      BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_COMPLIANCE_AUDIT);
  ASSERT_NE(adapter, nullptr);

  AlgContext ctx;
  AdapterStatus status;
  CompanyAuditOutputStruct out{};
  void* outputs[] = {&out};
  int num_out = 1;

  // Case 1: missing structured_verdicts
  EXPECT_NE(adapter->Pack(&ctx, outputs, &num_out, &status), 0);

  // Case 2: structured_verdicts missing required field 'risk_level' -> MUST
  // fail-closed
  StructuredDocumentBatch verdicts;
  nlohmann::json incomplete_obj = {{"only_verdict", "合规"}};
  verdicts.emplace_back(
      0, 0,
      JsonDocumentItem("{}", true, JsonParseStatus::kOk, "", incomplete_obj));
  ctx.Publish(kStructuredVerdicts, std::move(verdicts));

  RankedTextBatch policies;
  policies.emplace_back(0, 0, RankedCandidate("Clause", 1.0f, 1));
  ctx.Publish(kMatchedPolicy, std::move(policies));

  EXPECT_EQ(adapter->Pack(&ctx, outputs, &num_out, &status),
            COMPANY_ALG_ERR_INVALID_INPUT);
}

}  // namespace llm_edgeflow
