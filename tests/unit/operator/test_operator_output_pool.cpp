#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "adapter/operator/operator_output_pool.h"
#include "tests/support/scoped_allocation_failure.h"

namespace llm_edgeflow {

struct OutputPoolTestAccess {
  static uint32_t WaitingAcquirers(const OutputPoolState& pool) {
    std::lock_guard<std::mutex> lock(pool.mutex_);
    return pool.waiting_acquirers_;
  }
};

namespace {

bool AwaitWaitingAcquirer(const OutputPoolState& pool) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    if (OutputPoolTestAccess::WaitingAcquirers(pool) == 1) return true;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

struct AcquireResult {
  int code;
  void* block;
};

// A notification regression must fail a bounded check without leaving a joined
// test thread blocked forever. The fallback DestroyBlocks also notifies
// waiters.
bool FinishAcquirer(std::thread* worker, std::future<AcquireResult>* result,
                    const std::shared_ptr<OutputPoolState>& pool) {
  const bool ready =
      result->wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  if (!ready) pool->DestroyBlocks();
  const bool recovered = ready || result->wait_for(std::chrono::seconds(5)) ==
                                      std::future_status::ready;
  if (recovered)
    worker->join();
  else
    worker->detach();  // The thread owns its promise and pool until it exits.
  return ready;
}

struct AllocationTrace {
  std::atomic<int> allocations{0};
  std::atomic<int> root_live{0};
  std::atomic<int> nested_live{0};
  std::atomic<int> roots_destroyed{0};
  std::atomic<int> nested_destroyed{0};
  std::atomic<int> destroy_callbacks{0};
  std::atomic<int> resets{0};
  int fail_at = -1;
  bool reset_throws = false;
};

struct RootValue {
  explicit RootValue(AllocationTrace* trace) : trace(trace) {
    ++trace->root_live;
  }
  ~RootValue() {
    --trace->root_live;
    ++trace->roots_destroyed;
  }
  AllocationTrace* trace;
  int content = 0;
};

struct NestedValue {
  explicit NestedValue(AllocationTrace* trace) : trace(trace) {
    ++trace->nested_live;
  }
  ~NestedValue() {
    --trace->nested_live;
    ++trace->nested_destroyed;
  }
  AllocationTrace* trace;
};

OperatorValueTypeBinding TraceBinding(
    const std::shared_ptr<AllocationTrace>& trace) {
  OperatorValueTypeBinding binding;
  binding.canonical_suffix = "pool_contract";
  binding.external_c_type_name = "RootValue";
  binding.direction = IoDirection::kOutput;
  binding.output_layout.compute_block_payload_bytes =
      [](const ResolvedOutputPoolSpec&, size_t* bytes, std::string*) {
        *bytes = sizeof(RootValue) + sizeof(NestedValue);
        return true;
      };
  binding.allocate_external = [trace](const ResolvedOutputPoolSpec&,
                                      OwnedExternalBlock* block,
                                      std::string* error) {
    const int index = trace->allocations++;
    block->raw_struct = block->Own(std::make_unique<RootValue>(trace.get()));
    block->Own(std::make_unique<NestedValue>(trace.get()));
    if (index == trace->fail_at) {
      *error = "native allocator rejected completed nested block";
      return -71;
    }
    return 0;
  };
  binding.reset_external = [trace](void* raw, const ResolvedOutputPoolSpec&) {
    ++trace->resets;
    if (trace->reset_throws) throw std::runtime_error("native reset failure");
    static_cast<RootValue*>(raw)->content = 0;
  };
  binding.destroy_external = [trace](OwnedExternalBlock* block) {
    ++trace->destroy_callbacks;
    block->Destroy();
  };
  return binding;
}

class OperatorOutputPoolTest : public ::testing::Test {
 protected:
  void SetUp() override {
    trace = std::make_shared<AllocationTrace>();
    binding = TraceBinding(trace);
    spec.type = "pool_contract";
  }
  void Create(uint32_t depth) {
    std::string diagnostic;
    ASSERT_EQ(OutputPoolState::Create("pool_contract", depth, spec, &binding,
                                      &pool, &diagnostic),
              0)
        << diagnostic;
    ASSERT_NE(pool, nullptr);
  }
  std::shared_ptr<AllocationTrace> trace;
  OperatorValueTypeBinding binding;
  ResolvedOutputPoolSpec spec;
  std::shared_ptr<OutputPoolState> pool;
};

TEST_F(OperatorOutputPoolTest,
       PreallocationCreatesAllBlocksBeforeFirstAcquire) {
  Create(2);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(trace->allocations, 2);
  EXPECT_EQ(trace->root_live, 2);
  EXPECT_EQ(trace->nested_live, 2);
  EXPECT_EQ(pool->FreeBlockCount(), 2U);
  void* first = nullptr;
  void* second = nullptr;
  ASSERT_EQ(pool->Acquire(&first), 0);
  ASSERT_EQ(pool->Acquire(&second), 0);
  EXPECT_NE(first, second);
  EXPECT_EQ(trace->allocations, 2);
  EXPECT_EQ(pool->CheckedOutCount(), 2U);
  EXPECT_EQ(pool->FreeBlockCount(), 0U);
  pool->ReturnBlock(first);
  pool->ReturnBlock(second);
  EXPECT_EQ(pool->CheckedOutCount(), 0U);
  EXPECT_EQ(pool->FreeBlockCount(), 2U);
  pool.reset();
  EXPECT_EQ(trace->roots_destroyed, 2);
  EXPECT_EQ(trace->nested_destroyed, 2);
}

TEST_F(OperatorOutputPoolTest,
       PartialNativeAllocationFailureCleansPrefixAndCurrentBlock) {
  trace->fail_at = 1;
  std::string diagnostic;
  EXPECT_EQ(OutputPoolState::Create("pool_contract", 3, spec, &binding, &pool,
                                    &diagnostic),
            -71);
  EXPECT_EQ(pool, nullptr);
  EXPECT_EQ(diagnostic, "native allocator rejected completed nested block");
  EXPECT_EQ(trace->allocations, 2);
  EXPECT_EQ(trace->root_live, 0);
  EXPECT_EQ(trace->nested_live, 0);
  EXPECT_EQ(trace->roots_destroyed, 2);
  EXPECT_EQ(trace->nested_destroyed, 2);
  EXPECT_EQ(trace->destroy_callbacks, 1);
}

TEST_F(OperatorOutputPoolTest,
       NativeAllocationExceptionAlsoCleansCompletedPrefix) {
  auto ordinary_allocate = binding.allocate_external;
  bool injected = false;
  binding.allocate_external =
      [&, ordinary_allocate](const ResolvedOutputPoolSpec& requested,
                             OwnedExternalBlock* block, std::string* error) {
        if (trace->allocations == 1) {
          test_support::ScopedAllocationFailure failure(0);
          try {
            auto root = std::make_unique<RootValue>(trace.get());
            block->raw_struct = block->Own(std::move(root));
          } catch (const std::bad_alloc&) {
            injected = failure.Triggered();
            throw;
          }
        }
        return ordinary_allocate(requested, block, error);
      };
  std::string diagnostic;
  EXPECT_EQ(OutputPoolState::Create("pool_contract", 3, spec, &binding, &pool,
                                    &diagnostic),
            -4);
  EXPECT_TRUE(injected);
  EXPECT_EQ(pool, nullptr);
  EXPECT_FALSE(diagnostic.empty());
  EXPECT_EQ(trace->root_live, 0);
  EXPECT_EQ(trace->nested_live, 0);
  EXPECT_EQ(trace->roots_destroyed, 1);
  EXPECT_EQ(trace->nested_destroyed, 1);
}

TEST_F(OperatorOutputPoolTest, EmptyPoolWaitsUntilActualBlockReturn) {
  Create(1);
  ASSERT_NE(pool, nullptr);
  void* held = nullptr;
  ASSERT_EQ(pool->Acquire(&held), 0);
  auto promise = std::make_shared<std::promise<AcquireResult>>();
  auto result = promise->get_future();
  std::thread worker([owner = pool, promise] {
    void* block = nullptr;
    const int code = owner->Acquire(&block);
    promise->set_value({code, block});
  });
  const bool entered_wait = AwaitWaitingAcquirer(*pool);
  if (entered_wait)
    pool->ReturnBlock(held);
  else
    pool->CloseAndDrain();
  const bool ready = FinishAcquirer(&worker, &result, pool);
  ASSERT_TRUE(entered_wait);
  ASSERT_TRUE(ready);
  const auto acquired = result.get();
  EXPECT_EQ(acquired.code, 0);
  EXPECT_EQ(acquired.block, held);
  EXPECT_EQ(pool->CheckedOutCount(), 1U);
  EXPECT_EQ(pool->FreeBlockCount(), 0U);
  EXPECT_EQ(trace->allocations, 1);
  pool->ReturnBlock(acquired.block);
  EXPECT_EQ(trace->resets, 2);
}

TEST_F(OperatorOutputPoolTest,
       CloseWakesAnObservedWaitingAcquireWithClosedCode) {
  Create(1);
  ASSERT_NE(pool, nullptr);
  void* held = nullptr;
  ASSERT_EQ(pool->Acquire(&held), 0);
  auto promise = std::make_shared<std::promise<AcquireResult>>();
  auto result = promise->get_future();
  std::thread worker([owner = pool, promise] {
    void* block = nullptr;
    const int code = owner->Acquire(&block);
    promise->set_value({code, block});
  });
  const bool entered_wait = AwaitWaitingAcquirer(*pool);
  const uint32_t outstanding = pool->CloseAndDrain();
  const bool ready = FinishAcquirer(&worker, &result, pool);
  ASSERT_TRUE(entered_wait);
  ASSERT_TRUE(ready);
  const auto acquired = result.get();
  EXPECT_EQ(outstanding, 1U);
  EXPECT_EQ(acquired.code, -9);
  EXPECT_EQ(acquired.block, nullptr);
  EXPECT_EQ(OutputPoolTestAccess::WaitingAcquirers(*pool), 0U);
  pool->DestroyBlocks();
  EXPECT_EQ(trace->roots_destroyed, 1);
}

TEST_F(OperatorOutputPoolTest, ReturnRejectsForeignNullAndDuplicateBlocks) {
  Create(1);
  ASSERT_NE(pool, nullptr);
  void* block = nullptr;
  ASSERT_EQ(pool->Acquire(&block), 0);
  int foreign = 0;
  pool->ReturnBlock(&foreign);
  pool->ReturnBlock(nullptr);
  EXPECT_EQ(pool->CheckedOutCount(), 1U);
  EXPECT_EQ(pool->FreeBlockCount(), 0U);
  EXPECT_EQ(trace->resets, 0);
  static_cast<RootValue*>(block)->content = 7;
  pool->ReturnBlock(block);
  EXPECT_EQ(trace->resets, 1);
  EXPECT_EQ(static_cast<RootValue*>(block)->content, 0);
  pool->ReturnBlock(block);
  EXPECT_EQ(trace->resets, 1);
  EXPECT_EQ(pool->CheckedOutCount(), 0U);
  EXPECT_EQ(pool->FreeBlockCount(), 1U);
}

TEST_F(OperatorOutputPoolTest, ResetExceptionStillReturnsReusableBlock) {
  Create(1);
  ASSERT_NE(pool, nullptr);
  void* original = nullptr;
  ASSERT_EQ(pool->Acquire(&original), 0);
  trace->reset_throws = true;
  pool->ReturnBlock(original);
  EXPECT_EQ(trace->resets, 1);
  EXPECT_EQ(pool->FreeBlockCount(), 1U);
  EXPECT_EQ(pool->CheckedOutCount(), 0U);
  void* reused = nullptr;
  ASSERT_EQ(pool->Acquire(&reused), 0);
  EXPECT_EQ(reused, original);
  trace->reset_throws = false;
  pool->ReturnBlock(reused);
  EXPECT_EQ(trace->resets, 2);
}

TEST_F(OperatorOutputPoolTest, LastSharedReferenceReturnsBlockExactlyOnce) {
  Create(1);
  ASSERT_NE(pool, nullptr);
  void* block = nullptr;
  ASSERT_EQ(pool->Acquire(&block), 0);
  std::shared_ptr<void> lease(block, OutputPoolDeleter{pool, block});
  auto copy = lease;
  lease.reset();
  EXPECT_EQ(trace->resets, 0);
  EXPECT_EQ(pool->CheckedOutCount(), 1U);
  copy.reset();
  EXPECT_EQ(trace->resets, 1);
  EXPECT_EQ(pool->CheckedOutCount(), 0U);
  EXPECT_EQ(pool->FreeBlockCount(), 1U);
}

TEST_F(OperatorOutputPoolTest,
       LeaseWeakReferenceDoesNotKeepPoolOrPayloadAlive) {
  Create(1);
  ASSERT_NE(pool, nullptr);
  void* block = nullptr;
  ASSERT_EQ(pool->Acquire(&block), 0);
  std::shared_ptr<void> lease(block, OutputPoolDeleter{pool, block});
  std::weak_ptr<OutputPoolState> observed_pool = pool;
  pool.reset();
  EXPECT_TRUE(observed_pool.expired());
  EXPECT_EQ(trace->root_live, 0);
  EXPECT_EQ(trace->nested_live, 0);
  EXPECT_EQ(trace->roots_destroyed, 1);
  // The lease pointer is intentionally not dereferenced after pool destruction.
  lease.reset();
  EXPECT_EQ(trace->resets, 0);
  EXPECT_EQ(trace->roots_destroyed, 1);
}

TEST_F(OperatorOutputPoolTest,
       CloseReportsOutstandingAndDestroyConsumesBlocks) {
  Create(2);
  ASSERT_NE(pool, nullptr);
  void* outstanding = nullptr;
  ASSERT_EQ(pool->Acquire(&outstanding), 0);
  EXPECT_EQ(pool->CloseAndDrain(), 1U);
  void* refused = outstanding;
  EXPECT_EQ(pool->Acquire(&refused), -9);
  EXPECT_EQ(refused, nullptr);
  pool->ReturnBlock(outstanding);
  EXPECT_EQ(trace->resets, 0);
  EXPECT_EQ(pool->CheckedOutCount(), 1U);
  pool->DestroyBlocks();
  EXPECT_EQ(trace->root_live, 0);
  EXPECT_EQ(trace->nested_live, 0);
  EXPECT_EQ(pool->CheckedOutCount(), 0U);
  EXPECT_EQ(pool->CloseAndDrain(), 0U);
  pool.reset();
  EXPECT_EQ(trace->roots_destroyed, 2);
  EXPECT_EQ(trace->nested_destroyed, 2);
}

TEST_F(OperatorOutputPoolTest,
       GuardRollbackReturnsTrackedBlocksAndIsIdempotent) {
  Create(2);
  ASSERT_NE(pool, nullptr);
  void* first = nullptr;
  void* second = nullptr;
  ASSERT_EQ(pool->Acquire(&first), 0);
  ASSERT_EQ(pool->Acquire(&second), 0);
  {
    ScopedOutputLeaseGuard guard;
    guard.Reserve(2);
    guard.Track(pool, first);
    guard.Track(pool, second);
    guard.Rollback();
    EXPECT_EQ(pool->CheckedOutCount(), 0U);
    EXPECT_EQ(trace->resets, 2);
    guard.Rollback();
  }
  EXPECT_EQ(trace->resets, 2);
  EXPECT_EQ(pool->FreeBlockCount(), 2U);
}

TEST_F(OperatorOutputPoolTest,
       GuardCommitTransfersResponsibilityToSharedLease) {
  Create(1);
  ASSERT_NE(pool, nullptr);
  void* block = nullptr;
  ASSERT_EQ(pool->Acquire(&block), 0);
  std::shared_ptr<void> lease;
  {
    ScopedOutputLeaseGuard guard;
    guard.Track(pool, block);
    lease = std::shared_ptr<void>(block, OutputPoolDeleter{pool, block});
    guard.Commit();
  }
  EXPECT_EQ(trace->resets, 0);
  EXPECT_EQ(pool->CheckedOutCount(), 1U);
  lease.reset();
  EXPECT_EQ(trace->resets, 1);
}

TEST_F(OperatorOutputPoolTest,
       TrackAllocationFailureReturnsNewlyAcquiredBlock) {
  Create(1);
  ASSERT_NE(pool, nullptr);
  void* block = nullptr;
  ASSERT_EQ(pool->Acquire(&block), 0);
  ScopedOutputLeaseGuard guard;
  bool threw = false;
  bool injected = false;
  {
    test_support::ScopedAllocationFailure failure(0);
    try {
      guard.Track(pool, block);
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    injected = failure.Triggered();
  }
  EXPECT_TRUE(injected);
  EXPECT_TRUE(threw);
  EXPECT_EQ(trace->resets, 1);
  EXPECT_EQ(pool->CheckedOutCount(), 0U);
  EXPECT_EQ(pool->FreeBlockCount(), 1U);
  guard.Rollback();
  EXPECT_EQ(trace->resets, 1);
}

TEST_F(OperatorOutputPoolTest,
       SharedLeaseConstructorFailureReturnsBlockWithoutCommit) {
  Create(1);
  ASSERT_NE(pool, nullptr);
  void* block = nullptr;
  ASSERT_EQ(pool->Acquire(&block), 0);
  ScopedOutputLeaseGuard guard;
  guard.Track(pool, block);
  guard.Untrack(block);
  bool threw = false;
  bool committed = false;
  bool injected = false;
  {
    test_support::ScopedAllocationFailure failure(0);
    try {
      std::shared_ptr<void> lease(block, OutputPoolDeleter{pool, block});
      guard.Commit();
      committed = true;
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    injected = failure.Triggered();
  }
  EXPECT_TRUE(injected);
  EXPECT_TRUE(threw);
  EXPECT_FALSE(committed);
  EXPECT_EQ(trace->resets, 1);
  EXPECT_EQ(pool->CheckedOutCount(), 0U);
  EXPECT_EQ(pool->FreeBlockCount(), 1U);
  guard.Rollback();
  EXPECT_EQ(trace->resets, 1);
}

}  // namespace
}  // namespace llm_edgeflow
