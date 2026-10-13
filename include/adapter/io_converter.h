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
  std::string slot;
  std::string external_c_type_name;
  std::vector<std::shared_ptr<void>> values;

  const OperatorValueTypeBinding* binding = nullptr;

  template <typename T>
  std::optional<T> Read(const std::string& slot_name, size_t index,
                        AdapterStatus* status) const {
    if (!binding || binding->value_type != typeid(T) || slot != slot_name ||
        external_c_type_name != binding->external_c_type_name ||
        index >= values.size() || !values[index]) {
      if (status)
        *status = AdapterStatus::InvalidInput(
            "Missing or mismatched input binding", slot_name);
      return std::nullopt;
    }
    std::string error;
    const void* raw = values[index].get();
    if (binding->validate_external(raw, InputLimits{}, &error) != 0) {
      if (status)
        *status = AdapterStatus::InvalidInput(std::move(error), slot_name);
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
  std::string slot;
  std::string external_c_type_name;
  std::vector<void*> leased_values;
  const ResolvedOutputPoolSpec* pool_spec = nullptr;

  const OperatorValueTypeBinding* binding = nullptr;

  bool HasSlot(const std::string& slot_name, size_t index) const {
    return slot == slot_name && index < leased_values.size() &&
           leased_values[index];
  }

  template <typename T>
  AdapterStatus Write(const std::string& slot_name, size_t index,
                      const T& value) const {
    if (!binding || binding->value_type != typeid(T) ||
        external_c_type_name != binding->external_c_type_name ||
        !HasSlot(slot_name, index))
      return AdapterStatus::BufferTooSmall(
          "Missing or mismatched output binding", slot_name);
    if (!pool_spec)
      return AdapterStatus::BufferTooSmall(
          "Missing output capacity specification", slot_name);
    return binding->write_value(leased_values[index], std::any(value),
                                *pool_spec);
  }
};

/**
 * @brief 输入解码选项与调用诊断上下文
 */
inline constexpr size_t kMaxProcessBatchSize = 64;
inline constexpr char kCommonIoName[] = "common";

using IoPortBindings = std::unordered_map<std::string, std::string>;

struct IoConverterOptions {
  std::string type;
  std::string name;
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
    if (!params) throw std::logic_error("Missing parsed converter parameters");
    return params->Get<P>();
  }
};

struct InputDecodeOptions : IoConverterOptions {};
struct OutputEncodeOptions : IoConverterOptions {};

struct ExternalSlotDefinition {
  std::type_index value_type{typeid(void)};
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

struct IoConverterDefinition {
  std::string type;
  std::string name;
  ExternalSlotDefinition slot;
  std::vector<NodePortDefinition> logical_ports;
  ParameterSet params;
  std::string Label() const { return type + "/" + name; }
};

struct InputConverterDefinition : IoConverterDefinition {
  DecodeInputFn decode_fn = nullptr;
};

struct OutputConverterDefinition : IoConverterDefinition {
  EncodeOutputFn encode_fn = nullptr;
};

}  // namespace llm_edgeflow
