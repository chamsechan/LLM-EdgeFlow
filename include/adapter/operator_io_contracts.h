#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace llm_edgeflow {

enum class IoDirection { kUnknown, kInput, kOutput };

// Framework storage for immutable parameters. Structure authors use ordinary
// structs and MakeOutputParameterParser<T>; no inheritance is required.
struct OutputAllocationParameters {
  virtual ~OutputAllocationParameters() = default;
};

namespace detail {
template <typename T>
struct TypedOutputAllocationParameters final : OutputAllocationParameters {
  explicit TypedOutputAllocationParameters(T parameters)
      : value(std::move(parameters)) {}
  const T value;
};
}  // namespace detail

/**
 * @brief 单份输出的布局和分配参数，不包含队列深度或租约状态
 */
struct ResolvedOutputPoolSpec {
  std::string type;  // 规范输出后缀
  uint32_t meta_num = 0;
  int32_t metadata_type_id = 0;
  std::unordered_map<std::string, uint32_t> capacities;
  std::string allocator;  // Empty selects the ValueType default allocator.
  std::shared_ptr<const OutputAllocationParameters> params;

  template <typename T>
  const T& Parameters() const {
    const auto* typed =
        dynamic_cast<const detail::TypedOutputAllocationParameters<T>*>(
            params.get());
    if (!typed)
      throw std::invalid_argument("Output allocation parameter type mismatch");
    return typed->value;
  }

  uint32_t GetCapacity(const std::string& field) const noexcept {
    auto it = capacities.find(field);
    return it != capacities.end() ? it->second : 0;
  }
};

}  // namespace llm_edgeflow
