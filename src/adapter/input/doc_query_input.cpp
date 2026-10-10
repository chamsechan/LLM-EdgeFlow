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

constexpr auto kDocText = MakeBlackboardKey<TextBatch>("doc_text");
constexpr auto kQueryText = MakeBlackboardKey<TextBatch>("query_text");

constexpr const char* kInputSlot = "doc_in";

int DecodeOperatorDocQueryInput(const ExternalInputBatchView& source,
                                const InputDecodeOptions& options,
                                AlgContext* context, AdapterStatus* status) {
  if (!ValidateDecodeRequest(source, options, context, status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  TextBatch raw_docs;
  TextBatch raw_queries;

  raw_docs.reserve(source.count);
  raw_queries.reserve(source.count);

  for (size_t i = 0; i < source.count; ++i) {
    auto in = ReadInputSlot<DocumentInputValue>(source, kInputSlot, i, options,
                                                status);
    if (!in) return COMPANY_ALG_ERR_INVALID_INPUT;

    std::string doc_str = in->doc_text;
    std::string query_str = in->query_text;
    raw_docs.emplace_back(static_cast<uint32_t>(i), 0, std::move(doc_str));
    raw_queries.emplace_back(static_cast<uint32_t>(i), 0, std::move(query_str));
  }

  if (!AdapterValidationHelper::PublishContextValue(
          *context, options.Port(kDocText.name), std::move(raw_docs),
          options.Label().c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, options.Port(kQueryText.name), std::move(raw_queries),
          options.Label().c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

InputConverterDefinition MakeOperatorDocQueryInputConverter() {
  InputConverterDefinition def;
  def.type = kInputSlot;
  def.name = "doc_qa";
  def.slot = ExternalInputSlot<DocumentInputValue>(kInputSlot);
  def.logical_ports = {OutputPort(kDocText), OutputPort(kQueryText)};
  def.decode_fn = &DecodeOperatorDocQueryInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorDocQueryInputConverter());

}  // namespace
}  // namespace llm_edgeflow
