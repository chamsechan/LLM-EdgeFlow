#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace llm_edgeflow::test_support {

// A bounded handshake: timeout must fail the caller, never silently allow a
// Process to finish before the publication under test. Always resume and join
// the reader before fatal GoogleTest assertions.
class NodeProcessPause {
 public:
  explicit NodeProcessPause(
      std::chrono::milliseconds timeout = std::chrono::seconds(5))
      : timeout_(timeout) {}

  static void OnAllocation(void* user_data) {
    static_cast<NodeProcessPause*>(user_data)->Pause();
  }

  void Pause() {
    std::unique_lock<std::mutex> lock(mutex_);
    paused_ = true;
    condition_.notify_all();
    if (!condition_.wait_for(lock, timeout_, [this] { return resumed_; })) {
      throw std::runtime_error("Process publication handshake timed out");
    }
  }

  bool WaitUntilPaused() {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout_, [this] { return paused_; });
  }

  void Resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    resumed_ = true;
    condition_.notify_all();
  }

 private:
  const std::chrono::milliseconds timeout_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool paused_ = false;
  bool resumed_ = false;
};

}  // namespace llm_edgeflow::test_support
