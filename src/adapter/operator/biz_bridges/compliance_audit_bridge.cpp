#include "adapter/biz_results.h"
#include "adapter/operator/operator_biz_bridge_registry.h"

namespace llm_edgeflow {

void RegisterComplianceAuditBridge(OperatorBizBridgeRegistry& reg) {
  auto desc = MakeSingleSlotBizBridge<AuditResult>(
      ALG_BIZ_TYPE_COMPLIANCE_AUDIT, "ComplianceAudit",
      "CompanyAuditInputStruct", "builtin.compliance_audit", "audit_in",
      "audit_out");

  desc.convert_sample_input =
      [](const std::unordered_map<std::string, const void*>& slots,
         ProcessLocalShadowStorage& storage, const void** out_internal_dto,
         std::string* err) -> int {
    auto it = slots.find("audit_in");
    if (it == slots.end() || !it->second) {
      if (err) *err = "Missing required input slot audit_in";
      return -3;
    }
    const auto* in = static_cast<const CompanyOperatorAuditInput*>(it->second);
    auto* dto = storage.AllocateShadowDto<CompanyAuditInputStruct>();
    dto->request_id = in->request_id;
    dto->user_text = storage.StoreString(in->user_text);
    dto->channel_name = storage.StoreOptionalString(in->channel_name);
    *out_internal_dto = dto;
    return 0;
  };

  desc.convert_sample_output =
      [](const void* internal_dto, void* external_output_struct,
         const ResolvedOutputPoolSpec& spec, std::string* err) -> int {
    if (!internal_dto || !external_output_struct) {
      if (err) *err = "Null internal DTO or external output struct pointer";
      return -4;
    }
    const auto* in_dto = static_cast<const AuditResult*>(internal_dto);
    auto* out =
        static_cast<CompanyOperatorAuditOutput*>(external_output_struct);
    out->request_id = in_dto->request_id;
    out->risk_score = in_dto->risk_score;
    out->status_code = in_dto->status_code;

    int ret = OperatorBizBridgeRegistry::CopyToPooledString(
        in_dto->risk_level.c_str(), out->risk_level,
        spec.GetCapacity("risk_level"), "risk_level", err);
    if (ret != 0) return ret;

    ret = OperatorBizBridgeRegistry::CopyToPooledString(
        in_dto->matched_policy_clause.c_str(), out->matched_policy_clause,
        spec.GetCapacity("matched_policy_clause"), "matched_policy_clause",
        err);
    if (ret != 0) return ret;

    return OperatorBizBridgeRegistry::CopyToPooledString(
        in_dto->audit_verdict_json.c_str(), out->audit_verdict_json,
        spec.GetCapacity("audit_verdict_json"), "audit_verdict_json", err);
  };

  reg.RegisterBridge(desc);
}

REGISTER_OPERATOR_BIZ_BRIDGE(RegisterComplianceAuditBridge);

}  // namespace llm_edgeflow
