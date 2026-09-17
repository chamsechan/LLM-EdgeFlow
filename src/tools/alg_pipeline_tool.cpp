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
#include "edgeflow/operator/interface.h"
#include "nlohmann/json.hpp"
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

nlohmann::json ToolError(const std::string& code, const std::string& message) {
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
  if (!conf.is_object() || !conf.contains("pipe_path") ||
      !conf["pipe_path"].is_string()) {
    return std::nullopt;
  }
  fs::path pipe_path = conf["pipe_path"].get<std::string>();
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

bool ResolveDeploymentBoundary(
    const nlohmann::json& root, nlohmann::json* out_neutral_json,
    llm_edgeflow::PipelineIoBoundary* out_boundary,
    const llm_edgeflow::IoBindingDefinition** out_binding,
    nlohmann::json* out_error_json) {
  using namespace llm_edgeflow;

  PipelineDocumentSplit doc_split;
  std::string split_err;
  if (!SplitPipelineDocument(root, &doc_split, &split_err)) {
    *out_error_json = ToolError("DEPLOYMENT_ERROR", split_err);
    return false;
  }

  if (!doc_split.has_deployment || !doc_split.deployment.has_io) {
    *out_error_json =
        ToolError("MISSING_DEPLOYMENT_IO",
                  "Missing required 'deployment.io' in pipeline JSON");
    return false;
  }

  const std::string& binding_id = doc_split.deployment.io.io_binding;
  const auto* binding = IoBindingRegistry::Instance().FindBinding(binding_id);
  if (!binding) {
    *out_error_json =
        ToolError("UNKNOWN_IO_BINDING",
                  "Unknown or unregistered io_binding: " + binding_id +
                      " (at /deployment/io/io_binding)");
    return false;
  }

  if (binding->transport != "operator") {
    *out_error_json = ToolError(
        "UNSUPPORTED_TRANSPORT",
        "Binding transport mismatch for '" + binding_id +
            "': expected 'operator', but binding declared '" +
            binding->transport + "' (at /deployment/io/io_binding)");
    return false;
  }

  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      binding->input_converter_id);
  if (!in_conv) {
    *out_error_json = ToolError(
        "UNREGISTERED_CONVERTER",
        "Binding references unregistered input converter: " +
            binding->input_converter_id);
    return false;
  }

  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      binding->output_converter_id);
  if (!out_conv) {
    *out_error_json = ToolError(
        "UNREGISTERED_CONVERTER",
        "Binding references unregistered output converter: " +
            binding->output_converter_id);
    return false;
  }

  const auto& allocations = doc_split.deployment.io.output_allocations;
  for (auto it = allocations.begin(); it != allocations.end(); ++it) {
    bool found = false;
    for (const auto& slot : out_conv->external_slots) {
      if (slot.direction == PortDirection::kOutput &&
          slot.slot_name == it.key()) {
        found = true;
        break;
      }
    }
    if (!found) {
      *out_error_json = ToolError(
          "UNKNOWN_OUTPUT_SLOT",
          "Unknown configured output slot: " + it.key() +
              " (at /deployment/io/output_allocations/" + it.key() + ")");
      return false;
    }
  }

  for (const auto& slot : out_conv->external_slots) {
    if (slot.direction != PortDirection::kOutput) continue;
    if (!allocations.contains(slot.slot_name)) {
      if (slot.required) {
        *out_error_json = ToolError(
            "MISSING_OUTPUT_SLOT",
            "Missing required Operator output slot '" + slot.slot_name +
                "' (at /deployment/io/output_allocations/" + slot.slot_name +
                ")");
        return false;
      }
      continue;
    }
    ResolvedOutputPoolSpec pool_spec;
    std::string param_text;
    std::string alloc_err;
    if (OperatorConfigResolver::ResolveOutputAllocation(
            allocations[slot.slot_name], slot, &pool_spec, &param_text,
            &alloc_err) != 0) {
      *out_error_json = ToolError(
          "INVALID_OUTPUT_ALLOCATION",
          alloc_err + " (at /deployment/io/output_allocations/" +
              slot.slot_name + ")");
      return false;
    }
  }

  nlohmann::json staged_pipe_json = doc_split.neutral_pipeline_json;
  if (doc_split.deployment.has_model_paths &&
      !doc_split.deployment.model_paths.empty()) {
    std::unordered_set<std::string> known_model_ids;
    if (staged_pipe_json.contains("models") &&
        staged_pipe_json["models"].is_array()) {
      for (const auto& m : staged_pipe_json["models"]) {
        if (m.is_object() && m.contains("model_id") &&
            m["model_id"].is_string()) {
          known_model_ids.insert(m["model_id"].get<std::string>());
        }
      }
    }
    for (const auto& [mid, _] : doc_split.deployment.model_paths) {
      if (!known_model_ids.count(mid)) {
        *out_error_json = ToolError(
            "UNKNOWN_MODEL_ID",
            "Unknown model_id '" + mid + "' in '/deployment/model_paths'");
        return false;
      }
    }
    for (auto& m : staged_pipe_json["models"]) {
      if (m.is_object() && m.contains("model_id") &&
          m["model_id"].is_string()) {
        std::string mid = m["model_id"].get<std::string>();
        auto it = doc_split.deployment.model_paths.find(mid);
        if (it != doc_split.deployment.model_paths.end()) {
          m["model_path"] = it->second;
        }
      }
    }
  }

  if (staged_pipe_json.contains("models") &&
      staged_pipe_json["models"].is_array()) {
    for (const auto& m : staged_pipe_json["models"]) {
      if (m.is_object() && m.contains("model_path")) {
        if (!m["model_path"].is_string() ||
            m["model_path"].get<std::string>().empty()) {
          *out_error_json = ToolError(
              "INVALID_MODEL_PATH",
              "model_path in model declaration must be a non-empty string");
          return false;
        }
      }
    }
  }

  PipelineIoBoundary io_boundary;
  for (const auto& port : in_conv->logical_ports) {
    std::string key = port.logical_name;
    auto bit = binding->input_ports.find(port.logical_name);
    if (bit != binding->input_ports.end()) {
      key = bit->second;
    }
    io_boundary.input_published_ports.emplace_back(
        key, port.type_id, port.required, port.cardinality,
        port.provenance_policy, port.lifetime, port.lifetime_config_field);
  }

  for (const auto& port : out_conv->logical_ports) {
    std::string key = port.logical_name;
    auto bit = binding->output_ports.find(port.logical_name);
    if (bit != binding->output_ports.end()) {
      key = bit->second;
    }
    io_boundary.output_consumed_ports.emplace_back(
        key, port.type_id, port.required, port.cardinality,
        port.provenance_policy, port.lifetime, port.lifetime_config_field);
  }

  *out_boundary = std::move(io_boundary);
  *out_neutral_json = std::move(staged_pipe_json);
  if (out_binding) *out_binding = binding;
  return true;
}

nlohmann::json ResolveConf(const std::string& file, const std::string& root,
                           uint32_t depth) {
  using namespace llm_edgeflow;
  const auto ops = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  if (ops.Init() != 0)
    return PipelineError(DiagnosticCode::kRegistryConflict,
                         operator_api::GetOperatorLastError());
  struct RegistryGuard {
    operator_api::OperatorFunc ops;
    ~RegistryGuard() { ops.Deinit(); }
  } registry_guard{ops};
  ResolvedOperatorConfig resolved;
  std::string error;
  if (OperatorConfigResolver::Resolve(root.c_str(), file.c_str(), &resolved,
                                      &error, depth) != 0)
    return ToolError("DEPLOYMENT_CONFIG", error);
  if (!resolved.io_plan || !resolved.io_plan->pipeline_plan)
    return ToolError("DEPLOYMENT_CONFIG", "Missing pipeline plan in io_plan");

  const auto& plan = *resolved.io_plan->pipeline_plan;
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

  nlohmann::json paths = nlohmann::json::array();
  for (const auto& model : plan.models)
    paths.push_back({{"model_id", model.model_id},
                     {"source", resolved.io_plan->overridden_model_ids.count(
                                    model.model_id)
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
      auto path = ProfilePipeline(profile);
      std::string error;
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
    if (ops.Init != nullptr) ops.Init();
    struct OpsGuard {
      llm_edgeflow::operator_api::OperatorFunc ops;
      ~OpsGuard() {
        if (ops.Deinit != nullptr) ops.Deinit();
      }
    } ops_guard{ops};

    llm_edgeflow::PipelineIoBoundary io_boundary;
    const llm_edgeflow::PipelineIoBoundary* io_boundary_ptr = nullptr;
    const llm_edgeflow::IoBindingDefinition* binding_def = nullptr;
    nlohmann::json target_json = root;

    if (root.contains("deployment")) {
      nlohmann::json err_res;
      if (!ResolveDeploymentBoundary(root, &target_json, &io_boundary,
                                     &binding_def, &err_res)) {
        if (command == "plan") {
          err_res["plan"] = {{"layers", nlohmann::json::array()},
                             {"topological_order", nlohmann::json::array()}};
        }
        std::cout << err_res.dump(2) << std::endl;
        return 1;
      }
      io_boundary_ptr = &io_boundary;
    }

    if (command == "validate") {
      auto report = explain ? PipelineValidator::Explain(
                                  target_json,
                                  llm_edgeflow::ValidationPolicy::kStrict,
                                  io_boundary_ptr)
                            : PipelineValidator::Validate(
                                  target_json,
                                  llm_edgeflow::ValidationPolicy::kStrict,
                                  io_boundary_ptr);
      if (report.ok && binding_def != nullptr) {
        std::string biz = target_json.value("biz_name", "");
        if (biz != binding_def->biz_name) {
          std::cout << ToolError("BIZ_MISMATCH",
                                 "Pipeline biz_name '" + biz +
                                     "' does not match binding biz_name '" +
                                     binding_def->biz_name +
                                     "' (at /deployment/io/io_binding)")
                           .dump(2)
                    << std::endl;
          return 1;
        }
      }
      auto result = report.ToJson();
      std::cout << result.dump(2) << std::endl;
      return report.ok ? 0 : 1;
    } else {
      auto planned = PipelineValidator::ValidateAndPlan(
          target_json, llm_edgeflow::ValidationPolicy::kStrict,
          io_boundary_ptr);
      if (planned.report.ok && binding_def != nullptr) {
        std::string biz = target_json.value("biz_name", "");
        if (biz != binding_def->biz_name) {
          nlohmann::json err_res = ToolError(
              "BIZ_MISMATCH", "Pipeline biz_name '" + biz +
                                  "' does not match binding biz_name '" +
                                  binding_def->biz_name +
                                  "' (at /deployment/io/io_binding)");
          err_res["plan"] = {{"layers", nlohmann::json::array()},
                             {"topological_order", nlohmann::json::array()}};
          std::cout << err_res.dump(2) << std::endl;
          return 1;
        }
      }
      auto result = planned.report.ToJson();
      if (planned.report.ok) {
        result.erase("diagnostics");
      }
      std::cout << result.dump(2) << std::endl;
      return planned.report.ok ? 0 : 1;
    }
  }

  if (command == "validate-io") {
    if (argc < 4) {
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

    std::unique_ptr<llm_edgeflow::ValidatedIoPlan> plan;
    std::string error;
    int rc = llm_edgeflow::IoBindingResolver::ResolveFromFile(
        config_path, transport, model_root, &plan, &error);

    if (rc != 0 || !plan) {
      nlohmann::json err_res = {
          {"schema_version", 1},
          {"ok", false},
          {"diagnostics",
           nlohmann::json::array({{{"code", "IO_VALIDATION_ERROR"},
                                   {"path", "/"},
                                   {"message", error},
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
        {"external_output_type",
         plan->output_converter ? plan->output_converter->external_type : ""}};

    nlohmann::json result = {{"schema_version", 1},
                             {"ok", true},
                             {"binding", std::move(binding_info)},
                             {"diagnostics", nlohmann::json::array()}};
    std::cout << result.dump(2) << std::endl;
    return 0;
  }

  if (command == "edit") {
    if (argc != 3 || std::string(argv[2]) != "--stdin") {
      Usage();
      return 2;
    }
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
