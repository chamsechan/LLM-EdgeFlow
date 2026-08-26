#pragma once

#include <exception>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "core/alg_context.h"
#include "core/blackboard_key.h"
#include "core/node_base.h"
#include "core/session_context.h"

namespace alg_framework {

enum class NodeRuntimeCode : int {
  kInvalidContext = -2901,
  kUnhandledException = -2902,
};

class NodeBase : public INode {
 public:
  explicit NodeBase(std::string node_name) : node_name_(std::move(node_name)) {}
  ~NodeBase() override = default;

  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) noexcept final {
    if (!session_ctx) {
      return false;
    }
    try {
      return InitNode(config, *session_ctx);
    } catch (...) {
      return false;
    }
  }

  int Process(AlgContext* req_ctx) noexcept final {
    if (!req_ctx) {
      return static_cast<int>(NodeRuntimeCode::kInvalidContext);
    }
    try {
      return ProcessNode(*req_ctx);
    } catch (const std::exception& e) {
      SetUnhandledErrorNoexcept(*req_ctx, e.what());
      return static_cast<int>(NodeRuntimeCode::kUnhandledException);
    } catch (...) {
      SetUnhandledErrorNoexcept(*req_ctx, nullptr);
      return static_cast<int>(NodeRuntimeCode::kUnhandledException);
    }
  }

  const std::string& Name() const final { return node_name_; }

 protected:
  virtual bool InitNode(const nlohmann::json& /*config*/,
                        SessionContext& /*session_ctx*/) {
    return true;
  }

  virtual int ProcessNode(AlgContext& req_ctx) = 0;

  template <typename T>
  const T* Require(AlgContext& ctx, const BlackboardKey<T>& key, int error_code,
                   std::string_view semantic = {}) const {
    const bool key_exists = ctx.Has(key);
    const T* val = ctx.Get(key);
    if (!val) {
      std::string msg = node_name_ +
                        (key_exists ? ": type mismatch for input key '"
                                    : ": missing required input key '") +
                        key.name + "' (expected type: " + key.type_id + ")";
      if (!semantic.empty()) {
        msg += " for " + std::string(semantic);
      }
      ctx.SetError(error_code, std::move(msg));
      return nullptr;
    }
    return val;
  }

  template <typename T>
  void Publish(AlgContext& ctx, const BlackboardKey<T>& key, T value) const {
    ctx.Set(key, std::move(value));
  }

  int Fail(AlgContext& ctx, int error_code,
           std::string_view message) const noexcept {
    try {
      ctx.SetError(error_code, std::string(message));
    } catch (...) {
    }
    return error_code;
  }

 private:
  void SetUnhandledErrorNoexcept(AlgContext& ctx,
                                 const char* detail) const noexcept {
    try {
      std::string message = node_name_;
      if (detail) {
        message += " unhandled exception: ";
        message += detail;
      } else {
        message += " unknown unhandled exception";
      }
      ctx.SetError(static_cast<int>(NodeRuntimeCode::kUnhandledException),
                   std::move(message));
    } catch (...) {
    }
  }

  const std::string node_name_;
};

}  // namespace alg_framework
