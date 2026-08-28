#include <gtest/gtest.h>

#include <string>
#include <thread>
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
  EXPECT_EQ((*retrieved_prompts)[0].sub_id, 0);
  EXPECT_EQ((*retrieved_prompts)[0].data, "Prompt for 1001-0");
  EXPECT_EQ((*retrieved_prompts)[1].req_id, 1002U);
  EXPECT_EQ((*retrieved_prompts)[1].sub_id, 0);
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

}  // namespace alg_framework
