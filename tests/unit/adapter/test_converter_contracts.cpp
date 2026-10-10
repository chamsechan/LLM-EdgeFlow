#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "adapter/platform_value_binding.h"

namespace llm_edgeflow {
namespace {
struct ExpectedSize {
  const char* field;
  int64_t default_value;
  uint32_t maximum;
};
struct ExpectedConverter {
  const char* type;
  const char* name;
  const char* host;
  std::optional<int32_t> service;
  std::vector<std::pair<std::string, std::string>> ports;
  std::vector<ExpectedSize> sizes;
};

template <typename Definition>
void ExpectDefinition(const Definition& definition,
                      const ExpectedConverter& expected) {
  EXPECT_EQ(definition.type, expected.type);
  EXPECT_EQ(definition.name, expected.name);
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix(definition.type);
  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->ServiceType(definition.name), expected.service);
  EXPECT_EQ(binding->external_c_type_name, expected.host);
  EXPECT_EQ(definition.slot.type_suffix, expected.type);
  EXPECT_TRUE(definition.slot.required);
  EXPECT_TRUE(definition.slot.allocator.empty());
  EXPECT_TRUE(definition.slot.allocator_params.empty());
  EXPECT_EQ(definition.slot.metadata_count, 0u);
  EXPECT_EQ(definition.slot.metadata_type_id, 0);
  std::vector<std::pair<std::string, std::string>> ports;
  for (const auto& port : definition.logical_ports)
    ports.emplace_back(port.logical_name, port.type_id);
  EXPECT_EQ(ports, expected.ports);
  EXPECT_EQ(definition.params.Fields().size(), expected.sizes.size());
}
}  // namespace

TEST(ConverterContractsTest, NineProductionInputContracts) {
  const std::vector<ExpectedConverter> expected = {
      {"audio_in",
       "audio_asr_intent",
       "CompanyOperatorAudioInput",
       kMockServiceAudioAsrIntent,
       {{"audio", "AudioPcmBatch"}},
       {}},
      {"audit_in",
       "dialogue_audit",
       "CompanyOperatorAuditInput",
       kMockServiceDialogueAudit,
       {{"user_text", "TextBatch"}, {"channel_name", "TextBatch"}},
       {}},
      {"doc_in",
       "doc_qa",
       "CompanyOperatorDocInput",
       kMockServiceDocQa,
       {{"doc_text", "TextBatch"}, {"query_text", "TextBatch"}},
       {}},
      {"entity_in",
       "entity_extract",
       "CompanyOperatorEntityInput",
       kMockServiceEntityExtract,
       {{"sentence_text", "TextBatch"}},
       {}},
      {"entity_in",
       "translate",
       "CompanyOperatorEntityInput",
       kMockServiceTranslate,
       {{"query", "TextBatch"}},
       {}},
      {"frame",
       "ocr_invoice_qa",
       "CompanyFrame",
       kMockServiceOcrInvoiceQa,
       {{"image", "ImageFrameBatch"}},
       {}},
      {"keyword_in",
       "keyword_match",
       "CompanyOperatorKeywordInput",
       kMockServiceKeywordMatch,
       {{"sentence_text", "TextBatch"}},
       {}},
      {"rerank_in",
       "cross_rerank",
       "CompanyOperatorRerankInput",
       kMockServiceCrossRerank,
       {{"query_text", "TextBatch"}, {"candidates", "RankedTextBatch"}},
       {}},
      {"string",
       "ocr_invoice_qa",
       "CompanyString",
       std::nullopt,
       {{"question", "TextBatch"}},
       {}}};
  const auto& registry = IoConverterRegistry::Instance();
  for (const auto& entry : expected) {
    SCOPED_TRACE(std::string(entry.type) + "/" + entry.name);
    const auto* definition =
        registry.FindInputConverter(entry.type, entry.name);
    ASSERT_NE(definition, nullptr);
    ASSERT_NE(definition->decode_fn, nullptr);
    ExpectDefinition(*definition, entry);
  }
}

TEST(ConverterContractsTest, EightProductionOutputContractsAndSizeLimits) {
  const std::vector<ExpectedConverter> expected = {
      {"audio_out",
       "audio_asr_intent",
       "CompanyOperatorAudioOutput",
       kMockServiceAudioAsrIntent,
       {{"transcribed_text", "TextBatch"}, {"intent_slot", "RuleMatchBatch"}},
       {{"transcribed_text", 511, 16384}, {"intent_slot_json", 1023, 65536}}},
      {"audit_out",
       "dialogue_audit",
       "CompanyOperatorAuditOutput",
       kMockServiceDialogueAudit,
       {{"verdict", "StructuredDocumentBatch"},
        {"matched_policy", "RankedTextBatch"}},
       {{"risk_level", 31, 255},
        {"matched_policy_clause", 255, 4096},
        {"audit_verdict_json", 1023, 65536}}},
      {"doc_out",
       "doc_qa",
       "CompanyOperatorDocOutput",
       kMockServiceDocQa,
       {{"answer_text", "TextBatch"},
        {"intent", "RuleMatchBatch"},
        {"chunk_count", "Int32Batch"}},
       {{"intent_name", 63, 255}, {"answer_text", 1023, 65536}}},
      {"entity_out",
       "entity_extract",
       "CompanyOperatorEntityOutput",
       kMockServiceEntityExtract,
       {{"entities", "StructuredDocumentBatch"}},
       {{"entities_json", 2047, 65536}}},
      {"entity_out",
       "translate",
       "CompanyOperatorEntityOutput",
       kMockServiceTranslate,
       {{"translation", "TextBatch"}},
       {{"entities_json", 8191, 65536}}},
      {"keyword_out",
       "keyword_match",
       "CompanyOperatorKeywordOutput",
       kMockServiceKeywordMatch,
       {{"matches", "RuleMatchBatch"}},
       {{"match_result_json", 2047, 65536}}},
      {"od_out",
       "ocr_invoice_qa",
       "CompanyOdOutput",
       kMockServiceOcrInvoiceQa,
       {{"result", "StructuredDocumentBatch"},
        {"document", "OcrDocumentBatch"}},
       {{"result_json", 2047, 65536}}},
      {"rerank_out",
       "cross_rerank",
       "CompanyOperatorRerankOutput",
       kMockServiceCrossRerank,
       {{"ranked", "RankedTextBatch"}},
       {}}};
  const auto& registry = IoConverterRegistry::Instance();
  for (const auto& entry : expected) {
    SCOPED_TRACE(std::string(entry.type) + "/" + entry.name);
    const auto* definition =
        registry.FindOutputConverter(entry.type, entry.name);
    ASSERT_NE(definition, nullptr);
    ASSERT_NE(definition->encode_fn, nullptr);
    ExpectDefinition(*definition, entry);
    const auto* platform =
        OperatorValueTypeRegistry::Instance().GetOutputBinding(entry.type, "");
    ASSERT_NE(platform, nullptr);
    EXPECT_EQ(platform->output_layout.string_capacity_fields.size(),
              entry.sizes.size());
    const auto fields = OutputConverterParameterFields(*definition);
    std::map<std::string, ConfigFieldDefinition> by_name;
    for (const auto& field : fields) by_name.emplace(field.name, field);
    for (const auto& size : entry.sizes) {
      const auto parameter = std::string(size.field) + "_max_bytes";
      ASSERT_EQ(by_name.count(parameter), 1u);
      const auto& field = by_name.at(parameter);
      EXPECT_EQ(field.kind, ConfigValueKind::kInteger);
      EXPECT_EQ(field.default_value, size.default_value);
      EXPECT_EQ(field.minimum, std::optional<double>{1});
      EXPECT_EQ(field.maximum,
                std::optional<double>{static_cast<double>(size.maximum)});
      EXPECT_FALSE(field.required);
      EXPECT_EQ(platform->output_layout.string_capacity_fields.at(size.field)
                    .max_capacity,
                size.maximum);
    }
  }
}
}  // namespace llm_edgeflow
