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

  ImageRefBatch raw_images;

  raw_images.reserve(source.count);

  for (size_t i = 0; i < source.count; ++i) {
    auto frame =
        ReadInputSlot<ImageInputValue>(source, kFrameSlot, i, options, status);
    if (!frame) return COMPANY_ALG_ERR_INVALID_INPUT;

    std::string image_path = frame->image_uri;
    raw_images.emplace_back(static_cast<uint32_t>(i), 0, std::move(image_path));
  }

  if (!AdapterValidationHelper::PublishContextValue(
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
    auto query =
        ReadInputSlot<std::string>(source, kQuerySlot, i, options, status);
    if (!query) return COMPANY_ALG_ERR_INVALID_INPUT;

    std::string query_prompt = *query;
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
  def.slot = ExternalInputSlot<ImageInputValue>(kFrameSlot);
  def.logical_ports = {OutputPort(kImage)};
  def.decode_fn = &DecodeOperatorFrameInput;
  return def;
}

InputConverterDefinition MakeOperatorQueryInputConverter() {
  InputConverterDefinition def;
  def.type = kQuerySlot;
  def.name = "ocr_invoice_qa";
  def.slot = ExternalInputSlot<std::string>(kQuerySlot);
  def.logical_ports = {OutputPort(kQuestion)};
  def.decode_fn = &DecodeOperatorQueryInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorFrameInputConverter());
REGISTER_INPUT_CONVERTER(MakeOperatorQueryInputConverter());

}  // namespace
}  // namespace llm_edgeflow
