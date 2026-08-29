#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "engine/model_interface.h"

namespace alg_framework {

/**
 * @brief Layer 4 模型物化规格参数 (由 Pipeline 将 ValidatedModelPlan 映射而来)
 */
struct ModelLoadSpec {
  std::string model_type;
  std::string backend_type;
  std::string model_path;
  nlohmann::json model_config = nlohmann::json::object();
  nlohmann::json backend_config = nlohmann::json::object();
};

/**
 * @brief 模型运行时工厂 (ModelRuntimeFactory)
 *
 * 按照解耦流程执行：
 * 1. 查找并创建后端实例 (BackendRegistry)
 * 2. 加载后端会话 (backend->Load)
 * 3. 校验协议与并发契约
 * 4. 推导 model_resource_root
 * 5. 创建模型语义实例 (ModelRegistry)
 * 6. 校验模型标识与并发
 */
class ModelRuntimeFactory {
 public:
  static std::shared_ptr<IModel> Create(
      const ModelLoadSpec& spec, std::string* diagnostic = nullptr) noexcept;
};

}  // namespace alg_framework
