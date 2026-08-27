#pragma once

#include <string>
#include <vector>

#include "biz/keyword_match/keyword_match_dto.h"
#include "core/blackboard_key.h"
#include "core/common_contracts.h"

namespace alg_framework {

inline constexpr char kKeywordMatchBizName[] = "keyword_match_v1";
inline constexpr char kKeywordMatchBusinessName[] = "keyword_match_v1";

inline constexpr BlackboardKey<std::vector<std::string>> kInputSentences{
    "input_sentences", "vector<string>"};

inline constexpr BlackboardKey<std::vector<KeywordMatchResult>>
    kKeywordMatchOutputs{"keyword_match_outputs", "vector<KeywordMatchResult>"};

}  // namespace alg_framework
