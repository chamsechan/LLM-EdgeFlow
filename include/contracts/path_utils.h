#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace llm_edgeflow {

// 纯词法辅助函数：归一化、平台策略、部署根目录、存在性检查和符号链接解析
// 均由调用方负责。
inline bool HasParentPathComponent(const std::filesystem::path& path) {
  for (const auto& component : path) {
    if (component == "..") return true;
  }
  return false;
}

// 按路径分量比较已归一化/规范化的路径 (含根目录本身)。字符串前缀比较不够：
// /root/models_extra 不在 /root/models 内。
inline bool IsPathWithinRoot(const std::filesystem::path& root,
                             const std::filesystem::path& candidate) {
  auto root_it = root.begin();
  auto candidate_it = candidate.begin();
  while (root_it != root.end() && candidate_it != candidate.end() &&
         *root_it == *candidate_it) {
    ++root_it;
    ++candidate_it;
  }
  return root_it == root.end();
}

// 文件名以所在文档目录为基准；目录未知时仅校验相对写法，不检查文件存在。
inline bool ResolveFileUnderDirectory(const std::filesystem::path& base_dir,
                                      const std::string& value,
                                      std::filesystem::path* resolved,
                                      std::string* error) {
  const auto fail = [&](const std::string& message) {
    if (error) *error = message;
    return false;
  };
  if (error) error->clear();
  if (!resolved) return fail("File resolution output is null");
  resolved->clear();
  std::string portable = value;
  std::replace(portable.begin(), portable.end(), '\\', '/');
  if (portable.empty() || portable.find('\0') != std::string::npos)
    return fail("File must be a nonempty relative filename");
  if (portable.front() == '/' ||
      (portable.size() >= 2 &&
       std::isalpha(static_cast<unsigned char>(portable[0])) &&
       portable[1] == ':'))
    return fail("File must be relative to its document directory");
  const std::filesystem::path relative(portable);
  if (relative.is_absolute() || HasParentPathComponent(relative))
    return fail("File cannot contain a parent path component");
  if (base_dir.empty()) {
    *resolved = relative.lexically_normal();
    return true;
  }
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(base_dir, ec);
  if (ec) return fail("Failed to resolve document directory: " + ec.message());
  const auto root = std::filesystem::weakly_canonical(absolute, ec);
  if (ec) return fail("Failed to resolve document directory: " + ec.message());
  const auto candidate = std::filesystem::weakly_canonical(root / relative, ec);
  if (ec) return fail("Failed to resolve file: " + ec.message());
  if (!IsPathWithinRoot(root, candidate))
    return fail("File escapes its document directory");
  *resolved = candidate;
  return true;
}

}  // namespace llm_edgeflow
