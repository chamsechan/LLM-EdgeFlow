#pragma once

#include <cstdint>
#include <string>

namespace alg_framework {

/**
 * @brief 对话风控质检业务结果领域 DTO (Layer 3 <-> Layer 1 解耦)
 */
struct DialogueAuditResult {
  uint64_t request_id = 0;
  std::string risk_level;
  float risk_score = 0.0f;
  std::string matched_policy_clause;
  std::string audit_verdict_json;
  int status_code = 0;
};

}  // namespace alg_framework
