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

constexpr size_t kMaxPathLen = 4096;
constexpr size_t kMaxQueryLen = 64 * 1024;

int DecodeOperatorImageQueryInput(const ExternalInputBatchView& source,
                                  const InputDecodeOptions& options,
                                  const InputPortBindings& bindings,
                                  AlgContext* context, AdapterStatus* status) {
  if (!ValidateDecodeRequest(source, options, context, kMaxBatchSize, status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  std::vector<uint64_t> raw_req_ids;
  ImageRefBatch raw_images;
  TextBatch raw_queries;

  raw_req_ids.reserve(source.count);
  raw_images.reserve(source.count);
  raw_queries.reserve(source.count);

  for (size_t i = 0; i < source.count; ++i) {
    const auto* frame =
        ReadInputSlot<CompanyFrame>(source, "frame", i, options, status);
    if (!frame) return COMPANY_ALG_ERR_INVALID_INPUT;
    const auto* query =
        ReadInputSlot<CompanyString>(source, "string", i, options, status);
    if (!query) return COMPANY_ALG_ERR_INVALID_INPUT;

    if (!IsValidInputString(frame->image_uri)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Invalid frame.image_uri CompanyString", "frame.image_uri",
          options.converter_id.c_str(), static_cast<int>(i));
    }
    if (static_cast<size_t>(frame->image_uri->length) > kMaxPathLen) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "image_uri length exceeds limit", "frame.image_uri",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    if (!IsValidInputString(query)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Invalid query CompanyString", "string",
          options.converter_id.c_str(), static_cast<int>(i));
    }
    if (static_cast<size_t>(query->length) > kMaxQueryLen) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "query length exceeds limit", "string",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    std::string image_path = CopyInputString(*frame->image_uri);
    std::string query_prompt = CopyInputString(*query);

    raw_req_ids.push_back(frame->request_id);
    raw_images.emplace_back(static_cast<uint32_t>(i), 0, std::move(image_path));
    raw_queries.emplace_back(static_cast<uint32_t>(i), 0,
                             std::move(query_prompt));
  }

  if (!AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key(kRawRequestIds), std::move(raw_req_ids),
          options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key(kImagePaths), std::move(raw_images),
          options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key(kUserQueries), std::move(raw_queries),
          options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

InputConverterDefinition MakeOperatorImageQueryInputConverter() {
  InputConverterDefinition def;
  def.converter_id = "image_query.plain.operator.v1";

  def.schema_id = "image_query.plain.request";
  def.external_type = "CompanyFrame,CompanyString";
  def.max_batch_size = kMaxBatchSize;

  def.external_slots = {ExternalInputSlot<CompanyFrame>("frame"),
                        ExternalInputSlot<CompanyString>("string")};
  def.logical_ports = {OutputPort(kRawRequestIds), OutputPort(kImagePaths),
                       OutputPort(kUserQueries)};
  def.decode_fn = &DecodeOperatorImageQueryInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorImageQueryInputConverter());

}  // namespace
}  // namespace llm_edgeflow
