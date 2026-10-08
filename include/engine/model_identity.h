#pragma once

#include <string>

#include "engine/inference_definition.h"
#include "engine/model_capability_traits.h"
#include "engine/model_interface.h"

namespace llm_edgeflow {

// 一次性声明 Model 的标识。Derived 提供 kModelType 和 kConcurrency，
// 能力由所实现的接口决定。注册的 Definition 以相同的值为起点，运行时工厂
// 仍会将每个创建的实例与其 Definition 比对。
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

// 携带 ModelClass 标识的 Definition；调用方补充描述、协议和参数声明
// （def.params）。
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
