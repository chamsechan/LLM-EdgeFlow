#pragma once

#include <string>
#include <vector>

#include "business/entity_extract/entity_extract_dto.h"
#include "core/blackboard_key.h"
#include "core/common_contracts.h"

namespace alg_framework {

inline constexpr char kEntityExtractBusinessName[] = "entity_extract_0.6b_v1";
inline constexpr char kEntityExtractLlamaCppBusinessName[] =
    "entity_extract_llamacpp_0.6b_v1";

inline constexpr BlackboardKey<std::vector<std::string>> kInputSentences{
    "input_sentences", "vector<string>"};

inline constexpr BlackboardKey<std::vector<EntityExtractResult>>
    kEntityExtractOutputs{"entity_extract_outputs",
                          "vector<EntityExtractResult>"};

}  // namespace alg_framework
