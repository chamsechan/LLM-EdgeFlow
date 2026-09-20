#pragma once

#include <initializer_list>
#include <map>

#include "adapter/io_converter.h"

namespace llm_edgeflow {

inline std::vector<std::shared_ptr<void>> BorrowInputForTest(
    std::initializer_list<const void*> values) {
  std::vector<std::shared_ptr<void>> slots;
  for (const void* value : values) {
    slots.emplace_back(const_cast<void*>(value), [](void*) {});
  }
  return slots;
}

class TestOutputBatchView : public ExternalOutputBatchView {
 public:
  TestOutputBatchView() = default;
  TestOutputBatchView(const TestOutputBatchView&) = delete;
  TestOutputBatchView& operator=(const TestOutputBatchView&) = delete;
  TestOutputBatchView(TestOutputBatchView&&) = delete;
  TestOutputBatchView& operator=(TestOutputBatchView&&) = delete;

  void SetCapacity(const std::string& slot, const std::string& field,
                   size_t capacity) {
    auto& spec = specs_[slot];
    spec.capacities[field] = capacity;
    pool_specs[slot] = &spec;
  }

 private:
  std::map<std::string, ResolvedOutputPoolSpec> specs_;
};

}  // namespace llm_edgeflow
