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

constexpr auto kImage = MakeBlackboardKey<ImageRefBatch>("image");
constexpr auto kQuestion = MakeBlackboardKey<TextBatch>("question");

constexpr const char* kFrameSlot = "frame";
constexpr const char* kQuerySlot = "string";

int DecodeOperatorFrameInput(const ExternalInputBatchView& source,
                             const InputDecodeOptions& options,
                             AlgContext* context, AdapterStatus* status) {
  if (!ValidateDecodeRequest(source, options, context, status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  std::vector<uint64_t> raw_req_ids;
  ImageRefBatch raw_images;

  raw_req_ids.reserve(source.count);
  raw_images.reserve(source.count);

  for (size_t i = 0; i < source.count; ++i) {
    const auto* frame =
        ReadInputSlot<CompanyFrame>(source, kFrameSlot, i, options, status);
    if (!frame) return COMPANY_ALG_ERR_INVALID_INPUT;

    if (!IsValidInputString(frame->image_uri)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Invalid frame.image_uri CompanyString", "frame.image_uri",
          options.Label().c_str(), static_cast<int>(i));
    }
    if (static_cast<size_t>(frame->image_uri->length) >
        input_limits::kMaxImageUriBytes) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "image_uri length exceeds limit", "frame.image_uri",
          options.Label().c_str(), static_cast<int>(i));
    }

    std::string image_path = CopyInputString(*frame->image_uri);

    raw_req_ids.push_back(frame->request_id);
    raw_images.emplace_back(static_cast<uint32_t>(i), 0, std::move(image_path));
  }

  if (!PublishRequestIds(options, std::move(raw_req_ids), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, options.Port(kImage.name), std::move(raw_images),
          options.Label().c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

int DecodeOperatorQueryInput(const ExternalInputBatchView& source,
                             const InputDecodeOptions& options,
                             AlgContext* context, AdapterStatus* status) {
  if (!ValidateDecodeRequest(source, options, context, status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  TextBatch raw_queries;
  raw_queries.reserve(source.count);

  for (size_t i = 0; i < source.count; ++i) {
    const auto* query =
        ReadInputSlot<CompanyString>(source, kQuerySlot, i, options, status);
    if (!query) return COMPANY_ALG_ERR_INVALID_INPUT;

    if (!IsValidInputString(query)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Invalid query CompanyString", kQuerySlot,
          options.Label().c_str(), static_cast<int>(i));
    }
    if (static_cast<size_t>(query->length) > input_limits::kMaxTextBytes) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "query length exceeds limit", kQuerySlot,
          options.Label().c_str(), static_cast<int>(i));
    }

    std::string query_prompt = CopyInputString(*query);
    raw_queries.emplace_back(static_cast<uint32_t>(i), 0,
                             std::move(query_prompt));
  }

  if (!AdapterValidationHelper::PublishContextValue(
          *context, options.Port(kQuestion.name), std::move(raw_queries),
          options.Label().c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

InputConverterDefinition MakeOperatorFrameInputConverter() {
  InputConverterDefinition def;
  def.type = kFrameSlot;
  def.name = "ocr_invoice_qa";
  def.service_type = kMockServiceOcrInvoiceQa;
  def.slot = ExternalInputSlot<CompanyFrame>(kFrameSlot);
  def.logical_ports = {OutputPort(kImage)};
  def.decode_fn = &DecodeOperatorFrameInput;
  return def;
}

InputConverterDefinition MakeOperatorQueryInputConverter() {
  InputConverterDefinition def;
  def.type = kQuerySlot;
  def.name = "ocr_invoice_qa";
  def.slot = ExternalInputSlot<CompanyString>(kQuerySlot);
  def.logical_ports = {OutputPort(kQuestion)};
  def.decode_fn = &DecodeOperatorQueryInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorFrameInputConverter());
REGISTER_INPUT_CONVERTER(MakeOperatorQueryInputConverter());

}  // namespace
}  // namespace llm_edgeflow
