#include "adapter/deployment_preparation.h"

#include <algorithm>
#include <set>

#include "adapter/io_converter_registry.h"
#include "adapter/model_file_resolver.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "adapter/pipeline_document.h"
#include "contracts/config_schema_validation.h"
#include "core/diagnostic_code.h"
#include "core/name_suggestions.h"
#include "core/pipeline_config.h"

namespace llm_edgeflow {
namespace {
bool Fail(DeploymentDiagnostic* diagnostic, const std::string& code,
          const std::string& path, const std::string& message) {
  if (diagnostic) {
    diagnostic->code = code;
    diagnostic->path = path;
    diagnostic->message = message;
  }
  return false;
}

const char* ParameterErrorCode(ConfigFieldErrorKind kind) {
  switch (kind) {
    case ConfigFieldErrorKind::kUnknownField:
      return "UNKNOWN_CONFIG_FIELD";
    case ConfigFieldErrorKind::kMissingField:
      return "MISSING_CONFIG_FIELD";
    case ConfigFieldErrorKind::kOutOfRange:
    case ConfigFieldErrorKind::kNonFinite:
      return "CONFIG_FIELD_RANGE";
    case ConfigFieldErrorKind::kInvalidEnum:
      return "CONFIG_FIELD_ENUM";
    default:
      return "CONFIG_FIELD_TYPE";
  }
}

bool ParseParameters(const ParameterSet& declaration,
                     const std::vector<ConfigFieldDefinition>& fields,
                     const IoEntryConfig& entry, const std::string& path,
                     std::shared_ptr<const ParameterValues>* values,
                     DeploymentDiagnostic* diagnostic) {
  nlohmann::json normalized;
  std::vector<ConfigFieldValidationError> errors;
  if (!ValidateAndNormalizeFields(fields, entry.params, &normalized, &errors)) {
    const auto& error = errors.front();
    return Fail(diagnostic, ParameterErrorCode(error.kind),
                path + "/params" + error.path, error.message);
  }
  std::string error;
  if (!declaration.Parse(normalized, values, &error))
    return Fail(diagnostic, "INVALID_COMBINATION", path + "/params", error);
  return true;
}

std::string UnknownConverter(const IoEntryConfig& entry, bool input) {
  auto message = "Unknown converter: " + entry.type + "/" + entry.name;
  const auto append = [&](const auto& registered) {
    std::set<std::string> types;
    bool known_type = false;
    for (const auto& def : registered) {
      types.insert(def.type);
      if (def.type == entry.type) {
        known_type = true;
        message += "; registered name: " + def.name;
      }
    }
    if (!known_type)
      for (const auto& type :
           NearestNames(entry.type, {types.begin(), types.end()}))
        message += "; suggested type: " + type;
  };
  if (input)
    append(IoConverterRegistry::Instance().AllInputConverters());
  else
    append(IoConverterRegistry::Instance().AllOutputConverters());
  return message;
}
}  // namespace

bool PrepareDeploymentDocument(const nlohmann::json& document,
                               const DeploymentPrepareOptions& options,
                               PreparedDeployment* output,
                               DeploymentDiagnostic* diagnostic) {
  if (diagnostic) diagnostic->Clear();
  if (!output)
    return Fail(diagnostic, "DEPLOYMENT_ERROR", "/", "Output pointer is null");
  output->Clear();
  PipelineDocumentSplit split;
  std::string error, path;
  if (!SplitPipelineDocument(document, &split, &error, &path)) {
    const auto inputs = path.find("/inputs", 11);
    const bool output_input_type =
        path.rfind("/io/output/", 0) == 0 && inputs != std::string::npos &&
        (inputs + 7 == path.size() || path[inputs + 7] == '/');
    return Fail(diagnostic,
                output_input_type ? "FIELD_TYPE" : "DEPLOYMENT_ERROR",
                path.empty() ? "/" : path, error);
  }
  auto& converters = IoConverterRegistry::Instance();
  auto& value_types = OperatorValueTypeRegistry::Instance();
  std::vector<std::string> audit_errors;
  if (!converters.Audit(&audit_errors))
    return Fail(diagnostic, "REGISTRY_CONFLICT", "/io", audit_errors.front());

  // Audit guarantees the selected host and allocator bindings exist.

  PreparedDeployment prepared;
  std::set<std::pair<std::string, std::string>> seen;
  std::set<std::string> input_ports;
  for (size_t i = 0; i < split.inputs.size(); ++i) {
    const auto& entry = split.inputs[i];
    const auto at = "/io/input/" + std::to_string(i);
    if (!seen.emplace(entry.type, entry.name).second)
      return Fail(
          diagnostic, "DUPLICATE_IO_ENTRY", at,
          "Duplicate input converter: " + entry.type + "/" + entry.name);
    const auto* def = converters.FindInputConverter(entry.type, entry.name);
    if (!def)
      return Fail(diagnostic, "UNKNOWN_CONVERTER", at,
                  UnknownConverter(entry, true));
    SelectedInput selected;
    selected.converter = def;
    selected.host_binding = value_types.CopyBindingBySuffix(def->type).value();
    if (!ParseParameters(def->params, def->params.Fields(), entry, at,
                         &selected.params, diagnostic))
      return false;
    for (const auto& port : def->logical_ports) {
      if (!input_ports.insert(port.logical_name).second)
        return Fail(diagnostic, "DUPLICATE_PORT_PRODUCER", at,
                    "Duplicate input port: " + port.logical_name);
    }
    for (const auto& port : def->logical_ports) {
      const auto key = "input." + port.logical_name;
      selected.ports[port.logical_name] = key;
      prepared.io_boundary.input_published_ports.emplace_back(
          key, port.type_id, port.required, port.cardinality,
          port.provenance_policy, port.lifetime);
    }
    prepared.inputs.push_back(std::move(selected));
  }

  seen.clear();
  for (size_t i = 0; i < split.outputs.size(); ++i) {
    const auto& entry = split.outputs[i];
    const auto at = "/io/output/" + std::to_string(i);
    if (!seen.emplace(entry.type, entry.name).second)
      return Fail(
          diagnostic, "DUPLICATE_IO_ENTRY", at,
          "Duplicate output converter: " + entry.type + "/" + entry.name);
    const auto* def = converters.FindOutputConverter(entry.type, entry.name);
    if (!def)
      return Fail(diagnostic, "UNKNOWN_CONVERTER", at,
                  UnknownConverter(entry, false));
    SelectedOutput selected;
    selected.converter = def;
    selected.host_binding = value_types.CopyBindingBySuffix(def->type).value();
    selected.allocator_binding =
        value_types.CopyOutputBinding(def->type, def->slot.allocator).value();
    const auto& binding = selected.allocator_binding;
    if (!ParseParameters(def->params,
                         OutputConverterParameterFields(*def, &binding), entry,
                         at, &selected.params, diagnostic))
      return false;
    ResolvedOutputPoolSpec spec;
    spec.type = def->type;
    spec.allocator = def->slot.allocator;
    spec.params = converters.OutputParameters(def->type, def->name);
    spec.meta_num = def->slot.metadata_count;
    spec.metadata_type_id = def->slot.metadata_type_id;
    for (const auto& [field, limit] :
         binding.output_layout.string_capacity_fields) {
      const auto parameter = field + "_max_bytes";
      const auto value = selected.params->Integer(parameter);
      if (!value || *value < 1 ||
          static_cast<uint64_t>(*value) > limit.max_capacity)
        return Fail(diagnostic, "CONFIG_FIELD_RANGE",
                    at + "/params/" + EscapeJsonPointer(parameter),
                    "Effective output size must be within platform limits");
      spec.capacities[field] = static_cast<uint32_t>(*value);
    }
    if (!ResolveOutputPoolSpec(binding, spec, &selected.pool_spec, &error))
      return Fail(diagnostic, "INVALID_OUTPUT_ALLOCATION", at, error);
    for (const auto& [port, source] : entry.inputs) {
      const auto declared =
          std::find_if(def->logical_ports.begin(), def->logical_ports.end(),
                       [&](const auto& candidate) {
                         return candidate.logical_name == port;
                       });
      if (declared == def->logical_ports.end())
        return Fail(diagnostic, "UNKNOWN_FIELD",
                    at + "/inputs/" + EscapeJsonPointer(port),
                    "Unknown output converter logical input port: " + port);
      selected.ports[port] = source;
    }
    for (const auto& port : def->logical_ports) {
      const auto bound = entry.inputs.find(port.logical_name);
      IoPortDefinition consumer{
          bound == entry.inputs.end() ? "" : bound->second,
          port.type_id,
          port.required,
          port.cardinality,
          port.provenance_policy,
          port.lifetime};
      consumer.logical_name = port.logical_name;
      consumer.path = at + "/inputs";
      consumer.has_binding = bound != entry.inputs.end();
      prepared.io_boundary.output_consumed_ports.push_back(std::move(consumer));
    }
    prepared.outputs.push_back(std::move(selected));
  }

  if (!ResolveModelFiles(split.neutral_pipeline_json, options.pipeline_dir,
                         &prepared.neutral_pipeline_json, &error, diagnostic))
    return false;
  PipelineDiagnostic core_diagnostic;
  if (!ParsePipelineConfig(prepared.neutral_pipeline_json,
                           &prepared.parsed_pipeline, &core_diagnostic)) {
    if (diagnostic) diagnostic->pipeline_diagnostic = core_diagnostic;
    return Fail(diagnostic, DiagnosticCodeName(core_diagnostic.code),
                core_diagnostic.path, core_diagnostic.message);
  }
  *output = std::move(prepared);
  return true;
}
}  // namespace llm_edgeflow
