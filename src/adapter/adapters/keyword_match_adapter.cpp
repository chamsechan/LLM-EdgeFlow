#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/business_adapter_registry.h"
#include "business/keyword_match/keyword_match_dto.h"
#include "company_alg_interface.h"

namespace alg_framework {

class KeywordMatchAdapter : public IBusinessAdapter {
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
        OutputCardinality::kOneToOne};
    return desc;
  }

  bool ValidatePipelineBinding(
      const std::string& pipeline_biz_name) const override {
    return pipeline_biz_name.find("keyword") != std::string::npos ||
           pipeline_biz_name == "KeywordMatch";
  }

  int Unpack(const void** inputs, int num_inputs,
             AlgContext* ctx) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, BizName());
    if (valid_ret != 0 || !ctx) return COMPANY_ALG_ERR_INVALID_INPUT;

    std::vector<uint64_t> req_ids;
    std::vector<std::string> sentences;
    req_ids.reserve(num_inputs);
    sentences.reserve(num_inputs);

    for (int i = 0; i < num_inputs; ++i) {
      auto* in = static_cast<const CompanyKeywordInputStruct*>(inputs[i]);
      if (!in || !in->sentence_text) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }
      req_ids.push_back(in->request_id);
      sentences.push_back(in->sentence_text);
    }

    ctx->Set("raw_request_ids", std::move(req_ids));
    ctx->Set("input_sentences", std::move(sentences));
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs) const override {
    if (!ctx) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    auto* res =
        ctx->Get<std::vector<KeywordMatchResult>>("keyword_match_outputs");
    if (!res) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName());
    if (valid_ret != 0) return valid_ret;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<CompanyKeywordOutputStruct*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->is_hit = (*res)[i].is_hit;
      out_ptr->status_code = (*res)[i].status_code;

      AdapterValidationHelper::CheckedStringCopy(
          out_ptr->match_result_json, sizeof(out_ptr->match_result_json),
          (*res)[i].match_result_json.c_str(), "outputs[i].match_result_json",
          i, BizName());
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BUSINESS_ADAPTER(KeywordMatchAdapter);

}  // namespace alg_framework
