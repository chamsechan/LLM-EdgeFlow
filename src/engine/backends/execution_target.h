#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace llm_edgeflow {

inline std::string NormalizeExecutionPlatform(std::string platform) {
  std::transform(
      platform.begin(), platform.end(), platform.begin(),
      [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return platform;
}

}  // namespace llm_edgeflow
