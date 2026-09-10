#include <cmath>
#include <cstring>
#include <nlohmann/json.hpp>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_registry.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/biz_input_constraints.h"
#include "adapter/biz_results.h"
#include "adapter/result_packing_adapter.h"
#include "adapter/result_validation.h"
#include "edgeflow/c_api.h"

namespace llm_edgeflow {

inline static constexpr char kDialogueAuditBizName[] =
    "dialogue_compliance_audit_v1";

class ComplianceAuditAdapter
    : public ResultPackingAdapter<ComplianceAuditAdapter,
                                  CompanyAuditOutputStruct, AuditResult> {
 public:
  CompanyAlgBizType BizType() const override {
    return ALG_BIZ_TYPE_COMPLIANCE_AUDIT;
  }

  const char* AdapterName() const override { return "ComplianceAudit"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        ALG_BIZ_TYPE_COMPLIANCE_AUDIT,
        "ComplianceAudit",
        COMPANY_ALG_ABI_VERSION,
        "CompanyAuditInputStruct",
        "CompanyAuditOutputStruct",
        64,
        OwnershipPolicy::kCopyIn,
        ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne,
        {{kDialogueAuditBizName,
          "dialogue_audit",
          "对话合规审核",
          {RequiredBizInput(kRawRequestIds), RequiredBizInput(kUserTexts),
           RequiredBizInput(kChannelNames)},
          {BizOutput(kStructuredVerdicts),
           BizPortDefinition{kMatchedPolicy.name, kMatchedPolicy.type_id, true,
                             "N:1", "aggregate", "request"},
           BizOutput(kRuleMatches)}}}};
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx,
             AdapterStatus* out_status = nullptr) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, AdapterName());
    if (valid_ret != 0 || !ctx) {
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status, "Batch envelope validation failed or null AlgContext",
          "inputs", AdapterName());
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
                                                   AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      if (!AdapterValidationHelper::RequireBoundedString(
              "inputs[i].user_text", in->user_text, kMaxTextLen, i,
              AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      if (in->channel_name &&
          !AdapterValidationHelper::RequireBoundedString(
              "inputs[i].channel_name", in->channel_name,
              biz_input::kMaxChannelNameBytes, i, AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      req_ids.push_back(in->request_id);
      user_texts.emplace_back(static_cast<uint32_t>(i), 0, in->user_text);
      channel_names.emplace_back(static_cast<uint32_t>(i), 0,
                                 in->channel_name ? in->channel_name : "");
    }

    if (!AdapterValidationHelper::PublishContextValue(
            *ctx, kRawRequestIds, std::move(req_ids), AdapterName(),
            out_status) ||
        !AdapterValidationHelper::PublishContextValue(
            *ctx, kUserTexts, std::move(user_texts), AdapterName(),
            out_status) ||
        !AdapterValidationHelper::PublishContextValue(
            *ctx, kChannelNames, std::move(channel_names), AdapterName(),
            out_status)) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    return COMPANY_ALG_SUCCESS;
  }

  template <typename Output>
  int PackTyped(AlgContext* ctx, void** outputs, int* num_outputs,
                AdapterStatus* out_status = nullptr) const {
    if (!ctx) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          out_status, "Null AlgContext passed to Pack", "ctx", AdapterName());
    }

    const auto* verdicts = AdapterValidationHelper::ReadRequiredContextValue(
        *ctx, kStructuredVerdicts, AdapterName(), out_status);
    if (!verdicts) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    const auto* matched_policies = ctx->Read(kMatchedPolicy);
    const auto* raw_req_ids = ctx->Read(kRawRequestIds);

    int count = static_cast<int>(verdicts->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, AdapterName(), out_status);
    if (valid_ret != 0) return valid_ret;

    if (!matched_policies ||
        matched_policies->size() < static_cast<size_t>(count)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status,
          "matched_policies missing or count mismatch in AlgContext",
          "matched_policies", AdapterName());
    }

    std::vector<const StructuredDocumentBatch::value_type*> verdicts_by_request;
    if (!IndexResults(verdicts, raw_req_ids, &verdicts_by_request, "verdicts",
                      AdapterName(), out_status))
      return COMPANY_ALG_ERR_INVALID_INPUT;
    std::vector<const RankedTextBatch::value_type*> matched_policies_by_request;
    if (!IndexResults(matched_policies, raw_req_ids,
                      &matched_policies_by_request, "matched_policies",
                      AdapterName(), out_status, true))
      return COMPANY_ALG_ERR_INVALID_INPUT;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<Output*>(outputs[i]);
      uint64_t req_id =
          (raw_req_ids && i < static_cast<int>(raw_req_ids->size()))
              ? (*raw_req_ids)[i]
              : verdicts_by_request[i]->req_id;
      out_ptr->request_id = req_id;

      const auto& verdict_item = verdicts_by_request[i]->data;
      if (matched_policies_by_request[i]->data.rank != 1 ||
          !IsSuccessfulDocument(verdict_item) ||
          !verdict_item.structured_data.contains("risk_level") ||
          !verdict_item.structured_data.contains("risk_score") ||
          !verdict_item.structured_data["risk_level"].is_string() ||
          !verdict_item.structured_data["risk_score"].is_number()) {
        return AdapterValidationHelper::ReturnInvalidInput(
            out_status,
            "structured_data missing or invalid risk_level/risk_score types",
            "structured_verdicts", AdapterName(), i);
      }

      std::string risk_level =
          verdict_item.structured_data["risk_level"].get<std::string>();
      float risk_score =
          verdict_item.structured_data["risk_score"].get<float>();
      if (!std::isfinite(risk_score) || risk_score < 0 || risk_score > 1 ||
          (risk_level != "SAFE" && risk_level != "LOW_RISK" &&
           risk_level != "MEDIUM_RISK" && risk_level != "HIGH_RISK")) {
        return AdapterValidationHelper::ReturnInvalidInput(
            out_status, "Invalid risk level or score", "structured_verdicts",
            AdapterName(), i);
      }
      const std::string& verdict_json = verdict_item.json_payload;

      std::string policy_clause = matched_policies_by_request[i]->data.text;

      out_ptr->risk_score = risk_score;
      out_ptr->status_code = 0;

      if (!CopyResultString(out_ptr->risk_level, risk_level.c_str(),
                            "outputs[i].risk_level", i, AdapterName(),
                            out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }

      if (!CopyResultString(out_ptr->matched_policy_clause,
                            policy_clause.c_str(),
                            "outputs[i].matched_policy_clause", i,
                            AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }

      if (!CopyResultString(out_ptr->audit_verdict_json, verdict_json.c_str(),
                            "outputs[i].audit_verdict_json", i, AdapterName(),
                            out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BIZ_ADAPTER(ComplianceAuditAdapter);

}  // namespace llm_edgeflow
