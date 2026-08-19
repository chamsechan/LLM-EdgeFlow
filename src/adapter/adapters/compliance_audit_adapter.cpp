#include <cstring>
#include <vector>

#include "adapter/business_adapter_registry.h"
#include "business/dialogue_audit/dialogue_audit_dto.h"
#include "company_alg_interface.h"

namespace alg_framework {

class ComplianceAuditAdapter : public IBusinessAdapter {
 public:
  CompanyAlgBizType BizType() const override {
    return ALG_BIZ_TYPE_COMPLIANCE_AUDIT;
  }

  const char* BizName() const override { return "ComplianceAudit"; }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx) override {
    if (!inputs || num_inputs <= 0 || !ctx) return -3;

    std::vector<uint64_t> req_ids;
    std::vector<std::string> user_texts;
    std::vector<std::string> channel_names;

    req_ids.reserve(num_inputs);
    user_texts.reserve(num_inputs);
    channel_names.reserve(num_inputs);

    for (int i = 0; i < num_inputs; ++i) {
      auto* in = static_cast<const CompanyAuditInputStruct*>(inputs[i]);
      if (!in) return -3;
      req_ids.push_back(in->request_id);
      user_texts.push_back(in->user_text ? in->user_text : "");
      channel_names.push_back(in->channel_name ? in->channel_name : "");
    }

    ctx->Set("raw_request_ids", std::move(req_ids));
    ctx->Set("user_texts", std::move(user_texts));
    ctx->Set("channel_names", std::move(channel_names));
    return 0;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs) override {
    if (!ctx || !outputs || !num_outputs || *num_outputs <= 0) return -4;

    auto* res =
        ctx->Get<std::vector<DialogueAuditResult>>("compliance_audit_outputs");
    if (!res) return -4;

    int count = static_cast<int>(res->size());
    int out_limit = *num_outputs;
    for (int i = 0; i < count && i < out_limit; ++i) {
      auto* out_ptr = static_cast<CompanyAuditOutputStruct*>(outputs[i]);
      if (out_ptr) {
        out_ptr->request_id = (*res)[i].request_id;
        out_ptr->risk_score = (*res)[i].risk_score;
        out_ptr->status_code = (*res)[i].status_code;

        strncpy(out_ptr->risk_level, (*res)[i].risk_level.c_str(),
                sizeof(out_ptr->risk_level) - 1);
        out_ptr->risk_level[sizeof(out_ptr->risk_level) - 1] = '\0';

        strncpy(out_ptr->matched_policy_clause,
                (*res)[i].matched_policy_clause.c_str(),
                sizeof(out_ptr->matched_policy_clause) - 1);
        out_ptr->matched_policy_clause[sizeof(out_ptr->matched_policy_clause) -
                                       1] = '\0';

        strncpy(out_ptr->audit_verdict_json,
                (*res)[i].audit_verdict_json.c_str(),
                sizeof(out_ptr->audit_verdict_json) - 1);
        out_ptr->audit_verdict_json[sizeof(out_ptr->audit_verdict_json) - 1] =
            '\0';
      }
    }
    *num_outputs = count;
    return 0;
  }
};

REGISTER_BUSINESS_ADAPTER(ComplianceAuditAdapter);

}  // namespace alg_framework
