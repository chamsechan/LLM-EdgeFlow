#pragma once

#include <string>
#include <vector>

#include "business/dialogue_audit/dialogue_audit_dto.h"
#include "core/blackboard_key.h"
#include "core/common_contracts.h"
#include "core/traceable_item.h"

namespace alg_framework {

inline constexpr BlackboardKey<std::vector<std::string>> kUserTexts{
    "user_texts", "vector<string>"};

inline constexpr BlackboardKey<std::vector<std::string>> kChannelNames{
    "channel_names", "vector<string>"};

inline constexpr BlackboardKey<std::vector<bool>> kHardRiskFlags{
    "hard_risk_flags", "vector<bool>"};

inline constexpr BlackboardKey<std::vector<std::string>> kHitKeywords{
    "hit_keywords", "vector<string>"};

inline constexpr BlackboardKey<
    std::vector<TraceableItem<std::vector<std::string>>>>
    kCandidatePolicies{"candidate_policies", "traceable<vector<string>>[]"};

inline constexpr BlackboardKey<std::vector<std::string>> kMatchedPolicyClauses{
    "matched_policy_clauses", "vector<string>"};

inline constexpr BlackboardKey<std::vector<float>> kRerankScores{
    "rerank_scores", "vector<float>"};

inline constexpr BlackboardKey<std::vector<TraceableItem<std::string>>>
    kLlmAuditPrompts{"llm_audit_prompts", "traceable<string>[]"};

inline constexpr BlackboardKey<std::vector<TraceableItem<std::string>>>
    kGeneratedVerdicts{"generated_verdicts", "traceable<string>[]"};

inline constexpr BlackboardKey<std::vector<DialogueAuditResult>>
    kComplianceAuditOutputs{"compliance_audit_outputs",
                            "vector<DialogueAuditResult>"};

}  // namespace alg_framework
