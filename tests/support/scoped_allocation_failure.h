#pragma once

#include <array>
#include <cstddef>

namespace llm_edgeflow::test_support {

// 本线程下一次替换 new 分配时触发的一次性回调。调用前先清除，
// 使回调自身可以分配内存而不会递归。
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

// 仅供测试可执行文件使用的替换 new/delete 支持。只在同步操作期间启用，
// 且不要包住 GoogleTest 断言。做泄漏断言时，须先在本线程销毁被跟踪的分配，
// 再检查 Outstanding()。成功的操作可能把所有权转移到作用域之外，这些存活的
// 分配不再被跟踪。嵌套作用域会恢复外层的注入状态；其他线程不受影响。
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

  // 仅供 .cpp 中的替换分配函数使用。
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
  // 使用有界账本，避免在观察分配时自身分配内存。
  // 测试在把 Outstanding() 作为泄漏证据前，必须先断言 !Overflowed()。
  std::array<void*, 4096> allocations_{};
};

}  // namespace llm_edgeflow::test_support
