#pragma once

#include <exception>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/alg_context.h"
#include "core/blackboard_key.h"
#include "core/node_base.h"
#include "core/pipeline_validator.h"
#include "core/session_context.h"

namespace alg_framework {

enum class NodeRuntimeCode : int {
  kInvalidContext = -2901,
  kUnhandledException = -2902,
};

template <typename T>
class BoundInput {
 public:
  explicit BoundInput(std::string logical_name, std::string default_key = {},
                      std::string type_id = {})
      : logical_name_(std::move(logical_name)),
        actual_key_(default_key.empty() ? logical_name_
                                        : std::move(default_key)),
        type_id_(std::move(type_id)) {}

  void Resolve(std::string actual_key) {
    if (!actual_key.empty()) {
      actual_key_ = std::move(actual_key);
    }
  }

  const std::string& LogicalName() const { return logical_name_; }
  const std::string& ActualKey() const { return actual_key_; }

  const T* Get(const AlgContext& ctx) const { return ctx.Get<T>(actual_key_); }

  bool Has(const AlgContext& ctx) const { return ctx.Has(actual_key_); }

  const T* Require(AlgContext& ctx, int error_code,
                   std::string_view semantic = {}) const {
    const bool key_exists = ctx.Has(actual_key_);
    const T* val = ctx.Get<T>(actual_key_);
    if (!val) {
      std::string msg = (key_exists ? "Type mismatch for input port '"
                                    : "Missing required input port '") +
                        logical_name_ + "' (bound key: '" + actual_key_ + "')";
      if (!type_id_.empty()) {
        msg += " expected type: " + type_id_;
      }
      if (!semantic.empty()) {
        msg += " for " + std::string(semantic);
      }
      ctx.SetError(error_code, std::move(msg));
      return nullptr;
    }
    return val;
  }

 private:
  std::string logical_name_;
  std::string actual_key_;
  std::string type_id_;
};

template <typename T>
class BoundOutput {
 public:
  explicit BoundOutput(std::string logical_name, std::string default_key = {},
                       std::string type_id = {})
      : logical_name_(std::move(logical_name)),
        actual_key_(default_key.empty() ? logical_name_
                                        : std::move(default_key)),
        type_id_(std::move(type_id)) {}

  void Resolve(std::string actual_key) {
    if (!actual_key.empty()) {
      actual_key_ = std::move(actual_key);
    }
  }

  const std::string& LogicalName() const { return logical_name_; }
  const std::string& ActualKey() const { return actual_key_; }

  void Set(AlgContext& ctx, T value) const {
    ctx.Set(actual_key_, std::move(value));
  }

 private:
  std::string logical_name_;
  std::string actual_key_;
  std::string type_id_;
};

class NodeBase : public INode {
 public:
  explicit NodeBase(std::string node_name) : node_name_(std::move(node_name)) {}
  ~NodeBase() override = default;

  bool Init(const NodeInitContext& init_ctx) noexcept override {
    if (!init_ctx.session_ctx) {
      return false;
    }
    try {
      plan_ = init_ctx.plan;
      const nlohmann::json& cfg =
          init_ctx.config ? *init_ctx.config
                          : (plan_ ? plan_->normalized_config : empty_config_);
      return InitNode(cfg, *init_ctx.session_ctx);
    } catch (...) {
      return false;
    }
  }

  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) noexcept override {
    NodeInitContext ctx;
    ctx.config = &config;
    ctx.session_ctx = session_ctx;
    return Init(ctx);
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

  NodeControlResult Control(int cmd, const std::string& json_param) override {
    try {
      return ControlNode(cmd, json_param);
    } catch (const std::exception& e) {
      return NodeControlResult::Failed(-1, e.what());
    } catch (...) {
      return NodeControlResult::Failed(-1, "Unknown control exception");
    }
  }

  const std::string& Name() const final { return node_name_; }

 protected:
  virtual bool InitNode(const nlohmann::json& /*config*/,
                        SessionContext& /*session_ctx*/) {
    return true;
  }

  virtual int ProcessNode(AlgContext& req_ctx) = 0;

  virtual NodeControlResult ControlNode(int cmd,
                                        const std::string& json_param) {
    (void)cmd;
    (void)json_param;
    return NodeControlResult::Unsupported();
  }

  template <typename T>
  void BindPort(BoundInput<T>& in_port) {
    if (plan_) {
      std::string actual =
          plan_->FindPortKey(in_port.LogicalName(), PortDirection::kInput);
      if (!actual.empty()) {
        in_port.Resolve(std::move(actual));
      }
    }
  }

  template <typename T>
  void BindPort(BoundOutput<T>& out_port) {
    if (plan_) {
      std::string actual =
          plan_->FindPortKey(out_port.LogicalName(), PortDirection::kOutput);
      if (!actual.empty()) {
        out_port.Resolve(std::move(actual));
      }
    }
  }

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

  const ValidatedNodePlan* Plan() const { return plan_; }

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
  const ValidatedNodePlan* plan_ = nullptr;
  static inline const nlohmann::json empty_config_ = nlohmann::json::object();
};

}  // namespace alg_framework
