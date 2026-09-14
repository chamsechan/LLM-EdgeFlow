#pragma once

#include <array>
#include <cstddef>

namespace llm_edgeflow::test_support {

// One-shot callback on this thread's next replacement-new allocation. Clear
// before invoking so the callback may itself allocate without recursion.
class ScopedNextAllocationCallback {
 public:
  using Callback = void (*)(void*);
  ScopedNextAllocationCallback(Callback callback, void* user_data) noexcept;
  ~ScopedNextAllocationCallback();
  ScopedNextAllocationCallback(const ScopedNextAllocationCallback&) = delete;
  ScopedNextAllocationCallback& operator=(const ScopedNextAllocationCallback&) =
      delete;
  static void BeforeAllocation();

 private:
  static thread_local ScopedNextAllocationCallback* current_;
  ScopedNextAllocationCallback* previous_;
  Callback callback_;
  void* user_data_;
};

// Test-executable-only replacement new/delete support. Arm only around a
// synchronous operation, outside GoogleTest assertions. For leak assertions,
// destroy tracked allocations on this thread before checking Outstanding().
// Successful operations may transfer ownership beyond the scope; those
// surviving allocations are no longer tracked. Nested scopes restore the outer
// injection state; other threads are unaffected.
class ScopedAllocationFailure {
 public:
  explicit ScopedAllocationFailure(std::ptrdiff_t fail_after = -1) noexcept;
  ~ScopedAllocationFailure();
  ScopedAllocationFailure(const ScopedAllocationFailure&) = delete;
  ScopedAllocationFailure& operator=(const ScopedAllocationFailure&) = delete;

  void DisableFailure() noexcept { remaining_ = -1; }
  bool Triggered() const noexcept { return triggered_; }
  size_t Outstanding() const noexcept { return count_; }
  bool Overflowed() const noexcept { return overflowed_; }

  // Used exclusively by the replacement allocation functions in the .cpp.
  static void BeforeAllocation();
  static void RecordAllocation(void* ptr) noexcept;
  static void RecordDeallocation(void* ptr) noexcept;

 private:
  static thread_local ScopedAllocationFailure* current_;
  ScopedAllocationFailure* previous_;
  std::ptrdiff_t remaining_;
  bool triggered_ = false;
  bool overflowed_ = false;
  size_t count_ = 0;
  // A bounded ledger avoids allocating while observing allocation. Tests must
  // assert !Overflowed() before using Outstanding() as leak evidence.
  std::array<void*, 4096> allocations_{};
};

}  // namespace llm_edgeflow::test_support
