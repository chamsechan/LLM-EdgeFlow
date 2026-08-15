#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <vector>

#include "core/traceable_item.h"

namespace alg_framework {

/**
 * @brief 固定 Max Batch 硬件执行器
 *
 * 解决问题：
 * 当硬件引擎（如端侧 NPU/DSP）被静态编译为固定的 max_batch_size（例如 4）时，
 * 外部可能有任意数量 M（如 7）个待推理样本。
 *
 * 执行逻辑：
 * 1. 计算批次数：ceil(M / fixed_max_batch)
 * 2. 依次切出 batch，不足 fixed_max_batch 的批次自动使用 dummy_pad 补齐。
 * 3. 调用底层固定 batch 推理接口。
 * 4. 自动剔除 dummy_pad 的推理结果，仅保留有效输出。
 * 5. 将输出与原始 TraceableItem 的 (req_id, sub_id) 严格对齐。
 */
class FixedBatchExecutor {
 public:
  template <typename TIn, typename TOut>
  static int Execute(const std::vector<TraceableItem<TIn>>& all_items,
                     size_t fixed_max_batch, const TIn& dummy_pad_input,
                     std::function<int(const std::vector<TIn>& batch_in,
                                       std::vector<TOut>* batch_out)>
                         raw_infer_func,
                     std::vector<TraceableItem<TOut>>* all_outputs) {
    if (!all_outputs) return -1;
    all_outputs->clear();
    if (all_items.empty()) return 0;
    if (fixed_max_batch == 0) return -2;

    all_outputs->reserve(all_items.size());
    size_t total = all_items.size();
    size_t num_batches = (total + fixed_max_batch - 1) / fixed_max_batch;

    std::vector<TIn> batch_in(fixed_max_batch);
    std::vector<TOut> batch_out;

    for (size_t b = 0; b < num_batches; ++b) {
      size_t start_idx = b * fixed_max_batch;
      size_t valid_count = std::min(fixed_max_batch, total - start_idx);

      // 1. 填充输入批次 (有效项 + Dummy Pad 项)
      for (size_t i = 0; i < fixed_max_batch; ++i) {
        if (i < valid_count) {
          batch_in[i] = all_items[start_idx + i].data;
        } else {
          batch_in[i] = dummy_pad_input;
        }
      }

      // 2. 调用底层固定 Batch 硬件推理函数
      batch_out.clear();
      int ret = raw_infer_func(batch_in, &batch_out);
      if (ret != 0) {
        return ret;
      }

      // 3. 校验底层输出数量是否符合要求
      if (batch_out.size() < fixed_max_batch) {
        return -3;  // 底层引擎未返回完整 batch 输出
      }

      // 4. 剥离 Pad 项，保留有效结果并继承原始溯源元数据
      for (size_t i = 0; i < valid_count; ++i) {
        const auto& src = all_items[start_idx + i];
        all_outputs->emplace_back(src.req_id, src.sub_id,
                                  std::move(batch_out[i]));
      }
    }

    return 0;
  }
};

}  // namespace alg_framework
