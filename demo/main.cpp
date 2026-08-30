#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_options.h"
#include "demo/common/demo_registry.h"
#include "nlohmann/json.hpp"
#include "operator/operator_interface.h"

using namespace alg_demo;
using namespace llm_edgeflow::operator_api;

namespace {

struct OperatorGlobalGuard {
  OperatorFunc ops;
  bool initialized = false;

  OperatorGlobalGuard() {
    ops = Get_LLM_EDGEFLOW_OperatorTable();
    if (ops.Init && ops.Init() == 0) {
      initialized = true;
    }
  }

  ~OperatorGlobalGuard() {
    if (initialized && ops.Deinit) {
      ops.Deinit();
      initialized = false;
    }
  }
};

void ListProfilesAndBizs() {
  std::cout << "\n=== Registered Biz Cases ===" << std::endl;
  auto descs = DemoRegistry::Instance().ListDescriptors();
  for (const auto& d : descs) {
    std::cout << "  - " << d.biz_name << " (" << d.display_title << ")"
              << std::endl;
  }

  std::cout << "\n=== Configured Profiles (demo/profiles.json) ==="
            << std::endl;
  nlohmann::json root;
  std::string err;
  if (LoadAndValidateProfilesDocument("", &root, &err) == 0) {
    for (const auto& [name, p] : root["profiles"].items()) {
      std::string biz = p["biz"].get<std::string>();
      std::string suite =
          p.contains("suite") ? p["suite"].get<std::string>() : "smoke";
      std::string cfg = p["config"].get<std::string>();
      std::cout << "  - " << name << " [suite: " << suite << ", biz: " << biz
                << ", cfg: " << cfg << "]" << std::endl;
    }
  } else {
    std::cout << "  (" << err << ")" << std::endl;
  }
  std::cout << std::endl;
}

int RunSuite(const std::string& suite_name, const DemoOptions& base_cli_opts) {
  std::vector<std::string> target_profiles;
  std::string err;
  int ret = GetProfilesForSuite("", suite_name, &target_profiles, &err);
  if (ret != 0) {
    std::cerr << "[Main ERROR] Failed to load suite '" << suite_name
              << "': " << err << std::endl;
    return ret;
  }

  if (target_profiles.empty()) {
    std::cerr << "[Main WARN] No profiles found matching suite: " << suite_name
              << std::endl;
    return 0;
  }

  std::cout << "#############################################################"
               "#####\n"
            << "   LLM-EdgeFlow Demo Suite: [" << suite_name << "] ("
            << target_profiles.size() << " Profiles)\n"
            << "#############################################################"
               "#####\n";

  for (const auto& prof : target_profiles) {
    DemoOptions cli_opt = base_cli_opts;
    cli_opt.profile = prof;
    cli_opt.has_profile = true;

    DemoOptions merged_opt;
    ret = LoadAndMergeProfiles("", cli_opt, &merged_opt, &err);
    if (ret != 0) {
      std::cerr << "[Main ERROR] Failed to load profile '" << prof
                << "': " << err << std::endl;
      return ret;
    }

    const auto* desc = DemoRegistry::Instance().Find(merged_opt.biz);
    if (!desc) {
      std::cerr << "[Main ERROR] Biz '" << merged_opt.biz
                << "' not registered for profile '" << prof << "'" << std::endl;
      return 3;
    }

    ret = desc->run(merged_opt);
    if (ret != 0) {
      std::cerr << "[Main ERROR] Execution failed for profile: " << prof
                << " with code " << ret << std::endl;
      return ret;
    }
  }

  std::cout << "\n#############################################################"
               "#####\n"
            << "   ALL PROFILES IN SUITE [" << suite_name
            << "] EXECUTED SUCCESSFULLY!\n"
            << "#############################################################"
               "#####\n";
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  ConfigureLogLevelFromEnvironment();

  // 1. 解析命令行参数
  DemoOptions cli_options;
  std::string cli_err;
  int parse_ret = ParseCommandLine(argc, argv, &cli_options, &cli_err);
  if (parse_ret != 0) {
    std::cerr << "[CLI ERROR] " << cli_err << "\n" << std::endl;
    PrintHelp(argv[0]);
    return parse_ret;
  }

  if (cli_options.show_help) {
    PrintHelp(argv[0]);
    return 0;
  }

  if (cli_options.list_only) {
    ListProfilesAndBizs();
    return 0;
  }

  // 2. 初始化 Operator 全局环境 (RAII 自动管理 Init / Deinit)
  OperatorGlobalGuard global_guard;
  if (!global_guard.initialized) {
    std::cerr << "[Main ERROR] Global Operator Init failed: "
              << GetOperatorLastError() << std::endl;
    return 5;
  }

  // 3. 判断是否为 Suite 批量运行模式 (显式 --suite 或无参数默认 smoke)
  if (cli_options.has_suite) {
    return RunSuite(cli_options.suite, cli_options);
  }
  if (!cli_options.has_profile && !cli_options.has_biz &&
      !cli_options.has_config_path) {
    return RunSuite("smoke", cli_options);
  }

  // 4. 合并 Profile 与命令行配置 (单 Profile / 单业务模式)
  DemoOptions options;
  std::string merge_err;
  int merge_ret = LoadAndMergeProfiles("", cli_options, &options, &merge_err);
  if (merge_ret != 0) {
    std::cerr << "[Config ERROR] " << merge_err << std::endl;
    return merge_ret;
  }

  if (options.biz.empty()) {
    std::cerr << "[Main ERROR] No biz specified. Use --profile <name> or "
                 "--biz <name>."
              << std::endl;
    return 2;
  }

  // 5. 查找并分发业务
  const auto* desc = DemoRegistry::Instance().Find(options.biz);
  if (!desc) {
    std::cerr << "[Main ERROR] Unsupported or unregistered biz: '"
              << options.biz << "'" << std::endl;
    return 3;
  }

  int ret = desc->run(options);
  return ret;
}
