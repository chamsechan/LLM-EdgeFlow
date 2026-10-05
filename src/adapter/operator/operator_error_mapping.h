#pragma once

#include <string>
#include <string_view>

#include "core/pipeline.h"

namespace llm_edgeflow {

// 源于 Operator facade 之下的失败。其内部 Pipeline、Node 和 Model 错误码
// 只通过 GetOperatorLastError() 报告；宿主收到的是失败阶段的公开错误码。
enum class OperatorFailureStage {
  kCreatePreparation,   // 配置、部署、模型和 Backend 加载
  kProcessExecution,    // Pipeline::Execute 中 Node 或 Model 失败
  kControlRequest,      // Control 信封、目标或 payload 被拒绝
  kControlUnsupported,  // 没有 Node 声明或处理该命令
  kControlExecution,    // Node 拒绝或未能应用更新
  kControlRuntime,      // 无就绪运行时，或触发了异常屏障
};

OperatorFailureStage ControlFailureToOperatorStage(
    ControlFailureStage stage) noexcept;

int PublicFailureCode(OperatorFailureStage stage, int internal_code) noexcept;

std::string DescribeOperatorFailure(OperatorFailureStage stage,
                                    int internal_code, std::string_view detail);

}  // namespace llm_edgeflow
