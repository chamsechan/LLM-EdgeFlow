#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/result_validation.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

int EncodeOperatorAuditResult(AlgContext* context,
                              const OutputPortBindings& bindings,
                              const OutputEncodeOptions& options,
                              ExternalOutputBatchView* destination,
                              size_t* written_count, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Encode", "context",
        options.converter_id.c_str());
  }

  const auto* verdicts = context->Read(
      bindings.Key<StructuredDocumentBatch>("structured_verdicts"));
  if (!verdicts) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Missing required context value: structured_verdicts",
        "verdicts", options.converter_id.c_str());
  }

  const auto* matched_policies =
      context->Read(bindings.Key<RankedTextBatch>("matched_policies"));
  if (!matched_policies) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Missing required context value: matched_policies",
        "matched_policies", options.converter_id.c_str());
  }

  const auto* raw_req_ids =
      context->Read(bindings.Key<std::vector<uint64_t>>("raw_request_ids"));
  if (!raw_req_ids) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Missing required context value: raw_request_ids",
        "raw_request_ids", options.converter_id.c_str());
  }

  size_t count = verdicts->size();
  if (destination->count < count) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Destination item count is less than output count",
        "destination", options.converter_id.c_str());
  }

  if (matched_policies->size() < count) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "matched_policies count mismatch in AlgContext",
        "matched_policies", options.converter_id.c_str());
  }

  std::vector<const StructuredDocumentBatch::value_type*> verdicts_by_request;
  if (!IndexResults(verdicts, raw_req_ids, &verdicts_by_request, "verdicts",
                    options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  std::vector<const RankedTextBatch::value_type*> matched_policies_by_request;
  if (!IndexResults(matched_policies, raw_req_ids, &matched_policies_by_request,
                    "matched_policies", options.converter_id.c_str(), status,
                    true)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  for (size_t i = 0; i < count; ++i) {
    auto* out =
        destination->GetSlot<CompanyOperatorAuditOutput>("audit_out", i);
    if (!out) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status, "Missing audit_out slot item", "audit_out",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    out->request_id = (*raw_req_ids)[i];

    const auto& verdict_item = verdicts_by_request[i]->data;
    if (matched_policies_by_request[i]->data.rank != 1 ||
        !IsSuccessfulDocument(verdict_item) ||
        !verdict_item.structured_data.contains("risk_level") ||
        !verdict_item.structured_data.contains("risk_score") ||
        !verdict_item.structured_data["risk_level"].is_string() ||
        !verdict_item.structured_data["risk_score"].is_number()) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status,
          "structured_data missing or invalid risk_level/risk_score types",
          "structured_verdicts", options.converter_id.c_str(),
          static_cast<int>(i));
    }

    std::string risk_level =
        verdict_item.structured_data["risk_level"].get<std::string>();
    float risk_score = verdict_item.structured_data["risk_score"].get<float>();
    if (!std::isfinite(risk_score) || risk_score < 0 || risk_score > 1 ||
        (risk_level != "SAFE" && risk_level != "LOW_RISK" &&
         risk_level != "MEDIUM_RISK" && risk_level != "HIGH_RISK")) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Invalid risk level or score", "structured_verdicts",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    const std::string& verdict_json = verdict_item.json_payload;
    std::string policy_clause = matched_policies_by_request[i]->data.text;

    out->risk_score = risk_score;
    out->status_code = 0;

    std::string err;
    int ret = CopyToOperatorString(
        risk_level.c_str(), out->risk_level,
        destination->GetSlotCapacity("audit_out", "risk_level", 31),
        "risk_level", &err);
    if (ret != 0) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status, err.empty() ? "Buffer too small for risk_level" : err.c_str(),
          "risk_level", options.converter_id.c_str(), static_cast<int>(i));
    }

    ret = CopyToOperatorString(
        policy_clause.c_str(), out->matched_policy_clause,
        destination->GetSlotCapacity("audit_out", "matched_policy_clause", 255),
        "matched_policy_clause", &err);
    if (ret != 0) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status,
          err.empty() ? "Buffer too small for matched_policy_clause"
                      : err.c_str(),
          "matched_policy_clause", options.converter_id.c_str(),
          static_cast<int>(i));
    }

    ret = CopyToOperatorString(
        verdict_json.c_str(), out->audit_verdict_json,
        destination->GetSlotCapacity("audit_out", "audit_verdict_json", 1023),
        "audit_verdict_json", &err);
    if (ret != 0) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status,
          err.empty() ? "Buffer too small for audit_verdict_json" : err.c_str(),
          "audit_verdict_json", options.converter_id.c_str(),
          static_cast<int>(i));
    }
  }

  if (written_count) *written_count = count;
  return COMPANY_ALG_SUCCESS;
}

OutputConverterDefinition MakeOperatorAuditResultOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "audit_result.plain.operator.v1";

  def.schema_id = "audit_result.plain.response";
  def.schema_version = 1;
  def.external_type = "CompanyOperatorAuditOutput";
  def.cardinality = "1:1";
  def.max_batch_size = 64;
  def.capacity_policy = "reject_overflow";

  def.external_slots = {
      {"audit_out",
       "CompanyOperatorAuditOutput",
       PortDirection::kOutput,
       true,
       "CompanyOperatorAuditOutput",
       "audit_out",
       {"risk_level", "matched_policy_clause", "audit_verdict_json"}}};
  def.logical_ports = {
      RequiredInputPort(kRawRequestIds), RequiredInputPort(kStructuredVerdicts),
      RequiredInputPort("matched_policies", kMatchedPolicy, "N:1")};
  def.encode_fn = &EncodeOperatorAuditResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorAuditResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
