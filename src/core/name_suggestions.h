#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace llm_edgeflow {

size_t LevenshteinDistance(std::string_view s1, std::string_view s2);

// 按编辑距离排序名称；距离相同时按名称排序，保证输出确定。
std::vector<std::string> RankByEditDistance(std::string_view target,
                                            std::vector<std::string> names);

// 最多 `limit` 个与 `target` 足够接近、可能是拼写错误的名称。
std::vector<std::string> NearestNames(std::string_view target,
                                      std::vector<std::string> names,
                                      size_t limit = 3);

}  // namespace llm_edgeflow
