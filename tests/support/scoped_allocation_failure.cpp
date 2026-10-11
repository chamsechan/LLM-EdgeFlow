#include "scoped_allocation_failure.h"

#include <cstdlib>
#include <limits>
#include <new>

namespace llm_edgeflow::test_support {
thread_local ScopedAllocationFailure* ScopedAllocationFailure::active_ = nullptr;
thread_local ScopedNextAllocationCallback* ScopedNextAllocationCallback::active_ =
    nullptr;

ScopedAllocationFailure::ScopedAllocationFailure(std::ptrdiff_t count) noexcept
    : previous_(active_), remaining_(count) {
  active_ = this;
}
ScopedAllocationFailure::~ScopedAllocationFailure() { active_ = previous_; }

void ScopedAllocationFailure::BeforeAllocation() {
  if (!active_ || active_->remaining_ < 0) return;
  if (active_->remaining_ == 0) {
    active_->remaining_ = -1;
    active_->triggered_ = true;
    throw std::bad_alloc();
  }
  --active_->remaining_;
}

ScopedNextAllocationCallback::ScopedNextAllocationCallback(Callback callback,
                                                         void* state) noexcept
    : previous_(active_), callback_(callback), state_(state) {
  active_ = this;
}
ScopedNextAllocationCallback::~ScopedNextAllocationCallback() {
  active_ = previous_;
}
void ScopedNextAllocationCallback::BeforeAllocation() {
  if (!active_ || !active_->callback_) return;
  const auto callback = active_->callback_;
  active_->callback_ = nullptr;
  callback(active_->state_);
}
}  // namespace llm_edgeflow::test_support

namespace {
void* Allocate(std::size_t bytes, std::size_t alignment = 0) {
  llm_edgeflow::test_support::ScopedNextAllocationCallback::BeforeAllocation();
  llm_edgeflow::test_support::ScopedAllocationFailure::BeforeAllocation();
  if (bytes == 0) bytes = 1;
  if (alignment) {
    if (bytes > std::numeric_limits<std::size_t>::max() - (alignment - 1))
      throw std::bad_alloc();
    bytes = (bytes + alignment - 1) / alignment * alignment;
  }
  for (;;) {
    void* memory = alignment ? std::aligned_alloc(alignment, bytes)
                             : std::malloc(bytes);
    if (memory) return memory;
    const auto handler = std::get_new_handler();
    if (!handler) throw std::bad_alloc();
    handler();
  }
}

void* AllocateWithoutThrow(std::size_t bytes, std::size_t alignment = 0) noexcept {
  try {
    return Allocate(bytes, alignment);
  } catch (...) {
    return nullptr;
  }
}
}  // namespace

void* operator new(std::size_t bytes) { return Allocate(bytes); }
void* operator new[](std::size_t bytes) { return Allocate(bytes); }
void* operator new(std::size_t bytes, std::align_val_t alignment) {
  return Allocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
  return Allocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
  return AllocateWithoutThrow(bytes);
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
  return AllocateWithoutThrow(bytes);
}
void* operator new(std::size_t bytes, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  return AllocateWithoutThrow(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  return AllocateWithoutThrow(bytes, static_cast<std::size_t>(alignment));
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept {
  std::free(memory);
}
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
  std::free(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
  std::free(memory);
}
void operator delete(void* memory, const std::nothrow_t&) noexcept {
  std::free(memory);
}
void operator delete[](void* memory, const std::nothrow_t&) noexcept {
  std::free(memory);
}
void operator delete(void* memory, std::align_val_t,
                     const std::nothrow_t&) noexcept {
  std::free(memory);
}
void operator delete[](void* memory, std::align_val_t,
                       const std::nothrow_t&) noexcept {
  std::free(memory);
}
