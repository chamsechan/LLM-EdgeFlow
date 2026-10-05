#pragma once

#include <string>

#include "engine/backend_interface.h"
#include "engine/inference_definition.h"

namespace llm_edgeflow {

// 通过 Derived::kBackendType 一次性声明 Backend 提供者的类型。运行时工厂
// 仍会将提供者及每个已加载会话与所请求的 Definition 比对。
template <typename Derived>
class BackendIdentity : public IInferenceBackend {
 public:
  const std::string& BackendType() const noexcept override {
    static const std::string type = Derived::kBackendType;
    return type;
  }
};

// 携带 BackendClass 类型的 Definition；调用方补充协议、并发度、配置字段
// 和校验器。
template <typename BackendClass>
BackendDefinition MakeBackendDefinition() {
  BackendDefinition definition;
  definition.backend_type = BackendClass::kBackendType;
  return definition;
}

}  // namespace llm_edgeflow
