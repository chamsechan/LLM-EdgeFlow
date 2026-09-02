#include "adapter/operator/operator_biz_bridge_registry.h"

namespace llm_edgeflow {

void RegisterKeywordMatchBridge(OperatorBizBridgeRegistry& reg) {
  OperatorBizBridgeDescriptor desc;
  desc.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;
  desc.biz_name = "KeywordMatch";
  desc.internal_input_type_name = "CompanyKeywordInputStruct";
  desc.internal_output_type_name = "CompanyKeywordOutputStruct";
  desc.registration_identity = "builtin.keyword_match";

  OperatorBizSlot in_slot;
  in_slot.logical_name = "keyword_in";
  in_slot.type_suffix = "keyword_in";
  in_slot.direction = IoDirection::kInput;
  in_slot.required = true;
  desc.input_slots.push_back(in_slot);

  OperatorBizSlot out_slot;
  out_slot.logical_name = "keyword_out";
  out_slot.type_suffix = "keyword_out";
  out_slot.direction = IoDirection::kOutput;
  out_slot.required = true;
  desc.output_slots.push_back(out_slot);

  desc.convert_sample_input =
      [](const std::unordered_map<std::string, const void*>& slots,
         ProcessLocalShadowStorage& storage, const void** out_internal_dto,
         std::string* err) -> int {
    auto it = slots.find("keyword_in");
    if (it == slots.end() || !it->second) {
      if (err) *err = "Missing required input slot keyword_in";
      return -3;
    }
    const auto* in =
        static_cast<const CompanyOperatorKeywordInput*>(it->second);
    auto* dto = storage.AllocateShadowDto<CompanyKeywordInputStruct>();
    dto->request_id = in->request_id;
    dto->sentence_text = storage.StoreString(in->sentence_text);
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
        static_cast<const CompanyKeywordOutputStruct*>(internal_dto);
    auto* out =
        static_cast<CompanyOperatorKeywordOutput*>(external_output_struct);
    out->request_id = in_dto->request_id;
    out->is_hit = in_dto->is_hit;
    out->status_code = in_dto->status_code;

    return OperatorBizBridgeRegistry::CopyToPooledString(
        in_dto->match_result_json, out->match_result_json,
        spec.GetCapacity("match_result_json"), "match_result_json", err);
  };

  desc.create_shadow_output_dto = [](ProcessLocalShadowStorage& s) -> void* {
    return s.AllocateShadowDto<CompanyKeywordOutputStruct>();
  };

  reg.RegisterBridge(desc);
}

REGISTER_OPERATOR_BIZ_BRIDGE(RegisterKeywordMatchBridge);

}  // namespace llm_edgeflow
