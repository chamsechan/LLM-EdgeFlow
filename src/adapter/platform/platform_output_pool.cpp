#include "adapter/platform/platform_output_pool.h"

#include <atomic>
#include <iostream>

namespace alg_framework {

namespace {

std::atomic<int> g_publish_failure_countdown{-1};

}  // namespace

void OutputPoolState::SetPublishFailureCountdown(int count) noexcept {
  g_publish_failure_countdown.store(count);
}

int OutputPoolState::GetPublishFailureCountdown() noexcept {
  return g_publish_failure_countdown.load();
}

void OutputPoolDeleter::operator()(void*) const noexcept {
  if (auto pool = weak_pool.lock()) {
    pool->ReturnBlock(block);
  }
}

int OutputPoolState::Create(const std::string& suffix, uint32_t depth,
                            const ResolvedOutputPoolSpec& spec,
                            const PlatformValueTypeBinding* binding,
                            std::shared_ptr<OutputPoolState>* out_pool,
                            std::string* err) {
  if (!out_pool) {
    if (err) *err = "Null output pool pointer";
    return -2;
  }
  if (!binding || !binding->allocate_external || !binding->reset_external ||
      !binding->destroy_external) {
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

  auto pool = std::shared_ptr<OutputPoolState>(new OutputPoolState());
  pool->canonical_suffix_ = suffix;
  pool->depth_ = effective_depth;
  pool->spec_ = spec;
  pool->type_binding_ = binding;

  // 预留空间与固定环形队列
  pool->all_blocks_.reserve(effective_depth);
  pool->free_ring_.resize(effective_depth, nullptr);
  pool->block_states_.reserve(effective_depth);

  try {
    for (uint32_t i = 0; i < effective_depth; ++i) {
      OwnedExternalBlock block;
      int ret = binding->allocate_external(spec, &block, err);
      if (ret != 0 || !block.raw_struct) {
        pool->DestroyBlocks();
        return ret != 0 ? ret : -4;
      }
      void* raw = block.raw_struct;
      pool->block_states_[raw] = BlockState::kFree;
      pool->free_ring_[i] = raw;
      pool->all_blocks_.push_back(std::move(block));
    }
  } catch (const std::exception& e) {
    pool->DestroyBlocks();
    if (err)
      *err = "Allocation failure in OutputPoolState: " + std::string(e.what());
    return -4;
  } catch (...) {
    pool->DestroyBlocks();
    if (err) *err = "Unknown allocation failure in OutputPoolState";
    return -4;
  }

  pool->free_head_ = 0;
  pool->free_tail_ = 0;
  pool->free_count_ = effective_depth;
  pool->checked_out_count_ = 0;
  pool->closing_ = false;

  *out_pool = std::move(pool);
  return 0;
}

OutputPoolState::~OutputPoolState() { DestroyBlocks(); }

int OutputPoolState::Acquire(void** out_block) {
  if (!out_block) return -2;
  *out_block = nullptr;

  std::unique_lock<std::mutex> lock(mutex_);
  available_.wait(lock, [this]() { return closing_ || free_count_ > 0; });

  if (closing_) {
    return -9;
  }

  if (free_count_ == 0) {
    return -8;
  }

  void* block = free_ring_[free_head_];
  free_head_ = (free_head_ + 1) % free_ring_.size();
  --free_count_;

  auto it = block_states_.find(block);
  if (it == block_states_.end() || it->second != BlockState::kFree) {
    return -8;
  }

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
    block.Destroy();
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
