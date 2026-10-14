#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace llm_edgeflow {

enum class BatchFailureReason {
  kDuplicate,
  kMissing,
  kUnknown,
  kSubIdOverflow,
  kCountOverflow,
  kCallbackFailed,
};

inline const char* BatchFailureReasonName(BatchFailureReason reason) noexcept {
  switch (reason) {
    case BatchFailureReason::kDuplicate:
      return "duplicate";
    case BatchFailureReason::kMissing:
      return "missing";
    case BatchFailureReason::kUnknown:
      return "unknown";
    case BatchFailureReason::kSubIdOverflow:
      return "sub_id_overflow";
    case BatchFailureReason::kCountOverflow:
      return "count_overflow";
    case BatchFailureReason::kCallbackFailed:
      return "callback_failed";
  }
  return "unknown";
}

struct TraceableItemKey {
  uint32_t req_id = 0;
  uint32_t sub_id = 0;

  constexpr bool operator==(const TraceableItemKey& other) const noexcept {
    return req_id == other.req_id && sub_id == other.sub_id;
  }
  constexpr bool operator!=(const TraceableItemKey& other) const noexcept {
    return !(*this == other);
  }
  constexpr bool operator<(const TraceableItemKey& other) const noexcept {
    if (req_id != other.req_id) return req_id < other.req_id;
    return sub_id < other.sub_id;
  }
};

struct TraceableItemKeyHash {
  std::size_t operator()(const TraceableItemKey& k) const noexcept {
    uint64_t x = (static_cast<uint64_t>(k.req_id) << 32) | k.sub_id;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return static_cast<std::size_t>(x);
  }
};

struct BatchFailureDetail {
  std::string operation;
  BatchFailureReason reason = BatchFailureReason::kUnknown;
  std::optional<TraceableItemKey> key;
};

inline std::string FormatBatchFailureDetail(const BatchFailureDetail& detail) {
  std::string out;
  if (!detail.operation.empty()) {
    out += detail.operation;
  }
  if (detail.reason != BatchFailureReason::kUnknown) {
    if (!out.empty()) out += " ";
    out += BatchFailureReasonName(detail.reason);
  }
  if (detail.key.has_value()) {
    if (!out.empty()) out += " for ";
    out += "req_id=" + std::to_string(detail.key->req_id) +
           ", sub_id=" + std::to_string(detail.key->sub_id);
  }
  return out;
}

struct NodeFailure {
  std::string message;
  int cause_code = 0;
  std::optional<BatchFailureDetail> batch_detail;

  std::string FormatDiagnostic(std::string_view fallback_message = {}) const {
    std::string base =
        message.empty() ? std::string(fallback_message) : message;
    if (!batch_detail) return base;
    std::string source = FormatBatchFailureDetail(*batch_detail);
    if (source.empty()) return base;
    return base.empty() ? source : source + ": " + base;
  }

  NodeFailure() = default;
  NodeFailure(std::string msg, int cause = 0)
      : message(std::move(msg)), cause_code(cause) {}

  NodeFailure(std::string msg, BatchFailureDetail detail, int cause = 0)
      : message(std::move(msg)),
        cause_code(cause),
        batch_detail(std::move(detail)) {}
};

template <typename T>
class [[nodiscard]] NodeResult {
 public:
  // 禁止默认构造，避免未初始化或语义不明的状态。
  NodeResult() = delete;

  NodeResult(T value) : storage_(std::move(value)) {}  // NOLINT

  static NodeResult<T> Success(T value) {
    return NodeResult<T>(std::move(value));
  }

  static NodeResult<T> Failure(NodeFailure failure) {
    return NodeResult<T>(FailureTag{}, std::move(failure));
  }

  static NodeResult<T> Failure(std::string message, int cause_code = 0) {
    return Failure(NodeFailure(std::move(message), cause_code));
  }

  static NodeResult<T> Failure(std::string message, BatchFailureDetail detail,
                               int cause_code = 0) {
    return Failure(
        NodeFailure(std::move(message), std::move(detail), cause_code));
  }

  bool ok() const noexcept { return std::holds_alternative<T>(storage_); }

  explicit operator bool() const noexcept { return ok(); }

  const T& value() const& {
    if (!ok()) {
      throw std::logic_error(
          "Attempted to access value() on failed NodeResult: " +
          std::get<NodeFailure>(storage_).message);
    }
    return std::get<T>(storage_);
  }

  T& value() & {
    if (!ok()) {
      throw std::logic_error(
          "Attempted to access value() on failed NodeResult: " +
          std::get<NodeFailure>(storage_).message);
    }
    return std::get<T>(storage_);
  }

  T&& value() && {
    if (!ok()) {
      throw std::logic_error(
          "Attempted to access value() on failed NodeResult: " +
          std::get<NodeFailure>(storage_).message);
    }
    return std::get<T>(std::move(storage_));
  }

  const NodeFailure& failure() const& noexcept {
    static const NodeFailure empty_failure{};
    if (ok()) return empty_failure;
    return std::get<NodeFailure>(storage_);
  }

  NodeFailure ExtractFailure() && {
    if (ok()) return NodeFailure{};
    return std::get<NodeFailure>(std::move(storage_));
  }

 private:
  struct FailureTag {};
  NodeResult(FailureTag, NodeFailure failure) : storage_(std::move(failure)) {}

  std::variant<T, NodeFailure> storage_;
};

template <typename T>
struct IsNodeResult : std::false_type {
  using ValueType = T;
};

template <typename T>
struct IsNodeResult<NodeResult<T>> : std::true_type {
  using ValueType = T;
};

}  // namespace llm_edgeflow

namespace std {
template <>
struct hash<llm_edgeflow::TraceableItemKey> {
  std::size_t operator()(
      const llm_edgeflow::TraceableItemKey& k) const noexcept {
    return llm_edgeflow::TraceableItemKeyHash{}(k);
  }
};
}  // namespace std
