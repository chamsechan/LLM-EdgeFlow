#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include "adapter/biz_adapter_interface.h"
#include "adapter/operator/operator_biz_bridge_registry.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "nlohmann/json.hpp"
#include "operator/operator_interface.h"

namespace alg_framework {

/**
 * @brief 解析后的公司部署配置与合成 Pipeline JSON (v4 规范)
 */
struct ResolvedCompanyConfig {
  std::filesystem::path conf_path;
  std::filesystem::path pipeline_path;
  std::filesystem::path model_root_path;
  std::string biz_name;
  CompanyAlgBizType biz_type = ALG_BIZ_TYPE_UNKNOWN;
  std::shared_ptr<IBizAdapter> adapter;
  const OperatorBizBridgeDescriptor* bridge_descriptor = nullptr;
  nlohmann::json synthetic_pipeline_json;
  ResolvedOutputPoolSpec output_pool_spec;
  ResolvedInputLimits input_limits;
};

/**
 * @brief 公司部署配置解析器
 */
class CompanyConfResolver {
 public:
  /**
   * @brief
   * 校验并规范化模型引用路径（允许文件尚不存在，但严格限制在沙箱根目录下）
   */
  static int ResolveModelReferenceUnderRoot(const std::filesystem::path& root,
                                            const std::string& rel_or_abs,
                                            const char* field_name,
                                            std::filesystem::path* out_path,
                                            std::string* error_msg) noexcept;

  /**
   * @brief 基于 model_path 根目录与相对 cfg_file_name 解析配置
   */
  static int Resolve(
      const char* model_path, const char* cfg_file_name, int32_t device_id,
      llm_edgeflow::operator_api::ComputePlatform compute_platform,
      ResolvedCompanyConfig* result, std::string* error_msg,
      uint32_t max_frame_depth = 25) noexcept;
};

}  // namespace alg_framework
