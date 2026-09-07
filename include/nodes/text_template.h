#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace llm_edgeflow {

enum class TextTemplateTokenType { kLiteral, kVariable };

struct TextTemplateToken {
  TextTemplateTokenType type = TextTemplateTokenType::kLiteral;
  std::string value;
};

inline bool IsTextTemplateIdentifier(std::string_view name) {
  if (name.empty()) return false;
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_')) {
      return false;
    }
  }
  return true;
}

// Both {{name}} and the compatible {name} spelling are variables. Other
// single braces remain literal (for example JSON); double braces never escape
// variables. Only compile the original template, never inserted request text.
inline bool ParseTextTemplate(const std::string& pattern,
                              std::vector<TextTemplateToken>* tokens,
                              std::string* error = nullptr) {
  auto reject = [&](const std::string& message) {
    tokens->clear();
    if (error) *error = message;
    return false;
  };
  tokens->clear();
  size_t pos = 0;
  while (pos < pattern.size()) {
    const size_t open = pattern.find('{', pos);
    if (open == std::string::npos) {
      tokens->push_back({TextTemplateTokenType::kLiteral, pattern.substr(pos)});
      break;
    }
    if (open > pos) {
      tokens->push_back(
          {TextTemplateTokenType::kLiteral, pattern.substr(pos, open - pos)});
    }
    const bool is_double =
        open + 1 < pattern.size() && pattern[open + 1] == '{';
    const size_t width = is_double ? 2 : 1;
    const size_t close = pattern.find(is_double ? "}}" : "}", open + width);
    if (close == std::string::npos && is_double) {
      return reject("Unclosed {{ placeholder in template");
    }
    if (close != std::string::npos) {
      const std::string raw =
          pattern.substr(open + width, close - open - width);
      const size_t first = raw.find_first_not_of(" \t");
      const size_t last = raw.find_last_not_of(" \t");
      const std::string name = first == std::string::npos
                                   ? std::string{}
                                   : raw.substr(first, last - first + 1);
      if (IsTextTemplateIdentifier(name)) {
        tokens->push_back({TextTemplateTokenType::kVariable, name});
        pos = close + width;
        continue;
      }
      if (is_double) {
        return reject("Invalid {{name}} template placeholder: " + raw);
      }
    }
    tokens->push_back({TextTemplateTokenType::kLiteral, "{"});
    pos = open + 1;
  }
  return true;
}

}  // namespace llm_edgeflow
