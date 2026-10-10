#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_values.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"

namespace llm_edgeflow {
namespace {

constexpr auto kSentenceText = MakeBlackboardKey<TextBatch>("sentence_text");

constexpr const char* kEntitySlot = "entity_in";
constexpr const char* kKeywordSlot = "keyword_in";

AdapterStatus DecodeSentence(const TextInputValue& input, std::string* text) {
  *text = input.sentence_text;
  return AdapterStatus::Ok();
}

int DecodeOperatorEntityInput(const ExternalInputBatchView& source,
                              const InputDecodeOptions& options,
                              AlgContext* context, AdapterStatus* status) {
  return DecodeRequestRows<TextInputValue>(source, options, context, status,
                                           kEntitySlot, kSentenceText,
                                           &DecodeSentence);
}

int DecodeOperatorKeywordInput(const ExternalInputBatchView& source,
                               const InputDecodeOptions& options,
                               AlgContext* context, AdapterStatus* status) {
  return DecodeRequestRows<TextInputValue>(source, options, context, status,
                                           kKeywordSlot, kSentenceText,
                                           &DecodeSentence);
}

InputConverterDefinition MakeOperatorEntityInputConverter() {
  InputConverterDefinition def;
  def.type = kEntitySlot;
  def.name = "entity_extract";
  def.slot = ExternalInputSlot<TextInputValue>(kEntitySlot);
  def.logical_ports = {OutputPort(kSentenceText)};
  def.decode_fn = &DecodeOperatorEntityInput;
  return def;
}

InputConverterDefinition MakeOperatorKeywordInputConverter() {
  InputConverterDefinition def;
  def.type = kKeywordSlot;
  def.name = "keyword_match";
  def.slot = ExternalInputSlot<TextInputValue>(kKeywordSlot);
  def.logical_ports = {OutputPort(kSentenceText)};
  def.decode_fn = &DecodeOperatorKeywordInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorEntityInputConverter());
REGISTER_INPUT_CONVERTER(MakeOperatorKeywordInputConverter());

}  // namespace
}  // namespace llm_edgeflow
