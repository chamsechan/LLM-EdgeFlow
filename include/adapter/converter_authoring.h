#pragma once

#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "adapter/adapter_validation_helper.h"
#include "adapter/io_binding.h"
#include "adapter/io_binding_registry.h"
#include "adapter/io_converter.h"
#include "adapter/io_converter_registry.h"
#include "adapter/result_validation.h"
#include "contracts/diagnostic.h"
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

// Operator also checks carriers and the intersection of deployment batch
// limits.
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

inline int CopyToOperatorString(std::string_view src, CompanyString* dest,
                                uint32_t capacity, const char* field_name,
                                std::string* err) noexcept {
  try {
    if (!dest || !dest->data) {
      if (err)
        *err = std::string(field_name ? field_name : "string") +
               " in destination pool block is null";
      return -4;
    }
    const size_t len = src.size();
    if (len > capacity ||
        len > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
      if (err)
        *err = std::string(field_name ? field_name : "string") +
               " output length (" + std::to_string(len) +
               ") exceeds pool capacity (" + std::to_string(capacity) + ")";
      return -4;
    }
    if (len != 0) std::memcpy(dest->data, src.data(), len);
    dest->data[len] = '\0';
    dest->length = static_cast<int32_t>(len);
    return 0;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(err, e.what());
    return -4;
  } catch (...) {
    SetDiagnosticNoexcept(err, "Unknown exception in CopyToOperatorString");
    return -4;
  }
}

// The view must describe the actual leased storage. Never infer capacity from
// CompanyString.length (content length) or a converter-local fallback.
inline bool WriteOutputString(const ExternalOutputBatchView& view,
                              const char* slot, CompanyString* destination,
                              const char* field, std::string_view value,
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

// Add runtime location to a business error without making the business function
// depend on converter IDs, batch indices, or port bindings.
inline int ReturnRowStatus(const AdapterStatus& result, const std::string& id,
                           size_t index, AdapterStatus* status) {
  if (status) {
    *status = AdapterStatus(result.Code(), result.Message(), result.FieldPath(),
                            static_cast<int>(index), id);
  }
  return result.Code();
}

// A synchronous borrowed writer. Do not retain it or any destination pointers.
class OutputStringWriter {
 public:
  OutputStringWriter(const ExternalOutputBatchView& view, const char* slot,
                     const OutputEncodeOptions& options, size_t index)
      : view_(view), slot_(slot), options_(options), index_(index) {}

  AdapterStatus Write(CompanyString* destination, const char* field,
                      std::string_view value) const {
    AdapterStatus status;
    WriteOutputString(view_, slot_, destination, field, value, options_,
                      &status, index_);
    return status;
  }

 private:
  const ExternalOutputBatchView& view_;
  const char* slot_;
  const OutputEncodeOptions& options_;
  size_t index_;
};

// One required host slot -> one owned payload per request. Callback validates
// and copies borrowed host fields; publication starts only after every row
// passes.
template <typename Host, typename Payload, typename Decode>
int DecodeRequestRows(
    const ExternalInputBatchView& source, const InputDecodeOptions& options,
    const InputPortBindings& bindings, AlgContext* context,
    AdapterStatus* status, size_t max_batch_size, const char* slot,
    const BlackboardKey<std::vector<uint64_t>>& raw_ids_port,
    const BlackboardKey<std::vector<TraceableItem<Payload>>>& payload_port,
    Decode&& decode) {
  if (!ValidateDecodeRequest(source, options, context, max_batch_size, status))
    return COMPANY_ALG_ERR_INVALID_INPUT;
  std::vector<uint64_t> ids;
  std::vector<TraceableItem<Payload>> payloads;
  ids.reserve(source.count);
  payloads.reserve(source.count);
  for (size_t i = 0; i < source.count; ++i) {
    const auto* input = ReadInputSlot<Host>(source, slot, i, options, status);
    if (!input) return COMPANY_ALG_ERR_INVALID_INPUT;
    Payload payload{};
    const auto result = decode(*input, &payload);
    if (!result.IsOk())
      return ReturnRowStatus(result, options.converter_id, i, status);
    ids.push_back(input->request_id);
    payloads.emplace_back(static_cast<uint32_t>(i), 0, std::move(payload));
  }
  if (!AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key(raw_ids_port), std::move(ids),
          options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key(payload_port), std::move(payloads),
          options.converter_id.c_str(), status))
    return COMPANY_ALG_ERR_INVALID_INPUT;
  return COMPANY_ALG_SUCCESS;
}

// Exactly one result (sub_id == 0) per request, in any internal order. The
// framework restores external IDs; callback owns business fields/serialization.
template <typename Host, typename Payload, typename Encode>
int EncodeResultRows(
    AlgContext* context, const OutputPortBindings& bindings,
    const OutputEncodeOptions& options, ExternalOutputBatchView* destination,
    size_t* written_count, AdapterStatus* status, const char* slot,
    const BlackboardKey<std::vector<uint64_t>>& raw_ids_port,
    const BlackboardKey<std::vector<TraceableItem<Payload>>>& result_port,
    Encode&& encode) {
  if (written_count) *written_count = 0;
  if (!context)
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Encode", "context",
        options.converter_id.c_str());
  const auto* results =
      ReadOutputValue(*context, bindings, result_port, options, status, "res");
  if (!results) return COMPANY_ALG_ERR_INVALID_INPUT;
  const auto* ids =
      ReadOutputValue(*context, bindings, raw_ids_port, options, status);
  if (!ids) return COMPANY_ALG_ERR_INVALID_INPUT;
  if (!destination || destination->count < results->size())
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Destination item count is less than output count",
        "destination", options.converter_id.c_str());
  std::vector<const TraceableItem<Payload>*> ordered;
  if (!IndexResults(results, ids, &ordered, "res", options.converter_id.c_str(),
                    status))
    return COMPANY_ALG_ERR_INVALID_INPUT;
  for (size_t i = 0; i < ordered.size(); ++i) {
    auto* output = destination->GetSlot<Host>(slot, i);
    if (!output)
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status, std::string("Missing ") + slot + " slot block in output view",
          slot, options.converter_id.c_str(), static_cast<int>(i));
    output->request_id = (*ids)[i];
    const auto result =
        encode(ordered[i]->data, output,
               OutputStringWriter(*destination, slot, options, i));
    if (!result.IsOk())
      return ReturnRowStatus(result, options.converter_id, i, status);
  }
  if (written_count) *written_count = ordered.size();
  return COMPANY_ALG_SUCCESS;
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
