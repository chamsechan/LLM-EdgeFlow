#pragma once

#include <cstddef>
#include <string>

namespace llm_edgeflow {

// binding 未覆盖时采用的 Operator 标准批大小上限。
inline constexpr size_t kDefaultIoBindingMaxBatchSize = 64;

/**
 * @brief 接入绑定定义 (将外部输入/输出转换器与内部 Pipeline 业务契约显式关联)
 *
 * 每个业务只有一个 binding，以 biz_name 标识。
 */
struct IoBindingDefinition {
  std::string biz_name;

  std::string input_converter_id;
  std::string output_converter_id;
  // 单次 Process 的批大小上限，须为正；Operator 再按输出池深收紧。
  size_t max_batch_size = kDefaultIoBindingMaxBatchSize;
};

}  // namespace llm_edgeflow
