#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
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

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        ALG_BIZ_TYPE_COMPLIANCE_AUDIT,
        "ComplianceAudit",
        "2.0.0",
        "CompanyAuditInputStruct",
        "CompanyAuditOutputStruct",
        64,
        OwnershipPolicy::kCopyIn,
        ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne};
    return desc;
  }

  bool ValidatePipelineBinding(
      const std::string& pipeline_biz_name) const override {
    return pipeline_biz_name.find("audit") != std::string::npos ||
           pipeline_biz_name.find("compliance") != std::string::npos ||
           pipeline_biz_name == "ComplianceAudit";
  }

  int Unpack(const void** inputs, int num_inputs,
             AlgContext* ctx) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, BizName());
    if (valid_ret != 0 || !ctx) return COMPANY_ALG_ERR_INVALID_INPUT;

    std::vector<uint64_t> req_ids;
    std::vector<std::string> user_texts;
    std::vector<std::string> channel_names;

    req_ids.reserve(num_inputs);
    user_texts.reserve(num_inputs);
    channel_names.reserve(num_inputs);

    for (int i = 0; i < num_inputs; ++i) {
      auto* in = static_cast<const CompanyAuditInputStruct*>(inputs[i]);
      if (!in || !in->user_text) return COMPANY_ALG_ERR_INVALID_INPUT;

      req_ids.push_back(in->request_id);
      user_texts.push_back(in->user_text);
      channel_names.push_back(in->channel_name ? in->channel_name : "");
    }

    ctx->Set("raw_request_ids", std::move(req_ids));
    ctx->Set("user_texts", std::move(user_texts));
    ctx->Set("channel_names", std::move(channel_names));
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs) const override {
    if (!ctx) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    auto* res =
        ctx->Get<std::vector<DialogueAuditResult>>("compliance_audit_outputs");
    if (!res) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName());
    if (valid_ret != 0) return valid_ret;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<CompanyAuditOutputStruct*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->risk_score = (*res)[i].risk_score;
      out_ptr->status_code = (*res)[i].status_code;

      AdapterValidationHelper::CheckedStringCopy(
          out_ptr->risk_level, sizeof(out_ptr->risk_level),
          (*res)[i].risk_level.c_str(), "outputs[i].risk_level", i, BizName());

      AdapterValidationHelper::CheckedStringCopy(
          out_ptr->matched_policy_clause,
          sizeof(out_ptr->matched_policy_clause),
          (*res)[i].matched_policy_clause.c_str(),
          "outputs[i].matched_policy_clause", i, BizName());

      AdapterValidationHelper::CheckedStringCopy(
          out_ptr->audit_verdict_json, sizeof(out_ptr->audit_verdict_json),
          (*res)[i].audit_verdict_json.c_str(), "outputs[i].audit_verdict_json",
          i, BizName());
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BUSINESS_ADAPTER(ComplianceAuditAdapter);

}  // namespace alg_framework
