#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "nodes/node_base.h"

namespace llm_edgeflow {

// 已退役的类式端口辅助函数。直接驱动 NodeBase 运行时的测试和编写基准的
// 历史基线会用到它们；生产 Node 使用 Spec，端口绑定由 AuthorNode 负责。
class LegacyNodeBase : public NodeBase {
 public:
  using NodeBase::NodeBase;

 protected:
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

  // 保留 BindPort 的校验及从左到右的错误顺序。
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
      std::string msg = Name() +
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
};

}  // namespace llm_edgeflow
