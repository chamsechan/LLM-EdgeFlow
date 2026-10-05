#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace llm_edgeflow {

// binding 未覆盖时采用的 Operator 标准批大小上限。
inline constexpr size_t kDefaultIoBindingMaxBatchSize = 64;

/**
 * @brief 接入绑定定义 (将外部输入/输出转换器与内部 Pipeline 业务契约显式关联)
 */
struct IoBindingDefinition {
  std::string binding_id;
  std::string biz_name;

  std::string input_converter_id;
  std::string output_converter_id;
  // logical_name -> blackboard_key。未列出的 Converter 逻辑端口映射到同名键，
  // 只需列出改名的端口。
  std::unordered_map<std::string, std::string> input_ports;
  std::unordered_map<std::string, std::string> output_ports;
  // 默认使用 Operator 标准上限；0 表示不设上限。有效上限取 binding 及其
  // Converter 中最小的正值，且至少一方须为正。
  size_t max_batch_size = kDefaultIoBindingMaxBatchSize;
};

}  // namespace llm_edgeflow
