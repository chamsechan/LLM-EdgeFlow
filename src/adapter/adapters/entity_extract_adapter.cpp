#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/business_adapter_registry.h"
#include "business/entity_extract/entity_extract_dto.h"
#include "company_alg_interface.h"

namespace alg_framework {

class EntityExtractAdapter : public IBusinessAdapter {
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
        OutputCardinality::kOneToOne};
    return desc;
  }

  bool ValidatePipelineBinding(
      const std::string& pipeline_biz_name) const override {
    return pipeline_biz_name.find("entity_extract") != std::string::npos ||
           pipeline_biz_name == "EntityExtract";
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
      auto* in = static_cast<const CompanyEntityInputStruct*>(inputs[i]);
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
        ctx->Get<std::vector<EntityExtractResult>>("entity_extract_outputs");
    if (!res) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName());
    if (valid_ret != 0) return valid_ret;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<CompanyEntityOutputStruct*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->status_code = (*res)[i].status_code;

      AdapterValidationHelper::CheckedStringCopy(
          out_ptr->entities_json, sizeof(out_ptr->entities_json),
          (*res)[i].entities_json.c_str(), "outputs[i].entities_json", i,
          BizName());
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BUSINESS_ADAPTER(EntityExtractAdapter);

}  // namespace alg_framework
