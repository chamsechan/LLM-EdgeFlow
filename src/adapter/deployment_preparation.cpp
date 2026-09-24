#include "adapter/deployment_preparation.h"

#include <algorithm>
#include <unordered_map>

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

      diagnostic->pipeline_diagnostic.reset();
    }
    return false;
  }

  output->Clear();
  if (diagnostic) {
    diagnostic->Clear();
  }

  // S1: 检查调用参数与部署文档结构

  if (options.path_mode == DeploymentPathMode::kUnderRoot &&
      options.model_root_dir.empty()) {
    if (diagnostic) {
      diagnostic->code = "DEPLOYMENT_ERROR";
      diagnostic->path = "/";
      diagnostic->message =
          "model_root_dir cannot be empty when path_mode is kUnderRoot";
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
    }
    return false;
  }

  // S2: 从明确的 I/O 绑定取得内部业务身份。
  const auto& binding_id = doc_split.deployment.io.io_binding;
  const auto* binding = IoBindingRegistry::Instance().FindBinding(binding_id);
  if (!binding) {
    if (diagnostic) {
      diagnostic->code = "UNKNOWN_IO_BINDING";
      diagnostic->path = "/deployment/io/io_binding";
      diagnostic->message =
          "Unknown or unregistered io_binding: " + binding_id +
          " (at /deployment/io/io_binding)";
    }
    return false;
  }

  // 外部文档不保存业务名；只向 Core 的中性文档注入注册边界。
  doc_split.neutral_pipeline_json["biz_name"] = binding->biz_name;
  ParsedPipelineConfig original_config;
  PipelineDiagnostic core_diag;
  if (!ParsePipelineConfig(doc_split.neutral_pipeline_json, &original_config,
                           &core_diag)) {
    if (diagnostic) {
      diagnostic->pipeline_diagnostic = core_diag;
      diagnostic->code = DiagnosticCodeName(core_diag.code);
      diagnostic->path = core_diag.path;
      diagnostic->message = core_diag.message;
    }
    return false;
  }

  // S3: 解析转换器和业务边界。
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      binding->input_converter_id);
  if (!in_conv) {
    if (diagnostic) {
      diagnostic->code = "UNREGISTERED_CONVERTER";
      diagnostic->path = "/deployment/io/io_binding";
      diagnostic->message =
          "Binding references unregistered input converter: " +
          binding->input_converter_id;
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
    }
    return false;
  }

  std::string contract_error;
  if (!IoBindingRegistry::Instance().ValidateBizContract(binding->biz_name,
                                                         &contract_error)) {
    if (diagnostic) {
      diagnostic->code = "BIZ_IO_CONTRACT_MISMATCH";
      diagnostic->path = "/deployment/io/io_binding";
      diagnostic->message = contract_error;
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

  // S4: 必需输出槽采用注册默认值；可选槽由显式配置启用。
  const auto& allocations = doc_split.deployment.io.out_mem;
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
            "/deployment/io/out_mem/" + EscapeJsonPointer(it.key());
        diagnostic->code = "UNKNOWN_OUTPUT_SLOT";
        diagnostic->path = ptr;
        diagnostic->message =
            "Unknown configured output slot: " + it.key() + " (at " + ptr + ")";
      }
      return false;
    }
  }

  for (const auto& slot : out_conv->external_slots) {
    if (slot.direction != PortDirection::kOutput) continue;
    if (!slot.required && !allocations.contains(slot.slot_name)) continue;
    const auto allocation = allocations.contains(slot.slot_name)
                                ? allocations.at(slot.slot_name)
                                : nlohmann::json::object();
    ResolvedOutputPoolSpec pool_spec;
    std::string param_text;
    std::string alloc_err;
    int alloc_ret = OperatorConfigResolver::ResolveOutputAllocation(
        allocation, slot, &pool_spec, &param_text, &alloc_err);
    if (alloc_ret != 0) {
      if (diagnostic) {
        std::string ptr =
            "/deployment/io/out_mem/" + EscapeJsonPointer(slot.slot_name);
        diagnostic->code = "INVALID_OUTPUT_ALLOCATION";
        diagnostic->path = ptr;
        diagnostic->message = alloc_err + " (at " + ptr + ")";
      }
      return false;
    }
    local_output_specs[slot.slot_name] = std::move(pool_spec);
    local_output_params[slot.slot_name] = std::move(param_text);
  }

  // S5: 按模式解析模型条目的唯一路径。
  nlohmann::json resolved_neutral_json;
  if (options.path_mode == DeploymentPathMode::kUnderRoot) {
    std::string model_err;
    if (!ResolveDeploymentModelPaths(
            doc_split.neutral_pipeline_json, options.model_root_dir,
            &resolved_neutral_json, &model_err, diagnostic)) {
      return false;
    }
  } else {
    resolved_neutral_json = std::move(doc_split.neutral_pipeline_json);
  }

  // S6: 构造中性 I/O 边界，发布准备结果
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
  local_prep.neutral_pipeline_json = std::move(resolved_neutral_json);
  local_prep.io_boundary = std::move(io_boundary);

  *output = std::move(local_prep);
  return true;
}

void ProjectDeploymentDiagnostics(ValidationReport* report) {
  if (!report) return;

  for (auto& diag : report->diagnostics) {
    if (diag.path == "/biz_name") {
      diag.path = "/deployment/io/io_binding";
    }
    if (!diag.remediation.has_value()) continue;

    auto& fixes = diag.remediation->fixes;
    fixes.erase(std::remove_if(fixes.begin(), fixes.end(),
                               [](const ValidationFix& fix) {
                                 if (!fix.patch.is_array()) return false;
                                 for (const auto& op : fix.patch) {
                                   // Business identity is derived, not editable
                                   // in the external document. Model paths
                                   // remain editable.
                                   if (op.is_object() && op.contains("path") &&
                                       op["path"] == "/biz_name") {
                                     return true;
                                   }
                                 }
                                 return false;
                               }),
                fixes.end());
  }
}

}  // namespace llm_edgeflow
