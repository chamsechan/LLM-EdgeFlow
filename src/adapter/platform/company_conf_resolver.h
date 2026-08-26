#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include "adapter/business_adapter_interface.h"
#include "adapter/platform/platform_business_bridge_registry.h"
#include "adapter/platform/platform_value_type_registry.h"
#include "nlohmann/json.hpp"
#include "platform/platform_operator_interface.h"

namespace alg_framework {

/**
 * @brief 解析后的公司平台部署配置与合成 Pipeline JSON (v3 规范)
 */
struct ResolvedCompanyConfig {
  std::filesystem::path conf_path;
  std::filesystem::path pipeline_path;
  std::filesystem::path model_root_path;
  std::string business_name;
  CompanyAlgBizType biz_type = ALG_BIZ_TYPE_UNKNOWN;
  std::shared_ptr<IBusinessAdapter> adapter;
  const PlatformBusinessBridgeDescriptor* bridge_descriptor = nullptr;
  nlohmann::json synthetic_pipeline_json;
  ResolvedOutputPoolSpec output_pool_spec;
  ResolvedInputLimits input_limits;
};

/**
 * @brief 公司平台部署配置解析器
 */
class CompanyConfResolver {
 public:
  /**
   * @brief 基于 model_path 根目录与相对 cfg_file_name 解析配置
   */
  static int Resolve(const char* model_path, const char* cfg_file_name,
                     int32_t device_id,
                     llm_edgeflow::platform::ChipType chip_type,
                     ResolvedCompanyConfig* result,
                     std::string* error_msg) noexcept;
};

}  // namespace alg_framework
