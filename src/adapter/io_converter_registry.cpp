#include "adapter/io_converter_registry.h"

namespace llm_edgeflow {

IoConverterRegistry& IoConverterRegistry::Instance() {
  static IoConverterRegistry instance;
  return instance;
}

bool IoConverterRegistry::RegisterInputConverter(
    const InputConverterDefinition& def) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (def.converter_id.empty()) {
    conflict_errors_.push_back(
        "Empty converter_id in InputConverterDefinition");
    return false;
  }
  if (!def.decode_fn) {
    conflict_errors_.push_back(
        "Missing decode_fn in InputConverterDefinition for: " +
        def.converter_id);
    return false;
  }
  if (def.transport != "operator") {
    conflict_errors_.push_back(
        "Invalid transport '" + def.transport +
        "' in InputConverterDefinition for: " + def.converter_id);
    return false;
  }
  if (def.schema_id.empty()) {
    conflict_errors_.push_back(
        "Empty schema_id in InputConverterDefinition for: " + def.converter_id);
    return false;
  }
  if (def.schema_version < 1) {
    conflict_errors_.push_back(
        "Invalid schema_version (" + std::to_string(def.schema_version) +
        ") in InputConverterDefinition for: " + def.converter_id);
    return false;
  }
  if (def.external_type.empty()) {
    conflict_errors_.push_back(
        "Empty external_type in InputConverterDefinition for: " +
        def.converter_id);
    return false;
  }
  if (def.external_slots.empty()) {
    conflict_errors_.push_back(
        "Empty external_slots in InputConverterDefinition for: " +
        def.converter_id);
    return false;
  }
  for (const auto& slot : def.external_slots) {
    if (slot.slot_name.empty() || slot.type_id.empty()) {
      conflict_errors_.push_back(
          "Invalid external_slot (empty slot_name or type_id) in "
          "InputConverterDefinition for: " +
          def.converter_id);
      return false;
    }
    if (slot.type_suffix.empty()) {
      conflict_errors_.push_back(
          "Empty type_suffix for operator slot '" + slot.slot_name +
          "' in InputConverterDefinition for: " + def.converter_id);
      return false;
    }
  }
  if (def.logical_ports.empty()) {
    conflict_errors_.push_back(
        "Empty logical_ports in InputConverterDefinition for: " +
        def.converter_id);
    return false;
  }
  for (const auto& port : def.logical_ports) {
    if (port.logical_name.empty() || port.type_id.empty()) {
      conflict_errors_.push_back(
          "Invalid logical_port (empty logical_name or type_id) in "
          "InputConverterDefinition for: " +
          def.converter_id);
      return false;
    }
  }
  if (def.max_batch_size == 0) {
    conflict_errors_.push_back(
        "Invalid max_batch_size (0) in InputConverterDefinition for: " +
        def.converter_id);
    return false;
  }

  auto it = input_converters_.find(def.converter_id);
  if (it != input_converters_.end()) {
    conflict_errors_.push_back("Duplicate InputConverter registration: " +
                               def.converter_id);
    return false;
  }

  input_converters_[def.converter_id] = def;
  return true;
}

bool IoConverterRegistry::RegisterOutputConverter(
    const OutputConverterDefinition& def) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (def.converter_id.empty()) {
    conflict_errors_.push_back(
        "Empty converter_id in OutputConverterDefinition");
    return false;
  }
  if (!def.encode_fn) {
    conflict_errors_.push_back(
        "Missing encode_fn in OutputConverterDefinition for: " +
        def.converter_id);
    return false;
  }
  if (def.transport != "operator") {
    conflict_errors_.push_back(
        "Invalid transport '" + def.transport +
        "' in OutputConverterDefinition for: " + def.converter_id);
    return false;
  }
  if (def.schema_id.empty()) {
    conflict_errors_.push_back(
        "Empty schema_id in OutputConverterDefinition for: " +
        def.converter_id);
    return false;
  }
  if (def.schema_version < 1) {
    conflict_errors_.push_back(
        "Invalid schema_version (" + std::to_string(def.schema_version) +
        ") in OutputConverterDefinition for: " + def.converter_id);
    return false;
  }
  if (def.external_type.empty()) {
    conflict_errors_.push_back(
        "Empty external_type in OutputConverterDefinition for: " +
        def.converter_id);
    return false;
  }
  if (def.external_slots.empty()) {
    conflict_errors_.push_back(
        "Empty external_slots in OutputConverterDefinition for: " +
        def.converter_id);
    return false;
  }
  for (const auto& slot : def.external_slots) {
    if (slot.slot_name.empty() || slot.type_id.empty()) {
      conflict_errors_.push_back(
          "Invalid external_slot (empty slot_name or type_id) in "
          "OutputConverterDefinition for: " +
          def.converter_id);
      return false;
    }
    if (slot.type_suffix.empty()) {
      conflict_errors_.push_back(
          "Empty type_suffix for operator slot '" + slot.slot_name +
          "' in OutputConverterDefinition for: " + def.converter_id);
      return false;
    }
  }
  if (def.logical_ports.empty()) {
    conflict_errors_.push_back(
        "Empty logical_ports in OutputConverterDefinition for: " +
        def.converter_id);
    return false;
  }
  for (const auto& port : def.logical_ports) {
    if (port.logical_name.empty() || port.type_id.empty()) {
      conflict_errors_.push_back(
          "Invalid logical_port (empty logical_name or type_id) in "
          "OutputConverterDefinition for: " +
          def.converter_id);
      return false;
    }
  }
  if (def.max_batch_size == 0) {
    conflict_errors_.push_back(
        "Invalid max_batch_size (0) in OutputConverterDefinition for: " +
        def.converter_id);
    return false;
  }

  auto it = output_converters_.find(def.converter_id);
  if (it != output_converters_.end()) {
    conflict_errors_.push_back("Duplicate OutputConverter registration: " +
                               def.converter_id);
    return false;
  }

  output_converters_[def.converter_id] = def;
  return true;
}

const InputConverterDefinition* IoConverterRegistry::FindInputConverter(
    const std::string& converter_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = input_converters_.find(converter_id);
  if (it != input_converters_.end()) {
    return &it->second;
  }
  return nullptr;
}

const OutputConverterDefinition* IoConverterRegistry::FindOutputConverter(
    const std::string& converter_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = output_converters_.find(converter_id);
  if (it != output_converters_.end()) {
    return &it->second;
  }
  return nullptr;
}

std::vector<InputConverterDefinition> IoConverterRegistry::AllInputConverters()
    const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<InputConverterDefinition> result;
  result.reserve(input_converters_.size());
  for (const auto& [_, def] : input_converters_) {
    result.push_back(def);
  }
  return result;
}

std::vector<OutputConverterDefinition>
IoConverterRegistry::AllOutputConverters() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<OutputConverterDefinition> result;
  result.reserve(output_converters_.size());
  for (const auto& [_, def] : output_converters_) {
    result.push_back(def);
  }
  return result;
}

bool IoConverterRegistry::HasConflict() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !conflict_errors_.empty();
}

std::vector<std::string> IoConverterRegistry::GetConflictErrors() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return conflict_errors_;
}

void IoConverterRegistry::ClearForTesting() {
  std::lock_guard<std::mutex> lock(mutex_);
  input_converters_.clear();
  output_converters_.clear();
  conflict_errors_.clear();
}

void IoConverterRegistry::ResetConflictForTesting() {
  std::lock_guard<std::mutex> lock(mutex_);
  conflict_errors_.clear();
}

}  // namespace llm_edgeflow
