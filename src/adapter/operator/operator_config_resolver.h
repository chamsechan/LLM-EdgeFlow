#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include "adapter/io_binding_resolver.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "edgeflow/operator/interface.h"
#include "nlohmann/json.hpp"

namespace llm_edgeflow {

/**
 * @brief 解析后的公司部署配置与合成 Pipeline JSON
 */
struct ResolvedOperatorConfig {
  std::filesystem::path conf_path;
  std::filesystem::path pipeline_path;
  std::filesystem::path model_root_path;
  std::string biz_name;
  std::string io_binding;
  std::unique_ptr<ValidatedIoPlan> io_plan;
  ResolvedInputLimits input_limits;
};

/**
 * @brief 公司部署配置解析器
 */
class OperatorConfigResolver {
 public:
  static int ResolveOutputAllocation(const nlohmann::json& config,
                                     const ExternalSlotDefinition& slot,
                                     ResolvedOutputPoolSpec* result,
                                     std::string* parameter_text,
                                     std::string* error);

  static int Resolve(const char* model_path, const char* cfg_file_name,
                     ResolvedOperatorConfig* result, std::string* error_msg,
                     uint32_t max_frame_depth = 25,
                     DeploymentDiagnostic* out_diagnostic = nullptr) noexcept;
};

}  // namespace llm_edgeflow
