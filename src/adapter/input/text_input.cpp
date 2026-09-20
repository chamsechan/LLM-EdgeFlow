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

constexpr size_t kMaxSentenceLen = 64 * 1024;  // 64 KiB

int DecodeOperatorEntityInput(const ExternalInputBatchView& source,
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
    std::string text(in->sentence_text->data, in->sentence_text->length);
    req_ids.push_back(in->request_id);
    sentences.emplace_back(static_cast<uint32_t>(i), 0, std::move(text));
  }

  if (!AdapterValidationHelper::PublishContextValue(
          *context, bindings.GetActualKey("raw_request_ids"),
          std::move(req_ids), options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.GetActualKey("input_sentences"),
          std::move(sentences), options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

int DecodeOperatorKeywordInput(const ExternalInputBatchView& source,
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
    const auto* in =
        source.GetSlot<CompanyOperatorKeywordInput>("keyword_in", i);
    if (!in) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Missing keyword_in input slot or slot item is null",
          "keyword_in", options.converter_id.c_str(), static_cast<int>(i));
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
    std::string text(in->sentence_text->data, in->sentence_text->length);
    req_ids.push_back(in->request_id);
    sentences.emplace_back(static_cast<uint32_t>(i), 0, std::move(text));
  }

  if (!AdapterValidationHelper::PublishContextValue(
          *context, bindings.GetActualKey("raw_request_ids"),
          std::move(req_ids), options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.GetActualKey("input_sentences"),
          std::move(sentences), options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

InputConverterDefinition MakeOperatorEntityInputConverter() {
  InputConverterDefinition def;
  def.converter_id = "text.plain.operator.v1";

  def.schema_id = "text.plain.request";
  def.schema_version = 1;
  def.external_type = "CompanyOperatorEntityInput";
  def.max_batch_size = 64;

  def.external_slots = {{"entity_in",
                         "CompanyOperatorEntityInput",
                         PortDirection::kInput,
                         true,
                         "entity_in",
                         "entity_in",
                         {},
                         ""}};
  def.logical_ports = {
      NodePortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      NodePortDefinition("input_sentences", "TextBatch", true, "1:1")};
  def.decode_fn = &DecodeOperatorEntityInput;
  return def;
}

InputConverterDefinition MakeOperatorKeywordInputConverter() {
  InputConverterDefinition def;
  def.converter_id = "keyword.plain.operator.v1";

  def.schema_id = "text.plain.request";
  def.schema_version = 1;
  def.external_type = "CompanyOperatorKeywordInput";
  def.max_batch_size = 64;

  def.external_slots = {{"keyword_in",
                         "CompanyOperatorKeywordInput",
                         PortDirection::kInput,
                         true,
                         "keyword_in",
                         "keyword_in",
                         {},
                         ""}};
  def.logical_ports = {
      NodePortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      NodePortDefinition("input_sentences", "TextBatch", true, "1:1")};
  def.decode_fn = &DecodeOperatorKeywordInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorEntityInputConverter());
REGISTER_INPUT_CONVERTER(MakeOperatorKeywordInputConverter());

}  // namespace
}  // namespace llm_edgeflow
