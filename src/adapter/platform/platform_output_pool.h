#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/platform/platform_value_type_registry.h"

namespace alg_framework {

class OutputPoolState;

/**
 * @brief 输出池自定义 Deleter (仅捕获 weak_ptr，生命周期安全)
 */
struct OutputPoolDeleter {
  std::weak_ptr<OutputPoolState> weak_pool;
  void* block = nullptr;

  void operator()(void*) const noexcept;
};

/**
 * @brief 块状态枚举 (用于状态机账本严格防下溢与防重复归还)
 */
enum class BlockState { kFree, kCheckedOut };

/**
 * @brief 单个输出后缀的输出对象预分配池状态机 (固定容量零分配无下溢)
 */
class OutputPoolState : public std::enable_shared_from_this<OutputPoolState> {
 public:
  static int Create(const std::string& suffix, uint32_t depth,
                    const ResolvedOutputPoolSpec& spec,
                    const PlatformValueTypeBinding* binding,
                    std::shared_ptr<OutputPoolState>* out_pool,
                    std::string* err);

  ~OutputPoolState();

  /**
   * @brief 从空闲队列检出一个块 (池为空时条件变量阻塞等待)
   */
  int Acquire(void** out_block);

  /**
   * @brief 将已检出的块重置并归还池中 (校验归属与状态账本，noexcept
   * 且零内存分配)
   */
  void ReturnBlock(void* block) noexcept;

  /**
   * @brief 关闭输出池并唤醒所有等待线程
   * @return 关闭瞬间尚未归还的块数量 (checked_out_count)
   */
  uint32_t CloseAndDrain() noexcept;

  /**
   * @brief 释放池中所有块及嵌套分配
   */
  void DestroyBlocks() noexcept;

  bool IsClosing() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return closing_;
  }

  uint32_t Depth() const noexcept { return depth_; }
  const std::string& CanonicalSuffix() const noexcept {
    return canonical_suffix_;
  }
  const ResolvedOutputPoolSpec& Spec() const noexcept { return spec_; }

  uint32_t CheckedOutCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return checked_out_count_;
  }

  uint32_t FreeBlockCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(free_count_);
  }

  /**
   * @brief 测试专用的发布故障注入探针 (非公开 ABI，仅单测使用)
   */
  static void SetPublishFailureCountdown(int count) noexcept;
  static int GetPublishFailureCountdown() noexcept;

  /**
   * @brief 测试专用的命名故障阶段注入与对称构造/析构计数探针
   */
  enum class FailureStage {
    kNone = 0,
    kPoolStateAlloc,
    kSpecCopy,
    kAllBlocksReserve,
    kFreeRingResize,
    kBlockStatesReserve,
    kRootStructAlloc,
    kNestedStringAlloc,
    kNestedAnyAlloc,
    kCleanupRegister,
    kLedgerRegister,
    kHistoryBlockN
  };

  static void SetFailureStageProbe(FailureStage stage,
                                   int target_block_index = 0) noexcept;
  static FailureStage GetFailureStageProbe() noexcept;
  static int GetTargetBlockIndex() noexcept;
  static uint64_t GetConstructedCount() noexcept;
  static uint64_t GetDestroyedCount() noexcept;
  static void ResetInstanceCounters() noexcept;
  static void RecordConstructed() noexcept;
  static void RecordDestroyed() noexcept;

 private:
  OutputPoolState() noexcept { RecordConstructed(); }

  std::string canonical_suffix_;
  uint32_t depth_ = 0;
  ResolvedOutputPoolSpec spec_;
  const PlatformValueTypeBinding* type_binding_ = nullptr;

  std::vector<OwnedExternalBlock> all_blocks_;
  std::vector<void*> free_ring_;
  size_t free_head_ = 0;
  size_t free_tail_ = 0;
  size_t free_count_ = 0;
  std::unordered_map<void*, BlockState> block_states_;
  mutable std::mutex mutex_;
  std::condition_variable available_;
  uint32_t checked_out_count_ = 0;
  bool closing_ = false;
};

/**
 * @brief Process 执行期局部检出租约守卫 (RAII 回滚保护与单块状态转移)
 */
class ScopedOutputLeaseGuard {
 public:
  ScopedOutputLeaseGuard() = default;
  ~ScopedOutputLeaseGuard() { Rollback(); }

  ScopedOutputLeaseGuard(const ScopedOutputLeaseGuard&) = delete;
  ScopedOutputLeaseGuard& operator=(const ScopedOutputLeaseGuard&) = delete;

  void Reserve(size_t count) { leases_.reserve(count); }

  void Track(std::shared_ptr<OutputPoolState> pool, void* block) {
    leases_.push_back({std::move(pool), block});
  }

  void Untrack(void* block) noexcept {
    for (auto it = leases_.begin(); it != leases_.end(); ++it) {
      if (it->block == block) {
        leases_.erase(it);
        break;
      }
    }
  }

  void Commit() noexcept {
    committed_ = true;
    leases_.clear();
  }

  void Rollback() noexcept {
    if (committed_) return;
    for (auto& item : leases_) {
      if (item.pool && item.block) {
        item.pool->ReturnBlock(item.block);
      }
    }
    leases_.clear();
  }

 private:
  struct LeaseItem {
    std::shared_ptr<OutputPoolState> pool;
    void* block = nullptr;
  };
  std::vector<LeaseItem> leases_;
  bool committed_ = false;
};

}  // namespace alg_framework
