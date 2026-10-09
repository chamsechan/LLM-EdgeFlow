#include "adapter/operator/operator_process_binding.h"

#include <unordered_map>
#include <utility>

namespace llm_edgeflow {
namespace {

// 同侧 type 唯一时，宿主可自由选择前缀；复用 type 时必须以业务名区分。
// service_type 只用于载荷校验，不参与 key 寻址。
template <typename Item>
bool ResolveFrameKeys(const operator_api::NamedIo& frame,
                      const std::vector<Item>& items, const char* side,
                      size_t frame_idx, std::vector<std::string>* keys,
                      std::string* error) {
  keys->assign(items.size(), {});
  std::unordered_map<std::string, size_t> type_counts;
  for (const auto& item : items) ++type_counts[item.converter->type];

  const std::string location = " in frame " + std::to_string(frame_idx);
  for (const auto& [key, value] : frame) {
    std::string ns;
    std::string type;
    if (!OperatorValueTypeRegistry::ParseKey(key, &ns, &type)) {
      if (error)
        *error = std::string("Invalid ") + side + " key format" + location +
                 ": " + key;
      return false;
    }
    size_t index = 0;
    for (; index < items.size(); ++index) {
      const auto& converter = *items[index].converter;
      if (converter.type == type &&
          (type_counts.at(type) == 1 || ns == converter.name))
        break;
    }
    if (index == items.size()) {
      if (error)
        *error = std::string("Unknown or ambiguous ") + side + " key" +
                 location + ": " + key;
      return false;
    }
    if (!(*keys)[index].empty()) {
      if (error)
        *error = std::string("Duplicate ") + side + " slot mapping for " +
                 items[index].converter->Label() + location;
      return false;
    }
    (*keys)[index] = key;
  }
  for (size_t index = 0; index < items.size(); ++index) {
    if ((*keys)[index].empty() && items[index].converter->slot.required) {
      if (error)
        *error = std::string("Missing required ") + side + " slot for " +
                 items[index].converter->Label() + location;
      return false;
    }
  }
  return true;
}

}  // namespace

int ValidateAndExtractOperatorInputs(
    const operator_api::NamedIoBatch& inputs,
    const std::vector<SelectedInput>& items, const InputLimits& limits,
    std::vector<ExternalInputBatchView>* out_views, std::string* error) {
  if (!out_views) {
    if (error) *error = "Null out_views pointer";
    return -3;
  }
  out_views->assign(items.size(), {});
  for (size_t k = 0; k < items.size(); ++k) {
    const auto& slot = items[k].converter->slot;
    auto& view = (*out_views)[k];
    view.count = inputs.size();
    view.slot_types[slot.type_suffix] = slot.type_id;
    view.slots[slot.type_suffix].resize(inputs.size());
  }

  for (size_t i = 0; i < inputs.size(); ++i) {
    std::vector<std::string> keys;
    if (!ResolveFrameKeys(inputs[i], items, "input", i, &keys, error))
      return -3;
    for (size_t k = 0; k < items.size(); ++k) {
      if (keys[k].empty()) continue;
      const auto& item = items[k];
      const auto& slot = item.converter->slot;
      const auto& payload = inputs[i].at(keys[k]);
      if (!payload) {
        if (error) *error = "Null input shared_ptr for key: " + keys[k];
        return -3;
      }
      const auto* binding =
          OperatorValueTypeRegistry::Instance().GetBindingBySuffix(
              slot.type_suffix);
      if (!binding || !binding->validate_external) {
        if (error)
          *error =
              "Missing Operator input ValueType binding or "
              "validate_external for suffix '" +
              slot.type_suffix + "'";
        return -3;
      }
      std::string validation_error;
      const int result =
          binding->validate_external(payload.get(), limits, &validation_error);
      if (result != 0) {
        if (error)
          *error = "Validation failed for input key " + keys[k] + ": " +
                   validation_error;
        return result;
      }
      if (item.converter->service_type.has_value() &&
          binding->read_service_type) {
        const auto actual = binding->read_service_type(payload.get());
        if (actual != item.converter->service_type) {
          if (error)
            *error = "service_type mismatch for input " +
                     item.converter->Label() + " struct '" + slot.type_suffix +
                     "' in frame " + std::to_string(i) + ": expected " +
                     std::to_string(*item.converter->service_type) + ", got " +
                     (actual.has_value() ? std::to_string(*actual) : "none");
          return -3;
        }
      }
      (*out_views)[k].slots.at(slot.type_suffix)[i] = payload;
    }
  }
  return 0;
}

int ResolveOperatorOutputs(
    const operator_api::NamedIoBatch& outputs,
    const std::vector<SelectedOutput>& items,
    std::vector<std::vector<FrameOutputBinding>>* frame_bindings,
    std::string* error) {
  if (!frame_bindings) {
    if (error) *error = "Null frame_bindings pointer";
    return -4;
  }
  frame_bindings->assign(outputs.size(), {});
  for (size_t i = 0; i < outputs.size(); ++i) {
    std::vector<std::string> keys;
    if (!ResolveFrameKeys(outputs[i], items, "output", i, &keys, error))
      return -4;
    for (size_t k = 0; k < items.size(); ++k) {
      if (keys[k].empty()) continue;
      if (outputs[i].at(keys[k]) != nullptr) {
        if (error)
          *error = "Output shared_ptr must be null for key " + keys[k] +
                   " in frame " + std::to_string(i);
        return -4;
      }
      (*frame_bindings)[i].push_back({keys[k], items[k].converter->Label()});
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
      auto pit = output_pools.find(binding.item_label);
      if (pit == output_pools.end() || !pit->second) {
        if (error) {
          *error = "Missing output pool for slot " + binding.item_label;
        }
        return -5;
      }
      auto pool = pit->second;
      void* block = nullptr;
      int acq_ret = pool->Acquire(&block);
      if (acq_ret != 0 || !block) {
        if (error) {
          *error = "Output pool exhausted for slot " + binding.item_label;
        }
        return -4;
      }
      lease_guard->Track(pool, block);
      acquired_blocks->push_back(
          {f, binding.key, pool, block, binding.item_label});
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
