#pragma once

#include <string>

#include "engine/backend_interface.h"
#include "engine/inference_definition.h"

namespace llm_edgeflow {

// Declares a Backend provider's type once through Derived::kBackendType. The
// runtime factory still compares the provider and every loaded session with
// the requested Definition.
template <typename Derived>
class BackendIdentity : public IInferenceBackend {
 public:
  const std::string& BackendType() const noexcept override {
    static const std::string type = Derived::kBackendType;
    return type;
  }
};

// Definition carrying BackendClass's type; callers add protocols,
// concurrency, configuration fields and validator.
template <typename BackendClass>
BackendDefinition MakeBackendDefinition() {
  BackendDefinition definition;
  definition.backend_type = BackendClass::kBackendType;
  return definition;
}

}  // namespace llm_edgeflow
