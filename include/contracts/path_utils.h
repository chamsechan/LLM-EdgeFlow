#pragma once

#include <filesystem>

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

}  // namespace llm_edgeflow
