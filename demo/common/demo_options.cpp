#include "demo/common/demo_options.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

#include "demo/common/dataset_reader.h"
#include "nlohmann/json.hpp"

namespace alg_demo {

using llm_edgeflow::platform::ChipType;

namespace {

std::string ToLower(std::string str) {
  std::transform(str.begin(), str.end(), str.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return str;
}

const char* LegacyBizIdToName(int biz_id) {
  switch (biz_id) {
    case 1:
      return "entity_extract";
    case 2:
      return "keyword_match";
    case 3:
      return "doc_qa";
    case 4:
      return "dialogue_audit";
    case 5:
      return "ocr_doc_qa";
    case 6:
      return "audio_asr";
    case 7:
      return "cross_rerank";
    default:
      return "";
  }
}

}  // namespace

bool ParseChipType(const std::string& chip_str, ChipType* out_type) noexcept {
  if (!out_type) return false;
  std::string lower = ToLower(chip_str);

  if (lower == "ax650") {
    *out_type = ChipType::kAx650;
    return true;
  } else if (lower == "ascend310p" || lower == "ascend_310p") {
    *out_type = ChipType::kAscend310P;
    return true;
  } else if (lower == "ascend910b" || lower == "ascend_910b") {
    *out_type = ChipType::kAscend910B;
    return true;
  } else if (lower == "rk3588") {
    *out_type = ChipType::kRk3588;
    return true;
  } else if (lower == "nvidia_gpu" || lower == "nvidiagpu" || lower == "cuda") {
    *out_type = ChipType::kNvidiaGpu;
    return true;
  } else if (lower == "cpu_generic" || lower == "cpu") {
    *out_type = ChipType::kCpuGeneric;
    return true;
  }

  *out_type = ChipType::kUnknown;
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
    } else if (arg == "-b" || arg == "--business") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      out_options->business = argv[++i];
      out_options->has_business = true;
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
    } else if (arg == "--biz") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      try {
        int id = std::stoi(argv[++i]);
        out_options->legacy_biz_id = id;
        std::string mapped = LegacyBizIdToName(id);
        if (mapped.empty()) {
          if (error_msg) {
            *error_msg = "Unsupported legacy --biz ID: " + std::to_string(id) +
                         " (Must be 1..7)";
          }
          return 2;
        }
        std::cerr << "[DEPRECATION WARNING] Flag '--biz " << id
                  << "' is deprecated. Please use '--business " << mapped
                  << "' instead." << std::endl;
        if (!out_options->has_business) {
          out_options->business = mapped;
          out_options->has_business = true;
        }
      } catch (const std::exception&) {
        if (error_msg) *error_msg = "Invalid integer for --biz";
        return 2;
      }
    } else if (arg == "-c" || arg == "--config" || arg == "--conf") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      out_options->config_path = argv[++i];
      out_options->has_config_path = true;
    } else if (arg == "-d" || arg == "--dataset" || arg == "--data") {
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
      try {
        out_options->batch_size = std::stoi(argv[++i]);
        if (out_options->batch_size <= 0) {
          if (error_msg) *error_msg = "--batch-size must be > 0";
          return 2;
        }
        out_options->has_batch_size = true;
      } catch (...) {
        if (error_msg) *error_msg = "Invalid integer for --batch-size";
        return 2;
      }
    } else if (arg == "--device-id") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      try {
        out_options->device_id = std::stoi(argv[++i]);
        if (out_options->device_id < 0) {
          if (error_msg) *error_msg = "--device-id must be >= 0";
          return 2;
        }
        out_options->has_device_id = true;
      } catch (...) {
        if (error_msg) *error_msg = "Invalid integer for --device-id";
        return 2;
      }
    } else if (arg == "--chip") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      out_options->chip = argv[++i];
      ChipType dummy;
      if (!ParseChipType(out_options->chip, &dummy)) {
        if (error_msg) {
          *error_msg = "Unsupported chip type: '" + out_options->chip +
                       "'. Allowed: ax650, ascend310p, ascend910b, rk3588, "
                       "nvidia_gpu, cpu_generic";
        }
        return 2;
      }
      out_options->has_chip = true;
    } else if (arg == "--depth") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      try {
        int depth = std::stoi(argv[++i]);
        if (depth <= 0) {
          if (error_msg) *error_msg = "--depth must be > 0";
          return 2;
        }
        out_options->depth_num = static_cast<uint32_t>(depth);
        out_options->has_depth_num = true;
      } catch (...) {
        if (error_msg) *error_msg = "Invalid integer for --depth";
        return 2;
      }
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

int LoadAndMergeProfiles(const std::string& profiles_path,
                         const DemoOptions& cli_options,
                         DemoOptions* out_options, std::string* error_msg) {
  if (!out_options) {
    if (error_msg) *error_msg = "Null out_options pointer";
    return 3;
  }

  *out_options = cli_options;

  std::string resolved_path =
      ResolvePath(profiles_path.empty() ? "demo/profiles.json" : profiles_path);
  std::ifstream ifs(resolved_path);
  if (!ifs.is_open()) {
    if (cli_options.profile.empty()) {
      return 0;
    }
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
    if (error_msg) *error_msg = "Profiles root must be a JSON object";
    return 3;
  }

  if (!root.contains("schema_version") ||
      !root["schema_version"].is_number_integer()) {
    if (error_msg)
      *error_msg = "Missing or invalid 'schema_version' in profiles.json";
    return 3;
  }

  if (root["schema_version"].get<int>() != 1) {
    if (error_msg) {
      *error_msg = "Unsupported schema_version: " +
                   std::to_string(root["schema_version"].get<int>()) +
                   " (Expected: 1)";
    }
    return 3;
  }

  if (!root.contains("profiles") || !root["profiles"].is_object()) {
    if (error_msg) *error_msg = "Missing 'profiles' map in profiles.json";
    return 3;
  }

  const auto& profiles = root["profiles"];

  // 严格 Schema 校验所有 Profiles (P2-1)
  for (const auto& [name, p] : profiles.items()) {
    if (name.empty()) {
      if (error_msg)
        *error_msg = "Profile name cannot be empty string in profiles.json";
      return 3;
    }
    if (!p.is_object()) {
      if (error_msg) *error_msg = "Profile '" + name + "' must be an object";
      return 3;
    }
    if (!p.contains("business") || !p["business"].is_string() ||
        p["business"].get<std::string>().empty()) {
      if (error_msg)
        *error_msg =
            "Profile '" + name + "' must contain non-empty string 'business'";
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
        if (error_msg)
          *error_msg = "Profile '" + name + "' field 'suite' must be a string";
        return 3;
      }
      std::string s = p["suite"].get<std::string>();
      if (s != "smoke" && s != "real") {
        if (error_msg)
          *error_msg = "Profile '" + name + "' suite must be 'smoke' or 'real'";
        return 3;
      }
    }
    if (p.contains("batch_size")) {
      if (!p["batch_size"].is_number_integer() ||
          p["batch_size"].get<int>() <= 0) {
        if (error_msg)
          *error_msg = "Profile '" + name +
                       "' field 'batch_size' must be positive integer";
        return 3;
      }
    }
    if (p.contains("device_id")) {
      if (!p["device_id"].is_number_integer() ||
          p["device_id"].get<int>() < 0) {
        if (error_msg)
          *error_msg = "Profile '" + name +
                       "' field 'device_id' must be non-negative integer";
        return 3;
      }
    }
    if (p.contains("depth")) {
      if (!p["depth"].is_number_integer() || p["depth"].get<int>() <= 0) {
        if (error_msg)
          *error_msg =
              "Profile '" + name + "' field 'depth' must be positive integer";
        return 3;
      }
    }
    if (p.contains("chip")) {
      if (!p["chip"].is_string()) {
        if (error_msg)
          *error_msg = "Profile '" + name + "' field 'chip' must be a string";
        return 3;
      }
      ChipType dummy;
      if (!ParseChipType(p["chip"].get<std::string>(), &dummy)) {
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

  // 如果请求了特定 Profile
  if (!cli_options.profile.empty()) {
    if (!profiles.contains(cli_options.profile)) {
      if (error_msg) {
        *error_msg = "Profile '" + cli_options.profile + "' not found in " +
                     resolved_path;
      }
      return 3;
    }

    const auto& p = profiles[cli_options.profile];
    std::string prof_biz = p["business"].get<std::string>();
    std::string prof_cfg = p["config"].get<std::string>();
    std::string prof_data = p["dataset"].get<std::string>();

    // 冲突检查：若 CLI 显式提供了 --business，必须与 Profile business 完全一致
    if (cli_options.has_business && cli_options.business != prof_biz) {
      if (error_msg) {
        *error_msg = "Business conflict: CLI specified '--business " +
                     cli_options.business + "' but profile '" +
                     cli_options.profile + "' requires '" + prof_biz + "'";
      }
      return 3;
    }

    // 严格合并优先级：默认值 < Profile < CLI显式参数 (P1-1)
    out_options->business =
        cli_options.has_business ? cli_options.business : prof_biz;
    out_options->config_path =
        cli_options.has_config_path ? cli_options.config_path : prof_cfg;
    out_options->dataset_path =
        cli_options.has_dataset_path ? cli_options.dataset_path : prof_data;

    if (p.contains("suite") && !cli_options.has_suite) {
      out_options->suite = p["suite"].get<std::string>();
    }
    if (p.contains("batch_size") && !cli_options.has_batch_size) {
      out_options->batch_size = p["batch_size"].get<int>();
    }
    if (p.contains("device_id") && !cli_options.has_device_id) {
      out_options->device_id = p["device_id"].get<int>();
    }
    if (p.contains("chip") && !cli_options.has_chip) {
      out_options->chip = p["chip"].get<std::string>();
    }
    if (p.contains("depth") && !cli_options.has_depth_num) {
      out_options->depth_num = static_cast<uint32_t>(p["depth"].get<int>());
    }
    if (p.contains("control_file") && !cli_options.has_control_file) {
      out_options->control_file = p["control_file"].get<std::string>();
    }
  }

  // 终态芯片校验
  ChipType dummy_chip;
  if (!ParseChipType(out_options->chip, &dummy_chip)) {
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
      << "  -l, --list                 List all available business cases and "
         "profiles\n\n"
      << "Direct Execution Options:\n"
      << "  -b, --business <name>      Target business (e.g. entity_extract, "
         "doc_qa)\n"
      << "  -c, --config, --conf <path> Platform deployment .conf path\n"
      << "  -d, --dataset, --data <path> Business dataset path\n"
      << "  -o, --output-dir <path>    Results output directory (default: "
         "./results)\n\n"
      << "Execution Tuning Options:\n"
      << "  --batch-size <n>           Max batch size for Platform operator "
         "(default: 1)\n"
      << "  --device-id <n>            Target hardware device ID (default: 0)\n"
      << "  --chip <name>              Chip whitelist name (ax650, ascend310p, "
         "ascend910b,\n"
      << "                             rk3588, nvidia_gpu, cpu_generic)\n"
      << "  --depth <n>                Output descriptor depth count (default: "
         "1)\n"
      << "  --control-file <path>      Runtime control parameters JSON file\n"
      << "  --append                   Append output to existing results file "
         "instead of overwriting\n"
      << "  --allow-fallback-sample    Allow using fallback inline samples if "
         "dataset is missing\n"
      << "  -h, --help                 Display this help message\n\n"
      << "Legacy Compatibility:\n"
      << "  --biz <1..7>               Deprecated: Select business by numeric "
         "ID\n"
      << std::endl;
}

}  // namespace alg_demo
