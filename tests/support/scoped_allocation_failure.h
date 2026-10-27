#pragma once

#include <cstddef>

namespace llm_edgeflow::test_support {

// 在断言之外包住同步的生产调用。触发一次失败后即解除注入，使诊断和
// 清理能正常分配内存。
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

// 观察下一次真实分配，不为回调分配存储。调用回调前先解除，回调内可以
// 安全地设置新的观察者。
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
