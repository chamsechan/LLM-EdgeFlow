#include "adapter/operator/mock/operator_process_binding.h"

#include <unordered_set>
#include <utility>

namespace llm_edgeflow {
namespace {
using NamedIo = llm_edgeflow::operator_api::NamedIo;

// 同一载体类型被多次选用时使用 name.type；仅选用一次时接受任意前缀。
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
    std::vector<ExternalInputBatchView>* out_views, std::string* error) {
  if (!out_views) {
    if (error) *error = "Null input extraction destination";
    return -3;
  }
  out_views->clear();
  out_views->resize(selected.size());
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

void PublishOperatorOutputs(
    RuntimeOutputBatch* staged,
    const std::vector<std::vector<FrameOutputBinding>>& bindings,
    const std::vector<SelectedOutput>& selected,
    llm_edgeflow::operator_api::NamedIoBatch* outputs) {
  // 发布指针前，先完成所有可能抛异常的平台字段写入。
  // 最终移交前，所有租约均由中立输出批次持有。
  for (size_t row = 0; row < staged->rows.size(); ++row) {
    for (auto& slot : staged->rows[row]) {
      const auto& entry = selected.at(slot.output_index);
      if (const auto service =
              entry.host_binding.ServiceType(entry.converter->name))
        entry.host_binding.write_service_type(slot.value.get(), *service);
    }
  }
  // 修改调用方输出前，先确定所有写入位置。协议转换时已校验现有 key；
  // 提交阶段不插入新条目，也不分配内存。
  std::vector<std::shared_ptr<void>*> destinations;
  for (size_t row = 0; row < bindings.size(); ++row)
    for (const auto& binding : bindings[row])
      destinations.push_back(&outputs->at(row).at(binding.key));
  size_t index = 0;
  for (auto& row : staged->rows)
    for (auto& slot : row) *destinations[index++] = std::move(slot.value);
}

}  // namespace llm_edgeflow
