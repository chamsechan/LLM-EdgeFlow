#include "adapter/platform/platform_output_pool.h"

#include <iostream>

namespace alg_framework {

void OutputPoolDeleter::operator()(void*) const noexcept {
  auto sp = weak_pool.lock();
  if (sp && !sp->IsClosing() && block) {
    sp->ReturnBlock(block);
  }
}

int OutputPoolState::Create(const std::string& suffix, uint32_t depth,
                            const ResolvedOutputPoolSpec& spec,
                            const PlatformValueTypeBinding* binding,
                            std::shared_ptr<OutputPoolState>* out_pool,
                            std::string* err) {
  if (!out_pool) {
    if (err) *err = "Null out_pool pointer";
    return -4;
  }
  if (!binding || !binding->allocate_external || !binding->reset_external ||
      !binding->destroy_external) {
    if (err)
      *err = "Output value type binding for suffix '" + suffix +
             "' is missing allocate/reset/destroy functions";
    return -4;
  }

  uint32_t effective_depth = depth > 0 ? depth : 25;

  auto pool = std::shared_ptr<OutputPoolState>(new OutputPoolState());
  pool->canonical_suffix_ = suffix;
  pool->depth_ = effective_depth;
  pool->spec_ = spec;
  pool->type_binding_ = binding;

  try {
    pool->all_blocks_.reserve(effective_depth);
    for (uint32_t i = 0; i < effective_depth; ++i) {
      OwnedExternalBlock block;
      int ret = binding->allocate_external(spec, &block, err);
      if (ret != 0 || !block.raw_struct) {
        if (err && err->empty()) {
          *err = "Failed to allocate external block for suffix " + suffix;
        }
        pool->DestroyBlocks();
        return -4;
      }
      binding->reset_external(block.raw_struct, spec);
      pool->free_blocks_.push(block.raw_struct);
      pool->all_blocks_.push_back(std::move(block));
    }
  } catch (const std::exception& e) {
    if (err)
      *err = std::string("Exception allocating output pool: ") + e.what();
    pool->DestroyBlocks();
    return -4;
  } catch (...) {
    if (err) *err = "Unknown exception allocating output pool";
    pool->DestroyBlocks();
    return -4;
  }

  *out_pool = std::move(pool);
  return 0;
}

OutputPoolState::~OutputPoolState() { DestroyBlocks(); }

int OutputPoolState::Acquire(void** out_block) {
  if (!out_block) return -4;
  std::unique_lock<std::mutex> lock(mutex_);
  available_.wait(lock, [this]() { return closing_ || !free_blocks_.empty(); });

  if (closing_ || free_blocks_.empty()) {
    *out_block = nullptr;
    return -4;
  }

  void* block = free_blocks_.front();
  free_blocks_.pop();
  ++checked_out_count_;
  *out_block = block;
  return 0;
}

void OutputPoolState::ReturnBlock(void* block) noexcept {
  if (!block) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (closing_) return;
  if (type_binding_ && type_binding_->reset_external) {
    try {
      type_binding_->reset_external(block, spec_);
    } catch (...) {
    }
  }
  if (checked_out_count_ > 0) {
    --checked_out_count_;
  }
  free_blocks_.push(block);
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
  while (!free_blocks_.empty()) {
    free_blocks_.pop();
  }
  if (type_binding_ && type_binding_->destroy_external) {
    for (auto& block : all_blocks_) {
      try {
        type_binding_->destroy_external(&block);
      } catch (...) {
      }
    }
  }
  all_blocks_.clear();
  checked_out_count_ = 0;
}

}  // namespace alg_framework
