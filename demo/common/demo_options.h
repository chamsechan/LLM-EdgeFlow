#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "operator/operator_interface.h"

namespace alg_demo {

/**
 * @brief Apply the optional LLMEDGEFLOW_LEVEL environment setting.
 *
 * Valid decimal values are 0 (Fatal) through 5 (Verbose). Missing or invalid
 * values silently preserve the current process-wide log level.
 */
void ConfigureLogLevelFromEnvironment() noexcept;

/**
 * @brief Demo 运行参数对象 (由命令行参数、Profile 配置与默认安全值合并而成)
 */
struct DemoOptions {
  std::string profile;  // 预定义运行配置 Profile 标识
  std::string biz;  // 业务标识名 (如 entity_extract, keyword_match 等)
  std::string config_path;               // Operator .conf 路径
  std::string dataset_path;              // 业务测试集文件路径
  std::string output_dir = "./results";  // 结果输出根目录

  int batch_size = 1;  // 最大批大小 (支持按批分块分发)
  int device_id = 0;   // 设备 ID
  std::string chip = "cpu";  // 计算平台芯片类型字符串 (受严格白名单校验)
  uint32_t depth_num = 1;  // 输出结构体预分配深度

  std::optional<std::string> control_file;  // 运行时 Control JSON 文件路径
  std::string suite;                   // 执行套件 ("smoke", "real", "all")
  bool append = false;                 // 结果文件是否追加模式
  bool allow_fallback_sample = false;  // 测试集缺失时是否允许使用内置样例
  bool list_only = false;  // 是否仅列出可用 Business 和 Profile
  bool show_help = false;  // 是否显示帮助信息

  // 显式跟踪 CLI 是否显式提供了特定参数 (解决 CLI 默认值无法可靠覆盖 Profile
  // 问题)
  bool has_profile = false;
  bool has_biz = false;
  bool has_config_path = false;
  bool has_dataset_path = false;
  bool has_output_dir = false;
  bool has_batch_size = false;
  bool has_device_id = false;
  bool has_chip = false;
  bool has_depth_num = false;
  bool has_control_file = false;
  bool has_suite = false;
};

/**
 * @brief 解析芯片计算平台字符串为强类型 ComputePlatform (严格白名单校验)
 * @param chip_str 芯片字符串 (支持大小写无关)
 * @param out_type 输出 ComputePlatform
 * @return true 成功, false 字符串不在白名单内
 */
bool ParseComputePlatform(
    const std::string& chip_str,
    llm_edgeflow::operator_api::ComputePlatform* out_type) noexcept;

/**
 * @brief 解析命令行参数填充 DemoOptions
 * @param argc 命令行参数数量
 * @param argv 命令行参数数组
 * @param out_options 解析输出选项
 * @param error_msg 错误输出信息
 * @return 0 成功/帮助, 非 0 错误退出码 (2: CLI 参数错误)
 */
int ParseCommandLine(int argc, char* argv[], DemoOptions* out_options,
                     std::string* error_msg);

/**
 * @brief 解析并严格校验 profiles.json 文档 (校验 schema_version, profiles
 * 结构与所有字段)
 * @param profiles_path profiles.json 路径 (支持相对或绝对路径，为空默认使用
 * demo/profiles.json)
 * @param out_root 输出解析并校验通过的 JSON 对象
 * @param error_msg 错误输出信息
 * @return 0 成功, 非 0 错误码 (3: 格式或配置错误)
 */
int LoadAndValidateProfilesDocument(const std::string& profiles_path,
                                    nlohmann::json* out_root,
                                    std::string* error_msg);

/**
 * @brief 从 demo/profiles.json 读取并与 CLI 参数进行合并
 *        优先级: 命令行显式参数 > Profile 配置 > 默认值
 * @param profiles_path profiles.json 路径
 * @param cli_options 命令行选项
 * @param out_options 合并后的最终选项
 * @param error_msg 错误输出信息
 * @return 0 成功, 非 0 错误码 (3: Profile 解析/校验错误)
 */
int LoadAndMergeProfiles(const std::string& profiles_path,
                         const DemoOptions& cli_options,
                         DemoOptions* out_options, std::string* error_msg);

/**
 * @brief 根据套件名称获取满足条件的 Profile 名称列表
 * @param profiles_path profiles.json 路径
 * @param suite_name 套件名 ("smoke", "real", "all")
 * @param out_profiles 输出 Profile 标识列表
 * @param error_msg 错误输出信息
 * @return 0 成功, 非 0 错误码 (3: 格式或配置错误)
 */
int GetProfilesForSuite(const std::string& profiles_path,
                        const std::string& suite_name,
                        std::vector<std::string>* out_profiles,
                        std::string* error_msg);

/**
 * @brief 打印 Demo CLI 帮助信息
 * @param program_name 应用程序名称
 */
void PrintHelp(const char* program_name);

}  // namespace alg_demo
