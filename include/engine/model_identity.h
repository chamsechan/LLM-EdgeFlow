#pragma once

#include <string>

#include "engine/inference_definition.h"
#include "engine/model_capability_traits.h"
#include "engine/model_interface.h"

namespace llm_edgeflow {

// Declares a Model's identity once. Derived supplies kModelType and
// kConcurrency; the capability follows from the implemented interface. The
// registered Definition starts from the same values, and the runtime factory
// still checks every created instance against its Definition.
template <typename Derived, typename CapabilityInterface>
class ModelIdentity : public CapabilityInterface {
 public:
  using Capabilities = CapabilityInterface;

  const std::string& ModelType() const noexcept override {
    static const std::string type = Derived::kModelType;
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string capability =
        ModelCapabilityTraits<CapabilityInterface>::Capability();
    return capability;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return Derived::kConcurrency;
  }
};

// Definition carrying ModelClass's identity; callers add the description,
// protocol, configuration fields and validator.
template <typename ModelClass>
ModelDefinition MakeModelDefinition() {
  ModelDefinition definition;
  definition.model_type = ModelClass::kModelType;
  definition.capability =
      ModelCapabilityTraits<typename ModelClass::Capabilities>::Capability();
  definition.concurrency = ModelClass::kConcurrency;
  return definition;
}

}  // namespace llm_edgeflow
