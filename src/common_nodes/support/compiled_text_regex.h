#pragma once

#include <memory>
#include <string>
#include <unordered_map>

namespace llm_edgeflow {

enum class TextRegexSearchStatus { kMatched, kNotMatched, kError };

// Move-only RAII wrapper that keeps PCRE2 details out of capability Nodes.
class CompiledTextRegex final {
 public:
  CompiledTextRegex();
  ~CompiledTextRegex();

  CompiledTextRegex(CompiledTextRegex&&) noexcept;
  CompiledTextRegex& operator=(CompiledTextRegex&&) noexcept;

  CompiledTextRegex(const CompiledTextRegex&) = delete;
  CompiledTextRegex& operator=(const CompiledTextRegex&) = delete;

  bool Compile(const std::string& pattern, std::string* diagnostic);
  TextRegexSearchStatus Search(
      const std::string& subject,
      std::unordered_map<std::string, std::string>* named_captures,
      std::string* diagnostic) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace llm_edgeflow
