#pragma once

#include <nlohmann/json.hpp>

#include "core/common_contracts.h"

namespace llm_edgeflow {

// 构造规则匹配对象，供关键词响应和音频响应的 intent_slot 字段使用。
nlohmann::json BuildRuleMatchResponse(const RuleMatchItem& result);

}  // namespace llm_edgeflow
