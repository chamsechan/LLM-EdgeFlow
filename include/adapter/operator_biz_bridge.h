#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "adapter/operator_io_contracts.h"
#include "edgeflow/c_api.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {

/**
 * @brief Process 执行期局部影子 DTO 存储器
 * (保证执行期间指针生命周期与地址绝对稳定)
 */
struct ProcessLocalShadowStorage {
  std::deque<std::string> strings;
  std::deque<std::vector<float>> float_vectors;
  std::vector<std::shared_ptr<void>> shadow_dtos;

  const char* StoreString(const CompanyString* cs) {
    if (!cs || cs->length <= 0 || !cs->data) {
      strings.emplace_back("");
      return strings.back().c_str();
    }
    strings.emplace_back(cs->data, cs->length);
    return strings.back().c_str();
  }

  const char* StoreOptionalString(const CompanyString* cs) {
    return cs ? StoreString(cs) : nullptr;
  }

  template <typename T>
  T* AllocateShadowDto() {
    auto dto = std::make_shared<T>();
    T* raw = dto.get();
    shadow_dtos.push_back(std::move(dto));
    return raw;
  }
};

using ConvertSampleOutputFn = int (*)(const void* internal_dto,
                                      void* external_output_struct,
                                      const ResolvedOutputPoolSpec& spec,
                                      std::string* err);

/**
 * @brief 业务逻辑槽位定义
 */
struct OperatorBizSlot {
  std::string logical_name;  // 业务逻辑槽位名 (业务与方向内唯一)
  std::string type_suffix;   // 规范类型后缀
  IoDirection direction = IoDirection::kInput;
  bool required = true;
  // Output key suffix in the returned NamedIoBatch map.
  std::string key_suffix{};
  ConvertSampleOutputFn convert_output = nullptr;

  const std::string& KeySuffix() const { return key_suffix; }

  bool operator==(const OperatorBizSlot& other) const {
    return logical_name == other.logical_name &&
           type_suffix == other.type_suffix && direction == other.direction &&
           required == other.required && key_suffix == other.key_suffix &&
           convert_output == other.convert_output;
  }
};

using ConvertSampleInputFn = int (*)(
    const std::unordered_map<std::string, const void*>& slots_by_logical_name,
    ProcessLocalShadowStorage& storage, const void** out_internal_dto,
    std::string* err);

using CreateShadowOutputDtoFn = void* (*)(ProcessLocalShadowStorage& storage);

/**
 * @brief 业务桥接描述符
 */
struct OperatorBizBridgeDescriptor {
  CompanyAlgBizType biz_type = ALG_BIZ_TYPE_UNKNOWN;
  std::string adapter_name;
  std::string internal_input_type_name;
  std::string internal_output_type_name;
  std::string registration_identity;
  std::vector<OperatorBizSlot> input_slots;
  std::vector<OperatorBizSlot> output_slots;
  ConvertSampleInputFn convert_sample_input = nullptr;
  CreateShadowOutputDtoFn create_shadow_output_dto = nullptr;

  bool operator==(const OperatorBizBridgeDescriptor& other) const {
    return biz_type == other.biz_type && adapter_name == other.adapter_name &&
           internal_input_type_name == other.internal_input_type_name &&
           internal_output_type_name == other.internal_output_type_name &&
           registration_identity == other.registration_identity &&
           input_slots == other.input_slots &&
           output_slots == other.output_slots &&
           convert_sample_input == other.convert_sample_input &&
           create_shadow_output_dto == other.create_shadow_output_dto;
  }
};

// The built-in one-input/one-output pattern needs only its conversions and
// registered type names; slot boilerplate and result allocation are shared.
template <typename Result>
OperatorBizBridgeDescriptor MakeSingleSlotBizBridge(
    CompanyAlgBizType biz_type, std::string adapter_name,
    std::string input_type, std::string identity, std::string input_slot,
    std::string output_slot, std::string output_key_suffix = "") {
  OperatorBizBridgeDescriptor desc;
  desc.biz_type = biz_type;
  desc.adapter_name = std::move(adapter_name);
  desc.internal_input_type_name = std::move(input_type);
  desc.internal_output_type_name = Result::kTypeName;
  desc.registration_identity = std::move(identity);
  std::string actual_key_suffix =
      output_key_suffix.empty() ? output_slot : std::move(output_key_suffix);
  desc.input_slots.push_back(
      {input_slot, input_slot, IoDirection::kInput, true, "", nullptr});
  desc.output_slots.push_back({output_slot, output_slot, IoDirection::kOutput,
                               true, std::move(actual_key_suffix), nullptr});
  desc.create_shadow_output_dto =
      [](ProcessLocalShadowStorage& storage) -> void* {
    return storage.AllocateShadowDto<Result>();
  };
  return desc;
}

template <typename HostType>
struct OperatorSlotTraits;

template <>
struct OperatorSlotTraits<CompanyOperatorEntityInput> {
  inline static constexpr const char* kTypeSuffix = "entity_in";
  inline static constexpr IoDirection kDirection = IoDirection::kInput;
};

template <>
struct OperatorSlotTraits<CompanyOperatorEntityOutput> {
  inline static constexpr const char* kTypeSuffix = "entity_out";
  inline static constexpr IoDirection kDirection = IoDirection::kOutput;
};

template <typename InternalInput, typename InternalOutput, typename HostInput,
          typename HostOutput,
          int (*ConvertInput)(const HostInput&, ProcessLocalShadowStorage&,
                              const InternalInput**, std::string*),
          int (*ConvertOutput)(const InternalOutput&, HostOutput&,
                               const ResolvedOutputPoolSpec&, std::string*)>
inline OperatorBizBridgeDescriptor MakeTypedSingleSlotBizBridge(
    CompanyAlgBizType biz_type, std::string adapter_name,
    std::string internal_input_type_name, std::string identity,
    std::string input_slot = OperatorSlotTraits<HostInput>::kTypeSuffix,
    std::string output_slot = OperatorSlotTraits<HostOutput>::kTypeSuffix,
    std::string output_key_suffix = "") {
  auto desc = MakeSingleSlotBizBridge<InternalOutput>(
      biz_type, std::move(adapter_name), std::move(internal_input_type_name),
      std::move(identity), input_slot, output_slot);

  desc.input_slots.front().type_suffix =
      OperatorSlotTraits<HostInput>::kTypeSuffix;
  desc.output_slots.front().type_suffix =
      OperatorSlotTraits<HostOutput>::kTypeSuffix;
  desc.output_slots.front().key_suffix =
      output_key_suffix.empty() ? OperatorSlotTraits<HostOutput>::kTypeSuffix
                                : std::move(output_key_suffix);

  desc.convert_sample_input =
      [](const std::unordered_map<std::string, const void*>& slots,
         ProcessLocalShadowStorage& storage, const void** out_internal_dto,
         std::string* err) -> int {
    const void* payload = nullptr;
    auto it = slots.find(OperatorSlotTraits<HostInput>::kTypeSuffix);
    if (it != slots.end() && it->second) {
      payload = it->second;
    } else if (slots.size() == 1 && slots.begin()->second) {
      payload = slots.begin()->second;
    }
    if (!payload) {
      if (err) {
        *err = std::string("Missing required input slot ") +
               OperatorSlotTraits<HostInput>::kTypeSuffix;
      }
      return -3;
    }
    const auto* in = static_cast<const HostInput*>(payload);
    const InternalInput* typed_dto = nullptr;
    const int ret = ConvertInput(*in, storage, &typed_dto, err);
    if (ret == 0 && out_internal_dto) {
      *out_internal_dto = typed_dto;
    }
    return ret;
  };

  desc.output_slots.front().convert_output =
      [](const void* internal_dto, void* external_output_struct,
         const ResolvedOutputPoolSpec& spec, std::string* err) -> int {
    if (!internal_dto || !external_output_struct) {
      if (err) *err = "Null internal DTO or external output struct pointer";
      return -4;
    }
    const auto* in_dto = static_cast<const InternalOutput*>(internal_dto);
    auto* out = static_cast<HostOutput*>(external_output_struct);
    return ConvertOutput(*in_dto, *out, spec, err);
  };

  return desc;
}

template <typename InternalInput, typename InternalOutput, typename HostInput,
          typename HostOutput>
inline OperatorBizBridgeDescriptor MakeTypedSingleSlotBizBridge(
    CompanyAlgBizType biz_type, std::string adapter_name,
    std::string internal_input_type_name, std::string identity,
    std::string input_slot, std::string output_slot,
    ConvertSampleInputFn convert_input, ConvertSampleOutputFn convert_output,
    std::string output_key_suffix = "") {
  auto desc = MakeSingleSlotBizBridge<InternalOutput>(
      biz_type, std::move(adapter_name), std::move(internal_input_type_name),
      std::move(identity), input_slot, output_slot);

  desc.input_slots.front().type_suffix =
      OperatorSlotTraits<HostInput>::kTypeSuffix;
  desc.output_slots.front().type_suffix =
      OperatorSlotTraits<HostOutput>::kTypeSuffix;
  desc.output_slots.front().key_suffix =
      output_key_suffix.empty() ? OperatorSlotTraits<HostOutput>::kTypeSuffix
                                : std::move(output_key_suffix);
  desc.convert_sample_input = convert_input;
  desc.output_slots.front().convert_output = convert_output;
  return desc;
}

// Source-extension registration and output copy helpers. Registry state and
// output pool management remain private to the Integration implementation.
bool RegisterOperatorBizBridge(OperatorBizBridgeDescriptor descriptor);
int CopyToOperatorString(const char* source, CompanyString* destination,
                         uint32_t capacity, const char* field_name,
                         std::string* diagnostic) noexcept;

/**
 * @brief 就地业务自注册宏 (无需在中心维护列表)
 */
#define REGISTER_OPERATOR_BIZ_BRIDGE(BridgeRegisterFn)                       \
  namespace {                                                                \
  struct AutoRegister_##BridgeRegisterFn {                                   \
    AutoRegister_##BridgeRegisterFn() { BridgeRegisterFn(); }                \
  };                                                                         \
  static AutoRegister_##BridgeRegisterFn g_auto_register_##BridgeRegisterFn; \
  }

}  // namespace llm_edgeflow
