#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/business_adapter_registry.h"
#include "business/dialogue_audit/dialogue_audit_contract.h"
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
        OutputCardinality::kOneToOne,
        {{kDialogueAuditBusinessName,
          "dialogue_audit",
          "对话合规审核",
          {RequiredInput(kRawRequestIds), RequiredInput(kUserTexts),
           RequiredInput(kChannelNames)},
          {Output(kComplianceAuditOutputs)}}}};
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
    std::vector<std::string> user_texts;
    std::vector<std::string> channel_names;

    req_ids.reserve(num_inputs);
    user_texts.reserve(num_inputs);
    channel_names.reserve(num_inputs);

    constexpr size_t kMaxTextLen = 64 * 1024;  // 64KB 单文本上限

    for (int i = 0; i < num_inputs; ++i) {
      auto* in = static_cast<const CompanyAuditInputStruct*>(inputs[i]);
      if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in, i,
                                                   BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      // ADP-001, RECHECK-004: 有界字符串强校验
      if (!AdapterValidationHelper::RequireBoundedString(
              "inputs[i].user_text", in->user_text, kMaxTextLen, i, BizName(),
              out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      req_ids.push_back(in->request_id);
      user_texts.push_back(in->user_text);
      channel_names.push_back(in->channel_name ? in->channel_name : "");
    }

    ctx->Set(kRawRequestIds, std::move(req_ids));
    ctx->Set(kUserTexts, std::move(user_texts));
    ctx->Set(kChannelNames, std::move(channel_names));
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

    auto* res = ctx->Get(kComplianceAuditOutputs);
    if (!res) {
      if (out_status) {
        *out_status = AdapterStatus::BufferTooSmall(
            "compliance_audit_outputs not found in AlgContext",
            "compliance_audit_outputs", -1, BizName());
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
      auto* out_ptr = static_cast<CompanyAuditOutputStruct*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->risk_score = (*res)[i].risk_score;
      out_ptr->status_code = (*res)[i].status_code;

      // RECHECK-001: 严格拦截截断
      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->risk_level, sizeof(out_ptr->risk_level),
              (*res)[i].risk_level.c_str(), "outputs[i].risk_level", i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }

      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->matched_policy_clause,
              sizeof(out_ptr->matched_policy_clause),
              (*res)[i].matched_policy_clause.c_str(),
              "outputs[i].matched_policy_clause", i, BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }

      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->audit_verdict_json, sizeof(out_ptr->audit_verdict_json),
              (*res)[i].audit_verdict_json.c_str(),
              "outputs[i].audit_verdict_json", i, BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BUSINESS_ADAPTER(ComplianceAuditAdapter);

}  // namespace alg_framework
