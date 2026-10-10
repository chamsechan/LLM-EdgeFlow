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
#include "nlohmann/json.hpp"

namespace llm_edgeflow {
namespace {

constexpr auto kQuery = MakeBlackboardKey<TextBatch>("query");

constexpr const char* kInputSlot = "entity_in";

int ParseTranslateQuery(const std::string& raw_text, std::string* out_query) {
  const auto req_json = nlohmann::json::parse(raw_text, nullptr, false);
  if (!req_json.is_object() || !req_json.contains("query") ||
      !req_json["query"].is_string()) {
    return -1;
  }
  if (out_query) {
    *out_query = req_json["query"].get<std::string>();
  }
  return 0;
}

AdapterStatus DecodeTranslateQuery(const TextInputValue& input,
                                   std::string* query) {
  if (ParseTranslateQuery(input.sentence_text, query) != 0) {
    return AdapterStatus::InvalidInput(
        "Expected a JSON object with string field query", "json");
  }
  return AdapterStatus::Ok();
}

int DecodeOperatorTranslateJson(const ExternalInputBatchView& source,
                                const InputDecodeOptions& options,
                                AlgContext* context, AdapterStatus* status) {
  return DecodeRequestRows<TextInputValue>(source, options, context, status,
                                           kInputSlot, kQuery,
                                           &DecodeTranslateQuery);
}

InputConverterDefinition MakeOperatorTranslateJsonInputConverter() {
  InputConverterDefinition def;
  def.type = kInputSlot;
  def.name = "translate";
  def.slot = ExternalInputSlot<TextInputValue>(kInputSlot);
  def.logical_ports = {OutputPort(kQuery)};
  def.decode_fn = &DecodeOperatorTranslateJson;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorTranslateJsonInputConverter());

}  // namespace
}  // namespace llm_edgeflow
