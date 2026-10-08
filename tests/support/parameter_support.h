#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "contracts/parameter_set.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"

namespace llm_edgeflow::test_support {

// 直接调用模型 Create 或后端 Load 的测试，用已注册 Definition 声明的
// ParameterSet 解析配置，得到与工厂相同的参数。解析失败时返回空指针，
// 原因写入 diagnostic。
inline std::shared_ptr<const ParameterValues> ParseParams(
    const ParameterSet& params,
    const nlohmann::json& config = nlohmann::json::object(),
    std::string* diagnostic = nullptr) {
  std::shared_ptr<const ParameterValues> values;
  if (!params.Parse(config, &values, diagnostic)) return nullptr;
  return values;
}

inline std::shared_ptr<const ParameterValues> ParseModelParams(
    const std::string& model_type,
    const nlohmann::json& config = nlohmann::json::object(),
    std::string* diagnostic = nullptr) {
  const auto definition = ModelRegistry::Instance().Find(model_type);
  if (!definition) {
    if (diagnostic) *diagnostic = "Unknown model type: " + model_type;
    return nullptr;
  }
  return ParseParams(definition->params, config, diagnostic);
}

inline std::shared_ptr<const ParameterValues> ParseBackendParams(
    const std::string& backend_type,
    const nlohmann::json& config = nlohmann::json::object(),
    std::string* diagnostic = nullptr) {
  const auto definition = BackendRegistry::Instance().Find(backend_type);
  if (!definition) {
    if (diagnostic) *diagnostic = "Unknown backend type: " + backend_type;
    return nullptr;
  }
  return ParseParams(definition->params, config, diagnostic);
}

}  // namespace llm_edgeflow::test_support
