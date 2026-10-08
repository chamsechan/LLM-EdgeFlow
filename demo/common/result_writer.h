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
  nlohmann::json output;    // 各输出项显示函数填写的结果 JSON
};

/**
 * @brief 结果落盘写入器 (负责原子落盘 JSONL 记录与 summary.json)
 */
class ResultWriter {
 public:
  /**
   * @param run_label 未指定 Profile 时用作结果子目录名与记录中的 profile
   *                  字段；为空时使用 "default"。
   */
  explicit ResultWriter(const DemoOptions& options, std::string run_label = {});

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
  std::string run_label_;
};

}  // namespace alg_demo
