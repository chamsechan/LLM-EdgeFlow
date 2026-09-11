#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "adapter/operator/operator_config_resolver.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "edgeflow/operator/interface.h"
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

nlohmann::json ResolveConf(const std::string& file, const std::string& root,
                           uint32_t depth) {
  using namespace llm_edgeflow;
  const auto ops = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  if (ops.Init() != 0)
    return Error("REGISTRY_CONFLICT", operator_api::GetOperatorLastError());
  struct RegistryGuard {
    operator_api::OperatorFunc ops;
    ~RegistryGuard() { ops.Deinit(); }
  } registry_guard{ops};
  ResolvedOperatorConfig resolved;
  std::string error;
  if (OperatorConfigResolver::Resolve(root.c_str(), file.c_str(), &resolved,
                                      &error, depth) != 0)
    return Error("DEPLOYMENT_CONFIG", error);
  const auto plan =
      PipelineValidator::ValidateAndPlan(resolved.synthetic_pipeline_json);
  if (!plan.report.ok) return plan.report.ToJson();

  auto effective = resolved.synthetic_pipeline_json;
  for (const auto& [id, node] : plan.node_plans)
    effective["pipeline"][node.node.source_index]["config"] =
        node.normalized_config;
  for (const auto& model : plan.models) {
    effective["models"][model.source_index]["model_config"] =
        model.normalized_model_config;
    effective["models"][model.source_index]["backend_config"] =
        model.normalized_backend_config;
  }
  nlohmann::json conf;
  if (!ReadJson(resolved.conf_path.string(), &conf, &error))
    return Error("JSON_READ", error);
  const auto overrides =
      conf["data"].value("model_paths", nlohmann::json::object());
  nlohmann::json paths = nlohmann::json::array();
  for (const auto& model : plan.models)
    paths.push_back({{"model_id", model.model_id},
                     {"source", overrides.contains(model.model_id)
                                    ? "conf.data.model_paths"
                                    : "pipeline.models.model_path"},
                     {"resolved", model.resolved_model_path}});
  nlohmann::json output_pools = nlohmann::json::object();
  for (const auto& [slot, pool] : resolved.output_pool_specs) {
    output_pools[slot] = {{"type", pool.type},
                          {"allocator", pool.allocator},
                          {"params", resolved.output_parameter_text.at(slot)},
                          {"meta_num", pool.meta_num},
                          {"metadata_type_id", pool.metadata_type_id},
                          {"capacities", pool.capacities}};
  }
  nlohmann::json configuration = {
      {"conf_path", resolved.conf_path.string()},
      {"pipeline_path", resolved.pipeline_path.string()},
      {"model_root", resolved.model_root_path.string()},
      {"effective_pipeline", std::move(effective)},
      {"model_paths", std::move(paths)},
      {"output_pools", output_pools}};
  if (output_pools.size() == 1) {
    configuration["output_pool"] = output_pools.begin().value();
  }
  return {{"schema_version", 1},
          {"ok", true},
          {"configuration", std::move(configuration)}};
}

void Usage() {
  std::cerr << "Usage:\n"
            << "  alg_pipeline_tool catalog [--biz|-b NAME]\n"
            << "  alg_pipeline_tool describe-node NODE_TYPE\n"
            << "  alg_pipeline_tool init --biz|-b NAME [--profile "
               "NAME|--empty] [--raw]\n"
            << "  alg_pipeline_tool validate FILE|--stdin [--explain]\n"
            << "  alg_pipeline_tool plan FILE|--stdin [--explain]\n";
  std::cerr
      << "  alg_pipeline_tool resolve-conf FILE [--root DIR] [--depth N]\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    Usage();
    return 2;
  }
  const std::string command = argv[1];

  if (command == "resolve-conf") {
    if (argc < 3 || argc % 2 == 0) {
      Usage();
      return 2;
    }
    std::string root = ".";
    uint32_t depth = 25;
    bool has_root = false;
    bool has_depth = false;
    for (int i = 3; i < argc; i += 2) {
      const std::string option = argv[i];
      const std::string value = argv[i + 1];
      if (option == "--root" && !has_root && !value.empty()) {
        root = value;
        has_root = true;
      } else if (option == "--depth" && !has_depth) {
        const auto parsed =
            std::from_chars(value.data(), value.data() + value.size(), depth);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != value.data() + value.size()) {
          Usage();
          return 2;
        }
        has_depth = true;
      } else {
        Usage();
        return 2;
      }
    }
    nlohmann::json result;
    try {
      result = ResolveConf(argv[2], root, depth);
    } catch (const std::exception& error) {
      result = Error("DEPLOYMENT_CONFIG", error.what());
    }
    std::cout << result.dump(2) << std::endl;
    return result.value("ok", false) ? 0 : 1;
  }

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
    bool raw = false;
    for (int i = 2; i < argc; ++i) {
      std::string arg = argv[i];
      const bool has_value =
          i + 1 < argc && argv[i + 1][0] != '\0' && argv[i + 1][0] != '-';
      if ((arg == "--biz" || arg == "-b") && biz.empty() && has_value)
        biz = argv[++i];
      else if (arg == "--profile" && profile.empty() && has_value)
        profile = argv[++i];
      else if (arg == "--empty" && !empty)
        empty = true;
      else if (arg == "--raw" && !raw)
        raw = true;
      else {
        Usage();
        return 2;
      }
    }
    if (biz.empty() || (empty && !profile.empty())) {
      Usage();
      return 2;
    }
    if (!PipelineCatalog::FindBiz(biz)) {
      std::cout << Error("UNKNOWN_BIZ", biz).dump(2) << std::endl;
      return 1;
    }
    nlohmann::json pipeline = {{"biz_name", biz},
                               {"models", nlohmann::json::array()},
                               {"pipeline", nlohmann::json::array()}};
    if (!profile.empty()) {
      auto path = ProfilePipeline(profile);
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
    }
    auto result = raw ? std::move(pipeline)
                      : nlohmann::json({{"schema_version", 1},
                                        {"ok", true},
                                        {"pipeline", std::move(pipeline)}});
    std::cout << result.dump(2) << std::endl;
    return 0;
  }

  if (command == "validate" || command == "plan") {
    if (argc < 3 || argc > 4) {
      Usage();
      return 2;
    }
    bool explain = false;
    std::string file;
    for (int i = 2; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--explain") {
        explain = true;
      } else if (file.empty()) {
        file = arg;
      } else {
        Usage();
        return 2;
      }
    }
    if (file.empty()) {
      Usage();
      return 2;
    }
    nlohmann::json root;
    std::string error;
    if (!ReadJson(file, &root, &error)) {
      std::cout << Error("JSON_READ", error).dump(2) << std::endl;
      return 1;
    }
    auto report = explain ? PipelineValidator::Explain(root)
                          : PipelineValidator::Validate(root);
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
