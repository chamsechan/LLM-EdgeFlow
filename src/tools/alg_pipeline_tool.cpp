#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "nlohmann/json.hpp"

namespace {

using alg_framework::PipelineCatalog;
using alg_framework::PipelineValidator;
namespace fs = std::filesystem;

nlohmann::json Error(const std::string& code, const std::string& message) {
  return {{"schema_version", 1},
          {"ok", false},
          {"diagnostics", nlohmann::json::array({{{"code", code},
                                                  {"path", "/"},
                                                  {"message", message},
                                                  {"severity", "error"}}})}};
}

bool ReadJson(const std::string& path, nlohmann::json* output,
              std::string* error) {
  try {
    if (path == "--stdin") {
      std::cin >> *output;
    } else {
      std::ifstream stream(path);
      if (!stream.is_open()) {
        if (error) *error = "Cannot open JSON file: " + path;
        return false;
      }
      stream >> *output;
    }
    return true;
  } catch (const std::exception& exception) {
    if (error) *error = exception.what();
    return false;
  }
}

std::optional<fs::path> ProfilePipeline(
    const std::string& profile, nlohmann::json* profile_json = nullptr) {
  std::ifstream profiles_stream("demo/profiles.json");
  if (!profiles_stream.is_open()) return std::nullopt;
  nlohmann::json root;
  profiles_stream >> root;
  if (!root.contains("profiles") || !root["profiles"].contains(profile))
    return std::nullopt;
  const auto& selected = root["profiles"][profile];
  if (profile_json) *profile_json = selected;
  fs::path conf_path = selected["config"].get<std::string>();
  std::ifstream conf_stream(conf_path);
  if (!conf_stream.is_open()) return std::nullopt;
  nlohmann::json conf;
  conf_stream >> conf;
  const auto& data = conf.contains("data") ? conf["data"] : conf;
  fs::path pipe_path = data["pipe_path"].get<std::string>();
  if (pipe_path.is_relative()) pipe_path = conf_path.parent_path() / pipe_path;
  return pipe_path.lexically_normal();
}

nlohmann::json ProfilesJson(const std::string& business_filter) {
  nlohmann::json result = nlohmann::json::array();
  std::ifstream stream("demo/profiles.json");
  if (!stream.is_open()) return result;
  nlohmann::json root;
  try {
    stream >> root;
    for (const auto& [name, profile] : root["profiles"].items()) {
      auto pipeline_path = ProfilePipeline(name);
      if (!pipeline_path) continue;
      nlohmann::json pipeline;
      std::string error;
      if (!ReadJson(pipeline_path->string(), &pipeline, &error)) continue;
      std::string pipeline_business = pipeline.value("business_name", "");
      if (!business_filter.empty() && pipeline_business != business_filter)
        continue;
      result.push_back({{"name", name},
                        {"business", profile.value("business", "")},
                        {"pipeline_business", pipeline_business},
                        {"config", profile.value("config", "")},
                        {"dataset", profile.value("dataset", "")},
                        {"suite", profile.value("suite", "smoke")},
                        {"batch_size", profile.value("batch_size", 1)},
                        {"device_id", profile.value("device_id", 0)},
                        {"chip", profile.value("chip", "ax650")}});
    }
  } catch (...) {
    return nlohmann::json::array();
  }
  return result;
}

void Usage() {
  std::cerr
      << "Usage:\n"
      << "  alg_pipeline_tool catalog [--business NAME]\n"
      << "  alg_pipeline_tool describe-node NODE_TYPE\n"
      << "  alg_pipeline_tool init --business NAME [--profile NAME|--empty]\n"
      << "  alg_pipeline_tool normalize --explicit-dag FILE|--stdin\n"
      << "  alg_pipeline_tool validate FILE|--stdin\n"
      << "  alg_pipeline_tool plan FILE|--stdin\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    Usage();
    return 2;
  }
  const std::string command = argv[1];

  if (command == "catalog") {
    std::string business;
    if (argc == 4 && std::string(argv[2]) == "--business") business = argv[3];
    auto result = PipelineCatalog::ToJson(business);
    result["profiles"] = ProfilesJson(business);
    result["ok"] = business.empty() || !result["businesses"].empty();
    std::cout << result.dump(2) << std::endl;
    return result["ok"].get<bool>() ? 0 : 1;
  }

  if (command == "describe-node") {
    if (argc != 3) {
      Usage();
      return 2;
    }
    const auto* definition = PipelineCatalog::FindNode(argv[2]);
    if (!definition) {
      std::cout << Error("UNKNOWN_NODE_TYPE", argv[2]).dump(2) << std::endl;
      return 1;
    }
    auto result = PipelineCatalog::NodeToJson(*definition);
    result["schema_version"] = 1;
    result["ok"] = true;
    std::cout << result.dump(2) << std::endl;
    return 0;
  }

  if (command == "init") {
    std::string business;
    std::string profile;
    bool empty = false;
    for (int i = 2; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--business" && i + 1 < argc)
        business = argv[++i];
      else if (arg == "--profile" && i + 1 < argc)
        profile = argv[++i];
      else if (arg == "--empty")
        empty = true;
      else {
        Usage();
        return 2;
      }
    }
    if (business.empty() || !PipelineCatalog::FindBusiness(business)) {
      std::cout << Error("UNKNOWN_BUSINESS", business).dump(2) << std::endl;
      return 1;
    }
    if (!profile.empty() && !empty) {
      auto path = ProfilePipeline(profile);
      nlohmann::json pipeline;
      std::string error;
      if (!path || !ReadJson(path->string(), &pipeline, &error) ||
          pipeline.value("business_name", "") != business) {
        std::cout << Error("PROFILE_MISMATCH",
                           "Profile is unavailable or belongs to another "
                           "business contract")
                         .dump(2)
                  << std::endl;
        return 1;
      }
      std::cout << nlohmann::json({{"schema_version", 1},
                                   {"ok", true},
                                   {"pipeline", std::move(pipeline)}})
                       .dump(2)
                << std::endl;
      return 0;
    }
    nlohmann::json pipeline = {{"business_name", business},
                               {"models", nlohmann::json::array()},
                               {"pipeline", nlohmann::json::array()}};
    std::cout << nlohmann::json({{"schema_version", 1},
                                 {"ok", true},
                                 {"pipeline", std::move(pipeline)}})
                     .dump(2)
              << std::endl;
    return 0;
  }

  if (command == "validate" || command == "plan" || command == "normalize") {
    int path_index = 2;
    if (command == "normalize") {
      if (argc != 4 || std::string(argv[2]) != "--explicit-dag") {
        Usage();
        return 2;
      }
      path_index = 3;
    } else if (argc != 3) {
      Usage();
      return 2;
    }
    nlohmann::json root;
    std::string error;
    if (!ReadJson(argv[path_index], &root, &error)) {
      std::cout << Error("JSON_READ", error).dump(2) << std::endl;
      return 1;
    }
    if (command == "normalize") {
      nlohmann::json normalized;
      alg_framework::ValidationDiagnostic diagnostic;
      if (!PipelineValidator::NormalizeExplicitDag(root, &normalized,
                                                   &diagnostic)) {
        std::cout << Error(diagnostic.code, diagnostic.message).dump(2)
                  << std::endl;
        return 1;
      }
      std::cout << nlohmann::json({{"schema_version", 1},
                                   {"ok", true},
                                   {"pipeline", std::move(normalized)}})
                       .dump(2)
                << std::endl;
      return 0;
    }
    auto report = PipelineValidator::Validate(root);
    auto result = report.ToJson();
    if (command == "plan") result.erase("diagnostics");
    std::cout << result.dump(2) << std::endl;
    return report.ok ? 0 : 1;
  }

  Usage();
  return 2;
}
