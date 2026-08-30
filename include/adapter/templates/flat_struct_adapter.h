#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_interface.h"

namespace alg_framework {
namespace template_examples {

// 1. 纯 C 结构体声明
typedef struct {
  uint64_t request_id;
  const char* sentence_text;  // 必填 UTF-8 字符串
} TemplateFlatInput;

typedef struct {
  uint64_t request_id;
  int status_code;
  char result_json[512];  // 固定容量输出
} TemplateFlatOutput;

// 2. 内部 DTO
struct TemplateFlatResultDto {
  uint64_t request_id;
  int status_code;
  std::string result_json;
};

// 3. 模板适配器实现
class TemplateFlatStructAdapter : public IBusinessAdapter {
 public:
  CompanyAlgBizType BizType() const override {
    return static_cast<CompanyAlgBizType>(101);
  }

  const char* BizName() const override { return "TemplateFlatStruct"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        static_cast<CompanyAlgBizType>(101),
        "TemplateFlatStruct",
        "2.0.0",
        "TemplateFlatInput",
        "TemplateFlatOutput",
        64,
        OwnershipPolicy::kCopyIn,
        ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne,
        {BusinessDefinition{"TemplateFlatStruct",
                            "template_flat_pipeline_v1"}}};
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

    std::vector<uint64_t> req_ids;
    std::vector<std::string> sentences;
    req_ids.reserve(num_inputs);
    sentences.reserve(num_inputs);

    constexpr size_t kMaxTextLen = 64 * 1024;

    for (int i = 0; i < num_inputs; ++i) {
      auto* in = static_cast<const TemplateFlatInput*>(inputs[i]);
      if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in, i,
                                                   BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      if (!AdapterValidationHelper::RequireBoundedString(
              "inputs[i].sentence_text", in->sentence_text, kMaxTextLen, i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      req_ids.push_back(in->request_id);
      sentences.push_back(in->sentence_text);  // COPY_IN 深拷贝
    }

    ctx->Set("raw_request_ids", std::move(req_ids));
    ctx->Set("raw_sentences", std::move(sentences));
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs,
           AdapterStatus* out_status = nullptr) const override {
    if (!ctx) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    const auto* res =
        ctx->Read<std::vector<TemplateFlatResultDto>>("flat_final_outputs");
    if (!res) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName(), out_status);
    if (valid_ret != 0) return valid_ret;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<TemplateFlatOutput*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->status_code = (*res)[i].status_code;

      // RECHECK-001: 必须检查 CheckedStringCopy 返回值
      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->result_json, sizeof(out_ptr->result_json),
              (*res)[i].result_json.c_str(), "outputs[i].result_json", i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

}  // namespace template_examples
}  // namespace alg_framework
