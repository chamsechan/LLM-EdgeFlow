#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_values.h"
#include "adapter/result_validation.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"

namespace llm_edgeflow {
namespace {

constexpr auto kVerdict = MakeBlackboardKey<StructuredDocumentBatch>("verdict");
constexpr auto kMatchedPolicy =
    MakeBlackboardKey<RankedTextBatch>("matched_policy");

constexpr const char* kOutputSlot = "audit_out";

struct Params {
  int64_t risk_level_max_bytes = 0;
  int64_t matched_policy_clause_max_bytes = 0;
  int64_t audit_verdict_json_max_bytes = 0;
};

Parameters<Params> ParamSpec() {
  return Parameters<Params>({
      MaxBytes("risk_level", &Params::risk_level_max_bytes).Default(31),
      MaxBytes("matched_policy_clause",
               &Params::matched_policy_clause_max_bytes)
          .Default(255),
      MaxBytes("audit_verdict_json", &Params::audit_verdict_json_max_bytes)
          .Default(1023),
  });
}

int EncodeOperatorAuditResult(AlgContext* context,
                              const OutputEncodeOptions& options,
                              ExternalOutputBatchView* destination,
                              size_t* written_count, AdapterStatus* status) {
  if (written_count) *written_count = 0;
  if (!context) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Encode", "context",
        options.Label().c_str());
  }

  const auto* verdicts =
      ReadOutputValue(*context, kVerdict, options, status, "verdicts");
  if (!verdicts) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* matched_policy =
      ReadOutputValue(*context, kMatchedPolicy, options, status);
  if (!matched_policy) return COMPANY_ALG_ERR_INVALID_INPUT;

  size_t count = verdicts->size();
  if (!destination || destination->count < count) {
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
  if (!IndexResults(verdicts, destination->count, &verdicts_by_request,
                    "verdicts", options.Label().c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  std::vector<const RankedTextBatch::value_type*> matched_policy_by_request;
  if (!IndexResults(matched_policy, destination->count,
                    &matched_policy_by_request, "matched_policy",
                    options.Label().c_str(), status, true)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  size_t written = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!destination->HasSlot(kOutputSlot, i) && !destination->required) {
      continue;
    }
    AuditOutputValue out;

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

    out.risk_score = risk_score;
    out.status_code = 0;

    out.risk_level = risk_level;
    out.matched_policy_clause = policy_clause;
    out.audit_verdict_json = verdict_json;
    if (!WriteOutputValue(*destination, kOutputSlot, i, out, options, status)) {
      return status ? status->Code() : COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
    ++written;
  }

  if (written_count) *written_count = written;
  return COMPANY_ALG_SUCCESS;
}

OutputConverterDefinition MakeOperatorAuditResultOutputConverter() {
  OutputConverterDefinition def;
  def.type = kOutputSlot;
  def.name = "dialogue_audit";
  def.slot = ExternalOutputSlot<AuditOutputValue>(kOutputSlot);
  def.logical_ports = {RequiredInputPort(kVerdict),
                       RequiredInputPort(kMatchedPolicy, "N:1")};
  def.params = ParamSpec();
  def.encode_fn = &EncodeOperatorAuditResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorAuditResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
