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
 * @brief 解析后的中立部署配置与合成 Pipeline JSON
 */
struct ResolvedOperatorConfig {
  std::filesystem::path conf_path;
  std::filesystem::path pipeline_path;
  std::filesystem::path model_root_path;
  std::unique_ptr<ValidatedIoPlan> io_plan;
  uint32_t effective_frame_depth = 0;  // 归一化后的输出池深度
  uint32_t effective_process_batch_limit =
      0;  // min(池深度, kMaxProcessBatchSize)
};

/**
 * @brief 中立部署配置解析器
 */
class OperatorConfigResolver {
 public:
  static int Resolve(const char* model_path, const char* cfg_file_name,
                     ResolvedOperatorConfig* result, std::string* error_msg,
                     uint32_t max_frame_depth = kDefaultOutputPoolDepth,
                     DeploymentDiagnostic* out_diagnostic = nullptr) noexcept;
};

}  // namespace llm_edgeflow
