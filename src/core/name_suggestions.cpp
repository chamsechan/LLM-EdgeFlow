#include "name_suggestions.h"

#include <algorithm>
#include <utility>

namespace llm_edgeflow {

size_t LevenshteinDistance(std::string_view s1, std::string_view s2) {
  const size_t m = s1.size();
  const size_t n = s2.size();
  std::vector<size_t> dp(n + 1);
  for (size_t j = 0; j <= n; ++j) dp[j] = j;
  for (size_t i = 1; i <= m; ++i) {
    size_t prev = dp[0];
    dp[0] = i;
    for (size_t j = 1; j <= n; ++j) {
      size_t temp = dp[j];
      if (s1[i - 1] == s2[j - 1]) {
        dp[j] = prev;
      } else {
        dp[j] = 1 + std::min({prev, dp[j], dp[j - 1]});
      }
      prev = temp;
    }
  }
  return dp[n];
}

std::vector<std::string> RankByEditDistance(std::string_view target,
                                            std::vector<std::string> names) {
  std::vector<std::pair<size_t, std::string>> ranked;
  for (auto& name : names) {
    const size_t distance = LevenshteinDistance(target, name);
    ranked.emplace_back(distance, std::move(name));
  }
  std::sort(ranked.begin(), ranked.end());
  names.clear();
  for (auto& item : ranked) names.push_back(std::move(item.second));
  return names;
}

std::vector<std::string> NearestNames(std::string_view target,
                                      std::vector<std::string> names,
                                      size_t limit) {
  auto ranked = RankByEditDistance(target, std::move(names));
  std::vector<std::string> nearest;
  const size_t threshold = std::max(size_t{2}, target.size() / 3);
  for (auto& name : ranked) {
    if (nearest.size() == limit ||
        LevenshteinDistance(target, name) > threshold)
      break;
    nearest.push_back(std::move(name));
  }
  return nearest;
}

}  // namespace llm_edgeflow
