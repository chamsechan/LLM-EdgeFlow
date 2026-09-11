#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "adapter/biz_input_constraints.h"
#include "adapter/operator_value_type.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {

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
 * @brief CompanyAny 白名单类型描述符
 */
struct CompanyAnyTypeDescriptor {
  int32_t type_id = 0;
  size_t element_size = 0;
  size_t alignment = 0;
  const char* debug_name = nullptr;
};

/**
 * @brief 根据 type_id 查找 CompanyAny 元素类型描述 (白名单)
 */
const CompanyAnyTypeDescriptor* FindCompanyAnyType(int32_t type_id) noexcept;

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
 * 计入外层 Operator 镜像结构、嵌套 CompanyString/CompanyAny 结构及其数据区；
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
   * @brief 校验 CompanyString 合法性 (带显式长度、上限与嵌入 NUL 检查)
   */
  static int ValidateCompanyString(const CompanyString* str, size_t max_bytes,
                                   const char* field_name,
                                   std::string* err) noexcept;

  /**
   * @brief 校验 CompanyBuffer 合法性
   */
  static int ValidateCompanyBuffer(const CompanyBuffer* buf, size_t max_bytes,
                                   const char* field_name,
                                   std::string* err) noexcept;

  /**
   * @brief 校验 CompanyAny 合法性 (受类型白名单与尺寸乘法方程校验)
   */
  static int ValidateCompanyAnyPayload(const CompanyAny* any,
                                       size_t max_any_bytes,
                                       const char* field_name,
                                       std::string* err) noexcept;

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

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, OperatorValueTypeBinding>
      bindings_by_canonical_;
  std::unordered_map<std::string, OperatorValueTypeBinding> output_allocators_;
  bool has_conflict_ = false;
  bool audited_ = false;
};

}  // namespace llm_edgeflow
