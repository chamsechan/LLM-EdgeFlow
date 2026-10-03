#pragma once

#include <string>

#include "core/common_contracts.h"

namespace llm_edgeflow {

// Serializes a rule match result as the JSON object used by the keyword
// response (match_result_json) and the audio response (intent_slot_json).
std::string SerializeRuleMatchResponse(const RuleMatchItem& result);

}  // namespace llm_edgeflow
