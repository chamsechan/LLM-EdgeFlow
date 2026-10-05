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

// 两侧必须是同一 C++ 值类型；恒等映射需显式声明。
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

// 常用约定：必填槽位，type_suffix = slot_name，key_suffix 为空时回退到
// type_suffix。其他后缀或可选槽位请使用 ExternalSlotDefinition 的显式字段。
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

// 省略容量字段时由已注册的 ValueType 推导；
// 显式列表必须与该 ValueType 一致。
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

// Operator 在解码前已检查载体和有效批大小上限，并通过 options 传入；
// 此处防御性地再检查一次。
inline bool ValidateDecodeRequest(const ExternalInputBatchView& source,
                                  const InputDecodeOptions& options,
                                  AlgContext* context, AdapterStatus* status) {
  if (!context) {
    AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Decode", "context",
        options.converter_id.c_str());
    return false;
  }
  if (source.count == 0) {
    AdapterValidationHelper::ReturnInvalidInput(
        status, "Batch must contain at least one request", "slots",
        options.converter_id.c_str());
    return false;
  }
  if (options.max_batch_size != 0 && source.count > options.max_batch_size) {
    AdapterValidationHelper::ReturnInvalidInput(
        status,
        "Batch size out of range [1, " +
            std::to_string(options.max_batch_size) + "]",
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

// 仅做结构安全检查；大小上限和可选字段语义留在调用处，
// 以保持现有诊断信息和校验顺序不变。
inline bool IsValidInputString(const CompanyString* value) {
  return value && value->length >= 0 && (value->length == 0 || value->data);
}

// 须在校验后调用。空字符串的 data 指针可以为空。
inline std::string CopyInputString(const CompanyString& value) {
  return value.length == 0 ? std::string{}
                           : std::string(value.data, value.length);
}

// 所有行校验通过后、发布业务值前调用。
inline bool PublishRequestIds(const InputDecodeOptions& options,
                              std::vector<uint64_t> ids,
                              AdapterStatus* status) {
  if (!options.request_ids) {
    AdapterValidationHelper::ReturnInvalidInput(
        status, "Missing request id table in decode options", "request_ids",
        options.converter_id.c_str());
    return false;
  }
  *options.request_ids = std::move(ids);
  return true;
}

inline const std::vector<uint64_t>* RequestIds(
    const OutputEncodeOptions& options, AdapterStatus* status) {
  if (!options.request_ids) {
    AdapterValidationHelper::ReturnInvalidInput(
        status, "Missing request id table in encode options", "request_ids",
        options.converter_id.c_str());
  }
  return options.request_ids;
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

// view 必须描述实际租用的存储。不得从 CompanyString.length (内容长度)
// 或 Converter 本地的回退值推断容量。
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

// 为业务错误补充运行时位置，使业务函数无需依赖 Converter ID、批内索引或
// 端口绑定。
inline int ReturnRowStatus(const AdapterStatus& result, const std::string& id,
                           size_t index, AdapterStatus* status) {
  if (status) {
    *status = AdapterStatus(result.Code(), result.Message(), result.FieldPath(),
                            static_cast<int>(index), id);
  }
  return result.Code();
}

// 同步借用的写入器，不得保留它或任何目标指针。
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

// 每个请求：一个必填宿主槽位 -> 一个自有 payload。回调负责校验并复制借用的
// 宿主字段；所有行校验通过后才开始发布。
template <typename Host, typename Payload, typename Decode>
int DecodeRequestRows(
    const ExternalInputBatchView& source, const InputDecodeOptions& options,
    const InputPortBindings& bindings, AlgContext* context,
    AdapterStatus* status, const char* slot,
    const BlackboardKey<std::vector<TraceableItem<Payload>>>& payload_port,
    Decode&& decode) {
  if (!ValidateDecodeRequest(source, options, context, status))
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
  if (!PublishRequestIds(options, std::move(ids), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key(payload_port), std::move(payloads),
          options.converter_id.c_str(), status))
    return COMPANY_ALG_ERR_INVALID_INPUT;
  return COMPANY_ALG_SUCCESS;
}

// 每个请求恰好一个结果 (sub_id == 0)，内部顺序不限。框架负责恢复外部 ID，
// 回调负责业务字段与序列化。
template <typename Host, typename Payload, typename Encode>
int EncodeResultRows(
    AlgContext* context, const OutputPortBindings& bindings,
    const OutputEncodeOptions& options, ExternalOutputBatchView* destination,
    size_t* written_count, AdapterStatus* status, const char* slot,
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
  const auto* ids = RequestIds(options, status);
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

}  // namespace llm_edgeflow
