#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>

#include "adapter/deployment_io_config.h"
#include "adapter/io_catalog.h"
#include "adapter/io_converter_registry.h"
#include "adapter/io_plan_resolver.h"
#include "adapter/operator/operator_config_resolver.h"
#include "cli/pipeline_authoring.h"
#include "contracts/json_pointer.h"
#include "core/diagnostic_code.h"
#include "core/pipeline_catalog.h"
#include "demo/common/demo_profile_defaults.h"
#include "demo/common/demo_profile_fields.h"
#include "edgeflow/operator/interface.h"
#include "engine/model_registry.h"
#include "nlohmann/json.hpp"
#include "pipeline_document_validation.h"
#include "pipeline_json_schema.h"

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using llm_edgeflow::DiagnosticCode;
using llm_edgeflow::DiagnosticCodeName;
using llm_edgeflow::PipelineCatalog;

Json ToolError(const std::string& code, const std::string& message,
               const std::string& path = "/") {
  return {{"ok", false},
          {"diagnostics", Json::array({{{"code", code},
                                        {"path", path},
                                        {"message", message},
                                        {"severity", "error"}}})}};
}

Json PipelineError(DiagnosticCode code, const std::string& message) {
  return ToolError(DiagnosticCodeName(code), message);
}

int Print(const Json& result) {
  std::cout << result.dump(2) << std::endl;
  return result.value("ok", false) ? 0 : 1;
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

std::optional<fs::path> ProfilePipeline(const Json& profile) {
  llm_edgeflow::DeploymentIoConfig config;
  std::string error;
  if (!profile.contains("config") || !profile["config"].is_string() ||
      !llm_edgeflow::DeploymentIoConfig::ReadFromFile(
          profile["config"].get<std::string>(), &config, &error))
    return std::nullopt;
  return fs::path(config.resolved_pipe_path);
}

Json IoPairs(const Json& pipeline) {
  auto result = Json::object();
  for (const auto* side : {"input", "output"}) {
    result[side] = Json::array();
    for (const auto& entry : pipeline.at("io").at(side))
      result[side].push_back(
          {{"type", entry.at("type")}, {"name", entry.at("name")}});
  }
  return result;
}

Json ProfilesJson(std::string* error) {
  auto result = Json::array();
  std::ifstream stream("demo/profiles.json");
  if (!stream.is_open()) return result;
  try {
    const auto root = Json::parse(stream);
    for (const auto& [name, profile] : root.at("profiles").items()) {
      std::string fields_error;
      if (!alg_demo::ValidateProfileFields(profile, &fields_error)) {
        *error = "Profile '" + name + "': " + fields_error;
        return Json::array();
      }
      const auto path = ProfilePipeline(profile);
      Json pipeline;
      std::string read_error;
      if (!path || !ReadJson(path->string(), &pipeline, &read_error)) continue;
      result.push_back(
          {{"name", name},
           {"io", IoPairs(pipeline)},
           {"config", profile.at("config")},
           {"dataset", profile.value("dataset", "")},
           {"suite", profile.value("suite", "smoke")},
           {"batch_size",
            profile.value("batch_size", alg_demo::kDemoBatchSize)},
           {"device_id", profile.value("device_id", alg_demo::kDemoDeviceId)},
           {"chip", profile.value("chip", std::string(alg_demo::kDemoChip))}});
    }
  } catch (const std::exception& exception) {
    *error = exception.what();
  }
  return result;
}

Json IoJson(const llm_edgeflow::ValidatedIoPlan& plan) {
  Json result = {{"input", Json::array()}, {"output", Json::array()}};
  for (const auto& entry : plan.inputs)
    result["input"].push_back({{"type", entry.converter->type},
                               {"name", entry.converter->name},
                               {"external_type", entry.converter->slot.type_id},
                               {"params", entry.params->Effective()}});
  for (const auto& entry : plan.outputs)
    result["output"].push_back(
        {{"type", entry.converter->type},
         {"name", entry.converter->name},
         {"external_type", entry.converter->slot.type_id},
         {"params", entry.params->Effective()}});
  return result;
}

Json OutputPoolsJson(const llm_edgeflow::ValidatedIoPlan& plan) {
  auto result = Json::object();
  for (const auto& entry : plan.outputs) {
    const auto& definition = *entry.converter;
    const auto& pool = entry.pool_spec;
    const auto repeated =
        std::count_if(plan.outputs.begin(), plan.outputs.end(),
                      [&](const auto& selected) {
                        return selected.converter->type == definition.type;
                      }) > 1;
    const auto slot =
        repeated ? definition.name + "." + definition.type : definition.type;
    result[slot] = {{"type", pool.type},
                    {"allocator", pool.allocator},
                    {"params", entry.params->Effective().dump()},
                    {"meta_num", pool.meta_num},
                    {"metadata_type_id", pool.metadata_type_id},
                    {"capacities", pool.capacities}};
  }
  return result;
}

void AddParameterFiles(
    Json* files, const Json& params,
    const std::vector<llm_edgeflow::ConfigFieldDefinition>& fields,
    const std::string& model, const std::string& path) {
  for (const auto& field : fields)
    if (field.file && params.contains(field.name))
      files->push_back(
          {{"model", model},
           {"path", path + "/" + llm_edgeflow::EscapeJsonPointer(field.name)},
           {"resolved", params.at(field.name)}});
}

Json ResolveConf(const std::string& file, const std::string& root,
                 uint32_t depth, std::string* source_code) {
  using namespace llm_edgeflow;
  ResolvedOperatorConfig resolved;
  DeploymentDiagnostic diagnostic;
  std::string error;
  auto relative_file = fs::path(file);
  if (relative_file.is_absolute())
    relative_file = relative_file.lexically_relative(fs::absolute(root));
  if (OperatorConfigResolver::Resolve(root.c_str(),
                                      relative_file.string().c_str(), &resolved,
                                      &error, depth, &diagnostic) != 0) {
    *source_code = diagnostic.code;
    return ToolError("DEPLOYMENT_CONFIG",
                     error.empty() ? diagnostic.message : error,
                     diagnostic.path.empty() ? "/" : diagnostic.path);
  }
  const auto& io_plan = *resolved.io_plan;
  const auto& plan = *io_plan.pipeline_plan;
  auto effective = io_plan.resolved_pipeline_json;
  for (const auto& [name, node] : plan.node_plans)
    effective["pipeline"][node.node.source_index]["params"] =
        node.normalized_params;
  auto files = Json::array();
  for (const auto& model : plan.models) {
    auto& item = effective["models"][model.source_index];
    item["file"] = model.model_file;
    item["params"] = model.model_params;
    item["backend"]["params"] = model.backend_params;
    const auto path = "/models/" + std::to_string(model.source_index);
    files.push_back({{"model", model.model_name},
                     {"path", path + "/file"},
                     {"resolved", model.model_file}});
    const auto definition = PipelineCatalog::FindModel(model.impl_name);
    const auto backend = PipelineCatalog::FindBackend(model.backend_type);
    AddParameterFiles(&files, model.model_params, definition->params.Fields(),
                      model.model_name, path + "/params");
    AddParameterFiles(&files, model.backend_params, backend->params.Fields(),
                      model.model_name, path + "/backend/params");
  }
  return {{"ok", true},
          {"configuration",
           {{"io", IoJson(io_plan)},
            {"conf_path", resolved.conf_path.string()},
            {"pipeline_path", resolved.pipeline_path.string()},
            {"effective_frame_depth", resolved.effective_frame_depth},
            {"effective_process_batch_limit",
             resolved.effective_process_batch_limit},
            {"max_frame_depth_limit", kMaxOutputPoolDepth},
            {"effective_pipeline", std::move(effective)},
            {"model_files", std::move(files)},
            {"output_pools", OutputPoolsJson(io_plan)}}}};
}

Json Edges(const Json& document) {
  auto edges = Json::array();
  for (const auto& node : document.at("pipeline")) {
    const auto inputs = node.value("inputs", Json::object());
    for (const auto& [port, source] : inputs.items())
      edges.push_back(
          {{"source", source},
           {"target", node.at("name").get<std::string>() + "." + port}});
  }
  for (const auto& entry : document.at("io").at("output")) {
    const auto inputs = entry.value("inputs", Json::object());
    for (const auto& [port, source] : inputs.items())
      edges.push_back({{"source", source},
                       {"target", "output." + port},
                       {"converter", entry.at("type").get<std::string>() + "/" +
                                         entry.at("name").get<std::string>()}});
  }
  return edges;
}

void Usage() {
  std::cerr
      << "Usage:\n"
      << "  alg_pipeline_tool catalog\n"
      << "  alg_pipeline_tool export-schema\n"
      << "  alg_pipeline_tool describe-node TYPE\n"
      << "  alg_pipeline_tool describe-model TYPE BACKEND\n"
      << "  alg_pipeline_tool describe-backend TYPE\n"
      << "  alg_pipeline_tool init --input TYPE/NAME --output TYPE/NAME "
         "[--raw]\n"
      << "  alg_pipeline_tool init --profile NAME [--raw]\n"
      << "  alg_pipeline_tool validate FILE|--stdin [--explain]\n"
      << "  alg_pipeline_tool plan FILE|--stdin [--explain]\n"
      << "  alg_pipeline_tool validate-io CONFIG\n"
      << "  alg_pipeline_tool resolve-conf FILE [--root DIR] [--depth N]\n"
      << "  alg_pipeline_tool edit --stdin\n";
}

struct RegistryGuard {
  llm_edgeflow::operator_api::OperatorFunc ops{};
  bool initialized = false;
  ~RegistryGuard() {
    if (initialized && ops.DeInit) ops.DeInit();
  }
};

int Run(int argc, char* argv[]) {
  if (argc < 2) {
    Usage();
    return 2;
  }
  const std::string command = argv[1];
  RegistryGuard guard;
  if (command == "validate" || command == "plan" || command == "validate-io" ||
      command == "resolve-conf" || command == "edit") {
    guard.ops = llm_edgeflow::operator_api::Get_LLM_EDGEFLOW_OperatorTable();
    if (guard.ops.Init && guard.ops.Init() != 0)
      return Print(
          PipelineError(DiagnosticCode::kRegistryConflict,
                        llm_edgeflow::operator_api::GetOperatorLastError()));
    guard.initialized = guard.ops.Init != nullptr;
  }
  if (command == "catalog" || command == "export-schema") {
    if (argc != 2) {
      Usage();
      return 2;
    }
    const auto snapshot = PipelineCatalog::Snapshot();
    if (snapshot.node_registry_has_conflict)
      return Print(
          PipelineError(DiagnosticCode::kRegistryConflict,
                        "Node registry contains registration conflicts"));
    if (command == "export-schema") {
      std::cout << llm_edgeflow::BuildPipelineJsonSchema().dump(2) << std::endl;
      return 0;
    }
    auto result = llm_edgeflow::IoCatalog::ToJson(snapshot);
    std::string error;
    result["profiles"] = ProfilesJson(&error);
    if (!error.empty()) return Print(ToolError("INVALID_PROFILE", error));
    result["ok"] = true;
    return Print(result);
  }
  if (command == "describe-node") {
    if (argc != 3) {
      Usage();
      return 2;
    }
    const auto definition = PipelineCatalog::FindNode(argv[2]);
    if (!definition)
      return Print(PipelineError(DiagnosticCode::kUnknownNodeType, argv[2]));
    auto result = PipelineCatalog::NodeToJson(*definition);
    result["ok"] = true;
    return Print(result);
  }
  if (command == "describe-model") {
    if (argc != 4) {
      Usage();
      return 2;
    }
    const auto definitions =
        llm_edgeflow::ModelRegistry::Instance().FindImplementation(argv[2],
                                                                   argv[3]);
    if (definitions.size() != 1) {
      const auto code = definitions.empty()
                            ? DiagnosticCode::kBackendProtocolMismatch
                            : DiagnosticCode::kRegistryConflict;
      auto error = PipelineError(code, "No unique implementation for '" +
                                           std::string(argv[2]) + "' / '" +
                                           argv[3] + "'");
      if (!PipelineCatalog::FindBackend(argv[3]))
        error = PipelineError(DiagnosticCode::kUnknownBackend, argv[3]);
      const auto models = PipelineCatalog::Models();
      if (std::none_of(models.begin(), models.end(), [&](const auto& model) {
            return model.model_type == argv[2];
          }))
        error = PipelineError(DiagnosticCode::kUnknownModelType, argv[2]);
      PrintRegistrationHint(error);
      return Print(error);
    }
    auto result = PipelineCatalog::ModelToJson(definitions.front());
    result["ok"] = true;
    return Print(result);
  }
  if (command == "describe-backend") {
    if (argc != 3) {
      Usage();
      return 2;
    }
    const auto definition = PipelineCatalog::FindBackend(argv[2]);
    if (!definition) {
      const auto error =
          PipelineError(DiagnosticCode::kUnknownBackend, argv[2]);
      PrintRegistrationHint(error);
      return Print(error);
    }
    auto result = PipelineCatalog::BackendToJson(*definition);
    result["ok"] = true;
    return Print(result);
  }
  if (command == "init") {
    Json pipeline = {
        {"io", {{"input", Json::array()}, {"output", Json::array()}}},
        {"models", Json::array()},
        {"pipeline", Json::array()}};
    std::string profile;
    bool raw = false;
    for (int i = 2; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--raw" && !raw) {
        raw = true;
        continue;
      }
      if (i + 1 == argc || argv[i + 1][0] == '\0' || argv[i + 1][0] == '-') {
        Usage();
        return 2;
      }
      const std::string value = argv[++i];
      if (option == "--profile" && profile.empty())
        profile = value;
      else if (option == "--input" || option == "--output") {
        const auto split = value.find('/');
        if (split == 0 || split == std::string::npos ||
            split + 1 == value.size() ||
            value.find('/', split + 1) != std::string::npos) {
          Usage();
          return 2;
        }
        const auto type = value.substr(0, split),
                   name = value.substr(split + 1);
        const auto& registry = llm_edgeflow::IoConverterRegistry::Instance();
        if (option == "--input" ? !registry.FindInputConverter(type, name)
                                : !registry.FindOutputConverter(type, name))
          return Print(ToolError("UNKNOWN_CONVERTER", value));
        pipeline["io"][option == "--input" ? "input" : "output"].push_back(
            {{"type", type}, {"name", name}});
      } else {
        Usage();
        return 2;
      }
    }
    const auto& io = pipeline["io"];
    if (!profile.empty()) {
      if (!io["input"].empty() || !io["output"].empty()) {
        Usage();
        return 2;
      }
      Json profiles;
      std::string error;
      if (!ReadJson("demo/profiles.json", &profiles, &error) ||
          !profiles.at("profiles").contains(profile))
        return Print(ToolError("UNKNOWN_PROFILE", profile));
      const auto& selected = profiles["profiles"][profile];
      if (!alg_demo::ValidateProfileFields(selected, &error))
        return Print(ToolError("INVALID_PROFILE", error));
      const auto path = ProfilePipeline(selected);
      if (!path || !ReadJson(path->string(), &pipeline, &error))
        return Print(
            ToolError("UNKNOWN_PROFILE", error.empty() ? profile : error));
    } else if (io["input"].empty() || io["output"].empty()) {
      Usage();
      return 2;
    }
    if (raw) {
      std::cout << pipeline.dump(2) << std::endl;
      return 0;
    }
    return Print({{"ok", true}, {"pipeline", std::move(pipeline)}});
  }
  if (command == "validate" || command == "plan") {
    if (argc < 3 || argc > 4) {
      Usage();
      return 2;
    }
    std::string file;
    bool explain = false;
    for (int i = 2; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--explain" && !explain)
        explain = true;
      else if (file.empty() && (option == "--stdin" || option[0] != '-'))
        file = option;
      else {
        Usage();
        return 2;
      }
    }
    if (file.empty()) {
      Usage();
      return 2;
    }
    Json document;
    std::string error;
    if (!ReadJson(file, &document, &error))
      return Print(ToolError("JSON_READ", error));
    using llm_edgeflow::DocumentValidationMode;
    const auto mode = command == "plan" ? DocumentValidationMode::kPlan
                      : explain         ? DocumentValidationMode::kExplain
                                        : DocumentValidationMode::kValidate;
    const auto directory = file == "--stdin"
                               ? std::string{}
                               : fs::absolute(file).parent_path().string();
    auto result =
        llm_edgeflow::ValidatePipelineDocument(document, mode, directory);
    if (command == "plan" && explain && result.ok)
      result.response["plan"]["edges"] = Edges(document);
    PrintRegistrationHint(result.response);
    return Print(result.response);
  }
  if (command == "validate-io") {
    if (argc != 3) {
      Usage();
      return 2;
    }
    std::unique_ptr<llm_edgeflow::ValidatedIoPlan> plan;
    std::string error;
    llm_edgeflow::DeploymentDiagnostic diagnostic;
    if (llm_edgeflow::IoPlanResolver::ResolveFromFile(argv[2], &plan, &error,
                                                      &diagnostic) != 0 ||
        !plan) {
      const auto response = ToolError(
          "IO_VALIDATION_ERROR", error.empty() ? diagnostic.message : error,
          diagnostic.path.empty() ? "/" : diagnostic.path);
      PrintRegistrationHint(response, diagnostic.code);
      return Print(response);
    }
    return Print({{"ok", true},
                  {"io", IoJson(*plan)},
                  {"output_pools", OutputPoolsJson(*plan)},
                  {"diagnostics", Json::array()}});
  }
  if (command == "resolve-conf") {
    if (argc < 3 || argc % 2 == 0) {
      Usage();
      return 2;
    }
    std::string root = ".";
    uint32_t depth = llm_edgeflow::kDefaultOutputPoolDepth;
    std::set<std::string> seen;
    for (int i = 3; i < argc; i += 2) {
      const std::string option = argv[i], value = argv[i + 1];
      if (!seen.insert(option).second) {
        Usage();
        return 2;
      }
      if (option == "--root" && !value.empty())
        root = value;
      else if (option == "--depth") {
        const auto parsed =
            std::from_chars(value.data(), value.data() + value.size(), depth);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != value.data() + value.size()) {
          Usage();
          return 2;
        }
      } else {
        Usage();
        return 2;
      }
    }
    std::string source_code;
    const auto result = ResolveConf(argv[2], root, depth, &source_code);
    PrintRegistrationHint(result, source_code);
    return Print(result);
  }
  if (command == "edit") {
    if (argc != 3 || std::string(argv[2]) != "--stdin") {
      Usage();
      return 2;
    }
    std::string input;
    char buffer[65536];
    while (std::cin.read(buffer, sizeof(buffer)) || std::cin.gcount() > 0) {
      input.append(buffer, std::cin.gcount());
      if (input.size() > 4 * 1024 * 1024)
        return Print(
            ToolError("AUTHORING_ERROR",
                      "REQUEST_TOO_LARGE: 请求输入大小超过单次上限 4 MiB"));
    }
    Json request;
    try {
      request = Json::parse(input);
    } catch (const std::exception& error) {
      return Print(ToolError("JSON_READ", error.what()));
    }
    const auto result = llm_edgeflow::PipelineAuthoring::ApplyRequest(request);
    const auto response = result.ToJson();
    PrintRegistrationHint(response);
    return Print(response);
  }
  Usage();
  return 2;
}
}  // namespace

int main(int argc, char* argv[]) {
  try {
    return Run(argc, argv);
  } catch (const std::exception& error) {
    return Print(ToolError("INTERNAL_EXCEPTION", error.what()));
  } catch (...) {
    return Print(ToolError("INTERNAL_EXCEPTION", "Unknown internal exception"));
  }
}
