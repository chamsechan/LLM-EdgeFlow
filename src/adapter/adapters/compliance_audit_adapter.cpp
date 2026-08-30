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
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status, "Batch envelope validation failed or null AlgContext",
          "inputs", BizName());
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
      return AdapterValidationHelper::ReturnBufferTooSmall(
          out_status, "Null AlgContext passed to Pack", "ctx", BizName());
    }

    const auto* verdicts = AdapterValidationHelper::ReadRequiredContextValue(
        *ctx, kStructuredVerdicts, BizName(), out_status);
    if (!verdicts) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    const auto* matched_policies = ctx->Read(kMatchedPolicy);
    const auto* raw_req_ids = ctx->Read(kRawRequestIds);

    int count = static_cast<int>(verdicts->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName(), out_status);
    if (valid_ret != 0) return valid_ret;

    if (!matched_policies ||
        matched_policies->size() < static_cast<size_t>(count)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status,
          "matched_policies missing or count mismatch in AlgContext",
          "matched_policies", BizName());
    }

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<CompanyAuditOutputStruct*>(outputs[i]);
      uint64_t req_id =
          (raw_req_ids && i < static_cast<int>(raw_req_ids->size()))
              ? (*raw_req_ids)[i]
              : (*verdicts)[i].req_id;
      out_ptr->request_id = req_id;

      const auto& verdict_item = (*verdicts)[i].data;
      if (!verdict_item.structured_data.contains("risk_level") ||
          !verdict_item.structured_data.contains("risk_score") ||
          !verdict_item.structured_data["risk_level"].is_string() ||
          !verdict_item.structured_data["risk_score"].is_number()) {
        return AdapterValidationHelper::ReturnInvalidInput(
            out_status,
            "structured_data missing or invalid risk_level/risk_score types",
            "structured_verdicts", BizName(), i);
      }

      std::string risk_level =
          verdict_item.structured_data["risk_level"].get<std::string>();
      float risk_score =
          verdict_item.structured_data["risk_score"].get<float>();
      const std::string& verdict_json = verdict_item.json_payload;

      std::string policy_clause = (*matched_policies)[i].data.text;

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
