#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/biz_adapter_interface.h"
#include "contracts/traceable_item.h"
#include "core/alg_context.h"
#include "core/blackboard_key.h"

namespace llm_edgeflow {
namespace test {

/**
 * @brief 测试专用 Adapter 契约夹具 (RFC-0053 Section 6)
 *
 * 集中管理 AlgContext、句柄与输出缓冲区生命周期，
 * 提供来源扰动生成器和统一的双输出 (C / owned Result) 调用通道。
 */
class AdapterHarness {
 public:
  explicit AdapterHarness(std::shared_ptr<IBizAdapter> adapter)
      : adapter_(std::move(adapter)) {}

  IBizAdapter* Adapter() const { return adapter_.get(); }
  AlgContext& Context() { return ctx_; }
  const AlgContext& Context() const { return ctx_; }
  AdapterStatus& Status() { return status_; }
  const AdapterStatus& Status() const { return status_; }

  int Unpack(const std::vector<const void*>& inputs) {
    return adapter_->Unpack(const_cast<const void**>(inputs.data()),
                            static_cast<int>(inputs.size()), &ctx_, &status_);
  }

  template <typename COutput>
  int PackC(std::vector<COutput>* outputs) {
    if (!outputs) return -1;
    std::vector<void*> output_ptrs(outputs->size());
    for (size_t i = 0; i < outputs->size(); ++i) {
      output_ptrs[i] = &(*outputs)[i];
    }
    int count = static_cast<int>(outputs->size());
    int ret =
        adapter_->Pack(&ctx_, outputs->empty() ? nullptr : output_ptrs.data(),
                       &count, &status_);
    if (ret == 0 && count >= 0 &&
        static_cast<size_t>(count) <= outputs->size()) {
      outputs->resize(count);
    }
    return ret;
  }

  template <typename Result>
  int PackOwned(std::vector<Result>* outputs) {
    if (!outputs) return -1;
    std::vector<void*> output_ptrs(outputs->size());
    for (size_t i = 0; i < outputs->size(); ++i) {
      output_ptrs[i] = &(*outputs)[i];
    }
    int count = static_cast<int>(outputs->size());
    int ret = adapter_->PackResultBatch(
        &ctx_, outputs->empty() ? nullptr : output_ptrs.data(), &count,
        &status_);
    if (ret == 0 && count >= 0 &&
        static_cast<size_t>(count) <= outputs->size()) {
      outputs->resize(count);
    }
    return ret;
  }

  template <typename T>
  bool Publish(const BlackboardKey<T>& key, T value) {
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
  std::shared_ptr<IBizAdapter> adapter_;
  AlgContext ctx_;
  AdapterStatus status_;
};

}  // namespace test
}  // namespace llm_edgeflow
