#include "adapter/operator/operator_process_binding.h"

#include <new>
#include <unordered_set>
#include <utility>

namespace alg_framework {

int ConvertOperatorInputs(
    const llm_edgeflow::operator_api::NamedIoBatch& inputs,
    const OperatorBizBridgeDescriptor& bridge,
    const ResolvedInputLimits& limits,
    ProcessLocalShadowStorage* shadow_storage,
    std::vector<const void*>* internal_dtos, std::string* error) {
  if (!shadow_storage || !internal_dtos ||
      internal_dtos->size() != inputs.size()) {
    if (error) *error = "Invalid input conversion destination";
    return -3;
  }

  for (size_t i = 0; i < inputs.size(); ++i) {
    const auto& in_map = inputs[i];
    std::unordered_map<std::string, const void*> slots_by_logical;
    std::unordered_set<std::string> recognized_keys;

    for (const auto& required_slot : bridge.input_slots) {
      std::string found_key;
      const void* payload = nullptr;
      for (const auto& [key, value] : in_map) {
        std::string key_namespace;
        std::string suffix;
        if (!OperatorValueTypeRegistry::ParseKey(key, &key_namespace,
                                                 &suffix)) {
          if (error) {
            *error = "Invalid input key format in frame " + std::to_string(i) +
                     ": " + key;
          }
          return -3;
        }
        if (OperatorValueTypeRegistry::Instance().NormalizeSuffix(suffix) !=
            required_slot.type_suffix) {
          continue;
        }
        if (!found_key.empty()) {
          if (error) {
            *error = "Duplicate input slot mapping for suffix '" +
                     required_slot.type_suffix + "' in frame " +
                     std::to_string(i);
          }
          return -3;
        }
        found_key = key;
        if (!value || !value.get()) {
          if (error) *error = "Null input shared_ptr for key: " + key;
          return -3;
        }
        payload = value.get();
        recognized_keys.insert(key);
      }

      if (!payload && required_slot.required) {
        if (error) {
          *error = "Missing required input slot for suffix '" +
                   required_slot.type_suffix + "' in frame " +
                   std::to_string(i);
        }
        return -3;
      }

      if (payload) {
        const auto* binding =
            OperatorValueTypeRegistry::Instance().GetBindingBySuffix(
                required_slot.type_suffix);
        if (binding && binding->validate_external) {
          std::string validation_error;
          const int validation_result =
              binding->validate_external(payload, limits, &validation_error);
          if (validation_result != 0) {
            if (error) {
              *error = "Validation failed for input key " + found_key + ": " +
                       validation_error;
            }
            return validation_result;
          }
        }
        slots_by_logical[required_slot.logical_name] = payload;
      }
    }

    if (recognized_keys.size() != in_map.size()) {
      if (error) {
        *error =
            "Unknown extra input keys present in frame " + std::to_string(i);
      }
      return -3;
    }

    std::string conversion_error;
    const int conversion_result =
        bridge.convert_sample_input(slots_by_logical, *shadow_storage,
                                    &(*internal_dtos)[i], &conversion_error);
    if (conversion_result != 0 || !(*internal_dtos)[i]) {
      if (error) {
        *error = "ConvertSampleInput failed in frame " + std::to_string(i) +
                 ": " + conversion_error;
      }
      return conversion_result != 0 ? conversion_result : -3;
    }
  }
  return 0;
}

int ResolveOperatorOutputs(
    const llm_edgeflow::operator_api::NamedIoBatch& outputs,
    const OperatorBizBridgeDescriptor& bridge,
    std::vector<std::vector<FrameOutputBinding>>* frame_bindings,
    std::string* error) {
  if (!frame_bindings) {
    if (error) *error = "Invalid output binding destination";
    return -4;
  }
  frame_bindings->assign(outputs.size(), {});

  for (size_t i = 0; i < outputs.size(); ++i) {
    const auto& out_map = outputs[i];
    std::unordered_set<std::string> recognized_keys;
    for (const auto& required_slot : bridge.output_slots) {
      std::string found_key;
      for (const auto& [key, value] : out_map) {
        std::string key_namespace;
        std::string suffix;
        if (!OperatorValueTypeRegistry::ParseKey(key, &key_namespace,
                                                 &suffix)) {
          if (error) {
            *error = "Invalid output key format in frame " + std::to_string(i) +
                     ": " + key;
          }
          return -4;
        }
        if (OperatorValueTypeRegistry::Instance().NormalizeSuffix(suffix) !=
            required_slot.type_suffix) {
          continue;
        }
        if (!found_key.empty()) {
          if (error) {
            *error = "Duplicate output slot mapping for suffix '" +
                     required_slot.type_suffix + "' in frame " +
                     std::to_string(i);
          }
          return -4;
        }
        found_key = key;
        if (value && value.get()) {
          if (error) {
            *error = "Output slot key '" + key +
                     "' must be initialized to empty shared_ptr<void>";
          }
          return -4;
        }
        recognized_keys.insert(key);
      }

      if (found_key.empty() && required_slot.required) {
        if (error) {
          *error = "Missing required output slot key for suffix '" +
                   required_slot.type_suffix + "' in frame " +
                   std::to_string(i);
        }
        return -4;
      }
      if (!found_key.empty()) {
        (*frame_bindings)[i].push_back(
            {std::move(found_key), required_slot.type_suffix});
      }
    }

    if (recognized_keys.size() != out_map.size()) {
      if (error) {
        *error =
            "Unknown extra output keys present in frame " + std::to_string(i);
      }
      return -4;
    }
  }
  return 0;
}

int AcquireOperatorOutputBlocks(
    const std::vector<std::vector<FrameOutputBinding>>& frame_bindings,
    const std::unordered_map<std::string, std::shared_ptr<OutputPoolState>>&
        output_pools,
    ScopedOutputLeaseGuard* lease_guard,
    std::vector<AcquiredOutputBlock>* acquired_blocks, std::string* error) {
  if (!lease_guard || !acquired_blocks) {
    if (error) *error = "Invalid output acquisition destination";
    return -4;
  }

  size_t total_slots = 0;
  for (const auto& frame : frame_bindings) total_slots += frame.size();
  lease_guard->Reserve(total_slots);
  acquired_blocks->clear();
  acquired_blocks->reserve(total_slots);

  for (size_t i = 0; i < frame_bindings.size(); ++i) {
    for (const auto& binding : frame_bindings[i]) {
      const auto pool_it = output_pools.find(binding.canonical_suffix);
      if (pool_it == output_pools.end() || !pool_it->second) {
        if (error) {
          *error =
              "Output pool not found for suffix: " + binding.canonical_suffix;
        }
        return -4;
      }
      void* raw_block = nullptr;
      const int acquire_result = pool_it->second->Acquire(&raw_block);
      if (acquire_result != 0 || !raw_block) {
        if (error) {
          *error = "Failed to acquire output block from pool for suffix " +
                   binding.canonical_suffix;
        }
        return -4;
      }
      lease_guard->Track(pool_it->second, raw_block);
      acquired_blocks->push_back({i, binding.key, pool_it->second, raw_block});
    }
  }
  return 0;
}

void PublishOperatorOutputs(
    const std::vector<AcquiredOutputBlock>& acquired_blocks,
    llm_edgeflow::operator_api::NamedIoBatch* outputs,
    ScopedOutputLeaseGuard* lease_guard) {
  struct PendingOutput {
    std::shared_ptr<void>* destination = nullptr;
    std::shared_ptr<void> value;
  };
  std::vector<PendingOutput> pending_outputs;
  pending_outputs.reserve(acquired_blocks.size());

  for (const auto& acquired : acquired_blocks) {
    if (OutputPoolState::GetPublishFailureCountdown() >= 0) {
      if (OutputPoolState::GetPublishFailureCountdown() == 0) {
        OutputPoolState::SetPublishFailureCountdown(-1);
        throw std::bad_alloc();
      }
      OutputPoolState::SetPublishFailureCountdown(
          OutputPoolState::GetPublishFailureCountdown() - 1);
    }
    OutputPoolDeleter deleter{acquired.pool, acquired.raw_block};
    auto value = std::shared_ptr<void>(acquired.raw_block, deleter);
    auto* destination = &(*outputs)[acquired.frame_idx][acquired.key];
    lease_guard->Untrack(acquired.raw_block);
    pending_outputs.push_back({destination, std::move(value)});
  }

  lease_guard->Commit();
  for (auto& pending : pending_outputs) {
    if (pending.destination) {
      *pending.destination = std::move(pending.value);
    }
  }
}

}  // namespace alg_framework
