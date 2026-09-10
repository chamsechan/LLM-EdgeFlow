#include "adapter/biz_results.h"
#include "adapter/operator_biz_bridge.h"

namespace llm_edgeflow {

void RegisterEntityExtractBridge() {
  auto desc = MakeSingleSlotBizBridge<EntityResult>(
      ALG_BIZ_TYPE_ENTITY_EXTRACT, "EntityExtract", "CompanyEntityInputStruct",
      "builtin.entity_extract", "entity_in", "entity_out");

  desc.convert_sample_input =
      [](const std::unordered_map<std::string, const void*>& slots,
         ProcessLocalShadowStorage& storage, const void** out_internal_dto,
         std::string* err) -> int {
    auto it = slots.find("entity_in");
    if (it == slots.end() || !it->second) {
      if (err) *err = "Missing required input slot entity_in";
      return -3;
    }
    const auto* in = static_cast<const CompanyOperatorEntityInput*>(it->second);
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
    const auto* in_dto = static_cast<const EntityResult*>(internal_dto);
    auto* out =
        static_cast<CompanyOperatorEntityOutput*>(external_output_struct);
    out->request_id = in_dto->request_id;
    out->status_code = in_dto->status_code;

    return CopyToOperatorString(
        in_dto->entities_json.c_str(), out->entities_json,
        spec.GetCapacity("entities_json"), "entities_json", err);
  };

  RegisterOperatorBizBridge(desc);
}

REGISTER_OPERATOR_BIZ_BRIDGE(RegisterEntityExtractBridge);

}  // namespace llm_edgeflow
