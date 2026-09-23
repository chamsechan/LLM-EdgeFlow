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

template <typename Host>
AdapterStatus DecodeSentence(const Host& input, std::string* text) {
  if (!IsValidInputString(input.sentence_text)) {
    return AdapterStatus::InvalidInput(
        "sentence_text string pointer is null or invalid", "sentence_text");
  }
  if (static_cast<size_t>(input.sentence_text->length) > kMaxSentenceLen) {
    return AdapterStatus::InvalidInput(
        "sentence_text length exceeds 64 KiB limit", "sentence_text");
  }
  *text = CopyInputString(*input.sentence_text);
  return AdapterStatus::Ok();
}

int DecodeOperatorEntityInput(const ExternalInputBatchView& source,
                              const InputDecodeOptions& options,
                              const InputPortBindings& bindings,
                              AlgContext* context, AdapterStatus* status) {
  return DecodeRequestRows<CompanyOperatorEntityInput>(
      source, options, bindings, context, status, kMaxBatchSize, "entity_in",
      kRawRequestIds, kInputSentences,
      &DecodeSentence<CompanyOperatorEntityInput>);
}

int DecodeOperatorKeywordInput(const ExternalInputBatchView& source,
                               const InputDecodeOptions& options,
                               const InputPortBindings& bindings,
                               AlgContext* context, AdapterStatus* status) {
  return DecodeRequestRows<CompanyOperatorKeywordInput>(
      source, options, bindings, context, status, kMaxBatchSize, "keyword_in",
      kRawRequestIds, kInputSentences,
      &DecodeSentence<CompanyOperatorKeywordInput>);
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
