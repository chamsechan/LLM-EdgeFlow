#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_input_constraints.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "contracts/inference_payloads.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

constexpr size_t kMaxTextLen = 64 * 1024;  // 64KB

int DecodeOperatorAuditInput(const ExternalInputBatchView& source,
                             const InputDecodeOptions& options,
                             const InputPortBindings& bindings,
                             AlgContext* context, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Decode", "context",
        options.converter_id.c_str());
  }
  if (source.count == 0 || source.count > 64) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Batch size out of range [1, 64]", "slots",
        options.converter_id.c_str());
  }

  std::vector<uint64_t> req_ids;
  TextBatch user_texts;
  TextBatch channel_names;

  req_ids.reserve(source.count);
  user_texts.reserve(source.count);
  channel_names.reserve(source.count);

  for (size_t i = 0; i < source.count; ++i) {
    const auto* in = source.GetSlot<CompanyOperatorAuditInput>("audit_in", i);
    if (!in) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Missing audit_in input slot or slot item is null",
          "audit_in", options.converter_id.c_str(), static_cast<int>(i));
    }

    if (!in->user_text || in->user_text->length < 0 ||
        (in->user_text->length > 0 && !in->user_text->data)) {
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
      if (in->channel_name->length < 0 ||
          (in->channel_name->length > 0 && !in->channel_name->data)) {
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
      channel_str.assign(in->channel_name->data, in->channel_name->length);
    }

    std::string user_str(in->user_text->data, in->user_text->length);
    req_ids.push_back(in->request_id);
    user_texts.emplace_back(static_cast<uint32_t>(i), 0, std::move(user_str));
    channel_names.emplace_back(static_cast<uint32_t>(i), 0,
                               std::move(channel_str));
  }

  if (!AdapterValidationHelper::PublishContextValue(
          *context, bindings.GetActualKey("raw_request_ids"),
          std::move(req_ids), options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.GetActualKey("user_texts"), std::move(user_texts),
          options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.GetActualKey("channel_names"),
          std::move(channel_names), options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

InputConverterDefinition MakeOperatorAuditInputConverter() {
  InputConverterDefinition def;
  def.converter_id = "audit.plain.operator.v1";
  def.transport = "operator";
  def.schema_id = "audit.plain.request";
  def.schema_version = 1;
  def.external_type = "CompanyOperatorAuditInput";
  def.max_batch_size = 64;
  def.ownership_policy = "copy_in";
  def.thread_model = "stateless";
  def.external_slots = {{"audit_in",
                         "CompanyOperatorAuditInput",
                         PortDirection::kInput,
                         true,
                         "CompanyOperatorAuditInput",
                         "audit_in",
                         {}}};
  def.logical_ports = {
      NodePortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      NodePortDefinition("user_texts", "TextBatch", true, "1:1"),
      NodePortDefinition("channel_names", "TextBatch", true, "1:1")};
  def.decode_fn = &DecodeOperatorAuditInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorAuditInputConverter());

}  // namespace
}  // namespace llm_edgeflow
