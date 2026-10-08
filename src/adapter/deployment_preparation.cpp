#include "adapter/deployment_preparation.h"

#include <algorithm>
#include <set>
#include <unordered_map>

#include "adapter/deployment_model_resolver.h"
#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "adapter/pipeline_document.h"
#include "contracts/json_pointer.h"
#include "core/diagnostic_code.h"
#include "core/pipeline_config.h"

namespace llm_edgeflow {
namespace {

void SetDiagnostic(DeploymentDiagnostic* diagnostic, const std::string& code,
                   const std::string& path, const std::string& message) {
  if (!diagnostic) return;
  diagnostic->code = code;
  diagnostic->path = path;
  diagnostic->message = message;
  diagnostic->pipeline_diagnostic.reset();
}

std::string Join(const std::vector<std::string>& names) {
  std::string joined;
  for (const auto& name : names) {
    if (!joined.empty()) joined += ", ";
    joined += name;
  }
  return joined;
}

// 校验并补齐一项的参数：字段层错误沿用节点的诊断码，路径指向具体参数；
// 之后执行 Prepare 和 Validate，失败报 INVALID_COMBINATION。
bool ParseItemParams(const ParameterSet& declaration, const IoItemSpec& item,
                     const std::string& item_path,
                     std::shared_ptr<const ParameterValues>* values,
                     DeploymentDiagnostic* diagnostic) {
  std::vector<ValidationDiagnostic> field_diagnostics;
  nlohmann::json normalized;
  if (!ValidateAndNormalizeConfig(declaration.Fields(), item.params,
                                  &normalized, &field_diagnostics,
                                  item_path + "/params")) {
    const auto& first = field_diagnostics.front();
    SetDiagnostic(diagnostic, DiagnosticCodeName(first.code), first.path,
                  first.message);
    return false;
  }
  std::string error;
  if (!declaration.Parse(normalized, values, &error)) {
    SetDiagnostic(
        diagnostic, "INVALID_COMBINATION", item_path + "/params",
        "Invalid parameters for " + item.type + "/" + item.name + ": " + error);
    return false;
  }
  return true;
}

std::string UnknownConverterMessage(const char* side, const IoItemSpec& item,
                                    const std::vector<std::string>& names,
                                    const std::vector<std::string>& types) {
  std::string message = std::string("Unknown ") + side + " converter " +
                        item.type + "/" + item.name;
  if (!names.empty()) {
    message +=
        "; registered names for type '" + item.type + "': " + Join(names);
  } else {
    message += "; registered " + std::string(side) + " types: " + Join(types);
  }
  return message;
}

bool CheckUniqueItems(const char* side, const std::vector<IoItemSpec>& items,
                      DeploymentDiagnostic* diagnostic) {
  std::set<std::pair<std::string, std::string>> pairs;
  std::set<std::string> types;
  for (size_t i = 0; i < items.size(); ++i) {
    const std::string path =
        std::string("/io/") + side + "/" + std::to_string(i);
    if (!pairs.emplace(items[i].type, items[i].name).second) {
      SetDiagnostic(diagnostic, "DUPLICATE_IO_ENTRY", path,
                    "Duplicate io " + std::string(side) + " item " +
                        items[i].type + "/" + items[i].name);
      return false;
    }
    if (!types.insert(items[i].type).second) {
      SetDiagnostic(diagnostic, "INVALID_COMBINATION", path,
                    "Host struct type '" + items[i].type +
                        "' is used by more than one io " + side + " item");
      return false;
    }
  }
  return true;
}

// 由输出项的槽声明和尺寸参数生成输出池规格。
bool BuildOutputPoolSpec(const OutputConverterDefinition& def,
                         const ParameterValues& values,
                         const std::string& item_path,
                         ResolvedOutputPoolSpec* spec,
                         DeploymentDiagnostic* diagnostic) {
  const auto* binding = OperatorValueTypeRegistry::Instance().GetOutputBinding(
      def.slot.type_suffix, def.slot.allocator);
  if (!binding) {
    SetDiagnostic(diagnostic, "INVALID_OUTPUT_ALLOCATION", item_path,
                  "Missing output value binding for " + def.Label());
    return false;
  }
  ResolvedOutputPoolSpec requested;
  requested.type = def.slot.type_suffix;
  requested.allocator = def.slot.allocator;
  requested.meta_num = def.slot.metadata_count;
  requested.metadata_type_id = def.slot.metadata_type_id;
  std::string error;
  if (!NormalizeOutputParameters(*binding, def.slot.AllocatorParamsText(),
                                 &requested.params, &error)) {
    SetDiagnostic(diagnostic, "INVALID_OUTPUT_ALLOCATION", item_path,
                  "Output allocator parameters of " + def.Label() +
                      " are invalid: " + error);
    return false;
  }
  std::vector<std::string> fields;
  for (const auto& [field, config] :
       binding->output_layout.string_capacity_fields) {
    fields.push_back(field);
  }
  std::sort(fields.begin(), fields.end());
  for (const auto& field : fields) {
    const std::string parameter = field + "_max_bytes";
    const auto value = values.Integer(parameter);
    if (!value.has_value()) {
      SetDiagnostic(diagnostic, "INVALID_OUTPUT_ALLOCATION", item_path,
                    def.Label() + " declares no size parameter '" + parameter +
                        "' for output field '" + field + "'");
      return false;
    }
    const auto max =
        binding->output_layout.string_capacity_fields.at(field).max_capacity;
    if (*value < 1 || *value > static_cast<int64_t>(max)) {
      SetDiagnostic(diagnostic, "CONFIG_FIELD_RANGE",
                    item_path + "/params/" + EscapeJsonPointer(parameter),
                    "Parameter '" + parameter + "' (" + std::to_string(*value) +
                        ") must be within [1, " + std::to_string(max) +
                        "] (platform limit)");
      return false;
    }
    requested.capacities[field] = static_cast<uint32_t>(*value);
  }
  if (!ResolveOutputPoolSpec(*binding, requested, spec, &error)) {
    SetDiagnostic(
        diagnostic, "INVALID_OUTPUT_ALLOCATION", item_path,
        "Output allocation of " + def.Label() + " is invalid: " + error);
    return false;
  }
  return true;
}

}  // namespace

bool PrepareDeploymentDocument(const nlohmann::json& document,
                               const DeploymentPrepareOptions& options,
                               PreparedDeployment* output,
                               DeploymentDiagnostic* diagnostic) {
  if (!output) {
    SetDiagnostic(diagnostic, "DEPLOYMENT_ERROR", "/",
                  "Output pointer is null");
    return false;
  }
  output->Clear();
  if (diagnostic) diagnostic->Clear();

  // S1: 拆出 io 并检查结构。
  PipelineDocumentSplit split;
  std::string split_error;
  std::string split_path;
  if (!SplitPipelineDocument(document, &split, &split_error, &split_path)) {
    SetDiagnostic(diagnostic, "DEPLOYMENT_ERROR",
                  split_path.empty() ? "/" : split_path, split_error);
    return false;
  }

  // S2: 其余部分按 Core 文档检查结构。
  ParsedPipelineConfig parsed;
  PipelineDiagnostic core_diagnostic;
  if (!ParsePipelineConfig(split.core_json, &parsed, &core_diagnostic)) {
    if (diagnostic) {
      diagnostic->pipeline_diagnostic = core_diagnostic;
      diagnostic->code = DiagnosticCodeName(core_diagnostic.code);
      diagnostic->path = core_diagnostic.path;
      diagnostic->message = core_diagnostic.message;
    }
    return false;
  }

  // S3: 按（type, name）选出登记，校验参数；同一侧不重复。
  if (!CheckUniqueItems("input", split.io.input, diagnostic) ||
      !CheckUniqueItems("output", split.io.output, diagnostic)) {
    return false;
  }
  const auto& registry = IoConverterRegistry::Instance();
  IoSelection selection;
  std::set<std::string> published_ports;
  bool has_request_id_source = false;
  for (size_t i = 0; i < split.io.input.size(); ++i) {
    const auto& item = split.io.input[i];
    const std::string path = "/io/input/" + std::to_string(i);
    const auto* converter = registry.FindInputConverter(item.type, item.name);
    if (!converter) {
      SetDiagnostic(diagnostic, "UNKNOWN_CONVERTER", path,
                    UnknownConverterMessage(
                        "input", item, registry.InputNamesOfType(item.type),
                        registry.InputTypes()));
      return false;
    }
    SelectedInput selected;
    selected.converter = converter;
    if (!ParseItemParams(converter->params, item, path, &selected.params,
                         diagnostic)) {
      return false;
    }
    for (const auto& port : converter->logical_ports) {
      if (!published_ports.insert(port.logical_name).second) {
        SetDiagnostic(diagnostic, "DUPLICATE_PORT_PRODUCER", path,
                      "Write-once Blackboard port has multiple producers: " +
                          port.logical_name);
        return false;
      }
    }
    const auto* binding =
        OperatorValueTypeRegistry::Instance().GetBindingBySuffix(
            converter->slot.type_suffix);
    if (binding && binding->read_request_id) has_request_id_source = true;
    selection.inputs.push_back(std::move(selected));
  }
  if (!has_request_id_source) {
    SetDiagnostic(diagnostic, "INVALID_COMBINATION", "/io/input",
                  "No input item carries a request_id: at least one input "
                  "host struct must provide it");
    return false;
  }
  for (size_t i = 0; i < split.io.output.size(); ++i) {
    const auto& item = split.io.output[i];
    const std::string path = "/io/output/" + std::to_string(i);
    const auto* converter = registry.FindOutputConverter(item.type, item.name);
    if (!converter) {
      SetDiagnostic(diagnostic, "UNKNOWN_CONVERTER", path,
                    UnknownConverterMessage(
                        "output", item, registry.OutputNamesOfType(item.type),
                        registry.OutputTypes()));
      return false;
    }
    SelectedOutput selected;
    selected.converter = converter;
    if (!ParseItemParams(converter->params, item, path, &selected.params,
                         diagnostic) ||
        !BuildOutputPoolSpec(*converter, *selected.params, path,
                             &selected.pool_spec, diagnostic)) {
      return false;
    }
    selection.outputs.push_back(std::move(selected));
  }

  // S4: 解析模型条目的路径。
  nlohmann::json resolved_core_json;
  if (!options.model_root_dir.empty()) {
    std::string model_error;
    if (!ResolveDeploymentModelPaths(split.core_json, options.model_root_dir,
                                     &resolved_core_json, &model_error,
                                     diagnostic)) {
      return false;
    }
  } else {
    resolved_core_json = std::move(split.core_json);
  }

  // S5: 由所选 converter 组成中性 IO 边界；端口名即 Blackboard 键。
  PipelineIoBoundary io_boundary;
  for (const auto& input : selection.inputs) {
    for (const auto& port : input.converter->logical_ports) {
      io_boundary.input_published_ports.emplace_back(
          port.logical_name, port.type_id, port.required, port.cardinality,
          port.provenance_policy, port.lifetime, port.lifetime_config_field);
    }
  }
  for (const auto& out : selection.outputs) {
    for (const auto& port : out.converter->logical_ports) {
      io_boundary.output_consumed_ports.emplace_back(
          port.logical_name, port.type_id, port.required, port.cardinality,
          port.provenance_policy, port.lifetime, port.lifetime_config_field);
    }
  }

  PreparedDeployment prepared;
  static_cast<IoSelection&>(prepared) = std::move(selection);
  prepared.neutral_pipeline_json = std::move(resolved_core_json);
  prepared.io_boundary = std::move(io_boundary);
  *output = std::move(prepared);
  return true;
}

nlohmann::json EffectiveIoJson(const IoSelection& selection) {
  const auto item = [](const std::string& type, const std::string& name,
                       const ParameterValues* params) {
    nlohmann::json entry = {{"type", type}, {"name", name}};
    if (params && !params->Effective().empty()) {
      entry["params"] = params->Effective();
    }
    return entry;
  };
  nlohmann::json input = nlohmann::json::array();
  for (const auto& selected : selection.inputs) {
    input.push_back(item(selected.converter->type, selected.converter->name,
                         selected.params.get()));
  }
  nlohmann::json output = nlohmann::json::array();
  for (const auto& selected : selection.outputs) {
    output.push_back(item(selected.converter->type, selected.converter->name,
                          selected.params.get()));
  }
  return {{"input", std::move(input)}, {"output", std::move(output)}};
}

}  // namespace llm_edgeflow
