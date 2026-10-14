#include "adapter/adapter_status.h"
#include "adapter/converter_authoring.h"
#include "adapter/input/text_pair_input.h"
#include "adapter/io_converter.h"
#include "adapter/io_values.h"
#include "core/common_contracts.h"

namespace llm_edgeflow {
namespace {

constexpr auto kDocText = MakeBlackboardKey<TextBatch>("doc_text");
constexpr auto kQueryText = MakeBlackboardKey<TextBatch>("query_text");

constexpr const char* kInputSlot = "doc_in";

int DecodeOperatorDocQueryInput(const ExternalInputBatchView& source,
                                const InputDecodeOptions& options,
                                AlgContext* context, AdapterStatus* status) {
  return DecodeTextPair<DocumentInputValue>(
      source, options, context, status, kInputSlot, kDocText,
      &DocumentInputValue::doc_text, kQueryText,
      &DocumentInputValue::query_text);
}

InputConverterDefinition MakeOperatorDocQueryInputConverter() {
  InputConverterDefinition def;
  def.type = kInputSlot;
  def.name = "doc_qa";
  def.slot = ExternalInputSlot<DocumentInputValue>();
  def.logical_ports = {OutputPort(kDocText), OutputPort(kQueryText)};
  def.decode_fn = &DecodeOperatorDocQueryInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorDocQueryInputConverter());

}  // namespace
}  // namespace llm_edgeflow
