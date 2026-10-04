#pragma once

#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "contracts/diagnostic.h"
#include "contracts/registry_conflicts.h"

namespace llm_edgeflow::registry_support {

template <typename Mutex>
void RecordConflict(Mutex& mutex, RegistryConflicts& conflicts,
                    std::string_view error) noexcept {
  try {
    std::lock_guard<Mutex> lock(mutex);
    std::string message;
    try {
      message.assign(error);
    } catch (...) {
    }
    conflicts.Record(std::move(message));
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
bool HasConflict(Mutex& mutex, const RegistryConflicts& conflicts) noexcept {
  try {
    std::lock_guard<Mutex> lock(mutex);
    return conflicts.HasConflict();
  } catch (...) {
    return true;
  }
}

template <typename Mutex>
std::vector<std::string> GetConflictErrors(Mutex& mutex,
                                           const RegistryConflicts& conflicts) {
  std::lock_guard<Mutex> lock(mutex);
  return conflicts.Messages();
}

}  // namespace llm_edgeflow::registry_support
