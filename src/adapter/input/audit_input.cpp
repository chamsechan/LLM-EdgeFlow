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

constexpr size_t kMaxBatchSize = 64;

constexpr size_t kMaxTextLen = 64 * 1024;  // 64KB

int DecodeOperatorAuditInput(const ExternalInputBatchView& source,
                             const InputDecodeOptions& options,
                             const InputPortBindings& bindings,
                             AlgContext* context, AdapterStatus* status) {
  if (!ValidateDecodeRequest(source, options, context, kMaxBatchSize, status)) {
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
        source, "audit_in", i, options, status);
    if (!in) return COMPANY_ALG_ERR_INVALID_INPUT;

    if (!IsValidInputString(in->user_text)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Invalid user_text CompanyString", "audit_in.user_text",
          options.converter_id.c_str(), static_cast<int>(i));
    }
    if (static_cast<size_t>(in->user_text->length) > kMaxTextLen) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "user_text length exceeds limit", "audit_in.user_text",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    std::string channel_str;
    if (in->channel_name) {
      if (!IsValidInputString(in->channel_name)) {
        return AdapterValidationHelper::ReturnInvalidInput(
            status, "Invalid channel_name CompanyString",
            "audit_in.channel_name", options.converter_id.c_str(),
            static_cast<int>(i));
      }
      if (static_cast<size_t>(in->channel_name->length) >
          biz_input::kMaxChannelNameBytes) {
        return AdapterValidationHelper::ReturnInvalidInput(
            status, "channel_name length exceeds limit",
            "audit_in.channel_name", options.converter_id.c_str(),
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

  if (!AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key(kRawRequestIds), std::move(req_ids),
          options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key(kUserTexts), std::move(user_texts),
          options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key(kChannelNames), std::move(channel_names),
          options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

InputConverterDefinition MakeOperatorAuditInputConverter() {
  InputConverterDefinition def;
  def.converter_id = "audit.plain.operator.v1";

  def.schema_id = "audit.plain.request";
  def.external_type = "CompanyOperatorAuditInput";
  def.max_batch_size = kMaxBatchSize;

  def.external_slots = {
      ExternalInputSlot<CompanyOperatorAuditInput>("audit_in")};
  def.logical_ports = {OutputPort(kRawRequestIds), OutputPort(kUserTexts),
                       OutputPort(kChannelNames)};
  def.decode_fn = &DecodeOperatorAuditInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorAuditInputConverter());

}  // namespace
}  // namespace llm_edgeflow
