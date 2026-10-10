#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/session_context.h"
#include "nodes/node_result.h"

namespace llm_edgeflow {

// 在 Node 的会话生命周期内借用。Model 仍是显式的 Spec 依赖；
// 此 facade 不能查找模型或访问请求值。
class SessionResources {
 public:
  SessionResources() = default;
  explicit SessionResources(SessionContext& session) : session_(&session) {}

  std::string GetModelRevision(const std::string& model_name) const {
    return Session().GetModelManager().GetModelRevision(model_name);
  }

  // 普通工厂返回值或 NodeFailure。现有的会话 single-flight 机制会把失败共享给
  // 等待者，并允许下次调用重试。其他异常仍会到达 Node 运行时的异常屏障。
  template <typename T, typename Factory>
  NodeResult<std::shared_ptr<T>> GetOrCreateResult(
      const SessionResourceKey<T>& key, Factory&& factory) const {
    try {
      return NodeResult<std::shared_ptr<T>>::Success(
          Session().GetOrCreateResource<T>(key, [&]() {
            auto result = factory();
            if (!result.ok())
              throw ResourceFailure(std::move(result).ExtractFailure());
            return std::make_shared<T>(std::move(result).value());
          }));
    } catch (const ResourceFailure& error) {
      return NodeResult<std::shared_ptr<T>>::Failure(error.failure);
    }
  }

 private:
  struct ResourceFailure : std::exception {
    explicit ResourceFailure(NodeFailure value) : failure(std::move(value)) {}
    const char* what() const noexcept override {
      return failure.message.c_str();
    }
    NodeFailure failure;
  };

  SessionContext& Session() const {
    if (!session_)
      throw std::logic_error("Session resources are not initialized");
    return *session_;
  }
  SessionContext* session_ = nullptr;
};

}  // namespace llm_edgeflow
