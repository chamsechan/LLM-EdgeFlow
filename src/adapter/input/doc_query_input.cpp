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

constexpr size_t kMaxQueryLen = 64 * 1024;       // 64KB
constexpr size_t kMaxDocLen = 10 * 1024 * 1024;  // 10MB

int DecodeOperatorDocQueryInput(const ExternalInputBatchView& source,
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
  TextBatch raw_docs;
  TextBatch raw_queries;

  raw_req_ids.reserve(source.count);
  raw_docs.reserve(source.count);
  raw_queries.reserve(source.count);

  for (size_t i = 0; i < source.count; ++i) {
    const auto* in = source.GetSlot<CompanyOperatorDocInput>("doc_in", i);
    if (!in) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Missing doc_in input slot or slot item is null", "doc_in",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    if (!in->query_text || in->query_text->length < 0 ||
        (in->query_text->length > 0 && !in->query_text->data)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Invalid query_text CompanyString", "doc_in.query_text",
          options.converter_id.c_str(), static_cast<int>(i));
    }
    if (static_cast<size_t>(in->query_text->length) > kMaxQueryLen) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "query_text length exceeds limit", "doc_in.query_text",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    std::string doc_str;
    if (in->doc_text) {
      if (in->doc_text->length < 0 ||
          (in->doc_text->length > 0 && !in->doc_text->data)) {
        return AdapterValidationHelper::ReturnInvalidInput(
            status, "Invalid doc_text CompanyString", "doc_in.doc_text",
            options.converter_id.c_str(), static_cast<int>(i));
      }
      if (static_cast<size_t>(in->doc_text->length) > kMaxDocLen) {
        return AdapterValidationHelper::ReturnInvalidInput(
            status, "doc_text length exceeds limit", "doc_in.doc_text",
            options.converter_id.c_str(), static_cast<int>(i));
      }
      doc_str.assign(in->doc_text->data, in->doc_text->length);
    }

    std::string query_str(in->query_text->data, in->query_text->length);

    raw_req_ids.push_back(in->request_id);
    raw_docs.emplace_back(static_cast<uint32_t>(i), 0, std::move(doc_str));
    raw_queries.emplace_back(static_cast<uint32_t>(i), 0, std::move(query_str));
  }

  if (!AdapterValidationHelper::PublishContextValue(
          *context, bindings.GetActualKey("raw_request_ids"),
          std::move(raw_req_ids), options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.GetActualKey("raw_docs"), std::move(raw_docs),
          options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.GetActualKey("raw_queries"),
          std::move(raw_queries), options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

InputConverterDefinition MakeOperatorDocQueryInputConverter() {
  InputConverterDefinition def;
  def.converter_id = "doc_query.plain.operator.v1";

  def.schema_id = "doc_query.plain.request";
  def.schema_version = 1;
  def.external_type = "CompanyOperatorDocInput";
  def.max_batch_size = 64;

  def.external_slots = {{"doc_in",
                         "CompanyOperatorDocInput",
                         PortDirection::kInput,
                         true,
                         "CompanyOperatorDocInput",
                         "doc_in",
                         {}}};
  def.logical_ports = {
      NodePortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      NodePortDefinition("raw_docs", "TextBatch", true, "1:1"),
      NodePortDefinition("raw_queries", "TextBatch", true, "1:1")};
  def.decode_fn = &DecodeOperatorDocQueryInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorDocQueryInputConverter());

}  // namespace
}  // namespace llm_edgeflow
