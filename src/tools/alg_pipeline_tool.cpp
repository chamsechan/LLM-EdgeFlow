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

using llm_edgeflow::PipelineCatalog;
using llm_edgeflow::PipelineValidator;
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
  if (!conf.is_object() || conf.size() != 1 || !conf.contains("data") ||
      !conf["data"].is_object() || !conf["data"].contains("pipe_path") ||
      !conf["data"]["pipe_path"].is_string()) {
    return std::nullopt;
  }
  const auto& data = conf["data"];
  fs::path pipe_path = data["pipe_path"].get<std::string>();
  if (pipe_path.is_relative()) {
    if (!fs::exists(pipe_path)) {
      pipe_path = conf_path.parent_path() / pipe_path;
    }
  }
  return pipe_path.lexically_normal();
}

nlohmann::json ProfilesJson(const std::string& biz_filter) {
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
      std::string pipeline_biz = pipeline.value("biz_name", "");
      if (!biz_filter.empty() && pipeline_biz != biz_filter) continue;
      result.push_back({{"name", name},
                        {"biz", profile.value("biz", "")},
                        {"pipeline_biz", pipeline_biz},
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
  std::cerr << "Usage:\n"
            << "  alg_pipeline_tool catalog [--biz|-b NAME]\n"
            << "  alg_pipeline_tool describe-node NODE_TYPE\n"
            << "  alg_pipeline_tool init [--biz|-b] NAME [--profile "
               "NAME|--empty]\n"
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
    std::string biz;
    if (argc == 4 &&
        (std::string(argv[2]) == "--biz" || std::string(argv[2]) == "-b")) {
      biz = argv[3];
    } else if (argc != 2) {
      Usage();
      return 2;
    }
    auto result = PipelineCatalog::ToJson(biz);
    result["profiles"] = ProfilesJson(biz);
    result["ok"] = biz.empty() || !result["bizs"].empty();
    std::cout << result.dump(2) << std::endl;
    return result["ok"].get<bool>() ? 0 : 1;
  }

  if (command == "describe-node") {
    if (argc != 3) {
      Usage();
      return 2;
    }
    const auto definition = PipelineCatalog::FindNode(argv[2]);
    if (!definition) {
      std::cout << Error("UNKNOWN_NODE_TYPE", argv[2]).dump(2) << std::endl;
      return 1;
    }
    auto result = PipelineCatalog::NodeToJson(*definition);
    result["schema_version"] = 2;
    result["ok"] = true;
    std::cout << result.dump(2) << std::endl;
    return 0;
  }

  if (command == "init") {
    std::string biz;
    std::string profile;
    bool empty = false;
    for (int i = 2; i < argc; ++i) {
      std::string arg = argv[i];
      if ((arg == "--biz" || arg == "-b") && i + 1 < argc)
        biz = argv[++i];
      else if (arg == "--profile" && i + 1 < argc)
        profile = argv[++i];
      else if (arg == "--empty")
        empty = true;
      else {
        Usage();
        return 2;
      }
    }
    if (biz.empty() || !PipelineCatalog::FindBiz(biz)) {
      std::cout << Error("UNKNOWN_BIZ", biz).dump(2) << std::endl;
      return 1;
    }
    if (!profile.empty() && !empty) {
      auto path = ProfilePipeline(profile);
      nlohmann::json pipeline;
      std::string error;
      if (!path || !ReadJson(path->string(), &pipeline, &error) ||
          pipeline.value("biz_name", "") != biz) {
        std::cout << Error("PROFILE_MISMATCH",
                           "Profile is unavailable or belongs to another "
                           "biz contract")
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
    nlohmann::json pipeline = {{"biz_name", biz},
                               {"models", nlohmann::json::array()},
                               {"pipeline", nlohmann::json::array()}};
    std::cout << nlohmann::json({{"schema_version", 1},
                                 {"ok", true},
                                 {"pipeline", std::move(pipeline)}})
                     .dump(2)
              << std::endl;
    return 0;
  }

  if (command == "validate" || command == "plan") {
    if (argc != 3) {
      Usage();
      return 2;
    }
    nlohmann::json root;
    std::string error;
    if (!ReadJson(argv[2], &root, &error)) {
      std::cout << Error("JSON_READ", error).dump(2) << std::endl;
      return 1;
    }
    auto report = PipelineValidator::Validate(root);
    auto result = report.ToJson();
    // A failed plan request must retain the exact Validator diagnostics so
    // every consumer observes the same fail-closed report. Successful plans
    // omit the empty diagnostics array to keep the established CLI shape.
    if (command == "plan" && report.ok) result.erase("diagnostics");
    std::cout << result.dump(2) << std::endl;
    return report.ok ? 0 : 1;
  }

  Usage();
  return 2;
}
