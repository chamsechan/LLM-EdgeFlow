#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/session_context.h"
#include "nodes/node_result.h"

namespace llm_edgeflow {

// Borrowed for the node's session lifetime. Models remain explicit Spec
// dependencies; this facade cannot look up models or access request values.
class SessionResources {
 public:
  SessionResources() = default;
  explicit SessionResources(SessionContext& session) : session_(&session) {}

  std::string GetModelRevision(const std::string& model_id) const {
    return Session().GetModelManager().GetModelRevision(model_id);
  }

  // Ordinary factories return a value or NodeFailure. The existing session
  // single-flight shares failures with waiters and permits retry on the next
  // call. Other exceptions still reach the node runtime's exception barrier.
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
