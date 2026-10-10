#pragma once

namespace llm_edgeflow {

class OperatorValueTypeRegistry;

// 显式注册本仓库仅供开发使用的 mock 平台 binding。
void RegisterMockOperatorBindings(OperatorValueTypeRegistry& registry);

}  // namespace llm_edgeflow
