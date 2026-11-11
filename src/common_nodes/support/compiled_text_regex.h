#pragma once

#include <memory>
#include <string>
#include <unordered_map>

namespace llm_edgeflow {

enum class TextRegexSearchStatus { kMatched, kNotMatched, kError };

// 仅可移动的 RAII 包装，使 PCRE2 细节不进入能力节点。
class CompiledTextRegex final {
 public:
  CompiledTextRegex();
  ~CompiledTextRegex();

  CompiledTextRegex(CompiledTextRegex&&) noexcept;
  CompiledTextRegex& operator=(CompiledTextRegex&&) noexcept;

  CompiledTextRegex(const CompiledTextRegex&) = delete;
  CompiledTextRegex& operator=(const CompiledTextRegex&) = delete;

  /**
   * @brief 以 UTF-8 和 Unicode 字符属性编译模式，并预先提取命名捕获组元数据。
   * 成功后替换现有模式，失败时保留原模式并给出诊断。
   */
  bool Compile(const std::string& pattern, std::string* diagnostic);
  /**
   * @brief 搜索首个匹配并复制命名捕获内容；未参与匹配的命名组对应空字符串。
   * 每次调用清空捕获结果和诊断，区分未命中与执行错误；未编译模式按未命中处理。
   */
  TextRegexSearchStatus Search(
      const std::string& subject,
      std::unordered_map<std::string, std::string>* named_captures,
      std::string* diagnostic) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace llm_edgeflow
