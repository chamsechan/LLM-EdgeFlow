#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/input_limits.h"
#include "adapter/io_converter.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"
#include "edgeflow/operator/types.h"

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

  std::vector<uint64_t> req_ids;
  TextBatch user_texts;
  TextBatch channel_names;

  req_ids.reserve(source.count);
  user_texts.reserve(source.count);
  channel_names.reserve(source.count);

  for (size_t i = 0; i < source.count; ++i) {
    const auto* in = ReadInputSlot<CompanyOperatorAuditInput>(
        source, kInputSlot, i, options, status);
    if (!in) return COMPANY_ALG_ERR_INVALID_INPUT;

    if (!IsValidInputString(in->user_text)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Invalid user_text CompanyString", "audit_in.user_text",
          options.Label().c_str(), static_cast<int>(i));
    }
    if (static_cast<size_t>(in->user_text->length) >
        input_limits::kMaxTextBytes) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "user_text length exceeds limit", "audit_in.user_text",
          options.Label().c_str(), static_cast<int>(i));
    }

    std::string channel_str;
    if (in->channel_name) {
      if (!IsValidInputString(in->channel_name)) {
        return AdapterValidationHelper::ReturnInvalidInput(
            status, "Invalid channel_name CompanyString",
            "audit_in.channel_name", options.Label().c_str(),
            static_cast<int>(i));
      }
      if (static_cast<size_t>(in->channel_name->length) >
          input_limits::kMaxChannelNameBytes) {
        return AdapterValidationHelper::ReturnInvalidInput(
            status, "channel_name length exceeds limit",
            "audit_in.channel_name", options.Label().c_str(),
            static_cast<int>(i));
      }
      channel_str = CopyInputString(*in->channel_name);
    }

    std::string user_str = CopyInputString(*in->user_text);
    req_ids.push_back(in->request_id);
    user_texts.emplace_back(static_cast<uint32_t>(i), 0, std::move(user_str));
    channel_names.emplace_back(static_cast<uint32_t>(i), 0,
                               std::move(channel_str));
  }

  if (!PublishRequestIds(options, std::move(req_ids), status) ||
      !AdapterValidationHelper::PublishContextValue(
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
  def.service_type = kMockServiceDialogueAudit;
  def.slot = ExternalInputSlot<CompanyOperatorAuditInput>(kInputSlot);
  def.logical_ports = {OutputPort(kUserText), OutputPort(kChannelName)};
  def.decode_fn = &DecodeOperatorAuditInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorAuditInputConverter());

}  // namespace
}  // namespace llm_edgeflow
