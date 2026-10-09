#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "engine/backend_interface.h"
#include "engine/model_interface.h"

namespace llm_edgeflow {

/**
 * @brief 模型执行层模型物化规格参数 (由 Pipeline 将 ValidatedModelPlan
 * 映射而来)
 */
struct ModelLoadSpec {
  std::string impl_name;
  std::string backend_type;
  std::string model_file;
  nlohmann::json model_params = nlohmann::json::object();
  nlohmann::json backend_params = nlohmann::json::object();
  ExecutionTarget execution_target;
};

/**
 * @brief 模型运行时工厂 (ModelRuntimeFactory)
 *
 * 按照解耦流程执行：
 * 1. 查找 Definition，规范化并校验模型配置，再创建后端实例
 * 2. 加载后端会话 (backend->Load)
 * 3. 校验协议与并发契约
 * 4. 创建模型语义实例 (ModelRegistry)
 * 5. 校验模型标识与并发
 */
class ModelRuntimeFactory {
 public:
  static std::shared_ptr<IModel> Create(
      const ModelLoadSpec& spec, std::string* diagnostic = nullptr) noexcept;
};

}  // namespace llm_edgeflow
