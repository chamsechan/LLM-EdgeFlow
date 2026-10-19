#pragma once

#include <string>
#include <string_view>

#include "core/pipeline.h"

namespace llm_edgeflow {

// 源于 Operator facade 之下的失败。其内部 Pipeline、Node 和 Model 错误码
// 只通过 GetOperatorLastError() 报告；宿主收到的是失败类别对应的平台码。
enum class OperatorFailureStage {
  kCreatePreparation,   // 配置、部署、模型和 Backend 加载
  kProcessExecution,    // Pipeline::Execute 中 Node 或 Model 失败
  kControlRequest,      // Control 信封、目标或 payload 被拒绝
  kControlUnsupported,  // 没有 Node 声明或处理该命令
  kControlExecution,    // Node 拒绝或未能应用更新
  kRuntime,             // 无就绪运行时，或触发了异常屏障
};

// 框架的公共返回类别。各协议壳把每个类别映射为一个平台码；
// 平台可以让多个类别共用一个码值，但框架不合并类别。
enum class OperatorFailureCategory {
  kSuccess,
  kInvalidHandle,
  kInvalidParam,
  kInvalidInput,
  kOutputCapacity,
  kUnsupportedBiz,
  kRegistryConflict,
  kUnsupportedControl,
  kException,
  kUnknown,
};

// 失败阶段的固定名称，不含请求内容，可用于日志。
const char* OperatorFailureStageName(OperatorFailureStage stage) noexcept;

OperatorFailureStage ControlFailureToOperatorStage(
    ControlFailureStage stage) noexcept;

// 先判定成功码，再按失败阶段分类；未声明的内部码按所在阶段兜底。
OperatorFailureCategory ClassifyOperatorFailure(OperatorFailureStage stage,
                                                int internal_code) noexcept;

// 调用方可修正的拒绝记 Warning；资源与构建失败及其他类别记 Error。
bool IsRejectedOperatorRequest(OperatorFailureCategory category,
                               int internal_code) noexcept;

std::string DescribeOperatorFailure(OperatorFailureStage stage,
                                    int internal_code, std::string_view detail);

}  // namespace llm_edgeflow
