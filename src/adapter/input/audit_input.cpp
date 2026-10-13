#include "adapter/adapter_status.h"
#include "adapter/converter_authoring.h"
#include "adapter/input/text_pair_input.h"
#include "adapter/io_converter.h"
#include "adapter/io_values.h"
#include "core/common_contracts.h"

namespace llm_edgeflow {
namespace {

constexpr auto kUserText = MakeBlackboardKey<TextBatch>("user_text");
constexpr auto kChannelName = MakeBlackboardKey<TextBatch>("channel_name");

constexpr const char* kInputSlot = "audit_in";

int DecodeOperatorAuditInput(const ExternalInputBatchView& source,
                             const InputDecodeOptions& options,
                             AlgContext* context, AdapterStatus* status) {
  return DecodeTextPair<AuditInputValue>(
      source, options, context, status, kInputSlot, kUserText,
      &AuditInputValue::user_text, kChannelName,
      &AuditInputValue::channel_name);
}

InputConverterDefinition MakeOperatorAuditInputConverter() {
  InputConverterDefinition def;
  def.type = kInputSlot;
  def.name = "dialogue_audit";
  def.slot = ExternalInputSlot<AuditInputValue>();
  def.logical_ports = {OutputPort(kUserText), OutputPort(kChannelName)};
  def.decode_fn = &DecodeOperatorAuditInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorAuditInputConverter());

}  // namespace
}  // namespace llm_edgeflow
