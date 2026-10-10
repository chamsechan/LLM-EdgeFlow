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

constexpr auto kUserText = MakeBlackboardKey<TextBatch>("user_text");
constexpr auto kChannelName = MakeBlackboardKey<TextBatch>("channel_name");

constexpr const char* kInputSlot = "audit_in";

int DecodeOperatorAuditInput(const ExternalInputBatchView& source,
                             const InputDecodeOptions& options,
                             AlgContext* context, AdapterStatus* status) {
  if (!ValidateDecodeRequest(source, options, context, status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  TextBatch user_texts;
  TextBatch channel_names;

  user_texts.reserve(source.count);
  channel_names.reserve(source.count);

  for (size_t i = 0; i < source.count; ++i) {
    auto in =
        ReadInputSlot<AuditInputValue>(source, kInputSlot, i, options, status);
    if (!in) return COMPANY_ALG_ERR_INVALID_INPUT;

    std::string user_str = in->user_text;
    std::string channel_str = in->channel_name;
    user_texts.emplace_back(static_cast<uint32_t>(i), 0, std::move(user_str));
    channel_names.emplace_back(static_cast<uint32_t>(i), 0,
                               std::move(channel_str));
  }

  if (!AdapterValidationHelper::PublishContextValue(
          *context, options.Port(kUserText.name), std::move(user_texts),
          options.Label().c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, options.Port(kChannelName.name), std::move(channel_names),
          options.Label().c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

InputConverterDefinition MakeOperatorAuditInputConverter() {
  InputConverterDefinition def;
  def.type = kInputSlot;
  def.name = "dialogue_audit";
  def.slot = ExternalInputSlot<AuditInputValue>(kInputSlot);
  def.logical_ports = {OutputPort(kUserText), OutputPort(kChannelName)};
  def.decode_fn = &DecodeOperatorAuditInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorAuditInputConverter());

}  // namespace
}  // namespace llm_edgeflow
