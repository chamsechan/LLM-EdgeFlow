#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "adapter/platform/platform_output_pool.h"
#include "adapter/platform/platform_value_type_registry.h"

namespace alg_framework {

class PlatformOutputPoolTest : public ::testing::Test {
 protected:
  void SetUp() override {
    PlatformValueTypeRegistry::Instance().GlobalInit();
    PlatformValueTypeRegistry::SetAllocationFailureCountdown(-1);
    OutputPoolState::SetPublishFailureCountdown(-1);
  }

  void TearDown() override {
    PlatformValueTypeRegistry::SetAllocationFailureCountdown(-1);
    OutputPoolState::SetPublishFailureCountdown(-1);
  }
};

// 1. 深度 0 归一化为 25 且正常预分配，深度 > 1024 拦截
TEST_F(PlatformOutputPoolTest, DepthZeroNormalizedTo25AndMaxLimitChecked) {
  const auto* binding =
      PlatformValueTypeRegistry::Instance().GetBindingBySuffix("keyword_out");
  ASSERT_NE(binding, nullptr);

  ResolvedOutputPoolSpec spec;
  spec.type = "keyword_out";

  std::shared_ptr<OutputPoolState> pool;
  std::string err;
  int ret =
      OutputPoolState::Create("keyword_out", 0, spec, binding, &pool, &err);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(pool->Depth(), 25u);
  EXPECT_EQ(pool->FreeBlockCount(), 25u);
  EXPECT_EQ(pool->CheckedOutCount(), 0u);

  // 超过 1024 硬上限
  std::shared_ptr<OutputPoolState> pool_bad;
  ret = OutputPoolState::Create("keyword_out", 1025, spec, binding, &pool_bad,
                                &err);
  EXPECT_EQ(ret, -2);
  EXPECT_EQ(pool_bad, nullptr);
}

// 2. 状态账本防重复归还、防外部指针注入与无下溢
TEST_F(PlatformOutputPoolTest, LedgerRejectsDuplicateAndForeignBlocks) {
  const auto* binding =
      PlatformValueTypeRegistry::Instance().GetBindingBySuffix("keyword_out");
  ASSERT_NE(binding, nullptr);

  ResolvedOutputPoolSpec spec;
  spec.type = "keyword_out";

  std::shared_ptr<OutputPoolState> pool;
  std::string err;
  ASSERT_EQ(
      OutputPoolState::Create("keyword_out", 2, spec, binding, &pool, &err), 0);
  ASSERT_EQ(pool->FreeBlockCount(), 2u);

  void* block1 = nullptr;
  ASSERT_EQ(pool->Acquire(&block1), 0);
  ASSERT_NE(block1, nullptr);
  EXPECT_EQ(pool->FreeBlockCount(), 1u);
  EXPECT_EQ(pool->CheckedOutCount(), 1u);

  // 正常归还 1 次
  pool->ReturnBlock(block1);
  EXPECT_EQ(pool->FreeBlockCount(), 2u);
  EXPECT_EQ(pool->CheckedOutCount(), 0u);

  // 重复归还 block1 -> 应被账本拦截，不增加 free 数量，计数不下溢
  pool->ReturnBlock(block1);
  EXPECT_EQ(pool->FreeBlockCount(), 2u);
  EXPECT_EQ(pool->CheckedOutCount(), 0u);

  // 归还非本池分配的野指针 -> 应被账本拦截
  int dummy = 123;
  pool->ReturnBlock(&dummy);
  EXPECT_EQ(pool->FreeBlockCount(), 2u);
  EXPECT_EQ(pool->CheckedOutCount(), 0u);
}

// 3. 地址复用与 Reset 契约保留嵌套容量
TEST_F(PlatformOutputPoolTest, AddressReuseAndResetContract) {
  const auto* binding =
      PlatformValueTypeRegistry::Instance().GetBindingBySuffix("keyword_out");
  ASSERT_NE(binding, nullptr);

  ResolvedOutputPoolSpec spec;
  spec.type = "keyword_out";
  spec.capacities["match_result_json"] = 100;

  std::shared_ptr<OutputPoolState> pool;
  std::string err;
  ASSERT_EQ(
      OutputPoolState::Create("keyword_out", 1, spec, binding, &pool, &err), 0);

  void* b1 = nullptr;
  ASSERT_EQ(pool->Acquire(&b1), 0);
  auto* out1 = static_cast<CompanyPlatformKeywordOutput*>(b1);
  out1->request_id = 999;
  out1->is_hit = 1;
  std::strcpy(out1->match_result_json->data, "hello world");
  out1->match_result_json->length = 11;
  char* original_data_addr = out1->match_result_json->data;

  // 归还
  pool->ReturnBlock(b1);

  // 再次检出
  void* b2 = nullptr;
  ASSERT_EQ(pool->Acquire(&b2), 0);
  EXPECT_EQ(b1, b2);
  auto* out2 = static_cast<CompanyPlatformKeywordOutput*>(b2);

  // 验证 Reset 契约：内容重置为初始，但物理地址和容量完好保留
  EXPECT_EQ(out2->request_id, 0u);
  EXPECT_EQ(out2->is_hit, 0);
  EXPECT_EQ(out2->match_result_json->length, 0);
  EXPECT_EQ(out2->match_result_json->data[0], '\0');
  EXPECT_EQ(out2->match_result_json->data, original_data_addr);

  pool->ReturnBlock(b2);
}

// 4. ScopedOutputLeaseGuard 的 Untrack 与 Rollback 事务边界
TEST_F(PlatformOutputPoolTest, LeaseGuardTransactionRollback) {
  const auto* binding =
      PlatformValueTypeRegistry::Instance().GetBindingBySuffix("keyword_out");
  ASSERT_NE(binding, nullptr);

  ResolvedOutputPoolSpec spec;
  spec.type = "keyword_out";

  std::shared_ptr<OutputPoolState> pool;
  std::string err;
  ASSERT_EQ(
      OutputPoolState::Create("keyword_out", 3, spec, binding, &pool, &err), 0);
  EXPECT_EQ(pool->FreeBlockCount(), 3u);

  {
    ScopedOutputLeaseGuard guard;
    void *b1 = nullptr, *b2 = nullptr, *b3 = nullptr;
    ASSERT_EQ(pool->Acquire(&b1), 0);
    ASSERT_EQ(pool->Acquire(&b2), 0);
    ASSERT_EQ(pool->Acquire(&b3), 0);
    EXPECT_EQ(pool->FreeBlockCount(), 0u);

    guard.Track(pool, b1);
    guard.Track(pool, b2);
    guard.Track(pool, b3);

    // 模拟构造 shared_ptr 时将 b1 移交给外部智能指针 (Untrack)
    guard.Untrack(b1);

    // guard 析构触发 Rollback，b2 和 b3 应自动归还，b1 仍由模拟外部持有
  }

  EXPECT_EQ(pool->FreeBlockCount(), 2u);
  EXPECT_EQ(pool->CheckedOutCount(), 1u);
}

// 5. 确定性分配失败注入与全量回滚零泄漏测试 (R9-006)
TEST_F(PlatformOutputPoolTest, AllocatorFailureRollbackZeroLeak) {
  const auto* binding =
      PlatformValueTypeRegistry::Instance().GetBindingBySuffix("od_out");
  ASSERT_NE(binding, nullptr);

  ResolvedOutputPoolSpec spec;
  spec.type = "od_out";
  spec.meta_num = 10;
  spec.metadata_type_id = 1;  // float32
  spec.capacities["result_json"] = 512;

  // 针对 od_out（包含 1 个 root + 1 个 char buf + 1 个 cs + 1 个 meta buf + 1
  // 个 meta struct） 逐个注入分配失败探针
  for (int fail_step = 0; fail_step <= 5; ++fail_step) {
    PlatformValueTypeRegistry::SetAllocationFailureCountdown(fail_step);
    std::shared_ptr<OutputPoolState> pool;
    std::string err;
    int ret = OutputPoolState::Create("od_out", 3, spec, binding, &pool, &err);
    EXPECT_NE(ret, 0) << "Failed at step: " << fail_step;
    EXPECT_EQ(pool, nullptr);
  }
}

// 6. 多线程并发归还与条件变量唤醒
TEST_F(PlatformOutputPoolTest, ConcurrentAcquireReturnAndWakeup) {
  const auto* binding =
      PlatformValueTypeRegistry::Instance().GetBindingBySuffix("keyword_out");
  ASSERT_NE(binding, nullptr);

  ResolvedOutputPoolSpec spec;
  spec.type = "keyword_out";

  std::shared_ptr<OutputPoolState> pool;
  std::string err;
  ASSERT_EQ(
      OutputPoolState::Create("keyword_out", 1, spec, binding, &pool, &err), 0);

  void* block = nullptr;
  ASSERT_EQ(pool->Acquire(&block), 0);
  EXPECT_EQ(pool->FreeBlockCount(), 0u);

  std::atomic<bool> worker_done{false};
  std::thread worker([&]() {
    void* acquired = nullptr;
    // 阻塞等待唤醒
    if (pool->Acquire(&acquired) == 0 && acquired != nullptr) {
      pool->ReturnBlock(acquired);
      worker_done = true;
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  EXPECT_FALSE(worker_done);

  // 归还主线程块，触发唤醒
  pool->ReturnBlock(block);

  worker.join();
  EXPECT_TRUE(worker_done);
  EXPECT_EQ(pool->FreeBlockCount(), 1u);
  EXPECT_EQ(pool->CheckedOutCount(), 0u);
}

}  // namespace alg_framework
