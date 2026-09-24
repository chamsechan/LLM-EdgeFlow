#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "edgeflow/export.h"
#include "edgeflow/operator/types.h"
#include "edgeflow/version.h"
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
 * @brief 解析部署配置并返回由 I/O 绑定确定的业务契约名。
 * 只读预检，不加载模型、不执行转换；同业务的绑定须有一致的外部契约。
 * @param out_biz_name 必需的结果指针；失败时为空，成功时为完整业务名。
 * @return 0 成功，-2 参数/配置错误，其他负值为验证或内部异常。
 */
COMPANY_ALG_API int ResolveOperatorConfigBiz(
    const char* model_path, const char* cfg_file_name,
    std::string* out_biz_name, char* out_error_msg = nullptr,
    size_t error_buf_size = 0) noexcept;

}  // namespace llm_edgeflow::operator_api
