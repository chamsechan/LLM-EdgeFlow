#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "core/node_interface.h"
#include "core/validated_node_plan.h"
#include "nodes/node_error_codes.h"
#include "nodes/node_result.h"

namespace llm_edgeflow {

/**
 * @brief Connection facts of input ports captured defensively during Init.
 *
 * Immutable after initialization; retains distinction between "no plan"
 * and "plan present with explicit bindings".
 */
struct BindingFacts {
  bool has_plan = false;
  bool has_bindings = false;
  std::unordered_set<std::string> connected_inputs;

  bool IsConnected(const std::string& port_name) const noexcept {
    return connected_inputs.count(port_name) > 0;
  }
};

inline BindingFacts MakeBindingFacts(
    const NodeInitContext& ctx,
    std::unordered_set<std::string> connected_inputs) {
  BindingFacts facts;
  facts.has_plan = (ctx.plan != nullptr);
  facts.has_bindings = true;
  facts.connected_inputs = std::move(connected_inputs);
  return facts;
}

inline BindingFacts MakeBindingFacts(const NodeInitContext& ctx) {
  BindingFacts facts;
  facts.has_bindings = true;
  if (ctx.plan) {
    facts.has_plan = true;
    for (const auto& port : ctx.plan->ports) {
      if (port.direction == PortDirection::kInput &&
          !port.blackboard_key.empty()) {
        facts.connected_inputs.insert(port.logical_name);
      }
    }
  }
  return facts;
}

inline BindingFacts MakeBindingFacts(const ValidatedNodePlan* plan) {
  BindingFacts facts;
  facts.has_bindings = true;
  if (plan) {
    facts.has_plan = true;
    for (const auto& port : plan->ports) {
      if (port.direction == PortDirection::kInput &&
          !port.blackboard_key.empty()) {
        facts.connected_inputs.insert(port.logical_name);
      }
    }
  }
  return facts;
}

/**
 * @brief Thread-safe configuration snapshot manager for node instances
 * (RFC-0054).
 *
 * Enforces:
 * - Atomic snapshot acquisition for readers via atomic load acquire (without
 * the writer mutex; shared_ptr atomic operations may use internal locks).
 * - Serialized, transaction-safe candidate building and atomic publication for
 * writers.
 * - Readers safely retain old snapshots for arbitrary batch duration.
 * - Failed candidate building / validation never overwrites active
 * configuration.
 */
template <typename State>
class ConfigurationSnapshot {
 public:
  ConfigurationSnapshot() = default;

  explicit ConfigurationSnapshot(State initial_state) {
    Initialize(std::move(initial_state));
  }

  explicit ConfigurationSnapshot(std::shared_ptr<const State> initial_state) {
    Initialize(std::move(initial_state));
  }

  ~ConfigurationSnapshot() = default;

  ConfigurationSnapshot(const ConfigurationSnapshot&) = delete;
  ConfigurationSnapshot& operator=(const ConfigurationSnapshot&) = delete;
  ConfigurationSnapshot(ConfigurationSnapshot&&) = delete;
  ConfigurationSnapshot& operator=(ConfigurationSnapshot&&) = delete;

  bool Initialize(State initial_state) {
    std::lock_guard<std::mutex> lock(writer_mutex_);
    auto ptr = std::make_shared<State>(std::move(initial_state));
    std::atomic_store_explicit(&state_,
                               std::shared_ptr<const State>(std::move(ptr)),
                               std::memory_order_release);
    return true;
  }

  bool Initialize(std::shared_ptr<const State> initial_state) {
    if (!initial_state) return false;
    std::lock_guard<std::mutex> lock(writer_mutex_);
    std::atomic_store_explicit(&state_, std::move(initial_state),
                               std::memory_order_release);
    return true;
  }

  bool IsInitialized() const noexcept {
    return std::atomic_load_explicit(&state_, std::memory_order_acquire) !=
           nullptr;
  }

  std::shared_ptr<const State> Read() const noexcept {
    return std::atomic_load_explicit(&state_, std::memory_order_acquire);
  }

  template <typename BuildNextFn>
  NodeControlResult Update(BuildNextFn&& build_next) {
    std::unique_lock<std::mutex> lock(writer_mutex_);
    auto current =
        std::atomic_load_explicit(&state_, std::memory_order_acquire);
    if (!current) {
      return NodeControlResult::Failed(
          node_error::control::kInvalidRequest,
          "ConfigurationSnapshot is not initialized");
    }
    try {
      auto run_build = [&]() {
        if constexpr (std::is_invocable_v<BuildNextFn, const State&>) {
          return build_next(*current);
        } else {
          return build_next(current);
        }
      };
      auto res = run_build();
      if (!res.ok()) {
        const auto& failure = res.failure();
        int code = failure.cause_code != 0
                       ? failure.cause_code
                       : node_error::control::kInvalidRequest;
        return NodeControlResult::Failed(
            code, failure.message.empty() ? "Configuration update failed"
                                          : failure.message);
      }
      std::shared_ptr<const State> next_ptr;
      using ResValType = std::decay_t<decltype(res.value())>;
      if constexpr (std::is_same_v<ResValType, std::shared_ptr<const State>> ||
                    std::is_same_v<ResValType, std::shared_ptr<State>>) {
        next_ptr = std::move(res).value();
      } else {
        next_ptr = std::make_shared<State>(std::move(res).value());
      }
      if (!next_ptr) {
        return NodeControlResult::Failed(
            node_error::control::kInvalidRequest,
            "Configuration update produced null state");
      }
      auto success_res = NodeControlResult::Handled();
      std::atomic_store_explicit(&state_, std::move(next_ptr),
                                 std::memory_order_release);
      return success_res;
    } catch (const std::exception& e) {
      return NodeControlResult::Failed(node_error::control::kInvalidRequest,
                                       e.what());
    } catch (...) {
      return NodeControlResult::Failed(
          node_error::control::kInvalidRequest,
          "Unknown exception during configuration update");
    }
  }

 private:
  mutable std::mutex writer_mutex_;
  std::shared_ptr<const State> state_;
};

}  // namespace llm_edgeflow
