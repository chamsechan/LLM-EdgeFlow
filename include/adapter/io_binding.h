#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace llm_edgeflow {

/**
 * @brief 接入绑定定义 (将外部输入/输出转换器与内部 Pipeline 业务契约显式关联)
 */
struct IoBindingDefinition {
  std::string binding_id;
  std::string biz_name;

  std::string input_converter_id;
  std::string output_converter_id;
  // logical_name -> blackboard_key. Converter logical ports without an entry
  // map to the same name; list only renamed ports.
  std::unordered_map<std::string, std::string> input_ports;
  std::unordered_map<std::string, std::string> output_ports;
  // Zero adds no bound. The effective limit is the smallest positive value
  // among the binding and its converters; at least one must be positive.
  size_t max_batch_size = 0;
};

}  // namespace llm_edgeflow
