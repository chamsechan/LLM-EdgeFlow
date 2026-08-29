#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/alg_context.h"
#include "core/blackboard_key.h"
#include "core/common_contracts.h"
#include "core/traceable_item.h"

namespace alg_framework {

class TypedBlackboardContractsTest : public ::testing::Test {};

// 1. 验证强类型 BlackboardKey 的 Set/Get/Has/Erase 操作与类型匹配
TEST_F(TypedBlackboardContractsTest, TypedKeyOperations) {
  constexpr BlackboardKey<std::vector<std::string>> kTestStrings{"test_strings",
                                                                 "string[]"};
  constexpr BlackboardKey<int> kTestInt{"test_int", "int"};

  AlgContext ctx;
  EXPECT_FALSE(ctx.Has(kTestStrings));
  EXPECT_EQ(ctx.Get(kTestStrings), nullptr);

  std::vector<std::string> sample_data = {"alpha", "beta", "gamma"};
  ctx.Set(kTestStrings, sample_data);
  ctx.Set(kTestInt, 42);

  EXPECT_TRUE(ctx.Has(kTestStrings));
  EXPECT_TRUE(ctx.Has(kTestInt));

  auto* retrieved_strings = ctx.Get(kTestStrings);
  ASSERT_NE(retrieved_strings, nullptr);
  EXPECT_EQ(*retrieved_strings, sample_data);

  auto* retrieved_int = ctx.Get(kTestInt);
  ASSERT_NE(retrieved_int, nullptr);
  EXPECT_EQ(*retrieved_int, 42);

  // 删除操作
  ctx.Erase(kTestInt);
  EXPECT_FALSE(ctx.Has(kTestInt));
  EXPECT_EQ(ctx.Get(kTestInt), nullptr);
  EXPECT_TRUE(ctx.Has(kTestStrings));
}

// 2. 验证类型不匹配时安全返回 nullptr
TEST_F(TypedBlackboardContractsTest, TypeMismatchReturnsNullptr) {
  constexpr BlackboardKey<std::string> kStringKey{"poly_key", "string"};
  constexpr BlackboardKey<int> kIntKey{"poly_key", "int"};

  AlgContext ctx;
  ctx.Set(kStringKey, std::string("hello world"));

  EXPECT_TRUE(ctx.Has(kStringKey));
  EXPECT_NE(ctx.Get(kStringKey), nullptr);

  // 尝试用不同类型读取同名 key，应安全返回 nullptr
  auto* int_view = ctx.Get(kIntKey);
  EXPECT_EQ(int_view, nullptr);
}

// 3. 验证 CommonContracts 定义的公共 Key 的读写与 TraceableItem 支持
TEST_F(TypedBlackboardContractsTest, CommonContractsAndTraceableProvenance) {
  AlgContext ctx;

  std::vector<uint64_t> raw_req_ids = {1001, 1002};
  TextBatch queries = {TraceableItem<std::string>{1001, 0, "query 1"},
                       TraceableItem<std::string>{1002, 0, "query 2"}};
  ctx.Set(kRawRequestIds, raw_req_ids);
  ctx.Set(kRawQueries, queries);

  EXPECT_TRUE(ctx.Has(kRawRequestIds));
  EXPECT_TRUE(ctx.Has(kRawQueries));

  // TraceableItem 批处理样本可追溯性
  std::vector<TraceableItem<std::string>> prompts = {
      TraceableItem<std::string>{1001, 0, "Prompt for 1001-0"},
      TraceableItem<std::string>{1002, 0, "Prompt for 1002-0"}};
  ctx.Set(kLlmInputPrompts, prompts);

  auto* retrieved_prompts = ctx.Get(kLlmInputPrompts);
  ASSERT_NE(retrieved_prompts, nullptr);
  ASSERT_EQ(retrieved_prompts->size(), 2U);
  EXPECT_EQ((*retrieved_prompts)[0].req_id, 1001U);
  EXPECT_EQ((*retrieved_prompts)[0].sub_id, 0U);
  EXPECT_EQ((*retrieved_prompts)[0].data, "Prompt for 1001-0");
  EXPECT_EQ((*retrieved_prompts)[1].req_id, 1002U);
  EXPECT_EQ((*retrieved_prompts)[1].sub_id, 0U);
  EXPECT_EQ((*retrieved_prompts)[1].data, "Prompt for 1002-0");
}

// 4. 验证 AlgContext 状态重置与 Clear 生命周期
TEST_F(TypedBlackboardContractsTest, ContextClearAndResetSemantics) {
  constexpr BlackboardKey<std::string> kKey{"greeting", "string"};

  AlgContext ctx;
  ctx.Set(kKey, std::string("welcome"));
  ctx.SetError(-500, "sample error");

  EXPECT_TRUE(ctx.Has(kKey));
  EXPECT_FALSE(ctx.IsOk());
  EXPECT_EQ(ctx.GetErrorCode(), -500);
  EXPECT_EQ(ctx.GetErrorMessage(), "sample error");

  ctx.Clear();
  EXPECT_FALSE(ctx.Has(kKey));
  EXPECT_EQ(ctx.Get(kKey), nullptr);
  EXPECT_TRUE(ctx.IsOk());
  EXPECT_EQ(ctx.GetErrorCode(), 0);
}

// 5. 验证单次发布拒绝静默覆盖，兼容 Get 只返回只读视图
TEST_F(TypedBlackboardContractsTest, PublishIsWriteOnceAndGetIsReadOnly) {
  constexpr BlackboardKey<int> kCount{"count", "int"};
  static_assert(
      std::is_same_v<decltype(std::declval<AlgContext&>().Get(kCount)),
                     const int*>);

  AlgContext ctx;
  EXPECT_TRUE(ctx.Publish(kCount, 1));
  EXPECT_FALSE(ctx.Publish(kCount, 2));

  const int* value = ctx.Read(kCount);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(*value, 1);
}

// 6. 验证兼容替换、删除和清空不会使已经返回的只读快照悬空
TEST_F(TypedBlackboardContractsTest,
       ReadSnapshotsRemainStableUntilDestruction) {
  constexpr BlackboardKey<std::string> kValue{"value", "string"};

  AlgContext ctx;
  ASSERT_TRUE(ctx.Publish(kValue, std::string("first")));
  const std::string* first = ctx.Read(kValue);
  ASSERT_NE(first, nullptr);

  ctx.Set(kValue, std::string("second"));
  const std::string* second = ctx.Read(kValue);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(*first, "first");
  EXPECT_EQ(*second, "second");

  ctx.Erase(kValue);
  EXPECT_EQ(ctx.Read(kValue), nullptr);
  EXPECT_EQ(*first, "first");
  EXPECT_EQ(*second, "second");

  ctx.Set(kValue, std::string("third"));
  const std::string* third = ctx.Read(kValue);
  ASSERT_NE(third, nullptr);
  ctx.Clear();
  EXPECT_EQ(*third, "third");
  EXPECT_EQ(*first, "first");
}

// 7. 验证并发替换不会修改或释放已经发放的只读快照
TEST_F(TypedBlackboardContractsTest,
       ConcurrentReplacementKeepsIssuedReadViewStable) {
  AlgContext ctx;
  ASSERT_TRUE(ctx.Publish("counter", 0));
  const int* initial = ctx.Read<int>("counter");
  ASSERT_NE(initial, nullptr);

  std::atomic<bool> done{false};
  std::atomic<int> failures{0};
  std::thread writer([&]() {
    for (int value = 1; value <= 2000; ++value) {
      ctx.Set("counter", value);
    }
    done.store(true, std::memory_order_release);
  });
  std::thread reader([&]() {
    while (!done.load(std::memory_order_acquire)) {
      if (*initial != 0) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
      const int* current = ctx.Read<int>("counter");
      if (!current || *current < 0) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  writer.join();
  reader.join();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(*initial, 0);
  ASSERT_NE(ctx.Read<int>("counter"), nullptr);
  EXPECT_EQ(*ctx.Read<int>("counter"), 2000);
}

}  // namespace alg_framework
