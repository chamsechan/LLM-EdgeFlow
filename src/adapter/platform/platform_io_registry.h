#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "company_alg_interface.h"
#include "platform/platform_operator_interface.h"

namespace alg_framework {

enum class IoDirection { kInput, kOutput };

/**
 * @brief 命名 I/O 槽位别名组描述符 (明确表达主名与别名关系，消除歧义)
 */
struct PlatformIoSlotGroup {
  std::string canonical_suffix;
  std::vector<std::string> aliases;
  std::string c_type_name;
  IoDirection direction;
  bool is_required = true;

  bool Matches(const std::string& suffix) const noexcept {
    if (suffix == canonical_suffix) return true;
    for (const auto& a : aliases) {
      if (suffix == a) return true;
    }
    return false;
  }
};

struct PlatformIoDescriptor {
  CompanyAlgBizType biz_type;
  std::string biz_name;
  std::vector<PlatformIoSlotGroup> input_groups;
  std::vector<PlatformIoSlotGroup> output_groups;
};

/**
 * @brief 平台命名 I/O 注册与派发转换器 (零拷贝提取 shared_ptr 指针)
 */
class PlatformIoRegistry {
 public:
  static PlatformIoRegistry& Instance();

  /**
   * @brief 解析命名 I/O Key，提取后缀
   * @param[in] key 原始 Key (如 "camera_0.frame")
   * @param[out] out_namespace 前缀命名空间
   * @param[out] out_suffix 类型后缀 (最后一个点号之后)
   * @return true 解析成功, false 非法格式 (无点号、首尾点号等)
   */
  static bool ParseKey(const std::string& key, std::string* out_namespace,
                       std::string* out_suffix) noexcept;

  /**
   * @brief 显式注册平台 I/O 描述符 (带重复检测与冲突标记)
   */
  bool RegisterDescriptor(const PlatformIoDescriptor& desc);

  /**
   * @brief 校验描述符与业务 Adapter 及第一阶段单 DTO 契约是否一致。
   *
   * 该方法不修改注册中心状态，便于注册前检查和单元测试。
   */
  static bool ValidateDescriptor(const PlatformIoDescriptor& desc,
                                 std::string* error_msg = nullptr) noexcept;

  /**
   * @brief 根据业务类型获取平台 I/O 契约描述符
   */
  const PlatformIoDescriptor* GetDescriptor(CompanyAlgBizType biz_type) const;

  /**
   * @brief 校验并从 NamedIoBatch 提取底层 C 输入结构体指针数组 (零拷贝)
   */
  int ExtractInputs(CompanyAlgBizType biz_type,
                    const llm_edgeflow::platform::NamedIoBatch& inputs,
                    std::vector<const void*>* out_ptrs,
                    std::string* error_msg) const noexcept;

  /**
   * @brief 校验并从 NamedIoBatch 提取底层 C 输出结构体指针数组 (零拷贝)
   */
  int ExtractOutputs(CompanyAlgBizType biz_type,
                     const llm_edgeflow::platform::NamedIoBatch& outputs,
                     std::vector<void*>* out_ptrs,
                     std::string* error_msg) const noexcept;

  /**
   * @brief 检查注册中心是否存在注册冲突 (Fail-Closed 审计)
   */
  bool HasConflict() const;

 private:
  PlatformIoRegistry();
  void RegisterDefaults();

  mutable std::recursive_mutex mutex_;
  std::unordered_map<int, PlatformIoDescriptor> descriptors_;
  bool has_conflict_ = false;
  std::vector<std::string> registration_errors_;
};

}  // namespace alg_framework
