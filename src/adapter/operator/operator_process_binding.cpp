#include "adapter/operator/operator_process_binding.h"

#include <unordered_set>
#include <utility>

namespace llm_edgeflow {

int ValidateAndExtractOperatorInputs(
    const llm_edgeflow::operator_api::NamedIoBatch& inputs,
    const std::vector<SelectedInput>& items, const InputLimits& limits,
    ExternalInputBatchView* out_view, std::string* error) {
  if (!out_view) {
    if (error) *error = "Null out_view pointer";
    return -3;
  }
  out_view->count = inputs.size();
  out_view->slots.clear();
  out_view->slot_types.clear();
  for (const auto& item : items) {
    const auto& slot = item.converter->slot;
    out_view->slot_types[slot.type_suffix] = slot.type_id;
    out_view->slots[slot.type_suffix].resize(inputs.size());
  }

  for (size_t i = 0; i < inputs.size(); ++i) {
    const auto& in_map = inputs[i];
    std::unordered_set<std::string> recognized_keys;

    for (const auto& item : items) {
      const auto& slot = item.converter->slot;
      std::string found_key;
      std::shared_ptr<void> payload = nullptr;

      for (const auto& [key, value] : in_map) {
        std::string suffix;
        if (!OperatorValueTypeRegistry::ParseKey(key, nullptr, &suffix)) {
          if (error) {
            *error = "Invalid input key format in frame " + std::to_string(i) +
                     ": " + key;
          }
          return -3;
        }
        if (suffix != slot.type_suffix) {
          continue;
        }
        if (!found_key.empty()) {
          if (error) {
            *error = "Duplicate input slot mapping for suffix '" +
                     slot.type_suffix + "' in frame " + std::to_string(i);
          }
          return -3;
        }
        found_key = key;
        if (!value || !value.get()) {
          if (error) *error = "Null input shared_ptr for key: " + key;
          return -3;
        }
        payload = value;
        recognized_keys.insert(key);
      }

      if (!payload && slot.required) {
        if (error) {
          *error = "Missing required input slot for suffix '" +
                   slot.type_suffix + "' in frame " + std::to_string(i);
        }
        return -3;
      }

      if (payload) {
        const auto* binding =
            OperatorValueTypeRegistry::Instance().GetBindingBySuffix(
                slot.type_suffix);
        if (!binding || !binding->validate_external) {
          if (error) {
            *error =
                "Missing Operator input ValueType binding or validate_external "
                "for suffix '" +
                slot.type_suffix + "'";
          }
          return -3;
        }
        std::string validation_error;
        int validation_result = binding->validate_external(
            payload.get(), limits, &validation_error);
        if (validation_result != 0) {
          if (error) {
            *error = "Validation failed for input key " + found_key + ": " +
                     validation_error;
          }
          return validation_result;
        }
        // 每个请求核对宿主结构体的 service_type 是否属于该项登记的业务。
        if (item.converter->service_type.has_value() &&
            binding->read_service_type) {
          const auto actual = binding->read_service_type(payload.get());
          if (actual != item.converter->service_type) {
            if (error) {
              *error =
                  "service_type mismatch for input " + item.converter->Label() +
                  " struct '" + slot.type_suffix + "' in frame " +
                  std::to_string(i) + ": expected " +
                  std::to_string(*item.converter->service_type) + ", got " +
                  (actual.has_value() ? std::to_string(*actual)
                                      : std::string("none"));
            }
            return -3;
          }
        }
        out_view->slots[slot.type_suffix][i] = std::move(payload);
      }
    }

    if (recognized_keys.size() != in_map.size()) {
      if (error) {
        *error =
            "Unknown extra input keys present in frame " + std::to_string(i);
      }
      return -3;
    }
  }

  return 0;
}

int ResolveOperatorOutputs(
    const llm_edgeflow::operator_api::NamedIoBatch& outputs,
    const std::vector<SelectedOutput>& items,
    std::vector<std::vector<FrameOutputBinding>>* frame_bindings,
    std::string* error) {
  if (!frame_bindings) {
    if (error) *error = "Null frame_bindings pointer";
    return -4;
  }
  frame_bindings->clear();
  frame_bindings->resize(outputs.size());

  for (size_t i = 0; i < outputs.size(); ++i) {
    const auto& out_map = outputs[i];
    std::unordered_set<std::string> recognized_keys;

    for (const auto& item : items) {
      const auto& slot = item.converter->slot;
      std::string found_key;

      for (const auto& [key, value] : out_map) {
        std::string suffix;
        if (!OperatorValueTypeRegistry::ParseKey(key, nullptr, &suffix)) {
          if (error) {
            *error = "Invalid output key format in frame " + std::to_string(i) +
                     ": " + key;
          }
          return -4;
        }
        if (suffix != slot.type_suffix) {
          continue;
        }
        if (!found_key.empty()) {
          if (error) {
            *error = "Duplicate output slot mapping for suffix '" +
                     slot.type_suffix + "' in frame " + std::to_string(i);
          }
          return -4;
        }
        found_key = key;
        if (value != nullptr) {
          if (error) {
            *error = "Output shared_ptr must be null for key " + key +
                     " in frame " + std::to_string(i);
          }
          return -4;
        }
        recognized_keys.insert(key);
      }

      if (found_key.empty() && slot.required) {
        if (error) {
          *error = "Missing required output slot for suffix '" +
                   slot.type_suffix + "' in frame " + std::to_string(i);
        }
        return -4;
      }

      if (!found_key.empty()) {
        (*frame_bindings)[i].push_back(
            {found_key, slot.type_suffix, slot.type_suffix});
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
    if (error) *error = "Null destination in AcquireOperatorOutputBlocks";
    return -4;
  }
  acquired_blocks->clear();

  size_t total_slots = 0;
  for (const auto& fb : frame_bindings) {
    total_slots += fb.size();
  }
  lease_guard->Reserve(total_slots);
  acquired_blocks->reserve(total_slots);

  for (size_t f = 0; f < frame_bindings.size(); ++f) {
    for (const auto& binding : frame_bindings[f]) {
      auto pit = output_pools.find(binding.logical_name);
      if (pit == output_pools.end() || !pit->second) {
        if (error) {
          *error = "Missing output pool for slot " + binding.logical_name;
        }
        return -5;
      }
      auto pool = pit->second;
      void* block = nullptr;
      int acq_ret = pool->Acquire(&block);
      if (acq_ret != 0 || !block) {
        if (error) {
          *error = "Output pool exhausted for slot " + binding.logical_name;
        }
        return -4;
      }
      lease_guard->Track(pool, block);
      acquired_blocks->push_back(
          {f, binding.key, pool, block, binding.logical_name});
    }
  }

  return 0;
}

void PublishOperatorOutputs(
    const std::vector<AcquiredOutputBlock>& acquired_blocks,
    llm_edgeflow::operator_api::NamedIoBatch* outputs,
    ScopedOutputLeaseGuard* lease_guard) {
  if (!outputs || !lease_guard) return;

  std::vector<std::pair<size_t, std::pair<std::string, std::shared_ptr<void>>>>
      staged;
  staged.reserve(acquired_blocks.size());
  for (const auto& acq : acquired_blocks) {
    std::shared_ptr<void> sp(acq.raw_block,
                             OutputPoolDeleter{acq.pool, acq.raw_block});
    staged.emplace_back(acq.frame_idx, std::make_pair(acq.key, std::move(sp)));
  }

  for (auto& [frame_idx, kv] : staged) {
    (*outputs)[frame_idx][kv.first] = std::move(kv.second);
  }

  lease_guard->Commit();
}

}  // namespace llm_edgeflow
