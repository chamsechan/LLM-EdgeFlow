#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "engine/fixed_batch_executor.h"

using namespace alg_framework;

// 1. 奇数个分片样本（7个样本 / batch_size=4）自动补齐与剥离
TEST(FixedBatchExecutorTest, OddItemCountChunkAndPaddingStripping) {
  std::vector<TraceableItem<int>> inputs = {
      {0, 0, 10}, {0, 1, 20}, {0, 2, 30},  // Request 0 的 3 个分片
      {1, 0, 40}, {1, 1, 50}, {1, 2, 60}, {1, 3, 70}  // Request 1 的 4 个分片
  };

  size_t fixed_batch_size = 4;
  int dummy_pad = -999;
  int hardware_call_count = 0;

  std::vector<TraceableItem<int>> outputs;

  int ret = FixedBatchExecutor::Execute<int, int>(
      inputs, fixed_batch_size, dummy_pad,
      [&hardware_call_count, fixed_batch_size](const std::vector<int>& batch_in,
                                               std::vector<int>* batch_out) {
        hardware_call_count++;
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
  EXPECT_EQ(outputs.size(), 7);  // 自动剥离 Padding，保留刚好 7 个有效输出

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

  size_t fixed_batch_size = 2;
  std::string dummy_pad = "<PAD>";
  int call_count = 0;

  std::vector<TraceableItem<std::string>> outputs;

  int ret = FixedBatchExecutor::Execute<std::string, std::string>(
      inputs, fixed_batch_size, dummy_pad,
      [&call_count, fixed_batch_size](const std::vector<std::string>& batch_in,
                                      std::vector<std::string>* batch_out) {
        call_count++;
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
      empty_inputs, 4, 0.0f,
      [](const std::vector<float>& in, std::vector<float>* out) {
        (void)in;
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
      inputs, 2, -1,
      [](const std::vector<int>& in, std::vector<int>* out) {
        (void)in;
        (void)out;
        return -9999;  // 模拟硬件错误
      },
      &outputs);

  EXPECT_EQ(ret, -9999);
}
