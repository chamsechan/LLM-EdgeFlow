#pragma once

#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace llm_edgeflow::registry_support {

inline void SetDiagnostic(std::string* diagnostic,
                          const std::string& message) noexcept {
  if (!diagnostic) return;
  try {
    *diagnostic = message;
  } catch (...) {
  }
}

template <typename Mutex>
void RecordConflict(Mutex& mutex, bool& has_conflict,
                    std::vector<std::string>& errors,
                    std::string error) noexcept {
  try {
    std::lock_guard<Mutex> lock(mutex);
    has_conflict = true;
    try {
      errors.push_back(std::move(error));
    } catch (...) {
    }
  } catch (...) {
  }
}

template <typename Definition, typename Mutex, typename EntryMap>
std::optional<Definition> FindDefinition(Mutex& mutex, const EntryMap& entries,
                                         const std::string& type) noexcept {
  try {
    std::lock_guard<Mutex> lock(mutex);
    const auto it = entries.find(type);
    if (it == entries.end()) return std::nullopt;
    return it->second.definition;
  } catch (...) {
    return std::nullopt;
  }
}

template <typename Mutex, typename EntryMap>
bool HasType(Mutex& mutex, const EntryMap& entries,
             const std::string& type) noexcept {
  try {
    std::lock_guard<Mutex> lock(mutex);
    return entries.find(type) != entries.end();
  } catch (...) {
    return false;
  }
}

template <typename Mutex, typename EntryMap>
std::vector<std::string> ListTypes(Mutex& mutex, const EntryMap& entries) {
  std::lock_guard<Mutex> lock(mutex);
  std::vector<std::string> result;
  result.reserve(entries.size());
  for (const auto& item : entries) result.push_back(item.first);
  std::sort(result.begin(), result.end());
  return result;
}

template <typename Definition, typename Mutex, typename EntryMap, typename Less>
std::vector<Definition> ListDefinitions(Mutex& mutex, const EntryMap& entries,
                                        Less less) {
  std::lock_guard<Mutex> lock(mutex);
  std::vector<Definition> result;
  result.reserve(entries.size());
  for (const auto& item : entries) result.push_back(item.second.definition);
  std::sort(result.begin(), result.end(), std::move(less));
  return result;
}

template <typename Mutex>
bool HasConflict(Mutex& mutex, const bool& has_conflict) noexcept {
  try {
    std::lock_guard<Mutex> lock(mutex);
    return has_conflict;
  } catch (...) {
    return true;
  }
}

template <typename Mutex>
std::vector<std::string> GetConflictErrors(
    Mutex& mutex, const std::vector<std::string>& errors) {
  std::lock_guard<Mutex> lock(mutex);
  return errors;
}

template <typename Mutex, typename EntryMap>
void Clear(Mutex& mutex, EntryMap& entries, bool& has_conflict,
           std::vector<std::string>& errors) {
  std::lock_guard<Mutex> lock(mutex);
  entries.clear();
  has_conflict = false;
  errors.clear();
}

}  // namespace llm_edgeflow::registry_support
