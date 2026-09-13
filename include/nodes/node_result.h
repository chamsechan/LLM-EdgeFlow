#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace llm_edgeflow {

enum class NodeErrorKind {
  kBusinessError,
  kModelCallError,
  kOutputCountMismatch,
  kOutputProvenanceMismatch,
  kInputError,
  kInternalError,
};

inline const char* NodeErrorKindName(NodeErrorKind kind) noexcept {
  switch (kind) {
    case NodeErrorKind::kBusinessError:
      return "BusinessError";
    case NodeErrorKind::kModelCallError:
      return "ModelCallError";
    case NodeErrorKind::kOutputCountMismatch:
      return "OutputCountMismatch";
    case NodeErrorKind::kOutputProvenanceMismatch:
      return "OutputProvenanceMismatch";
    case NodeErrorKind::kInputError:
      return "InputError";
    case NodeErrorKind::kInternalError:
      return "InternalError";
  }
  return "UnknownError";
}

struct NodeFailure {
  NodeErrorKind kind = NodeErrorKind::kBusinessError;
  std::string message;
  int cause_code = 0;
  std::string stage;
  std::string model_slot;
  std::string source_location;

  NodeFailure() = default;
  NodeFailure(NodeErrorKind k, std::string msg, int cause = 0,
              std::string stg = {}, std::string slot = {}, std::string loc = {})
      : kind(k),
        message(std::move(msg)),
        cause_code(cause),
        stage(std::move(stg)),
        model_slot(std::move(slot)),
        source_location(std::move(loc)) {}
};

template <typename T>
class [[nodiscard]] NodeResult {
 public:
  // Disallow default construction to avoid uninitialized / ambiguous states.
  NodeResult() = delete;

  // Construct from value
  NodeResult(T value) : storage_(std::move(value)) {}  // NOLINT

  static NodeResult<T> Success(T value) {
    return NodeResult<T>(std::move(value));
  }

  static NodeResult<T> Failure(NodeFailure failure) {
    return NodeResult<T>(FailureTag{}, std::move(failure));
  }

  static NodeResult<T> Failure(NodeErrorKind kind, std::string message,
                               int cause_code = 0, std::string stage = {},
                               std::string model_slot = {},
                               std::string source_location = {}) {
    return NodeResult<T>(
        FailureTag{},
        NodeFailure(kind, std::move(message), cause_code, std::move(stage),
                    std::move(model_slot), std::move(source_location)));
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

}  // namespace llm_edgeflow
