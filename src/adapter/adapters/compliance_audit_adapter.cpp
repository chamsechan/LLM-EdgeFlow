#include <cstring>
#include <nlohmann/json.hpp>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_registry.h"
#include "company_alg_interface.h"
#include "core/common_contracts.h"

namespace alg_framework {

inline static constexpr char kDialogueAuditBusinessName[] =
    "dialogue_compliance_audit_v1";

class ComplianceAuditAdapter : public IBizAdapter {
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
          {Output(kStructuredVerdicts), Output(kMatchedPolicy),
           Output(kRuleMatches)}}}};
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
    TextBatch user_texts;
    TextBatch channel_names;

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

      if (!AdapterValidationHelper::RequireBoundedString(
              "inputs[i].user_text", in->user_text, kMaxTextLen, i, BizName(),
              out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      req_ids.push_back(in->request_id);
      user_texts.emplace_back(static_cast<uint32_t>(i), 0, in->user_text);
      channel_names.emplace_back(static_cast<uint32_t>(i), 0,
                                 in->channel_name ? in->channel_name : "");
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

    const auto* verdicts = ctx->Get(kStructuredVerdicts);
    const auto* matched_policies = ctx->Get(kMatchedPolicy);
    const auto* rule_matches = ctx->Get(kRuleMatches);
    const auto* raw_req_ids = ctx->Get(kRawRequestIds);

    if (!verdicts) {
      if (out_status) {
        *out_status = AdapterStatus::BufferTooSmall(
            "structured_verdicts not found in AlgContext",
            "structured_verdicts", -1, BizName());
      }
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    int count = static_cast<int>(verdicts->size());
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
      uint64_t req_id =
          (raw_req_ids && i < static_cast<int>(raw_req_ids->size()))
              ? (*raw_req_ids)[i]
              : (*verdicts)[i].req_id;
      out_ptr->request_id = req_id;

      const auto& verdict_item = (*verdicts)[i].data;
      std::string risk_level =
          verdict_item.structured_data.value("risk_level", "SAFE");
      float risk_score =
          verdict_item.structured_data.value("risk_score", 0.10f);
      const std::string& verdict_json = verdict_item.json_payload;

      std::string policy_clause;
      if (matched_policies && i < static_cast<int>(matched_policies->size())) {
        policy_clause = (*matched_policies)[i].data.text;
      }

      out_ptr->risk_score = risk_score;
      out_ptr->status_code = 0;

      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->risk_level, sizeof(out_ptr->risk_level),
              risk_level.c_str(), "outputs[i].risk_level", i, BizName(),
              out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }

      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->matched_policy_clause,
              sizeof(out_ptr->matched_policy_clause), policy_clause.c_str(),
              "outputs[i].matched_policy_clause", i, BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }

      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->audit_verdict_json, sizeof(out_ptr->audit_verdict_json),
              verdict_json.c_str(), "outputs[i].audit_verdict_json", i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BIZ_ADAPTER(ComplianceAuditAdapter);

}  // namespace alg_framework
