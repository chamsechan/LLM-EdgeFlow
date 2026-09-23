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
#include "nlohmann/json.hpp"

namespace llm_edgeflow {
namespace {

constexpr size_t kMaxBatchSize = 64;

constexpr size_t kMaxSentenceLen = 64 * 1024;  // 64 KiB

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

AdapterStatus DecodeTranslateQuery(const CompanyOperatorEntityInput& input,
                                   std::string* query) {
  if (!IsValidInputString(input.sentence_text)) {
    return AdapterStatus::InvalidInput(
        "sentence_text string pointer is null or invalid", "sentence_text");
  }
  if (static_cast<size_t>(input.sentence_text->length) > kMaxSentenceLen) {
    return AdapterStatus::InvalidInput(
        "sentence_text length exceeds 64 KiB limit", "sentence_text");
  }
  if (ParseTranslateQuery(CopyInputString(*input.sentence_text), query) != 0) {
    return AdapterStatus::InvalidInput(
        "Expected a JSON object with string field query", "json");
  }
  return AdapterStatus::Ok();
}

int DecodeOperatorTranslateJson(const ExternalInputBatchView& source,
                                const InputDecodeOptions& options,
                                const InputPortBindings& bindings,
                                AlgContext* context, AdapterStatus* status) {
  return DecodeRequestRows<CompanyOperatorEntityInput>(
      source, options, bindings, context, status, kMaxBatchSize, "entity_in",
      kRawRequestIds, kInputSentences, &DecodeTranslateQuery);
}

InputConverterDefinition MakeOperatorTranslateJsonInputConverter() {
  InputConverterDefinition def;
  def.converter_id = "translate.json.operator.v1";

  def.schema_id = "translate.json.request";
  def.external_type = "CompanyOperatorEntityInput";
  def.max_batch_size = kMaxBatchSize;

  def.external_slots = {
      ExternalInputSlot<CompanyOperatorEntityInput>("entity_in", "entity_in")};
  def.logical_ports = {OutputPort(kRawRequestIds), OutputPort(kInputSentences)};
  def.decode_fn = &DecodeOperatorTranslateJson;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorTranslateJsonInputConverter());

}  // namespace
}  // namespace llm_edgeflow
