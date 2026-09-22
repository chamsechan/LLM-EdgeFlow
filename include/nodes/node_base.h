#pragma once

#include <exception>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/alg_context.h"
#include "core/blackboard_key.h"
#include "core/common_contracts.h"
#include "core/node_interface.h"
#include "core/session_context.h"
#include "core/validated_node_plan.h"
#include "edgeflow/log.h"

namespace llm_edgeflow {

enum class NodeRuntimeCode : int {
  kInvalidContext = -2901,
  kUnhandledException = -2902,
};

template <typename T>
class BoundInput {
 public:
  explicit BoundInput(std::string logical_name, std::string default_key = {})
      : logical_name_(std::move(logical_name)),
        actual_key_(default_key.empty() ? logical_name_
                                        : std::move(default_key)),
        type_id_(BlackboardTypeTraits<T>::TypeName()) {}

  explicit BoundInput(const BlackboardKey<T>& key)
      : logical_name_(key.name),
        actual_key_(key.name),
        type_id_(key.type_id ? key.type_id
                             : BlackboardTypeTraits<T>::TypeName()) {
    if (key.type_id &&
        std::string_view(key.type_id) != BlackboardTypeTraits<T>::TypeName()) {
      throw std::invalid_argument(
          "BlackboardKey type_id does not match BlackboardTypeTraits<T>");
    }
  }

  void Resolve(std::string actual_key) {
    if (!actual_key.empty()) {
      actual_key_ = std::move(actual_key);
      is_bound_ = true;
    }
  }

  void Unbind() {
    actual_key_.clear();
    is_bound_ = false;
  }

  const std::string& LogicalName() const { return logical_name_; }
  const std::string& ActualKey() const { return actual_key_; }
  const std::string& TypeId() const { return type_id_; }
  bool IsBound() const { return is_bound_; }

  const T* Get(const AlgContext& ctx) const {
    return actual_key_.empty() ? nullptr : ctx.Read<T>(actual_key_);
  }

  bool Has(const AlgContext& ctx) const {
    return !actual_key_.empty() && ctx.Has(actual_key_);
  }

  const T* Require(AlgContext& ctx, int error_code,
                   std::string_view semantic = {}) const {
    const bool key_exists = ctx.Has(actual_key_);
    const T* val = ctx.Read<T>(actual_key_);
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
  bool is_bound_ = false;
};

template <typename T>
class BoundOutput {
 public:
  explicit BoundOutput(std::string logical_name, std::string default_key = {})
      : logical_name_(std::move(logical_name)),
        actual_key_(default_key.empty() ? logical_name_
                                        : std::move(default_key)),
        type_id_(BlackboardTypeTraits<T>::TypeName()) {}

  explicit BoundOutput(const BlackboardKey<T>& key)
      : logical_name_(key.name),
        actual_key_(key.name),
        type_id_(key.type_id ? key.type_id
                             : BlackboardTypeTraits<T>::TypeName()) {
    if (key.type_id &&
        std::string_view(key.type_id) != BlackboardTypeTraits<T>::TypeName()) {
      throw std::invalid_argument(
          "BlackboardKey type_id does not match BlackboardTypeTraits<T>");
    }
  }

  void Resolve(std::string actual_key) {
    if (!actual_key.empty()) {
      actual_key_ = std::move(actual_key);
      is_bound_ = true;
    }
  }

  const std::string& LogicalName() const { return logical_name_; }
  const std::string& ActualKey() const { return actual_key_; }
  const std::string& TypeId() const { return type_id_; }
  bool IsBound() const { return is_bound_; }

  void Set(AlgContext& ctx, T value) const {
    if (!ctx.Publish(actual_key_, std::move(value))) {
      throw std::logic_error("Duplicate output publication for key '" +
                             actual_key_ + "'");
    }
  }

 private:
  std::string logical_name_;
  std::string actual_key_;
  std::string type_id_;
  bool is_bound_ = false;
};

namespace detail {

enum class PortBindingStatus { kUnbound, kTypeMismatch, kBound };

struct PortBindingResult {
  PortBindingStatus status;
  const ResolvedPortBinding* binding;
};

template <class PortT>
PortBindingResult ResolvePortBinding(const ValidatedNodePlan& plan,
                                     PortDirection direction, PortT& port) {
  const auto* binding = plan.FindPort(port.LogicalName(), direction);
  if (!binding || binding->blackboard_key.empty()) {
    return {PortBindingStatus::kUnbound, binding};
  }
  if (binding->type_id != port.TypeId()) {
    return {PortBindingStatus::kTypeMismatch, binding};
  }
  port.Resolve(binding->blackboard_key);
  return {PortBindingStatus::kBound, binding};
}

}  // namespace detail

class NodeBase : public INode {
 public:
  explicit NodeBase(std::string node_name) : node_name_(std::move(node_name)) {}
  ~NodeBase() override = default;

  bool Init(const NodeInitContext& init_ctx) noexcept final {
    if (init_ctx.diagnostic) init_ctx.diagnostic->clear();
    if (!init_ctx.session_ctx) {
      return init_ctx.Fail("Node initialization requires SessionContext");
    }
    if (!init_ctx.plan) {
      return init_ctx.Fail("Node initialization requires ValidatedNodePlan");
    }
    try {
      return InitNode(init_ctx, init_ctx.plan->normalized_config,
                      *init_ctx.session_ctx);
    } catch (const std::exception& e) {
      ALG_LOG_ERROR("[NodeBase] Exception in InitNode for %s: %s\n",
                    node_name_.c_str(), e.what());
      return init_ctx.Fail(e.what());
    } catch (...) {
      ALG_LOG_ERROR("[NodeBase] Unknown exception in InitNode for %s\n",
                    node_name_.c_str());
      return init_ctx.Fail("Unknown exception in InitNode");
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
  virtual bool InitNode(const NodeInitContext& init_ctx,
                        const nlohmann::json& config,
                        SessionContext& session_ctx) {
    (void)init_ctx;
    (void)config;
    (void)session_ctx;
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
  void BindPort(const NodeInitContext& init_ctx, BoundInput<T>& in_port) const {
    const auto result = detail::ResolvePortBinding(
        *init_ctx.plan, PortDirection::kInput, in_port);
    if (result.status != detail::PortBindingStatus::kUnbound) {
      if (result.status == detail::PortBindingStatus::kTypeMismatch) {
        throw std::invalid_argument(
            "Input port TypeId mismatch for " + in_port.LogicalName() +
            " (expected: " + in_port.TypeId() +
            ", bound: " + result.binding->type_id + ")");
      }
    } else {
      in_port.Unbind();
    }
  }

  template <typename T>
  void BindPort(const NodeInitContext& init_ctx,
                BoundOutput<T>& out_port) const {
    const auto result = detail::ResolvePortBinding(
        *init_ctx.plan, PortDirection::kOutput, out_port);
    if (result.status == detail::PortBindingStatus::kUnbound) {
      throw std::invalid_argument("Output port is unbound in plan: " +
                                  out_port.LogicalName());
    }
    if (result.status == detail::PortBindingStatus::kTypeMismatch) {
      throw std::invalid_argument("Output port TypeId mismatch for " +
                                  out_port.LogicalName() +
                                  " (expected: " + out_port.TypeId() +
                                  ", bound: " + result.binding->type_id + ")");
    }
  }

  template <typename T>
  BoundInput<T> BindInput(const NodeInitContext& init_ctx,
                          std::string logical_name) const {
    BoundInput<T> port(std::move(logical_name));
    BindPort(init_ctx, port);
    return port;
  }

  // Preserve BindPort's validation and left-to-right error ordering.
  template <typename... Ports>
  void BindPorts(const NodeInitContext& init_ctx, Ports&... ports) const {
    (BindPort(init_ctx, ports), ...);
  }

  template <typename T>
  BoundOutput<T> BindOutput(const NodeInitContext& init_ctx,
                            std::string logical_name) const {
    BoundOutput<T> port(std::move(logical_name));
    BindPort(init_ctx, port);
    return port;
  }

  template <typename T>
  const T* Require(AlgContext& ctx, const BlackboardKey<T>& key, int error_code,
                   std::string_view semantic = {}) const {
    const bool key_exists = ctx.Has(key);
    const T* val = ctx.Read(key);
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
    if (!ctx.Publish(key, std::move(value))) {
      throw std::logic_error("Duplicate output publication for key '" +
                             std::string(key.name) + "'");
    }
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

}  // namespace llm_edgeflow
