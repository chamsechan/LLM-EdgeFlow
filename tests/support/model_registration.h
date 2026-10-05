#pragma once

#include <memory>
#include <string>
#include <utility>

#include "core/session_context.h"

namespace llm_edgeflow {

// 通过与 Pipeline 注册部署模型相同的原子批处理路径注册一个测试模型。
inline bool RegisterTestModel(ModelManager& manager, std::string model_id,
                              std::shared_ptr<IModel> model,
                              std::string revision = {},
                              std::string model_type = {},
                              std::string capability = {},
                              std::string backend_type = {}) {
  if (model_id.empty() || !model) return false;
  ModelRegistration registration;
  registration.model_id = std::move(model_id);
  registration.model_type = std::move(model_type);
  registration.capability = std::move(capability);
  registration.backend_type = std::move(backend_type);
  registration.revision = std::move(revision);
  registration.model = std::move(model);
  return manager.RegisterBatch({std::move(registration)});
}

}  // namespace llm_edgeflow
