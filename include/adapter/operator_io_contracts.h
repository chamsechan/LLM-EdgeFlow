#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace llm_edgeflow {

enum class IoDirection { kUnknown, kInput, kOutput };

/**
 * @brief 输出池规范
 */
struct ResolvedOutputPoolSpec {
  std::string type;  // 规范输出后缀
  uint32_t meta_num = 0;
  int32_t metadata_type_id = 0;
  std::unordered_map<std::string, uint32_t> capacities;

  uint32_t GetCapacity(const std::string& field) const noexcept {
    auto it = capacities.find(field);
    return it != capacities.end() ? it->second : 0;
  }
};

}  // namespace llm_edgeflow
