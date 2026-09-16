#include <gtest/gtest.h>

#include <cstring>
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
#include "edgeflow/c_api.h"
#include "edgeflow/operator/types.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"
#include "tests/support/adapter_harness.h"

namespace llm_edgeflow {

class AdapterPurityTest : public ::testing::Test {
 protected:
  void SetUp() override { SharedAlgorithmRuntime::GlobalInit(); }
};

struct CustomMultiFieldInput {
  uint64_t req_id;
  const char* topic;
  const char* content;
};

DECLARE_EXTERNAL_TYPE_TRAITS(CustomMultiFieldInput, "CustomMultiFieldInput");

// =========================================================================
// 1. All 8 Businesses Converter Purity
// =========================================================================

// 1.1 DocQaConverter Purity (Biz 1)
TEST_F(AdapterPurityTest, DocQaAdapterPurity) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "doc_query.plain.cabi.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "doc_answer.plain.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(
      in_conv, out_conv,
      InputPortBindings({{"raw_request_ids", "raw_request_ids"},
                         {"raw_docs", "raw_docs"},
                         {"raw_queries", "raw_queries"}}),
      OutputPortBindings({{"raw_request_ids", "raw_request_ids"},
                          {"llm_answers", "llm_answers"},
                          {"intent_matches", "intent_matches"},
                          {"doc_chunk_counts", "doc_chunk_counts"}}));

  CompanyDocInputStruct in{};
  in.request_id = 1001;
  in.doc_text = "Doc Content";
  in.query_text = "Query Question";

  ASSERT_EQ(harness.DecodeCAbi({&in}), 0);

  const auto* req_ids =
      harness.Context().Read<std::vector<uint64_t>>("raw_request_ids");
  const auto* docs = harness.Context().Read<TextBatch>("raw_docs");
  const auto* queries = harness.Context().Read<TextBatch>("raw_queries");
  ASSERT_NE(req_ids, nullptr);
  ASSERT_NE(docs, nullptr);
  ASSERT_NE(queries, nullptr);
  EXPECT_EQ((*req_ids)[0], 1001u);
  EXPECT_EQ((*docs)[0].data, "Doc Content");
  EXPECT_EQ((*queries)[0].data, "Query Question");

  // Output encoding
  TextBatch answers;
  answers.emplace_back(0, 0, "Model Generated Answer");
  harness.Publish("llm_answers", std::move(answers));

  RuleMatchBatch intents;
  intents.emplace_back(0, 0,
                       RuleMatchItem(1, "GENERAL_QA", "query", "{}", 0.95f));
  harness.Publish("intent_matches", std::move(intents));

  Int32Batch chunk_counts;
  chunk_counts.emplace_back(0, 0, 1);
  harness.Publish("doc_chunk_counts", std::move(chunk_counts));

  std::vector<CompanyDocOutputStruct> outputs(1);
  ASSERT_EQ(harness.EncodeCAbi(&outputs), 0);

  EXPECT_EQ(outputs[0].request_id, 1001u);
  EXPECT_EQ(outputs[0].chunk_count, 1);
  EXPECT_STREQ(outputs[0].intent_name, "GENERAL_QA");
  EXPECT_FLOAT_EQ(outputs[0].confidence, 0.95f);
  EXPECT_STREQ(outputs[0].answer_text, "Model Generated Answer");
}

// 1.2 KeywordMatchConverter Purity (Biz 2)
TEST_F(AdapterPurityTest, KeywordMatchAdapterPurity) {
  const auto* in_conv =
      IoConverterRegistry::Instance().FindInputConverter("text.plain.cabi.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "keyword.result.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(
      in_conv, out_conv,
      InputPortBindings({{"raw_request_ids", "raw_request_ids"},
                         {"input_sentences", "input_sentences"}}),
      OutputPortBindings({{"raw_request_ids", "raw_request_ids"},
                          {"rule_matches", "rule_matches"}}));

  CompanyEntityInputStruct in{1002, "Some text"};
  ASSERT_EQ(harness.DecodeCAbi({&in}), 0);

  RuleMatchBatch matches;
  matches.emplace_back(
      0, 0,
      RuleMatchItem(1, "TEST_CAT", "测试", "{\"intent\":\"TEST_CAT\"}", 0.9f));
  harness.Publish("rule_matches", std::move(matches));

  std::vector<CompanyKeywordOutputStruct> outputs(1);
  ASSERT_EQ(harness.EncodeCAbi(&outputs), 0);

  EXPECT_EQ(outputs[0].request_id, 1002u);
  EXPECT_EQ(outputs[0].is_hit, 1);
  EXPECT_STREQ(outputs[0].match_result_json, "{\"intent\":\"TEST_CAT\"}");
}

// 1.3 EntityExtractConverter Purity (Biz 3)
TEST_F(AdapterPurityTest, EntityExtractAdapterPurity) {
  const auto* in_conv =
      IoConverterRegistry::Instance().FindInputConverter("text.plain.cabi.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "document.structured.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(
      in_conv, out_conv,
      InputPortBindings({{"raw_request_ids", "raw_request_ids"},
                         {"input_sentences", "input_sentences"}}),
      OutputPortBindings({{"raw_request_ids", "raw_request_ids"},
                          {"extracted_entities", "extracted_entities"}}));

  CompanyEntityInputStruct in{1003, "Entity text"};
  ASSERT_EQ(harness.DecodeCAbi({&in}), 0);

  StructuredDocumentBatch entities;
  entities.emplace_back(
      0, 0, JsonDocumentItem("[\"E1\"]", true, JsonParseStatus::kOk));
  harness.Publish("extracted_entities", std::move(entities));

  std::vector<CompanyEntityOutputStruct> outputs(1);
  ASSERT_EQ(harness.EncodeCAbi(&outputs), 0);

  EXPECT_EQ(outputs[0].request_id, 1003u);
  EXPECT_EQ(outputs[0].status_code, 0);
  EXPECT_STREQ(outputs[0].entities_json, "[\"E1\"]");
}

// 1.4 ComplianceAuditConverter Purity (Biz 4)
TEST_F(AdapterPurityTest, ComplianceAuditAdapterPurity) {
  const auto* in_conv =
      IoConverterRegistry::Instance().FindInputConverter("audit.plain.cabi.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "audit_result.plain.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(
      in_conv, out_conv,
      InputPortBindings({{"raw_request_ids", "raw_request_ids"},
                         {"user_texts", "user_texts"},
                         {"channel_names", "channel_names"}}),
      OutputPortBindings({{"raw_request_ids", "raw_request_ids"},
                          {"structured_verdicts", "structured_verdicts"},
                          {"matched_policies", "matched_policies"}}));

  CompanyAuditInputStruct in{1004, "audit sentence", "channel_vip"};
  ASSERT_EQ(harness.DecodeCAbi({&in}), 0);

  StructuredDocumentBatch verdicts;
  verdicts.emplace_back(
      0, 0,
      JsonDocumentItem("{\"risk_level\":\"SAFE\",\"risk_score\":0.1}", true,
                       JsonParseStatus::kOk, "",
                       {{"risk_level", "SAFE"}, {"risk_score", 0.1f}}));
  harness.Publish("structured_verdicts", std::move(verdicts));

  RankedTextBatch policies;
  policies.emplace_back(0, 0, RankedCandidate("Clause 1", 1.0f, 1, 0));
  harness.Publish("matched_policies", std::move(policies));

  std::vector<CompanyAuditOutputStruct> outputs(1);
  ASSERT_EQ(harness.EncodeCAbi(&outputs), 0);

  EXPECT_EQ(outputs[0].request_id, 1004u);
  EXPECT_FLOAT_EQ(outputs[0].risk_score, 0.1f);
  EXPECT_STREQ(outputs[0].risk_level, "SAFE");
  EXPECT_STREQ(outputs[0].matched_policy_clause, "Clause 1");
}

// 1.5 OcrDocQaConverter Purity (Biz 5)
TEST_F(AdapterPurityTest, OcrDocQaAdapterPurity) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "image_query.plain.cabi.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "invoice_result.plain.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(
      in_conv, out_conv,
      InputPortBindings({{"raw_request_ids", "raw_request_ids"},
                         {"image_paths", "image_paths"},
                         {"user_queries", "user_queries"}}),
      OutputPortBindings({{"raw_request_ids", "raw_request_ids"},
                          {"extracted_invoice_json", "extracted_invoice_json"},
                          {"ocr_docs", "ocr_docs"}}));

  CompanyOcrDocInputStruct in{1005, "/path/invoice.jpg", "Total amount?"};
  ASSERT_EQ(harness.DecodeCAbi({&in}), 0);

  StructuredDocumentBatch invoices;
  invoices.emplace_back(
      0, 0, JsonDocumentItem("{\"total\":99.9}", true, JsonParseStatus::kOk));
  harness.Publish("extracted_invoice_json", std::move(invoices));

  OcrDocumentBatch ocr_docs;
  OcrDocumentItem ocr_item;
  ocr_item.boxes.push_back({0, 0, 10, 10, "Total", 0.99f});
  ocr_docs.emplace_back(0, 0, std::move(ocr_item));
  harness.Publish("ocr_docs", std::move(ocr_docs));

  std::vector<CompanyOcrDocOutputStruct> outputs(1);
  ASSERT_EQ(harness.EncodeCAbi(&outputs), 0);

  EXPECT_EQ(outputs[0].request_id, 1005u);
  EXPECT_EQ(outputs[0].detected_box_count, 1U);
  EXPECT_STREQ(outputs[0].extracted_invoice_json, "{\"total\":99.9}");
}

// 1.6 AudioAsrIntentConverter Purity (Biz 6)
TEST_F(AdapterPurityTest, AudioAsrIntentAdapterPurity) {
  const auto* in_conv =
      IoConverterRegistry::Instance().FindInputConverter("audio.pcm.cabi.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "audio_result.plain.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(
      in_conv, out_conv,
      InputPortBindings({{"raw_request_ids", "raw_request_ids"},
                         {"audio_inputs", "audio_inputs"}}),
      OutputPortBindings({{"raw_request_ids", "raw_request_ids"},
                          {"transcripts", "transcripts"},
                          {"intent_slots", "intent_slots"}}));

  std::vector<float> pcm(1600, 0.05f);
  CompanyAudioInputStruct in{1006, pcm.data(), static_cast<int>(pcm.size()),
                             16000};
  ASSERT_EQ(harness.DecodeCAbi({&in}), 0);

  TextBatch transcripts;
  transcripts.emplace_back(0, 0, "turn left");
  harness.Publish("transcripts", std::move(transcripts));

  RuleMatchBatch intent_slots;
  intent_slots.emplace_back(
      0, 0, RuleMatchItem(1, "NAV", "", "{\"intent\":\"NAV\"}", 0.99f));
  harness.Publish("intent_slots", std::move(intent_slots));

  std::vector<CompanyAudioOutputStruct> outputs(1);
  ASSERT_EQ(harness.EncodeCAbi(&outputs), 0);

  EXPECT_EQ(outputs[0].request_id, 1006u);
  EXPECT_STREQ(outputs[0].transcribed_text, "turn left");
  EXPECT_STREQ(outputs[0].intent_slot_json, "{\"intent\":\"NAV\"}");
}

// 1.7 CrossRerankConverter Purity (Biz 7)
TEST_F(AdapterPurityTest, CrossRerankAdapterPurity) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "rerank.plain.cabi.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "rerank_result.plain.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(
      in_conv, out_conv,
      InputPortBindings({{"raw_request_ids", "raw_request_ids"},
                         {"rerank_queries", "rerank_queries"},
                         {"rerank_candidates", "rerank_candidates"},
                         {"rerank_pairs", "rerank_pairs"}}),
      OutputPortBindings({{"raw_request_ids", "raw_request_ids"},
                          {"ranked_results", "ranked_results"}}));

  const char* passages[] = {"cand0", "cand1"};
  CompanyRerankBatchInputStruct in{};
  in.request_id = 1007;
  in.query_text = "query";
  in.candidate_count = 2;
  in.candidate_passages[0] = passages[0];
  in.candidate_passages[1] = passages[1];

  ASSERT_EQ(harness.DecodeCAbi({&in}), 0);

  RankedTextBatch ranked;
  ranked.emplace_back(0, 0, RankedCandidate("cand1", 0.85f, 1, 1));
  ranked.emplace_back(0, 1, RankedCandidate("cand0", 0.45f, 2, 0));
  harness.Publish("ranked_results", std::move(ranked));

  std::vector<CompanyRerankBatchOutputStruct> outputs(1);
  ASSERT_EQ(harness.EncodeCAbi(&outputs), 0);

  EXPECT_EQ(outputs[0].request_id, 1007u);
  EXPECT_EQ(outputs[0].count, 2);
  EXPECT_FLOAT_EQ(outputs[0].scores[0], 0.85f);
  EXPECT_EQ(outputs[0].sorted_indices[0], 1);
  EXPECT_FLOAT_EQ(outputs[0].scores[1], 0.45f);
  EXPECT_EQ(outputs[0].sorted_indices[1], 0);
}

// 1.8 TranslateConverter Purity (Biz 8)
TEST_F(AdapterPurityTest, TranslateAdapterPurity) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "translate.json.cabi.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "translate.json.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(
      in_conv, out_conv,
      InputPortBindings({{"raw_request_ids", "raw_request_ids"},
                         {"input_sentences", "input_sentences"}}),
      OutputPortBindings({{"raw_request_ids", "raw_request_ids"},
                          {"llm_answers", "llm_answers"}}));

  CompanyEntityInputStruct in{1008, "{\"query\":\"Hello\"}"};
  ASSERT_EQ(harness.DecodeCAbi({&in}), 0);

  TextBatch answers;
  answers.emplace_back(0, 0, "Bonjour");
  harness.Publish("llm_answers", std::move(answers));

  std::vector<CompanyEntityOutputStruct> outputs(1);
  ASSERT_EQ(harness.EncodeCAbi(&outputs), 0);

  EXPECT_EQ(outputs[0].request_id, 1008u);
  EXPECT_EQ(outputs[0].status_code, 0);
  auto parsed = nlohmann::json::parse(outputs[0].entities_json);
  EXPECT_EQ(parsed["translated"], "Bonjour");
}

// =========================================================================
// 2. Contract Invariants and Security Edge Cases
// =========================================================================

TEST_F(AdapterPurityTest, DocQaAdapter_FailClosedWhenMissingOutputs) {
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "doc_answer.plain.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  OutputPortBindings out_bindings({{"raw_request_ids", "raw_request_ids"},
                                   {"llm_answers", "llm_answers"},
                                   {"intent_matches", "intent_matches"},
                                   {"doc_chunk_counts", "doc_chunk_counts"}});

  // Case 1: missing llm_answers
  {
    test::AdapterHarness harness(out_conv, out_bindings);
    harness.Publish("raw_request_ids", std::vector<uint64_t>{1001});
    std::vector<CompanyDocOutputStruct> outputs(1);
    EXPECT_NE(harness.EncodeCAbi(&outputs), 0);
  }

  // Case 2: has llm_answers but missing intent_matches -> MUST fail-closed
  {
    test::AdapterHarness harness(out_conv, out_bindings);
    harness.Publish("raw_request_ids", std::vector<uint64_t>{1001});
    TextBatch answers;
    answers.emplace_back(0, 0, "Some answer");
    harness.Publish("llm_answers", std::move(answers));
    std::vector<CompanyDocOutputStruct> outputs(1);
    EXPECT_EQ(harness.EncodeCAbi(&outputs), COMPANY_ALG_ERR_INVALID_INPUT);
  }

  // Case 3: has intent_matches but missing explicit chunk counts -> MUST
  // fail-closed
  {
    test::AdapterHarness harness(out_conv, out_bindings);
    harness.Publish("raw_request_ids", std::vector<uint64_t>{1001});
    TextBatch answers;
    answers.emplace_back(0, 0, "Some answer");
    harness.Publish("llm_answers", std::move(answers));
    RuleMatchBatch intents;
    intents.emplace_back(0, 0, RuleMatchItem(1, "QA", "", "{}", 0.9f));
    harness.Publish("intent_matches", std::move(intents));
    std::vector<CompanyDocOutputStruct> outputs(1);
    EXPECT_EQ(harness.EncodeCAbi(&outputs), COMPANY_ALG_ERR_INVALID_INPUT);
  }

  // Case 4: all outputs exist but raw_request_ids is absent
  {
    test::AdapterHarness harness(out_conv, out_bindings);
    TextBatch answers;
    answers.emplace_back(0, 0, "Some answer");
    harness.Publish("llm_answers", std::move(answers));
    RuleMatchBatch intents;
    intents.emplace_back(0, 0, RuleMatchItem(1, "QA", "", "{}", 0.9f));
    harness.Publish("intent_matches", std::move(intents));
    Int32Batch chunk_counts;
    chunk_counts.emplace_back(0, 0, 1);
    harness.Publish("doc_chunk_counts", std::move(chunk_counts));
    std::vector<CompanyDocOutputStruct> outputs(1);
    EXPECT_NE(harness.EncodeCAbi(&outputs), 0);
  }
}

TEST_F(AdapterPurityTest,
       ComplianceAuditAdapter_FailClosedWhenMissingStructuredFields) {
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "audit_result.plain.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  OutputPortBindings out_bindings(
      {{"raw_request_ids", "raw_request_ids"},
       {"structured_verdicts", "structured_verdicts"},
       {"matched_policies", "matched_policies"}});

  test::AdapterHarness harness(out_conv, out_bindings);
  harness.Publish("raw_request_ids", std::vector<uint64_t>{1001});

  // structured_verdicts missing required field 'risk_level' -> MUST fail-closed
  StructuredDocumentBatch verdicts;
  nlohmann::json incomplete_obj = {{"only_verdict", "合规"}};
  verdicts.emplace_back(
      0, 0,
      JsonDocumentItem("{}", true, JsonParseStatus::kOk, "", incomplete_obj));
  harness.Publish("structured_verdicts", std::move(verdicts));

  RankedTextBatch policies;
  policies.emplace_back(0, 0, RankedCandidate("Clause", 1.0f, 1));
  harness.Publish("matched_policies", std::move(policies));

  std::vector<CompanyAuditOutputStruct> outputs(1);
  EXPECT_EQ(harness.EncodeCAbi(&outputs), COMPANY_ALG_ERR_INVALID_INPUT);
}

TEST_F(AdapterPurityTest, AuditJoinsRankOneByRequestAndRejectsFallback) {
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "audit_result.plain.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  OutputPortBindings out_bindings(
      {{"raw_request_ids", "raw_request_ids"},
       {"structured_verdicts", "structured_verdicts"},
       {"matched_policies", "matched_policies"}});

  for (const auto parse_status :
       {JsonParseStatus::kOk, JsonParseStatus::kFailed,
        JsonParseStatus::kFallbackApplied}) {
    test::AdapterHarness harness(out_conv, out_bindings);
    harness.Publish("raw_request_ids", std::vector<uint64_t>{100, 200});

    StructuredDocumentBatch verdicts;
    for (uint32_t id : {1u, 0u}) {
      verdicts.emplace_back(
          id, 0,
          JsonDocumentItem("{}", true, parse_status, "",
                           {{"risk_level", "SAFE"}, {"risk_score", 0.1f}}));
    }
    harness.Publish("structured_verdicts", std::move(verdicts));
    harness.Publish("matched_policies",
                    RankedTextBatch{{0, 0, {"req0 first", 1.0f, 1, 1}},
                                    {0, 1, {"req0 second", 0.5f, 2, 2}},
                                    {1, 0, {"req1 first", 1.0f, 1, 1}}});

    std::vector<CompanyAuditOutputStruct> outputs(2);
    const int ret = harness.EncodeCAbi(&outputs);
    if (parse_status == JsonParseStatus::kOk) {
      ASSERT_EQ(ret, 0) << harness.Status().ToString();
      EXPECT_EQ(outputs[1].request_id, 200u);
      EXPECT_STREQ(outputs[1].matched_policy_clause, "req1 first");
    } else {
      EXPECT_NE(ret, 0);
    }
  }
}

TEST_F(AdapterPurityTest, OneToOneResultsRejectDuplicateAndOutOfRangeIds) {
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "keyword.result.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  OutputPortBindings out_bindings({{"raw_request_ids", "raw_request_ids"},
                                   {"rule_matches", "rule_matches"}});

  for (const auto& ids :
       {std::vector<uint32_t>{0, 0}, std::vector<uint32_t>{0, 2}}) {
    test::AdapterHarness harness(out_conv, out_bindings);
    harness.Publish("raw_request_ids", std::vector<uint64_t>{100, 200});
    RuleMatchBatch matches;
    for (auto id : ids) matches.emplace_back(id, 0, RuleMatchItem{});
    harness.Publish("rule_matches", std::move(matches));

    std::vector<CompanyKeywordOutputStruct> outputs(2);
    EXPECT_EQ(harness.EncodeCAbi(&outputs), COMPANY_ALG_ERR_INVALID_INPUT);
  }
}

TEST_F(AdapterPurityTest, ComplianceAuditAdapter_RejectsOversizedChannelName) {
  const auto* in_conv =
      IoConverterRegistry::Instance().FindInputConverter("audit.plain.cabi.v1");
  ASSERT_NE(in_conv, nullptr);

  InputPortBindings in_bindings({{"raw_request_ids", "raw_request_ids"},
                                 {"user_texts", "user_texts"},
                                 {"channel_names", "channel_names"}});

  const std::string valid_channel(256, 'c');
  const std::string oversized_channel(257, 'c');

  // Valid length <= 256
  {
    test::AdapterHarness harness(in_conv, in_bindings);
    CompanyAuditInputStruct in{5001, "test query", valid_channel.c_str()};
    EXPECT_EQ(harness.DecodeCAbi({&in}), COMPANY_ALG_SUCCESS);
  }

  // Oversized length > 256
  {
    test::AdapterHarness harness(in_conv, in_bindings);
    CompanyAuditInputStruct in{5002, "test query", oversized_channel.c_str()};
    EXPECT_EQ(harness.DecodeCAbi({&in}), COMPANY_ALG_ERR_INVALID_INPUT);
  }
}

TEST_F(AdapterPurityTest, VariableDocResultPreservesLongAnswerAndCAbiLimit) {
  const std::string long_answer(5000, 'a');

  AlgContext ctx;
  ctx.Publish("raw_request_ids", std::vector<uint64_t>{10});
  ctx.Publish("llm_answers", TextBatch{{0, 0, long_answer}});
  ctx.Publish("intent_matches",
              RuleMatchBatch{{0, 0, RuleMatchItem(1, "QA", "", "{}", 0.9f)}});
  ctx.Publish("doc_chunk_counts", Int32Batch{{0, 0, 1}});

  // 1. C ABI fixed buffer: sizeof(answer_text) is 1024, must return
  // BUFFER_TOO_SMALL
  const auto* cabi_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "doc_answer.plain.cabi.v1");
  ASSERT_NE(cabi_conv, nullptr);

  CompanyDocOutputStruct cabi_out{};
  void* cabi_ptrs[] = {&cabi_out};
  ExternalOutputBatchView cabi_dest;
  cabi_dest.items = cabi_ptrs;
  cabi_dest.count = 1;
  cabi_dest.capacity = 1;

  OutputPortBindings bindings({{"raw_request_ids", "raw_request_ids"},
                               {"llm_answers", "llm_answers"},
                               {"intent_matches", "intent_matches"},
                               {"doc_chunk_counts", "doc_chunk_counts"}});
  OutputEncodeOptions options;
  options.converter_id = cabi_conv->converter_id;
  size_t written = 0;
  AdapterStatus status;
  int ret = cabi_conv->encode_fn(&ctx, bindings, options, &cabi_dest, &written,
                                 &status);
  EXPECT_EQ(ret, COMPANY_ALG_ERR_BUFFER_TOO_SMALL);

  // 2. Operator variable buffer: capacity = 6000, must succeed and preserve
  // full answer
  const auto* op_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "doc_answer.plain.operator.v1");
  ASSERT_NE(op_conv, nullptr);

  CompanyOperatorDocOutput op_out{};
  std::vector<char> ans_buf(6000);
  CompanyString cs_ans{0, ans_buf.data()};
  op_out.answer_text = &cs_ans;

  std::vector<char> intent_buf(128);
  CompanyString cs_intent{0, intent_buf.data()};
  op_out.intent_name = &cs_intent;

  ExternalOutputBatchView op_dest;
  op_dest.leased_slots["doc_out"].push_back(&op_out);
  op_dest.slot_capacities["doc_out"]["answer_text"] = 6000;
  op_dest.slot_capacities["doc_out"]["intent_name"] = 128;
  op_dest.count = 1;

  options.converter_id = op_conv->converter_id;
  ret =
      op_conv->encode_fn(&ctx, bindings, options, &op_dest, &written, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(op_out.request_id, 10U);
  EXPECT_EQ(std::string(op_out.answer_text->data), long_answer);
}

TEST_F(AdapterPurityTest, InputBatchSkeleton_CopyInPurity) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "translate.json.cabi.v1");
  ASSERT_NE(in_conv, nullptr);

  test::AdapterHarness harness(
      in_conv, InputPortBindings({{"raw_request_ids", "raw_request_ids"},
                                  {"input_sentences", "input_sentences"}}));

  std::string buffer = "{\"query\":\"original query\"}";
  CompanyEntityInputStruct in{5555, buffer.c_str()};

  ASSERT_EQ(harness.DecodeCAbi({&in}), 0);

  // Overwrite external buffer
  buffer[11] = 'X';
  buffer[12] = 'X';

  const auto* sentences = harness.Context().Read<TextBatch>("input_sentences");
  ASSERT_NE(sentences, nullptr);
  EXPECT_EQ((*sentences)[0].data, "original query");
}

TEST_F(AdapterPurityTest, InputBatchSkeleton_ExternalDuplicateIdsAllowed) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "translate.json.cabi.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "translate.json.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(
      in_conv, out_conv,
      InputPortBindings({{"raw_request_ids", "raw_request_ids"},
                         {"input_sentences", "input_sentences"}}),
      OutputPortBindings({{"raw_request_ids", "raw_request_ids"},
                          {"llm_answers", "llm_answers"}}));

  CompanyEntityInputStruct in0{1234, "{\"query\":\"q0\"}"};
  CompanyEntityInputStruct in1{1234, "{\"query\":\"q1\"}"};

  ASSERT_EQ(harness.DecodeCAbi({&in0, &in1}), 0);

  const auto* req_ids =
      harness.Context().Read<std::vector<uint64_t>>("raw_request_ids");
  const auto* sentences = harness.Context().Read<TextBatch>("input_sentences");
  ASSERT_NE(req_ids, nullptr);
  ASSERT_NE(sentences, nullptr);
  EXPECT_EQ((*req_ids)[0], 1234u);
  EXPECT_EQ((*req_ids)[1], 1234u);
  EXPECT_EQ((*sentences)[0].req_id, 0u);
  EXPECT_EQ((*sentences)[1].req_id, 1u);

  TextBatch answers{{0, 0, "ans0"}, {1, 0, "ans1"}};
  harness.Publish("llm_answers", std::move(answers));

  std::vector<CompanyEntityOutputStruct> outputs(2);
  ASSERT_EQ(harness.EncodeCAbi(&outputs), 0);
  EXPECT_EQ(outputs[0].request_id, 1234u);
  EXPECT_EQ(outputs[1].request_id, 1234u);
}

TEST_F(AdapterPurityTest, InputBatchSkeleton_AllSamplesValidatedBeforePublish) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "translate.json.cabi.v1");
  ASSERT_NE(in_conv, nullptr);

  test::AdapterHarness harness(
      in_conv, InputPortBindings({{"raw_request_ids", "raw_request_ids"},
                                  {"input_sentences", "input_sentences"}}));

  CompanyEntityInputStruct in0{1, "{\"query\":\"valid\"}"};
  CompanyEntityInputStruct in1{2, "invalid json"};

  EXPECT_EQ(harness.DecodeCAbi({&in0, &in1}), COMPANY_ALG_ERR_INVALID_INPUT);

  // AlgContext must be completely unpopulated
  EXPECT_EQ(harness.Context().Read<std::vector<uint64_t>>("raw_request_ids"),
            nullptr);
  EXPECT_EQ(harness.Context().Read<TextBatch>("input_sentences"), nullptr);
}

TEST_F(AdapterPurityTest, DocQaAdapter_MultiWayResultsReorderedAndPerturbed) {
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "doc_answer.plain.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  test::AdapterHarness harness(
      out_conv, OutputPortBindings({{"raw_request_ids", "raw_request_ids"},
                                    {"llm_answers", "llm_answers"},
                                    {"intent_matches", "intent_matches"},
                                    {"doc_chunk_counts", "doc_chunk_counts"}}));

  harness.Publish("raw_request_ids", std::vector<uint64_t>{1001, 2002});

  // Perturbed order: index 1 published before index 0
  TextBatch answers{{1, 0, "Answer 1"}, {0, 0, "Answer 0"}};
  RuleMatchBatch intents{{0, 0, RuleMatchItem(1, "INTENT_0", "", "{}", 0.9f)},
                         {1, 0, RuleMatchItem(2, "INTENT_1", "", "{}", 0.8f)}};
  Int32Batch chunks{{0, 0, 3}, {1, 0, 5}};

  harness.Publish("llm_answers", std::move(answers));
  harness.Publish("intent_matches", std::move(intents));
  harness.Publish("doc_chunk_counts", std::move(chunks));

  std::vector<CompanyDocOutputStruct> outputs(2);
  ASSERT_EQ(harness.EncodeCAbi(&outputs), 0);
  EXPECT_EQ(outputs[0].request_id, 1001u);
  EXPECT_STREQ(outputs[0].answer_text, "Answer 0");
  EXPECT_STREQ(outputs[0].intent_name, "INTENT_0");
  EXPECT_EQ(outputs[0].chunk_count, 3);

  EXPECT_EQ(outputs[1].request_id, 2002u);
  EXPECT_STREQ(outputs[1].answer_text, "Answer 1");
  EXPECT_STREQ(outputs[1].intent_name, "INTENT_1");
  EXPECT_EQ(outputs[1].chunk_count, 5);
}

// =========================================================================
// 3. Section 13.1 Independent Reuse Proofs
// =========================================================================

// Proof 1: Input Converters Match Biz Declared Host Types & Prove Reuse via
// Test Binding
TEST_F(AdapterPurityTest, ReuseProof_1_InputConverterReusedAcrossBindings) {
  const auto* entity_binding =
      IoBindingRegistry::Instance().FindBinding("entity_extract.cabi.v1");
  ASSERT_NE(entity_binding, nullptr);
  const auto* keyword_binding =
      IoBindingRegistry::Instance().FindBinding("keyword_match.cabi.v1");
  ASSERT_NE(keyword_binding, nullptr);

  EXPECT_EQ(entity_binding->input_converter_id, "text.plain.cabi.v1");
  EXPECT_EQ(keyword_binding->input_converter_id, "keyword.plain.cabi.v1");

  const auto* entity_conv =
      IoConverterRegistry::Instance().FindInputConverter("text.plain.cabi.v1");
  ASSERT_NE(entity_conv, nullptr);
  EXPECT_EQ(entity_conv->external_type, "CompanyEntityInputStruct");

  const auto* keyword_conv = IoConverterRegistry::Instance().FindInputConverter(
      "keyword.plain.cabi.v1");
  ASSERT_NE(keyword_conv, nullptr);
  EXPECT_EQ(keyword_conv->external_type, "CompanyKeywordInputStruct");

  // Decode input with entity binding: constructs CompanyEntityInputStruct
  {
    test::AdapterHarness harness(
        entity_conv, InputPortBindings(entity_binding->input_ports));
    CompanyEntityInputStruct in{8001, "entity sentence"};
    EXPECT_EQ(harness.DecodeCAbi({&in}), 0);
    const auto* sentences =
        harness.Context().Read<TextBatch>("input_sentences");
    ASSERT_NE(sentences, nullptr);
    EXPECT_EQ((*sentences)[0].data, "entity sentence");
  }

  // Decode input with keyword binding: constructs CompanyKeywordInputStruct
  {
    test::AdapterHarness harness(
        keyword_conv, InputPortBindings(keyword_binding->input_ports));
    CompanyKeywordInputStruct in{8002, "keyword sentence"};
    EXPECT_EQ(harness.DecodeCAbi({&in}), 0);
    const auto* sentences =
        harness.Context().Read<TextBatch>("input_sentences");
    ASSERT_NE(sentences, nullptr);
    EXPECT_EQ((*sentences)[0].data, "keyword sentence");
  }

  // 跨业务复用证明：在测试专用绑定中复用 text.plain.cabi.v1
  {
    IoBindingDefinition test_reuse_binding;
    test_reuse_binding.binding_id = "test_purity_reuse.cabi.v1";
    test_reuse_binding.biz_name = "entity_extract_v1";
    test_reuse_binding.transport = "cabi";
    test_reuse_binding.input_converter_id = "text.plain.cabi.v1";
    test_reuse_binding.output_converter_id = "document.structured.cabi.v1";
    test_reuse_binding.input_ports = entity_binding->input_ports;
    test_reuse_binding.output_ports = entity_binding->output_ports;
    test_reuse_binding.max_batch_size = 64;
    IoBindingRegistry::Instance().RegisterBinding(test_reuse_binding);

    const auto* b_test =
        IoBindingRegistry::Instance().FindBinding("test_purity_reuse.cabi.v1");
    ASSERT_NE(b_test, nullptr);
    EXPECT_EQ(b_test->input_converter_id, entity_binding->input_converter_id);
  }
}

// Proof 2: Output Converter Reused Across Pipelines
TEST_F(AdapterPurityTest, ReuseProof_2_OutputConverterReusedAcrossPipelines) {
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "document.structured.cabi.v1");
  ASSERT_NE(out_conv, nullptr);

  OutputPortBindings bindings({{"raw_request_ids", "raw_request_ids"},
                               {"extracted_entities", "extracted_entities"}});

  // Context A: Entity Extraction pipeline output
  {
    test::AdapterHarness harness(out_conv, bindings);
    harness.Publish("raw_request_ids", std::vector<uint64_t>{9001});
    StructuredDocumentBatch batch;
    batch.emplace_back(
        0, 0,
        JsonDocumentItem("[\"PERSON: Alice\"]", true, JsonParseStatus::kOk));
    harness.Publish("extracted_entities", std::move(batch));

    std::vector<CompanyEntityOutputStruct> outputs(1);
    ASSERT_EQ(harness.EncodeCAbi(&outputs), 0);
    EXPECT_EQ(outputs[0].request_id, 9001U);
    EXPECT_STREQ(outputs[0].entities_json, "[\"PERSON: Alice\"]");
  }

  // Context B: Generic structured JSON pipeline output producing same schema
  {
    test::AdapterHarness harness(out_conv, bindings);
    harness.Publish("raw_request_ids", std::vector<uint64_t>{9002});
    StructuredDocumentBatch batch;
    batch.emplace_back(
        0, 0,
        JsonDocumentItem("{\"summary\":\"ok\"}", true, JsonParseStatus::kOk));
    harness.Publish("extracted_entities", std::move(batch));

    std::vector<CompanyEntityOutputStruct> outputs(1);
    ASSERT_EQ(harness.EncodeCAbi(&outputs), 0);
    EXPECT_EQ(outputs[0].request_id, 9002U);
    EXPECT_STREQ(outputs[0].entities_json, "{\"summary\":\"ok\"}");
  }
}

// Proof 3: Multiple External Input Formats Driving Same Pipeline
TEST_F(AdapterPurityTest,
       ReuseProof_3_MultipleExternalInputFormatsForSamePipeline) {
  // Register custom input converter that converts CustomMultiFieldInput to
  // TextBatch
  InputConverterDefinition custom_in_def;
  custom_in_def.converter_id = "test.multi_field.cabi.v1";
  custom_in_def.transport = "cabi";
  custom_in_def.schema_id = "multi_field.request";
  custom_in_def.schema_version = 1;
  custom_in_def.external_type = "CustomMultiFieldInput";
  custom_in_def.external_slots = {ExternalSlotDefinition(
      "inputs", "CustomMultiFieldInput", PortDirection::kInput, true)};
  custom_in_def.max_batch_size = 64;
  custom_in_def.logical_ports = {
      NodePortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      NodePortDefinition("texts", "TextBatch", true, "1:1")};
  custom_in_def.decode_fn = [](const ExternalInputBatchView& src,
                               const InputDecodeOptions&,
                               const InputPortBindings& bindings,
                               AlgContext* ctx, AdapterStatus*) -> int {
    std::vector<uint64_t> ids;
    TextBatch texts;
    for (size_t i = 0; i < src.count; ++i) {
      const auto* item = src.GetCAbi<CustomMultiFieldInput>(i);
      if (!item) return -3;
      ids.push_back(item->req_id);
      std::string combined =
          std::string(item->topic) + ": " + std::string(item->content);
      texts.emplace_back(static_cast<uint32_t>(i), 0, combined);
    }
    ctx->Publish(bindings.GetActualKey("raw_request_ids"), ids);
    ctx->Publish(bindings.GetActualKey("texts"), texts);
    return 0;
  };

  EXPECT_TRUE(
      IoConverterRegistry::Instance().RegisterInputConverter(custom_in_def));

  // Format A: CompanyEntityInputStruct via text.plain.cabi.v1
  AlgContext ctx_a;
  {
    const auto* in_a = IoConverterRegistry::Instance().FindInputConverter(
        "text.plain.cabi.v1");
    ASSERT_NE(in_a, nullptr);
    CompanyEntityInputStruct req_a{777, "AI: Revolution in robotics"};
    const void* items[] = {&req_a};
    ExternalInputBatchView view;
    view.items = items;
    view.count = 1;
    view.type_id = "CompanyEntityInputStruct";
    InputPortBindings bindings({{"raw_request_ids", "raw_request_ids"},
                                {"input_sentences", "input_sentences"}});
    InputDecodeOptions opts;
    opts.converter_id = in_a->converter_id;
    AdapterStatus st;
    ASSERT_EQ(in_a->decode_fn(view, opts, bindings, &ctx_a, &st), 0);
  }

  // Format B: CustomMultiFieldInput via test.multi_field.cabi.v1
  AlgContext ctx_b;
  {
    const auto* in_b = IoConverterRegistry::Instance().FindInputConverter(
        "test.multi_field.cabi.v1");
    ASSERT_NE(in_b, nullptr);
    CustomMultiFieldInput req_b{777, "AI", "Revolution in robotics"};
    const void* items[] = {&req_b};
    ExternalInputBatchView view;
    view.items = items;
    view.count = 1;
    view.type_id = "CustomMultiFieldInput";
    InputPortBindings bindings(
        {{"raw_request_ids", "raw_request_ids"}, {"texts", "input_sentences"}});
    InputDecodeOptions opts;
    opts.converter_id = in_b->converter_id;
    AdapterStatus st;
    ASSERT_EQ(in_b->decode_fn(view, opts, bindings, &ctx_b, &st), 0);
  }

  // Both produce identical internal TextBatch on "input_sentences"
  const auto* texts_a = ctx_a.Read<TextBatch>("input_sentences");
  const auto* texts_b = ctx_b.Read<TextBatch>("input_sentences");
  ASSERT_NE(texts_a, nullptr);
  ASSERT_NE(texts_b, nullptr);
  EXPECT_EQ((*texts_a)[0].data, (*texts_b)[0].data);
  EXPECT_EQ((*texts_a)[0].data, "AI: Revolution in robotics");
}

// Proof 4: Independently Switch Output Formats for Same Pipeline
TEST_F(AdapterPurityTest, ReuseProof_4_IndependentlySwitchOutputFormat) {
  // Prepare common AlgContext with both structured document and keyword results
  AlgContext ctx;
  ctx.Publish("raw_request_ids", std::vector<uint64_t>{5001});

  StructuredDocumentBatch docs;
  docs.emplace_back(
      0, 0, JsonDocumentItem("[\"item_1\"]", true, JsonParseStatus::kOk));
  ctx.Publish("extracted_entities", std::move(docs));

  RuleMatchBatch rules;
  rules.emplace_back(
      0, 0, RuleMatchItem(1, "URGENT", "急", "{\"flag\":\"urgent\"}", 0.99f));
  ctx.Publish("rule_matches", std::move(rules));

  // Binding Output A: document.structured.cabi.v1 -> CompanyEntityOutputStruct
  {
    const auto* out_a = IoConverterRegistry::Instance().FindOutputConverter(
        "document.structured.cabi.v1");
    ASSERT_NE(out_a, nullptr);
    CompanyEntityOutputStruct out{};
    void* items[] = {&out};
    ExternalOutputBatchView dest;
    dest.items = items;
    dest.count = 1;
    dest.capacity = 1;
    dest.type_id = out_a->external_type;
    OutputPortBindings bindings({{"raw_request_ids", "raw_request_ids"},
                                 {"extracted_entities", "extracted_entities"}});
    OutputEncodeOptions opts;
    opts.converter_id = out_a->converter_id;
    size_t written = 0;
    AdapterStatus st;
    ASSERT_EQ(out_a->encode_fn(&ctx, bindings, opts, &dest, &written, &st), 0);
    EXPECT_EQ(out.request_id, 5001U);
    EXPECT_STREQ(out.entities_json, "[\"item_1\"]");
  }

  // Binding Output B: keyword.result.cabi.v1 -> CompanyKeywordOutputStruct
  {
    const auto* out_b = IoConverterRegistry::Instance().FindOutputConverter(
        "keyword.result.cabi.v1");
    ASSERT_NE(out_b, nullptr);
    CompanyKeywordOutputStruct out{};
    void* items[] = {&out};
    ExternalOutputBatchView dest;
    dest.items = items;
    dest.count = 1;
    dest.capacity = 1;
    dest.type_id = out_b->external_type;
    OutputPortBindings bindings({{"raw_request_ids", "raw_request_ids"},
                                 {"rule_matches", "rule_matches"}});
    OutputEncodeOptions opts;
    opts.converter_id = out_b->converter_id;
    size_t written = 0;
    AdapterStatus st;
    ASSERT_EQ(out_b->encode_fn(&ctx, bindings, opts, &dest, &written, &st), 0);
    EXPECT_EQ(out.request_id, 5001U);
    EXPECT_EQ(out.is_hit, 1);
    EXPECT_STREQ(out.match_result_json, "{\"flag\":\"urgent\"}");
  }
}

// Proof 5: Same Carrier with Different Schemas
TEST_F(AdapterPurityTest, ReuseProof_5_SameCarrierDifferentSchema) {
  const auto* plain_conv =
      IoConverterRegistry::Instance().FindInputConverter("text.plain.cabi.v1");
  ASSERT_NE(plain_conv, nullptr);
  const auto* json_conv = IoConverterRegistry::Instance().FindInputConverter(
      "translate.json.cabi.v1");
  ASSERT_NE(json_conv, nullptr);

  // Payload 1: Pure plain text "Hello plain text"
  CompanyEntityInputStruct plain_req{101, "Hello plain text"};
  const void* plain_items[] = {&plain_req};
  ExternalInputBatchView plain_view;
  plain_view.items = plain_items;
  plain_view.count = 1;
  plain_view.type_id = "CompanyEntityInputStruct";

  InputPortBindings bindings({{"raw_request_ids", "raw_request_ids"},
                              {"input_sentences", "input_sentences"}});
  InputDecodeOptions opts;

  // text.plain.cabi.v1 accepts it as plain text
  {
    AlgContext ctx;
    AdapterStatus st;
    opts.converter_id = plain_conv->converter_id;
    EXPECT_EQ(plain_conv->decode_fn(plain_view, opts, bindings, &ctx, &st), 0);
    const auto* s = ctx.Read<TextBatch>("input_sentences");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ((*s)[0].data, "Hello plain text");
  }

  // translate.json.cabi.v1 rejects it because it is not JSON
  {
    AlgContext ctx;
    AdapterStatus st;
    opts.converter_id = json_conv->converter_id;
    EXPECT_EQ(json_conv->decode_fn(plain_view, opts, bindings, &ctx, &st),
              COMPANY_ALG_ERR_INVALID_INPUT);
    EXPECT_EQ(st.FieldPath(), "json");
  }

  // Payload 2: JSON formatted string "{\"query\": \"Hello JSON\"}"
  CompanyEntityInputStruct json_req{102, "{\"query\": \"Hello JSON\"}"};
  const void* json_items[] = {&json_req};
  ExternalInputBatchView json_view;
  json_view.items = json_items;
  json_view.count = 1;
  json_view.type_id = "CompanyEntityInputStruct";

  // translate.json.cabi.v1 succeeds and extracts "query"
  {
    AlgContext ctx;
    AdapterStatus st;
    opts.converter_id = json_conv->converter_id;
    EXPECT_EQ(json_conv->decode_fn(json_view, opts, bindings, &ctx, &st), 0);
    const auto* s = ctx.Read<TextBatch>("input_sentences");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ((*s)[0].data, "Hello JSON");
  }
}

// Proof 6: Negative Combinations Rejected
TEST_F(AdapterPurityTest, ReuseProof_6_NegativeCombinations) {
  // 1. Unknown or unregistered io_binding
  DeploymentIoConfig bad_binding_cfg;
  bad_binding_cfg.io_binding = "non_existent.binding.v999";
  bad_binding_cfg.pipe_path = "pipeline_keyword_match_rules.json";
  std::unique_ptr<ValidatedIoPlan> plan;
  std::string error;
  int ret = IoBindingResolver::ResolveFromConfig(bad_binding_cfg, "cabi",
                                                 "./models", &plan, &error);
  EXPECT_EQ(ret, -2);
  EXPECT_NE(error.find("Unknown or unregistered io_binding"),
            std::string::npos);

  // 2. Transport mismatch: CABI requested, but operator binding specified
  DeploymentIoConfig mismatch_cfg;
  mismatch_cfg.io_binding = "keyword_match.operator.v1";
  mismatch_cfg.pipe_path = "pipeline_keyword_match_rules.json";
  ret = IoBindingResolver::ResolveFromConfig(mismatch_cfg, "cabi", "./models",
                                             &plan, &error);
  EXPECT_EQ(ret, -2);
  EXPECT_NE(error.find("Binding transport mismatch"), std::string::npos);

  // 3. DeploymentIoConfig schema validation rejects invalid version
  nlohmann::json invalid_version_json = {
      {"schema_version", 999},
      {"data",
       {{"pipe_path", "test.json"}, {"io_binding", "keyword_match.cabi.v1"}}}};
  DeploymentIoConfig parsed_cfg;
  EXPECT_FALSE(DeploymentIoConfig::Parse(invalid_version_json, ".", "cabi",
                                         &parsed_cfg, &error));
  EXPECT_NE(error.find("schema_version"), std::string::npos);

  // 4. CABI config rejects outputs block
  nlohmann::json cabi_with_outputs_json = {
      {"schema_version", 1},
      {"data",
       {{"pipe_path", "test.json"},
        {"io_binding", "keyword_match.cabi.v1"},
        {"outputs", {{"main", {{"type", "test"}}}}}}}};
  EXPECT_FALSE(DeploymentIoConfig::Parse(cabi_with_outputs_json, ".", "cabi",
                                         &parsed_cfg, &error));
  EXPECT_NE(error.find("outputs"), std::string::npos);

  // 5. Operator config with unknown output slot rejected by parity check
  DeploymentIoConfig unknown_out_cfg;
  unknown_out_cfg.io_binding = "keyword_match.operator.v1";
  unknown_out_cfg.pipe_path = "pipeline_keyword_match_rules.json";
  unknown_out_cfg.outputs = {{"unknown_slot", {{"type", "String"}}}};
  ret = IoBindingResolver::ResolveFromConfig(unknown_out_cfg, "operator",
                                             "./models", &plan, &error);
  EXPECT_EQ(ret, -2);
  EXPECT_NE(error.find("Unknown configured output slot: unknown_slot"),
            std::string::npos);

  // 6. Unknown model_id in model_paths rejected
  DeploymentIoConfig unknown_mid_cfg;
  unknown_mid_cfg.io_binding = "keyword_match.cabi.v1";
  unknown_mid_cfg.pipe_path = "configs/pipeline_keyword_match_rules.json";
  unknown_mid_cfg.resolved_pipe_path =
      "configs/pipeline_keyword_match_rules.json";
  unknown_mid_cfg.model_paths = {{"non_existent_model", "dummy_path"}};
  ret = IoBindingResolver::ResolveFromConfig(unknown_mid_cfg, "cabi",
                                             "./models", &plan, &error);
  EXPECT_EQ(ret, -2);
  EXPECT_NE(
      error.find("Unknown model_id 'non_existent_model' in 'model_paths'"),
      std::string::npos);
}

// Proof 7: Validation Before Initialization (probe model not loaded on invalid
// binding)
TEST_F(AdapterPurityTest, ReuseProof_7_ValidationBeforeInitialization) {
  // Attempt to create algorithm instance with non-existent or invalid binding
  // configuration
  CompanyAlgParamCreate param{};
  param.config_file_path = "non_existent_path.json";
  param.model_root_dir = "./models";
  param.device_id = 0;

  void* handle = nullptr;
  int ret = Alg_Create(&handle, &param);
  EXPECT_NE(ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(handle, nullptr);
}

}  // namespace llm_edgeflow
