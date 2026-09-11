#ifndef EDGEFLOW_PLATFORM_MOCK_OPERATOR_TYPES_H_
#define EDGEFLOW_PLATFORM_MOCK_OPERATOR_TYPES_H_

// Local platform mock declarations for this repository's Demo and tests.
// These are existing external-environment substitutes, not company SDK headers.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace llm_edgeflow::operator_api {

/**
 * @brief 目标计算平台枚举 (显式支持白名单)
 */
enum class ComputePlatform : int32_t {
  kUnknown = 0,
  kAx650 = 1,       // AX650 NPU
  kAscend310P = 2,  // 华为昇腾 310P NPU
  kAscend910B = 3,  // 华为昇腾 910B NPU
  kRk3588 = 4,      // 瑞芯微 RK3588 NPU
  kCuda = 5,        // NVIDIA GPU (CUDA/TensorRT)
  kCpu = 6,         // 通用 CPU
};

/**
 * @brief 运行时动态控制命令枚举
 */
enum class ControlCommand : int32_t {
  kUpdateRules = 1,      // 更新词表 / 规则库
  kSwitchPrompt = 2,     // 切换提示词模板
  kUpdateThreshold = 3,  // 调整判定阈值
  kJson = 4,  // 通用 JSON 传输；节点命令由 ControlJsonParam::cmd_id 指定
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
 * @brief kJson 对应参数。同步调用返回前指针必须有效；实现拥有 payload 拷贝。
 * json_param_str 必须为非空 JSON object，UTF-8 字节数 < 65536（不含终止符）。
 * cmd_id 是节点声明的正整数；未声明命令返回 unsupported。
 */
struct ControlJsonParam {
  int32_t cmd_id = 0;
  const char* json_param_str = nullptr;
};

/**
 * @brief 算法句柄创建参数 (ABI v4 契约)
 */
struct CreateParam {
  const char* cfg_file_name = nullptr;  // 必填、非空、相对配置文件路径
  const char* model_path =
      nullptr;  // 必填、非空，模型和配置共同所在的目录根路径
  int32_t device_id = 0;  // 目标加速设备 ID (必须 >= 0)
  ComputePlatform compute_platform =
      ComputePlatform::kUnknown;  // 目标硬件计算平台
  uint32_t max_frame_depth =
      25;  // 每个逻辑输出槽位的池深度 (0 按默认 25 归一化)
};

/**
 * @brief 命名 I/O 槽位容器类型定义
 * 输入通常是仅在 Process 返回前有效的借用视图。输出是所属 handle 的池租约，
 * shared_ptr 不会延长 handle 或池内存的生命周期。需要跨调用保存结果时，复制字段
 * 到调用方持有的值，再清空输出容器；不得在 Destroy 后访问输出指针。
 */
using OpaqueData = std::shared_ptr<void>;
using NamedIo = std::unordered_map<std::string, OpaqueData>;
using NamedIoBatch = std::vector<NamedIo>;

/**
 * @brief Operator 统一函数表契约 (ABI 隔离屏障，全函数 noexcept)
 */
struct OperatorFunc {
  int (*Init)() noexcept;
  int (*Create)(void** handle, const CreateParam* param) noexcept;
  // 输出占用池容量，释放最后一份输出引用才归还租约。池满时 Process 等待；
  // 不要在同一线程持有全部旧租约时继续同步 Process，否则无法返回释放租约。
  int (*Process)(void* handle, const NamedIoBatch& inputs,
                 NamedIoBatch& outputs) noexcept;
  int (*Control)(void* handle, ControlCommand command,
                 void* control_param) noexcept;
  // 调用前等待所有 Process/Control 返回并释放输出。有效 handle 一经 Destroy
  // 即被消费，即使因未归还输出返回错误也不可重试 Destroy 或继续使用 handle。
  int (*Destroy)(void* handle) noexcept;
  int (*Deinit)() noexcept;
};

}  // namespace llm_edgeflow::operator_api

#endif  // EDGEFLOW_PLATFORM_MOCK_OPERATOR_TYPES_H_
