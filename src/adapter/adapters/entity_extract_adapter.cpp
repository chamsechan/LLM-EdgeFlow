#include <cstring>
#include <vector>

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

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx) override {
    if (!inputs || num_inputs <= 0 || !ctx) return -3;

    std::vector<uint64_t> req_ids;
    std::vector<std::string> sentences;
    req_ids.reserve(num_inputs);
    sentences.reserve(num_inputs);

    for (int i = 0; i < num_inputs; ++i) {
      auto* in = static_cast<const CompanyEntityInputStruct*>(inputs[i]);
      if (!in) return -3;
      req_ids.push_back(in->request_id);
      sentences.push_back(in->sentence_text ? in->sentence_text : "");
    }

    ctx->Set("raw_request_ids", std::move(req_ids));
    ctx->Set("input_sentences", std::move(sentences));
    return 0;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs) override {
    if (!ctx || !outputs || !num_outputs || *num_outputs <= 0) return -4;

    auto* res =
        ctx->Get<std::vector<EntityExtractResult>>("entity_extract_outputs");
    if (!res) return -4;

    int count = static_cast<int>(res->size());
    int out_limit = *num_outputs;
    for (int i = 0; i < count && i < out_limit; ++i) {
      auto* out_ptr = static_cast<CompanyEntityOutputStruct*>(outputs[i]);
      if (out_ptr) {
        out_ptr->request_id = (*res)[i].request_id;
        out_ptr->status_code = (*res)[i].status_code;
        strncpy(out_ptr->entities_json, (*res)[i].entities_json.c_str(),
                sizeof(out_ptr->entities_json) - 1);
        out_ptr->entities_json[sizeof(out_ptr->entities_json) - 1] = '\0';
      }
    }
    *num_outputs = count;
    return 0;
  }
};

REGISTER_BUSINESS_ADAPTER(EntityExtractAdapter);

}  // namespace alg_framework
