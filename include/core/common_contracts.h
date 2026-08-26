#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/blackboard_key.h"
#include "core/traceable_item.h"

namespace alg_framework {

inline constexpr BlackboardKey<std::vector<uint64_t>> kRawRequestIds{
    "raw_request_ids", "vector<uint64>"};

inline constexpr BlackboardKey<std::vector<std::string>> kRawQueries{
    "raw_queries", "vector<string>"};

inline constexpr BlackboardKey<std::vector<TraceableItem<std::string>>>
    kLlmInputPrompts{"llm_input_prompts", "traceable<string>[]"};

inline constexpr BlackboardKey<std::vector<TraceableItem<std::string>>>
    kGeneratedLlmAnswers{"generated_llm_answers", "traceable<string>[]"};

}  // namespace alg_framework
