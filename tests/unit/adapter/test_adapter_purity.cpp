#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "adapter/adapter_authoring.h"
#include "adapter/adapter_batch.h"
#include "adapter/adapter_result.h"
#include "adapter/biz_adapter_registry.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/biz_results.h"
#include "adapter/shared_algorithm_runtime.h"
#include "core/alg_context.h"
#include "edgeflow/c_api.h"
#include "tests/support/adapter_harness.h"

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

namespace llm_edgeflow {
TEST_F(AdapterPurityTest, AuditJoinsRankOneByRequestAndRejectsFallback) {
  auto adapter =
      BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_COMPLIANCE_AUDIT);
  for (const auto parse_status :
       {JsonParseStatus::kOk, JsonParseStatus::kFailed,
        JsonParseStatus::kFallbackApplied}) {
    AlgContext ctx;
    ctx.Publish(kRawRequestIds, std::vector<uint64_t>{100, 200});
    StructuredDocumentBatch verdicts;
    for (uint32_t id : {1u, 0u}) {
      verdicts.emplace_back(
          id, 0,
          JsonDocumentItem("{}", true, parse_status, "",
                           {{"risk_level", "SAFE"}, {"risk_score", 0.1}}));
    }
    ctx.Publish(kStructuredVerdicts, std::move(verdicts));
    ctx.Publish(kMatchedPolicy, RankedTextBatch{{0, 0, {"req0 first", 1, 1}},
                                                {0, 1, {"req0 second", 0.5, 2}},
                                                {1, 0, {"req1 first", 1, 1}}});
    CompanyAuditOutputStruct out[2]{};
    void* outputs[] = {&out[0], &out[1]};
    int count = 2;
    AdapterStatus status;
    const int ret = adapter->Pack(&ctx, outputs, &count, &status);
    if (parse_status == JsonParseStatus::kOk) {
      ASSERT_EQ(ret, 0) << status.ToString();
      EXPECT_EQ(out[1].request_id, 200u);
      EXPECT_STREQ(out[1].matched_policy_clause, "req1 first");
    } else {
      EXPECT_NE(ret, 0);
    }
  }
}
TEST_F(AdapterPurityTest, OneToOneResultsRejectDuplicateAndOutOfRangeIds) {
  auto adapter =
      BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_KEYWORD_MATCH);
  for (const auto& ids :
       {std::vector<uint32_t>{0, 0}, std::vector<uint32_t>{0, 2}}) {
    AlgContext ctx;
    ctx.Publish(kRawRequestIds, std::vector<uint64_t>{100, 200});
    RuleMatchBatch matches;
    for (auto id : ids) matches.emplace_back(id, 0, RuleMatchItem{});
    ctx.Publish(kRuleMatches, std::move(matches));
    CompanyKeywordOutputStruct out[2]{};
    void* outputs[] = {&out[0], &out[1]};
    int count = 2;
    EXPECT_EQ(adapter->Pack(&ctx, outputs, &count),
              COMPANY_ALG_ERR_INVALID_INPUT);
  }
}

TEST_F(AdapterPurityTest, ComplianceAuditAdapter_RejectsOversizedChannelName) {
  auto adapter =
      BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_COMPLIANCE_AUDIT);
  ASSERT_NE(adapter, nullptr);

  const std::string valid_channel(256, 'c');
  const std::string oversized_channel(257, 'c');

  // Valid length <= 256
  {
    CompanyAuditInputStruct in{};
    in.request_id = 5001;
    in.user_text = "test query";
    in.channel_name = valid_channel.c_str();
    const void* inputs[] = {&in};
    AlgContext ctx;
    AdapterStatus status;
    EXPECT_EQ(adapter->Unpack(inputs, 1, &ctx, &status), COMPANY_ALG_SUCCESS);
  }

  // Oversized length > 256
  {
    CompanyAuditInputStruct in{};
    in.request_id = 5002;
    in.user_text = "test query";
    in.channel_name = oversized_channel.c_str();
    const void* inputs[] = {&in};
    AlgContext ctx;
    AdapterStatus status;
    EXPECT_EQ(adapter->Unpack(inputs, 1, &ctx, &status),
              COMPANY_ALG_ERR_INVALID_INPUT);
  }
}
}  // namespace llm_edgeflow

namespace llm_edgeflow {
TEST_F(AdapterPurityTest, VariableDocResultPreservesLongAnswerAndCAbiLimit) {
  auto adapter = BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_DOC_QA);
  AlgContext ctx;
  const std::string answer(5000, 'a');
  ctx.Publish(kRawRequestIds, std::vector<uint64_t>{10});
  ctx.Publish(kLlmAnswers, TextBatch{{0, 0, answer}});
  ctx.Publish(kIntentMatches, RuleMatchBatch{{0, 0, RuleMatchItem{}}});
  ctx.Publish(kDocChunkCounts, Int32Batch{{0, 0, 1}});
  CompanyDocOutputStruct fixed{};
  void* fixed_outputs[] = {&fixed};
  int count = 1;
  EXPECT_EQ(adapter->Pack(&ctx, fixed_outputs, &count),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  DocResult variable;
  void* variable_outputs[] = {&variable};
  count = 1;
  ASSERT_EQ(adapter->PackResultBatch(&ctx, variable_outputs, &count), 0);
  EXPECT_EQ(variable.answer_text, answer);
  EXPECT_EQ(variable.request_id, 10u);
}

// RFC-0053: TranslateAdapter Purity
TEST_F(AdapterPurityTest, TranslateAdapterPurity) {
  auto adapter =
      BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_TRANSLATE);
  ASSERT_NE(adapter, nullptr);

  CompanyEntityInputStruct in{};
  in.request_id = 9901;
  in.sentence_text = "{\"query\":\"测试翻译句子\"}";
  const void* inputs[] = {&in};

  AlgContext ctx;
  AdapterStatus status;
  ASSERT_EQ(adapter->Unpack(inputs, 1, &ctx, &status), 0);

  const auto* req_ids = ctx.Read(kRawRequestIds);
  const auto* sentences = ctx.Read(kInputSentences);
  ASSERT_NE(req_ids, nullptr);
  ASSERT_NE(sentences, nullptr);
  EXPECT_EQ((*req_ids)[0], 9901u);
  EXPECT_EQ((*sentences)[0].data, "测试翻译句子");

  TextBatch answers;
  answers.emplace_back(0, 0, "Translated Sentence");
  ctx.Publish(kLlmAnswers, std::move(answers));

  CompanyEntityOutputStruct out{};
  void* outputs[] = {&out};
  int num_out = 1;
  ASSERT_EQ(adapter->Pack(&ctx, outputs, &num_out, &status), 0);

  EXPECT_EQ(out.request_id, 9901u);
  EXPECT_EQ(out.status_code, 0);
  EXPECT_EQ(nlohmann::json::parse(out.entities_json),
            nlohmann::json({{"translated", "Translated Sentence"}}));
}

// RFC-0053: Copy-In purity (mutating input buffer does not mutate context)
TEST_F(AdapterPurityTest, InputBatchSkeleton_CopyInPurity) {
  auto adapter =
      BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_TRANSLATE);
  ASSERT_NE(adapter, nullptr);

  std::string buffer = "{\"query\":\"original query\"}";
  CompanyEntityInputStruct in{};
  in.request_id = 5555;
  in.sentence_text = buffer.c_str();
  const void* inputs[] = {&in};

  AlgContext ctx;
  AdapterStatus status;
  ASSERT_EQ(adapter->Unpack(inputs, 1, &ctx, &status), 0);

  // Overwrite external buffer
  buffer[11] = 'X';
  buffer[12] = 'X';

  const auto* sentences = ctx.Read(kInputSentences);
  ASSERT_NE(sentences, nullptr);
  EXPECT_EQ((*sentences)[0].data, "original query");
}

// RFC-0053: External duplicate request IDs allowed (internal req_id is index)
TEST_F(AdapterPurityTest, InputBatchSkeleton_ExternalDuplicateIdsAllowed) {
  auto adapter =
      BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_TRANSLATE);
  ASSERT_NE(adapter, nullptr);

  CompanyEntityInputStruct in0{1234, "{\"query\":\"q0\"}"};
  CompanyEntityInputStruct in1{1234, "{\"query\":\"q1\"}"};
  const void* inputs[] = {&in0, &in1};

  AlgContext ctx;
  AdapterStatus status;
  ASSERT_EQ(adapter->Unpack(inputs, 2, &ctx, &status), 0);

  const auto* req_ids = ctx.Read(kRawRequestIds);
  const auto* sentences = ctx.Read(kInputSentences);
  ASSERT_NE(req_ids, nullptr);
  ASSERT_NE(sentences, nullptr);
  EXPECT_EQ((*req_ids)[0], 1234u);
  EXPECT_EQ((*req_ids)[1], 1234u);
  EXPECT_EQ((*sentences)[0].req_id, 0u);
  EXPECT_EQ((*sentences)[1].req_id, 1u);

  TextBatch answers{{0, 0, "ans0"}, {1, 0, "ans1"}};
  ctx.Publish(kLlmAnswers, std::move(answers));

  CompanyEntityOutputStruct out0{}, out1{};
  void* outputs[] = {&out0, &out1};
  int count = 2;
  ASSERT_EQ(adapter->Pack(&ctx, outputs, &count, &status), 0);
  EXPECT_EQ(out0.request_id, 1234u);
  EXPECT_EQ(out1.request_id, 1234u);
}

// RFC-0053: All samples validated before publish (fail-closed, no partial
// publication)
TEST_F(AdapterPurityTest, InputBatchSkeleton_AllSamplesValidatedBeforePublish) {
  auto adapter =
      BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_TRANSLATE);
  ASSERT_NE(adapter, nullptr);

  // Sample 0 is valid, sample 1 has invalid JSON
  CompanyEntityInputStruct in0{1, "{\"query\":\"valid\"}"};
  CompanyEntityInputStruct in1{2, "invalid json"};
  const void* inputs[] = {&in0, &in1};

  AlgContext ctx;
  AdapterStatus status;
  EXPECT_EQ(adapter->Unpack(inputs, 2, &ctx, &status),
            COMPANY_ALG_ERR_INVALID_INPUT);

  // AlgContext must be completely unpopulated
  EXPECT_EQ(ctx.Read(kRawRequestIds), nullptr);
  EXPECT_EQ(ctx.Read(kInputSentences), nullptr);
}

// RFC-0053: Subsequent key conflict partial publication behavior (existing
// AlgContext semantics)
TEST_F(AdapterPurityTest,
       InputBatchSkeleton_SubsequentKeyConflictPartialPublish) {
  auto adapter =
      BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_TRANSLATE);
  ASSERT_NE(adapter, nullptr);

  AlgContext ctx;
  // Pre-publish kInputSentences to cause conflict on the second publish
  TextBatch existing_sentences{{0, 0, "pre-existing"}};
  ctx.Publish(kInputSentences, std::move(existing_sentences));

  CompanyEntityInputStruct in0{101, "{\"query\":\"test query\"}"};
  const void* inputs[] = {&in0};
  AdapterStatus status;
  int ret = adapter->Unpack(inputs, 1, &ctx, &status);

  // Unpack fails due to conflict on kInputSentences
  EXPECT_EQ(ret, COMPANY_ALG_ERR_INVALID_INPUT);

  // Partial publish behavior: kRawRequestIds was published before the conflict
  // and remains in ctx
  const auto* raw_ids = ctx.Read(kRawRequestIds);
  ASSERT_NE(raw_ids, nullptr);
  EXPECT_EQ(raw_ids->size(), 1u);
  EXPECT_EQ((*raw_ids)[0], 101u);

  // kInputSentences retained its pre-existing value
  const auto* sentences = ctx.Read(kInputSentences);
  ASSERT_NE(sentences, nullptr);
  EXPECT_EQ((*sentences)[0].data, "pre-existing");
}

// RFC-0053: Multi-way results out-of-order alignment, owned packing, and
// perturbation testing
TEST_F(AdapterPurityTest, DocQaAdapter_MultiWayResultsReorderedAndPerturbed) {
  auto adapter = BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_DOC_QA);
  ASSERT_NE(adapter, nullptr);

  test::AdapterHarness harness(adapter);
  harness.Publish(kRawRequestIds, std::vector<uint64_t>{1001, 2002});

  // Reordered answers: index 1 before index 0
  TextBatch answers{{1, 0, "Answer 1"}, {0, 0, "Answer 0"}};
  RuleMatchBatch intents{{0, 0, RuleMatchItem(1, "INTENT_0", "", "{}", 0.9f)},
                         {1, 0, RuleMatchItem(2, "INTENT_1", "", "{}", 0.8f)}};
  Int32Batch chunks{{0, 0, 3}, {1, 0, 5}};

  harness.Publish(kLlmAnswers, answers);
  harness.Publish(kIntentMatches, intents);
  harness.Publish(kDocChunkCounts, chunks);

  // 1. Pack C array outputs
  std::vector<CompanyDocOutputStruct> outputs(2);
  ASSERT_EQ(harness.PackC(&outputs), 0);
  EXPECT_EQ(outputs[0].request_id, 1001u);
  EXPECT_STREQ(outputs[0].answer_text, "Answer 0");
  EXPECT_STREQ(outputs[0].intent_name, "INTENT_0");
  EXPECT_EQ(outputs[0].chunk_count, 3);

  EXPECT_EQ(outputs[1].request_id, 2002u);
  EXPECT_STREQ(outputs[1].answer_text, "Answer 1");
  EXPECT_STREQ(outputs[1].intent_name, "INTENT_1");
  EXPECT_EQ(outputs[1].chunk_count, 5);

  // 2. Pack owned Result outputs (dual representation behavior)
  std::vector<DocResult> owned_outputs(2);
  ASSERT_EQ(harness.PackOwned(&owned_outputs), 0);
  EXPECT_EQ(owned_outputs[0].request_id, 1001u);
  EXPECT_EQ(owned_outputs[0].answer_text, "Answer 0");
  EXPECT_EQ(owned_outputs[0].intent_name, "INTENT_0");
  EXPECT_EQ(owned_outputs[0].chunk_count, 3);

  EXPECT_EQ(owned_outputs[1].request_id, 2002u);
  EXPECT_EQ(owned_outputs[1].answer_text, "Answer 1");
  EXPECT_EQ(owned_outputs[1].intent_name, "INTENT_1");
  EXPECT_EQ(owned_outputs[1].chunk_count, 5);

  // 3. Harness Perturbations: out-of-range req_id, invalid sub_id, duplicate
  // req_id, missing item
  for (const auto anomaly :
       {test::AdapterHarness::ProvenanceAnomaly::kOutOfRangeReqId,
        test::AdapterHarness::ProvenanceAnomaly::kInvalidSubId,
        test::AdapterHarness::ProvenanceAnomaly::kDuplicateReqId,
        test::AdapterHarness::ProvenanceAnomaly::kMissingReqId}) {
    test::AdapterHarness h(adapter);
    h.Publish(kRawRequestIds, std::vector<uint64_t>{1001, 2002});
    auto perturbed_answers = test::AdapterHarness::PerturbBatch(
        std::vector<std::string>{"Answer 0", "Answer 1"}, anomaly);
    h.Publish(kLlmAnswers, perturbed_answers);
    h.Publish(kIntentMatches, intents);
    h.Publish(kDocChunkCounts, chunks);
    std::vector<CompanyDocOutputStruct> out(2);
    EXPECT_EQ(h.PackC(&out), COMPANY_ALG_ERR_INVALID_INPUT);
  }
}

// RFC-0053: RequestResults Multi-Way Alignment Direct Unit Coverage
TEST(RequestResultsTest, MultiWayAlignmentAndAccessors) {
  std::vector<uint64_t> raw_ids = {100, 200};
  TextBatch answers{{0, 0, "ans0"}, {1, 0, "ans1"}};
  RuleMatchBatch intents{{0, 0, RuleMatchItem(1, "INTENT", "")},
                         {1, 0, RuleMatchItem(2, "OTHER", "")}};
  Int32Batch chunks{{0, 0, 7}, {1, 0, 14}};

  std::vector<const TextBatch::value_type*> p = {&answers[0], &answers[1]};
  std::vector<const RuleMatchBatch::value_type*> s0 = {&intents[0],
                                                       &intents[1]};
  std::vector<const Int32Batch::value_type*> s1 = {&chunks[0], &chunks[1]};

  // With raw_request_ids
  RequestResults<TextBatch, RuleMatchBatch, Int32Batch> results(
      &raw_ids, std::move(p), std::make_tuple(std::move(s0), std::move(s1)));

  EXPECT_EQ(results.Size(), 2u);
  EXPECT_EQ(results.RequestId(0), 100u);
  EXPECT_EQ(results.RequestId(1), 200u);
  EXPECT_EQ(results.Primary(0).data, "ans0");
  EXPECT_EQ(results.Primary(1).data, "ans1");
  EXPECT_EQ(results.Secondary<0>(0).data.category, "INTENT");
  EXPECT_EQ(results.Secondary<0>(1).data.category, "OTHER");
  EXPECT_EQ(results.Secondary<1>(0).data, 7);
  EXPECT_EQ(results.Secondary<1>(1).data, 14);

  // Without raw_request_ids (falls back to primary req_id)
  std::vector<const TextBatch::value_type*> p_fallback = {&answers[0],
                                                          &answers[1]};
  RequestResults<TextBatch> fallback_res(nullptr, std::move(p_fallback), {});
  EXPECT_EQ(fallback_res.RequestId(0), 0u);
  EXPECT_EQ(fallback_res.RequestId(1), 1u);
}

// RFC-0053: AdapterResult Direct Unit Coverage
TEST(AdapterResultTest, TypedAndVoidMethods) {
  // 1. AdapterResult<T> Ok
  auto ok_res = AdapterResult<std::string>::Ok("hello_edgeflow");
  EXPECT_TRUE(ok_res.IsOk());
  EXPECT_TRUE(static_cast<bool>(ok_res));
  EXPECT_EQ(ok_res.ReturnCode(), COMPANY_ALG_SUCCESS);
  EXPECT_EQ(ok_res.Value(), "hello_edgeflow");
  EXPECT_EQ(*ok_res, "hello_edgeflow");
  EXPECT_EQ(ok_res->size(), 14u);
  EXPECT_EQ(ok_res.ValueOr("default"), "hello_edgeflow");
  EXPECT_EQ(ok_res.TakeValue(), "hello_edgeflow");

  // 2. AdapterResult<T> InvalidInput
  auto err_res = AdapterResult<std::string>::InvalidInput(
      "custom error", "req.field", 2, "TestAdapter");
  EXPECT_FALSE(err_res.IsOk());
  EXPECT_FALSE(static_cast<bool>(err_res));
  EXPECT_EQ(err_res.ReturnCode(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(err_res.Status().Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(err_res.Status().FieldPath(), "req.field");
  EXPECT_EQ(err_res.Status().SampleIndex(), 2);
  EXPECT_EQ(err_res.Status().AdapterName(), "TestAdapter");
  EXPECT_EQ(err_res.ValueOr("fallback"), "fallback");

  // 3. AdapterResult<T> BufferTooSmall
  auto buf_res = AdapterResult<int>::BufferTooSmall("buffer short", "out.buf",
                                                    0, "TestAdapter");
  EXPECT_FALSE(buf_res.IsOk());
  EXPECT_EQ(buf_res.ReturnCode(), COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(buf_res.Status().Code(), COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(buf_res.ValueOr(42), 42);

  // 4. AdapterResult<void>
  auto void_ok = AdapterResult<void>::Ok();
  EXPECT_TRUE(void_ok.IsOk());
  EXPECT_EQ(void_ok.ReturnCode(), COMPANY_ALG_SUCCESS);

  auto void_err = AdapterResult<void>::InvalidInput("void error");
  EXPECT_FALSE(void_err.IsOk());
  EXPECT_EQ(void_err.ReturnCode(), COMPANY_ALG_ERR_INVALID_INPUT);

  auto void_buf = AdapterResult<void>::BufferTooSmall("void buf error");
  EXPECT_FALSE(void_buf.IsOk());
  EXPECT_EQ(void_buf.ReturnCode(), COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
}

// RFC-0053: ReadMultiWayResults Parameterized Reader Direct Coverage
TEST(ReadMultiWayResultsTest, ReadsAndAlignsMultiWayResults) {
  AlgContext ctx;
  ctx.Publish(kRawRequestIds, std::vector<uint64_t>{3001, 3002});
  // Reordered answers to verify alignment
  TextBatch answers{{1, 0, "ans_1"}, {0, 0, "ans_0"}};
  RuleMatchBatch intents{{0, 0, RuleMatchItem(1, "INTENT_A", "", "{}", 0.95f)},
                         {1, 0, RuleMatchItem(2, "INTENT_B", "", "{}", 0.85f)}};
  Int32Batch chunks{{0, 0, 4}, {1, 0, 8}};

  ctx.Publish(kLlmAnswers, std::move(answers));
  ctx.Publish(kIntentMatches, std::move(intents));
  ctx.Publish(kDocChunkCounts, std::move(chunks));

  const ResultBindingSpec<TextBatch> primary_spec(
      kLlmAnswers, "answers", "answers", true, "Missing answers",
      COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  const ResultBindingSpec<std::vector<uint64_t>> raw_req_ids_spec(
      kRawRequestIds, "raw_request_ids", "raw_request_ids", true,
      "raw_request_ids mismatch", COMPANY_ALG_ERR_INVALID_INPUT);
  const ResultBindingSpec<RuleMatchBatch> intent_spec(
      kIntentMatches, "intent_matches", "intent_matches", true,
      "intent_matches mismatch", COMPANY_ALG_ERR_INVALID_INPUT);
  const ResultBindingSpec<Int32Batch> chunk_spec(
      kDocChunkCounts, "doc_chunk_counts", "chunk_counts", true,
      "doc_chunk_counts mismatch", COMPANY_ALG_ERR_INVALID_INPUT);

  std::vector<CompanyDocOutputStruct> outs(2);
  void* output_ptrs[2] = {&outs[0], &outs[1]};
  int count = 2;
  AdapterStatus status;
  RequestResults<TextBatch, RuleMatchBatch, Int32Batch> results;
  int ret = ReadMultiWayResults(&ctx, output_ptrs, &count, "TestDocQA", &status,
                                &results, primary_spec, raw_req_ids_spec,
                                intent_spec, chunk_spec);
  ASSERT_EQ(ret, COMPANY_ALG_SUCCESS);
  ASSERT_EQ(results.Size(), 2u);

  EXPECT_EQ(results.RequestId(0), 3001u);
  EXPECT_EQ(results.Primary(0).data, "ans_0");
  EXPECT_EQ(results.Secondary<0>(0).data.category, "INTENT_A");
  EXPECT_FLOAT_EQ(results.Secondary<0>(0).data.score, 0.95f);
  EXPECT_EQ(results.Secondary<1>(0).data, 4);

  EXPECT_EQ(results.RequestId(1), 3002u);
  EXPECT_EQ(results.Primary(1).data, "ans_1");
  EXPECT_EQ(results.Secondary<0>(1).data.category, "INTENT_B");
  EXPECT_FLOAT_EQ(results.Secondary<0>(1).data.score, 0.85f);
  EXPECT_EQ(results.Secondary<1>(1).data, 8);

  // Core reader without outputs/num_outputs
  RequestResults<TextBatch, RuleMatchBatch, Int32Batch> core_results;
  EXPECT_EQ(ReadMultiWayResults(&ctx, "TestDocQA", &status, &core_results,
                                primary_spec, raw_req_ids_spec, intent_spec,
                                chunk_spec),
            COMPANY_ALG_SUCCESS);
  EXPECT_EQ(core_results.Size(), 2u);

  // Null num_outputs returns BUFFER_TOO_SMALL safely without crashing
  EXPECT_EQ(ReadMultiWayResults(&ctx, output_ptrs, nullptr, "TestDocQA",
                                &status, &results, primary_spec,
                                raw_req_ids_spec, intent_spec, chunk_spec),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
}

// 验证变长模板推导：0 路次要结果与 3 路次要结果对齐
TEST(ReadMultiWayResultsTest, VariadicSecondarySlotsDeductionAndAlignment) {
  AlgContext ctx;
  ctx.Publish(kRawRequestIds, std::vector<uint64_t>{2001, 2002});
  ctx.Publish(kLlmAnswers, TextBatch{{0, 0, "ans_0"}, {1, 0, "ans_1"}});
  ctx.Publish(kIntentMatches,
              RuleMatchBatch{{0, 0, RuleMatchItem(1, "A", "", "{}", 0.9f)},
                             {1, 0, RuleMatchItem(2, "B", "", "{}", 0.8f)}});
  ctx.Publish(kDocChunkCounts, Int32Batch{{0, 0, 3}, {1, 0, 5}});
  ctx.Publish(kDocChunks, TextBatch{{0, 0, "chunk_0"}, {1, 0, "chunk_1"}});

  const ResultBindingSpec<TextBatch> primary_spec(kLlmAnswers, "answers");
  const ResultBindingSpec<std::vector<uint64_t>> raw_req_ids_spec(
      kRawRequestIds, "raw_request_ids");
  const ResultBindingSpec<RuleMatchBatch> intent_spec(kIntentMatches,
                                                      "intent_matches");
  const ResultBindingSpec<Int32Batch> chunk_spec(kDocChunkCounts,
                                                 "doc_chunk_counts");
  const ResultBindingSpec<TextBatch> doc_spec(kDocChunks, "doc_chunks");

  AdapterStatus status;

  // 1. 0 路次要槽位（仅 primary + raw_req_ids）
  RequestResults<TextBatch> zero_results;
  int ret0 = ReadMultiWayResults(&ctx, "TestAdapter", &status, &zero_results,
                                 primary_spec, raw_req_ids_spec);
  ASSERT_EQ(ret0, COMPANY_ALG_SUCCESS);
  ASSERT_EQ(zero_results.Size(), 2u);
  EXPECT_EQ(zero_results.RequestId(0), 2001u);
  EXPECT_EQ(zero_results.Primary(0).data, "ans_0");
  EXPECT_EQ(zero_results.RequestId(1), 2002u);
  EXPECT_EQ(zero_results.Primary(1).data, "ans_1");

  // 2. 3 路次要槽位（3+ variadic secondary specs）
  RequestResults<TextBatch, RuleMatchBatch, Int32Batch, TextBatch>
      three_results;
  int ret3 = ReadMultiWayResults(&ctx, "TestAdapter", &status, &three_results,
                                 primary_spec, raw_req_ids_spec, intent_spec,
                                 chunk_spec, doc_spec);
  ASSERT_EQ(ret3, COMPANY_ALG_SUCCESS);
  ASSERT_EQ(three_results.Size(), 2u);
  EXPECT_EQ(three_results.RequestId(0), 2001u);
  EXPECT_EQ(three_results.Primary(0).data, "ans_0");
  EXPECT_EQ(three_results.Secondary<0>(0).data.category, "A");
  EXPECT_EQ(three_results.Secondary<1>(0).data, 3);
  EXPECT_EQ(three_results.Secondary<2>(0).data, "chunk_0");

  EXPECT_EQ(three_results.RequestId(1), 2002u);
  EXPECT_EQ(three_results.Primary(1).data, "ans_1");
  EXPECT_EQ(three_results.Secondary<0>(1).data.category, "B");
  EXPECT_EQ(three_results.Secondary<1>(1).data, 5);
  EXPECT_EQ(three_results.Secondary<2>(1).data, "chunk_1");
}

// RFC-0053: ReadMultiWayResults Error Mappings and Diagnostics
TEST(ReadMultiWayResultsTest, ErrorMappingsAndDiagnostics) {
  AlgContext ctx;
  AdapterStatus status;
  RequestResults<TextBatch> results;

  // 1. Missing primary with custom error code (-7)
  const ResultBindingSpec<TextBatch> custom_primary_spec(
      kLlmAnswers, "custom_field", "custom_field", true, "Custom missing msg",
      -7);
  const ResultBindingSpec<std::vector<uint64_t>> raw_spec(
      kRawRequestIds, "raw_req_ids", "raw_req_ids", true);

  EXPECT_EQ(ReadMultiWayResults(&ctx, "TestAdapter", &status, &results,
                                custom_primary_spec, raw_spec),
            -7);
  EXPECT_EQ(status.Code(), -7);
  EXPECT_EQ(status.FieldPath(), "custom_field");
  EXPECT_EQ(status.Message(), "Custom missing msg");

  // 2. Count mismatch in secondary batch
  ctx.Publish(kLlmAnswers, TextBatch{{0, 0, "ans0"}, {1, 0, "ans1"}});
  ctx.Publish(kRawRequestIds, std::vector<uint64_t>{1, 2});
  ctx.Publish(kDocChunkCounts, Int32Batch{{0, 0, 1}});  // only 1 item != 2

  const ResultBindingSpec<TextBatch> ok_primary_spec(kLlmAnswers, "answers");
  const ResultBindingSpec<Int32Batch> secondary_spec(
      kDocChunkCounts, "chunk_counts", "chunk_counts", true, "count mismatch",
      COMPANY_ALG_ERR_INVALID_INPUT);

  RequestResults<TextBatch, Int32Batch> mismatch_results;
  EXPECT_EQ(ReadMultiWayResults(&ctx, "TestAdapter", &status, &mismatch_results,
                                ok_primary_spec, raw_spec, secondary_spec),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "chunk_counts");
  EXPECT_EQ(status.Message(), "count mismatch");
}

// RFC-0053 §2.2: ReadMultiWayResults Rejects Non-Required Bindings
TEST(ReadMultiWayResultsTest, RejectsNonRequiredBindingsWithDiagnostic) {
  AlgContext ctx;
  ctx.Publish(kLlmAnswers, TextBatch{{0, 0, "ans0"}});
  ctx.Publish(kRawRequestIds, std::vector<uint64_t>{1001});
  ctx.Publish(kDocChunkCounts, Int32Batch{{0, 0, 1}});

  AdapterStatus status;
  RequestResults<TextBatch, Int32Batch> results;

  // 1. Non-required primary spec
  const ResultBindingSpec<TextBatch> opt_primary_spec(kLlmAnswers, "answers",
                                                      "answers", /*req=*/false);
  const ResultBindingSpec<std::vector<uint64_t>> req_raw_spec(
      kRawRequestIds, "raw_request_ids", "raw_request_ids", /*req=*/true);
  const ResultBindingSpec<Int32Batch> req_secondary_spec(
      kDocChunkCounts, "chunk_counts", "chunk_counts", /*req=*/true);

  EXPECT_EQ(
      ReadMultiWayResults(&ctx, "TestAdapter", &status, &results,
                          opt_primary_spec, req_raw_spec, req_secondary_spec),
      COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "answers");
  EXPECT_EQ(status.Message(),
            "Optional bindings not supported in current phase");
  EXPECT_EQ(status.AdapterName(), "TestAdapter");

  // 2. Non-required raw_req_ids spec
  const ResultBindingSpec<TextBatch> req_primary_spec(kLlmAnswers, "answers",
                                                      "answers", /*req=*/true);
  const ResultBindingSpec<std::vector<uint64_t>> opt_raw_spec(
      kRawRequestIds, "raw_request_ids", "raw_request_ids", /*req=*/false);

  EXPECT_EQ(
      ReadMultiWayResults(&ctx, "TestAdapter", &status, &results,
                          req_primary_spec, opt_raw_spec, req_secondary_spec),
      COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "raw_request_ids");
  EXPECT_EQ(status.Message(),
            "Optional bindings not supported in current phase");

  // 3. Non-required secondary spec
  const ResultBindingSpec<Int32Batch> opt_secondary_spec(
      kDocChunkCounts, "chunk_counts", "chunk_counts", /*req=*/false);

  EXPECT_EQ(
      ReadMultiWayResults(&ctx, "TestAdapter", &status, &results,
                          req_primary_spec, req_raw_spec, opt_secondary_spec),
      COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "chunk_counts");
  EXPECT_EQ(status.Message(),
            "Optional bindings not supported in current phase");

  // 4. Overload with outputs and num_outputs also rejects non-required bindings
  CompanyDocOutputStruct out;
  void* output_ptrs[1] = {&out};
  int count = 1;

  EXPECT_EQ(ReadMultiWayResults(&ctx, output_ptrs, &count, "TestAdapter",
                                &status, &results, opt_primary_spec,
                                req_raw_spec, req_secondary_spec),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "answers");
  EXPECT_EQ(status.Message(),
            "Optional bindings not supported in current phase");

  EXPECT_EQ(ReadMultiWayResults(&ctx, output_ptrs, &count, "TestAdapter",
                                &status, &results, req_primary_spec,
                                req_raw_spec, opt_secondary_spec),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "chunk_counts");
  EXPECT_EQ(status.Message(),
            "Optional bindings not supported in current phase");

  EXPECT_EQ(ReadMultiWayResults(&ctx, output_ptrs, &count, "TestAdapter",
                                &status, &results, req_primary_spec,
                                opt_raw_spec, req_secondary_spec),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "raw_request_ids");
  EXPECT_EQ(status.Message(),
            "Optional bindings not supported in current phase");

  // 5. Multiple secondary specs short-circuiting to first non-required binding
  const ResultBindingSpec<RuleMatchBatch> req_intent_spec(
      kIntentMatches, "intent_matches", "intent_matches", /*req=*/true);
  const ResultBindingSpec<RuleMatchBatch> opt_intent_spec(
      kIntentMatches, "intent_matches", "intent_matches", /*req=*/false);
  RequestResults<TextBatch, Int32Batch, RuleMatchBatch> multi_sec_results;

  // Second secondary spec is optional -> rejected with second secondary
  // field_name
  EXPECT_EQ(
      ReadMultiWayResults(&ctx, "TestAdapter", &status, &multi_sec_results,
                          req_primary_spec, req_raw_spec, req_secondary_spec,
                          opt_intent_spec),
      COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "intent_matches");
  EXPECT_EQ(status.Message(),
            "Optional bindings not supported in current phase");

  // Both secondary specs are optional -> short-circuits to first non-required
  // secondary
  EXPECT_EQ(
      ReadMultiWayResults(&ctx, "TestAdapter", &status, &multi_sec_results,
                          req_primary_spec, req_raw_spec, opt_secondary_spec,
                          opt_intent_spec),
      COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "chunk_counts");
  EXPECT_EQ(status.Message(),
            "Optional bindings not supported in current phase");
}

// RFC-0053: OneToOneTextAdapter Custom Null Context Hooks
TEST(OneToOneTextAdapterTest, CustomNullContextHooks) {
  struct CustomHookSpecProvider {
    static const OneToOneTextAdapterSpec& GetSpec() {
      static const OneToOneTextAdapterSpec spec = [] {
        OneToOneTextAdapterSpec s;
        s.biz_type = static_cast<CompanyAlgBizType>(999);
        s.adapter_name = "CustomHook";
        s.unpack_null_ctx_hook = [](const char* name,
                                    AdapterStatus* out_status) -> int {
          if (out_status) {
            *out_status = AdapterStatus(-123, "Custom unpack null ctx",
                                        "custom_unpack", -1, name);
          }
          return -123;
        };
        s.pack_null_ctx_hook = [](const char* name,
                                  AdapterStatus* out_status) -> int {
          if (out_status) {
            *out_status = AdapterStatus(-456, "Custom pack null ctx",
                                        "custom_pack", -1, name);
          }
          return -456;
        };
        return s;
      }();
      return spec;
    }
  };

  OneToOneTextAdapter<CustomHookSpecProvider> adapter;
  AdapterStatus status;

  CompanyEntityInputStruct input{1, "test"};
  const void* inputs[] = {&input};
  EXPECT_EQ(adapter.Unpack(inputs, 1, nullptr, &status), -123);
  EXPECT_EQ(status.Code(), -123);
  EXPECT_EQ(status.FieldPath(), "custom_unpack");
  EXPECT_EQ(status.Message(), "Custom unpack null ctx");

  CompanyEntityOutputStruct output{};
  void* outputs[] = {&output};
  int count = 1;
  EXPECT_EQ(adapter.Pack(nullptr, outputs, &count, &status), -456);
  EXPECT_EQ(status.Code(), -456);
  EXPECT_EQ(status.FieldPath(), "custom_pack");
  EXPECT_EQ(status.Message(), "Custom pack null ctx");
}

// RFC-0053: DocQaAdapter Characterization and Diagnostics
TEST_F(AdapterPurityTest, DocQaAdapterDiagnosticsCharacterization) {
  auto adapter = BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_DOC_QA);
  ASSERT_NE(adapter, nullptr);

  CompanyDocOutputStruct out{};
  void* outputs[] = {&out};
  int count = 1;
  AdapterStatus status;

  // 1. Null AlgContext: returns BUFFER_TOO_SMALL (-4) with field "ctx"
  EXPECT_EQ(adapter->Pack(nullptr, outputs, &count, &status),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(status.FieldPath(), "ctx");

  // 2. Empty AlgContext (missing answers): returns BUFFER_TOO_SMALL (-4) with
  // field "llm_answers" and legacy message "llm_answers not found in
  // AlgContext"
  AlgContext empty_ctx;
  EXPECT_EQ(adapter->Pack(&empty_ctx, outputs, &count, &status),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(status.FieldPath(), "llm_answers");
  EXPECT_EQ(status.Message(), "llm_answers not found in AlgContext");

  // 3. Null num_outputs pointer with valid context: returns BUFFER_TOO_SMALL
  // (-4) safely without crashing
  AlgContext valid_ctx;
  valid_ctx.Publish(kRawRequestIds, std::vector<uint64_t>{1001});
  valid_ctx.Publish(kLlmAnswers, TextBatch{{0, 0, "Doc answer"}});
  valid_ctx.Publish(
      kIntentMatches,
      RuleMatchBatch{{0, 0, RuleMatchItem(0, "QA", "", "{}", 1.0f)}});
  valid_ctx.Publish(kDocChunkCounts, Int32Batch{{0, 0, 5}});

  EXPECT_EQ(adapter->Pack(&valid_ctx, outputs, nullptr, &status),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_BUFFER_TOO_SMALL);

  // 4. Missing intent_matches: returns INVALID_INPUT (-3) with field
  // "intent_matches"
  AlgContext no_intent;
  no_intent.Publish(kRawRequestIds, std::vector<uint64_t>{1001});
  no_intent.Publish(kLlmAnswers, TextBatch{{0, 0, "Doc answer"}});
  no_intent.Publish(kDocChunkCounts, Int32Batch{{0, 0, 5}});
  count = 1;
  EXPECT_EQ(adapter->Pack(&no_intent, outputs, &count, &status),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "intent_matches");

  // 5. Missing doc_chunk_counts: returns INVALID_INPUT (-3) with field
  // "doc_chunk_counts"
  AlgContext no_chunks;
  no_chunks.Publish(kRawRequestIds, std::vector<uint64_t>{1001});
  no_chunks.Publish(kLlmAnswers, TextBatch{{0, 0, "Doc answer"}});
  no_chunks.Publish(
      kIntentMatches,
      RuleMatchBatch{{0, 0, RuleMatchItem(0, "QA", "", "{}", 1.0f)}});
  count = 1;
  EXPECT_EQ(adapter->Pack(&no_chunks, outputs, &count, &status),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "doc_chunk_counts");

  // 6. Missing raw_request_ids: returns INVALID_INPUT (-3) with field
  // "raw_request_ids"
  AlgContext no_ids;
  no_ids.Publish(kLlmAnswers, TextBatch{{0, 0, "Doc answer"}});
  no_ids.Publish(
      kIntentMatches,
      RuleMatchBatch{{0, 0, RuleMatchItem(0, "QA", "", "{}", 1.0f)}});
  no_ids.Publish(kDocChunkCounts, Int32Batch{{0, 0, 5}});
  count = 1;
  EXPECT_EQ(adapter->Pack(&no_ids, outputs, &count, &status),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "raw_request_ids");

  // 7. Successful Pack
  count = 1;
  EXPECT_EQ(adapter->Pack(&valid_ctx, outputs, &count, &status),
            COMPANY_ALG_SUCCESS);
  EXPECT_EQ(count, 1);
  EXPECT_EQ(out.request_id, 1001u);
  EXPECT_FLOAT_EQ(out.confidence, 1.0f);
  EXPECT_EQ(out.chunk_count, 5);
  EXPECT_STREQ(out.intent_name, "QA");
  EXPECT_STREQ(out.answer_text, "Doc answer");

  // 8. Invalid provenance on answers: IndexResults fails with field "answers"
  // (index_name), distinguishing it from missing "llm_answers"
  AlgContext bad_answers_ctx;
  bad_answers_ctx.Publish(kRawRequestIds, std::vector<uint64_t>{1001});
  bad_answers_ctx.Publish(kLlmAnswers,
                          TextBatch{{5, 0, "Doc answer"}});  // req_id 5 >= 1
  bad_answers_ctx.Publish(
      kIntentMatches,
      RuleMatchBatch{{0, 0, RuleMatchItem(0, "QA", "", "{}", 1.0f)}});
  bad_answers_ctx.Publish(kDocChunkCounts, Int32Batch{{0, 0, 5}});
  count = 1;
  EXPECT_EQ(adapter->Pack(&bad_answers_ctx, outputs, &count, &status),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "answers");
  EXPECT_EQ(status.Message(),
            "Missing, duplicate or invalid result provenance");

  // 9. Invalid provenance on doc_chunk_counts: IndexResults fails with field
  // "chunk_counts" (index_name), distinguishing it from missing
  // "doc_chunk_counts"
  AlgContext bad_chunks_ctx;
  bad_chunks_ctx.Publish(kRawRequestIds, std::vector<uint64_t>{1001});
  bad_chunks_ctx.Publish(kLlmAnswers, TextBatch{{0, 0, "Doc answer"}});
  bad_chunks_ctx.Publish(
      kIntentMatches,
      RuleMatchBatch{{0, 0, RuleMatchItem(0, "QA", "", "{}", 1.0f)}});
  bad_chunks_ctx.Publish(kDocChunkCounts,
                         Int32Batch{{5, 0, 5}});  // req_id 5 >= 1
  count = 1;
  EXPECT_EQ(adapter->Pack(&bad_chunks_ctx, outputs, &count, &status),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "chunk_counts");
  EXPECT_EQ(status.Message(),
            "Missing, duplicate or invalid result provenance");
}

// RFC-0053: AdapterHarness Capacity Pre-Query
TEST_F(AdapterPurityTest, AdapterHarnessCapacityPreQuery) {
  auto adapter = BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_DOC_QA);
  ASSERT_NE(adapter, nullptr);

  test::AdapterHarness harness(adapter);
  harness.Publish(kRawRequestIds, std::vector<uint64_t>{10, 20, 30});
  harness.Publish(kLlmAnswers,
                  TextBatch{{0, 0, "a0"}, {1, 0, "a1"}, {2, 0, "a2"}});
  harness.Publish(kIntentMatches, RuleMatchBatch{{0, 0, RuleMatchItem{}},
                                                 {1, 0, RuleMatchItem{}},
                                                 {2, 0, RuleMatchItem{}}});
  harness.Publish(kDocChunkCounts, Int32Batch{{0, 0, 1}, {1, 0, 2}, {2, 0, 3}});

  // Query capacity with empty outputs vector
  std::vector<CompanyDocOutputStruct> empty_c_outs;
  int ret_c = harness.PackC(&empty_c_outs);
  EXPECT_EQ(ret_c, COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_TRUE(empty_c_outs.empty());

  std::vector<DocResult> empty_owned_outs;
  int ret_owned = harness.PackOwned(&empty_owned_outs);
  EXPECT_EQ(ret_owned, COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_TRUE(empty_owned_outs.empty());
}

}  // namespace llm_edgeflow
