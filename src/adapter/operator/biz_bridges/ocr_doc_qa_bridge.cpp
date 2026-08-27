#include "adapter/operator/operator_biz_bridge_registry.h"

namespace alg_framework {

void RegisterOcrDocQaBridge(OperatorBizBridgeRegistry& reg) {
  OperatorBizBridgeDescriptor desc;
  desc.biz_type = ALG_BIZ_TYPE_OCR_DOC_QA;
  desc.biz_name = "OcrDocQA";
  desc.internal_input_type_name = "CompanyOcrDocInputStruct";
  desc.internal_output_type_name = "CompanyOcrDocOutputStruct";
  desc.registration_identity = "builtin.ocr_doc_qa";

  // Slot 1: frame -> CompanyFrame
  OperatorBizSlot frame_slot;
  frame_slot.logical_name = "frame";
  frame_slot.type_suffix = "frame";
  frame_slot.direction = IoDirection::kInput;
  frame_slot.required = true;
  desc.input_slots.push_back(frame_slot);

  // Slot 2: string -> CompanyString
  OperatorBizSlot string_slot;
  string_slot.logical_name = "string";
  string_slot.type_suffix = "string";
  string_slot.direction = IoDirection::kInput;
  string_slot.required = true;
  desc.input_slots.push_back(string_slot);

  // Output slot: od_out -> CompanyOdOutput
  OperatorBizSlot out_slot;
  out_slot.logical_name = "od_out";
  out_slot.type_suffix = "od_out";
  out_slot.direction = IoDirection::kOutput;
  out_slot.required = true;
  desc.output_slots.push_back(out_slot);

  desc.convert_sample_input =
      [](const std::unordered_map<std::string, const void*>& slots,
         ProcessLocalShadowStorage& storage, const void** out_internal_dto,
         std::string* err) -> int {
    auto it_frame = slots.find("frame");
    if (it_frame == slots.end() || !it_frame->second) {
      if (err) *err = "Missing required input slot frame";
      return -3;
    }
    auto it_str = slots.find("string");
    if (it_str == slots.end() || !it_str->second) {
      if (err) *err = "Missing required input slot string";
      return -3;
    }

    const auto* frame = static_cast<const CompanyFrame*>(it_frame->second);
    const auto* query = static_cast<const CompanyString*>(it_str->second);

    auto* dto = storage.AllocateShadowDto<CompanyOcrDocInputStruct>();
    dto->request_id = frame->request_id;
    dto->image_path = storage.StoreString(frame->image_uri);
    dto->query_prompt = storage.StoreString(query);
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
        static_cast<const CompanyOcrDocOutputStruct*>(internal_dto);
    auto* out = static_cast<CompanyOdOutput*>(external_output_struct);
    out->request_id = in_dto->request_id;
    out->detected_box_count = in_dto->detected_box_count;
    out->status_code = in_dto->status_code;

    return OperatorBizBridgeRegistry::CopyToPooledString(
        in_dto->extracted_invoice_json, out->result_json,
        spec.GetCapacity("result_json", 2047), "result_json", err);
  };

  desc.create_shadow_output_dto = [](ProcessLocalShadowStorage& s) -> void* {
    return s.AllocateShadowDto<CompanyOcrDocOutputStruct>();
  };

  reg.RegisterBridge(desc);
}

REGISTER_OPERATOR_BIZ_BRIDGE(RegisterOcrDocQaBridge);

}  // namespace alg_framework
