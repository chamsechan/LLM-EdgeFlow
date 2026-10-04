#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace llm_edgeflow {

size_t LevenshteinDistance(std::string_view s1, std::string_view s2);

// Orders names by edit distance; ties use the name for deterministic output.
std::vector<std::string> RankByEditDistance(std::string_view target,
                                            std::vector<std::string> names);

// Up to `limit` names close enough to `target` to be a likely typo.
std::vector<std::string> NearestNames(std::string_view target,
                                      std::vector<std::string> names,
                                      size_t limit = 3);

}  // namespace llm_edgeflow
