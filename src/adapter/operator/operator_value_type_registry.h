#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "adapter/input_limits.h"
#include "adapter/operator_value_type.h"

namespace llm_edgeflow {
namespace test_support {
class RegistryTestAccess;
}

inline constexpr uint32_t kDefaultOutputPoolDepth = 25;
inline constexpr uint32_t kMaxOutputPoolDepth = 1024;
inline constexpr size_t kMaxHandlePoolPayloadBytes =
    64 * 1024 * 1024;  // 64 MiB

/**
 * @brief 安全乘法助手函数 (checked-multiply)
 */
inline bool CheckedMultiply(size_t lhs, size_t rhs, size_t* out) noexcept {
  if (!out) return false;
  if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
    return false;
  }
  *out = lhs * rhs;
  return true;
}

/**
 * @brief 安全加法助手函数 (checked-add)
 */
inline bool CheckedAdd(size_t lhs, size_t rhs, size_t* out) noexcept {
  if (!out) return false;
  if (rhs > std::numeric_limits<size_t>::max() - lhs) {
    return false;
  }
  *out = lhs + rhs;
  return true;
}

/**
 * @brief 按值类型 Schema 校验并补齐输出池规范
 */
bool ResolveOutputPoolSpec(const OperatorValueTypeBinding& binding,
                           const ResolvedOutputPoolSpec& requested,
                           ResolvedOutputPoolSpec* resolved,
                           std::string* err) noexcept;

/**
 * @brief 计算输出池预分配业务载荷的确定性字节数
 *
 * 由 binding 计入外层结构、嵌套布局及其数据区；
 * 不把 STL 容器、allocator、控制块等实现相关管理开销伪装成可精确计算的载荷。
 */
bool ComputeOutputPoolPayloadBytes(const OperatorValueTypeBinding& binding,
                                   const ResolvedOutputPoolSpec& spec,
                                   uint32_t depth, size_t* out_bytes,
                                   std::string* err) noexcept;

bool ComputeOutputPoolPayloadBytes(const std::string& suffix,
                                   const ResolvedOutputPoolSpec& spec,
                                   uint32_t depth, size_t* out_bytes,
                                   std::string* err) noexcept;

/**
 * @brief Operator 全局值类型表 (SSOT)
 */
class OperatorValueTypeRegistry {
 public:
  static OperatorValueTypeRegistry& Instance();

  OperatorValueTypeRegistry();
  void RegisterBuiltinBindings();

  /**
   * @brief 解析 Key (例如 "camera_0.frame") 提取命名空间和后缀
   */
  static bool ParseKey(const std::string& key, std::string* out_namespace,
                       std::string* out_suffix) noexcept;

  /**
   * @brief 检查是否存在冲突
   */
  bool HasConflict() const;

  /**
   * @brief 全局初始化并冻结注册表 (幂等安全，若存在冲突返回 -6)
   */
  int GlobalInit();

  /**
   * @brief 注册值类型绑定 (在写入前执行严格的原子预检)
   */
  bool RegisterBinding(const OperatorValueTypeBinding& binding);
  bool RegisterOutputAllocator(const std::string& name,
                               const OperatorValueTypeBinding& binding);
  const OperatorValueTypeBinding* GetOutputBinding(
      const std::string& suffix, const std::string& allocator) const;

  /**
   * @brief 根据规范后缀获取绑定描述符
   */
  const OperatorValueTypeBinding* GetBindingBySuffix(
      const std::string& suffix) const;

  // Prepared plans own snapshots, including before GlobalInit freezes the
  // table.
  std::optional<OperatorValueTypeBinding> CopyBindingBySuffix(
      const std::string& suffix) const;
  std::optional<OperatorValueTypeBinding> CopyOutputBinding(
      const std::string& suffix, const std::string& allocator) const;

 private:
  friend class test_support::RegistryTestAccess;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, OperatorValueTypeBinding>
      bindings_by_canonical_;
  std::unordered_map<std::string, OperatorValueTypeBinding> output_allocators_;
  bool has_conflict_ = false;
  bool audited_ = false;
};

}  // namespace llm_edgeflow
