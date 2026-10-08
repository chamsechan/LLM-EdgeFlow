#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "adapter/io_converter_registry.h"
#include "platform_mock/operator_data_types.h"

namespace llm_edgeflow {
namespace {

struct ExpectedParam {
  std::string name;
  int64_t default_value;
};

struct ExpectedConverter {
  std::string type;
  std::string name;
  std::optional<int32_t> service_type;
  std::string struct_name;
  std::vector<std::pair<std::string, std::string>> ports;  // 名字、类型
  std::vector<ExpectedParam> params;  // 只有输出项有尺寸参数
};

// 生产登记的外部契约：变更其中任何一项都意味着对外契约变化，需要同步修改这里。
// service_type 是平台模拟的占位取值，进内网后按真实头文件核对。
const std::vector<ExpectedConverter>& ExpectedInputs() {
  static const std::vector<ExpectedConverter> kInputs = {
      {"audio_in",
       "audio_asr_intent",
       COMPANY_MOCK_SERVICE_AUDIO_ASR_INTENT,
       "CompanyOperatorAudioInput",
       {{"audio_inputs", "AudioPcmBatch"}},
       {}},
      {"audit_in",
       "dialogue_audit",
       COMPANY_MOCK_SERVICE_DIALOGUE_AUDIT,
       "CompanyOperatorAuditInput",
       {{"user_texts", "TextBatch"}, {"channel_names", "TextBatch"}},
       {}},
      {"doc_in",
       "doc_qa",
       COMPANY_MOCK_SERVICE_DOC_QA,
       "CompanyOperatorDocInput",
       {{"raw_docs", "TextBatch"}, {"raw_queries", "TextBatch"}},
       {}},
      {"entity_in",
       "entity_extract",
       COMPANY_MOCK_SERVICE_ENTITY_EXTRACT,
       "CompanyOperatorEntityInput",
       {{"input_sentences", "TextBatch"}},
       {}},
      {"entity_in",
       "translate",
       COMPANY_MOCK_SERVICE_TRANSLATE,
       "CompanyOperatorEntityInput",
       {{"input_sentences", "TextBatch"}},
       {}},
      {"frame",
       "ocr_invoice_qa",
       COMPANY_MOCK_SERVICE_OCR_INVOICE_QA,
       "CompanyFrame",
       {{"image_paths", "ImageRefBatch"}},
       {}},
      {"keyword_in",
       "keyword_match",
       COMPANY_MOCK_SERVICE_KEYWORD_MATCH,
       "CompanyOperatorKeywordInput",
       {{"input_sentences", "TextBatch"}},
       {}},
      {"rerank_in",
       "cross_rerank",
       COMPANY_MOCK_SERVICE_CROSS_RERANK,
       "CompanyOperatorRerankInput",
       {{"rerank_queries", "TextBatch"},
        {"rerank_candidates", "RankedTextBatch"},
        {"rerank_pairs", "QueryCandidatesBatch"}},
       {}},
      // CompanyString 没有 service_type 成员，登记不填。
      {"string",
       "ocr_invoice_qa",
       std::nullopt,
       "CompanyString",
       {{"user_queries", "TextBatch"}},
       {}},
  };
  return kInputs;
}

const std::vector<ExpectedConverter>& ExpectedOutputs() {
  static const std::vector<ExpectedConverter> kOutputs = {
      {"audio_out",
       "audio_asr_intent",
       COMPANY_MOCK_SERVICE_AUDIO_ASR_INTENT,
       "CompanyOperatorAudioOutput",
       {{"transcripts", "TextBatch"}, {"intent_slots", "RuleMatchBatch"}},
       {{"transcribed_text_max_bytes", 511},
        {"intent_slot_json_max_bytes", 1023}}},
      {"audit_out",
       "dialogue_audit",
       COMPANY_MOCK_SERVICE_DIALOGUE_AUDIT,
       "CompanyOperatorAuditOutput",
       {{"structured_verdicts", "StructuredDocumentBatch"},
        {"matched_policy", "RankedTextBatch"}},
       {{"risk_level_max_bytes", 31},
        {"matched_policy_clause_max_bytes", 255},
        {"audit_verdict_json_max_bytes", 1023}}},
      {"doc_out",
       "doc_qa",
       COMPANY_MOCK_SERVICE_DOC_QA,
       "CompanyOperatorDocOutput",
       {{"llm_answers", "TextBatch"},
        {"intent_matches", "RuleMatchBatch"},
        {"doc_chunk_counts", "Int32Batch"}},
       {{"intent_name_max_bytes", 63}, {"answer_text_max_bytes", 1023}}},
      {"entity_out",
       "entity_extract",
       COMPANY_MOCK_SERVICE_ENTITY_EXTRACT,
       "CompanyOperatorEntityOutput",
       {{"extracted_entities", "StructuredDocumentBatch"}},
       {{"entities_json_max_bytes", 2047}}},
      {"entity_out",
       "translate",
       COMPANY_MOCK_SERVICE_TRANSLATE,
       "CompanyOperatorEntityOutput",
       {{"llm_answers", "TextBatch"}},
       {{"entities_json_max_bytes", 8191}}},
      {"keyword_out",
       "keyword_match",
       COMPANY_MOCK_SERVICE_KEYWORD_MATCH,
       "CompanyOperatorKeywordOutput",
       {{"rule_matches", "RuleMatchBatch"}},
       {{"match_result_json_max_bytes", 2047}}},
      {"od_out",
       "ocr_invoice_qa",
       COMPANY_MOCK_SERVICE_OCR_INVOICE_QA,
       "CompanyOdOutput",
       {{"extracted_invoice_json", "StructuredDocumentBatch"},
        {"ocr_docs", "OcrDocumentBatch"}},
       {{"result_json_max_bytes", 2047}}},
      // 结构体没有字符串字段，因此没有尺寸参数。
      {"rerank_out",
       "cross_rerank",
       COMPANY_MOCK_SERVICE_CROSS_RERANK,
       "CompanyOperatorRerankOutput",
       {{"ranked_results", "RankedTextBatch"}},
       {}},
  };
  return kOutputs;
}

template <typename Definition>
void ExpectMatches(const Definition& actual,
                   const ExpectedConverter& expected) {
  const std::string label = expected.type + "/" + expected.name;
  EXPECT_EQ(actual.type, expected.type) << label;
  EXPECT_EQ(actual.name, expected.name) << label;
  EXPECT_EQ(actual.service_type, expected.service_type) << label;
  EXPECT_EQ(actual.slot.type_id, expected.struct_name) << label;
  EXPECT_EQ(actual.slot.type_suffix, expected.type) << label;

  ASSERT_EQ(actual.logical_ports.size(), expected.ports.size()) << label;
  for (size_t i = 0; i < expected.ports.size(); ++i) {
    EXPECT_EQ(actual.logical_ports[i].Name(), expected.ports[i].first) << label;
    EXPECT_EQ(actual.logical_ports[i].type_id, expected.ports[i].second)
        << label << " port " << expected.ports[i].first;
  }

  const auto& fields = actual.params.Fields();
  ASSERT_EQ(fields.size(), expected.params.size()) << label;
  for (size_t i = 0; i < expected.params.size(); ++i) {
    EXPECT_EQ(fields[i].name, expected.params[i].name) << label;
    EXPECT_EQ(fields[i].default_value, expected.params[i].default_value)
        << label << " " << expected.params[i].name;
  }
}

}  // namespace

TEST(ConverterContractsTest, ProductionInputConvertersAreExactlyTheContract) {
  auto& registry = IoConverterRegistry::Instance();
  const auto all = registry.AllInputConverters();
  EXPECT_EQ(all.size(), ExpectedInputs().size());
  for (const auto& expected : ExpectedInputs()) {
    SCOPED_TRACE("in " + expected.type + "/" + expected.name);
    const auto* found =
        registry.FindInputConverter(expected.type, expected.name);
    ASSERT_NE(found, nullptr);
    ExpectMatches(*found, expected);
    EXPECT_NE(found->decode_fn, nullptr);
  }
}

TEST(ConverterContractsTest, ProductionOutputConvertersAreExactlyTheContract) {
  auto& registry = IoConverterRegistry::Instance();
  const auto all = registry.AllOutputConverters();
  EXPECT_EQ(all.size(), ExpectedOutputs().size());
  for (const auto& expected : ExpectedOutputs()) {
    SCOPED_TRACE("out " + expected.type + "/" + expected.name);
    const auto* found =
        registry.FindOutputConverter(expected.type, expected.name);
    ASSERT_NE(found, nullptr);
    ExpectMatches(*found, expected);
    EXPECT_NE(found->encode_fn, nullptr);
  }
}

TEST(ConverterContractsTest, ProductionConvertersPassTheRegistryAudit) {
  auto& registry = IoConverterRegistry::Instance();
  std::vector<std::string> errors;
  EXPECT_FALSE(registry.HasConflict());
  EXPECT_TRUE(registry.Audit(&errors));
  EXPECT_TRUE(errors.empty());
  // 17 个生产登记：9 个输入 + 8 个输出。
  EXPECT_EQ(registry.AllInputConverters().size() +
                registry.AllOutputConverters().size(),
            17U);
}

}  // namespace llm_edgeflow
