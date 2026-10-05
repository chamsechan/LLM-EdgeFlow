#pragma once

#include <string>
#include <utility>
#include <vector>

namespace llm_edgeflow {

// 单个注册表的 fail-closed 冲突状态，由注册表自身的互斥锁保护；
// 即使消息无法保存，冲突也会被记录。
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

  // 已记录的消息；存在冲突时绝不为空。
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
