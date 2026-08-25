#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/business_adapter_interface.h"

namespace alg_framework {
namespace template_examples {

typedef struct {
  const char* tag_name;
  float weight;
} TemplateTagItem;

typedef struct {
  uint64_t request_id;
  int tag_count;
  const TemplateTagItem* tag_array;  // 嵌套动态数组指针
} TemplateNestedArrayInput;

typedef struct {
  uint64_t request_id;
  int status_code;
  int total_tags_processed;
  char summary[256];
} TemplateNestedArrayOutput;

struct TemplateTagDto {
  std::string tag_name;
  float weight = 0.0f;
};

struct TemplateNestedArrayItemDto {
  uint64_t request_id;
  std::vector<TemplateTagDto> tags;
};

struct TemplateNestedArrayResultDto {
  uint64_t request_id;
  int status_code;
  int total_tags_processed;
  std::string summary;
};

class TemplateNestedArrayAdapter : public IBusinessAdapter {
 public:
  CompanyAlgBizType BizType() const override {
    return static_cast<CompanyAlgBizType>(103);
  }

  const char* BizName() const override { return "TemplateNestedArray"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        static_cast<CompanyAlgBizType>(103),
        "TemplateNestedArray",
        "2.0.0",
        "TemplateNestedArrayInput",
        "TemplateNestedArrayOutput",
        64,
        OwnershipPolicy::kCopyIn,
        ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne,
        {BusinessDefinition{"TemplateNestedArray",
                            "template_nested_array_pipeline_v1"}}};
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx,
             AdapterStatus* out_status = nullptr) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, BizName());
    if (valid_ret != 0 || !ctx) {
      if (out_status) {
        *out_status = AdapterStatus::InvalidInput(
            "Batch envelope validation failed", "inputs", -1, BizName());
      }
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }

    std::vector<TemplateNestedArrayItemDto> items;
    items.reserve(num_inputs);

    constexpr int kMaxTagsPerItem = 1000;

    for (int i = 0; i < num_inputs; ++i) {
      auto* in = static_cast<const TemplateNestedArrayInput*>(inputs[i]);
      if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in, i,
                                                   BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      // ADP-001, RECHECK-004: 范围校验与乘法溢出保护
      if (!AdapterValidationHelper::RequireRange(
              "inputs[i].tag_count", in->tag_count, 0, kMaxTagsPerItem, i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      if (in->tag_count > 0) {
        if (!AdapterValidationHelper::RequireNotNull("inputs[i].tag_array",
                                                     in->tag_array, i,
                                                     BizName(), out_status)) {
          return COMPANY_ALG_ERR_INVALID_INPUT;
        }
        if (!AdapterValidationHelper::CheckedMultiply(
                "inputs[i].tag_array", in->tag_count, sizeof(TemplateTagItem),
                10 * 1024 * 1024, i, BizName(), out_status)) {
          return COMPANY_ALG_ERR_INVALID_INPUT;
        }
      }

      TemplateNestedArrayItemDto item;
      item.request_id = in->request_id;
      item.tags.reserve(in->tag_count);

      for (int k = 0; k < in->tag_count; ++k) {
        std::string tag_path = "inputs[i].tag_array[" + std::to_string(k) + "]";
        if (!AdapterValidationHelper::RequireBoundedString(
                (tag_path + ".tag_name").c_str(), in->tag_array[k].tag_name,
                4096, i, BizName(), out_status)) {
          return COMPANY_ALG_ERR_INVALID_INPUT;
        }
        TemplateTagDto tag_dto;
        tag_dto.tag_name = in->tag_array[k].tag_name;
        tag_dto.weight = in->tag_array[k].weight;
        item.tags.push_back(std::move(tag_dto));
      }
      items.push_back(std::move(item));
    }

    ctx->Set("nested_array_items", std::move(items));
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs,
           AdapterStatus* out_status = nullptr) const override {
    if (!ctx) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    auto* res = ctx->Get<std::vector<TemplateNestedArrayResultDto>>(
        "nested_array_outputs");
    if (!res) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName());
    if (valid_ret != 0) return valid_ret;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<TemplateNestedArrayOutput*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->status_code = (*res)[i].status_code;
      out_ptr->total_tags_processed = (*res)[i].total_tags_processed;

      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->summary, sizeof(out_ptr->summary),
              (*res)[i].summary.c_str(), "outputs[i].summary", i, BizName(),
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
