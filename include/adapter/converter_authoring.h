#pragma once

#include <cstring>
#include <string>
#include <utility>

#include "adapter/adapter_validation_helper.h"
#include "adapter/io_binding.h"
#include "adapter/io_binding_registry.h"
#include "adapter/io_converter.h"
#include "adapter/io_converter_registry.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {

// Both sides must carry the same C++ value type. Identity mapping is explicit.
template <typename T>
inline std::pair<std::string, std::string> BindIoPort(
    const BlackboardKey<T>& logical_port, const BlackboardKey<T>& actual_key) {
  return {logical_port.name, actual_key.name};
}

template <typename T>
inline std::pair<std::string, std::string> BindIoPort(
    const BlackboardKey<T>& port) {
  return BindIoPort(port, port);
}

// Common convention: required slot, type_suffix = slot_name, and an empty
// key_suffix that falls back to type_suffix. For other suffixes or optional
// slots, use ExternalSlotDefinition's explicit fields.
template <typename T>
inline ExternalSlotDefinition ExternalInputSlot(
    std::string slot_name,
    std::string value_type = ExternalTypeTraits<T>::TypeName()) {
  return {slot_name,
          ExternalTypeTraits<T>::TypeName(),
          PortDirection::kInput,
          true,
          std::move(value_type),
          slot_name};
}

template <typename T>
inline ExternalSlotDefinition ExternalOutputSlot(
    std::string slot_name, std::vector<std::string> capacity_fields = {}) {
  return {slot_name,
          ExternalTypeTraits<T>::TypeName(),
          PortDirection::kOutput,
          true,
          ExternalTypeTraits<T>::TypeName(),
          slot_name,
          std::move(capacity_fields)};
}

// Direct callbacks share only these checks with each other. Operator keeps its
// own carrier validation and the intersection of all deployment batch limits.
inline bool ValidateDecodeRequest(const ExternalInputBatchView& source,
                                  const InputDecodeOptions& options,
                                  AlgContext* context, size_t max_batch_size,
                                  AdapterStatus* status) {
  if (!context) {
    AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Decode", "context",
        options.converter_id.c_str());
    return false;
  }
  if (source.count == 0 || source.count > max_batch_size) {
    AdapterValidationHelper::ReturnInvalidInput(
        status,
        "Batch size out of range [1, " + std::to_string(max_batch_size) + "]",
        "slots", options.converter_id.c_str());
    return false;
  }
  return true;
}

template <typename T>
inline const T* ReadInputSlot(const ExternalInputBatchView& source,
                              const char* slot, size_t index,
                              const InputDecodeOptions& options,
                              AdapterStatus* status) {
  const auto* value = source.GetSlot<T>(slot, index);
  if (!value) {
    AdapterValidationHelper::ReturnInvalidInput(
        status,
        std::string("Missing ") + slot + " input slot or slot item is null",
        slot, options.converter_id.c_str(), static_cast<int>(index));
  }
  return value;
}

// Structural safety only; size limits and optional-field semantics stay at the
// call site so existing diagnostics and validation order remain unchanged.
inline bool IsValidInputString(const CompanyString* value) {
  return value && value->length >= 0 && (value->length == 0 || value->data);
}

// Call after validation. Empty strings may legitimately have a null data
// pointer.
inline std::string CopyInputString(const CompanyString& value) {
  return value.length == 0 ? std::string{}
                           : std::string(value.data, value.length);
}

template <typename T>
inline const T* ReadOutputValue(AlgContext& context,
                                const OutputPortBindings& bindings,
                                const BlackboardKey<T>& port,
                                const OutputEncodeOptions& options,
                                AdapterStatus* status,
                                const char* field_path = nullptr) {
  const auto* value = context.Read(bindings.Key(port));
  if (!value) {
    AdapterValidationHelper::ReturnInvalidInput(
        status, std::string("Missing required context value: ") + port.name,
        field_path ? field_path : port.name, options.converter_id.c_str());
  }
  return value;
}

inline int CopyToOperatorString(const char* src, CompanyString* dest,
                                uint32_t capacity, const char* field_name,
                                std::string* err) noexcept {
  try {
    if (!dest || !dest->data) {
      if (err)
        *err = std::string(field_name ? field_name : "string") +
               " in destination pool block is null";
      return -4;
    }
    if (!src) {
      dest->length = 0;
      dest->data[0] = '\0';
      return 0;
    }
    size_t len = std::strlen(src);
    if (len > capacity) {
      if (err)
        *err = std::string(field_name ? field_name : "string") +
               " output length (" + std::to_string(len) +
               ") exceeds pool capacity (" + std::to_string(capacity) + ")";
      return -4;
    }
    std::memcpy(dest->data, src, len);
    dest->data[len] = '\0';
    dest->length = static_cast<int32_t>(len);
    return 0;
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    return -4;
  } catch (...) {
    if (err) *err = "Unknown exception in CopyToOperatorString";
    return -4;
  }
}

// The view must describe the actual leased storage. Never infer capacity from
// CompanyString.length (content length) or a converter-local fallback.
inline bool WriteOutputString(const ExternalOutputBatchView& view,
                              const char* slot, CompanyString* destination,
                              const char* field, const char* value,
                              const OutputEncodeOptions& options,
                              AdapterStatus* status, size_t index) {
  const auto* spec = view.GetPoolSpec(slot);
  if (!spec) {
    AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing output capacity specification", field,
        options.converter_id.c_str(), static_cast<int>(index));
    return false;
  }
  const auto capacity = spec->capacities.find(field);
  if (capacity == spec->capacities.end()) {
    AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing output capacity specification", field,
        options.converter_id.c_str(), static_cast<int>(index));
    return false;
  }
  std::string error;
  if (CopyToOperatorString(value, destination, capacity->second, field,
                           &error) == COMPANY_ALG_SUCCESS) {
    return true;
  }
  AdapterValidationHelper::ReturnBufferTooSmall(status, std::move(error), field,
                                                options.converter_id.c_str(),
                                                static_cast<int>(index));
  return false;
}

#define EDGEFLOW_CONCAT_IMPL(s1, s2) s1##s2
#define EDGEFLOW_CONCAT(s1, s2) EDGEFLOW_CONCAT_IMPL(s1, s2)

#define REGISTER_INPUT_CONVERTER(def_expr)                  \
  static const bool EDGEFLOW_CONCAT(g_reg_input_converter_, \
                                    __COUNTER__) = []() {   \
    return ::llm_edgeflow::IoConverterRegistry::Instance()  \
        .RegisterInputConverter(def_expr);                  \
  }()

#define REGISTER_OUTPUT_CONVERTER(def_expr)                  \
  static const bool EDGEFLOW_CONCAT(g_reg_output_converter_, \
                                    __COUNTER__) = []() {    \
    return ::llm_edgeflow::IoConverterRegistry::Instance()   \
        .RegisterOutputConverter(def_expr);                  \
  }()

#define REGISTER_IO_BINDING(binding_expr)                                    \
  static const bool EDGEFLOW_CONCAT(g_reg_io_binding_, __COUNTER__) = []() { \
    return ::llm_edgeflow::IoBindingRegistry::Instance().RegisterBinding(    \
        binding_expr);                                                       \
  }()

#define REGISTER_BIZ_EXPOSURE(exposure_expr)                                   \
  static const bool EDGEFLOW_CONCAT(g_reg_biz_exposure_, __COUNTER__) = []() { \
    return ::llm_edgeflow::IoBindingRegistry::Instance().RegisterExposure(     \
        exposure_expr);                                                        \
  }()

}  // namespace llm_edgeflow
