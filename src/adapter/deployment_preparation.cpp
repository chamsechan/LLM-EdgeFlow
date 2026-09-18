#include "adapter/deployment_preparation.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

#include "adapter/deployment_model_resolver.h"
#include "adapter/io_binding_registry.h"
#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_config_resolver.h"
#include "adapter/pipeline_document.h"
#include "core/diagnostic_code.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_config.h"

namespace llm_edgeflow {

bool PrepareDeploymentDocument(const nlohmann::json& document,
                               const DeploymentPrepareOptions& options,
                               PreparedDeployment* output,
                               DeploymentDiagnostic* diagnostic) {
  if (!output) {
    if (diagnostic) {
      diagnostic->code = "DEPLOYMENT_ERROR";
      diagnostic->path = "/";
      diagnostic->message = "Output pointer is null";
      diagnostic->legacy_status = -1;
      diagnostic->pipeline_diagnostic.reset();
    }
    return false;
  }

  output->Clear();
  if (diagnostic) {
    diagnostic->Clear();
  }

  // S1: 检查调用参数与部署文档结构
  if (options.transport != "operator") {
    if (diagnostic) {
      diagnostic->code = "UNSUPPORTED_TRANSPORT";
      diagnostic->path = "/";
      diagnostic->message = "Unsupported transport: '" + options.transport +
                            "' (only 'operator' is supported)";
      diagnostic->legacy_status = -2;
    }
    return false;
  }

  if (options.path_mode == DeploymentPathMode::kUnderRoot &&
      options.model_root_dir.empty()) {
    if (diagnostic) {
      diagnostic->code = "DEPLOYMENT_ERROR";
      diagnostic->path = "/";
      diagnostic->message =
          "model_root_dir cannot be empty when path_mode is kUnderRoot";
      diagnostic->legacy_status = -2;
    }
    return false;
  }

  if (options.path_mode == DeploymentPathMode::kLexicalOnly &&
      !options.model_root_dir.empty()) {
    if (diagnostic) {
      diagnostic->code = "DEPLOYMENT_ERROR";
      diagnostic->path = "/";
      diagnostic->message =
          "model_root_dir must be empty when path_mode is kLexicalOnly";
      diagnostic->legacy_status = -2;
    }
    return false;
  }

  PipelineDocumentSplit doc_split;
  std::string split_err;
  std::string split_path;
  if (!SplitPipelineDocument(document, &doc_split, &split_err, &split_path)) {
    if (diagnostic) {
      diagnostic->code = "DEPLOYMENT_ERROR";
      diagnostic->path = split_path.empty() ? "/" : split_path;
      diagnostic->message = split_err;
      diagnostic->legacy_status = -2;
    }
    return false;
  }

  if (!doc_split.has_deployment || !doc_split.deployment.has_io) {
    if (diagnostic) {
      diagnostic->code = "MISSING_DEPLOYMENT_IO";
      diagnostic->path = "/deployment/io";
      diagnostic->message = "Missing required 'deployment.io' in pipeline JSON";
      diagnostic->legacy_status = -2;
    }
    return false;
  }

  // S2: 覆盖之前验证原始中性结构
  ParsedPipelineConfig original_config;
  PipelineDiagnostic core_diag;
  if (!ParsePipelineConfig(doc_split.neutral_pipeline_json, &original_config,
                           &core_diag)) {
    if (diagnostic) {
      diagnostic->pipeline_diagnostic = core_diag;
      bool is_model_path = false;
      if (core_diag.path.rfind("/models/", 0) == 0) {
        auto second_slash = core_diag.path.find('/', 8);
        if (second_slash != std::string::npos &&
            core_diag.path.substr(second_slash) == "/model_path") {
          is_model_path = true;
        }
      }

      if (is_model_path && (core_diag.code == DiagnosticCode::kFieldType ||
                            core_diag.code == DiagnosticCode::kFieldRange)) {
        diagnostic->code = "INVALID_MODEL_PATH";
        diagnostic->path = core_diag.path;
        diagnostic->message =
            "model_path in model declaration must be a non-empty string (at " +
            core_diag.path + ")";
        diagnostic->legacy_status = -2;
      } else {
        diagnostic->code = DiagnosticCodeName(core_diag.code);
        diagnostic->path = core_diag.path;
        diagnostic->message = "Validation failed: " +
                              std::string(DiagnosticCodeName(core_diag.code)) +
                              " at " + core_diag.path + ": " +
                              core_diag.message;
        diagnostic->legacy_status = -3;
      }
    }
    return false;
  }

  // S3: 解析 binding、converter 和业务边界
  const std::string& binding_id = doc_split.deployment.io.io_binding;
  const auto* binding = IoBindingRegistry::Instance().FindBinding(binding_id);
  if (!binding) {
    if (diagnostic) {
      diagnostic->code = "UNKNOWN_IO_BINDING";
      diagnostic->path = "/deployment/io/io_binding";
      diagnostic->message =
          "Unknown or unregistered io_binding: " + binding_id +
          " (at /deployment/io/io_binding)";
      diagnostic->legacy_status = -2;
    }
    return false;
  }

  if (binding->transport != "operator") {
    if (diagnostic) {
      diagnostic->code = "UNSUPPORTED_TRANSPORT";
      diagnostic->path = "/deployment/io/io_binding";
      diagnostic->message = "Binding transport mismatch for '" + binding_id +
                            "': expected 'operator', but binding declared '" +
                            binding->transport +
                            "' (at /deployment/io/io_binding)";
      diagnostic->legacy_status = -2;
    }
    return false;
  }

  if (original_config.biz_name != binding->biz_name) {
    if (diagnostic) {
      diagnostic->code = "BIZ_MISMATCH";
      diagnostic->path = "/deployment/io/io_binding";
      diagnostic->message = "Pipeline biz_name '" + original_config.biz_name +
                            "' does not match binding biz_name '" +
                            binding->biz_name +
                            "' (at /deployment/io/io_binding)";
      diagnostic->legacy_status = -2;
    }
    return false;
  }

  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      binding->input_converter_id);
  if (!in_conv) {
    if (diagnostic) {
      diagnostic->code = "UNREGISTERED_CONVERTER";
      diagnostic->path = "/deployment/io/io_binding";
      diagnostic->message =
          "Binding references unregistered input converter: " +
          binding->input_converter_id;
      diagnostic->legacy_status = -2;
    }
    return false;
  }

  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      binding->output_converter_id);
  if (!out_conv) {
    if (diagnostic) {
      diagnostic->code = "UNREGISTERED_CONVERTER";
      diagnostic->path = "/deployment/io/io_binding";
      diagnostic->message =
          "Binding references unregistered output converter: " +
          binding->output_converter_id;
      diagnostic->legacy_status = -2;
    }
    return false;
  }

  size_t max_batch =
      std::min(in_conv->max_batch_size, out_conv->max_batch_size);
  const auto* exposure =
      IoBindingRegistry::Instance().FindExposure(binding->biz_name);
  if (exposure) {
    max_batch = std::min(max_batch, exposure->max_batch_size);
  }

  // S4: 解析输出分配 (R ⊆ C ⊆ A)
  const auto& allocations = doc_split.deployment.io.output_allocations;
  std::unordered_map<std::string, ResolvedOutputPoolSpec> local_output_specs;
  std::unordered_map<std::string, std::string> local_output_params;

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
      if (diagnostic) {
        std::string ptr =
            "/deployment/io/output_allocations/" + EscapeJsonPointer(it.key());
        diagnostic->code = "UNKNOWN_OUTPUT_SLOT";
        diagnostic->path = ptr;
        diagnostic->message =
            "Unknown configured output slot: " + it.key() + " (at " + ptr + ")";
        diagnostic->legacy_status = -2;
      }
      return false;
    }
  }

  for (const auto& slot : out_conv->external_slots) {
    if (slot.direction != PortDirection::kOutput) continue;
    if (!allocations.contains(slot.slot_name)) {
      if (slot.required) {
        if (diagnostic) {
          std::string ptr = "/deployment/io/output_allocations/" +
                            EscapeJsonPointer(slot.slot_name);
          diagnostic->code = "MISSING_OUTPUT_SLOT";
          diagnostic->path = ptr;
          diagnostic->message = "Missing required Operator output slot '" +
                                slot.slot_name + "' (at " + ptr + ")";
          diagnostic->legacy_status = -2;
        }
        return false;
      }
      continue;
    }
    ResolvedOutputPoolSpec pool_spec;
    std::string param_text;
    std::string alloc_err;
    int alloc_ret = OperatorConfigResolver::ResolveOutputAllocation(
        allocations[slot.slot_name], slot, &pool_spec, &param_text, &alloc_err);
    if (alloc_ret != 0) {
      if (diagnostic) {
        std::string ptr = "/deployment/io/output_allocations/" +
                          EscapeJsonPointer(slot.slot_name);
        diagnostic->code = "INVALID_OUTPUT_ALLOCATION";
        diagnostic->path = ptr;
        diagnostic->message = alloc_err + " (at " + ptr + ")";
        diagnostic->legacy_status = -2;
      }
      return false;
    }
    local_output_specs[slot.slot_name] = std::move(pool_spec);
    local_output_params[slot.slot_name] = std::move(param_text);
  }

  // S5: 应用模型路径覆盖并记录来源
  std::unordered_map<std::string, size_t> model_id_to_index;
  for (size_t i = 0; i < original_config.models.size(); ++i) {
    model_id_to_index[original_config.models[i].model_id] = i;
  }

  if (document.contains("deployment") && document["deployment"].is_object() &&
      document["deployment"].contains("model_paths") &&
      document["deployment"]["model_paths"].is_object()) {
    for (const auto& [mid, _] : document["deployment"]["model_paths"].items()) {
      if (model_id_to_index.find(mid) == model_id_to_index.end()) {
        if (diagnostic) {
          std::string ptr = "/deployment/model_paths/" + EscapeJsonPointer(mid);
          diagnostic->code = "UNKNOWN_MODEL_ID";
          diagnostic->path = ptr;
          diagnostic->message = "Unknown model_id '" + mid +
                                "' in '/deployment/model_paths' (at " + ptr +
                                ")";
          diagnostic->legacy_status = -2;
        }
        return false;
      }
    }
  }

  std::vector<std::string> model_path_source_pointers(
      original_config.models.size());
  for (size_t i = 0; i < original_config.models.size(); ++i) {
    model_path_source_pointers[i] =
        "/models/" + std::to_string(i) + "/model_path";
  }

  nlohmann::json staged_neutral_json = doc_split.neutral_pipeline_json;
  std::unordered_set<std::string> overridden_model_ids;
  if (doc_split.deployment.has_model_paths) {
    for (const auto& [mid, override_path] : doc_split.deployment.model_paths) {
      auto it = model_id_to_index.find(mid);
      if (it != model_id_to_index.end()) {
        size_t idx = it->second;
        staged_neutral_json["models"][idx]["model_path"] = override_path;
        overridden_model_ids.insert(mid);
        model_path_source_pointers[idx] =
            "/deployment/model_paths/" + EscapeJsonPointer(mid);
      }
    }
  }

  // S6: 按模式处理有效路径
  nlohmann::json resolved_neutral_json;
  if (options.path_mode == DeploymentPathMode::kUnderRoot) {
    std::string model_err;
    if (!ResolveDeploymentModelPaths(
            staged_neutral_json, options.model_root_dir, &resolved_neutral_json,
            &model_err, overridden_model_ids, diagnostic)) {
      return false;
    }
  } else {
    resolved_neutral_json = std::move(staged_neutral_json);
  }

  // S7: 构造中性 I/O 边界，发布准备结果
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

  PreparedDeployment local_prep;
  local_prep.binding = *binding;
  local_prep.input_converter = in_conv;
  local_prep.output_converter = out_conv;
  local_prep.input_port_bindings = InputPortBindings(binding->input_ports);
  local_prep.output_port_bindings = OutputPortBindings(binding->output_ports);
  local_prep.effective_max_batch_size = max_batch;
  local_prep.output_specs = std::move(local_output_specs);
  local_prep.output_parameter_texts = std::move(local_output_params);
  local_prep.overridden_model_ids = std::move(overridden_model_ids);
  local_prep.model_path_source_pointers = std::move(model_path_source_pointers);
  local_prep.neutral_pipeline_json = std::move(resolved_neutral_json);
  local_prep.io_boundary = std::move(io_boundary);

  *output = std::move(local_prep);
  return true;
}

void ProjectModelPathDiagnostics(const PreparedDeployment& prepared,
                                 ValidationReport* report) {
  if (!report) return;

  for (auto& diag : report->diagnostics) {
    if (diag.path.rfind("/models/", 0) == 0) {
      auto second_slash = diag.path.find('/', 8);
      if (second_slash != std::string::npos &&
          diag.path.substr(second_slash) == "/model_path") {
        std::string idx_str = diag.path.substr(8, second_slash - 8);
        if (!idx_str.empty() &&
            std::all_of(idx_str.begin(), idx_str.end(), ::isdigit)) {
          size_t idx = std::stoul(idx_str);
          if (idx < prepared.model_path_source_pointers.size()) {
            diag.path = prepared.model_path_source_pointers[idx];
          }
        }
      }
    }

    if (diag.remediation.has_value()) {
      auto& fixes = diag.remediation->fixes;
      fixes.erase(
          std::remove_if(
              fixes.begin(), fixes.end(),
              [&](const ValidationFix& fix) {
                if (!fix.patch.is_array()) return false;
                for (const auto& op : fix.patch) {
                  if (op.is_object() && op.contains("path") &&
                      op["path"].is_string()) {
                    std::string p = op["path"].get<std::string>();
                    if (p.rfind("/models/", 0) == 0) {
                      auto second_slash = p.find('/', 8);
                      if (second_slash != std::string::npos &&
                          p.substr(second_slash) == "/model_path") {
                        std::string idx_str = p.substr(8, second_slash - 8);
                        if (!idx_str.empty() &&
                            std::all_of(idx_str.begin(), idx_str.end(),
                                        ::isdigit)) {
                          size_t idx = std::stoul(idx_str);
                          if (idx <
                              prepared.model_path_source_pointers.size()) {
                            if (prepared.model_path_source_pointers[idx].rfind(
                                    "/deployment/model_paths/", 0) == 0) {
                              return true;
                            }
                          }
                        }
                      }
                    }
                  }
                }
                return false;
              }),
          fixes.end());
    }
  }
}

}  // namespace llm_edgeflow
