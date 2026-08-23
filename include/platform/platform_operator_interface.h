#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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
  float threshold = 0.0f;                       // 判定阈值 (范围 0.0f ~ 1.0f)
};

/**
 * @brief 平台创建期配置参数
 */
struct PlatformConfig {
  int32_t batch_size = 1;  // 单次 Process 允许提交的最大样本数 (必须 > 0)
  int32_t device_id = 0;   // 目标硬件设备 ID (必须 >= 0)
  ChipType type = ChipType::kUnknown;  // 硬件芯片类型 (必须在显式支持白名单内)
};

/**
 * @brief 可注入的输出对象分配/释放回调钩子 (用于 depth_num 预分配管理)
 *
 * 回调由 Operator 异常屏障保护。释放回调抛异常时，Destroy/Deinit 会继续
 * 清理其他输出对象和句柄，并向调用方返回 -99（标准异常）或 -100（未知异常）。
 */
using OutputAllocator = std::shared_ptr<void> (*)(const char* slot_suffix,
                                                  void* user_data);
using OutputDeallocator = void (*)(const char* slot_suffix,
                                   std::shared_ptr<void> ptr, void* user_data);

/**
 * @brief 算法句柄创建参数
 */
struct CreateParam {
  const char* cfg_file_name = nullptr;  // 公司平台 .conf 部署配置文件路径
  PlatformConfig platform_config;       // 平台运行参数
  uint32_t depth_num = 1;  // Create 阶段输出结构体创建组数预声明 (必须 > 0)
  OutputAllocator output_allocator = nullptr;      // 可选: 输出对象自定义分配器
  OutputDeallocator output_deallocator = nullptr;  // 可选: 输出对象自定义释放器
  void* user_data = nullptr;                       // 回调上下文指针
};

/**
 * @brief 命名 I/O 槽位容器类型定义 (零拷贝借用外部指针)
 */
using OpaqueData = std::shared_ptr<void>;
using NamedIo = std::unordered_map<std::string, OpaqueData>;
using NamedIoBatch = std::vector<NamedIo>;

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
 * @param cfg_file_name 平台 .conf 部署配置文件路径
 * @param expected_biz_type 预期算法业务类型 (CompanyAlgBizType)
 * @param out_error_msg 错误输出信息缓冲区 (可选)
 * @param error_buf_size 缓冲区容量
 * @return 0 校验通过且兼容, -1 参数非法, -2 配置解析或文件不存在, -3 业务不匹配
 */
int ValidatePlatformConfigBinding(const char* cfg_file_name,
                                  int32_t expected_biz_type,
                                  char* out_error_msg = nullptr,
                                  size_t error_buf_size = 0) noexcept;

}  // namespace llm_edgeflow::platform
