#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace llm_edgeflow::platform {

/**
 * @brief 目标计算芯片类型枚举
 */
enum class ChipType : int32_t {
  kUnknown = 0,
  kAx650 = 1,  // AX650 / 移植时替换为公司公共枚举的正式值
};

/**
 * @brief 运行时动态控制命令枚举
 */
enum class ControlCommand : int32_t {
  kUpdateRules = 1,      // 更新词表 / 规则库
  kSwitchPrompt = 2,     // 切换提示词模板
  kUpdateThreshold = 3,  // 调整判定阈值
  // 后续命令只能追加，不能复用已有数值
};

/**
 * @brief 平台创建期配置参数
 */
struct PlatformConfig {
  int32_t batch_size = 1;              // 单次 Process 允许提交的最大样本数
  int32_t device_id = 0;               // 目标硬件设备 ID
  ChipType type = ChipType::kUnknown;  // 硬件芯片类型
};

/**
 * @brief 算法句柄创建参数
 */
struct CreateParam {
  const char* cfg_file_name = nullptr;  // 公司平台 .conf 部署配置文件路径
  PlatformConfig platform_config;       // 平台运行参数
  uint32_t depth_num = 1;               // Create 阶段输出结构体创建组数预声明
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

}  // namespace llm_edgeflow::platform
