#pragma once

#include <any>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/operator_io_contracts.h"
#include "adapter/operator_value_type.h"
#include "contracts/parameter_set.h"
#include "core/alg_context.h"
#include "core/blackboard_key.h"
#include "core/port_definition.h"
#include "core/validated_node_plan.h"

namespace llm_edgeflow {

/**
 * @brief 外部宿主输入批次同步只读视图
 */
class ExternalInputBatchView {
 public:
  size_t count = 0;
  std::unordered_map<std::string, std::vector<std::shared_ptr<void>>> slots;
  std::unordered_map<std::string, std::string> slot_types;

  const OperatorValueTypeBinding* binding = nullptr;

  template <typename T>
  std::optional<T> Read(const std::string& slot, size_t index,
                        AdapterStatus* status) const {
    auto it = slots.find(slot);
    auto type = slot_types.find(slot);
    if (!binding || binding->value_type != typeid(T) ||
        type == slot_types.end() ||
        type->second != binding->external_c_type_name || it == slots.end() ||
        index >= it->second.size() || !it->second[index]) {
      if (status)
        *status = AdapterStatus::InvalidInput(
            "Missing or mismatched input binding", slot);
      return std::nullopt;
    }
    std::string error;
    const void* raw = it->second[index].get();
    if (binding->validate_external(raw, InputLimits{}, &error) != 0) {
      if (status) *status = AdapterStatus::InvalidInput(std::move(error), slot);
      return std::nullopt;
    }
    return std::any_cast<T>(binding->read_value(raw));
  }
};

/** 同步 Encode 借用租约池中的槽位和不可变规格。 */
class ExternalOutputBatchView {
 public:
  size_t count = 0;
  bool required = true;
  std::unordered_map<std::string, std::vector<void*>> leased_slots;
  std::unordered_map<std::string, std::string> slot_types;
  std::unordered_map<std::string, const ResolvedOutputPoolSpec*> pool_specs;

  const ResolvedOutputPoolSpec* GetPoolSpec(
      const std::string& slot_name) const {
    auto it = pool_specs.find(slot_name);
    return it == pool_specs.end() ? nullptr : it->second;
  }

  const OperatorValueTypeBinding* binding = nullptr;

  bool HasSlot(const std::string& slot, size_t index) const {
    auto it = leased_slots.find(slot);
    return it != leased_slots.end() && index < it->second.size() &&
           it->second[index];
  }

  template <typename T>
  AdapterStatus Write(const std::string& slot, size_t index,
                      uint64_t request_id, const T& value) const {
    auto type = slot_types.find(slot);
    if (!binding || binding->value_type != typeid(T) ||
        type == slot_types.end() ||
        type->second != binding->external_c_type_name || !HasSlot(slot, index))
      return AdapterStatus::BufferTooSmall(
          "Missing or mismatched output binding", slot);
    const auto* spec = GetPoolSpec(slot);
    if (!spec)
      return AdapterStatus::BufferTooSmall(
          "Missing output capacity specification", slot);
    auto* raw = leased_slots.at(slot)[index];
    auto result = binding->write_value(raw, std::any(value), *spec);
    if (result.IsOk() && binding->write_request_id)
      binding->write_request_id(raw, request_id);
    return result;
  }
};

/**
 * @brief 输入解码选项与调用诊断上下文
 */
inline constexpr size_t kMaxProcessBatchSize = 64;
inline constexpr char kCommonIoName[] = "common";

using IoPortBindings = std::unordered_map<std::string, std::string>;

struct InputDecodeOptions {
  std::string type;
  std::string name;
  const std::vector<uint64_t>* request_ids = nullptr;
  const ParameterValues* params = nullptr;
  const IoPortBindings* ports = nullptr;

  std::string Label() const { return type + "/" + name; }
  std::string Port(const std::string& logical_port) const {
    if (!ports) return {};
    const auto it = ports->find(logical_port);
    return it == ports->end() ? std::string{} : it->second;
  }
  template <typename P>
  const P& Params() const {
    if (!params) throw std::logic_error("Missing parsed input parameters");
    return params->Get<P>();
  }
};

struct OutputEncodeOptions {
  std::string type;
  std::string name;
  const std::vector<uint64_t>* request_ids = nullptr;
  const ParameterValues* params = nullptr;
  const IoPortBindings* ports = nullptr;

  std::string Label() const { return type + "/" + name; }
  std::string Port(const std::string& logical_port) const {
    if (!ports) return {};
    const auto it = ports->find(logical_port);
    return it == ports->end() ? std::string{} : it->second;
  }
  template <typename P>
  const P& Params() const {
    if (!params) throw std::logic_error("Missing parsed output parameters");
    return params->Get<P>();
  }
};

struct ExternalSlotDefinition {
  std::type_index value_type{typeid(void)};
  std::string type_suffix;
  bool required = true;
  std::string allocator;
  std::string allocator_params;
  uint32_t metadata_count = 0;
  int32_t metadata_type_id = 0;
};

using DecodeInputFn = int (*)(const ExternalInputBatchView& source,
                              const InputDecodeOptions& options,
                              AlgContext* context, AdapterStatus* status);
using EncodeOutputFn = int (*)(AlgContext* context,
                               const OutputEncodeOptions& options,
                               ExternalOutputBatchView* destination,
                               size_t* written_count, AdapterStatus* status);

struct InputConverterDefinition {
  std::string type;
  std::string name;
  ExternalSlotDefinition slot;
  std::vector<NodePortDefinition> logical_ports;
  ParameterSet params;
  DecodeInputFn decode_fn = nullptr;
  std::string Label() const { return type + "/" + name; }
};

struct OutputConverterDefinition {
  std::string type;
  std::string name;
  ExternalSlotDefinition slot;
  std::vector<NodePortDefinition> logical_ports;
  ParameterSet params;
  EncodeOutputFn encode_fn = nullptr;
  std::string Label() const { return type + "/" + name; }
};

}  // namespace llm_edgeflow
