#pragma once

#include <cstddef>
#include <memory>

#include "edgeflow/export.h"
#include "edgeflow/operator/types.h"
#include "platform_mock/operator_types.h"

namespace llm_edgeflow::operator_api {

inline bool IsSupportedComputePlatform(ComputePlatform platform) noexcept {
  return platform == ComputePlatform::kAx650 ||
         platform == ComputePlatform::kAscend310P ||
         platform == ComputePlatform::kAscend910B ||
         platform == ComputePlatform::kRk3588 ||
         platform == ComputePlatform::kCuda ||
         platform == ComputePlatform::kCpu;
}

inline const char* ComputePlatformToString(ComputePlatform platform) noexcept {
  switch (platform) {
    case ComputePlatform::kAx650:
      return "AX650";
    case ComputePlatform::kAscend310P:
      return "ASCEND_310P";
    case ComputePlatform::kAscend910B:
      return "ASCEND_910B";
    case ComputePlatform::kRk3588:
      return "RK3588";
    case ComputePlatform::kCuda:
      return "CUDA";
    case ComputePlatform::kCpu:
      return "CPU";
    default:
      return "UNKNOWN";
  }
}

/**
 * @brief 辅助构造只读借用输入 shared_ptr (空 Deleter，不持有所有权)
 */
template <typename T>
inline std::shared_ptr<void> MakeBorrowedOperatorInput(const T* ptr) {
  return std::shared_ptr<void>(const_cast<void*>(static_cast<const void*>(ptr)),
                               [](void*) {});
}

/**
 * @brief 获取 Operator 函数表入口
 */
COMPANY_ALG_API OperatorFunc Get_LLM_EDGEFLOW_OperatorTable() noexcept;

/**
 * @brief 获取当前线程最近一次 Operator 门面结构化诊断错误信息
 */
COMPANY_ALG_API const char* GetOperatorLastError() noexcept;

/**
 * @brief 校验部署配置 .conf 与预期业务类型是否兼容
 * (只读无副作用预检，不抛出任何异常)
 * @param model_path 模型与配置根目录
 * @param cfg_file_name 相对配置文件路径
 * @param expected_biz_type 预期算法业务类型 (CompanyAlgBizType)
 * @param out_error_msg 错误输出信息缓冲区 (可选)
 * @param error_buf_size 缓冲区容量
 * @return 0 校验通过且兼容, -1 参数非法, -2 配置解析或文件不存在/逃逸, -3
 * 业务不匹配
 */
COMPANY_ALG_API int ValidateOperatorConfigBinding(
    const char* model_path, const char* cfg_file_name,
    int32_t expected_biz_type, char* out_error_msg = nullptr,
    size_t error_buf_size = 0) noexcept;

}  // namespace llm_edgeflow::operator_api
