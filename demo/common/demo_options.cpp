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
    } else if (arg == "-b" || arg == "--business") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      out_options->business = argv[++i];
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
        if (out_options->business.empty()) {
          out_options->business = mapped;
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
    } else if (arg == "-d" || arg == "--dataset" || arg == "--data") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      out_options->dataset_path = argv[++i];
    } else if (arg == "-o" || arg == "--output-dir") {
      if (i + 1 >= argc) {
        if (error_msg) *error_msg = "Missing value for argument: " + arg;
        return 2;
      }
      out_options->output_dir = argv[++i];
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
    // 如果没有显式指定 profile，且没有
    // profiles.json，且有直接命令行参数，则允许继续
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
    if (!p.is_object()) {
      if (error_msg)
        *error_msg =
            "Profile entry '" + cli_options.profile + "' must be an object";
      return 3;
    }

    // 必填字段校验
    if (!p.contains("business") || !p["business"].is_string() ||
        !p.contains("config") || !p["config"].is_string() ||
        !p.contains("dataset") || !p["dataset"].is_string()) {
      if (error_msg) {
        *error_msg =
            "Profile '" + cli_options.profile +
            "' must contain string fields: 'business', 'config', 'dataset'";
      }
      return 3;
    }

    std::string prof_biz = p["business"].get<std::string>();
    std::string prof_cfg = p["config"].get<std::string>();
    std::string prof_data = p["dataset"].get<std::string>();

    if (p.contains("suite")) {
      std::string suite = p["suite"].get<std::string>();
      if (suite != "smoke" && suite != "real") {
        if (error_msg) {
          *error_msg = "Profile '" + cli_options.profile +
                       "' suite must be 'smoke' or 'real'";
        }
        return 3;
      }
    }

    // CLI > Profile 覆盖合并
    if (cli_options.business.empty()) {
      out_options->business = prof_biz;
    } else if (cli_options.business != prof_biz) {
      if (error_msg) {
        *error_msg = "Business conflict: CLI specified '--business " +
                     cli_options.business + "' but profile '" +
                     cli_options.profile + "' requires '" + prof_biz + "'";
      }
      return 3;
    }

    if (cli_options.config_path.empty()) {
      out_options->config_path = prof_cfg;
    }
    if (cli_options.dataset_path.empty()) {
      out_options->dataset_path = prof_data;
    }

    if (p.contains("batch_size") && p["batch_size"].is_number_integer() &&
        cli_options.batch_size == 1) {
      out_options->batch_size = p["batch_size"].get<int>();
    }
    if (p.contains("device_id") && p["device_id"].is_number_integer() &&
        cli_options.device_id == 0) {
      out_options->device_id = p["device_id"].get<int>();
    }
    if (p.contains("chip") && p["chip"].is_string() &&
        cli_options.chip == "ax650") {
      out_options->chip = p["chip"].get<std::string>();
    }
    if (p.contains("depth") && p["depth"].is_number_integer() &&
        cli_options.depth_num == 1) {
      out_options->depth_num = static_cast<uint32_t>(p["depth"].get<int>());
    }
    if (p.contains("control_file") && p["control_file"].is_string() &&
        !cli_options.control_file.has_value()) {
      out_options->control_file = p["control_file"].get<std::string>();
    }
  }

  // 终态校验
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
      << "Profile Options:\n"
      << "  -p, --profile <name>       Run with a pre-configured profile\n"
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
