#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "engine/fixed_batch_executor.h"

using namespace llm_edgeflow;

// 1. 奇数个分片样本（7个样本 / batch_size=4）自动补齐与剥离
TEST(FixedBatchExecutorTest, OddItemCountChunkAndPaddingStripping) {
  std::vector<TraceableItem<int>> inputs = {
      {0, 0, 10}, {0, 1, 20}, {0, 2, 30},  // Request 0 的 3 个分片
      {1, 0, 40}, {1, 1, 50}, {1, 2, 60}, {1, 3, 70}  // Request 1 的 4 个分片
  };

  const size_t fixed_batch_size = 4;
  const int dummy_pad = -999;
  int hardware_call_count = 0;

  std::vector<TraceableItem<int>> outputs;

  int ret = FixedBatchExecutor::Execute<int, int>(
      inputs, BatchPolicy{fixed_batch_size, fixed_batch_size},
      [&hardware_call_count, fixed_batch_size, dummy_pad, &inputs](
          const BatchSlice& slice, std::vector<int>* batch_out) {
        hardware_call_count++;
        std::vector<int> batch_in(fixed_batch_size, dummy_pad);
        for (size_t i = 0; i < slice.valid_count; ++i) {
          batch_in[i] = inputs[slice.offset + i].data;
        }
        EXPECT_EQ(batch_in.size(), fixed_batch_size);
        batch_out->resize(fixed_batch_size);
        for (size_t i = 0; i < fixed_batch_size; ++i) {
          if (batch_in[i] == -999) {
            (*batch_out)[i] = -999;  // Pad 输出
          } else {
            (*batch_out)[i] = batch_in[i] * 2;  // 真实计算
          }
        }
        return 0;
      },
      &outputs);

  EXPECT_EQ(ret, 0);
  EXPECT_EQ(hardware_call_count, 2);  // 7 / 4 向上取整 = 2 次硬件调用
  EXPECT_EQ(outputs.size(), 7U);  // 自动剥离 Padding，保留刚好 7 个有效输出

  // 验证输出与原始 TraceableItem 的 (req_id, sub_id) 严格对齐
  for (size_t i = 0; i < inputs.size(); ++i) {
    EXPECT_EQ(outputs[i].req_id, inputs[i].req_id);
    EXPECT_EQ(outputs[i].sub_id, inputs[i].sub_id);
    EXPECT_EQ(outputs[i].data, inputs[i].data * 2);
  }
}

// 2. 恰好为 batch_size 整数倍的样本处理
TEST(FixedBatchExecutorTest, ExactMultipleBatchSize) {
  std::vector<TraceableItem<std::string>> inputs = {{101, 0, "alpha"},
                                                    {101, 1, "beta"},
                                                    {102, 0, "gamma"},
                                                    {102, 1, "delta"}};

  const size_t fixed_batch_size = 2;
  const std::string dummy_pad = "<PAD>";
  int call_count = 0;

  std::vector<TraceableItem<std::string>> outputs;

  int ret = FixedBatchExecutor::Execute<std::string, std::string>(
      inputs, BatchPolicy{fixed_batch_size, fixed_batch_size},
      [&call_count, fixed_batch_size, &dummy_pad, &inputs](
          const BatchSlice& slice, std::vector<std::string>* batch_out) {
        call_count++;
        std::vector<std::string> batch_in(fixed_batch_size, dummy_pad);
        for (size_t i = 0; i < slice.valid_count; ++i) {
          batch_in[i] = inputs[slice.offset + i].data;
        }
        EXPECT_EQ(batch_in.size(), fixed_batch_size);
        batch_out->resize(fixed_batch_size);
        for (size_t i = 0; i < fixed_batch_size; ++i) {
          (*batch_out)[i] = batch_in[i] + "_processed";
        }
        return 0;
      },
      &outputs);

  EXPECT_EQ(ret, 0);
  EXPECT_EQ(call_count, 2);
  EXPECT_EQ(outputs.size(), 4);
  EXPECT_EQ(outputs[0].data, "alpha_processed");
  EXPECT_EQ(outputs[3].data, "delta_processed");
}

// 3. 空输入边界处理
TEST(FixedBatchExecutorTest, EmptyInputHandling) {
  std::vector<TraceableItem<float>> empty_inputs;
  std::vector<TraceableItem<float>> outputs;

  int ret = FixedBatchExecutor::Execute<float, float>(
      empty_inputs, BatchPolicy{4, 4},
      [](const BatchSlice& slice, std::vector<float>* out) {
        (void)slice;
        (void)out;
        return 0;
      },
      &outputs);

  EXPECT_EQ(ret, 0);
  EXPECT_TRUE(outputs.empty());
}

// 4. 硬件错误码向上传递
TEST(FixedBatchExecutorTest, ErrorPropagationFromKernel) {
  std::vector<TraceableItem<int>> inputs = {{1, 0, 100}};
  std::vector<TraceableItem<int>> outputs;

  int ret = FixedBatchExecutor::Execute<int, int>(
      inputs, BatchPolicy{2, 2},
      [](const BatchSlice& slice, std::vector<int>* out) {
        (void)slice;
        (void)out;
        return -9999;  // 模拟硬件错误
      },
      &outputs);

  EXPECT_EQ(ret, -9999);
}

// 5. 回调返回数量不一致或异常时硬捕获并清空回滚
TEST(FixedBatchExecutorTest, StrictOutputsAndRollback) {
  std::vector<TraceableItem<int>> inputs = {
      {1, 0, 10}, {1, 1, 20}, {2, 0, 30}, {2, 1, 40}, {3, 0, 50}};
  BatchPolicy policy{2, 0};  // dynamic max batch size = 2
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
