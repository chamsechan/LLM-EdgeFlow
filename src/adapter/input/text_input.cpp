#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "contracts/inference_payloads.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

constexpr size_t kMaxBatchSize = 64;

constexpr size_t kMaxSentenceLen = 64 * 1024;  // 64 KiB

template <typename T>
int DecodeSentenceInput(const ExternalInputBatchView& source,
                        const InputDecodeOptions& options,
                        const InputPortBindings& bindings, AlgContext* context,
                        AdapterStatus* status, const char* slot_name) {
  if (!ValidateDecodeRequest(source, options, context, kMaxBatchSize, status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  std::vector<uint64_t> req_ids;
  TextBatch sentences;
  req_ids.reserve(source.count);
  sentences.reserve(source.count);

  for (size_t i = 0; i < source.count; ++i) {
    const auto* in = ReadInputSlot<T>(source, slot_name, i, options, status);
    if (!in) return COMPANY_ALG_ERR_INVALID_INPUT;
    if (!IsValidInputString(in->sentence_text)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "sentence_text string pointer is null or invalid",
          "sentence_text", options.converter_id.c_str(), static_cast<int>(i));
    }
    if (static_cast<size_t>(in->sentence_text->length) > kMaxSentenceLen) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "sentence_text length exceeds 64 KiB limit", "sentence_text",
          options.converter_id.c_str(), static_cast<int>(i));
    }
    std::string text = CopyInputString(*in->sentence_text);
    req_ids.push_back(in->request_id);
    sentences.emplace_back(static_cast<uint32_t>(i), 0, std::move(text));
  }

  if (!AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key(kRawRequestIds), std::move(req_ids),
          options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key(kInputSentences), std::move(sentences),
          options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

int DecodeOperatorEntityInput(const ExternalInputBatchView& source,
                              const InputDecodeOptions& options,
                              const InputPortBindings& bindings,
                              AlgContext* context, AdapterStatus* status) {
  return DecodeSentenceInput<CompanyOperatorEntityInput>(
      source, options, bindings, context, status, "entity_in");
}

int DecodeOperatorKeywordInput(const ExternalInputBatchView& source,
                               const InputDecodeOptions& options,
                               const InputPortBindings& bindings,
                               AlgContext* context, AdapterStatus* status) {
  return DecodeSentenceInput<CompanyOperatorKeywordInput>(
      source, options, bindings, context, status, "keyword_in");
}

InputConverterDefinition MakeOperatorEntityInputConverter() {
  InputConverterDefinition def;
  def.converter_id = "text.plain.operator.v1";

  def.schema_id = "text.plain.request";
  def.external_type = "CompanyOperatorEntityInput";
  def.max_batch_size = kMaxBatchSize;

  def.external_slots = {
      ExternalInputSlot<CompanyOperatorEntityInput>("entity_in", "entity_in")};
  def.logical_ports = {OutputPort(kRawRequestIds), OutputPort(kInputSentences)};
  def.decode_fn = &DecodeOperatorEntityInput;
  return def;
}

InputConverterDefinition MakeOperatorKeywordInputConverter() {
  InputConverterDefinition def;
  def.converter_id = "keyword.plain.operator.v1";

  def.schema_id = "text.plain.request";
  def.external_type = "CompanyOperatorKeywordInput";
  def.max_batch_size = kMaxBatchSize;

  def.external_slots = {ExternalInputSlot<CompanyOperatorKeywordInput>(
      "keyword_in", "keyword_in")};
  def.logical_ports = {OutputPort(kRawRequestIds), OutputPort(kInputSentences)};
  def.decode_fn = &DecodeOperatorKeywordInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorEntityInputConverter());
REGISTER_INPUT_CONVERTER(MakeOperatorKeywordInputConverter());

}  // namespace
}  // namespace llm_edgeflow
