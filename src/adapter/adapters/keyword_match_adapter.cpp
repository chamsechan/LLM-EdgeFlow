#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_registry.h"
#include "adapter/biz_blackboard_keys.h"
#include "company_alg_interface.h"

namespace llm_edgeflow {

inline static constexpr char kKeywordMatchBizName[] = "keyword_match_v1";

class KeywordMatchAdapter : public IBizAdapter {
 public:
  CompanyAlgBizType BizType() const override {
    return ALG_BIZ_TYPE_KEYWORD_MATCH;
  }

  const char* BizName() const override { return "KeywordMatch"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        ALG_BIZ_TYPE_KEYWORD_MATCH,
        "KeywordMatch",
        "2.0.0",
        "CompanyKeywordInputStruct",
        "CompanyKeywordOutputStruct",
        64,
        OwnershipPolicy::kCopyIn,
        ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne,
        {{kKeywordMatchBizName,
          "keyword_match",
          "关注词匹配",
          {RequiredInput(kRawRequestIds), RequiredInput(kInputSentences)},
          {Output(kRuleMatches)}}}};
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
      auto* in = static_cast<const CompanyKeywordInputStruct*>(inputs[i]);
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
        *ctx, kRuleMatches, BizName(), out_status);
    if (!res) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    const auto* raw_req_ids = ctx->Read(kRawRequestIds);

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName(), out_status);
    if (valid_ret != 0) return valid_ret;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<CompanyKeywordOutputStruct*>(outputs[i]);
      uint64_t req_id =
          (raw_req_ids && i < static_cast<int>(raw_req_ids->size()))
              ? (*raw_req_ids)[i]
              : (*res)[i].req_id;
      out_ptr->request_id = req_id;
      out_ptr->is_hit = (*res)[i].data.is_hit;
      out_ptr->status_code = (*res)[i].data.status_code;

      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->match_result_json, sizeof(out_ptr->match_result_json),
              (*res)[i].data.match_result_json.c_str(),
              "outputs[i].match_result_json", i, BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BIZ_ADAPTER(KeywordMatchAdapter);

}  // namespace llm_edgeflow
