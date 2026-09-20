#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "adapter/deployment_io_config.h"
#include "adapter/io_binding_registry.h"
#include "adapter/io_binding_resolver.h"
#include "adapter/io_catalog.h"
#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_config_resolver.h"
#include "adapter/pipeline_document.h"
#include "core/common_contracts.h"
#include "core/diagnostic_code.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "demo/common/demo_profile_defaults.h"
#include "edgeflow/operator/interface.h"
#include "nlohmann/json.hpp"
#include "pipeline_document_validation.h"
#include "tools/pipeline_authoring.h"

namespace {

using llm_edgeflow::DiagnosticCode;
using llm_edgeflow::DiagnosticCodeName;
using llm_edgeflow::PipelineCatalog;
using llm_edgeflow::PipelineValidator;
namespace fs = std::filesystem;

nlohmann::json PipelineError(DiagnosticCode code, const std::string& message) {
  return {{"schema_version", 1},
          {"ok", false},
          {"diagnostics",
           nlohmann::json::array({{{"code", DiagnosticCodeName(code)},
                                   {"path", "/"},
                                   {"message", message},
                                   {"severity", "error"}}})}};
}

nlohmann::json ToolError(const std::string& code, const std::string& message,
                         const std::string& path = "/") {
  return {{"schema_version", 1},
          {"ok", false},
          {"diagnostics", nlohmann::json::array({{{"code", code},
                                                  {"path", path},
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

std::optional<fs::path> ProfilePipeline(const nlohmann::json& profile) {
  llm_edgeflow::DeploymentIoConfig config;
  std::string error;
  if (!profile.contains("config") || !profile["config"].is_string() ||
      !llm_edgeflow::DeploymentIoConfig::ReadFromFile(
          profile["config"].get<std::string>(), "operator", &config, &error)) {
    return std::nullopt;
  }
  return fs::path(config.resolved_pipe_path);
}

nlohmann::json ProfilesJson(const std::string& biz_filter) {
  nlohmann::json result = nlohmann::json::array();
  std::ifstream stream("demo/profiles.json");
  if (!stream.is_open()) return result;
  nlohmann::json root;
  try {
    stream >> root;
    for (const auto& [name, profile] : root["profiles"].items()) {
      auto pipeline_path = ProfilePipeline(profile);
      if (!pipeline_path) continue;
      nlohmann::json pipeline;
      std::string error;
      if (!ReadJson(pipeline_path->string(), &pipeline, &error)) continue;
      std::string pipeline_biz = pipeline.value("biz_name", "");
      if (!biz_filter.empty() && pipeline_biz != biz_filter) continue;
      result.push_back(
          {{"name", name},
           {"biz", profile.value("biz", "")},
           {"pipeline_biz", pipeline_biz},
           {"config", profile.value("config", "")},
           {"dataset", profile.value("dataset", "")},
           {"suite", profile.value("suite", "smoke")},
           {"batch_size",
            profile.value("batch_size", alg_demo::kDemoBatchSize)},
           {"device_id", profile.value("device_id", alg_demo::kDemoDeviceId)},
           {"chip", profile.value("chip", std::string(alg_demo::kDemoChip))}});
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
  if (ops.Init != nullptr && ops.Init() != 0)
    return PipelineError(DiagnosticCode::kRegistryConflict,
                         operator_api::GetOperatorLastError());
  struct RegistryGuard {
    operator_api::OperatorFunc ops;
    ~RegistryGuard() {
      if (ops.Deinit != nullptr) ops.Deinit();
    }
  } registry_guard{ops};
  ResolvedOperatorConfig resolved;
  std::string error;
  DeploymentDiagnostic diag;
  if (OperatorConfigResolver::Resolve(root.c_str(), file.c_str(), &resolved,
                                      &error, depth, &diag) != 0) {
    const std::string message =
        error.empty() ? (diag.message.empty() ? "Deployment configuration error"
                                              : diag.message)
                      : error;
    return ToolError("DEPLOYMENT_CONFIG", message);
  }
  if (!resolved.io_plan || !resolved.io_plan->pipeline_plan)
    return ToolError("DEPLOYMENT_CONFIG", "Missing pipeline plan in io_plan");

  const auto& plan = *resolved.io_plan->pipeline_plan;
  if (!plan.report.ok) {
    const std::string message = plan.report.diagnostics.empty()
                                    ? "Pipeline validation failed"
                                    : plan.report.diagnostics.front().message;
    return ToolError("DEPLOYMENT_CONFIG", message);
  }

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

  nlohmann::json paths = nlohmann::json::array();
  for (const auto& model : plan.models)
    paths.push_back(
        {{"model_id", model.model_id},
         {"source", resolved.io_plan->overridden_model_ids.count(model.model_id)
                        ? "pipeline.deployment.model_paths"
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
      << "  alg_pipeline_tool resolve-conf FILE [--root DIR] [--depth N]\n"
      << "  alg_pipeline_tool validate-io CONFIG [--transport operator] "
         "[--model-root DIR]\n"
      << "  alg_pipeline_tool edit --stdin\n"
      << "  alg_pipeline_tool fix-deps FILE [--in-place]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
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
      result = ToolError("DEPLOYMENT_CONFIG", error.what());
    } catch (...) {
      result = ToolError("DEPLOYMENT_CONFIG", "Unknown internal exception");
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
    const auto snapshot = PipelineCatalog::Snapshot();
    if (snapshot.node_registry_has_conflict) {
      std::string message = "Node registry contains registration conflicts";
      for (const auto& err : snapshot.node_registry_errors) {
        message += ": " + err;
      }
      std::cout
          << PipelineError(DiagnosticCode::kRegistryConflict, message).dump(2)
          << std::endl;
      return 1;
    }
    auto result = llm_edgeflow::IoCatalog::ToJson(snapshot, biz);
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
      std::cout
          << PipelineError(DiagnosticCode::kUnknownNodeType, argv[2]).dump(2)
          << std::endl;
      return 1;
    }
    auto result = PipelineCatalog::NodeToJson(*definition);
    result["schema_version"] = 3;
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
      std::cout << PipelineError(DiagnosticCode::kUnknownBiz, biz).dump(2)
                << std::endl;
      return 1;
    }
    nlohmann::json pipeline = {{"biz_name", biz},
                               {"models", nlohmann::json::array()},
                               {"pipeline", nlohmann::json::array()}};
    if (!profile.empty()) {
      std::string error;
      nlohmann::json profiles;
      std::optional<fs::path> path;
      if (ReadJson("demo/profiles.json", &profiles, &error) &&
          profiles.contains("profiles") && profiles["profiles"].is_object() &&
          profiles["profiles"].contains(profile)) {
        path = ProfilePipeline(profiles["profiles"][profile]);
      }
      if (!path || !ReadJson(path->string(), &pipeline, &error) ||
          pipeline.value("biz_name", "") != biz) {
        std::cout << ToolError("PROFILE_MISMATCH",
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
      std::cout << ToolError("JSON_READ", error).dump(2) << std::endl;
      return 1;
    }
    const auto ops =
        llm_edgeflow::operator_api::Get_LLM_EDGEFLOW_OperatorTable();
    if (ops.Init != nullptr && ops.Init() != 0) {
      std::cout << PipelineError(
                       DiagnosticCode::kRegistryConflict,
                       llm_edgeflow::operator_api::GetOperatorLastError())
                       .dump(2)
                << std::endl;
      return 1;
    }
    struct OpsGuard {
      llm_edgeflow::operator_api::OperatorFunc ops;
      ~OpsGuard() {
        if (ops.Deinit != nullptr) ops.Deinit();
      }
    } ops_guard{ops};

    using llm_edgeflow::DocumentValidationMode;
    DocumentValidationMode mode =
        (command == "validate") ? (explain ? DocumentValidationMode::kExplain
                                           : DocumentValidationMode::kValidate)
                                : DocumentValidationMode::kPlan;

    try {
      auto result = llm_edgeflow::ValidatePipelineDocument(root, mode);
      std::cout << result.response.dump(2) << std::endl;
      return result.ok ? 0 : 1;
    } catch (const std::exception& error) {
      std::cout << ToolError("INTERNAL_EXCEPTION", error.what()).dump(2)
                << std::endl;
      return 1;
    } catch (...) {
      std::cout << ToolError("INTERNAL_EXCEPTION", "Unknown internal exception")
                       .dump(2)
                << std::endl;
      return 1;
    }
  }

  if (command == "validate-io") {
    if (argc < 3) {
      Usage();
      return 2;
    }
    std::string config_path = argv[2];
    std::string transport = "operator";
    std::string model_root;
    for (int i = 3; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--transport" && i + 1 < argc) {
        transport = argv[++i];
      } else if (arg == "--model-root" && i + 1 < argc) {
        model_root = argv[++i];
      } else {
        Usage();
        return 2;
      }
    }

    const auto ops =
        llm_edgeflow::operator_api::Get_LLM_EDGEFLOW_OperatorTable();
    if (ops.Init != nullptr && ops.Init() != 0) {
      std::cout << PipelineError(
                       DiagnosticCode::kRegistryConflict,
                       llm_edgeflow::operator_api::GetOperatorLastError())
                       .dump(2)
                << std::endl;
      return 1;
    }
    struct OpsGuard {
      llm_edgeflow::operator_api::OperatorFunc ops;
      ~OpsGuard() {
        if (ops.Deinit != nullptr) ops.Deinit();
      }
    } ops_guard{ops};

    try {
      std::unique_ptr<llm_edgeflow::ValidatedIoPlan> plan;
      std::string error;
      llm_edgeflow::DeploymentDiagnostic diag;
      int rc = llm_edgeflow::IoBindingResolver::ResolveFromFile(
          config_path, transport, model_root, &plan, &error, &diag);

      if (rc != 0 || !plan) {
        std::string diag_path = diag.path.empty() ? "/" : diag.path;
        std::string diag_msg = error.empty() ? diag.message : error;
        nlohmann::json err_res = {
            {"schema_version", 1},
            {"ok", false},
            {"diagnostics",
             nlohmann::json::array({{{"code", "IO_VALIDATION_ERROR"},
                                     {"path", diag_path},
                                     {"message", diag_msg},
                                     {"severity", "error"}}})}};
        std::cout << err_res.dump(2) << std::endl;
        return 1;
      }

      nlohmann::json binding_info = {
          {"binding_id", plan->binding.binding_id},
          {"biz_name", plan->binding.biz_name},
          {"transport", plan->binding.transport},
          {"input_converter_id", plan->binding.input_converter_id},
          {"output_converter_id", plan->binding.output_converter_id},
          {"input_port_mapping", plan->binding.input_ports},
          {"output_port_mapping", plan->binding.output_ports},
          {"effective_max_batch_size", plan->effective_max_batch_size},
          {"external_input_type",
           plan->input_converter ? plan->input_converter->external_type : ""},
          {"external_output_type", plan->output_converter
                                       ? plan->output_converter->external_type
                                       : ""}};

      nlohmann::json result = {{"schema_version", 1},
                               {"ok", true},
                               {"binding", std::move(binding_info)},
                               {"diagnostics", nlohmann::json::array()}};
      std::cout << result.dump(2) << std::endl;
      return 0;
    } catch (const std::exception& error) {
      std::cout << ToolError("INTERNAL_EXCEPTION", error.what()).dump(2)
                << std::endl;
      return 1;
    } catch (...) {
      std::cout << ToolError("INTERNAL_EXCEPTION", "Unknown internal exception")
                       .dump(2)
                << std::endl;
      return 1;
    }
  }

  if (command == "edit") {
    if (argc != 3 || std::string(argv[2]) != "--stdin") {
      Usage();
      return 2;
    }
    const auto ops =
        llm_edgeflow::operator_api::Get_LLM_EDGEFLOW_OperatorTable();
    if (ops.Init != nullptr && ops.Init() != 0) {
      std::cout << PipelineError(
                       DiagnosticCode::kRegistryConflict,
                       llm_edgeflow::operator_api::GetOperatorLastError())
                       .dump(2)
                << std::endl;
      return 1;
    }
    struct OpsGuard {
      llm_edgeflow::operator_api::OperatorFunc ops;
      ~OpsGuard() {
        if (ops.Deinit != nullptr) ops.Deinit();
      }
    } ops_guard{ops};

    std::string input_str;
    char buffer[65536];
    while (std::cin.read(buffer, sizeof(buffer)) || std::cin.gcount() > 0) {
      input_str.append(buffer, std::cin.gcount());
      if (input_str.size() > 4 * 1024 * 1024) {
        nlohmann::json err_res = {
            {"schema_version", 1},
            {"ok", false},
            {"diagnostics",
             {{{"code", "AUTHORING_ERROR"},
               {"path", "/"},
               {"message", "REQUEST_TOO_LARGE: 请求输入大小超过单次上限 4 MiB"},
               {"severity", "error"}}}}};
        std::cout << err_res.dump(2) << std::endl;
        return 1;
      }
    }
    nlohmann::json request;
    try {
      request = nlohmann::json::parse(input_str);
    } catch (const std::exception& e) {
      std::cout << ToolError("JSON_READ", e.what()).dump(2) << std::endl;
      return 1;
    }
    try {
      auto result = llm_edgeflow::PipelineAuthoring::ApplyRequest(request);
      std::cout << result.ToJson().dump(2) << std::endl;
      return result.ok ? 0 : 1;
    } catch (const std::exception& error) {
      std::cout << ToolError("AUTHORING_ERROR", error.what()).dump(2)
                << std::endl;
      return 1;
    } catch (...) {
      std::cout << ToolError("AUTHORING_ERROR", "未知编排工具错误").dump(2)
                << std::endl;
      return 1;
    }
  }

  if (command == "fix-deps") {
    if (argc < 3 || argc > 4) {
      Usage();
      return 2;
    }
    std::string file = argv[2];
    bool in_place = false;
    if (argc == 4) {
      if (std::string(argv[3]) == "--in-place") {
        in_place = true;
      } else {
        Usage();
        return 2;
      }
    }
    const auto ops =
        llm_edgeflow::operator_api::Get_LLM_EDGEFLOW_OperatorTable();
    if (ops.Init != nullptr && ops.Init() != 0) {
      std::cout << PipelineError(
                       DiagnosticCode::kRegistryConflict,
                       llm_edgeflow::operator_api::GetOperatorLastError())
                       .dump(2)
                << std::endl;
      return 1;
    }
    struct OpsGuard {
      llm_edgeflow::operator_api::OperatorFunc ops;
      ~OpsGuard() {
        if (ops.Deinit != nullptr) ops.Deinit();
      }
    } ops_guard{ops};

    try {
      auto result = llm_edgeflow::PipelineAuthoring::FixDeps(file, in_place);
      std::cout << result.ToJson().dump(2) << std::endl;
      return result.ok ? 0 : 1;
    } catch (const std::exception& error) {
      std::cout << ToolError("AUTHORING_ERROR", error.what()).dump(2)
                << std::endl;
      return 1;
    } catch (...) {
      std::cout << ToolError("AUTHORING_ERROR", "未知编排工具错误").dump(2)
                << std::endl;
      return 1;
    }
  }

  Usage();
  return 2;
}
