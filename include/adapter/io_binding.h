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
  std::string transport;  // "cabi" 或 "operator"
  std::string input_converter_id;
  std::string output_converter_id;
  std::unordered_map<std::string, std::string> input_ports;   // logical_name -> blackboard_key
  std::unordered_map<std::string, std::string> output_ports;  // logical_name -> blackboard_key
};

/**
 * @brief 业务生产暴露能力声明 (声明生产环境必需支持的入口形式与批次约束)
 */
struct BizExposureDefinition {
  std::string biz_name;
  size_t max_batch_size = 64;
  std::vector<std::string> required_transports;  // {"cabi", "operator"}
};

}  // namespace llm_edgeflow
