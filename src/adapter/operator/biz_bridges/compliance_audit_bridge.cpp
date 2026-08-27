#include "adapter/operator/operator_biz_bridge_registry.h"

namespace alg_framework {

void RegisterComplianceAuditBridge(OperatorBizBridgeRegistry& reg) {
  OperatorBizBridgeDescriptor desc;
  desc.biz_type = ALG_BIZ_TYPE_COMPLIANCE_AUDIT;
  desc.biz_name = "ComplianceAudit";
  desc.internal_input_type_name = "CompanyAuditInputStruct";
  desc.internal_output_type_name = "CompanyAuditOutputStruct";
  desc.registration_identity = "builtin.compliance_audit";

  OperatorBizSlot in_slot;
  in_slot.logical_name = "audit_in";
  in_slot.type_suffix = "audit_in";
  in_slot.direction = IoDirection::kInput;
  in_slot.required = true;
  desc.input_slots.push_back(in_slot);

  OperatorBizSlot out_slot;
  out_slot.logical_name = "audit_out";
  out_slot.type_suffix = "audit_out";
  out_slot.direction = IoDirection::kOutput;
  out_slot.required = true;
  desc.output_slots.push_back(out_slot);

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
    const auto* in_dto =
        static_cast<const CompanyAuditOutputStruct*>(internal_dto);
    auto* out =
        static_cast<CompanyOperatorAuditOutput*>(external_output_struct);
    out->request_id = in_dto->request_id;
    out->risk_score = in_dto->risk_score;
    out->status_code = in_dto->status_code;

    int ret = OperatorBizBridgeRegistry::CopyToPooledString(
        in_dto->risk_level, out->risk_level, spec.GetCapacity("risk_level", 31),
        "risk_level", err);
    if (ret != 0) return ret;

    ret = OperatorBizBridgeRegistry::CopyToPooledString(
        in_dto->matched_policy_clause, out->matched_policy_clause,
        spec.GetCapacity("matched_policy_clause", 255), "matched_policy_clause",
        err);
    if (ret != 0) return ret;

    return OperatorBizBridgeRegistry::CopyToPooledString(
        in_dto->audit_verdict_json, out->audit_verdict_json,
        spec.GetCapacity("audit_verdict_json", 1023), "audit_verdict_json",
        err);
  };

  desc.create_shadow_output_dto = [](ProcessLocalShadowStorage& s) -> void* {
    return s.AllocateShadowDto<CompanyAuditOutputStruct>();
  };

  reg.RegisterBridge(desc);
}

REGISTER_OPERATOR_BIZ_BRIDGE(RegisterComplianceAuditBridge);

}  // namespace alg_framework
