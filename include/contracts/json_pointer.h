#pragma once

#include <string>
#include <string_view>

namespace llm_edgeflow {

inline std::string EscapeJsonPointer(std::string_view token) {
  std::string escaped;
  escaped.reserve(token.size());
  for (char c : token) {
    if (c == '~') {
      escaped += "~0";
    } else if (c == '/') {
      escaped += "~1";
    } else {
      escaped += c;
    }
  }
  return escaped;
}

}  // namespace llm_edgeflow
