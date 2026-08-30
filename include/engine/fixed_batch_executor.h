#pragma once

#include <algorithm>
#include <cstddef>
#include <exception>
#include <utility>
#include <vector>

#include "contracts/traceable_item.h"
#include "engine/inference_definition.h"

namespace alg_framework {

/**
 * @brief 单批次分片切片元数据
 */
struct BatchSlice {
  size_t offset = 0;
  size_t valid_count = 0;
  size_t execution_count = 0;
};

/**
 * @brief 固定/动态 Max Batch 硬件批处理调度器
 *
 * 解决问题：
 * 1. 当底层推理运行时（如 NPU/DSP/TensorRT）要求固定 Batch
 * 时，自动计算切片与补齐。
 * 2. 具备样本溯源能力：自动保留 (req_id, sub_id) 元数据，剔除 Pad 结果。
 * 3. 失败全量回滚：任一批次推理失败或异常时，清空已产出结果，避免半脏数据暴露。
 */
class FixedBatchExecutor {
 public:
  /**
   * @brief 基于 BatchSlice 和 BatchPolicy 的新版中性批处理入口
   */
  template <typename TIn, typename TOut, typename RunBatch>
  static int Execute(const std::vector<TraceableItem<TIn>>& inputs,
                     const BatchPolicy& policy, RunBatch&& run_batch,
                     std::vector<TraceableItem<TOut>>* outputs) noexcept {
    if (!outputs) return -1;
    outputs->clear();
    if (inputs.empty()) return 0;
    if (policy.max_batch_size == 0) return -2;
    if (policy.fixed_batch_size != 0 &&
        policy.fixed_batch_size != policy.max_batch_size) {
      return -2;
    }

    try {
      outputs->reserve(inputs.size());
      size_t total = inputs.size();
      size_t batch_size = policy.max_batch_size;

      for (size_t offset = 0; offset < total; offset += batch_size) {
        size_t valid_count = std::min(batch_size, total - offset);
        size_t exec_count = (policy.fixed_batch_size > 0)
                                ? policy.fixed_batch_size
                                : valid_count;

        BatchSlice slice{offset, valid_count, exec_count};
        std::vector<TOut> batch_outputs;

        int ret = 0;
        try {
          ret = run_batch(slice, &batch_outputs);
        } catch (...) {
          outputs->clear();
          return -4;
        }

        if (ret != 0) {
          outputs->clear();
          return ret;
        }

        // 严格数量检查：固定批次必须严格等于 exec_count，动态批次必须严格等于
        // valid_count
        size_t expected_count =
            (policy.fixed_batch_size > 0) ? exec_count : valid_count;
        if (batch_outputs.size() != expected_count) {
          outputs->clear();
          return -3;
        }

        for (size_t i = 0; i < valid_count; ++i) {
          const auto& src = inputs[offset + i];
          outputs->emplace_back(src.req_id, src.sub_id,
                                std::move(batch_outputs[i]));
        }
      }
      return 0;
    } catch (...) {
      outputs->clear();
      return -5;
    }
  }
};

}  // namespace alg_framework
