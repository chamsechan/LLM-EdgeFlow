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

// Only {{name}} placeholders are variables. Single braces remain literal (for
// example JSON); double braces never escape variables. Only compile the
// original template, never inserted request text.
inline bool ParseTextTemplate(const std::string& pattern,
                              std::vector<TextTemplateToken>* tokens,
                              std::string* error = nullptr) {
  auto reject = [&](const std::string& message) {
    tokens->clear();
    if (error) *error = message;
    return false;
  };
  auto add_literal = [&](std::string_view lit) {
    if (lit.empty()) return;
    if (!tokens->empty() &&
        tokens->back().type == TextTemplateTokenType::kLiteral) {
      tokens->back().value.append(lit);
    } else {
      tokens->push_back({TextTemplateTokenType::kLiteral, std::string(lit)});
    }
  };
  tokens->clear();
  size_t pos = 0;
  while (pos < pattern.size()) {
    const size_t open = pattern.find('{', pos);
    if (open == std::string::npos) {
      add_literal(pattern.substr(pos));
      break;
    }
    if (open > pos) {
      add_literal(pattern.substr(pos, open - pos));
    }
    const bool is_double =
        open + 1 < pattern.size() && pattern[open + 1] == '{';
    if (!is_double) {
      add_literal("{");
      pos = open + 1;
      continue;
    }
    const size_t close = pattern.find("}}", open + 2);
    if (close == std::string::npos) {
      return reject("Unclosed {{ placeholder in template");
    }
    const std::string raw = pattern.substr(open + 2, close - open - 2);
    const size_t first = raw.find_first_not_of(" \t");
    const size_t last = raw.find_last_not_of(" \t");
    const std::string name = first == std::string::npos
                                 ? std::string{}
                                 : raw.substr(first, last - first + 1);
    if (!IsTextTemplateIdentifier(name)) {
      return reject("Invalid {{name}} template placeholder: " + raw);
    }
    tokens->push_back({TextTemplateTokenType::kVariable, name});
    pos = close + 2;
  }
  return true;
}

}  // namespace llm_edgeflow
