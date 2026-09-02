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

#include "operator/company_operator_types.h"

namespace llm_edgeflow {

enum class IoDirection { kUnknown, kInput, kOutput };

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
 * @brief 输入限制配置
 */
struct ResolvedInputLimits {
  size_t max_text_bytes = 64 * 1024;              // 64 KiB
  size_t max_doc_text_bytes = 10 * 1024 * 1024;   // 10 MiB
  size_t max_image_uri_bytes = 4096;              // 4 KiB
  int32_t max_audio_pcm_samples = 960000;         // 960k samples
  size_t max_audio_pcm_bytes = 10 * 1024 * 1024;  // 10 MiB
  int32_t min_sample_rate = 8000;
  int32_t max_sample_rate = 192000;
  int32_t max_rerank_candidates = 8;
  size_t max_buffer_bytes = 10 * 1024 * 1024;  // 10 MiB
  size_t max_any_bytes = 10 * 1024 * 1024;     // 10 MiB
};

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

struct OutputCapacityFieldConfig {
  uint32_t default_capacity = 0;
  uint32_t max_capacity = 0;
};

using ComputeOutputBlockPayloadBytesFn = std::function<bool(
    const ResolvedOutputPoolSpec& spec, size_t* out_bytes, std::string* err)>;

/**
 * @brief Operator 输出类型的容量与内存布局契约
 */
struct OperatorOutputLayoutDescriptor {
  // 每个字段对应输出镜像结构中的一个 CompanyString 指针。
  std::unordered_map<std::string, OutputCapacityFieldConfig>
      string_capacity_fields;
  uint32_t max_metadata_elements = 0;
  ComputeOutputBlockPayloadBytesFn compute_block_payload_bytes;
};

/**
 * @brief 移动语义清理动作 (无需 std::function 堆分配或控制块)
 */
struct CleanupAction {
  void* ptr = nullptr;
  void (*deleter)(void*) noexcept = nullptr;

  void Execute() noexcept {
    if (ptr && deleter) {
      deleter(ptr);
      ptr = nullptr;
    }
  }
};

/**
 * @brief 由输出池持有所有权的外部结构块 (具备完整 RAII 自动回滚与类型安全清理)
 */
struct OwnedExternalBlock {
  void* raw_struct = nullptr;
  std::vector<CleanupAction> cleanups;

  OwnedExternalBlock() = default;
  ~OwnedExternalBlock() { Destroy(); }

  OwnedExternalBlock(OwnedExternalBlock&& other) noexcept
      : raw_struct(other.raw_struct), cleanups(std::move(other.cleanups)) {
    other.raw_struct = nullptr;
  }

  OwnedExternalBlock& operator=(OwnedExternalBlock&& other) noexcept {
    if (this != &other) {
      Destroy();
      raw_struct = other.raw_struct;
      cleanups = std::move(other.cleanups);
      other.raw_struct = nullptr;
    }
    return *this;
  }

  OwnedExternalBlock(const OwnedExternalBlock&) = delete;
  OwnedExternalBlock& operator=(const OwnedExternalBlock&) = delete;

  void Destroy() noexcept {
    for (auto it = cleanups.rbegin(); it != cleanups.rend(); ++it) {
      it->Execute();
    }
    cleanups.clear();
    raw_struct = nullptr;
  }
};

using ValidateExternalFn = std::function<int(
    const void* ptr, const ResolvedInputLimits& limits, std::string* err)>;

using AllocateExternalFn =
    std::function<int(const ResolvedOutputPoolSpec& spec,
                      OwnedExternalBlock* out_block, std::string* err)>;

using ResetExternalFn =
    std::function<void(void* ptr, const ResolvedOutputPoolSpec& spec)>;

using DestroyExternalFn = std::function<void(OwnedExternalBlock* block)>;

/**
 * @brief Operator 值类型绑定描述符
 */
struct OperatorValueTypeBinding {
  std::string canonical_suffix;
  std::string external_c_type_name;
  IoDirection direction = IoDirection::kUnknown;
  OperatorOutputLayoutDescriptor output_layout;
  ValidateExternalFn validate_external;
  AllocateExternalFn allocate_external;
  ResetExternalFn reset_external;
  DestroyExternalFn destroy_external;
};

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

  /**
   * @brief 根据规范后缀获取绑定描述符
   */
  const OperatorValueTypeBinding* GetBindingBySuffix(
      const std::string& suffix) const;

  /**
   * @brief 测试专用的分配故障注入探针 (非公开 ABI，仅单测使用)
   */
  static void SetAllocationFailureCountdown(int count) noexcept;
  static int GetAllocationFailureCountdown() noexcept;

  enum class RegistryExceptionInjectPoint {
    kNone = 0,
    kCopyCanonicalMap,
    kCanonicalInsert,
    kPublish
  };

  static void SetExceptionInjectPoint(
      RegistryExceptionInjectPoint point) noexcept;
  static RegistryExceptionInjectPoint GetExceptionInjectPoint() noexcept;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, OperatorValueTypeBinding>
      bindings_by_canonical_;
  bool has_conflict_ = false;
  bool audited_ = false;
};

}  // namespace llm_edgeflow
