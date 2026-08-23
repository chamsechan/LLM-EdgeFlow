#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "demo/common/demo_options.h"
#include "nlohmann/json.hpp"

namespace alg_demo {

/**
 * @brief 单个样本的执行结果记录
 */
struct DemoSampleResult {
  uint64_t request_id = 0;
  int status = 0;           // 0 成功, 非 0 错误
  double latency_ms = 0.0;  // 耗时 (ms)
  std::string error;        // 错误信息 (若失败)
  nlohmann::json output;    // 业务自定义结果 JSON
};

/**
 * @brief 整体运行统计摘要
 */
struct DemoRunSummary {
  int schema_version = 1;
  std::string profile;
  std::string business;
  std::string config_path;
  std::string dataset_path;
  int total_samples = 0;
  int success_count = 0;
  int failed_count = 0;
  double total_latency_ms = 0.0;
  double avg_latency_ms = 0.0;
  std::string error;
};

/**
 * @brief 结果落盘写入器 (负责原子落盘 JSONL 记录与 summary.json)
 */
class ResultWriter {
 public:
  explicit ResultWriter(const DemoOptions& options);

  /**
   * @brief 写入全部样本结果与统计摘要
   * @param results 单样本结果列表
   * @param total_duration_ms 总体执行耗时
   * @param error_msg 错误输出信息
   * @return 0 成功, 非 0 错误码 (6: 结果目录或文件写入错误)
   */
  int WriteResults(const std::vector<DemoSampleResult>& results,
                   double total_duration_ms, std::string* error_msg = nullptr);

  std::string GetTargetOutputDir() const;

 private:
  DemoOptions options_;
};

}  // namespace alg_demo
