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

int DecodeOperatorTranslateJson(const ExternalInputBatchView& source,
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
  TextBatch sentences;
  req_ids.reserve(source.count);
  sentences.reserve(source.count);

  for (size_t i = 0; i < source.count; ++i) {
    const auto* in = source.GetSlot<CompanyOperatorEntityInput>("entity_in", i);
    if (!in) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Missing entity_in input slot or slot item is null",
          "entity_in", options.converter_id.c_str(), static_cast<int>(i));
    }
    if (!in->sentence_text || in->sentence_text->length < 0 ||
        (in->sentence_text->length > 0 && !in->sentence_text->data)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "sentence_text string pointer is null or invalid",
          "sentence_text", options.converter_id.c_str(), static_cast<int>(i));
    }
    if (static_cast<size_t>(in->sentence_text->length) > kMaxSentenceLen) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "sentence_text length exceeds 64 KiB limit", "sentence_text",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    std::string raw(in->sentence_text->data, in->sentence_text->length);
    std::string query;
    if (ParseTranslateQuery(raw, &query) != 0) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Expected a JSON object with string field query", "json",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    req_ids.push_back(in->request_id);
    sentences.emplace_back(static_cast<uint32_t>(i), 0, std::move(query));
  }

  if (!AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key<std::vector<uint64_t>>("raw_request_ids"),
          std::move(req_ids), options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key<TextBatch>("input_sentences"),
          std::move(sentences), options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

InputConverterDefinition MakeOperatorTranslateJsonInputConverter() {
  InputConverterDefinition def;
  def.converter_id = "translate.json.operator.v1";

  def.schema_id = "translate.json.request";
  def.schema_version = 1;
  def.external_type = "CompanyOperatorEntityInput";
  def.max_batch_size = 64;

  def.external_slots = {{"entity_in",
                         "CompanyOperatorEntityInput",
                         PortDirection::kInput,
                         true,
                         "entity_in",
                         "entity_in",
                         {}}};
  def.logical_ports = {OutputPort(kRawRequestIds), OutputPort(kInputSentences)};
  def.decode_fn = &DecodeOperatorTranslateJson;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorTranslateJsonInputConverter());

}  // namespace
}  // namespace llm_edgeflow
