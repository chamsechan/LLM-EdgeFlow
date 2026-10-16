#pragma once

#include "adapter/operator/operator_failure_category.h"

namespace llm_edgeflow {

// 本壳的平台返回码表：每个框架失败类别对应一个平台码。
int PlatformFailureCode(OperatorFailureCategory category) noexcept;

}  // namespace llm_edgeflow
