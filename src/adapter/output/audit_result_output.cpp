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

constexpr const char* kOutputSlot = "audit_out";

int EncodeOperatorAuditResult(AlgContext* context,
                              const OutputEncodeOptions& options,
                              ExternalOutputBatchView* destination,
                              size_t* written_count, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Encode", "context",
        options.Label().c_str());
  }

  const auto* verdicts = ReadOutputValue(*context, kStructuredVerdicts, options,
                                         status, "verdicts");
  if (!verdicts) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* matched_policy =
      ReadOutputValue(*context, kMatchedPolicy, options, status);
  if (!matched_policy) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* raw_req_ids = RequestIds(options, status);
  if (!raw_req_ids) return COMPANY_ALG_ERR_INVALID_INPUT;

  size_t count = verdicts->size();
  if (destination->count < count) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Destination item count is less than output count",
        "destination", options.Label().c_str());
  }

  if (matched_policy->size() < count) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "matched_policy count mismatch in AlgContext", "matched_policy",
        options.Label().c_str());
  }

  std::vector<const StructuredDocumentBatch::value_type*> verdicts_by_request;
  if (!IndexResults(verdicts, raw_req_ids, &verdicts_by_request, "verdicts",
                    options.Label().c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  std::vector<const RankedTextBatch::value_type*> matched_policy_by_request;
  if (!IndexResults(matched_policy, raw_req_ids, &matched_policy_by_request,
                    "matched_policy", options.Label().c_str(), status, true)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  size_t non_null_written = 0;
  for (size_t i = 0; i < count; ++i) {
    auto* out =
        destination->GetSlot<CompanyOperatorAuditOutput>(kOutputSlot, i);
    if (!out) {
      if (options.required) {
        return AdapterValidationHelper::ReturnBufferTooSmall(
            status, "Missing audit_out slot item", kOutputSlot,
            options.Label().c_str(), static_cast<int>(i));
      }
      continue;
    }

    out->request_id = (*raw_req_ids)[i];

    const auto& verdict_item = verdicts_by_request[i]->data;
    if (matched_policy_by_request[i]->data.rank != 1 ||
        !IsSuccessfulDocument(verdict_item) ||
        !verdict_item.structured_data.contains("risk_level") ||
        !verdict_item.structured_data.contains("risk_score") ||
        !verdict_item.structured_data["risk_level"].is_string() ||
        !verdict_item.structured_data["risk_score"].is_number()) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status,
          "structured_data missing or invalid risk_level/risk_score types",
          "structured_verdicts", options.Label().c_str(), static_cast<int>(i));
    }

    std::string risk_level =
        verdict_item.structured_data["risk_level"].get<std::string>();
    float risk_score = verdict_item.structured_data["risk_score"].get<float>();
    if (!std::isfinite(risk_score) || risk_score < 0 || risk_score > 1 ||
        (risk_level != "SAFE" && risk_level != "LOW_RISK" &&
         risk_level != "MEDIUM_RISK" && risk_level != "HIGH_RISK")) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Invalid risk level or score", "structured_verdicts",
          options.Label().c_str(), static_cast<int>(i));
    }

    const std::string& verdict_json = verdict_item.json_payload;
    std::string policy_clause = matched_policy_by_request[i]->data.text;

    out->risk_score = risk_score;
    out->status_code = 0;

    if (!WriteOutputString(*destination, kOutputSlot, out->risk_level,
                           "risk_level", risk_level, options, status, i)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    if (!WriteOutputString(*destination, kOutputSlot,
                           out->matched_policy_clause, "matched_policy_clause",
                           policy_clause, options, status, i)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    if (!WriteOutputString(*destination, kOutputSlot, out->audit_verdict_json,
                           "audit_verdict_json", verdict_json, options, status,
                           i)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
    ++non_null_written;
  }

  if (written_count) *written_count = non_null_written;
  return COMPANY_ALG_SUCCESS;
}

// 输出字符串字段的尺寸参数；默认值按该业务的载荷设定，上限由平台结构登记决定。
struct Params {
  int64_t risk_level_max_bytes{};
  int64_t matched_policy_clause_max_bytes{};
  int64_t audit_verdict_json_max_bytes{};
};

auto ParamSpec() {
  return Parameters<Params>(
      {MaxBytes("risk_level", &Params::risk_level_max_bytes).Default(31),
       MaxBytes("matched_policy_clause",
                &Params::matched_policy_clause_max_bytes)
           .Default(255),
       MaxBytes("audit_verdict_json", &Params::audit_verdict_json_max_bytes)
           .Default(1023)});
}

OutputConverterDefinition MakeOperatorAuditResultOutputConverter() {
  OutputConverterDefinition def;
  def.type = kOutputSlot;
  def.name = "dialogue_audit";
  def.service_type =
      COMPANY_MOCK_SERVICE_DIALOGUE_AUDIT;  // 占位取值，进内网核对
  def.slot = ExternalOutputSlot<CompanyOperatorAuditOutput>(kOutputSlot);
  def.logical_ports = {RequiredInputPort(kStructuredVerdicts),
                       RequiredInputPort(kMatchedPolicy, "N:1")};
  def.params = ParamSpec();
  def.encode_fn = &EncodeOperatorAuditResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorAuditResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
