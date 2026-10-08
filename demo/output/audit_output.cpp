#include <iomanip>
#include <iostream>
#include <string>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"

namespace alg_demo {
namespace {

void ShowAuditResult(const void* output, const nlohmann::json& request_info,
                     uint64_t* request_id, int32_t* status,
                     nlohmann::json* sample_output) {
  const auto& out = *static_cast<const CompanyOperatorAuditOutput*>(output);
  const std::string risk_level = CopyCompanyString(out.risk_level);
  const std::string matched_policy =
      CopyCompanyString(out.matched_policy_clause);
  const std::string audit_verdict_json =
      CopyCompanyString(out.audit_verdict_json);

  PrintDivider();
  std::cout << "  Request ID    : " << out.request_id << "\n";
  if (request_info.contains("channel")) {
    std::cout << "  Channel       : "
              << request_info["channel"].get<std::string>() << "\n";
  }
  if (request_info.contains("dialogue")) {
    std::cout << "  Dialogue Text : \""
              << request_info["dialogue"].get<std::string>() << "\"\n";
  }
  std::cout << "  Risk Level    : " << risk_level << " (Score: " << std::fixed
            << std::setprecision(2) << out.risk_score << ")\n"
            << "  Matched Policy: "
            << (!matched_policy.empty() ? matched_policy : "none") << "\n"
            << "  Audit Verdict : " << audit_verdict_json << std::endl;

  *request_id = out.request_id;
  *status = out.status_code;
  if (request_info.contains("channel")) {
    (*sample_output)["channel"] = request_info["channel"];
  }
  (*sample_output)["risk_level"] = risk_level;
  (*sample_output)["risk_score"] = out.risk_score;
  (*sample_output)["matched_policy"] = matched_policy;
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
