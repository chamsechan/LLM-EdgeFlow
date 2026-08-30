#include "demo/common/demo_options.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include "company_alg_log.h"
#include "demo/common/dataset_reader.h"
#include "nlohmann/json.hpp"

namespace alg_demo {

using llm_edgeflow::operator_api::ComputePlatform;

namespace {

std::string ToLower(std::string str) {
  std::transform(str.begin(), str.end(), str.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return str;
}

/**
 * @brief 严格整数解析函数，拒绝包含尾随非法字符的字符串 (P2-1)
 */
bool ParseStrictInt64(const std::string& str, int64_t* out_val) {
  if (str.empty()) return false;
  size_t idx = 0;
  try {
    int64_t val = std::stoll(str, &idx);
    if (idx != str.size()) {
      return false;  // 存在尾随非法字符 (如 "1abc")
    }
    if (out_val) *out_val = val;
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

void ConfigureLogLevelFromEnvironment() noexcept {
  const char* value = std::getenv("LLMEDGEFLOW_LEVEL");
  if (!value || value[0] < '0' || value[0] > '5' || value[1] != '\0') {
    return;
  }
  const int parsed_level = value[0] - '0';
  (void)AlgBase_setLogLevelByName(COMPANY_ALG_LOG_NAME, parsed_level);
}

bool ParseComputePlatform(const std::string& chip_str,
                          ComputePlatform* out_type) noexcept {
  if (!out_type) return false;
  std::string lower = ToLower(chip_str);

  if (lower == "ax650") {
    *out_type = ComputePlatform::kAx650;
    return true;
  } else if (lower == "ascend310p" || lower == "ascend_310p") {
    *out_type = ComputePlatform::kAscend310P;
    return true;
  } else if (lower == "ascend910b" || lower == "ascend_910b") {
    *out_type = ComputePlatform::kAscend910B;
    return true;
  } else if (lower == "rk3588") {
    *out_type = ComputePlatform::kRk3588;
    return true;
  } else if (lower == "cuda" || lower == "nvidia_gpu" || lower == "nvidiagpu") {
    *out_type = ComputePlatform::kCuda;
    return true;
  } else if (lower == "cpu" || lower == "cpu_generic") {
    *out_type = ComputePlatform::kCpu;
    return true;
  }

  *out_type = ComputePlatform::kUnknown;
  return false;
}

int ParseCommandLine(int argc, char* argv[], DemoOptions* out_options,
                     std::string* error_msg) {
  if (!out_options) {
    if (error_msg) *error_msg = "Null out_options pointer provided";
    return 2;
  }

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      out_options->show_help = true;
      return 0;
    } else if (arg == "-l" || arg == "--list") {
      out_options->list_only = true;
      return 0;
    } else if (arg == "-p" || arg == "--profile") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      out_options->profile = argv[++i];
      out_options->has_profile = true;
    } else if (arg == "-b" || arg == "--biz") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      out_options->biz = argv[++i];
      out_options->has_biz = true;
    } else if (arg == "--suite") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      std::string suite_str = argv[++i];
      if (suite_str != "smoke" && suite_str != "real" && suite_str != "all") {
        if (error_msg) {
          *error_msg = "Invalid --suite '" + suite_str +
                       "'. Must be smoke, real, or all.";
        }
        return 2;
      }
      out_options->suite = suite_str;
      out_options->has_suite = true;
    } else if (arg == "-c" || arg == "--config") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      out_options->config_path = argv[++i];
      out_options->has_config_path = true;
    } else if (arg == "-d" || arg == "--dataset") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      out_options->dataset_path = argv[++i];
      out_options->has_dataset_path = true;
    } else if (arg == "-o" || arg == "--output-dir") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      out_options->output_dir = argv[++i];
      out_options->has_output_dir = true;
    } else if (arg == "--batch-size") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      int64_t val = 0;
      if (!ParseStrictInt64(argv[++i], &val) || val <= 0 || val > 100000) {
        if (error_msg) {
          *error_msg = "Invalid integer for --batch-size: '" +
                       std::string(argv[i]) + "' (Must be integer 1..100000)";
        }
        return 2;
      }
      out_options->batch_size = static_cast<int>(val);
      out_options->has_batch_size = true;
    } else if (arg == "--device-id") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      int64_t val = 0;
      if (!ParseStrictInt64(argv[++i], &val) || val < 0 || val > 1024) {
        if (error_msg) {
          *error_msg = "Invalid integer for --device-id: '" +
                       std::string(argv[i]) + "' (Must be integer 0..1024)";
        }
        return 2;
      }
      out_options->device_id = static_cast<int>(val);
      out_options->has_device_id = true;
    } else if (arg == "--chip") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      out_options->chip = argv[++i];
      ComputePlatform dummy;
      if (!ParseComputePlatform(out_options->chip, &dummy)) {
        if (error_msg) {
          *error_msg = "Unsupported chip type: '" + out_options->chip +
                       "'. Allowed: ax650, ascend310p, ascend910b, rk3588, "
                       "cuda, cpu";
        }
        return 2;
      }
      out_options->has_chip = true;
    } else if (arg == "--depth") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      int64_t val = 0;
      if (!ParseStrictInt64(argv[++i], &val) || val <= 0 || val > 100000) {
        if (error_msg) {
          *error_msg = "Invalid integer for --depth: '" + std::string(argv[i]) +
                       "' (Must be integer 1..100000)";
        }
        return 2;
      }
      out_options->depth_num = static_cast<uint32_t>(val);
      out_options->has_depth_num = true;
    } else if (arg == "--control-file") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      out_options->control_file = argv[++i];
      out_options->has_control_file = true;
    } else if (arg == "--append") {
      out_options->append = true;
    } else if (arg == "--allow-fallback-sample") {
      out_options->allow_fallback_sample = true;
    } else {
      if (error_msg) *error_msg = "Unknown CLI option: '" + arg + "'";
      return 2;
    }
  }

  return 0;
}

int LoadAndValidateProfilesDocument(const std::string& profiles_path,
                                    nlohmann::json* out_root,
                                    std::string* error_msg) {
  if (!out_root) {
    if (error_msg) *error_msg = "Null out_root pointer";
    return 3;
  }

  std::string resolved_path =
      ResolvePath(profiles_path.empty() ? "demo/profiles.json" : profiles_path);
  std::ifstream ifs(resolved_path);
  if (!ifs.is_open()) {
    if (error_msg) {
      *error_msg = "Failed to open profiles file: " + resolved_path;
    }
    return 3;
  }

  nlohmann::json root;
  try {
    ifs >> root;
  } catch (const std::exception& e) {
    if (error_msg) {
      *error_msg =
          "Invalid JSON in profiles file '" + resolved_path + "': " + e.what();
    }
    return 3;
  }

  if (!root.is_object()) {
    if (error_msg)
      *error_msg = "Profiles root must be a JSON object in " + resolved_path;
    return 3;
  }

  if (!root.contains("schema_version") ||
      !root["schema_version"].is_number_integer()) {
    if (error_msg)
      *error_msg = "Missing or invalid 'schema_version' in profiles file: " +
                   resolved_path;
    return 3;
  }

  if (root["schema_version"].get<int>() != 2) {
    if (error_msg) {
      *error_msg = "Unsupported schema_version: " +
                   std::to_string(root["schema_version"].get<int>()) +
                   " (Expected: 2)";
    }
    return 3;
  }

  if (!root.contains("profiles") || !root["profiles"].is_object()) {
    if (error_msg)
      *error_msg =
          "Missing 'profiles' map object in profiles file: " + resolved_path;
    return 3;
  }

  const auto& profiles = root["profiles"];

  // 严格 Schema 校验所有 Profiles (逐字段类型与数值区间安全检验)
  for (const auto& [name, p] : profiles.items()) {
    if (name.empty()) {
      if (error_msg)
        *error_msg = "Profile name cannot be empty string in " + resolved_path;
      return 3;
    }
    if (!p.is_object()) {
      if (error_msg) *error_msg = "Profile '" + name + "' must be an object";
      return 3;
    }
    bool has_biz = p.contains("biz") && p["biz"].is_string() &&
                   !p["biz"].get<std::string>().empty();
    if (!has_biz) {
      if (error_msg)
        *error_msg =
            "Profile '" + name + "' must contain non-empty string 'biz'";
      return 3;
    }
    if (!p.contains("config") || !p["config"].is_string() ||
        p["config"].get<std::string>().empty()) {
      if (error_msg)
        *error_msg =
            "Profile '" + name + "' must contain non-empty string 'config'";
      return 3;
    }
    if (!p.contains("dataset") || !p["dataset"].is_string() ||
        p["dataset"].get<std::string>().empty()) {
      if (error_msg)
        *error_msg =
            "Profile '" + name + "' must contain non-empty string 'dataset'";
      return 3;
    }
    if (p.contains("suite")) {
      if (!p["suite"].is_string()) {
        if (error_msg) {
          *error_msg = "Profile '" + name +
                       "' field 'suite' must be string ('smoke' or 'real')";
        }
        return 3;
      }
      std::string s = p["suite"].get<std::string>();
      if (s != "smoke" && s != "real") {
        if (error_msg) {
          *error_msg = "Profile '" + name +
                       "' suite must be 'smoke' or 'real' (got '" + s + "')";
        }
        return 3;
      }
    }
    if (p.contains("batch_size")) {
      if (!p["batch_size"].is_number_integer()) {
        if (error_msg)
          *error_msg =
              "Profile '" + name + "' field 'batch_size' must be integer";
        return 3;
      }
      int64_t val = p["batch_size"].get<int64_t>();
      if (val <= 0 || val > 100000) {
        if (error_msg) {
          *error_msg = "Profile '" + name +
                       "' field 'batch_size' must be between 1 and 100000";
        }
        return 3;
      }
    }
    if (p.contains("device_id")) {
      if (!p["device_id"].is_number_integer()) {
        if (error_msg)
          *error_msg =
              "Profile '" + name + "' field 'device_id' must be integer";
        return 3;
      }
      int64_t val = p["device_id"].get<int64_t>();
      if (val < 0 || val > 1024) {
        if (error_msg) {
          *error_msg = "Profile '" + name +
                       "' field 'device_id' must be between 0 and 1024";
        }
        return 3;
      }
    }
    if (p.contains("depth")) {
      if (!p["depth"].is_number_integer()) {
        if (error_msg)
          *error_msg = "Profile '" + name + "' field 'depth' must be integer";
        return 3;
      }
      int64_t val = p["depth"].get<int64_t>();
      if (val <= 0 || val > 100000) {
        if (error_msg) {
          *error_msg = "Profile '" + name +
                       "' field 'depth' must be between 1 and 100000";
        }
        return 3;
      }
    }
    if (p.contains("chip")) {
      if (!p["chip"].is_string()) {
        if (error_msg)
          *error_msg = "Profile '" + name + "' field 'chip' must be a string";
        return 3;
      }
      ComputePlatform dummy;
      if (!ParseComputePlatform(p["chip"].get<std::string>(), &dummy)) {
        if (error_msg)
          *error_msg = "Profile '" + name + "' chip '" +
                       p["chip"].get<std::string>() + "' is not supported";
        return 3;
      }
    }
    if (p.contains("control_file") && !p["control_file"].is_string()) {
      if (error_msg)
        *error_msg =
            "Profile '" + name + "' field 'control_file' must be a string";
      return 3;
    }
  }

  *out_root = std::move(root);
  return 0;
}

int GetProfilesForSuite(const std::string& profiles_path,
                        const std::string& suite_name,
                        std::vector<std::string>* out_profiles,
                        std::string* error_msg) {
  if (!out_profiles) {
    if (error_msg) *error_msg = "Null out_profiles pointer";
    return 3;
  }
  out_profiles->clear();

  nlohmann::json root;
  int ret = LoadAndValidateProfilesDocument(profiles_path, &root, error_msg);
  if (ret != 0) {
    return ret;
  }

  const auto& profiles = root["profiles"];
  for (const auto& [name, p] : profiles.items()) {
    std::string s =
        p.contains("suite") ? p["suite"].get<std::string>() : "smoke";
    if (suite_name == "all" || s == suite_name) {
      out_profiles->push_back(name);
    }
  }

  return 0;
}

int LoadAndMergeProfiles(const std::string& profiles_path,
                         const DemoOptions& cli_options,
                         DemoOptions* out_options, std::string* error_msg) {
  if (!out_options) {
    if (error_msg) *error_msg = "Null out_options pointer";
    return 3;
  }

  *out_options = cli_options;

  if (cli_options.profile.empty()) {
    // 未指定 Profile，无需从配置文件合并
    return 0;
  }

  nlohmann::json root;
  int ret = LoadAndValidateProfilesDocument(profiles_path, &root, error_msg);
  if (ret != 0) {
    return ret;
  }

  const auto& profiles = root["profiles"];
  if (!profiles.contains(cli_options.profile)) {
    if (error_msg) {
      *error_msg =
          "Profile '" + cli_options.profile + "' not found in profiles file";
    }
    return 3;
  }

  const auto& p = profiles[cli_options.profile];
  std::string prof_biz = p["biz"].get<std::string>();
  std::string prof_cfg = p["config"].get<std::string>();
  std::string prof_data = p["dataset"].get<std::string>();

  // 冲突检查：若 CLI 显式提供了 --biz，必须与 Profile biz 完全一致
  if (cli_options.has_biz && cli_options.biz != prof_biz) {
    if (error_msg) {
      *error_msg = "Biz conflict: CLI specified '--biz " + cli_options.biz +
                   "' but profile '" + cli_options.profile + "' requires '" +
                   prof_biz + "'";
    }
    return 3;
  }

  // 严格合并优先级：默认值 < Profile < CLI显式参数 (P1-1)
  out_options->biz = cli_options.has_biz ? cli_options.biz : prof_biz;
  out_options->config_path =
      cli_options.has_config_path ? cli_options.config_path : prof_cfg;
  out_options->dataset_path =
      cli_options.has_dataset_path ? cli_options.dataset_path : prof_data;

  if (p.contains("suite") && !cli_options.has_suite) {
    out_options->suite = p["suite"].get<std::string>();
  }
  if (p.contains("batch_size") && !cli_options.has_batch_size) {
    out_options->batch_size = static_cast<int>(p["batch_size"].get<int64_t>());
  }
  if (p.contains("device_id") && !cli_options.has_device_id) {
    out_options->device_id = static_cast<int>(p["device_id"].get<int64_t>());
  }
  if (p.contains("chip") && !cli_options.has_chip) {
    out_options->chip = p["chip"].get<std::string>();
  }
  if (p.contains("depth") && !cli_options.has_depth_num) {
    out_options->depth_num = static_cast<uint32_t>(p["depth"].get<int64_t>());
  }
  if (p.contains("control_file") && !cli_options.has_control_file) {
    out_options->control_file = p["control_file"].get<std::string>();
  }

  // 终态芯片校验
  ComputePlatform dummy_chip;
  if (!ParseComputePlatform(out_options->chip, &dummy_chip)) {
    if (error_msg) {
      *error_msg = "Unsupported chip type: '" + out_options->chip + "'";
    }
    return 3;
  }

  return 0;
}

void PrintHelp(const char* program_name) {
  std::cout
      << "Usage: " << program_name << " [options]\n\n"
      << "Profile & Suite Options:\n"
      << "  -p, --profile <name>       Run with a pre-configured profile\n"
      << "  --suite <smoke|real|all>   Run an entire suite of profiles\n"
      << "  -l, --list                 List all available biz cases and "
         "profiles\n\n"
      << "Direct Execution Options:\n"
      << "  -b, --biz <name>           Target biz (e.g. entity_extract, "
         "doc_qa)\n"
      << "  -c, --config <path>         Operator deployment .conf path\n"
      << "  -d, --dataset <path>        Business dataset path\n"
      << "  -o, --output-dir <path>    Results output directory (default: "
         "./results)\n\n"
      << "Execution Tuning Options:\n"
      << "  --batch-size <n>           Max batch size for Operator execution "
         "(default: 1)\n"
      << "  --device-id <n>            Target hardware device ID (default: 0)\n"
      << "  --chip <name>              Compute platform name (ax650, "
         "ascend310p, "
         "ascend910b,\n"
      << "                             rk3588, cuda, cpu)\n"
      << "  --depth <n>                Output descriptor depth count (default: "
         "1)\n"
      << "  --control-file <path>      Runtime control parameters JSON file\n"
      << "  --append                   Append output to existing results file "
         "instead of overwriting\n"
      << "  --allow-fallback-sample    Allow using fallback inline samples if "
         "dataset is missing\n"
      << "  -h, --help                 Display this help message\n\n"
      << std::endl;
}

}  // namespace alg_demo
