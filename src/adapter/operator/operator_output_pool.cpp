#include "adapter/operator/operator_output_pool.h"

#include "contracts/diagnostic.h"

namespace llm_edgeflow {

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
  try {
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

    auto pool = std::shared_ptr<OutputPoolState>(new OutputPoolState());
    pool->depth_ = effective_depth;
    pool->spec_ = resolved_spec;
    pool->type_binding_ = *binding;

    pool->all_blocks_.reserve(effective_depth);
    pool->free_ring_.resize(effective_depth, nullptr);

    pool->block_states_.reserve(effective_depth);

    for (uint32_t i = 0; i < effective_depth; ++i) {
      OwnedExternalBlock block;
      int ret =
          pool->type_binding_.allocate_external(resolved_spec, &block, err);
      if (ret != 0 || !block.raw_struct) {
        pool->DestroyBlocks();
        if (out_pool) *out_pool = nullptr;
        if (err && err->empty()) {
          *err = "Failed allocating external block " + std::to_string(i);
        }
        return ret != 0 ? ret : -4;
      }

      void* raw = block.raw_struct;
      pool->block_states_[raw] = BlockState::kFree;
      pool->free_ring_[i] = raw;
      pool->all_blocks_.push_back(std::move(block));
    }

    pool->free_head_ = 0;
    pool->free_count_ = effective_depth;
    pool->checked_out_count_ = 0;
    pool->closing_ = false;

    *out_pool = std::move(pool);
    return 0;
  } catch (const std::exception& e) {
    if (out_pool) *out_pool = nullptr;
    SetDiagnosticNoexcept(err, e.what());
    return -4;
  } catch (...) {
    if (out_pool) *out_pool = nullptr;
    SetDiagnosticNoexcept(err, "Unknown exception");
    return -4;
  }
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
    type_binding_.reset_external(block, spec_);
  } catch (...) {
  }

  it->second = BlockState::kFree;
  --checked_out_count_;

  if (free_count_ < free_ring_.size()) {
    const size_t tail = (free_head_ + free_count_) % free_ring_.size();
    free_ring_[tail] = block;
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
      type_binding_.destroy_external(&block);
    } catch (...) {
    }
  }
  all_blocks_.clear();
  free_ring_.clear();
  free_head_ = 0;
  free_count_ = 0;
  block_states_.clear();
  checked_out_count_ = 0;
}

int AcquireRuntimeOutputBlocks(
    const RuntimeOutputBatch& outputs,
    const std::vector<std::shared_ptr<OutputPoolState>>& output_pools,
    ScopedOutputLeaseGuard* lease_guard,
    std::vector<AcquiredOutputBlock>* acquired_blocks, std::string* error) {
  if (!lease_guard || !acquired_blocks) {
    if (error) *error = "Null destination in AcquireRuntimeOutputBlocks";
    return -4;
  }
  acquired_blocks->clear();

  size_t total_slots = 0;
  for (const auto& fb : outputs.rows) {
    total_slots += fb.size();
  }
  lease_guard->Reserve(total_slots);
  acquired_blocks->reserve(total_slots);

  for (size_t f = 0; f < outputs.rows.size(); ++f) {
    for (size_t slot = 0; slot < outputs.rows[f].size(); ++slot) {
      const auto& binding = outputs.rows[f][slot];
      if (binding.output_index >= output_pools.size() ||
          !output_pools[binding.output_index]) {
        if (error) {
          *error = "Missing output pool for slot " +
                   std::to_string(binding.output_index);
        }
        return -5;
      }
      auto pool = output_pools[binding.output_index];
      void* block = nullptr;
      int acq_ret = pool->Acquire(&block);
      if (acq_ret != 0 || !block) {
        if (error) {
          *error = "Output pool exhausted for slot " +
                   std::to_string(binding.output_index);
        }
        return -4;
      }
      lease_guard->Track(pool, block);
      acquired_blocks->push_back({f, slot, pool, block, binding.output_index});
    }
  }

  return 0;
}

void PublishRuntimeOutputs(
    const std::vector<AcquiredOutputBlock>& acquired_blocks,
    RuntimeOutputBatch* outputs, ScopedOutputLeaseGuard* lease_guard) {
  if (!outputs || !lease_guard) return;
  RuntimeOutputBatch staged = *outputs;
  for (const auto& acquired : acquired_blocks) {
    // shared_ptr 控制块分配失败时会调用删除器。须先解除守卫对此块的管理，
    // 避免同一内存块被重复归还。
    lease_guard->Untrack(acquired.raw_block);
    std::shared_ptr<void> value(
        acquired.raw_block,
        OutputPoolDeleter{acquired.pool, acquired.raw_block});
    staged.rows.at(acquired.frame_idx).at(acquired.slot_idx).value =
        std::move(value);
  }
  // 仅向调用方已选槽位提交值，行和槽位的存储仍由调用方持有；
  // 所有暂存容器在此释放。
  for (size_t row = 0; row < outputs->rows.size(); ++row)
    for (size_t slot = 0; slot < outputs->rows[row].size(); ++slot)
      outputs->rows[row][slot].value = std::move(staged.rows[row][slot].value);
  lease_guard->Commit();
}

}  // namespace llm_edgeflow
