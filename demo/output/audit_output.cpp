#include <string>

#include "demo/common/demo_io_registry.h"
#include "platform_mock/operator_data_types.h"

namespace alg_demo {
namespace {

void ShowAuditResult(const void* output, const nlohmann::json& request_info,
                     int32_t* status, nlohmann::json* sample_output) {
  const auto& out = *static_cast<const CompanyOperatorAuditOutput*>(output);
  *status = out.status_code;
  std::string risk_level;
  std::string matched_policy_clause;
  std::string audit_verdict_json;
  if (out.risk_level && out.risk_level->data) {
    risk_level.assign(out.risk_level->data, out.risk_level->length);
  }
  if (out.matched_policy_clause && out.matched_policy_clause->data) {
    matched_policy_clause.assign(out.matched_policy_clause->data,
                                 out.matched_policy_clause->length);
  }
  if (out.audit_verdict_json && out.audit_verdict_json->data) {
    audit_verdict_json.assign(out.audit_verdict_json->data,
                              out.audit_verdict_json->length);
  }
  const auto channel = request_info.find("channel");
  if (channel != request_info.end() && channel->is_string()) {
    (*sample_output)["channel"] = *channel;
  }
  (*sample_output)["risk_level"] = risk_level;
  (*sample_output)["risk_score"] = out.risk_score;
  (*sample_output)["matched_policy"] = matched_policy_clause;
  if (!audit_verdict_json.empty()) {
    auto parsed = nlohmann::json::parse(audit_verdict_json, nullptr, false);
    if (parsed.is_discarded()) {
      (*sample_output)["audit_verdict_raw"] = audit_verdict_json;
    } else {
      (*sample_output)["audit_verdict"] = parsed;
    }
  }
}

REGISTER_DEMO_OUTPUT("CompanyOperatorAuditOutput", ShowAuditResult);

}  // namespace
}  // namespace alg_demo
