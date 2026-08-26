#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "platform/company_platform_types.h"

namespace alg_framework {

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

  uint32_t GetCapacity(const std::string& field, uint32_t default_val) const {
    auto it = capacities.find(field);
    if (it != capacities.end() && it->second > 0) {
      return it->second;
    }
    return default_val;
  }
};

/**
 * @brief 由输出池持有所有权的外部结构块
 */
struct OwnedExternalBlock {
  void* raw_struct = nullptr;
  std::vector<void*> owned_nested_buffers;
  std::vector<CompanyString*> nested_company_strings;
  ResolvedOutputPoolSpec spec;
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
 * @brief 平台值类型绑定描述符
 */
struct PlatformValueTypeBinding {
  std::string canonical_suffix;
  std::vector<std::string> aliases;
  std::string external_c_type_name;
  ValidateExternalFn validate_external;
  AllocateExternalFn allocate_external;
  ResetExternalFn reset_external;
  DestroyExternalFn destroy_external;
};

/**
 * @brief 平台全局值类型表 (SSOT)
 */
class PlatformValueTypeRegistry {
 public:
  static PlatformValueTypeRegistry& Instance();

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
  static int ValidateCompanyAnyPayload(const CompanyAny* any, size_t max_bytes,
                                       const char* field_name,
                                       std::string* err) noexcept;

  /**
   * @brief 注册值类型绑定 (全量预检后原子写入)
   */
  bool RegisterBinding(PlatformValueTypeBinding binding);

  /**
   * @brief 根据后缀 (规范名或别名) 获取绑定
   */
  const PlatformValueTypeBinding* GetBindingBySuffix(
      const std::string& suffix) const;

  /**
   * @brief 将可能为别名的后缀归一化为规范后缀
   */
  std::string NormalizeSuffix(const std::string& suffix) const;

  /**
   * @brief 执行全局审计 (Fail-Closed 校验重名/别名冲突/缺失工厂)
   */
  int GlobalInit();

  /**
   * @brief 检查是否存在冲突
   */
  bool HasConflict() const { return has_conflict_; }

 private:
  PlatformValueTypeRegistry();
  void RegisterBuiltinBindings();

  bool has_conflict_ = false;
  bool audited_ = false;
  std::unordered_map<std::string, PlatformValueTypeBinding>
      bindings_by_canonical_;
  std::unordered_map<std::string, std::string> alias_to_canonical_;
};

}  // namespace alg_framework
