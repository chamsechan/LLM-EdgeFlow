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

namespace llm_edgeflow {

/**
 * @brief 外部宿主输入批次同步只读视图
 */
class ExternalInputBatchView {
 public:
  // C ABI 纯指针数组
  const void** items = nullptr;
  size_t count = 0;
  std::string type_id;

  // Operator 具名槽位输入: slot_name -> vector of shared_ptr<void>
  std::unordered_map<std::string, std::vector<std::shared_ptr<void>>> slots;

  template <typename T>
  const T* GetCAbi(size_t index) const {
    if (!items || index >= count) return nullptr;
    return static_cast<const T*>(items[index]);
  }

  template <typename T>
  const T* GetSlot(const std::string& slot_name, size_t index) const {
    auto it = slots.find(slot_name);
    if (it == slots.end() || index >= it->second.size()) return nullptr;
    return static_cast<const T*>(it->second[index].get());
  }

  template <typename T>
  const T* At(size_t index, const std::string& slot_name = "") const {
    if (!slot_name.empty()) {
      auto it = slots.find(slot_name);
      if (it != slots.end()) {
        if (index >= it->second.size()) return nullptr;
        return static_cast<const T*>(it->second[index].get());
      }
    }
    return GetCAbi<T>(index);
  }
};

/**
 * @brief 外部宿主输出批次目标借用视图
 */
class ExternalOutputBatchView {
 public:
  // C ABI 输出指针数组
  void** items = nullptr;
  size_t count = 0;
  size_t capacity = 0;
  std::string type_id;

  // Operator 已租用输出块: slot_name -> vector of void*
  std::unordered_map<std::string, std::vector<void*>> leased_slots;
  // Operator 槽位字段容量: slot_name -> field_name -> capacity
  std::unordered_map<std::string, std::unordered_map<std::string, size_t>>
      slot_capacities;
  // Operator 槽位池 Spec: slot_name -> ResolvedOutputPoolSpec
  std::unordered_map<std::string, ResolvedOutputPoolSpec> pool_specs;

  const ResolvedOutputPoolSpec* GetPoolSpec(
      const std::string& slot_name) const {
    auto sit = pool_specs.find(slot_name);
    if (sit != pool_specs.end()) return &sit->second;
    for (const auto& kv : pool_specs) {
      auto dot = kv.first.rfind('.');
      if (dot != std::string::npos && kv.first.substr(dot + 1) == slot_name) {
        return &kv.second;
      }
    }
    return nullptr;
  }

  template <typename T>
  T* GetCAbi(size_t index) const {
    size_t limit = capacity > 0 ? capacity : count;
    if (!items || index >= limit) return nullptr;
    return static_cast<T*>(items[index]);
  }

  template <typename T>
  T* GetSlot(const std::string& slot_name, size_t index) const {
    auto it = leased_slots.find(slot_name);
    if (it != leased_slots.end()) {
      if (index >= it->second.size()) return nullptr;
      return static_cast<T*>(it->second[index]);
    }
    for (const auto& kv : leased_slots) {
      auto dot = kv.first.rfind('.');
      if (dot != std::string::npos && kv.first.substr(dot + 1) == slot_name) {
        if (index >= kv.second.size()) return nullptr;
        return static_cast<T*>(kv.second[index]);
      }
    }
    return nullptr;
  }

  size_t GetSlotCapacity(const std::string& slot_name,
                         const std::string& field_name,
                         size_t default_cap = 0) const {
    auto sit = slot_capacities.find(slot_name);
    if (sit != slot_capacities.end()) {
      auto fit = sit->second.find(field_name);
      if (fit != sit->second.end()) return fit->second;
    }
    for (const auto& kv : slot_capacities) {
      auto dot = kv.first.rfind('.');
      if (dot != std::string::npos && kv.first.substr(dot + 1) == slot_name) {
        auto fit = kv.second.find(field_name);
        if (fit != kv.second.end()) return fit->second;
      }
    }
    return default_cap;
  }
};

/**
 * @brief 输入解码选项与调用诊断上下文
 */
struct InputDecodeOptions {
  std::string binding_id;
  std::string converter_id;
  std::string transport;  // "cabi" 或 "operator"
  size_t max_batch_size = 64;
};

/**
 * @brief 输出编码选项与调用诊断上下文
 */
struct OutputEncodeOptions {
  std::string binding_id;
  std::string converter_id;
  std::string transport;  // "cabi" 或 "operator"
  size_t max_batch_size = 64;
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

  template <typename T>
  BlackboardKey<T> Key(const std::string& logical_name) const {
    auto it = mapping_.find(logical_name);
    if (it != mapping_.end()) {
      return BlackboardKey<T>{it->second.c_str(),
                              BlackboardTypeTraits<T>::TypeName()};
    }
    return BlackboardKey<T>{logical_name.c_str(),
                            BlackboardTypeTraits<T>::TypeName()};
  }

  const std::string& GetActualKey(const std::string& logical_name) const {
    auto it = mapping_.find(logical_name);
    if (it != mapping_.end()) {
      return it->second;
    }
    return logical_name;
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

  template <typename T>
  BlackboardKey<T> Key(const std::string& logical_name) const {
    auto it = mapping_.find(logical_name);
    if (it != mapping_.end()) {
      return BlackboardKey<T>{it->second.c_str(),
                              BlackboardTypeTraits<T>::TypeName()};
    }
    return BlackboardKey<T>{logical_name.c_str(),
                            BlackboardTypeTraits<T>::TypeName()};
  }

  const std::string& GetActualKey(const std::string& logical_name) const {
    auto it = mapping_.find(logical_name);
    if (it != mapping_.end()) {
      return it->second;
    }
    return logical_name;
  }

  const std::unordered_map<std::string, std::string>& All() const {
    return mapping_;
  }

 private:
  std::unordered_map<std::string, std::string> mapping_;
};

/**
 * @brief 外部槽位定义 (C ABI 结构体或 Operator 槽位)
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
  std::string transport;  // "cabi" 或 "operator"
  std::string schema_id;
  int schema_version = 1;
  std::string external_type;
  std::vector<ExternalSlotDefinition> external_slots;
  std::vector<NodePortDefinition> logical_ports;  // 发布的内部逻辑输出端口
  size_t max_batch_size = 64;
  std::string ownership_policy = "copy_in";
  std::string thread_model = "stateless";
  DecodeInputFn decode_fn = nullptr;
};

/**
 * @brief 输出转换器 Definition
 */
struct OutputConverterDefinition {
  std::string converter_id;
  std::string transport;  // "cabi" 或 "operator"
  std::string schema_id;
  int schema_version = 1;
  std::string external_type;
  std::vector<NodePortDefinition> logical_ports;  // 消费的内部逻辑输入端口
  std::vector<ExternalSlotDefinition> external_slots;
  std::string cardinality = "1:1";
  size_t max_batch_size = 64;
  std::string capacity_policy = "reject_overflow";
  std::string thread_model = "stateless";
  EncodeOutputFn encode_fn = nullptr;
};

}  // namespace llm_edgeflow
