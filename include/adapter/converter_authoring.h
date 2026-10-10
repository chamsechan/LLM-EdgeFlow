#pragma once

#include <string>
#include <utility>

#include "adapter/adapter_validation_helper.h"
#include "adapter/io_converter.h"
#include "adapter/io_converter_registry.h"
#include "adapter/io_values.h"
#include "adapter/result_validation.h"

namespace llm_edgeflow {

template <typename T>
inline ExternalSlotDefinition ExternalInputSlot(std::string type) {
  ExternalSlotDefinition slot;
  slot.value_type = typeid(T);
  slot.type_suffix = std::move(type);
  return slot;
}

template <typename T>
inline ExternalSlotDefinition ExternalOutputSlot(std::string type) {
  return ExternalInputSlot<T>(std::move(type));
}

template <typename P>
inline FieldBuilder<P, int64_t> MaxBytes(std::string field,
                                         int64_t P::*member) {
  return Field(std::move(field) + "_max_bytes", member).Minimum(1);
}

inline bool ValidateDecodeRequest(const ExternalInputBatchView& source,
                                  const InputDecodeOptions& options,
                                  AlgContext* context, AdapterStatus* status) {
  if (!context) {
    AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Decode", "context",
        options.Label().c_str());
    return false;
  }
  if (source.count == 0) {
    AdapterValidationHelper::ReturnInvalidInput(
        status, "Batch must contain at least one request", "slots",
        options.Label().c_str());
    return false;
  }
  return true;
}

template <typename T>
inline std::optional<T> ReadInputSlot(const ExternalInputBatchView& source,
                                      const char* slot, size_t index,
                                      const InputDecodeOptions& options,
                                      AdapterStatus* status) {
  auto value = source.Read<T>(slot, index, status);
  if (!value && status) {
    *status =
        AdapterStatus(status->Code(), status->Message(), status->FieldPath(),
                      static_cast<int>(index), options.Label());
  }
  return value;
}

inline const std::vector<uint64_t>* RequestIds(
    const OutputEncodeOptions& options, AdapterStatus* status) {
  if (!options.request_ids) {
    AdapterValidationHelper::ReturnInvalidInput(
        status, "Missing request id table in encode options", "request_ids",
        options.Label().c_str());
  }
  return options.request_ids;
}

template <typename T>
inline const T* ReadOutputValue(AlgContext& context,
                                const BlackboardKey<T>& port,
                                const OutputEncodeOptions& options,
                                AdapterStatus* status,
                                const char* field_path = nullptr) {
  const auto* value = context.Read<T>(options.Port(port.name));
  if (!value) {
    AdapterValidationHelper::ReturnInvalidInput(
        status, std::string("Missing required context value: ") + port.name,
        field_path ? field_path : port.name, options.Label().c_str());
  }
  return value;
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

template <typename Value>
bool WriteOutputValue(const ExternalOutputBatchView& destination,
                      const char* slot, size_t index, const Value& value,
                      const OutputEncodeOptions& options,
                      AdapterStatus* status) {
  const auto* ids = RequestIds(options, status);
  if (!ids) return false;
  if (index >= ids->size()) {
    AdapterValidationHelper::ReturnInvalidInput(
        status, "Output row exceeds request id table", "request_ids",
        options.Label().c_str(), static_cast<int>(index));
    return false;
  }
  const auto result = destination.Write(slot, index, (*ids)[index], value);
  if (!result.IsOk()) {
    ReturnRowStatus(result, options.Label(), index, status);
    return false;
  }
  return true;
}

// 每个请求：binding 读取一个中性值，回调完成业务校验与 payload 转换。
// 所有行通过后才发布请求自有数据。
template <typename Value, typename Payload, typename Decode>
int DecodeRequestRows(
    const ExternalInputBatchView& source, const InputDecodeOptions& options,
    AlgContext* context, AdapterStatus* status, const char* slot,
    const BlackboardKey<std::vector<TraceableItem<Payload>>>& payload_port,
    Decode&& decode) {
  if (!ValidateDecodeRequest(source, options, context, status))
    return COMPANY_ALG_ERR_INVALID_INPUT;
  std::vector<TraceableItem<Payload>> payloads;
  payloads.reserve(source.count);
  for (size_t i = 0; i < source.count; ++i) {
    auto input = ReadInputSlot<Value>(source, slot, i, options, status);
    if (!input) return COMPANY_ALG_ERR_INVALID_INPUT;
    Payload payload{};
    const auto result = decode(*input, &payload);
    if (!result.IsOk())
      return ReturnRowStatus(result, options.Label(), i, status);
    payloads.emplace_back(static_cast<uint32_t>(i), 0, std::move(payload));
  }
  if (!AdapterValidationHelper::PublishContextValue(
          *context, options.Port(payload_port.name), std::move(payloads),
          options.Label().c_str(), status))
    return COMPANY_ALG_ERR_INVALID_INPUT;
  return COMPANY_ALG_SUCCESS;
}

// 每个请求恰好一个结果 (sub_id == 0)，内部顺序不限。框架负责恢复外部 ID，
// 回调负责业务字段与序列化。
template <typename Value, typename Payload, typename Encode>
int EncodeResultRows(
    AlgContext* context, const OutputEncodeOptions& options,
    ExternalOutputBatchView* destination, size_t* written_count,
    AdapterStatus* status, const char* slot,
    const BlackboardKey<std::vector<TraceableItem<Payload>>>& result_port,
    Encode&& encode) {
  if (written_count) *written_count = 0;
  if (!context)
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Encode", "context",
        options.Label().c_str());
  const auto* results =
      ReadOutputValue(*context, result_port, options, status, "res");
  if (!results) return COMPANY_ALG_ERR_INVALID_INPUT;
  const auto* ids = RequestIds(options, status);
  if (!ids) return COMPANY_ALG_ERR_INVALID_INPUT;
  if (!destination || destination->count < results->size())
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Destination item count is less than output count",
        "destination", options.Label().c_str());
  std::vector<const TraceableItem<Payload>*> ordered;
  if (!IndexResults(results, ids, &ordered, "res", options.Label().c_str(),
                    status))
    return COMPANY_ALG_ERR_INVALID_INPUT;
  size_t written = 0;
  for (size_t i = 0; i < ordered.size(); ++i) {
    if (!destination->HasSlot(slot, i) && !destination->required) continue;
    Value output{};
    const auto result = encode(ordered[i]->data, &output);
    if (!result.IsOk())
      return ReturnRowStatus(result, options.Label(), i, status);
    if (!WriteOutputValue(*destination, slot, i, output, options, status))
      return status ? status->Code() : COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    ++written;
  }
  if (written_count) *written_count = written;
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

}  // namespace llm_edgeflow
