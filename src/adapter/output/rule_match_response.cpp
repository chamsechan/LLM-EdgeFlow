#include "adapter/output/rule_match_response.h"

#include <nlohmann/json.hpp>
#include <utility>

namespace llm_edgeflow {

std::string SerializeRuleMatchResponse(const RuleMatchItem& result) {
  nlohmann::json matches = nlohmann::json::array();
  for (const auto& hit : result.matches) {
    nlohmann::json entry;
    entry["category"] = hit.category;
    if (hit.source == RuleMatchSource::kRule) {
      entry["rule_id"] = hit.rule_id;
      entry["pattern"] = hit.pattern;
      entry["score"] = hit.score;
    } else {
      entry["matched_word"] = hit.pattern;
    }
    matches.push_back(std::move(entry));
  }

  nlohmann::json response;
  response["matches"] = std::move(matches);
  if (result.is_hit && !result.category.empty()) {
    response["intent"] = result.category;
    response["matched_word"] = result.matched_word;
    response["confidence"] = result.score;
    response["slots"] = result.slots;
  }
  return response.dump();
}

}  // namespace llm_edgeflow
