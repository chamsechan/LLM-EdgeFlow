#include "adapter/io_binding_resolver.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_set>

#include "adapter/deployment_model_resolver.h"
#include "adapter/io_binding_registry.h"
#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_config_resolver.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "adapter/pipeline_document.h"
#include "core/diagnostic_code.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {

namespace fs = std::filesystem;

int IoBindingResolver::ResolveFromFile(
    const std::string& config_path, const std::string& transport,
    const std::string& model_root_dir,
    std::unique_ptr<ValidatedIoPlan>* out_plan, std::string* out_error) {
  DeploymentIoConfig config;
  std::string err;
  if (!DeploymentIoConfig::ReadFromFile(config_path, transport, &config,
                                        &err)) {
    if (out_error) *out_error = err;
    return -2;
  }
  return ResolveFromConfig(config, transport, model_root_dir, out_plan,
                           out_error);
}

int IoBindingResolver::ResolveFromConfig(
    const DeploymentIoConfig& config, const std::string& transport,
    const std::string& model_root_dir,
    std::unique_ptr<ValidatedIoPlan>* out_plan, std::string* out_error) {
  if (!out_plan) {
    if (out_error) *out_error = "Null out_plan pointer";
    return -1;
  }
  *out_plan = nullptr;

  std::ifstream pipe_ifs(config.resolved_pipe_path);
  if (!pipe_ifs.is_open()) {
    if (out_error) {
      *out_error = "Failed to open pipeline file: " + config.resolved_pipe_path;
    }
    return -2;
  }
  nlohmann::json raw_pipe_json;
  try {
    pipe_ifs >> raw_pipe_json;
  } catch (const std::exception& e) {
    if (out_error) {
      *out_error = "JSON parse exception in pipeline file: " +
                   config.resolved_pipe_path + ": " + e.what();
    }
    return -2;
  }

  return ResolveFromPipelineJson(raw_pipe_json, transport, model_root_dir,
                                 out_plan, out_error);
}

int IoBindingResolver::ResolveFromPipelineJson(
    const nlohmann::json& pipeline_json, const std::string& transport,
    const std::string& model_root_dir,
    std::unique_ptr<ValidatedIoPlan>* out_plan, std::string* out_error) {
  if (!out_plan) {
    if (out_error) *out_error = "Null out_plan pointer";
    return -1;
  }
  *out_plan = nullptr;

  // 1. 检查入口 transport
  if (transport != "operator") {
    if (out_error) {
      *out_error = "Unsupported transport: '" + transport +
                   "' (only 'operator' is supported)";
    }
    return -2;
  }

  // 2. 拆分并校验 Pipeline 文档中的 deployment 部分 (RFC-0061)
  PipelineDocumentSplit doc_split;
  std::string split_err;
  if (!SplitPipelineDocument(pipeline_json, &doc_split, &split_err)) {
    if (out_error) *out_error = split_err;
    return -2;
  }

  if (!doc_split.has_deployment || !doc_split.deployment.has_io) {
    if (out_error) {
      *out_error =
          "Missing required 'deployment.io' in pipeline JSON for deployment";
    }
    return -2;
  }

  // 3. 查找并核对绑定定义
  const std::string& binding_id = doc_split.deployment.io.io_binding;
  const auto* binding = IoBindingRegistry::Instance().FindBinding(binding_id);
  if (!binding) {
    if (out_error) {
      *out_error = "Unknown or unregistered io_binding: " + binding_id +
                   " (at /deployment/io/io_binding)";
    }
    return -2;
  }

  if (binding->transport != "operator") {
    if (out_error) {
      *out_error = "Binding transport mismatch for '" + binding_id +
                   "': expected 'operator', but binding declared '" +
                   binding->transport + "' (at /deployment/io/io_binding)";
    }
    return -2;
  }

  // 4. 查找输入与输出转换器
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      binding->input_converter_id);
  if (!in_conv) {
    if (out_error) {
      *out_error = "Binding references unregistered input converter: " +
                   binding->input_converter_id;
    }
    return -2;
  }

  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      binding->output_converter_id);
  if (!out_conv) {
    if (out_error) {
      *out_error = "Binding references unregistered output converter: " +
                   binding->output_converter_id;
    }
    return -2;
  }

  // 5. 计算有效批次上限
  size_t max_batch =
      std::min(in_conv->max_batch_size, out_conv->max_batch_size);
  const auto* exposure =
      IoBindingRegistry::Instance().FindExposure(binding->biz_name);
  if (exposure) {
    max_batch = std::min(max_batch, exposure->max_batch_size);
  }

  // 6. 校验 output_allocations 配置与槽位 (必须满足 R ⊆ C ⊆ A)
  const auto& allocations = doc_split.deployment.io.output_allocations;
  std::unordered_map<std::string, ResolvedOutputPoolSpec> output_specs;
  std::unordered_map<std::string, std::string> output_params;

  // 6.1 拒绝未在输出转换器中声明的未知槽位配置 (C ⊆ A)
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
      if (out_error) {
        *out_error = "Unknown configured output slot: " + it.key() +
                     " (at /deployment/io/output_allocations/" + it.key() + ")";
      }
      return -2;
    }
  }

  // 6.2 校验并解析每个输出槽位配置，确保所有必需槽位存在 (R ⊆ C)
  for (const auto& slot : out_conv->external_slots) {
    if (slot.direction != PortDirection::kOutput) continue;
    if (!allocations.contains(slot.slot_name)) {
      if (slot.required) {
        if (out_error) {
          *out_error = "Missing required Operator output slot '" +
                       slot.slot_name +
                       "' (at /deployment/io/output_allocations/" +
                       slot.slot_name + ")";
        }
        return -2;
      }
      continue;
    }
    const auto& slot_cfg = allocations[slot.slot_name];
    ResolvedOutputPoolSpec pool_spec;
    std::string param_text;
    std::string alloc_err;
    int alloc_ret = OperatorConfigResolver::ResolveOutputAllocation(
        slot_cfg, slot, &pool_spec, &param_text, &alloc_err);
    if (alloc_ret != 0) {
      if (out_error) *out_error = alloc_err;
      return alloc_ret;
    }
    output_specs[slot.slot_name] = std::move(pool_spec);
    output_params[slot.slot_name] = std::move(param_text);
  }

  // 6.3 默认深度下的句柄池载荷总预算校验
  size_t total_handle_pool_bytes = 0;
  for (const auto& [slot_name, pool_spec] : output_specs) {
    const auto* output_binding =
        OperatorValueTypeRegistry::Instance().GetOutputBinding(
            pool_spec.type, pool_spec.allocator);
    if (!output_binding || output_binding->direction != IoDirection::kOutput) {
      if (out_error) {
        *out_error =
            "Missing output value binding for suffix '" + pool_spec.type + "'";
      }
      return -2;
    }
    size_t slot_pool_bytes = 0;
    std::string budget_err;
    if (!ComputeOutputPoolPayloadBytes(*output_binding, pool_spec,
                                       kDefaultOutputPoolDepth,
                                       &slot_pool_bytes, &budget_err)) {
      if (out_error) {
        *out_error = "Output pool budget calculation failed: " + budget_err;
      }
      return -2;
    }
    if (!CheckedAdd(total_handle_pool_bytes, slot_pool_bytes,
                    &total_handle_pool_bytes)) {
      if (out_error) *out_error = "Handle pool budget addition overflowed";
      return -2;
    }
  }
  if (total_handle_pool_bytes > kMaxHandlePoolPayloadBytes) {
    if (out_error) {
      *out_error = "Total output pool payload (" +
                   std::to_string(total_handle_pool_bytes) +
                   " bytes) exceeds per-handle payload budget (" +
                   std::to_string(kMaxHandlePoolPayloadBytes) + " bytes)";
    }
    return -2;
  }

  // 7. 应用覆盖与解析模型路径
  nlohmann::json staged_pipe_json = doc_split.neutral_pipeline_json;
  std::unordered_set<std::string> overridden_model_ids;
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
        if (out_error) {
          *out_error =
              "Unknown model_id '" + mid + "' in '/deployment/model_paths'";
        }
        return -2;
      }
    }
    for (auto& m : staged_pipe_json["models"]) {
      if (m.is_object() && m.contains("model_id") &&
          m["model_id"].is_string()) {
        std::string mid = m["model_id"].get<std::string>();
        auto it = doc_split.deployment.model_paths.find(mid);
        if (it != doc_split.deployment.model_paths.end()) {
          m["model_path"] = it->second;
          overridden_model_ids.insert(mid);
        }
      }
    }
  }

  nlohmann::json resolved_pipeline_json;
  if (!model_root_dir.empty()) {
    std::string model_resolve_err;
    if (!ResolveDeploymentModelPaths(staged_pipe_json, model_root_dir,
                                     &resolved_pipeline_json,
                                     &model_resolve_err)) {
      if (out_error) *out_error = model_resolve_err;
      return -2;
    }
  } else {
    // 环境无关检查: 模型路径必须是非空字符串
    if (staged_pipe_json.contains("models") &&
        staged_pipe_json["models"].is_array()) {
      for (const auto& m : staged_pipe_json["models"]) {
        if (m.is_object() && m.contains("model_path")) {
          if (!m["model_path"].is_string() ||
              m["model_path"].get<std::string>().empty()) {
            if (out_error) {
              *out_error =
                  "model_path in model declaration must be a non-empty string";
            }
            return -2;
          }
        }
      }
    }
    resolved_pipeline_json = staged_pipe_json;
  }

  // 8. 构造中性 PipelineIoBoundary
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

  // 9. 调用 PipelineValidator 进行统一中性计划验证 (Core 校验 neutral_pipeline_json)
  auto plan = std::make_unique<ValidatedPipelinePlan>(
      PipelineValidator::ValidateAndPlan(
          resolved_pipeline_json, ValidationPolicy::kStrict, &io_boundary));

  if (!plan->report.ok) {
    if (out_error) {
      if (!plan->report.diagnostics.empty()) {
        const auto& d = plan->report.diagnostics.front();
        *out_error =
            "Validation failed: " + std::string(DiagnosticCodeName(d.code)) +
            " at " + d.path + ": " + d.message;
      } else {
        *out_error = "Validation failed without diagnostics";
      }
    }
    return -3;
  }

  // 10. 核对 Pipeline biz_name 与 binding biz_name
  if (plan->config.biz_name != binding->biz_name) {
    if (out_error) {
      *out_error = "Pipeline biz_name '" + plan->config.biz_name +
                   "' does not match binding biz_name '" + binding->biz_name +
                   "' (at /deployment/io/io_binding)";
    }
    return -3;
  }

  // 11. 组装不可变接入计划
  auto io_plan = std::make_unique<ValidatedIoPlan>();
  io_plan->binding = *binding;
  io_plan->input_converter = in_conv;
  io_plan->output_converter = out_conv;
  io_plan->input_port_bindings = InputPortBindings(binding->input_ports);
  io_plan->output_port_bindings = OutputPortBindings(binding->output_ports);
  io_plan->effective_max_batch_size = max_batch;
  io_plan->operator_output_specs = std::move(output_specs);
  io_plan->operator_output_parameter_texts = std::move(output_params);
  io_plan->overridden_model_ids = std::move(overridden_model_ids);
  io_plan->resolved_pipeline_json = resolved_pipeline_json;
  io_plan->pipeline_plan = std::move(plan);

  *out_plan = std::move(io_plan);
  return 0;
}

int IoBindingResolver::ResolveFromPipelineJson(
    const nlohmann::json& pipeline_json, const std::string& binding_id,
    const std::string& transport, const std::string& model_root_dir,
    std::unique_ptr<ValidatedIoPlan>* out_plan, std::string* out_error) {
  if (pipeline_json.is_object() && pipeline_json.contains("deployment") &&
      pipeline_json["deployment"].is_object() &&
      pipeline_json["deployment"].contains("io")) {
    return ResolveFromPipelineJson(pipeline_json, transport, model_root_dir,
                                   out_plan, out_error);
  }

  nlohmann::json allocations = nlohmann::json::object();
  const auto* binding = IoBindingRegistry::Instance().FindBinding(binding_id);
  if (binding) {
    const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
        binding->output_converter_id);
    if (out_conv) {
      for (const auto& slot : out_conv->external_slots) {
        if (slot.direction == PortDirection::kOutput && slot.required) {
          std::string slot_type =
              slot.type_suffix.empty() ? slot.slot_name : slot.type_suffix;
          nlohmann::json slot_alloc = {
              {"type", slot_type},
              {"meta_num", 0},
              {"metadata_type_id", 0},
              {"capacities", nlohmann::json::object()}};
          const auto* val_binding =
              OperatorValueTypeRegistry::Instance().GetOutputBinding(
                  slot_type, "");
          for (const auto& cap : slot.capacity_fields) {
            uint32_t cap_val = 1024;
            if (val_binding &&
                val_binding->output_layout.string_capacity_fields.count(cap)) {
              cap_val = val_binding->output_layout.string_capacity_fields.at(cap)
                            .default_capacity;
            }
            slot_alloc["capacities"][cap] = cap_val;
          }
          allocations[slot.slot_name] = slot_alloc;
        }
      }
    }
  }

  nlohmann::json synthetic = pipeline_json;
  synthetic["deployment"] = {
      {"io",
       {{"io_binding", binding_id},
        {"output_allocations", allocations}}}};
  return ResolveFromPipelineJson(synthetic, transport, model_root_dir,
                                 out_plan, out_error);
}

}  // namespace llm_edgeflow
