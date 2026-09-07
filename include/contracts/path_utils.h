#pragma once

#include <filesystem>

namespace llm_edgeflow {

// Pure lexical helpers: callers own normalization, platform-specific policies,
// deployment roots, existence checks, and symlink resolution.
inline bool HasParentPathComponent(const std::filesystem::path& path) {
  for (const auto& component : path) {
    if (component == "..") return true;
  }
  return false;
}

// Compare normalized/canonical paths by components, including the root itself.
// A string prefix is insufficient: /root/models_extra is outside /root/models.
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
