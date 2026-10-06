#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/biz_input_constraints.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "contracts/inference_payloads.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kEntitySlot = "entity_in";
constexpr const char* kKeywordSlot = "keyword_in";

template <typename Host>
AdapterStatus DecodeSentence(const Host& input, std::string* text) {
  if (!IsValidInputString(input.sentence_text)) {
    return AdapterStatus::InvalidInput(
        "sentence_text string pointer is null or invalid", "sentence_text");
  }
  if (static_cast<size_t>(input.sentence_text->length) >
      biz_input::kMaxTextBytes) {
    return AdapterStatus::InvalidInput(
        "sentence_text length exceeds 64 KiB limit", "sentence_text");
  }
  *text = CopyInputString(*input.sentence_text);
  return AdapterStatus::Ok();
}

int DecodeOperatorEntityInput(const ExternalInputBatchView& source,
                              const InputDecodeOptions& options,
                              AlgContext* context, AdapterStatus* status) {
  return DecodeRequestRows<CompanyOperatorEntityInput>(
      source, options, context, status, kEntitySlot, kInputSentences,
      &DecodeSentence<CompanyOperatorEntityInput>);
}

int DecodeOperatorKeywordInput(const ExternalInputBatchView& source,
                               const InputDecodeOptions& options,
                               AlgContext* context, AdapterStatus* status) {
  return DecodeRequestRows<CompanyOperatorKeywordInput>(
      source, options, context, status, kKeywordSlot, kInputSentences,
      &DecodeSentence<CompanyOperatorKeywordInput>);
}

InputConverterDefinition MakeOperatorEntityInputConverter() {
  InputConverterDefinition def;
  def.converter_id = "text.plain";
  def.external_slots = {
      ExternalInputSlot<CompanyOperatorEntityInput>(kEntitySlot)};
  def.logical_ports = {OutputPort(kInputSentences)};
  def.decode_fn = &DecodeOperatorEntityInput;
  return def;
}

InputConverterDefinition MakeOperatorKeywordInputConverter() {
  InputConverterDefinition def;
  def.converter_id = "keyword.plain";
  def.external_slots = {
      ExternalInputSlot<CompanyOperatorKeywordInput>(kKeywordSlot)};
  def.logical_ports = {OutputPort(kInputSentences)};
  def.decode_fn = &DecodeOperatorKeywordInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorEntityInputConverter());
REGISTER_INPUT_CONVERTER(MakeOperatorKeywordInputConverter());

}  // namespace
}  // namespace llm_edgeflow
