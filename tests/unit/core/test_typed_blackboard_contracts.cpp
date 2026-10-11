#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "contracts/traceable_item.h"
#include "core/alg_context.h"
#include "core/blackboard_key.h"
#include "core/common_contracts.h"
#include "nodes/node_base.h"

namespace llm_edgeflow {

class TypedBlackboardContractsTest : public ::testing::Test {};

// 1. 验证强类型 BlackboardKey 的 Publish/Read/Has 操作与类型匹配
TEST_F(TypedBlackboardContractsTest, TypedKeyOperations) {
  constexpr BlackboardKey<std::vector<std::string>> kTestStrings{"test_strings",
                                                                 "string[]"};
  constexpr BlackboardKey<int> kTestInt{"test_int", "int"};

  AlgContext ctx;
  EXPECT_FALSE(ctx.Has("missing"));
  EXPECT_EQ(ctx.Read<int>("missing"), nullptr);
  EXPECT_FALSE(ctx.Has(kTestStrings));
  EXPECT_EQ(ctx.Read(kTestStrings), nullptr);

  std::vector<std::string> sample_data = {"alpha", "beta", "gamma"};
  ASSERT_TRUE(ctx.Publish(kTestStrings, sample_data));
  ASSERT_TRUE(ctx.Publish(kTestInt, 42));

  EXPECT_TRUE(ctx.Has(kTestStrings));
  EXPECT_TRUE(ctx.Has(kTestInt));

  auto* retrieved_strings = ctx.Read(kTestStrings);
  ASSERT_NE(retrieved_strings, nullptr);
  EXPECT_EQ(*retrieved_strings, sample_data);

  auto* retrieved_int = ctx.Read(kTestInt);
  ASSERT_NE(retrieved_int, nullptr);
  EXPECT_EQ(*retrieved_int, 42);
  EXPECT_EQ(ctx.Read<double>(kTestInt.name), nullptr);
}

// 2. 验证类型不匹配时安全返回 nullptr
TEST_F(TypedBlackboardContractsTest, TypeMismatchReturnsNullptr) {
  constexpr BlackboardKey<std::string> kStringKey{"poly_key", "string"};
  constexpr BlackboardKey<int> kIntKey{"poly_key", "int"};

  AlgContext ctx;
  ASSERT_TRUE(ctx.Publish(kStringKey, std::string("hello world")));

  EXPECT_TRUE(ctx.Has(kStringKey));
  EXPECT_NE(ctx.Read(kStringKey), nullptr);

  // 尝试用不同类型读取同名 key，应安全返回 nullptr
  auto* int_view = ctx.Read(kIntKey);
  EXPECT_EQ(int_view, nullptr);
}

// 3. 验证 CommonContracts 中性值类型的读写与 TraceableItem 支持
TEST_F(TypedBlackboardContractsTest, CommonContractsAndTraceableProvenance) {
  constexpr BlackboardKey<std::vector<uint64_t>> kRequestIds{"test_request_ids",
                                                             "vector<uint64>"};
  constexpr BlackboardKey<TextBatch> kQueries{"test_queries", "TextBatch"};
  constexpr BlackboardKey<TextBatch> kPrompts{"test_prompts", "TextBatch"};
  AlgContext ctx;

  std::vector<uint64_t> raw_req_ids = {1001, 1002};
  TextBatch queries = {TraceableItem<std::string>{1001, 0, "query 1"},
                       TraceableItem<std::string>{1002, 0, "query 2"}};
  ASSERT_TRUE(ctx.Publish(kRequestIds, raw_req_ids));
  ASSERT_TRUE(ctx.Publish(kQueries, queries));

  EXPECT_TRUE(ctx.Has(kRequestIds));
  EXPECT_TRUE(ctx.Has(kQueries));

  // TraceableItem 批处理样本可追溯性
  std::vector<TraceableItem<std::string>> prompts = {
      TraceableItem<std::string>{1001, 0, "Prompt for 1001-0"},
      TraceableItem<std::string>{1002, 0, "Prompt for 1002-0"},
      TraceableItem<std::string>{1001, 1, "Prompt for 1001-1"}};
  ASSERT_TRUE(ctx.Publish(kPrompts, prompts));

  auto* retrieved_prompts = ctx.Read(kPrompts);
  ASSERT_NE(retrieved_prompts, nullptr);
  ASSERT_EQ(retrieved_prompts->size(), 3U);
  EXPECT_EQ((*retrieved_prompts)[0].req_id, 1001U);
  EXPECT_EQ((*retrieved_prompts)[0].sub_id, 0U);
  EXPECT_EQ((*retrieved_prompts)[0].data, "Prompt for 1001-0");
  EXPECT_EQ((*retrieved_prompts)[1].req_id, 1002U);
  EXPECT_EQ((*retrieved_prompts)[1].sub_id, 0U);
  EXPECT_EQ((*retrieved_prompts)[1].data, "Prompt for 1002-0");
  EXPECT_EQ((*retrieved_prompts)[2].req_id, 1001U);
  EXPECT_EQ((*retrieved_prompts)[2].sub_id, 1U);
  EXPECT_EQ((*retrieved_prompts)[2].data, "Prompt for 1001-1");
}

// 4. 验证错误状态与 write-once 请求值共享同一请求生命周期
TEST_F(TypedBlackboardContractsTest, ErrorStateDoesNotMutatePublishedValues) {
  constexpr BlackboardKey<std::string> kKey{"greeting", "string"};

  AlgContext ctx;
  ASSERT_TRUE(ctx.Publish(kKey, std::string("welcome")));
  ctx.SetError(-500, "sample error");

  EXPECT_TRUE(ctx.Has(kKey));
  EXPECT_FALSE(ctx.IsOk());
  EXPECT_EQ(ctx.GetErrorCode(), -500);
  EXPECT_EQ(ctx.GetErrorMessage(), "sample error");

  const auto* value = ctx.Read(kKey);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(*value, "welcome");

  AlgContext next_request;
  EXPECT_FALSE(next_request.Has(kKey));
  EXPECT_TRUE(next_request.IsOk());
  EXPECT_EQ(next_request.GetErrorCode(), 0);
}

// 5. 验证单次发布拒绝静默覆盖，Read 只返回只读视图
TEST_F(TypedBlackboardContractsTest, PublishIsWriteOnceAndReadIsReadOnly) {
  constexpr BlackboardKey<int> kCount{"count", "int"};
  static_assert(
      std::is_same_v<decltype(std::declval<AlgContext&>().Read(kCount)),
                     const int*>);

  AlgContext ctx;
  EXPECT_TRUE(ctx.Publish(kCount, 1));
  EXPECT_FALSE(ctx.Publish(kCount, 2));

  const int* value = ctx.Read(kCount);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(*value, 1);
}

// 6. 验证重复发布和容器扩容不会使已经返回的只读视图悬空
TEST_F(TypedBlackboardContractsTest, ReadViewsRemainStableAcrossPublications) {
  constexpr BlackboardKey<std::string> kValue{"value", "string"};

  AlgContext ctx;
  ASSERT_TRUE(ctx.Publish(kValue, std::string("first")));
  const std::string* first = ctx.Read(kValue);
  ASSERT_NE(first, nullptr);

  EXPECT_FALSE(ctx.Publish(kValue, std::string("second")));
  for (int i = 0; i < 4096; ++i) {
    ASSERT_TRUE(ctx.Publish("additional_" + std::to_string(i), i));
  }
  const std::string* current = ctx.Read(kValue);
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current, first);
  EXPECT_EQ(*first, "first");
}

// 7. 验证并发发布不同 key 时已经发放的只读视图保持稳定
TEST_F(TypedBlackboardContractsTest,
       ConcurrentPublicationsKeepIssuedReadViewStable) {
  AlgContext ctx;
  ASSERT_TRUE(ctx.Publish("counter", 0));
  const int* initial = ctx.Read<int>("counter");
  ASSERT_NE(initial, nullptr);

  std::atomic<bool> done{false};
  std::atomic<int> failures{0};
  std::thread writer([&]() {
    for (int value = 1; value <= 2000; ++value) {
      if (!ctx.Publish("counter_" + std::to_string(value), value)) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
    }
    done.store(true, std::memory_order_release);
  });
  std::thread reader([&]() {
    while (!done.load(std::memory_order_acquire)) {
      if (*initial != 0) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
      const int* current = ctx.Read<int>("counter");
      if (current != initial || !current || *current != 0) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  writer.join();
  reader.join();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(*initial, 0);
  ASSERT_NE(ctx.Read<int>("counter_2000"), nullptr);
  EXPECT_EQ(*ctx.Read<int>("counter_2000"), 2000);
}

TEST_F(TypedBlackboardContractsTest, BlackboardKeyAndBoundInputContracts) {
  constexpr auto key = MakeBlackboardKey<TextBatch>("custom_key");
  EXPECT_STREQ(key.name, "custom_key");
  EXPECT_STREQ(key.type_id, "TextBatch");

  const BlackboardKey<TextBatch> mismatched_key{"key", "WrongType"};
  EXPECT_THROW((BoundInput<TextBatch>(mismatched_key)), std::invalid_argument);
  EXPECT_THROW((BoundOutput<TextBatch>(mismatched_key)), std::invalid_argument);

  EXPECT_NO_THROW((BoundInput<TextBatch>(key)));
  EXPECT_NO_THROW((BoundOutput<TextBatch>(key)));
}

static std::atomic<int> g_probe_constructed_count{0};
static std::atomic<int> g_probe_destructed_count{0};

struct LifetimeProbe {
  std::vector<float> large_payload;
  explicit LifetimeProbe(size_t elements = 10000)
      : large_payload(elements, 1.234f) {
    g_probe_constructed_count.fetch_add(1);
  }
  LifetimeProbe(const LifetimeProbe& o) : large_payload(o.large_payload) {
    g_probe_constructed_count.fetch_add(1);
  }
  LifetimeProbe(LifetimeProbe&& o) noexcept
      : large_payload(std::move(o.large_payload)) {
    g_probe_constructed_count.fetch_add(1);
  }
  ~LifetimeProbe() { g_probe_destructed_count.fetch_add(1); }
};

TEST_F(TypedBlackboardContractsTest, LargePayloadRaiiDestruction) {
  using namespace llm_edgeflow;

  g_probe_constructed_count.store(0);
  g_probe_destructed_count.store(0);

  const int test_cycles = 50;
  for (int i = 0; i < test_cycles; ++i) {
    {
      AlgContext ctx;
      ctx.Publish("probe_1", LifetimeProbe(20000));
      ctx.Publish("probe_2", LifetimeProbe(20000));

      EXPECT_TRUE(ctx.Has("probe_1"));
      EXPECT_TRUE(ctx.Has("probe_2"));
      auto* p1 = ctx.Read<LifetimeProbe>("probe_1");
      ASSERT_NE(p1, nullptr);
      EXPECT_EQ(p1->large_payload.size(), 20000);
      // 作用域退出，ctx 销毁
    }
  }

  // 严格断言：构造次数与析构次数 100% 精确相等，且大于 0，无任何内存泄漏
  EXPECT_GT(g_probe_constructed_count.load(), 0);
  EXPECT_EQ(g_probe_constructed_count.load(), g_probe_destructed_count.load());
}

}  // namespace llm_edgeflow
