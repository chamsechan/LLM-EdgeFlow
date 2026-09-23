#pragma once

#include <algorithm>
#include <cstddef>
#include <exception>
#include <utility>
#include <vector>

#include "contracts/diagnostic.h"
#include "contracts/traceable_item.h"
#include "engine/inference_definition.h"

namespace llm_edgeflow {

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
  // Single-item model semantics on a dynamic session. Tensor/fixed-batch
  // models continue to use Execute and explicitly prepare their padded batch.
  // The callback owns only one input -> one output; Execute owns provenance and
  // rollback. exception_code preserves a model's established exception mapping.
  template <typename TIn, typename TOut, typename RunItem>
  static int ExecuteItems(const std::vector<TraceableItem<TIn>>& inputs,
                          const BatchPolicy& policy, RunItem&& run_item,
                          std::vector<TraceableItem<TOut>>* outputs,
                          std::string* diagnostic = nullptr,
                          int exception_code = -4) noexcept {
    if (outputs && !inputs.empty() && policy.fixed_batch_size != 0) {
      outputs->clear();
      SetDiagnosticNoexcept(diagnostic,
                            "Item execution requires a non-fixed batch policy");
      return -2;
    }
    return Execute<TIn, TOut>(
        inputs, policy,
        [&](const BatchSlice& slice, std::vector<TOut>* batch) {
          try {
            batch->reserve(slice.valid_count);
            for (size_t i = 0; i < slice.valid_count; ++i) {
              TOut output{};
              if (diagnostic) diagnostic->clear();
              const int code = run_item(inputs[slice.offset + i], &output);
              if (code != 0) return code;
              batch->push_back(std::move(output));
            }
            return 0;
          } catch (const std::exception& error) {
            SetDiagnosticNoexcept(diagnostic, error.what());
            return exception_code;
          } catch (...) {
            SetDiagnosticNoexcept(diagnostic,
                                  "Unknown item execution exception");
            return exception_code;
          }
        },
        outputs, diagnostic);
  }

  /**
   * @brief 基于 BatchSlice 和 BatchPolicy 的新版中性批处理入口
   */
  template <typename TIn, typename TOut, typename RunBatch>
  static int Execute(const std::vector<TraceableItem<TIn>>& inputs,
                     const BatchPolicy& policy, RunBatch&& run_batch,
                     std::vector<TraceableItem<TOut>>* outputs,
                     std::string* diagnostic = nullptr) noexcept {
    if (diagnostic) diagnostic->clear();
    if (!outputs) {
      SetDiagnosticNoexcept(diagnostic, "Batch output pointer is null");
      return -1;
    }
    outputs->clear();
    if (inputs.empty()) return 0;
    if (policy.max_batch_size == 0) {
      SetDiagnosticNoexcept(diagnostic, "Batch max size must be positive");
      return -2;
    }
    if (policy.fixed_batch_size != 0 &&
        policy.fixed_batch_size != policy.max_batch_size) {
      SetDiagnosticNoexcept(diagnostic, "Fixed batch size must equal max size");
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
          if (diagnostic) diagnostic->clear();
          ret = run_batch(slice, &batch_outputs);
        } catch (const std::exception& error) {
          SetDiagnosticNoexcept(diagnostic, error.what());
          outputs->clear();
          return -4;
        } catch (...) {
          SetDiagnosticNoexcept(diagnostic,
                                "Unknown batch execution exception");
          outputs->clear();
          return -4;
        }

        if (ret != 0) {
          if (diagnostic && diagnostic->empty()) {
            SetDiagnosticNoexcept(diagnostic, "Batch execution failed");
          }
          outputs->clear();
          return ret;
        }

        // 严格数量检查：固定批次必须严格等于 exec_count，动态批次必须严格等于
        // valid_count
        size_t expected_count =
            (policy.fixed_batch_size > 0) ? exec_count : valid_count;
        if (batch_outputs.size() != expected_count) {
          SetDiagnosticNoexcept(diagnostic, "Batch output count mismatch");
          outputs->clear();
          return -3;
        }

        for (size_t i = 0; i < valid_count; ++i) {
          const auto& src = inputs[offset + i];
          outputs->emplace_back(src.req_id, src.sub_id,
                                std::move(batch_outputs[i]));
        }
      }
      if (diagnostic) diagnostic->clear();
      return 0;
    } catch (const std::exception& error) {
      SetDiagnosticNoexcept(diagnostic, error.what());
      outputs->clear();
      return -5;
    } catch (...) {
      SetDiagnosticNoexcept(diagnostic, "Unknown batch allocation exception");
      outputs->clear();
      return -5;
    }
  }
};

}  // namespace llm_edgeflow
