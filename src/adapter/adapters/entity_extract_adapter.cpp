#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_registry.h"
#include "biz/entity_extract/entity_extract_contract.h"
#include "company_alg_interface.h"

namespace alg_framework {

class EntityExtractAdapter : public IBizAdapter {
 public:
  CompanyAlgBizType BizType() const override {
    return ALG_BIZ_TYPE_ENTITY_EXTRACT;
  }

  const char* BizName() const override { return "EntityExtract"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        ALG_BIZ_TYPE_ENTITY_EXTRACT,
        "EntityExtract",
        "2.0.0",
        "CompanyEntityInputStruct",
        "CompanyEntityOutputStruct",
        64,
        OwnershipPolicy::kCopyIn,
        ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne,
        {{kEntityExtractBusinessName,
          "entity_extract",
          "实体抽取",
          {RequiredInput(kRawRequestIds), RequiredInput(kInputSentences)},
          {Output(kEntityExtractOutputs)}},
         {kEntityExtractLlamaCppBusinessName,
          "entity_extract",
          "实体抽取（llama.cpp）",
          {RequiredInput(kRawRequestIds), RequiredInput(kInputSentences)},
          {Output(kEntityExtractOutputs)}}}};
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx,
             AdapterStatus* out_status = nullptr) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, BizName());
    if (valid_ret != 0 || !ctx) {
      if (out_status) {
        *out_status = AdapterStatus::InvalidInput(
            "Batch envelope validation failed or null AlgContext", "inputs", -1,
            BizName());
      }
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }

    std::vector<uint64_t> req_ids;
    std::vector<std::string> sentences;
    req_ids.reserve(num_inputs);
    sentences.reserve(num_inputs);

    constexpr size_t kMaxSentenceLen = 64 * 1024;  // 64KB 单文本上限

    for (int i = 0; i < num_inputs; ++i) {
      auto* in = static_cast<const CompanyEntityInputStruct*>(inputs[i]);
      if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in, i,
                                                   BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      // ADP-001, RECHECK-004: 有界字符串强校验
      if (!AdapterValidationHelper::RequireBoundedString(
              "inputs[i].sentence_text", in->sentence_text, kMaxSentenceLen, i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      req_ids.push_back(in->request_id);
      sentences.push_back(in->sentence_text);
    }

    ctx->Set(kRawRequestIds, std::move(req_ids));
    ctx->Set(kInputSentences, std::move(sentences));
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs,
           AdapterStatus* out_status = nullptr) const override {
    if (!ctx) {
      if (out_status) {
        *out_status = AdapterStatus::BufferTooSmall(
            "Null AlgContext passed to Pack", "ctx", -1, BizName());
      }
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    auto* res = ctx->Get(kEntityExtractOutputs);
    if (!res) {
      if (out_status) {
        *out_status = AdapterStatus::BufferTooSmall(
            "entity_extract_outputs not found in AlgContext",
            "entity_extract_outputs", -1, BizName());
      }
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName());
    if (valid_ret != 0) {
      if (out_status) {
        *out_status = AdapterStatus::BufferTooSmall(
            "Output slots insufficient or null", "outputs", -1, BizName());
      }
      return valid_ret;
    }

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<CompanyEntityOutputStruct*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->status_code = (*res)[i].status_code;

      // RECHECK-001: 严格拦截截断
      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->entities_json, sizeof(out_ptr->entities_json),
              (*res)[i].entities_json.c_str(), "outputs[i].entities_json", i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BIZ_ADAPTER(EntityExtractAdapter);

}  // namespace alg_framework
