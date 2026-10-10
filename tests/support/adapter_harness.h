#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/io_converter.h"
#include "contracts/traceable_item.h"
#include "core/alg_context.h"
#include "core/blackboard_key.h"
#include "tests/support/adapter_test_views.h"

namespace llm_edgeflow {
namespace test {

template <typename Definition>
std::shared_ptr<const ParameterValues> ParseConverterParametersForTest(
    const Definition& definition,
    const nlohmann::json& config = nlohmann::json::object()) {
  std::shared_ptr<const ParameterValues> values;
  std::string diagnostic;
  if (!definition.params.Parse(config, &values, &diagnostic))
    throw std::invalid_argument(definition.Label() + ": " + diagnostic);
  return values;
}

template <typename Definition>
IoPortBindings ConverterPortsForTest(const Definition& definition) {
  IoPortBindings ports;
  for (const auto& port : definition.logical_ports)
    ports.emplace(port.logical_name, port.logical_name);
  return ports;
}

template <typename Options>
class ParsedConverterOptions : public Options {
 public:
  template <typename Definition>
  explicit ParsedConverterOptions(
      const Definition& definition,
      const nlohmann::json& config = nlohmann::json::object())
      : ParsedConverterOptions(definition, config,
                               ConverterPortsForTest(definition)) {}

  template <typename Definition>
  ParsedConverterOptions(const Definition& definition,
                         const nlohmann::json& config, IoPortBindings ports)
      : values_(ParseConverterParametersForTest(definition, config)),
        ports_(std::make_shared<const IoPortBindings>(std::move(ports))) {
    this->type = definition.type;
    this->name = definition.name;
    this->params = values_.get();
    this->ports = ports_.get();
  }

 private:
  std::shared_ptr<const ParameterValues> values_;
  std::shared_ptr<const IoPortBindings> ports_;
};

using ParsedInputOptions = ParsedConverterOptions<InputDecodeOptions>;
using ParsedOutputOptions = ParsedConverterOptions<OutputEncodeOptions>;

/**
 * @brief 测试专用 Adapter / Converter 契约夹具
 *
 * 集中管理 AlgContext、句柄与输入输出视图生命周期，
 * 提供来源扰动生成器和统一的 Operator 解码/编码调用通道。
 */
class AdapterHarness {
 public:
  AdapterHarness(const InputConverterDefinition* input_conv,
                 const OutputConverterDefinition* output_conv)
      : in_conv_(input_conv),
        out_conv_(output_conv),
        input_params_(input_conv ? ParseConverterParametersForTest(*input_conv)
                                 : nullptr),
        output_params_(output_conv
                           ? ParseConverterParametersForTest(*output_conv)
                           : nullptr),
        input_ports_(input_conv ? ConverterPortsForTest(*input_conv)
                                : IoPortBindings{}),
        output_ports_(output_conv ? ConverterPortsForTest(*output_conv)
                                  : IoPortBindings{}) {}

  explicit AdapterHarness(const InputConverterDefinition* input_conv)
      : AdapterHarness(input_conv, nullptr) {}

  explicit AdapterHarness(const OutputConverterDefinition* output_conv)
      : AdapterHarness(nullptr, output_conv) {}

  AlgContext& Context() { return ctx_; }
  const AlgContext& Context() const { return ctx_; }
  AdapterStatus& Status() { return status_; }
  const AdapterStatus& Status() const { return status_; }
  void SetRequestIds(std::vector<uint64_t> ids) {
    request_ids_ = std::move(ids);
  }
  const std::vector<uint64_t>& RequestIds() const { return request_ids_; }

  int DecodeOperator(const std::vector<const void*>& inputs) {
    if (!in_conv_ || !in_conv_->decode_fn) return -1;
    ExternalInputBatchView view;
    view.count = inputs.size();
    const std::string& slot_name = in_conv_->type;
    if (!slot_name.empty()) {
      view.slot_types[slot_name] = OperatorValueTypeRegistry::Instance()
                                       .GetBindingBySuffix(in_conv_->type)
                                       ->external_c_type_name;
      for (const void* input : inputs)
        view.slots[slot_name].emplace_back(const_cast<void*>(input),
                                           [](void*) {});
    }
    return DecodeOperator(view);
  }

  int DecodeOperator(const ExternalInputBatchView& view) {
    if (!in_conv_ || !in_conv_->decode_fn) return -1;
    InputDecodeOptions options;
    options.type = in_conv_->type;
    options.name = in_conv_->name;
    options.params = input_params_.get();
    options.ports = &input_ports_;
    options.request_ids = &request_ids_;

    const auto* binding =
        OperatorValueTypeRegistry::Instance().GetBindingBySuffix(
            in_conv_->type);
    request_ids_.clear();
    if (binding && binding->read_request_id) {
      const auto it = view.slots.find(in_conv_->type);
      if (it != view.slots.end())
        for (const auto& item : it->second) {
          if (item)
            request_ids_.push_back(binding->read_request_id(item.get()));
        }
    }
    return DecodeForTest(*in_conv_, view, options, &ctx_, &status_);
  }

  template <typename COutput>
  int EncodeOperator(std::vector<COutput>* outputs,
                     const std::map<std::string, size_t>& capacities = {}) {
    if (!out_conv_ || !out_conv_->encode_fn || !outputs) return -1;
    std::vector<void*> output_ptrs(outputs->size());
    for (size_t i = 0; i < outputs->size(); ++i) {
      output_ptrs[i] = &(*outputs)[i];
    }
    ResolvedOutputPoolSpec pool_spec;
    for (const auto& [field, capacity] : capacities) {
      pool_spec.capacities.emplace(field, static_cast<uint32_t>(capacity));
    }
    ExternalOutputBatchView view;
    view.count = outputs->size();
    const std::string& slot_name = out_conv_->type;
    if (!slot_name.empty()) {
      view.slot_types[slot_name] = OperatorValueTypeRegistry::Instance()
                                       .GetBindingBySuffix(out_conv_->type)
                                       ->external_c_type_name;
      view.leased_slots[slot_name] = output_ptrs;
      view.pool_specs[slot_name] = &pool_spec;
    }
    size_t written = 0;
    int ret = EncodeOperator(&view, &written);
    if (ret == 0 && written <= outputs->size()) {
      outputs->resize(written);
    }
    return ret;
  }

  int EncodeOperator(ExternalOutputBatchView* view, size_t* written_count) {
    if (!out_conv_ || !out_conv_->encode_fn || !view) return -1;
    OutputEncodeOptions options;
    options.type = out_conv_->type;
    options.name = out_conv_->name;
    options.params = output_params_.get();
    options.ports = &output_ports_;
    options.request_ids = &request_ids_;
    return EncodeForTest(*out_conv_, &ctx_, options, view, written_count,
                         &status_);
  }

  template <typename T>
  bool Publish(const BlackboardKey<T>& key, T value) {
    return ctx_.Publish(key, std::move(value));
  }

  template <typename T>
  bool Publish(const std::string& key, T value) {
    return ctx_.Publish(key, std::move(value));
  }

  enum class ProvenanceAnomaly {
    kNone,
    kReordered,
    kOutOfRangeReqId,
    kInvalidSubId,
    kDuplicateReqId,
    kMissingReqId
  };

  template <typename ItemType>
  static std::vector<TraceableItem<ItemType>> PerturbBatch(
      const std::vector<ItemType>& values, ProvenanceAnomaly anomaly) {
    std::vector<TraceableItem<ItemType>> batch;
    uint32_t n = static_cast<uint32_t>(values.size());
    switch (anomaly) {
      case ProvenanceAnomaly::kNone:
        for (uint32_t i = 0; i < n; ++i) batch.emplace_back(i, 0, values[i]);
        break;
      case ProvenanceAnomaly::kReordered:
        for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
          batch.emplace_back(static_cast<uint32_t>(i), 0, values[i]);
        }
        break;
      case ProvenanceAnomaly::kOutOfRangeReqId:
        for (uint32_t i = 0; i < n; ++i)
          batch.emplace_back(i + n, 0, values[i]);
        break;
      case ProvenanceAnomaly::kInvalidSubId:
        for (uint32_t i = 0; i < n; ++i) batch.emplace_back(i, 1, values[i]);
        break;
      case ProvenanceAnomaly::kDuplicateReqId:
        for (uint32_t i = 0; i < n; ++i) batch.emplace_back(0, 0, values[i]);
        break;
      case ProvenanceAnomaly::kMissingReqId:
        if (n > 1) {
          for (uint32_t i = 0; i < n - 1; ++i) {
            batch.emplace_back(i, 0, values[i]);
          }
        }
        break;
    }
    return batch;
  }

 private:
  const InputConverterDefinition* in_conv_ = nullptr;
  const OutputConverterDefinition* out_conv_ = nullptr;
  std::shared_ptr<const ParameterValues> input_params_;
  std::shared_ptr<const ParameterValues> output_params_;
  IoPortBindings input_ports_;
  IoPortBindings output_ports_;
  AlgContext ctx_;
  AdapterStatus status_;
  std::vector<uint64_t> request_ids_;
};

}  // namespace test
}  // namespace llm_edgeflow
