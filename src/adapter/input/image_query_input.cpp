#include <string>
#include <utility>

#include "adapter/adapter_status.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_values.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"

namespace llm_edgeflow {
namespace {

constexpr auto kImage = MakeBlackboardKey<ImageFrameBatch>("image");
constexpr auto kQuestion = MakeBlackboardKey<TextBatch>("question");

constexpr const char* kFrameSlot = "frame";
constexpr const char* kQuerySlot = "string";

int DecodeOperatorFrameInput(const ExternalInputBatchView& source,
                             const InputDecodeOptions& options,
                             AlgContext* context, AdapterStatus* status) {
  return DecodeRequestRows<ImageInputValue>(
      source, options, context, status, kFrameSlot, kImage,
      [](ImageInputValue& value, ImageFrame* frame) {
        *frame = std::move(value.frame);
        return AdapterStatus::Ok();
      });
}

int DecodeOperatorQueryInput(const ExternalInputBatchView& source,
                             const InputDecodeOptions& options,
                             AlgContext* context, AdapterStatus* status) {
  return DecodeRequestRows<std::string>(
      source, options, context, status, kQuerySlot, kQuestion,
      [](std::string& value, std::string* question) {
        *question = std::move(value);
        return AdapterStatus::Ok();
      });
}

InputConverterDefinition MakeOperatorFrameInputConverter() {
  InputConverterDefinition def;
  def.type = kFrameSlot;
  def.name = "ocr_invoice_qa";
  def.slot = ExternalInputSlot<ImageInputValue>();
  def.logical_ports = {OutputPort(kImage)};
  def.decode_fn = &DecodeOperatorFrameInput;
  return def;
}

InputConverterDefinition MakeOperatorQueryInputConverter() {
  InputConverterDefinition def;
  def.type = kQuerySlot;
  def.name = "ocr_invoice_qa";
  def.slot = ExternalInputSlot<std::string>();
  def.logical_ports = {OutputPort(kQuestion)};
  def.decode_fn = &DecodeOperatorQueryInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorFrameInputConverter());
REGISTER_INPUT_CONVERTER(MakeOperatorQueryInputConverter());

}  // namespace
}  // namespace llm_edgeflow
