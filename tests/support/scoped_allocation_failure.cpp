#include "scoped_allocation_failure.h"

#include <cstdlib>
#include <limits>
#include <new>

namespace llm_edgeflow::test_support {

thread_local ScopedAllocationFailure* ScopedAllocationFailure::current_ =
    nullptr;

ScopedAllocationFailure::ScopedAllocationFailure(
    std::ptrdiff_t fail_after) noexcept
    : previous_(current_), remaining_(fail_after) {
  current_ = this;
}

ScopedAllocationFailure::~ScopedAllocationFailure() { current_ = previous_; }

void ScopedAllocationFailure::BeforeAllocation() {
  if (!current_ || current_->remaining_ < 0) return;
  if (current_->remaining_-- == 0) {
    // One-shot failure allows diagnostics and exception cleanup to allocate.
    current_->triggered_ = true;
    throw std::bad_alloc();
  }
}

void ScopedAllocationFailure::RecordAllocation(void* ptr) noexcept {
  if (!current_) return;
  if (current_->count_ == current_->allocations_.size()) {
    current_->overflowed_ = true;
    return;
  }
  current_->allocations_[current_->count_++] = ptr;
}

void ScopedAllocationFailure::RecordDeallocation(void* ptr) noexcept {
  for (auto* scope = current_; scope; scope = scope->previous_) {
    for (size_t i = 0; i < scope->count_; ++i) {
      if (scope->allocations_[i] == ptr) {
        scope->allocations_[i] = scope->allocations_[--scope->count_];
        return;
      }
    }
  }
}

}  // namespace llm_edgeflow::test_support

namespace {

using llm_edgeflow::test_support::ScopedAllocationFailure;

void* Allocate(size_t size, size_t alignment = 0) {
  ScopedAllocationFailure::BeforeAllocation();
  if (size == 0) size = 1;
  if (alignment) {
    if (size > std::numeric_limits<size_t>::max() - (alignment - 1)) {
      throw std::bad_alloc();
    }
    size = (size + alignment - 1) / alignment * alignment;
  }
  for (;;) {
    void* ptr =
        alignment ? std::aligned_alloc(alignment, size) : std::malloc(size);
    if (ptr) {
      ScopedAllocationFailure::RecordAllocation(ptr);
      return ptr;
    }
    auto handler = std::get_new_handler();
    if (!handler) throw std::bad_alloc();
    handler();
  }
}

void Deallocate(void* ptr) noexcept {
  if (!ptr) return;
  ScopedAllocationFailure::RecordDeallocation(ptr);
  std::free(ptr);
}

}  // namespace

void* operator new(size_t size) { return Allocate(size); }
void* operator new[](size_t size) { return Allocate(size); }
void* operator new(size_t size, std::align_val_t alignment) {
  return Allocate(size, static_cast<size_t>(alignment));
}
void* operator new[](size_t size, std::align_val_t alignment) {
  return Allocate(size, static_cast<size_t>(alignment));
}
void* operator new(size_t size, const std::nothrow_t&) noexcept {
  try {
    return Allocate(size);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](size_t size, const std::nothrow_t& tag) noexcept {
  return ::operator new(size, tag);
}
void* operator new(size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  try {
    return Allocate(size, static_cast<size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](size_t size, std::align_val_t alignment,
                     const std::nothrow_t& tag) noexcept {
  return ::operator new(size, alignment, tag);
}

void operator delete(void* ptr) noexcept { Deallocate(ptr); }
void operator delete[](void* ptr) noexcept { Deallocate(ptr); }
void operator delete(void* ptr, size_t) noexcept { Deallocate(ptr); }
void operator delete[](void* ptr, size_t) noexcept { Deallocate(ptr); }
void operator delete(void* ptr, std::align_val_t) noexcept { Deallocate(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept {
  Deallocate(ptr);
}
void operator delete(void* ptr, size_t, std::align_val_t) noexcept {
  Deallocate(ptr);
}
void operator delete[](void* ptr, size_t, std::align_val_t) noexcept {
  Deallocate(ptr);
}
void operator delete(void* ptr, const std::nothrow_t&) noexcept {
  Deallocate(ptr);
}
void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
  Deallocate(ptr);
}
void operator delete(void* ptr, std::align_val_t,
                     const std::nothrow_t&) noexcept {
  Deallocate(ptr);
}
void operator delete[](void* ptr, std::align_val_t,
                       const std::nothrow_t&) noexcept {
  Deallocate(ptr);
}
