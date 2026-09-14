#pragma once

#include <string>
#include <utility>

#include "adapter/adapter_status.h"
#include "edgeflow/c_api.h"

namespace llm_edgeflow {

/**
 * @brief 类型化适配器转换结果 (ADP-001, RFC-0053)
 *
 * 持有成功值 T，或独立的 return_code 与完整 AdapterStatus 诊断。
 * 返回码与诊断码不假设相等。属于 Integration 层，不复用 NodeResult。
 */
template <typename T>
class AdapterResult {
 public:
  AdapterResult(const T& value)  // NOLINT(google-explicit-constructor)
      : is_ok_(true), return_code_(COMPANY_ALG_SUCCESS), value_(value) {}

  AdapterResult(T&& value)  // NOLINT(google-explicit-constructor)
      : is_ok_(true),
        return_code_(COMPANY_ALG_SUCCESS),
        value_(std::move(value)) {}

  AdapterResult(int return_code, AdapterStatus status)
      : is_ok_(false),
        return_code_(return_code),
        status_(std::move(status)),
        value_() {}

  static AdapterResult<T> Ok(T value) {
    return AdapterResult<T>(std::move(value));
  }

  static AdapterResult<T> Error(int return_code, AdapterStatus status) {
    return AdapterResult<T>(return_code, std::move(status));
  }

  static AdapterResult<T> InvalidInput(std::string message,
                                       std::string field_path = "",
                                       int sample_index = -1,
                                       std::string adapter_name = "") {
    return AdapterResult<T>(
        COMPANY_ALG_ERR_INVALID_INPUT,
        AdapterStatus::InvalidInput(std::move(message), std::move(field_path),
                                    sample_index, std::move(adapter_name)));
  }

  static AdapterResult<T> BufferTooSmall(std::string message,
                                         std::string field_path = "",
                                         int sample_index = -1,
                                         std::string adapter_name = "") {
    return AdapterResult<T>(
        COMPANY_ALG_ERR_BUFFER_TOO_SMALL,
        AdapterStatus::BufferTooSmall(std::move(message), std::move(field_path),
                                      sample_index, std::move(adapter_name)));
  }

  bool IsOk() const noexcept { return is_ok_; }
  explicit operator bool() const noexcept { return is_ok_; }

  int ReturnCode() const noexcept { return return_code_; }
  const AdapterStatus& Status() const noexcept { return status_; }

  const T& Value() const { return value_; }
  T& Value() { return value_; }
  T&& TakeValue() { return std::move(value_); }
  T ValueOr(T default_val) const {
    return is_ok_ ? value_ : std::move(default_val);
  }

  const T* operator->() const { return &value_; }
  T* operator->() { return &value_; }
  const T& operator*() const { return value_; }
  T& operator*() { return value_; }

 private:
  bool is_ok_{false};
  int return_code_{COMPANY_ALG_SUCCESS};
  AdapterStatus status_;
  T value_{};
};

template <>
class AdapterResult<void> {
 public:
  AdapterResult() : is_ok_(true), return_code_(COMPANY_ALG_SUCCESS) {}

  AdapterResult(int return_code, AdapterStatus status)
      : is_ok_(false), return_code_(return_code), status_(std::move(status)) {}

  static AdapterResult<void> Ok() { return AdapterResult<void>(); }

  static AdapterResult<void> Error(int return_code, AdapterStatus status) {
    return AdapterResult<void>(return_code, std::move(status));
  }

  static AdapterResult<void> InvalidInput(std::string message,
                                          std::string field_path = "",
                                          int sample_index = -1,
                                          std::string adapter_name = "") {
    return AdapterResult<void>(
        COMPANY_ALG_ERR_INVALID_INPUT,
        AdapterStatus::InvalidInput(std::move(message), std::move(field_path),
                                    sample_index, std::move(adapter_name)));
  }

  static AdapterResult<void> BufferTooSmall(std::string message,
                                            std::string field_path = "",
                                            int sample_index = -1,
                                            std::string adapter_name = "") {
    return AdapterResult<void>(
        COMPANY_ALG_ERR_BUFFER_TOO_SMALL,
        AdapterStatus::BufferTooSmall(std::move(message), std::move(field_path),
                                      sample_index, std::move(adapter_name)));
  }

  bool IsOk() const noexcept { return is_ok_; }
  explicit operator bool() const noexcept { return is_ok_; }

  int ReturnCode() const noexcept { return return_code_; }
  const AdapterStatus& Status() const noexcept { return status_; }

 private:
  bool is_ok_{false};
  int return_code_{COMPANY_ALG_SUCCESS};
  AdapterStatus status_;
};

}  // namespace llm_edgeflow
