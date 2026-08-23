#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "company_alg_interface.h"
#include "platform/platform_operator_interface.h"

namespace alg_framework {

enum class IoDirection { kInput, kOutput };

struct PlatformIoSlotDescriptor {
  std::string suffix;
  std::string c_type_name;
  IoDirection direction;
  bool required = true;
};

struct PlatformIoDescriptor {
  CompanyAlgBizType biz_type;
  std::vector<PlatformIoSlotDescriptor> slots;
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

 private:
  PlatformIoRegistry();
  void RegisterDefaults();

  std::unordered_map<int, PlatformIoDescriptor> descriptors_;
};

}  // namespace alg_framework
