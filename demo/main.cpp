#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_options.h"
#include "demo/common/demo_registry.h"
#include "nlohmann/json.hpp"
#include "platform/platform_operator_interface.h"

using namespace alg_demo;
using namespace llm_edgeflow::platform;

namespace {

struct PlatformGlobalGuard {
  OperatorFunc ops;
  bool initialized = false;

  PlatformGlobalGuard() {
    ops = Get_LLM_EDGEFLOW_OperatorTable();
    if (ops.Init && ops.Init() == 0) {
      initialized = true;
    }
  }

  ~PlatformGlobalGuard() {
    if (initialized && ops.Deinit) {
      ops.Deinit();
      initialized = false;
    }
  }
};

void ListProfilesAndBusinesses() {
  std::cout << "\n=== Registered Business Cases ===" << std::endl;
  auto descs = DemoRegistry::Instance().ListDescriptors();
  for (const auto& d : descs) {
    std::cout << "  - " << d.business_name << " (" << d.display_title << ")"
              << std::endl;
  }

  std::cout << "\n=== Configured Profiles (demo/profiles.json) ==="
            << std::endl;
  std::string profiles_path = ResolvePath("demo/profiles.json");
  std::ifstream ifs(profiles_path);
  if (ifs.is_open()) {
    try {
      nlohmann::json root;
      ifs >> root;
      if (root.contains("profiles") && root["profiles"].is_object()) {
        for (const auto& [name, p] : root["profiles"].items()) {
          std::string biz = p.value("business", "unknown");
          std::string suite = p.value("suite", "smoke");
          std::string cfg = p.value("config", "");
          std::cout << "  - " << name << " [suite: " << suite
                    << ", biz: " << biz << ", cfg: " << cfg << "]" << std::endl;
        }
      }
    } catch (...) {
      std::cout << "  (Failed to parse profiles.json)" << std::endl;
    }
  } else {
    std::cout << "  (profiles.json not found)" << std::endl;
  }
  std::cout << std::endl;
}

int RunSuite(const std::string& suite_name, const DemoOptions& base_cli_opts) {
  std::string profiles_path = ResolvePath("demo/profiles.json");
  std::ifstream ifs(profiles_path);
  if (!ifs.is_open()) {
    std::cerr << "[Main ERROR] Cannot open profiles file: " << profiles_path
              << std::endl;
    return 3;
  }

  nlohmann::json root;
  try {
    ifs >> root;
  } catch (const std::exception& e) {
    std::cerr << "[Main ERROR] Invalid JSON in profiles file: " << e.what()
              << std::endl;
    return 3;
  }

  if (!root.contains("profiles") || !root["profiles"].is_object()) {
    std::cerr << "[Main ERROR] Missing 'profiles' object in profiles.json"
              << std::endl;
    return 3;
  }

  std::vector<std::string> target_profiles;
  for (const auto& [prof_name, prof_data] : root["profiles"].items()) {
    std::string s = prof_data.value("suite", "smoke");
    if (suite_name == "all" || s == suite_name) {
      target_profiles.push_back(prof_name);
    }
  }

  if (target_profiles.empty()) {
    std::cerr << "[Main ERROR] No profiles found matching suite '" << suite_name
              << "'" << std::endl;
    return 3;
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
    std::string err;
    int ret = LoadAndMergeProfiles("", cli_opt, &merged_opt, &err);
    if (ret != 0) {
      std::cerr << "[Main ERROR] Failed to load profile '" << prof
                << "': " << err << std::endl;
      return ret;
    }

    const auto* desc = DemoRegistry::Instance().Find(merged_opt.business);
    if (!desc) {
      std::cerr << "[Main ERROR] Business '" << merged_opt.business
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
    ListProfilesAndBusinesses();
    return 0;
  }

  // 2. 初始化平台全局环境 (RAII 自动管理 Init / Deinit)
  PlatformGlobalGuard global_guard;
  if (!global_guard.initialized) {
    std::cerr << "[Main ERROR] Global Platform Init failed: "
              << GetPlatformLastError() << std::endl;
    return 5;
  }

  // 3. 判断是否为 Suite 批量运行模式 (显式 --suite 或无参数默认 smoke)
  if (cli_options.has_suite) {
    return RunSuite(cli_options.suite, cli_options);
  }
  if (!cli_options.has_profile && !cli_options.has_business &&
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

  if (options.business.empty()) {
    std::cerr << "[Main ERROR] No business specified. Use --profile <name> or "
                 "--business <name>."
              << std::endl;
    return 2;
  }

  // 5. 查找并分发业务
  const auto* desc = DemoRegistry::Instance().Find(options.business);
  if (!desc) {
    std::cerr << "[Main ERROR] Unsupported or unregistered business: '"
              << options.business << "'" << std::endl;
    return 3;
  }

  int ret = desc->run(options);
  return ret;
}
