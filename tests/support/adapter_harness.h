#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/io_converter.h"
#include "contracts/traceable_item.h"
#include "core/alg_context.h"
#include "core/blackboard_key.h"

namespace llm_edgeflow {
namespace test {

/**
 * @brief 测试专用 Adapter / Converter 契约夹具
 *
 * 集中管理 AlgContext、句柄与输入输出视图生命周期，
 * 提供来源扰动生成器和统一的 C ABI 解码/编码调用通道。
 */
class AdapterHarness {
 public:
  AdapterHarness(const InputConverterDefinition* input_conv,
                 const OutputConverterDefinition* output_conv,
                 InputPortBindings in_bindings = {},
                 OutputPortBindings out_bindings = {})
      : in_conv_(input_conv),
        out_conv_(output_conv),
        in_bindings_(std::move(in_bindings)),
        out_bindings_(std::move(out_bindings)) {}

  explicit AdapterHarness(const InputConverterDefinition* input_conv,
                          InputPortBindings in_bindings = {})
      : in_conv_(input_conv),
        out_conv_(nullptr),
        in_bindings_(std::move(in_bindings)) {}

  explicit AdapterHarness(const OutputConverterDefinition* output_conv,
                          OutputPortBindings out_bindings = {})
      : in_conv_(nullptr),
        out_conv_(output_conv),
        out_bindings_(std::move(out_bindings)) {}

  AlgContext& Context() { return ctx_; }
  const AlgContext& Context() const { return ctx_; }
  AdapterStatus& Status() { return status_; }
  const AdapterStatus& Status() const { return status_; }

  int DecodeCAbi(const std::vector<const void*>& inputs) {
    if (!in_conv_ || !in_conv_->decode_fn) return -1;
    ExternalInputBatchView view;
    view.items =
        inputs.empty() ? nullptr : const_cast<const void**>(inputs.data());
    view.count = inputs.size();
    view.type_id = in_conv_->external_type;
    InputDecodeOptions options;
    options.converter_id = in_conv_->converter_id;
    options.transport = in_conv_->transport;
    return in_conv_->decode_fn(view, options, in_bindings_, &ctx_, &status_);
  }

  template <typename COutput>
  int EncodeCAbi(std::vector<COutput>* outputs) {
    if (!out_conv_ || !out_conv_->encode_fn || !outputs) return -1;
    std::vector<void*> output_ptrs(outputs->size());
    for (size_t i = 0; i < outputs->size(); ++i) {
      output_ptrs[i] = &(*outputs)[i];
    }
    ExternalOutputBatchView view;
    view.items = outputs->empty() ? nullptr : output_ptrs.data();
    view.count = outputs->size();
    view.capacity = outputs->size();
    view.type_id = out_conv_->external_type;
    OutputEncodeOptions options;
    options.converter_id = out_conv_->converter_id;
    options.transport = out_conv_->transport;
    size_t written = 0;
    int ret = out_conv_->encode_fn(&ctx_, out_bindings_, options, &view,
                                   &written, &status_);
    if (ret == 0 && written <= outputs->size()) {
      outputs->resize(written);
    }
    return ret;
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
  InputPortBindings in_bindings_;
  OutputPortBindings out_bindings_;
  AlgContext ctx_;
  AdapterStatus status_;
};

}  // namespace test
}  // namespace llm_edgeflow
