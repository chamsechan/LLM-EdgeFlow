#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "platform/company_platform_types.h"

namespace llm_edgeflow::platform {

/**
 * @brief 目标计算芯片类型枚举 (显式支持白名单)
 */
enum class ChipType : int32_t {
  kUnknown = 0,
  kAx650 = 1,       // AX650 NPU
  kAscend310P = 2,  // 华为昇腾 310P NPU
  kAscend910B = 3,  // 华为昇腾 910B NPU
  kRk3588 = 4,      // 瑞芯微 RK3588 NPU
  kNvidiaGpu = 5,   // NVIDIA GPU (CUDA/TensorRT)
  kCpuGeneric = 6,  // 通用 CPU
};

inline bool IsSupportedChipType(ChipType type) noexcept {
  return type == ChipType::kAx650 || type == ChipType::kAscend310P ||
         type == ChipType::kAscend910B || type == ChipType::kRk3588 ||
         type == ChipType::kNvidiaGpu || type == ChipType::kCpuGeneric;
}

inline const char* ChipTypeToString(ChipType type) noexcept {
  switch (type) {
    case ChipType::kAx650:
      return "AX650";
    case ChipType::kAscend310P:
      return "ASCEND_310P";
    case ChipType::kAscend910B:
      return "ASCEND_910B";
    case ChipType::kRk3588:
      return "RK3588";
    case ChipType::kNvidiaGpu:
      return "NVIDIA_GPU";
    case ChipType::kCpuGeneric:
      return "CPU_GENERIC";
    default:
      return "UNKNOWN";
  }
}

/**
 * @brief 运行时动态控制命令枚举
 */
enum class ControlCommand : int32_t {
  kUpdateRules = 1,      // 更新词表 / 规则库
  kSwitchPrompt = 2,     // 切换提示词模板
  kUpdateThreshold = 3,  // 调整判定阈值
  // 后续命令只能追加，不能复用已有数值
};

// -------------------------------------------------------------
// 强类型 Control 参数结构体契约 (每个命令声明唯一结构体，杜绝内存探测)
// -------------------------------------------------------------

/**
 * @brief kUpdateRules 对应强类型参数结构体
 */
struct ControlUpdateRulesParam {
  const char* rules_json_str =
      nullptr;  // JSON 格式的规则/分类词表字符串 (带最大长度约束)
};

/**
 * @brief kSwitchPrompt 对应强类型参数结构体
 */
struct ControlSwitchPromptParam {
  const char* prompt_id = nullptr;  // 提示词标识符 (可选)
  const char* prompt_template_str =
      nullptr;  // 提示词模板字符串 (包含 {context}/{query} 等占位符)
};

/**
 * @brief kUpdateThreshold 对应强类型参数结构体
 */
struct ControlUpdateThresholdParam {
  const char* category_or_rule_name = nullptr;  // 目标规则名或分类名 (可选)
  float threshold = 0.0f;  // 判定阈值 (范围 0.0f ~ 1.0f)
};

/**
 * @brief 算法句柄创建参数 (ABI v3 契约)
 */
struct CreateParam {
  const char* cfg_file_name = nullptr;  // 必填、非空、相对配置文件路径
  const char* model_path =
      nullptr;  // 必填、非空，模型和配置共同所在的目录根路径
  int32_t device_id = 0;  // 目标加速设备 ID (必须 >= 0)
  ChipType platform_type = ChipType::kUnknown;  // 硬件芯片类型
  uint32_t max_frame_depth = 25;  // 每种输出类型的池深度 (0 按默认 25 归一化)
};

/**
 * @brief 命名 I/O 槽位容器类型定义
 */
using OpaqueData = std::shared_ptr<void>;
using NamedIo = std::unordered_map<std::string, OpaqueData>;
using NamedIoBatch = std::vector<NamedIo>;

/**
 * @brief 辅助构造只读借用输入 shared_ptr (空 Deleter，不持有所有权)
 */
template <typename T>
inline std::shared_ptr<void> MakeBorrowedPlatformInput(const T* ptr) {
  return std::shared_ptr<void>(const_cast<void*>(static_cast<const void*>(ptr)),
                               [](void*) {});
}

/**
 * @brief 平台 Operator 统一函数表契约 (ABI 隔离屏障，全函数 noexcept)
 */
struct OperatorFunc {
  int (*Init)() noexcept;
  int (*Create)(void** handle, const CreateParam* param) noexcept;
  int (*Process)(void* handle, const NamedIoBatch& inputs,
                 NamedIoBatch& outputs) noexcept;
  int (*Control)(void* handle, ControlCommand command,
                 void* control_param) noexcept;
  int (*Destroy)(void* handle) noexcept;
  int (*Deinit)() noexcept;
};

/**
 * @brief 获取平台 Operator 函数表入口
 */
OperatorFunc Get_LLM_EDGEFLOW_OperatorTable() noexcept;

/**
 * @brief 获取当前线程最近一次平台门面结构化诊断错误信息
 */
const char* GetPlatformLastError() noexcept;

/**
 * @brief 校验平台部署配置 .conf 与预期业务类型是否兼容
 * (只读无副作用预检，不抛出任何异常)
 * @param model_path 模型与配置根目录
 * @param cfg_file_name 相对配置文件路径
 * @param expected_biz_type 预期算法业务类型 (CompanyAlgBizType)
 * @param out_error_msg 错误输出信息缓冲区 (可选)
 * @param error_buf_size 缓冲区容量
 * @return 0 校验通过且兼容, -1 参数非法, -2 配置解析或文件不存在/逃逸, -3
 * 业务不匹配
 */
int ValidatePlatformConfigBinding(const char* model_path,
                                  const char* cfg_file_name,
                                  int32_t expected_biz_type,
                                  char* out_error_msg = nullptr,
                                  size_t error_buf_size = 0) noexcept;

}  // namespace llm_edgeflow::platform
