#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/session_context.h"

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

  template <typename T, typename Factory>
  std::shared_ptr<T> GetOrCreateResource(const SessionResourceKey<T>& key,
                                         Factory&& factory) const {
    return Session().GetOrCreateResource<T>(key,
                                            std::forward<Factory>(factory));
  }

 private:
  SessionContext& Session() const {
    if (!session_)
      throw std::logic_error("Session resources are not initialized");
    return *session_;
  }
  SessionContext* session_ = nullptr;
};

}  // namespace llm_edgeflow
