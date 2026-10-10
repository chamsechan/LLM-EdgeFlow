#include "adapter/operator/operator_process_binding.h"

#include <unordered_set>
#include <utility>

namespace llm_edgeflow {
namespace {
using NamedIo = llm_edgeflow::operator_api::NamedIo;

// A repeated carrier uses name.type; a unique carrier accepts any namespace.
bool FindSlotKey(const NamedIo& row, const std::string& type,
                 const std::string& name, bool repeated_type, size_t frame,
                 std::string* found, std::string* error) {
  found->clear();
  for (const auto& [key, value] : row) {
    std::string prefix, suffix;
    if (!OperatorValueTypeRegistry::ParseKey(key, &prefix, &suffix)) {
      if (error)
        *error =
            "Invalid key format in frame " + std::to_string(frame) + ": " + key;
      return false;
    }
    if (suffix != type || (repeated_type && prefix != name)) continue;
    if (!found->empty()) {
      if (error)
        *error = "Duplicate slot mapping for " + type + "/" + name +
                 " in frame " + std::to_string(frame);
      return false;
    }
    *found = key;
  }
  return true;
}

template <typename Selected>
bool RepeatedType(const std::vector<Selected>& selected,
                  const std::string& type) {
  size_t count = 0;
  for (const auto& entry : selected) count += entry.converter->type == type;
  return count > 1;
}
}  // namespace

int ValidateAndExtractOperatorInputs(
    const llm_edgeflow::operator_api::NamedIoBatch& inputs,
    const std::vector<SelectedInput>& selected, const InputLimits& limits,
    std::vector<ExternalInputBatchView>* out_views,
    std::vector<uint64_t>* request_ids, std::string* error) {
  if (!out_views || !request_ids) {
    if (error) *error = "Null input extraction destination";
    return -3;
  }
  out_views->clear();
  out_views->resize(selected.size());
  request_ids->assign(inputs.size(), 0);
  std::vector<bool> has_id(inputs.size(), false);
  std::vector<std::unordered_set<std::string>> recognized(inputs.size());
  for (size_t index = 0; index < selected.size(); ++index) {
    const auto& def = *selected[index].converter;
    const auto* binding = &selected[index].host_binding;
    auto& view = (*out_views)[index];
    view.count = inputs.size();
    view.binding = binding;
    view.slot_types[def.type] = binding->external_c_type_name;
    const auto service = binding->ServiceType(def.name);
    auto& payloads = view.slots[def.type];
    payloads.resize(inputs.size());
    for (size_t frame = 0; frame < inputs.size(); ++frame) {
      std::string key;
      if (!FindSlotKey(inputs[frame], def.type, def.name,
                       RepeatedType(selected, def.type), frame, &key, error))
        return -3;
      if (key.empty()) {
        if (def.slot.required) {
          if (error)
            *error = "Missing required input " + def.Label() + " in frame " +
                     std::to_string(frame);
          return -3;
        }
        continue;
      }
      const auto& payload = inputs[frame].at(key);
      if (!payload) {
        if (error) *error = "Null input shared_ptr for key: " + key;
        return -3;
      }
      std::string validation_error;
      const int result =
          binding->validate_external(payload.get(), limits, &validation_error);
      if (result != 0) {
        if (error)
          *error = "Validation failed for input key " + key + ": " +
                   validation_error;
        return result;
      }
      if (service && binding->read_service_type(payload.get()) != *service) {
        if (error)
          *error = "Struct " + binding->external_c_type_name + " frame " +
                   std::to_string(frame) + " has service_type " +
                   std::to_string(binding->read_service_type(payload.get())) +
                   "; expected " + std::to_string(*service);
        return -3;
      }
      if (binding->read_request_id) {
        const auto id = binding->read_request_id(payload.get());
        if (has_id[frame] && (*request_ids)[frame] != id) {
          if (error)
            *error = "Struct " + binding->external_c_type_name + " frame " +
                     std::to_string(frame) + " has request_id " +
                     std::to_string(id) + "; expected " +
                     std::to_string((*request_ids)[frame]);
          return -3;
        }
        (*request_ids)[frame] = id;
        has_id[frame] = true;
      }
      recognized[frame].insert(key);
      payloads[frame] = payload;
    }
  }
  for (size_t frame = 0; frame < inputs.size(); ++frame) {
    if (recognized[frame].size() != inputs[frame].size()) {
      if (error)
        *error = "Unknown extra input keys present in frame " +
                 std::to_string(frame);
      return -3;
    }
    if (!has_id[frame]) {
      if (error)
        *error = "No request_id source in frame " + std::to_string(frame);
      return -3;
    }
  }
  return 0;
}

int ResolveOperatorOutputs(
    const llm_edgeflow::operator_api::NamedIoBatch& outputs,
    const std::vector<SelectedOutput>& selected,
    std::vector<std::vector<FrameOutputBinding>>* frame_bindings,
    std::string* error) {
  if (!frame_bindings) {
    if (error) *error = "Null frame_bindings pointer";
    return -4;
  }
  frame_bindings->clear();
  frame_bindings->resize(outputs.size());
  for (size_t frame = 0; frame < outputs.size(); ++frame) {
    std::unordered_set<std::string> recognized;
    for (size_t index = 0; index < selected.size(); ++index) {
      const auto& def = *selected[index].converter;
      std::string key;
      if (!FindSlotKey(outputs[frame], def.type, def.name,
                       RepeatedType(selected, def.type), frame, &key, error))
        return -4;
      if (key.empty()) {
        if (def.slot.required) {
          if (error)
            *error = "Missing required output " + def.Label() + " in frame " +
                     std::to_string(frame);
          return -4;
        }
        continue;
      }
      if (outputs[frame].at(key)) {
        if (error)
          *error = "Output shared_ptr must be null for key " + key +
                   " in frame " + std::to_string(frame);
        return -4;
      }
      recognized.insert(key);
      (*frame_bindings)[frame].push_back({key, index});
    }
    if (recognized.size() != outputs[frame].size()) {
      if (error)
        *error = "Unknown extra output keys present in frame " +
                 std::to_string(frame);
      return -4;
    }
  }
  return 0;
}

int AcquireOperatorOutputBlocks(
    const std::vector<std::vector<FrameOutputBinding>>& frame_bindings,
    const std::vector<std::shared_ptr<OutputPoolState>>& output_pools,
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
      if (binding.output_index >= output_pools.size() ||
          !output_pools[binding.output_index]) {
        if (error) {
          *error = "Missing output pool for slot " +
                   std::to_string(binding.output_index);
        }
        return -5;
      }
      auto pool = output_pools[binding.output_index];
      void* block = nullptr;
      int acq_ret = pool->Acquire(&block);
      if (acq_ret != 0 || !block) {
        if (error) {
          *error = "Output pool exhausted for slot " +
                   std::to_string(binding.output_index);
        }
        return -4;
      }
      lease_guard->Track(pool, block);
      acquired_blocks->push_back(
          {f, binding.key, pool, block, binding.output_index});
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
