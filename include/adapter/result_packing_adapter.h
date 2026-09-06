#pragma once

#include <string>

#include "adapter/biz_adapter_interface.h"

namespace llm_edgeflow {

// One business PackTyped implementation serves both public C arrays and the
// Operator's owned variable-length results. Validation cannot drift by facade.
template <typename Derived, typename COutput, typename Result>
class ResultPackingAdapter : public IBizAdapter {
 public:
  int Pack(AlgContext* ctx, void** outputs, int* count,
           AdapterStatus* status = nullptr) const final {
    return static_cast<const Derived*>(this)->template PackTyped<COutput>(
        ctx, outputs, count, status);
  }
  const char* ResultTypeName() const final { return Result::kTypeName; }
  int PackResultBatch(AlgContext* ctx, void** outputs, int* count,
                      AdapterStatus* status = nullptr) const final {
    return static_cast<const Derived*>(this)->template PackTyped<Result>(
        ctx, outputs, count, status);
  }
};

template <size_t N>
bool CopyResultString(char (&destination)[N], const char* value,
                      const char* field, int index, const char* biz,
                      AdapterStatus* status) {
  return AdapterValidationHelper::CheckedStringCopy(destination, N, value,
                                                    field, index, biz, status);
}

inline bool CopyResultString(std::string& destination, const char* value,
                             const char*, int, const char*, AdapterStatus*) {
  destination = value ? value : "";
  return true;
}

}  // namespace llm_edgeflow
