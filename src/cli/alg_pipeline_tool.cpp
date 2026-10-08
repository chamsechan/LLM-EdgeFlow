#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "adapter/deployment_io_config.h"
#include "adapter/io_catalog.h"
#include "adapter/io_converter_registry.h"
#include "adapter/io_plan_resolver.h"
#include "adapter/operator/operator_config_resolver.h"
#include "adapter/pipeline_document.h"
#include "cli/pipeline_authoring.h"
#include "core/common_contracts.h"
#include "core/diagnostic_code.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "demo/common/demo_profile_defaults.h"
#include "demo/common/demo_profile_fields.h"
#include "edgeflow/operator/interface.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"
#include "nlohmann/json.hpp"
#include "pipeline_document_validation.h"
#include "pipeline_json_schema.h"

namespace {

using llm_edgeflow::DiagnosticCode;
using llm_edgeflow::DiagnosticCodeName;
using llm_edgeflow::PipelineCatalog;
using llm_edgeflow::PipelineValidator;
namespace fs = std::filesystem;

nlohmann::json PipelineError(DiagnosticCode code, const std::string& message) {
  return {{"ok", false},
          {"diagnostics",
           nlohmann::json::array({{{"code", DiagnosticCodeName(code)},
                                   {"path", "/"},
                                   {"message", message},
                                   {"severity", "error"}}})}};
}

nlohmann::json ToolError(const std::string& code, const std::string& message,
                         const std::string& path = "/") {
  return {{"ok", false},
          {"diagnostics", nlohmann::json::array({{{"code", code},
                                                  {"path", path},
                                                  {"message", message},
                                                  {"severity", "error"}}})}};
}

// 构建/测试注册指引保留在 CLI 中；SDK/Core 诊断保持与工具无关。
// 被包装的部署错误另行提供其原始错误码。
void PrintRegistrationHint(const nlohmann::json& response,
                           const std::string& source_code = {}) {
#ifdef LLM_EDGEFLOW_TOOL_HAS_TEST_REGISTRATIONS
  (void)response;
  (void)source_code;
#else
  auto unknown_registration = [](const std::string& code) {
    return code == "UNKNOWN_MODEL_TYPE" || code == "UNKNOWN_BACKEND";
  };
  auto has_unknown_registration = [&](const nlohmann::json& report) {
    auto diagnostics = report.find("diagnostics");
    if (diagnostics == report.end() || !diagnostics->is_array()) return false;
    for (const auto& diagnostic : *diagnostics) {
      if (diagnostic.is_object() &&
          unknown_registration(diagnostic.value("code", "")))
        return true;
    }
    return false;
  };
  bool needs_hint =
      unknown_registration(source_code) || has_unknown_registration(response);
  auto validation = response.find("validation");
  if (validation != response.end() && validation->is_object()) {
    needs_hint = needs_hint || has_unknown_registration(*validation);
  }
  if (needs_hint) {
    std::cerr << "提示：当前 alg_pipeline_tool 只包含本次构建启用的生产注册。\n"
              << "  - 可选 Backend 需要用对应的构建预设重新构建，见 "
                 "doc/VERIFIABLE_SELECTION.md#构建变体。\n"
              << "  - 使用测试 Model/Backend 的配置（例如 demo/fixtures/mock/ "
                 "下的方案）"
                 "请改用 alg_pipeline_tool_test，见 "
                 "tools/pipeline_studio/README.md#校验工具选择。\n";
  }
#endif
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
          profile["config"].get<std::string>(), &config, &error)) {
    return std::nullopt;
  }
  return fs::path(config.resolved_pipe_path);
}

nlohmann::json ProfilesJson(std::string* error) {
  nlohmann::json result = nlohmann::json::array();
  std::ifstream stream("demo/profiles.json");
  if (!stream.is_open()) return result;
  nlohmann::json root;
  try {
    stream >> root;
    for (const auto& [name, profile] : root["profiles"].items()) {
      std::string fields_error;
      if (!alg_demo::ValidateProfileFields(profile, &fields_error)) {
        if (error) *error = "Profile '" + name + "': " + fields_error;
        return nlohmann::json::array();
      }
      auto pipeline_path = ProfilePipeline(profile);
      if (!pipeline_path) continue;
      nlohmann::json pipeline;
      std::string error;
      if (!ReadJson(pipeline_path->string(), &pipeline, &error)) continue;
      if (!pipeline.is_object() || !pipeline.contains("io") ||
          !pipeline["io"].is_object())
        continue;
      result.push_back(
          {{"name", name},
           {"io", pipeline["io"]},
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

nlohmann::json OutputPoolsJson(const llm_edgeflow::ValidatedIoPlan& plan) {
  nlohmann::json output_pools = nlohmann::json::object();
  for (const auto& output : plan.outputs) {
    const auto& pool = output.pool_spec;
    output_pools[output.converter->slot.type_suffix] = {
        {"type", pool.type},
        {"allocator", pool.allocator},
        {"params", output.converter->slot.AllocatorParamsText()},
        {"meta_num", pool.meta_num},
        {"metadata_type_id", pool.metadata_type_id},
        {"capacities", pool.capacities}};
  }
  return output_pools;
}

nlohmann::json ResolveConf(const std::string& file, const std::string& root,
                           uint32_t depth, std::string* source_code) {
  using namespace llm_edgeflow;
  const auto ops = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  if (ops.Init != nullptr && ops.Init() != 0)
    return PipelineError(DiagnosticCode::kRegistryConflict,
                         operator_api::GetOperatorLastError());
  struct RegistryGuard {
    operator_api::OperatorFunc ops;
    ~RegistryGuard() {
      if (ops.DeInit != nullptr) ops.DeInit();
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
    *source_code = diag.code;
    return ToolError("DEPLOYMENT_CONFIG", message);
  }
  if (!resolved.io_plan || !resolved.io_plan->pipeline_plan)
    return ToolError("DEPLOYMENT_CONFIG", "Missing pipeline plan in io_plan");

  const auto& plan = *resolved.io_plan->pipeline_plan;
  if (!plan.report.ok) {
    const std::string message = plan.report.diagnostics.empty()
                                    ? "Pipeline validation failed"
                                    : plan.report.diagnostics.front().message;
    if (!plan.report.diagnostics.empty())
      *source_code = DiagnosticCodeName(plan.report.diagnostics.front().code);
    return ToolError("DEPLOYMENT_CONFIG", message);
  }

  auto effective = resolved.io_plan->resolved_pipeline_json;
  for (const auto& [id, node] : plan.node_plans)
    effective["pipeline"][node.node.source_index]["config"] =
        node.normalized_config;
  for (const auto& model : plan.models) {
    effective["models"][model.source_index]["model_path"] =
        model.resolved_model_path;
    effective["models"][model.source_index]["model_config"] =
        model.normalized_model_config;
    effective["models"][model.source_index]["backend_config"] =
        model.normalized_backend_config;
  }

  nlohmann::json paths = nlohmann::json::array();
  for (const auto& model : plan.models)
    paths.push_back({{"model_id", model.model_id},
                     {"source", "pipeline.models.model_path"},
                     {"resolved", model.resolved_model_path}});
  nlohmann::json configuration = {
      {"io", resolved.io_plan->resolved_pipeline_json.at("io")},
      {"conf_path", resolved.conf_path.string()},
      {"pipeline_path", resolved.pipeline_path.string()},
      {"model_root", resolved.model_root_path.string()},
      {"effective_frame_depth", resolved.effective_frame_depth},
      {"effective_process_batch_limit", resolved.effective_process_batch_limit},
      {"max_frame_depth_limit", kMaxOutputPoolDepth},
      {"effective_pipeline", std::move(effective)},
      {"model_paths", std::move(paths)},
      {"output_pools", OutputPoolsJson(*resolved.io_plan)}};
  return {{"ok", true}, {"configuration", std::move(configuration)}};
}

void Usage() {
  std::cerr << "Usage:\n"
            << "  alg_pipeline_tool catalog\n"
            << "  alg_pipeline_tool export-schema\n"
            << "  alg_pipeline_tool describe-node NODE_TYPE\n"
            << "  alg_pipeline_tool describe-model MODEL_TYPE\n"
            << "  alg_pipeline_tool describe-backend BACKEND_TYPE\n"
            << "  alg_pipeline_tool validate FILE|--stdin [--explain]\n"
            << "  alg_pipeline_tool plan FILE|--stdin [--explain]\n";
  std::cerr
      << "  alg_pipeline_tool resolve-conf FILE [--root DIR] [--depth N]\n"
      << "  alg_pipeline_tool validate-io CONFIG "
         "[--model-root DIR]\n"
      << "  alg_pipeline_tool edit --stdin\n";
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
    std::string source_code;
    try {
      result = ResolveConf(argv[2], root, depth, &source_code);
    } catch (const std::exception& error) {
      result = ToolError("DEPLOYMENT_CONFIG", error.what());
    } catch (...) {
      result = ToolError("DEPLOYMENT_CONFIG", "Unknown internal exception");
    }
    std::cout << result.dump(2) << std::endl;
    PrintRegistrationHint(result, source_code);
    return result.value("ok", false) ? 0 : 1;
  }

  if (command == "catalog" || command == "export-schema") {
    if (argc != 2) {
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
    auto result = llm_edgeflow::IoCatalog::ToJson(snapshot);
    if (command == "export-schema") {
      if (llm_edgeflow::ModelRegistry::Instance().HasConflict() ||
          llm_edgeflow::BackendRegistry::Instance().HasConflict() ||
          llm_edgeflow::IoConverterRegistry::Instance().HasConflict()) {
        std::cout << PipelineError(
                         DiagnosticCode::kRegistryConflict,
                         "Cannot export schema with registration conflicts")
                         .dump(2)
                  << std::endl;
        return 1;
      }
      std::cout << llm_edgeflow::BuildPipelineJsonSchema(result).dump(2)
                << std::endl;
      return 0;
    }
    std::string profile_error;
    result["profiles"] = ProfilesJson(&profile_error);
    if (!profile_error.empty()) {
      std::cout << ToolError("INVALID_PROFILE", profile_error).dump(2)
                << std::endl;
      return 1;
    }
    result["ok"] = true;
    std::cout << result.dump(2) << std::endl;
    return 0;
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
    result["ok"] = true;
    std::cout << result.dump(2) << std::endl;
    return 0;
  }

  if (command == "describe-model" || command == "describe-backend") {
    if (argc != 3) {
      Usage();
      return 2;
    }
    nlohmann::json result;
    if (command == "describe-model") {
      const auto definition = PipelineCatalog::FindModel(argv[2]);
      if (!definition) {
        const auto error =
            PipelineError(DiagnosticCode::kUnknownModelType, argv[2]);
        std::cout << error.dump(2) << std::endl;
        PrintRegistrationHint(error);
        return 1;
      }
      result = PipelineCatalog::ModelToJson(*definition);
    } else {
      const auto definition = PipelineCatalog::FindBackend(argv[2]);
      if (!definition) {
        const auto error =
            PipelineError(DiagnosticCode::kUnknownBackend, argv[2]);
        std::cout << error.dump(2) << std::endl;
        PrintRegistrationHint(error);
        return 1;
      }
      result = PipelineCatalog::BackendToJson(*definition);
    }
    result["ok"] = true;
    std::cout << result.dump(2) << std::endl;
    return 0;
  }

  if (command == "init") {
    std::cout << ToolError("NOT_AVAILABLE",
                           "init is unavailable until the tool rewrite for the "
                           "io format: write the draft JSON directly and check "
                           "it with validate/plan")
                     .dump(2)
              << std::endl;
    return 1;
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
        if (ops.DeInit != nullptr) ops.DeInit();
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
      PrintRegistrationHint(result.response);
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

    std::string model_root;
    for (int i = 3; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--model-root" && i + 1 < argc) {
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
        if (ops.DeInit != nullptr) ops.DeInit();
      }
    } ops_guard{ops};

    try {
      std::unique_ptr<llm_edgeflow::ValidatedIoPlan> plan;
      std::string error;
      llm_edgeflow::DeploymentDiagnostic diag;
      int rc = llm_edgeflow::IoPlanResolver::ResolveFromFile(
          config_path, model_root, &plan, &error, &diag);

      if (rc != 0 || !plan) {
        std::string diag_path = diag.path.empty() ? "/" : diag.path;
        std::string diag_msg = error.empty() ? diag.message : error;
        nlohmann::json err_res = {
            {"ok", false},
            {"diagnostics",
             nlohmann::json::array({{{"code", "IO_VALIDATION_ERROR"},
                                     {"path", diag_path},
                                     {"message", diag_msg},
                                     {"severity", "error"}}})}};
        std::cout << err_res.dump(2) << std::endl;
        PrintRegistrationHint(err_res, diag.code);
        return 1;
      }

      nlohmann::json result = {{"ok", true},
                               {"io", plan->resolved_pipeline_json.at("io")},
                               {"output_pools", OutputPoolsJson(*plan)},
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
        if (ops.DeInit != nullptr) ops.DeInit();
      }
    } ops_guard{ops};

    std::string input_str;
    char buffer[65536];
    while (std::cin.read(buffer, sizeof(buffer)) || std::cin.gcount() > 0) {
      input_str.append(buffer, std::cin.gcount());
      if (input_str.size() > 4 * 1024 * 1024) {
        nlohmann::json err_res = {
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
      auto response = result.ToJson();
      std::cout << response.dump(2) << std::endl;
      PrintRegistrationHint(response);
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
