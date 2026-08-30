#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_interface.h"

namespace alg_framework {
namespace template_examples {

enum TemplateUnionType {
  TEMPLATE_PAYLOAD_TEXT = 1,
  TEMPLATE_PAYLOAD_IMAGE = 2,
};

typedef struct {
  const char* text_content;
} TemplateTextPayload;

typedef struct {
  const char* image_path;
  int width;
  int height;
} TemplateImagePayload;

typedef struct {
  uint64_t request_id;
  int payload_type;  // 1: Text, 2: Image
  union {
    TemplateTextPayload text;
    TemplateImagePayload image;
  } data;
} TemplateTaggedUnionInput;

typedef struct {
  uint64_t request_id;
  int status_code;
  char verdict[256];
} TemplateTaggedUnionOutput;

struct TemplateUnionItemDto {
  uint64_t request_id;
  int payload_type;
  std::string text_content;
  std::string image_path;
  int width = 0;
  int height = 0;
};

struct TemplateUnionResultDto {
  uint64_t request_id;
  int status_code;
  std::string verdict;
};

class TemplateTaggedUnionAdapter : public IBusinessAdapter {
 public:
  CompanyAlgBizType BizType() const override {
    return static_cast<CompanyAlgBizType>(102);
  }

  const char* BizName() const override { return "TemplateTaggedUnion"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        static_cast<CompanyAlgBizType>(102),
        "TemplateTaggedUnion",
        "2.0.0",
        "TemplateTaggedUnionInput",
        "TemplateTaggedUnionOutput",
        64,
        OwnershipPolicy::kCopyIn,
        ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne,
        {BusinessDefinition{"TemplateTaggedUnion",
                            "template_tagged_union_pipeline_v1"}}};
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx,
             AdapterStatus* out_status = nullptr) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, BizName());
    if (valid_ret != 0 || !ctx) {
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status, "Batch envelope validation failed", "inputs", BizName());
    }

    std::vector<TemplateUnionItemDto> items;
    items.reserve(num_inputs);

    for (int i = 0; i < num_inputs; ++i) {
      auto* in = static_cast<const TemplateTaggedUnionInput*>(inputs[i]);
      if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in, i,
                                                   BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      // ADP-001, RECHECK-004: 严格校验枚举有效性
      if (!AdapterValidationHelper::RequireEnum(
              "inputs[i].payload_type", in->payload_type,
              {TEMPLATE_PAYLOAD_TEXT, TEMPLATE_PAYLOAD_IMAGE}, i, BizName(),
              out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      TemplateUnionItemDto item;
      item.request_id = in->request_id;
      item.payload_type = in->payload_type;

      if (in->payload_type == TEMPLATE_PAYLOAD_TEXT) {
        if (!AdapterValidationHelper::RequireBoundedString(
                "inputs[i].data.text.text_content", in->data.text.text_content,
                64 * 1024, i, BizName(), out_status)) {
          return COMPANY_ALG_ERR_INVALID_INPUT;
        }
        item.text_content = in->data.text.text_content;
      } else if (in->payload_type == TEMPLATE_PAYLOAD_IMAGE) {
        if (!AdapterValidationHelper::RequireBoundedString(
                "inputs[i].data.image.image_path", in->data.image.image_path,
                4096, i, BizName(), out_status)) {
          return COMPANY_ALG_ERR_INVALID_INPUT;
        }
        if (!AdapterValidationHelper::RequireRange(
                "inputs[i].data.image.width", in->data.image.width, 1, 8192, i,
                BizName(), out_status) ||
            !AdapterValidationHelper::RequireRange(
                "inputs[i].data.image.height", in->data.image.height, 1, 8192,
                i, BizName(), out_status)) {
          return COMPANY_ALG_ERR_INVALID_INPUT;
        }
        item.image_path = in->data.image.image_path;
        item.width = in->data.image.width;
        item.height = in->data.image.height;
      }
      items.push_back(std::move(item));
    }

    ctx->Set("tagged_union_items", std::move(items));
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs,
           AdapterStatus* out_status = nullptr) const override {
    if (!ctx) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    const auto* res =
        ctx->Read<std::vector<TemplateUnionResultDto>>("tagged_union_outputs");
    if (!res) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName());
    if (valid_ret != 0) return valid_ret;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<TemplateTaggedUnionOutput*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->status_code = (*res)[i].status_code;

      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->verdict, sizeof(out_ptr->verdict),
              (*res)[i].verdict.c_str(), "outputs[i].verdict", i, BizName(),
              out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

}  // namespace template_examples
}  // namespace alg_framework
