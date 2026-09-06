#include "adapter/biz_results.h"
#include "adapter/operator/operator_biz_bridge_registry.h"

namespace llm_edgeflow {

void RegisterDocQaBridge(OperatorBizBridgeRegistry& reg) {
  auto desc = MakeSingleSlotBizBridge<DocResult>(
      ALG_BIZ_TYPE_DOC_QA, "DocQA", "CompanyDocInputStruct", "builtin.doc_qa",
      "doc_in", "doc_out");

  desc.convert_sample_input =
      [](const std::unordered_map<std::string, const void*>& slots,
         ProcessLocalShadowStorage& storage, const void** out_internal_dto,
         std::string* err) -> int {
    auto it = slots.find("doc_in");
    if (it == slots.end() || !it->second) {
      if (err) *err = "Missing required input slot doc_in";
      return -3;
    }
    const auto* in = static_cast<const CompanyOperatorDocInput*>(it->second);
    auto* dto = storage.AllocateShadowDto<CompanyDocInputStruct>();
    dto->request_id = in->request_id;
    dto->query_text = storage.StoreString(in->query_text);
    dto->doc_text = storage.StoreOptionalString(in->doc_text);
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
    const auto* in_dto = static_cast<const DocResult*>(internal_dto);
    auto* out = static_cast<CompanyOperatorDocOutput*>(external_output_struct);
    out->request_id = in_dto->request_id;
    out->confidence = in_dto->confidence;
    out->chunk_count = in_dto->chunk_count;
    out->status_code = in_dto->status_code;

    int ret = OperatorBizBridgeRegistry::CopyToPooledString(
        in_dto->intent_name.c_str(), out->intent_name,
        spec.GetCapacity("intent_name"), "intent_name", err);
    if (ret != 0) return ret;

    return OperatorBizBridgeRegistry::CopyToPooledString(
        in_dto->answer_text.c_str(), out->answer_text,
        spec.GetCapacity("answer_text"), "answer_text", err);
  };

  reg.RegisterBridge(desc);
}

REGISTER_OPERATOR_BIZ_BRIDGE(RegisterDocQaBridge);

}  // namespace llm_edgeflow
