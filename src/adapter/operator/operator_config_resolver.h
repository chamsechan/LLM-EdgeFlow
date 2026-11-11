#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include "adapter/io_plan_resolver.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "nlohmann/json.hpp"

namespace llm_edgeflow {

/**
 * @brief 解析后的中性部署配置与合成 Pipeline JSON
 */
struct ResolvedOperatorConfig {
  std::filesystem::path conf_path;
  std::filesystem::path pipeline_path;
  std::unique_ptr<ValidatedIoPlan> io_plan;
  uint32_t effective_frame_depth = 0;  // 归一化后的输出池深度
  uint32_t effective_process_batch_limit =
      0;  // min(池深度, kMaxProcessBatchSize)
};

/**
 * @brief 中性部署配置解析器
 */
class OperatorConfigResolver {
 public:
  /**
   * @brief 在部署根目录内解析配置文件，生成接入计划、池深度和批次上限。
   * 配置路径须为相对路径且不得越界；模型文件相对 Pipeline 目录解析。
   * 配置或计划失败时返回错误码和诊断，不创建运行时资源。
   */
  static int Resolve(const char* model_path, const char* cfg_file_name,
                     ResolvedOperatorConfig* result, std::string* error_msg,
                     uint32_t max_frame_depth = kDefaultOutputPoolDepth,
                     DeploymentDiagnostic* out_diagnostic = nullptr) noexcept;
};

}  // namespace llm_edgeflow
