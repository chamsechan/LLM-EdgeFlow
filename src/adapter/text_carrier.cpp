#include "adapter/text_carrier.h"

namespace llm_edgeflow {

int ConvertTextCarrierInput(const CompanyOperatorEntityInput& in,
                            ProcessLocalShadowStorage& storage,
                            const CompanyEntityInputStruct** out_internal_dto,
                            std::string* err) {
  (void)err;
  auto* dto = storage.AllocateShadowDto<CompanyEntityInputStruct>();
  dto->request_id = in.request_id;
  dto->sentence_text = storage.StoreString(in.sentence_text);
  *out_internal_dto = dto;
  return 0;
}

int ConvertTextCarrierOutput(const EntityResult& in_dto,
                             CompanyOperatorEntityOutput& out,
                             const ResolvedOutputPoolSpec& spec,
                             std::string* err) {
  out.request_id = in_dto.request_id;
  out.status_code = in_dto.status_code;
  return CopyToOperatorString(in_dto.entities_json.c_str(), out.entities_json,
                              spec.GetCapacity("entities_json"),
                              "entities_json", err);
}

OperatorBizBridgeDescriptor MakeTextCarrierBridge(CompanyAlgBizType biz_type,
                                                  std::string adapter_name,
                                                  std::string identity) {
  return MakeTypedSingleSlotBizBridge<
      CompanyEntityInputStruct, EntityResult, CompanyOperatorEntityInput,
      CompanyOperatorEntityOutput, &ConvertTextCarrierInput,
      &ConvertTextCarrierOutput>(
      biz_type, std::move(adapter_name), "CompanyEntityInputStruct",
      std::move(identity), "entity_in", "entity_out");
}

}  // namespace llm_edgeflow
