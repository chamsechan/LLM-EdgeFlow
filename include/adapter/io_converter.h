#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/operator_io_contracts.h"
#include "core/alg_context.h"
#include "core/blackboard_key.h"
#include "core/port_definition.h"
#include "core/validated_node_plan.h"
#include "platform_mock/operator_data_types.h"

namespace llm_edgeflow {

/**
 * @brief 外部类型标识萃取器 (SSOT Type Traits for Operator structs)
 */
template <typename T>
struct ExternalTypeTraits {
  static constexpr const char* TypeName() { return ""; }
};

#define DECLARE_EXTERNAL_TYPE_TRAITS(Type, Name)             \
  template <>                                                \
  struct ExternalTypeTraits<Type> {                          \
    static constexpr const char* TypeName() { return Name; } \
  }

DECLARE_EXTERNAL_TYPE_TRAITS(CompanyString, "CompanyString");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyBuffer, "CompanyBuffer");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyAny, "CompanyAny");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyFrame, "CompanyFrame");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOdOutput, "CompanyOdOutput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorAuditInput,
                             "CompanyOperatorAuditInput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorAuditOutput,
                             "CompanyOperatorAuditOutput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorKeywordInput,
                             "CompanyOperatorKeywordInput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorKeywordOutput,
                             "CompanyOperatorKeywordOutput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorEntityInput,
                             "CompanyOperatorEntityInput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorEntityOutput,
                             "CompanyOperatorEntityOutput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorDocInput,
                             "CompanyOperatorDocInput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorDocOutput,
                             "CompanyOperatorDocOutput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorAudioInput,
                             "CompanyOperatorAudioInput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorAudioOutput,
                             "CompanyOperatorAudioOutput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorRerankInput,
                             "CompanyOperatorRerankInput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorRerankOutput,
                             "CompanyOperatorRerankOutput");
DECLARE_EXTERNAL_TYPE_TRAITS(int, "int");

/**
 * @brief 外部宿主输入批次同步只读视图
 */
class ExternalInputBatchView {
 public:
  size_t count = 0;
  std::unordered_map<std::string, std::vector<std::shared_ptr<void>>> slots;
  std::unordered_map<std::string, std::string> slot_types;

  template <typename T>
  const T* GetSlot(const std::string& slot_name, size_t index) const {
    auto it = slots.find(slot_name);
    if (it == slots.end() || index >= it->second.size()) return nullptr;
    if constexpr (!std::is_void_v<T>) {
      auto type = slot_types.find(slot_name);
      if (type == slot_types.end() ||
          type->second != ExternalTypeTraits<T>::TypeName())
        return nullptr;
    }
    return static_cast<const T*>(it->second[index].get());
  }
};

/** Synchronous Encode borrows slots and immutable specs from leased pools. */
class ExternalOutputBatchView {
 public:
  size_t count = 0;
  std::unordered_map<std::string, std::vector<void*>> leased_slots;
  std::unordered_map<std::string, std::string> slot_types;
  std::unordered_map<std::string, const ResolvedOutputPoolSpec*> pool_specs;

  const ResolvedOutputPoolSpec* GetPoolSpec(
      const std::string& slot_name) const {
    auto it = pool_specs.find(slot_name);
    return it == pool_specs.end() ? nullptr : it->second;
  }

  template <typename T>
  T* GetSlot(const std::string& slot_name, size_t index) const {
    auto it = leased_slots.find(slot_name);
    if (it == leased_slots.end() || index >= it->second.size()) return nullptr;
    if constexpr (!std::is_void_v<T>) {
      auto type = slot_types.find(slot_name);
      if (type == slot_types.end() ||
          type->second != ExternalTypeTraits<T>::TypeName())
        return nullptr;
    }
    return static_cast<T*>(it->second[index]);
  }

  size_t GetSlotCapacity(const std::string& slot_name,
                         const std::string& field_name,
                         size_t default_cap = 0) const {
    const auto* spec = GetPoolSpec(slot_name);
    if (!spec) return default_cap;
    auto it = spec->capacities.find(field_name);
    return it == spec->capacities.end() ? default_cap : it->second;
  }
};

/**
 * @brief 输入解码选项与调用诊断上下文
 */
struct InputDecodeOptions {
  std::string converter_id;
};

/**
 * @brief 输出编码选项与调用诊断上下文
 */
struct OutputEncodeOptions {
  std::string converter_id;
};

/**
 * @brief 逻辑端口到实际 Blackboard Key 的映射助手
 */
class InputPortBindings {
 public:
  InputPortBindings() = default;
  explicit InputPortBindings(
      std::unordered_map<std::string, std::string> mapping)
      : mapping_(std::move(mapping)) {}

  bool HasKey(const std::string& logical_name) const {
    return mapping_.find(logical_name) != mapping_.end();
  }

  template <typename T>
  BlackboardKey<T> Key(const std::string& logical_name) const {
    auto it = mapping_.find(logical_name);
    if (it != mapping_.end()) {
      return BlackboardKey<T>{it->second.c_str(),
                              BlackboardTypeTraits<T>::TypeName()};
    }
    return BlackboardKey<T>{"", BlackboardTypeTraits<T>::TypeName()};
  }

  const std::string& GetActualKey(const std::string& logical_name) const {
    auto it = mapping_.find(logical_name);
    if (it != mapping_.end()) {
      return it->second;
    }
    static const std::string kEmpty;
    return kEmpty;
  }

  const std::unordered_map<std::string, std::string>& All() const {
    return mapping_;
  }

 private:
  std::unordered_map<std::string, std::string> mapping_;
};

class OutputPortBindings {
 public:
  OutputPortBindings() = default;
  explicit OutputPortBindings(
      std::unordered_map<std::string, std::string> mapping)
      : mapping_(std::move(mapping)) {}

  bool HasKey(const std::string& logical_name) const {
    return mapping_.find(logical_name) != mapping_.end();
  }

  template <typename T>
  BlackboardKey<T> Key(const std::string& logical_name) const {
    auto it = mapping_.find(logical_name);
    if (it != mapping_.end()) {
      return BlackboardKey<T>{it->second.c_str(),
                              BlackboardTypeTraits<T>::TypeName()};
    }
    return BlackboardKey<T>{"", BlackboardTypeTraits<T>::TypeName()};
  }

  const std::string& GetActualKey(const std::string& logical_name) const {
    auto it = mapping_.find(logical_name);
    if (it != mapping_.end()) {
      return it->second;
    }
    static const std::string kEmpty;
    return kEmpty;
  }

  const std::unordered_map<std::string, std::string>& All() const {
    return mapping_;
  }

 private:
  std::unordered_map<std::string, std::string> mapping_;
};

/**
 * @brief 外部槽位定义 (Operator 槽位)
 */
struct ExternalSlotDefinition {
  std::string slot_name;
  std::string type_id;
  PortDirection direction = PortDirection::kInput;
  bool required = true;
  std::string value_type;
  std::string type_suffix;  // Operator ValueType 规范后缀 (如 "plain_text",
                            // "entity_out")
  std::vector<std::string> capacity_fields;
  std::string key_suffix;  // 外部 map key 后缀 (为空时使用 type_suffix)

  ExternalSlotDefinition() = default;
  ExternalSlotDefinition(std::string slot_name, std::string type_id,
                         PortDirection direction = PortDirection::kInput,
                         bool required = true, std::string value_type = "",
                         std::string type_suffix = "",
                         std::vector<std::string> capacity_fields = {},
                         std::string key_suffix = "")
      : slot_name(std::move(slot_name)),
        type_id(std::move(type_id)),
        direction(direction),
        required(required),
        value_type(std::move(value_type)),
        type_suffix(std::move(type_suffix)),
        capacity_fields(std::move(capacity_fields)),
        key_suffix(std::move(key_suffix)) {}

  const std::string& KeySuffix() const {
    return !key_suffix.empty() ? key_suffix : type_suffix;
  }
};

// 统一输入/输出转换回调函数指针类型
using DecodeInputFn = int (*)(const ExternalInputBatchView& source,
                              const InputDecodeOptions& options,
                              const InputPortBindings& bindings,
                              AlgContext* context, AdapterStatus* status);

using EncodeOutputFn = int (*)(AlgContext* context,
                               const OutputPortBindings& bindings,
                               const OutputEncodeOptions& options,
                               ExternalOutputBatchView* destination,
                               size_t* written_count, AdapterStatus* status);

/**
 * @brief 输入转换器 Definition
 */
struct InputConverterDefinition {
  std::string converter_id;

  std::string schema_id;
  int schema_version = 1;
  std::string external_type;
  std::vector<ExternalSlotDefinition> external_slots;
  std::vector<NodePortDefinition> logical_ports;  // 发布的内部逻辑输出端口
  size_t max_batch_size = 64;

  DecodeInputFn decode_fn = nullptr;
};

/**
 * @brief 输出转换器 Definition
 */
struct OutputConverterDefinition {
  std::string converter_id;

  std::string schema_id;
  int schema_version = 1;
  std::string external_type;
  std::vector<NodePortDefinition> logical_ports;  // 消费的内部逻辑输入端口
  std::vector<ExternalSlotDefinition> external_slots;
  std::string cardinality = "1:1";
  size_t max_batch_size = 64;
  std::string capacity_policy = "reject_overflow";

  EncodeOutputFn encode_fn = nullptr;
};

}  // namespace llm_edgeflow
