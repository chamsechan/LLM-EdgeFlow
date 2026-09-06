#include "adapter/biz_results.h"
#include "adapter/operator/operator_biz_bridge_registry.h"

namespace llm_edgeflow {

void RegisterKeywordMatchBridge(OperatorBizBridgeRegistry& reg) {
  auto desc = MakeSingleSlotBizBridge<KeywordResult>(
      ALG_BIZ_TYPE_KEYWORD_MATCH, "KeywordMatch", "CompanyKeywordInputStruct",
      "builtin.keyword_match", "keyword_in", "keyword_out");

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
    const auto* in_dto = static_cast<const KeywordResult*>(internal_dto);
    auto* out =
        static_cast<CompanyOperatorKeywordOutput*>(external_output_struct);
    out->request_id = in_dto->request_id;
    out->is_hit = in_dto->is_hit;
    out->status_code = in_dto->status_code;

    return OperatorBizBridgeRegistry::CopyToPooledString(
        in_dto->match_result_json.c_str(), out->match_result_json,
        spec.GetCapacity("match_result_json"), "match_result_json", err);
  };

  reg.RegisterBridge(desc);
}

REGISTER_OPERATOR_BIZ_BRIDGE(RegisterKeywordMatchBridge);

}  // namespace llm_edgeflow
