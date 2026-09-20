#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "contracts/inference_payloads.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

constexpr size_t kMaxPathLen = 4096;
constexpr size_t kMaxQueryLen = 64 * 1024;

int DecodeOperatorImageQueryInput(const ExternalInputBatchView& source,
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

  std::vector<uint64_t> raw_req_ids;
  ImageRefBatch raw_images;
  TextBatch raw_queries;

  raw_req_ids.reserve(source.count);
  raw_images.reserve(source.count);
  raw_queries.reserve(source.count);

  for (size_t i = 0; i < source.count; ++i) {
    const auto* frame = source.GetSlot<CompanyFrame>("frame", i);
    if (!frame) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Missing frame input slot or slot item is null", "frame",
          options.converter_id.c_str(), static_cast<int>(i));
    }
    const auto* query = source.GetSlot<CompanyString>("string", i);
    if (!query) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Missing string input slot or slot item is null", "string",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    if (!frame->image_uri || frame->image_uri->length < 0 ||
        (frame->image_uri->length > 0 && !frame->image_uri->data)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Invalid frame.image_uri CompanyString", "frame.image_uri",
          options.converter_id.c_str(), static_cast<int>(i));
    }
    if (static_cast<size_t>(frame->image_uri->length) > kMaxPathLen) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "image_uri length exceeds limit", "frame.image_uri",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    if (query->length < 0 || (query->length > 0 && !query->data)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Invalid query CompanyString", "string",
          options.converter_id.c_str(), static_cast<int>(i));
    }
    if (static_cast<size_t>(query->length) > kMaxQueryLen) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "query length exceeds limit", "string",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    std::string image_path(frame->image_uri->data, frame->image_uri->length);
    std::string query_prompt(query->data, query->length);

    raw_req_ids.push_back(frame->request_id);
    raw_images.emplace_back(static_cast<uint32_t>(i), 0, std::move(image_path));
    raw_queries.emplace_back(static_cast<uint32_t>(i), 0,
                             std::move(query_prompt));
  }

  if (!AdapterValidationHelper::PublishContextValue(
          *context, bindings.GetActualKey("raw_request_ids"),
          std::move(raw_req_ids), options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.GetActualKey("image_paths"), std::move(raw_images),
          options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.GetActualKey("user_queries"),
          std::move(raw_queries), options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

InputConverterDefinition MakeOperatorImageQueryInputConverter() {
  InputConverterDefinition def;
  def.converter_id = "image_query.plain.operator.v1";

  def.schema_id = "image_query.plain.request";
  def.schema_version = 1;
  def.external_type = "CompanyFrame,CompanyString";
  def.max_batch_size = 64;

  def.external_slots = {{"frame",
                         "CompanyFrame",
                         PortDirection::kInput,
                         true,
                         "CompanyFrame",
                         "frame",
                         {}},
                        {"string",
                         "CompanyString",
                         PortDirection::kInput,
                         true,
                         "CompanyString",
                         "string",
                         {}}};
  def.logical_ports = {
      NodePortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      NodePortDefinition("image_paths", "ImageRefBatch", true, "1:1"),
      NodePortDefinition("user_queries", "TextBatch", true, "1:1")};
  def.decode_fn = &DecodeOperatorImageQueryInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorImageQueryInputConverter());

}  // namespace
}  // namespace llm_edgeflow
