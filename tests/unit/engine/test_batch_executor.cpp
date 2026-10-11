#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/fixed_batch_executor.h"

using namespace llm_edgeflow;

TEST(FixedBatchExecutorTest, BatchBoundariesPreserveProvenance) {
  for (size_t count : {0U, 4U, 7U, 8U}) {
    SCOPED_TRACE(count);
    std::vector<TraceableItem<std::string>> inputs;
    for (size_t i = 0; i < count; ++i)
      inputs.emplace_back(i / 3, i % 3, "item" + std::to_string(i));
    std::vector<TraceableItem<std::string>> outputs{{99, 99, "stale"}};
    size_t calls = 0;
    const int result = FixedBatchExecutor::Execute<std::string, std::string>(
        inputs, BatchPolicy{4, 4},
        [&](const BatchSlice& slice, std::vector<std::string>* batch) {
          ++calls;
          EXPECT_EQ(slice.execution_count, 4U);
          EXPECT_EQ(slice.valid_count,
                    std::min(size_t{4}, count - slice.offset));
          batch->assign(slice.execution_count, "padding");
          for (size_t i = 0; i < slice.valid_count; ++i)
            (*batch)[i] = inputs[slice.offset + i].data + "_processed";
          return 0;
        },
        &outputs);
    ASSERT_EQ(result, 0);
    EXPECT_EQ(calls, (count + 3) / 4);
    ASSERT_EQ(outputs.size(), count);
    for (size_t i = 0; i < count; ++i) {
      EXPECT_EQ(outputs[i].req_id, inputs[i].req_id);
      EXPECT_EQ(outputs[i].sub_id, inputs[i].sub_id);
      EXPECT_EQ(outputs[i].data, inputs[i].data + "_processed");
    }
  }
}

// 4. 硬件错误码向上传递
TEST(FixedBatchExecutorTest, KernelFailureRollsBackAndAllowsRetry) {
  const std::vector<TraceableItem<int>> inputs{
      {1, 0, 100}, {2, 1, 200}, {2, 2, 300}};
  std::vector<TraceableItem<int>> outputs{{99, 99, -1}};
  for (bool fail : {true, false}) {
    SCOPED_TRACE(fail);
    int calls = 0;
    const int result = FixedBatchExecutor::Execute<int, int>(
        inputs, BatchPolicy{2, 2},
        [&](const BatchSlice& slice, std::vector<int>* batch) {
          ++calls;
          if (fail) return -505;
          batch->assign(slice.execution_count, 42);
          return 0;
        },
        &outputs);
    EXPECT_EQ(result, fail ? -505 : 0);
    EXPECT_EQ(calls, fail ? 1 : 2);
    if (fail)
      EXPECT_TRUE(outputs.empty());
    else {
      ASSERT_EQ(outputs.size(), inputs.size());
      for (size_t i = 0; i < inputs.size(); ++i) {
        EXPECT_EQ(outputs[i].req_id, inputs[i].req_id);
        EXPECT_EQ(outputs[i].sub_id, inputs[i].sub_id);
        EXPECT_EQ(outputs[i].data, 42);
      }
    }
  }
}

// 5. 回调返回数量不一致或异常时硬捕获并清空回滚
TEST(FixedBatchExecutorTest, StrictOutputsAndRollback) {
  std::vector<TraceableItem<int>> inputs = {
      {1, 0, 10}, {1, 1, 20}, {2, 0, 30}, {2, 1, 40}, {3, 0, 50}};
  BatchPolicy policy{2, 0};  // 动态批大小上限 = 2
  std::vector<TraceableItem<int>> outputs;

  // 1. 正常执行
  int ret = FixedBatchExecutor::Execute<int, int>(
      inputs, policy,
      [](const BatchSlice& slice, std::vector<int>* batch_out) {
        batch_out->resize(slice.valid_count);
        for (size_t i = 0; i < slice.valid_count; ++i) {
          (*batch_out)[i] = static_cast<int>(slice.offset + i);
        }
        return 0;
      },
      &outputs);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(outputs.size(), 5U);
  EXPECT_EQ(outputs[0].req_id, 1U);
  EXPECT_EQ(outputs[4].req_id, 3U);

  // 2. 回调返回多于预期数量时拒绝并回滚
  outputs.clear();
  ret = FixedBatchExecutor::Execute<int, int>(
      inputs, policy,
      [](const BatchSlice& slice, std::vector<int>* batch_out) {
        batch_out->resize(slice.valid_count + 1, 0);  // 多返回一项
        return 0;
      },
      &outputs);
  EXPECT_EQ(ret, -3);
  EXPECT_TRUE(outputs.empty());

  // 3. 回调返回少于预期数量时拒绝并回滚
  outputs.clear();
  ret = FixedBatchExecutor::Execute<int, int>(
      inputs, policy,
      [](const BatchSlice& slice, std::vector<int>* batch_out) {
        batch_out->resize(slice.valid_count - 1, 0);  // 少返回一项
        return 0;
      },
      &outputs);
  EXPECT_EQ(ret, -3);
  EXPECT_TRUE(outputs.empty());

  // 4. 回调抛异常时硬捕获并回滚
  outputs.clear();
  ret = FixedBatchExecutor::Execute<int, int>(
      inputs, policy,
      [](const BatchSlice&, std::vector<int>*) -> int {
        throw std::runtime_error("Simulated inference exception");
      },
      &outputs);
  EXPECT_EQ(ret, -4);
  EXPECT_TRUE(outputs.empty());
}

TEST(FixedBatchExecutorTest, LaterFailurePreservesDiagnosticAndRollsBack) {
  const std::vector<TraceableItem<int>> inputs{{1, 0, 10}, {2, 0, 20}};
  std::vector<TraceableItem<int>> outputs{{9, 0, 99}};
  std::string diagnostic = "stale error";
  int calls = 0;
  const int result = FixedBatchExecutor::Execute<int, int>(
      inputs, BatchPolicy{1, 0},
      [&](const BatchSlice&, std::vector<int>* batch_outputs) {
        ++calls;
        if (calls == 2) {
          diagnostic = "scripted failure on second batch";
          return -77;
        }
        batch_outputs->push_back(42);
        return 0;
      },
      &outputs, &diagnostic);
  EXPECT_EQ(result, -77);
  EXPECT_EQ(calls, 2);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(diagnostic, "scripted failure on second batch");
}

TEST(FixedBatchExecutorTest, SuccessAndEmptyBatchClearPreviousDiagnostic) {
  const std::vector<TraceableItem<int>> inputs{{1, 2, 10}};
  std::vector<TraceableItem<int>> outputs;
  std::string diagnostic = "previous failure";
  int calls = 0;
  const auto run = [&](const BatchSlice&, std::vector<int>* batch_outputs) {
    ++calls;
    batch_outputs->push_back(20);
    return 0;
  };
  const int result = FixedBatchExecutor::Execute<int, int>(
      inputs, BatchPolicy{1, 0}, run, &outputs, &diagnostic);
  ASSERT_EQ(result, 0);
  EXPECT_TRUE(diagnostic.empty());
  ASSERT_EQ(outputs.size(), 1U);
  EXPECT_EQ(outputs[0].req_id, 1U);
  EXPECT_EQ(outputs[0].sub_id, 2U);
  EXPECT_EQ(outputs[0].data, 20);

  diagnostic = "another failure";
  const int empty_result = FixedBatchExecutor::Execute<int, int>(
      {}, BatchPolicy{1, 0}, run, &outputs, &diagnostic);
  EXPECT_EQ(empty_result, 0);
  EXPECT_TRUE(diagnostic.empty());
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(calls, 1);
}

TEST(FixedBatchExecutorTest, LaterExceptionRetainsWhatAndRollsBack) {
  const std::vector<TraceableItem<int>> inputs{{1, 0, 10}, {2, 0, 20}};
  std::vector<TraceableItem<int>> outputs;
  std::string diagnostic = "stale error";
  int calls = 0;
  const int result = FixedBatchExecutor::Execute<int, int>(
      inputs, BatchPolicy{1, 0},
      [&](const BatchSlice&, std::vector<int>* batch_outputs) {
        if (++calls == 2) {
          throw std::runtime_error("device unavailable on second batch");
        }
        batch_outputs->push_back(42);
        return 0;
      },
      &outputs, &diagnostic);
  EXPECT_EQ(result, -4);
  EXPECT_EQ(calls, 2);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(diagnostic, "device unavailable on second batch");
}

TEST(FixedBatchExecutorTest, LaterFailureDoesNotReuseEarlierBatchDiagnostic) {
  std::vector<TraceableItem<int>> inputs = {{0, 0, 10}, {1, 0, 20}};
  std::vector<TraceableItem<int>> outputs;
  std::string diagnostic;
  size_t calls = 0;
  const int result = FixedBatchExecutor::Execute<int, int>(
      inputs, BatchPolicy{1, 1},
      [&](const BatchSlice& slice, std::vector<int>* batch_outputs) {
        ++calls;
        if (slice.offset == 0) {
          diagnostic = "earlier successful batch note";
          batch_outputs->push_back(10);
          return 0;
        }
        return -73;
      },
      &outputs, &diagnostic);
  EXPECT_EQ(result, -73);
  EXPECT_EQ(calls, 2U);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(diagnostic, "Batch execution failed");
}

TEST(FixedBatchExecutorTest, ExecuteItemsPreservesOrderAndDuplicateProvenance) {
  const std::vector<TraceableItem<int>> inputs{
      {9, 3, 10}, {9, 3, 20}, {1, 7, 30}};
  std::vector<TraceableItem<int>> outputs{{99, 99, -1}};
  std::string diagnostic = "stale";
  int calls = 0;
  const int result = FixedBatchExecutor::ExecuteItems<int, int>(
      inputs, BatchPolicy{2, 0},
      [&](const TraceableItem<int>& item, int* output) {
        ++calls;
        *output = item.data * 2;
        return 0;
      },
      &outputs, &diagnostic);
  ASSERT_EQ(result, 0);
  EXPECT_EQ(calls, 3);
  EXPECT_TRUE(diagnostic.empty());
  ASSERT_EQ(outputs.size(), inputs.size());
  for (size_t i = 0; i < inputs.size(); ++i) {
    EXPECT_EQ(outputs[i].req_id, inputs[i].req_id);
    EXPECT_EQ(outputs[i].sub_id, inputs[i].sub_id);
    EXPECT_EQ(outputs[i].data, inputs[i].data * 2);
  }
}

TEST(FixedBatchExecutorTest, ExecuteItemsValidatesOutputsAndFixedPolicy) {
  const std::vector<TraceableItem<int>> inputs{{1, 0, 10}};
  std::vector<TraceableItem<int>> outputs{{99, 0, -1}};
  int calls = 0;
  auto run = [&](const TraceableItem<int>&, int*) {
    ++calls;
    return 0;
  };
  int result = FixedBatchExecutor::ExecuteItems<int, int>(
      inputs, BatchPolicy{1, 0}, run, nullptr);
  EXPECT_EQ(result, -1);
  result = FixedBatchExecutor::ExecuteItems<int, int>(inputs, BatchPolicy{2, 2},
                                                      run, &outputs);
  EXPECT_EQ(result, -2);
  EXPECT_TRUE(outputs.empty());
  outputs.emplace_back(99, 0, -1);
  std::string diagnostic = "stale";
  result = FixedBatchExecutor::ExecuteItems<int, int>(
      {}, BatchPolicy{2, 2}, run, &outputs, &diagnostic);
  EXPECT_EQ(result, 0);
  EXPECT_TRUE(outputs.empty());
  EXPECT_TRUE(diagnostic.empty());
  EXPECT_EQ(calls, 0);
}

TEST(FixedBatchExecutorTest, ExecuteItemsRollsBackOnLaterError) {
  const std::vector<TraceableItem<int>> inputs{
      {1, 0, 10}, {2, 0, 20}, {3, 0, 30}};
  std::vector<TraceableItem<int>> outputs;
  std::string diagnostic = "stale";
  int calls = 0;
  const int result = FixedBatchExecutor::ExecuteItems<int, int>(
      inputs, BatchPolicy{3, 0},
      [&](const TraceableItem<int>& item, int* output) {
        ++calls;
        if (item.data == 20) {
          diagnostic = "item inference failed";
          return -73;
        }
        *output = item.data;
        return 0;
      },
      &outputs, &diagnostic);
  EXPECT_EQ(result, -73);
  EXPECT_EQ(calls, 2);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(diagnostic, "item inference failed");
}

TEST(FixedBatchExecutorTest, ExecuteItemsRollsBackBothExceptionKinds) {
  for (bool standard_exception : {false, true}) {
    SCOPED_TRACE(standard_exception);
    const std::vector<TraceableItem<int>> inputs{{1, 0, 10}, {2, 0, 20}};
    std::vector<TraceableItem<int>> outputs;
    std::string diagnostic;
    int calls = 0;
    const int result = FixedBatchExecutor::ExecuteItems<int, int>(
        inputs, BatchPolicy{2, 0},
        [&](const TraceableItem<int>& item, int* output) {
          if (++calls == 2) {
            if (standard_exception)
              throw std::runtime_error("item unavailable");
            throw 42;
          }
          *output = item.data;
          return 0;
        },
        &outputs, &diagnostic, -87);
    EXPECT_EQ(result, -87);
    EXPECT_EQ(calls, 2);
    EXPECT_TRUE(outputs.empty());
    if (standard_exception)
      EXPECT_EQ(diagnostic, "item unavailable");
    else
      EXPECT_FALSE(diagnostic.empty());
  }
}

TEST(FixedBatchExecutorTest, ExecuteItemsDoesNotReuseEarlierItemDiagnostic) {
  const std::vector<TraceableItem<int>> inputs{{1, 0, 10}, {2, 0, 20}};
  std::vector<TraceableItem<int>> outputs;
  std::string diagnostic;
  int calls = 0;
  const int result = FixedBatchExecutor::ExecuteItems<int, int>(
      inputs, BatchPolicy{2, 0},
      [&](const TraceableItem<int>& item, int* output) {
        ++calls;
        if (item.data == 10) {
          diagnostic = "earlier successful item note";
          *output = item.data;
          return 0;
        }
        return -73;
      },
      &outputs, &diagnostic);
  EXPECT_EQ(result, -73);
  EXPECT_EQ(calls, 2);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(diagnostic, "Batch execution failed");
}
