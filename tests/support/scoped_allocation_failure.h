#pragma once

#include <cstddef>

namespace llm_edgeflow::test_support {

// Arm around synchronous production calls, outside assertions. One failure
// disarms injection so diagnostics and cleanup can allocate normally.
class ScopedAllocationFailure {
 public:
  explicit ScopedAllocationFailure(
      std::ptrdiff_t successful_allocations = -1) noexcept;
  ~ScopedAllocationFailure();
  ScopedAllocationFailure(const ScopedAllocationFailure&) = delete;
  ScopedAllocationFailure& operator=(const ScopedAllocationFailure&) = delete;
  bool Triggered() const noexcept { return triggered_; }
  static void BeforeAllocation();

 private:
  static thread_local ScopedAllocationFailure* active_;
  ScopedAllocationFailure* previous_;
  std::ptrdiff_t remaining_;
  bool triggered_ = false;
};

// Observe the next real allocation without allocating callback storage.
// Disarm before invoking the callback; it can safely arm a new observer.
class ScopedNextAllocationCallback {
 public:
  using Callback = void (*)(void*);
  ScopedNextAllocationCallback(Callback callback, void* state) noexcept;
  ~ScopedNextAllocationCallback();
  ScopedNextAllocationCallback(const ScopedNextAllocationCallback&) = delete;
  ScopedNextAllocationCallback& operator=(const ScopedNextAllocationCallback&) =
      delete;
  static void BeforeAllocation();

 private:
  static thread_local ScopedNextAllocationCallback* active_;
  ScopedNextAllocationCallback* previous_;
  Callback callback_;
  void* state_;
};
}  // namespace llm_edgeflow::test_support
