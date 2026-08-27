#include "adapter/platform/platform_business_bridge_registry.h"

namespace alg_framework {

void RegisterCrossRerankBridge(PlatformBusinessBridgeRegistry& reg) {
  PlatformBusinessBridgeDescriptor desc;
  desc.biz_type = ALG_BIZ_TYPE_CROSS_RERANK;
  desc.biz_name = "CrossRerank";
  desc.internal_input_type_name = "CompanyRerankBatchInputStruct";
  desc.internal_output_type_name = "CompanyRerankBatchOutputStruct";
  desc.registration_identity = "builtin.cross_rerank";

  PlatformBusinessSlot in_slot;
  in_slot.logical_name = "rerank_in";
  in_slot.type_suffix = "rerank_in";
  in_slot.direction = IoDirection::kInput;
  in_slot.required = true;
  desc.input_slots.push_back(in_slot);

  PlatformBusinessSlot out_slot;
  out_slot.logical_name = "rerank_out";
  out_slot.type_suffix = "rerank_out";
  out_slot.direction = IoDirection::kOutput;
  out_slot.required = true;
  desc.output_slots.push_back(out_slot);

  desc.convert_sample_input =
      [](const std::unordered_map<std::string, const void*>& slots,
         ProcessLocalShadowStorage& storage, const void** out_internal_dto,
         std::string* err) -> int {
    auto it = slots.find("rerank_in");
    if (it == slots.end() || !it->second) {
      if (err) *err = "Missing required input slot rerank_in";
      return -3;
    }
    const auto* in = static_cast<const CompanyPlatformRerankInput*>(it->second);
    auto* dto = storage.AllocateShadowDto<CompanyRerankBatchInputStruct>();
    dto->request_id = in->request_id;
    dto->candidate_count = in->candidate_count;
    dto->query_text = storage.StoreString(in->query_text);

    int count = in->candidate_count;
    if (count > COMPANY_PLATFORM_MAX_RERANK_CANDIDATES) {
      count = COMPANY_PLATFORM_MAX_RERANK_CANDIDATES;
    }
    for (int i = 0; i < count; ++i) {
      dto->candidate_passages[i] =
          storage.StoreString(in->candidate_passages[i]);
    }
    for (int i = count; i < 8; ++i) {
      dto->candidate_passages[i] = nullptr;
    }
    *out_internal_dto = dto;
    return 0;
  };

  desc.convert_sample_output =
      [](const void* internal_dto, void* external_output_struct,
         const ResolvedOutputPoolSpec& /*spec*/, std::string* err) -> int {
    if (!internal_dto || !external_output_struct) {
      if (err) *err = "Null internal DTO or external output struct pointer";
      return -4;
    }
    const auto* in_dto =
        static_cast<const CompanyRerankBatchOutputStruct*>(internal_dto);
    auto* out =
        static_cast<CompanyPlatformRerankOutput*>(external_output_struct);
    out->request_id = in_dto->request_id;
    out->count = in_dto->count;
    out->status_code = in_dto->status_code;

    for (int i = 0;
         i < in_dto->count && i < COMPANY_PLATFORM_MAX_RERANK_CANDIDATES; ++i) {
      out->scores[i] = in_dto->scores[i];
      out->sorted_indices[i] = in_dto->sorted_indices[i];
    }
    return 0;
  };

  desc.create_shadow_output_dto = [](ProcessLocalShadowStorage& s) -> void* {
    return s.AllocateShadowDto<CompanyRerankBatchOutputStruct>();
  };

  reg.RegisterBridge(desc);
}

REGISTER_PLATFORM_BUSINESS_BRIDGE(RegisterCrossRerankBridge);

}  // namespace alg_framework
