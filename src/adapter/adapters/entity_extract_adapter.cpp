#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_registry.h"
#include "company_alg_interface.h"
#include "core/common_contracts.h"

namespace llm_edgeflow {

inline static constexpr char kEntityExtractBizName[] = "entity_extract_0.6b_v1";
inline static constexpr char kEntityExtractLlamaCppBizName[] =
    "entity_extract_llamacpp_0.6b_v1";

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
        {{kEntityExtractBizName,
          "entity_extract",
          "实体抽取",
          {RequiredInput(kRawRequestIds), RequiredInput(kInputSentences)},
          {Output(kExtractedEntities)}},
         {kEntityExtractLlamaCppBizName,
          "entity_extract",
          "实体抽取（llama.cpp）",
          {RequiredInput(kRawRequestIds), RequiredInput(kInputSentences)},
          {Output(kExtractedEntities)}}}};
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx,
             AdapterStatus* out_status = nullptr) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, BizName());
    if (valid_ret != 0 || !ctx) {
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status, "Batch envelope validation failed or null AlgContext",
          "inputs", BizName());
    }

    std::vector<uint64_t> req_ids;
    TextBatch sentences;
    req_ids.reserve(num_inputs);
    sentences.reserve(num_inputs);

    constexpr size_t kMaxSentenceLen = 64 * 1024;  // 64KB 单文本上限

    for (int i = 0; i < num_inputs; ++i) {
      auto* in = static_cast<const CompanyEntityInputStruct*>(inputs[i]);
      if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in, i,
                                                   BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      if (!AdapterValidationHelper::RequireBoundedString(
              "inputs[i].sentence_text", in->sentence_text, kMaxSentenceLen, i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      req_ids.push_back(in->request_id);
      sentences.emplace_back(static_cast<uint32_t>(i), 0, in->sentence_text);
    }

    if (!AdapterValidationHelper::PublishContextValue(
            *ctx, kRawRequestIds, std::move(req_ids), BizName(), out_status) ||
        !AdapterValidationHelper::PublishContextValue(*ctx, kInputSentences,
                                                      std::move(sentences),
                                                      BizName(), out_status)) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs,
           AdapterStatus* out_status = nullptr) const override {
    if (!ctx) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          out_status, "Null AlgContext passed to Pack", "ctx", BizName());
    }

    const auto* res = AdapterValidationHelper::ReadRequiredContextValue(
        *ctx, kExtractedEntities, BizName(), out_status);
    if (!res) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    const auto* raw_req_ids = ctx->Read(kRawRequestIds);

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName(), out_status);
    if (valid_ret != 0) return valid_ret;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<CompanyEntityOutputStruct*>(outputs[i]);
      uint64_t req_id =
          (raw_req_ids && i < static_cast<int>(raw_req_ids->size()))
              ? (*raw_req_ids)[i]
              : (*res)[i].req_id;
      out_ptr->request_id = req_id;
      out_ptr->status_code = 0;

      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->entities_json, sizeof(out_ptr->entities_json),
              (*res)[i].data.json_payload.c_str(), "outputs[i].entities_json",
              i, BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BIZ_ADAPTER(EntityExtractAdapter);

}  // namespace llm_edgeflow
