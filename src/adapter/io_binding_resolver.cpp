#include "adapter/io_binding_resolver.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "adapter/deployment_model_resolver.h"
#include "adapter/io_binding_registry.h"
#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_value_type_registry.h"
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

  // 1. 查找绑定定义
  const auto* binding =
      IoBindingRegistry::Instance().FindBinding(config.io_binding);
  if (!binding) {
    if (out_error) {
      *out_error = "Unknown or unregistered io_binding: " + config.io_binding +
                   " (at data.io_binding)";
    }
    return -2;
  }

  // 2. 检查入口类型匹配
  if (binding->transport != transport) {
    if (out_error) {
      *out_error = "Binding transport mismatch for '" + config.io_binding +
                   "': expected '" + transport + "', but binding declared '" +
                   binding->transport + "'";
    }
    return -2;
  }

  // 3. 查找输入与输出转换器
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

  // 4. 计算有效批次上限
  size_t max_batch =
      std::min(in_conv->max_batch_size, out_conv->max_batch_size);
  const auto* exposure =
      IoBindingRegistry::Instance().FindExposure(binding->biz_name);
  if (exposure) {
    max_batch = std::min(max_batch, exposure->max_batch_size);
  }

  // 5. 若为 Operator 入口，校验 outputs 配置与槽位
  std::unordered_map<std::string, ResolvedOutputPoolSpec> output_specs;
  if (transport == "operator") {
    for (const auto& slot : out_conv->external_slots) {
      if (slot.direction != PortDirection::kOutput) continue;
      if (!config.outputs.contains(slot.slot_name)) {
        if (slot.required) {
          if (out_error) {
            *out_error = "Missing required Operator output slot '" +
                         slot.slot_name + "' in data.outputs";
          }
          return -2;
        }
        continue;
      }
      const auto& slot_cfg = config.outputs[slot.slot_name];
      if (!slot_cfg.is_object()) {
        if (out_error) {
          *out_error = "data.outputs[" + slot.slot_name + "] must be an object";
        }
        return -2;
      }
      std::string slot_type = slot_cfg.value("type", slot.type_suffix);
      std::string allocator = slot_cfg.value("allocator", "");

      const auto* val_binding =
          OperatorValueTypeRegistry::Instance().GetOutputBinding(slot_type,
                                                                 allocator);
      if (!val_binding) {
        if (out_error) {
          *out_error = "No registered Operator output ValueType binding for slot '" +
                       slot.slot_name + "' (type: " + slot_type +
                       ", allocator: " + allocator + ")";
        }
        return -2;
      }

      ResolvedOutputPoolSpec spec;
      spec.type = slot_type;
      spec.allocator = allocator;
      if (slot_cfg.contains("capacities") &&
          slot_cfg["capacities"].is_object()) {
        for (auto it = slot_cfg["capacities"].begin();
             it != slot_cfg["capacities"].end(); ++it) {
          if (it.value().is_number_unsigned()) {
            spec.capacities[it.key()] = it.value().get<uint32_t>();
          }
        }
      }
      output_specs[slot.slot_name] = std::move(spec);
    }
  }

  // 6. 读取 Pipeline JSON
  std::ifstream pipe_ifs(config.resolved_pipe_path);
  if (!pipe_ifs.is_open()) {
    if (out_error) {
      *out_error =
          "Failed to open pipeline file: " + config.resolved_pipe_path;
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

  // 7. 应用覆盖与解析模型路径
  nlohmann::json staged_pipe_json = raw_pipe_json;
  if (!config.model_paths.empty() && staged_pipe_json.contains("models") &&
      staged_pipe_json["models"].is_array()) {
    for (auto& m : staged_pipe_json["models"]) {
      if (m.is_object() && m.contains("model_id")) {
        std::string mid = m["model_id"].get<std::string>();
        auto it = config.model_paths.find(mid);
        if (it != config.model_paths.end()) {
          m["model_path"] = it->second;
        }
      }
    }
  }

  nlohmann::json resolved_pipeline_json;
  std::string model_resolve_err;
  if (!ResolveDeploymentModelPaths(staged_pipe_json, model_root_dir,
                                   &resolved_pipeline_json,
                                   &model_resolve_err)) {
    if (out_error) *out_error = model_resolve_err;
    return -2;
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

  // 9. 调用 PipelineValidator 进行统一中性计划验证
  auto plan = std::make_unique<ValidatedPipelinePlan>(
      PipelineValidator::ValidateAndPlan(resolved_pipeline_json,
                                         ValidationPolicy::kStrict,
                                         &io_boundary));

  if (!plan->report.ok) {
    if (out_error) {
      if (!plan->report.diagnostics.empty()) {
        const auto& d = plan->report.diagnostics.front();
        *out_error = "Validation failed: " +
                     std::string(DiagnosticCodeName(d.code)) + " at " +
                     d.path + ": " + d.message;
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
                   "'";
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
  io_plan->pipeline_plan = std::move(plan);

  *out_plan = std::move(io_plan);
  return 0;
}

}  // namespace llm_edgeflow
