#pragma once

#include <string>

#include "core/common_contracts.h"

namespace llm_edgeflow {

// 将规则匹配结果序列化为 JSON 对象，供关键词响应 (match_result_json)
// 和音频响应 (intent_slot_json) 使用。
std::string SerializeRuleMatchResponse(const RuleMatchItem& result);

}  // namespace llm_edgeflow
