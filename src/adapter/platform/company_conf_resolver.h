#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include "adapter/business_adapter_interface.h"
#include "nlohmann/json.hpp"
#include "platform/platform_operator_interface.h"

namespace alg_framework {

/**
 * @brief 解析后的公司平台部署配置与合成 Pipeline JSON
 */
struct ResolvedCompanyConfig {
  std::filesystem::path conf_path;
  std::filesystem::path pipeline_path;
  std::optional<std::filesystem::path> single_model_path;
  std::unordered_map<std::string, std::filesystem::path> model_paths_by_id;
  std::string business_name;
  CompanyAlgBizType biz_type = ALG_BIZ_TYPE_UNKNOWN;
  std::shared_ptr<IBusinessAdapter> adapter;
  nlohmann::json synthetic_pipeline_json;
};

/**
 * @brief 公司平台部署配置解析器 (集中收敛 .conf 解析与 Pipeline JSON 覆盖逻辑)
 */
class CompanyConfResolver {
 public:
  /**
   * @brief 解析 .conf 并生成可直接用于 Pipeline::BuildFromJson 的配置对象
   * @param[in] conf_file .conf 路径
   * @param[in] platform_config 平台运行配置
   * @param[out] result 解析结果
   * @param[out] error_msg 结构化诊断错误信息
   * @return true 成功, false 失败
   */
  static bool Resolve(
      const std::string& conf_file,
      const llm_edgeflow::platform::PlatformConfig& platform_config,
      ResolvedCompanyConfig* result, std::string* error_msg) noexcept;
};

}  // namespace alg_framework
