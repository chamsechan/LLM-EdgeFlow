#pragma once

#include <memory>
#include <string>
#include <utility>

#include "core/session_context.h"

namespace llm_edgeflow {

// 通过与 Pipeline 注册部署模型相同的原子批处理路径注册一个测试模型。
inline bool RegisterTestModel(ModelManager& manager, std::string model_name,
                              std::shared_ptr<IModel> model,
                              std::string revision = {},
                              std::string impl_name = {},
                              std::string model_type = {},
                              std::string backend_type = {}) {
  if (model_name.empty() || !model) return false;
  ModelRegistration registration;
  registration.model_name = std::move(model_name);
  registration.impl_name = std::move(impl_name);
  registration.model_type = std::move(model_type);
  registration.backend_type = std::move(backend_type);
  registration.revision = std::move(revision);
  registration.model = std::move(model);
  return manager.RegisterBatch({std::move(registration)});
}

}  // namespace llm_edgeflow
