#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

// Within a direction, repeated types use name.type as the host key.
// A unique type accepts any nonempty key namespace.
struct OperatorIoEntry {
  std::string type;
  std::string name;
  std::string type_name;
  std::optional<int32_t> service_type;
  bool required = true;
};

struct OperatorIoContract {
  std::vector<OperatorIoEntry> inputs;
  std::vector<OperatorIoEntry> outputs;
};

/**
 * @brief 只读解析和校验部署配置，返回宿主 I/O 契约，不加载模型或执行转换。
 * @param out 必需的结果指针；失败时清空两侧条目。
 * @return 0 成功，-2 参数/配置错误，其他负值为验证或内部异常。
 */
COMPANY_ALG_API int ResolveOperatorConfigIo(const char* model_path,
                                            const char* cfg_file_name,
                                            OperatorIoContract* out,
                                            char* out_error_msg = nullptr,
                                            size_t error_buf_size = 0) noexcept;

}  // namespace llm_edgeflow::operator_api
