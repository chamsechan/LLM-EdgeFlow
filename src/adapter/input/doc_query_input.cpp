#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/biz_input_constraints.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "contracts/inference_payloads.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kInputSlot = "doc_in";

int DecodeOperatorDocQueryInput(const ExternalInputBatchView& source,
                                const InputDecodeOptions& options,
                                AlgContext* context, AdapterStatus* status) {
  if (!ValidateDecodeRequest(source, options, context, status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  std::vector<uint64_t> raw_req_ids;
  TextBatch raw_docs;
  TextBatch raw_queries;

  raw_req_ids.reserve(source.count);
  raw_docs.reserve(source.count);
  raw_queries.reserve(source.count);

  for (size_t i = 0; i < source.count; ++i) {
    const auto* in = ReadInputSlot<CompanyOperatorDocInput>(source, kInputSlot,
                                                            i, options, status);
    if (!in) return COMPANY_ALG_ERR_INVALID_INPUT;

    if (!IsValidInputString(in->query_text)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Invalid query_text CompanyString", "doc_in.query_text",
          options.converter_id.c_str(), static_cast<int>(i));
    }
    if (static_cast<size_t>(in->query_text->length) >
        biz_input::kMaxTextBytes) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "query_text length exceeds limit", "doc_in.query_text",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    std::string doc_str;
    if (in->doc_text) {
      if (!IsValidInputString(in->doc_text)) {
        return AdapterValidationHelper::ReturnInvalidInput(
            status, "Invalid doc_text CompanyString", "doc_in.doc_text",
            options.converter_id.c_str(), static_cast<int>(i));
      }
      if (static_cast<size_t>(in->doc_text->length) >
          biz_input::kMaxDocTextBytes) {
        return AdapterValidationHelper::ReturnInvalidInput(
            status, "doc_text length exceeds limit", "doc_in.doc_text",
            options.converter_id.c_str(), static_cast<int>(i));
      }
      doc_str = CopyInputString(*in->doc_text);
    }

    std::string query_str = CopyInputString(*in->query_text);

    raw_req_ids.push_back(in->request_id);
    raw_docs.emplace_back(static_cast<uint32_t>(i), 0, std::move(doc_str));
    raw_queries.emplace_back(static_cast<uint32_t>(i), 0, std::move(query_str));
  }

  if (!PublishRequestIds(options, std::move(raw_req_ids), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, kRawDocs, std::move(raw_docs), options.converter_id.c_str(),
          status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, kRawQueries, std::move(raw_queries),
          options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

InputConverterDefinition MakeOperatorDocQueryInputConverter() {
  InputConverterDefinition def;
  def.converter_id = "doc_query.plain.operator.v1";

  def.schema_id = "doc_query.plain.request";

  def.external_slots = {ExternalInputSlot<CompanyOperatorDocInput>(kInputSlot)};
  def.logical_ports = {OutputPort(kRawDocs), OutputPort(kRawQueries)};
  def.decode_fn = &DecodeOperatorDocQueryInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorDocQueryInputConverter());

}  // namespace
}  // namespace llm_edgeflow
