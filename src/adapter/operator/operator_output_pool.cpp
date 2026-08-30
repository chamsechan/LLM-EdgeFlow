#include "adapter/operator/operator_output_pool.h"

#include <atomic>

namespace alg_framework {

namespace {

std::atomic<int> g_publish_failure_countdown{-1};
std::atomic<OutputPoolState::FailureStage> g_failure_stage{
    OutputPoolState::FailureStage::kNone};
std::atomic<int> g_target_block_index{0};
std::atomic<uint64_t> g_constructed_count{0};
std::atomic<uint64_t> g_destroyed_count{0};

}  // namespace

void OutputPoolState::SetPublishFailureCountdown(int count) noexcept {
  g_publish_failure_countdown.store(count);
}

int OutputPoolState::GetPublishFailureCountdown() noexcept {
  return g_publish_failure_countdown.load();
}

void OutputPoolState::SetFailureStageProbe(FailureStage stage,
                                           int target_block_index) noexcept {
  g_failure_stage.store(stage);
  g_target_block_index.store(target_block_index);
}

OutputPoolState::FailureStage OutputPoolState::GetFailureStageProbe() noexcept {
  return g_failure_stage.load();
}

int OutputPoolState::GetTargetBlockIndex() noexcept {
  return g_target_block_index.load();
}

uint64_t OutputPoolState::GetConstructedCount() noexcept {
  return g_constructed_count.load();
}

uint64_t OutputPoolState::GetDestroyedCount() noexcept {
  return g_destroyed_count.load();
}

void OutputPoolState::ResetInstanceCounters() noexcept {
  g_constructed_count.store(0);
  g_destroyed_count.store(0);
  g_failure_stage.store(FailureStage::kNone);
  g_target_block_index.store(0);
}

void OutputPoolState::RecordConstructed() noexcept {
  g_constructed_count.fetch_add(1);
}

void OutputPoolState::RecordDestroyed() noexcept {
  g_destroyed_count.fetch_add(1);
}

void OutputPoolDeleter::operator()(void*) const noexcept {
  if (auto pool = weak_pool.lock()) {
    pool->ReturnBlock(block);
  }
}

int OutputPoolState::Create(const std::string& suffix, uint32_t depth,
                            const ResolvedOutputPoolSpec& spec,
                            const OperatorValueTypeBinding* binding,
                            std::shared_ptr<OutputPoolState>* out_pool,
                            std::string* err) {
  if (!out_pool) {
    if (err) *err = "Null output pool pointer";
    return -2;
  }
  *out_pool = nullptr;

  if (!binding || binding->direction != IoDirection::kOutput ||
      binding->canonical_suffix != suffix || !binding->allocate_external ||
      !binding->reset_external || !binding->destroy_external) {
    if (err) *err = "Invalid or incomplete binding for suffix: " + suffix;
    return -2;
  }

  uint32_t effective_depth = (depth == 0) ? kDefaultOutputPoolDepth : depth;
  if (effective_depth > kMaxOutputPoolDepth) {
    if (err) {
      *err = "Output pool depth " + std::to_string(effective_depth) +
             " exceeds max limit " + std::to_string(kMaxOutputPoolDepth);
    }
    return -2;
  }

  ResolvedOutputPoolSpec resolved_spec;
  if (!ResolveOutputPoolSpec(*binding, spec, &resolved_spec, err)) {
    return -2;
  }

  size_t estimated_bytes = 0;
  if (!ComputeOutputPoolPayloadBytes(*binding, resolved_spec, depth,
                                     &estimated_bytes, err)) {
    return -2;
  }

  try {
    if (g_failure_stage.load() == FailureStage::kPoolStateAlloc) {
      throw std::bad_alloc();
    }

    auto pool = std::shared_ptr<OutputPoolState>(new OutputPoolState());
    pool->canonical_suffix_ = suffix;
    pool->depth_ = effective_depth;
    if (g_failure_stage.load() == FailureStage::kSpecCopy) {
      throw std::bad_alloc();
    }
    pool->spec_ = resolved_spec;
    pool->type_binding_ = binding;

    if (g_failure_stage.load() == FailureStage::kAllBlocksReserve) {
      throw std::bad_alloc();
    }
    pool->all_blocks_.reserve(effective_depth);
    if (g_failure_stage.load() == FailureStage::kFreeRingResize) {
      throw std::bad_alloc();
    }
    pool->free_ring_.resize(effective_depth, nullptr);

    if (g_failure_stage.load() == FailureStage::kBlockStatesReserve) {
      throw std::bad_alloc();
    }
    pool->block_states_.reserve(effective_depth);

    for (uint32_t i = 0; i < effective_depth; ++i) {
      if (g_failure_stage.load() == FailureStage::kHistoryBlockN &&
          static_cast<int>(i) == g_target_block_index.load()) {
        pool->DestroyBlocks();
        if (out_pool) *out_pool = nullptr;
        if (err)
          *err = "Injected failure at history block " + std::to_string(i);
        return -4;
      }

      OwnedExternalBlock block;
      int ret = binding->allocate_external(resolved_spec, &block, err);
      if (ret != 0 || !block.raw_struct) {
        pool->DestroyBlocks();
        if (out_pool) *out_pool = nullptr;
        if (err && err->empty()) {
          *err = "Failed allocating external block " + std::to_string(i);
        }
        return ret != 0 ? ret : -4;
      }

      if (g_failure_stage.load() == FailureStage::kLedgerRegister &&
          static_cast<int>(i) == g_target_block_index.load()) {
        block.Destroy();
        pool->DestroyBlocks();
        if (out_pool) *out_pool = nullptr;
        if (err)
          *err =
              "Injected failure at ledger register block " + std::to_string(i);
        return -4;
      }

      void* raw = block.raw_struct;
      pool->block_states_[raw] = BlockState::kFree;
      pool->free_ring_[i] = raw;
      pool->all_blocks_.push_back(std::move(block));
    }

    pool->free_head_ = 0;
    pool->free_tail_ = 0;
    pool->free_count_ = effective_depth;
    pool->checked_out_count_ = 0;
    pool->closing_ = false;

    *out_pool = std::move(pool);
    return 0;
  } catch (const std::exception& e) {
    if (out_pool) *out_pool = nullptr;
    if (err)
      *err = "Allocation failure in OutputPoolState: " + std::string(e.what());
    return -4;
  } catch (...) {
    if (out_pool) *out_pool = nullptr;
    if (err) *err = "Unknown allocation failure in OutputPoolState";
    return -4;
  }
}

OutputPoolState::~OutputPoolState() {
  DestroyBlocks();
  RecordDestroyed();
}

int OutputPoolState::Acquire(void** out_block) {
  if (!out_block) return -2;
  *out_block = nullptr;

  std::unique_lock<std::mutex> lock(mutex_);
  available_.wait(lock, [this]() { return closing_ || free_count_ > 0; });

  if (closing_) {
    return -9;
  }

  if (free_count_ == 0 || free_ring_.empty()) {
    return -8;
  }

  void* block = free_ring_[free_head_];
  if (!block) {
    return -8;
  }

  auto it = block_states_.find(block);
  if (it == block_states_.end() || it->second != BlockState::kFree) {
    return -8;
  }

  // 严格先通过全部校验，才推进环形队列与修改状态账本
  free_head_ = (free_head_ + 1) % free_ring_.size();
  --free_count_;
  it->second = BlockState::kCheckedOut;
  ++checked_out_count_;
  *out_block = block;
  return 0;
}

void OutputPoolState::ReturnBlock(void* block) noexcept {
  if (!block) return;

  std::lock_guard<std::mutex> lock(mutex_);
  if (closing_) {
    return;
  }

  auto it = block_states_.find(block);
  if (it == block_states_.end()) {
    // 拒绝未知外部注入指针
    return;
  }

  if (it->second != BlockState::kCheckedOut) {
    // 拒绝重复归还
    return;
  }

  if (checked_out_count_ == 0) {
    // 防下溢保护
    return;
  }

  try {
    if (type_binding_ && type_binding_->reset_external) {
      type_binding_->reset_external(block, spec_);
    }
  } catch (...) {
  }

  it->second = BlockState::kFree;
  --checked_out_count_;

  if (free_count_ < free_ring_.size()) {
    free_ring_[free_tail_] = block;
    free_tail_ = (free_tail_ + 1) % free_ring_.size();
    ++free_count_;
  }

  available_.notify_one();
}

uint32_t OutputPoolState::CloseAndDrain() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  closing_ = true;
  available_.notify_all();
  return checked_out_count_;
}

void OutputPoolState::DestroyBlocks() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  closing_ = true;
  available_.notify_all();

  for (auto& block : all_blocks_) {
    try {
      if (type_binding_ && type_binding_->destroy_external) {
        type_binding_->destroy_external(&block);
      } else {
        block.Destroy();
      }
    } catch (...) {
    }
  }
  all_blocks_.clear();
  free_ring_.clear();
  free_head_ = 0;
  free_tail_ = 0;
  free_count_ = 0;
  block_states_.clear();
  checked_out_count_ = 0;
}

}  // namespace alg_framework
