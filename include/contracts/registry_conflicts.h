#pragma once

#include <string>
#include <utility>
#include <vector>

namespace llm_edgeflow {

// Fail-closed conflict state of one registry. The registry guards it with its
// own mutex; a conflict stays recorded even if its message cannot be stored.
class RegistryConflicts {
 public:
  void Record(std::string message) noexcept {
    has_conflict_ = true;
    try {
      messages_.push_back(std::move(message));
    } catch (...) {
    }
  }

  bool HasConflict() const noexcept { return has_conflict_; }

  // Recorded messages; never empty while a conflict is recorded.
  std::vector<std::string> Messages() const {
    if (has_conflict_ && messages_.empty()) {
      return {"Registry conflict recorded without a stored message"};
    }
    return messages_;
  }

  void Clear() noexcept {
    has_conflict_ = false;
    messages_.clear();
  }

 private:
  bool has_conflict_ = false;
  std::vector<std::string> messages_;
};

}  // namespace llm_edgeflow
