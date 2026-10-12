#pragma once

#include <memory>
#include <string>

#include "engine/model_registry.h"

namespace llm_edgeflow::test_support {

template <typename Model>
std::shared_ptr<Model> CreateModel(std::shared_ptr<IBackendSession> session,
                                   const nlohmann::json& parameters,
                                   std::string* diagnostic) {
  const auto definition = ModelRegistry::Instance().Find(Model::kImplName);
  if (!definition) {
    if (diagnostic) *diagnostic = "Model definition is not registered";
    return nullptr;
  }
  ModelCreateContext context;
  context.backend_session = std::move(session);
  if (!definition->params.Parse(parameters, &context.params, diagnostic)) {
    return nullptr;
  }
  return std::dynamic_pointer_cast<Model>(Model::Create(context, diagnostic));
}

}  // namespace llm_edgeflow::test_support
