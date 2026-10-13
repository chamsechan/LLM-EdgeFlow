#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

#include "contracts/error_codes.h"
#include "contracts/parameters.h"
#include "core/node_interface.h"
#include "core/validated_node_plan.h"
#include "nodes/node_result.h"

namespace llm_edgeflow {

inline BindingFacts MakeBindingFacts(const NodeInitContext& ctx) {
  BindingFacts facts;
  facts.has_bindings = true;
  for (const auto& port : ctx.plan->ports) {
    if (port.direction == PortDirection::kInput &&
        !port.blackboard_key.empty()) {
      facts.connected_inputs.insert(port.logical_name);
      facts.input_lifetimes[port.logical_name] = port.lifetime;
    }
  }
  return facts;
}

/**
 * @brief Node 实例的线程安全配置快照管理器。
 *
 * 保证：
 * - 读者通过 atomic load acquire 原子获取快照 (不持有写锁；shared_ptr
 *   的原子操作可能使用内部锁)。
 * - 写者串行、事务安全地构建候选配置并原子发布。
 * - 读者可在任意批次处理期间安全持有旧快照。
 * - 候选构建或校验失败绝不覆盖当前生效的配置。
 */
template <typename State>
class ConfigurationSnapshot {
 public:
  ConfigurationSnapshot() = default;

  explicit ConfigurationSnapshot(State initial_state) {
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
          error::kInvalidState, "ConfigurationSnapshot is not initialized");
    }
    try {
      NodeResult<State> res = build_next(*current);
      if (!res.ok()) {
        const auto& failure = res.failure();
        int code = failure.cause_code != 0 ? failure.cause_code
                                           : error::kInvalidArgument;
        return NodeControlResult::Failed(
            code, failure.message.empty() ? "Configuration update failed"
                                          : failure.message);
      }
      std::shared_ptr<const State> next_ptr =
          std::make_shared<State>(std::move(res).value());
      auto success_res = NodeControlResult::Handled();
      std::atomic_store_explicit(&state_, std::move(next_ptr),
                                 std::memory_order_release);
      return success_res;
    } catch (const std::exception& e) {
      return NodeControlResult::Failed(error::kInternalException, e.what());
    } catch (...) {
      return NodeControlResult::Failed(
          error::kUnknownException,
          "Unknown exception during configuration update");
    }
  }

 private:
  mutable std::mutex writer_mutex_;
  std::shared_ptr<const State> state_;
};

}  // namespace llm_edgeflow
