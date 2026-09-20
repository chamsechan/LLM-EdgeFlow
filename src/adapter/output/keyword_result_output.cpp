#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_results.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/result_validation.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

int EncodeOperatorKeywordResult(AlgContext* context,
                                const OutputPortBindings& bindings,
                                const OutputEncodeOptions& options,
                                ExternalOutputBatchView* destination,
                                size_t* written_count, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Null AlgContext passed to Encode", "context",
        options.converter_id.c_str());
  }

  const auto* res =
      context->Read<RuleMatchBatch>(bindings.GetActualKey("rule_matches"));
  if (!res) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: rule_matches", "res",
        options.converter_id.c_str());
  }

  const auto* raw_req_ids = context->Read<std::vector<uint64_t>>(
      bindings.GetActualKey("raw_request_ids"));
  if (!raw_req_ids) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: raw_request_ids",
        "raw_request_ids", options.converter_id.c_str());
  }

  size_t count = res->size();
  if (!destination || destination->count < count) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Destination item count is less than output count",
        "destination", options.converter_id.c_str());
  }

  std::vector<const RuleMatchBatch::value_type*> res_by_request;
  if (!IndexResults(res, raw_req_ids, &res_by_request, "res",
                    options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  std::string diag_err;
  for (size_t i = 0; i < count; ++i) {
    auto* out =
        destination->GetSlot<CompanyOperatorKeywordOutput>("keyword_out", i);
    if (!out) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status, "Missing keyword_out slot block in output view",
          "keyword_out", options.converter_id.c_str(), static_cast<int>(i));
    }
    out->request_id = (*raw_req_ids)[i];
    out->is_hit = res_by_request[i]->data.is_hit;
    out->status_code = res_by_request[i]->data.status_code;

    uint32_t cap =
        destination->GetSlotCapacity("keyword_out", "match_result_json", 2047);
    int ret = CopyToOperatorString(
        res_by_request[i]->data.match_result_json.c_str(),
        out->match_result_json, cap, "match_result_json", &diag_err);
    if (ret != 0) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status, diag_err.c_str(), "match_result_json",
          options.converter_id.c_str(), static_cast<int>(i));
    }
  }

  if (written_count) *written_count = count;
  return COMPANY_ALG_SUCCESS;
}

OutputConverterDefinition MakeOperatorKeywordResultOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "keyword.result.operator.v1";

  def.schema_id = "keyword.result.response";
  def.schema_version = 1;
  def.external_type = "CompanyOperatorKeywordOutput";
  def.cardinality = "1:1";
  def.max_batch_size = 64;
  def.capacity_policy = "reject_overflow";

  def.external_slots = {{"keyword_out",
                         "CompanyOperatorKeywordOutput",
                         PortDirection::kOutput,
                         true,
                         "CompanyOperatorKeywordOutput",
                         "keyword_out",
                         {"match_result_json"}}};
  def.logical_ports = {
      NodePortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      NodePortDefinition("rule_matches", "RuleMatchBatch", true, "1:1")};
  def.encode_fn = &EncodeOperatorKeywordResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorKeywordResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
