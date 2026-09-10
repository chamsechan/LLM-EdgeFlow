#include "adapter/biz_results.h"
#include "adapter/operator_biz_bridge.h"

namespace llm_edgeflow {

void RegisterCrossRerankBridge() {
  auto desc = MakeSingleSlotBizBridge<RerankResult>(
      ALG_BIZ_TYPE_CROSS_RERANK, "CrossRerank", "CompanyRerankBatchInputStruct",
      "builtin.cross_rerank", "rerank_in", "rerank_out");

  desc.convert_sample_input =
      [](const std::unordered_map<std::string, const void*>& slots,
         ProcessLocalShadowStorage& storage, const void** out_internal_dto,
         std::string* err) -> int {
    auto it = slots.find("rerank_in");
    if (it == slots.end() || !it->second) {
      if (err) *err = "Missing required input slot rerank_in";
      return -3;
    }
    const auto* in = static_cast<const CompanyOperatorRerankInput*>(it->second);
    auto* dto = storage.AllocateShadowDto<CompanyRerankBatchInputStruct>();
    dto->request_id = in->request_id;
    dto->candidate_count = in->candidate_count;
    dto->query_text = storage.StoreString(in->query_text);

    int count = in->candidate_count;
    if (count < 0) {
      count = 0;
    } else if (count > COMPANY_OPERATOR_MAX_RERANK_CANDIDATES) {
      count = COMPANY_OPERATOR_MAX_RERANK_CANDIDATES;
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
    const auto* in_dto = static_cast<const RerankResult*>(internal_dto);
    auto* out =
        static_cast<CompanyOperatorRerankOutput*>(external_output_struct);
    out->request_id = in_dto->request_id;
    out->count = in_dto->count;
    out->status_code = in_dto->status_code;

    for (int i = 0;
         i < in_dto->count && i < COMPANY_OPERATOR_MAX_RERANK_CANDIDATES; ++i) {
      out->scores[i] = in_dto->scores[i];
      out->sorted_indices[i] = in_dto->sorted_indices[i];
    }
    return 0;
  };

  RegisterOperatorBizBridge(desc);
}

REGISTER_OPERATOR_BIZ_BRIDGE(RegisterCrossRerankBridge);

}  // namespace llm_edgeflow
