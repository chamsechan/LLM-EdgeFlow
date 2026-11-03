#pragma once

#include <memory>
#include <utility>

namespace llm_edgeflow::detail {

// 通过对象的 Clone 保持值语义；复制后的可变绑定状态属于各自实例。
template <typename T>
class ClonePtr {
 public:
  ClonePtr(std::unique_ptr<T> value) : value_(std::move(value)) {}

  ClonePtr(const ClonePtr& other) : value_(other ? other->Clone() : nullptr) {}
  ClonePtr(ClonePtr&&) noexcept = default;
  ClonePtr& operator=(const ClonePtr& other) {
    if (this != &other) value_ = other ? other->Clone() : nullptr;
    return *this;
  }
  ClonePtr& operator=(ClonePtr&&) noexcept = default;

  explicit operator bool() const noexcept { return bool(value_); }
  T* get() noexcept { return value_.get(); }
  const T* get() const noexcept { return value_.get(); }
  T& operator*() noexcept { return *value_; }
  const T& operator*() const noexcept { return *value_; }
  T* operator->() noexcept { return value_.get(); }
  const T* operator->() const noexcept { return value_.get(); }

 private:
  std::unique_ptr<T> value_;
};

}  // namespace llm_edgeflow::detail
