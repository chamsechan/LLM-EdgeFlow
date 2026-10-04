#pragma once

#include <memory>
#include <string>
#include <utility>

#include "core/session_context.h"

namespace llm_edgeflow {

// Registers one test model through the same atomic batch path that Pipeline
// uses for a deployment's models.
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
