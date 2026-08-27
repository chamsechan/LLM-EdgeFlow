#include "adapter/platform/platform_business_bridge_registry.h"

namespace alg_framework {

void RegisterEntityExtractBridge(PlatformBusinessBridgeRegistry& reg) {
  PlatformBusinessBridgeDescriptor desc;
  desc.biz_type = ALG_BIZ_TYPE_ENTITY_EXTRACT;
  desc.biz_name = "EntityExtract";
  desc.internal_input_type_name = "CompanyEntityInputStruct";
  desc.internal_output_type_name = "CompanyEntityOutputStruct";
  desc.registration_identity = "builtin.entity_extract";

  PlatformBusinessSlot in_slot;
  in_slot.logical_name = "entity_in";
  in_slot.type_suffix = "entity_in";
  in_slot.direction = IoDirection::kInput;
  in_slot.required = true;
  desc.input_slots.push_back(in_slot);

  PlatformBusinessSlot out_slot;
  out_slot.logical_name = "entity_out";
  out_slot.type_suffix = "entity_out";
  out_slot.direction = IoDirection::kOutput;
  out_slot.required = true;
  desc.output_slots.push_back(out_slot);

  desc.convert_sample_input =
      [](const std::unordered_map<std::string, const void*>& slots,
         ProcessLocalShadowStorage& storage, const void** out_internal_dto,
         std::string* err) -> int {
    auto it = slots.find("entity_in");
    if (it == slots.end() || !it->second) {
      if (err) *err = "Missing required input slot entity_in";
      return -3;
    }
    const auto* in = static_cast<const CompanyPlatformEntityInput*>(it->second);
    auto* dto = storage.AllocateShadowDto<CompanyEntityInputStruct>();
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
        static_cast<const CompanyEntityOutputStruct*>(internal_dto);
    auto* out =
        static_cast<CompanyPlatformEntityOutput*>(external_output_struct);
    out->request_id = in_dto->request_id;
    out->status_code = in_dto->status_code;

    return PlatformBusinessBridgeRegistry::CopyToPooledString(
        in_dto->entities_json, out->entities_json,
        spec.GetCapacity("entities_json", 2047), "entities_json", err);
  };

  desc.create_shadow_output_dto = [](ProcessLocalShadowStorage& s) -> void* {
    return s.AllocateShadowDto<CompanyEntityOutputStruct>();
  };

  reg.RegisterBridge(desc);
}

REGISTER_PLATFORM_BUSINESS_BRIDGE(RegisterEntityExtractBridge);

}  // namespace alg_framework
