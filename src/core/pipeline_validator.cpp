#include "core/pipeline_validator.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_config.h"
#include "engine/engine_registry.h"

namespace alg_framework {

const char* DiagnosticCodeName(DiagnosticCode code) noexcept {
  switch (code) {
    case DiagnosticCode::kOk:
      return "OK";
    case DiagnosticCode::kJsonParse:
      return "JSON_PARSE";
    case DiagnosticCode::kConfigFileOpen:
      return "CONFIG_FILE_OPEN";
    case DiagnosticCode::kRootType:
      return "ROOT_TYPE";
    case DiagnosticCode::kUnknownField:
      return "UNKNOWN_FIELD";
    case DiagnosticCode::kMissingField:
      return "MISSING_FIELD";
    case DiagnosticCode::kFieldType:
      return "FIELD_TYPE";
    case DiagnosticCode::kFieldRange:
      return "FIELD_RANGE";
    case DiagnosticCode::kInvalidCombination:
      return "INVALID_COMBINATION";
    case DiagnosticCode::kDuplicateModelId:
      return "DUPLICATE_MODEL_ID";
    case DiagnosticCode::kDuplicateNodeId:
      return "DUPLICATE_NODE_ID";
    case DiagnosticCode::kUnknownBusiness:
      return "UNKNOWN_BUSINESS";
    case DiagnosticCode::kUnknownNodeType:
      return "UNKNOWN_NODE_TYPE";
    case DiagnosticCode::kUnknownEngineType:
      return "UNKNOWN_ENGINE_TYPE";
    case DiagnosticCode::kInvalidDependency:
      return "INVALID_DEPENDENCY";
    case DiagnosticCode::kDuplicateDependency:
      return "DUPLICATE_DEPENDENCY";
    case DiagnosticCode::kDagCycle:
      return "DAG_CYCLE";
    case DiagnosticCode::kRegistryConflict:
      return "REGISTRY_CONFLICT";
    case DiagnosticCode::kUnknownConfigField:
      return "UNKNOWN_CONFIG_FIELD";
    case DiagnosticCode::kMissingConfigField:
      return "MISSING_CONFIG_FIELD";
    case DiagnosticCode::kConfigFieldType:
      return "CONFIG_FIELD_TYPE";
    case DiagnosticCode::kConfigFieldRange:
      return "CONFIG_FIELD_RANGE";
    case DiagnosticCode::kConfigFieldEnum:
      return "CONFIG_FIELD_ENUM";
    case DiagnosticCode::kUnknownModelReference:
      return "UNKNOWN_MODEL_REFERENCE";
    case DiagnosticCode::kModelCapabilityMismatch:
      return "MODEL_CAPABILITY_MISMATCH";
    case DiagnosticCode::kNodeBusinessMismatch:
      return "NODE_BUSINESS_MISMATCH";
    case DiagnosticCode::kMissingInputProducer:
      return "MISSING_INPUT_PRODUCER";
    case DiagnosticCode::kDuplicatePortProducer:
      return "DUPLICATE_PORT_PRODUCER";
    case DiagnosticCode::kMissingBusinessOutput:
      return "MISSING_BUSINESS_OUTPUT";
    case DiagnosticCode::kNodeNotParallelSafe:
      return "NODE_NOT_PARALLEL_SAFE";
    case DiagnosticCode::kParallelWriteConflict:
      return "PARALLEL_WRITE_CONFLICT";
    case DiagnosticCode::kSerializedEngineConcurrency:
      return "SERIALIZED_ENGINE_CONCURRENCY";
    case DiagnosticCode::kInternalException:
      return "INTERNAL_EXCEPTION";
  }
  return "UNKNOWN";
}

namespace {

DiagnosticCode PipelineErrorCodeToDiagnosticCode(PipelineErrorCode code) {
  switch (code) {
    case PipelineErrorCode::kOk:
      return DiagnosticCode::kOk;
    case PipelineErrorCode::kJsonParse:
      return DiagnosticCode::kJsonParse;
    case PipelineErrorCode::kConfigFileOpen:
      return DiagnosticCode::kConfigFileOpen;
    case PipelineErrorCode::kRootType:
      return DiagnosticCode::kRootType;
    case PipelineErrorCode::kUnknownField:
      return DiagnosticCode::kUnknownField;
    case PipelineErrorCode::kMissingField:
      return DiagnosticCode::kMissingField;
    case PipelineErrorCode::kFieldType:
      return DiagnosticCode::kFieldType;
    case PipelineErrorCode::kFieldRange:
      return DiagnosticCode::kFieldRange;
    case PipelineErrorCode::kInvalidCombination:
      return DiagnosticCode::kInvalidCombination;
    case PipelineErrorCode::kDuplicateModelId:
      return DiagnosticCode::kDuplicateModelId;
    case PipelineErrorCode::kDuplicateNodeId:
      return DiagnosticCode::kDuplicateNodeId;
    case PipelineErrorCode::kUnknownNodeType:
      return DiagnosticCode::kUnknownNodeType;
    case PipelineErrorCode::kUnknownEngineType:
      return DiagnosticCode::kUnknownEngineType;
    case PipelineErrorCode::kInvalidDependency:
      return DiagnosticCode::kInvalidDependency;
    case PipelineErrorCode::kDagCycle:
      return DiagnosticCode::kDagCycle;
    case PipelineErrorCode::kRegistryConflict:
      return DiagnosticCode::kRegistryConflict;
    case PipelineErrorCode::kEngineCreateFailed:
      return DiagnosticCode::kUnknownEngineType;
    case PipelineErrorCode::kEngineLoadFailed:
      return DiagnosticCode::kUnknownModelReference;
    case PipelineErrorCode::kNodeCreateFailed:
      return DiagnosticCode::kUnknownNodeType;
    case PipelineErrorCode::kNodeInitFailed:
      return DiagnosticCode::kUnknownConfigField;
    case PipelineErrorCode::kInternalException:
      return DiagnosticCode::kInternalException;
    case PipelineErrorCode::kInvalidBuildState:
      return DiagnosticCode::kInternalException;
  }
  return DiagnosticCode::kInternalException;
}

void Add(ValidationReport* report, DiagnosticCode code, std::string path,
         std::string message, std::string node_id = {}, std::string port = {},
         std::vector<std::string> related = {},
         std::vector<std::string> suggestions = {}) {
  report->diagnostics.push_back({code, std::move(path), std::move(message),
                                 "error", std::move(node_id), std::move(port),
                                 std::move(related), std::move(suggestions)});
}

bool MatchesKind(const nlohmann::json& value, ConfigValueKind kind) {
  switch (kind) {
    case ConfigValueKind::kString:
      return value.is_string();
    case ConfigValueKind::kInteger:
      return value.is_number_integer();
    case ConfigValueKind::kNumber:
      return value.is_number();
    case ConfigValueKind::kBoolean:
      return value.is_boolean();
    case ConfigValueKind::kObject:
      return value.is_object();
    case ConfigValueKind::kArray:
      return value.is_array();
  }
  return false;
}

void ValidateConfigFields(const std::vector<ConfigFieldDefinition>& definitions,
                          const nlohmann::json& config,
                          const std::string& path_prefix,
                          const std::string& node_or_engine_id,
                          ValidationReport* report) {
  if (!config.is_object()) {
    Add(report, DiagnosticCode::kConfigFieldType, path_prefix,
        "Config must be a JSON object", node_or_engine_id);
    return;
  }

  // 1. 未知字段校验
  for (auto it = config.begin(); it != config.end(); ++it) {
    const std::string& key = it.key();
    bool found = std::any_of(definitions.begin(), definitions.end(),
                             [&](const auto& f) { return f.name == key; });
    if (!found) {
      Add(report, DiagnosticCode::kUnknownConfigField, path_prefix + "/" + key,
          "Unknown config field: " + key, node_or_engine_id);
    }
  }

  // 2. 已声明字段约束校验：required, 类型, 数值范围, enum 枚举值
  for (const auto& field : definitions) {
    std::string field_path = path_prefix + "/" + field.name;
    if (!config.contains(field.name)) {
      if (field.required) {
        Add(report, DiagnosticCode::kMissingConfigField, field_path,
            "Missing required config field: " + field.name, node_or_engine_id);
      }
      continue;
    }

    const auto& val = config[field.name];
    if (!MatchesKind(val, field.kind)) {
      Add(report, DiagnosticCode::kConfigFieldType, field_path,
          "Expected " + std::string(ConfigValueKindName(field.kind)),
          node_or_engine_id);
      continue;
    }

    if (val.is_number()) {
      double num_val = val.get<double>();
      if (field.minimum.has_value() && num_val < *field.minimum) {
        Add(report, DiagnosticCode::kConfigFieldRange, field_path,
            "Numeric value is below minimum " + std::to_string(*field.minimum),
            node_or_engine_id);
      } else if (field.maximum.has_value() && num_val > *field.maximum) {
        Add(report, DiagnosticCode::kConfigFieldRange, field_path,
            "Numeric value exceeds maximum " + std::to_string(*field.maximum),
            node_or_engine_id);
      }
    }

    if (field.kind == ConfigValueKind::kString && !field.enum_values.empty() &&
        val.is_string()) {
      std::string str_val = val.get<std::string>();
      if (std::find(field.enum_values.begin(), field.enum_values.end(),
                    str_val) == field.enum_values.end()) {
        Add(report, DiagnosticCode::kConfigFieldEnum, field_path,
            "String value '" + str_val + "' not in allowed enum values",
            node_or_engine_id);
      }
    }
  }
}

const std::vector<ParsedNodeConfig>& EffectiveNodes(
    const ParsedPipelineConfig& parsed) {
  return parsed.nodes;
}

bool ResolveTopology(const std::vector<ParsedNodeConfig>& nodes,
                     ValidationReport* report) {
  std::unordered_map<std::string, size_t> index;
  std::unordered_map<std::string, int> degrees;
  std::unordered_map<std::string, std::vector<std::string>> children;
  for (size_t i = 0; i < nodes.size(); ++i) {
    index[nodes[i].id] = i;
    degrees[nodes[i].id] = 0;
  }
  bool invalid_reference = false;
  for (const auto& node : nodes) {
    std::unordered_set<std::string> seen;
    for (size_t d = 0; d < node.depends_on.size(); ++d) {
      const auto& dep = node.depends_on[d];
      std::string path = "/pipeline/" + std::to_string(node.source_index) +
                         "/depends_on/" + std::to_string(d);
      if (dep == node.id) {
        Add(report, DiagnosticCode::kDagCycle, path,
            "Node cannot depend on itself", node.id, {}, {dep});
        invalid_reference = true;
      } else if (index.find(dep) == index.end()) {
        Add(report, DiagnosticCode::kInvalidDependency, path,
            "Dependency references an unknown node: " + dep, node.id, {},
            {dep});
        invalid_reference = true;
      } else if (!seen.insert(dep).second) {
        Add(report, DiagnosticCode::kDuplicateDependency, path,
            "Dependency is declared more than once: " + dep, node.id, {},
            {dep});
        invalid_reference = true;
      } else {
        ++degrees[node.id];
        children[dep].push_back(node.id);
      }
    }
  }
  std::vector<std::string> current;
  for (const auto& node : nodes) {
    if (degrees[node.id] == 0) current.push_back(node.id);
  }
  size_t resolved = 0;
  while (!current.empty()) {
    report->topological_layers.push_back(current);
    std::vector<std::string> next;
    for (const auto& id : current) {
      report->topological_order.push_back(id);
      ++resolved;
      for (const auto& child : children[id]) {
        if (--degrees[child] == 0) next.push_back(child);
      }
    }
    current = std::move(next);
  }
  if (resolved != nodes.size()) {
    Add(report, DiagnosticCode::kDagCycle, "/pipeline",
        "Cyclic dependency detected in pipeline");
    return false;
  }
  return !invalid_reference;
}

}  // namespace

nlohmann::json ValidationDiagnostic::ToJson() const {
  nlohmann::json item = {{"code", DiagnosticCodeName(code)},
                         {"path", path},
                         {"message", message},
                         {"severity", severity}};
  if (!node_id.empty()) item["node_id"] = node_id;
  if (!port.empty()) item["port"] = port;
  if (!related_nodes.empty()) item["related_nodes"] = related_nodes;
  if (!suggestions.empty()) item["suggestions"] = suggestions;
  return item;
}

nlohmann::json ValidationReport::ToJson() const {
  nlohmann::json items = nlohmann::json::array();
  for (const auto& diagnostic : diagnostics) {
    items.push_back(diagnostic.ToJson());
  }
  return {{"schema_version", 1},
          {"ok", ok},
          {"diagnostics", std::move(items)},
          {"plan",
           {{"topological_order", topological_order},
            {"layers", topological_layers}}}};
}

ValidatedPipelinePlan PipelineValidator::ValidateAndPlan(
    const nlohmann::json& root, ValidationPolicy policy) {
  ValidatedPipelinePlan plan;
  ValidationReport& report = plan.report;
  PipelineDiagnostic parse_diag;
  if (!ParsePipelineConfig(root, &plan.config, &parse_diag)) {
    Add(&report, PipelineErrorCodeToDiagnosticCode(parse_diag.code),
        parse_diag.path, parse_diag.message);
    report.ok = false;
    return plan;
  }
  const auto& parsed = plan.config;

  const auto* business = PipelineCatalog::FindBusiness(parsed.business_name);
  if (!business && policy == ValidationPolicy::kStrict) {
    Add(&report, DiagnosticCode::kUnknownBusiness, "/business_name",
        "No registered business contract accepts pipeline name: " +
            parsed.business_name);
  }
  if (NodeFactory::Instance().HasConflict()) {
    Add(&report, DiagnosticCode::kRegistryConflict, "/pipeline",
        "Node registry contains registration conflicts");
  }
  if (EngineFactory::Instance().HasConflict()) {
    Add(&report, DiagnosticCode::kRegistryConflict, "/models",
        "Engine registry contains registration conflicts");
  }

  std::unordered_map<std::string, std::string> model_capabilities;
  std::unordered_map<std::string, EngineThreadModel> model_thread_models;
  for (const auto& model : parsed.models) {
    const auto* engine = PipelineCatalog::FindEngine(model.engine_type);
    bool factory_has = EngineFactory::Instance().Has(model.engine_type);
    if (!factory_has || (!engine && policy == ValidationPolicy::kStrict)) {
      Add(&report, DiagnosticCode::kUnknownEngineType,
          "/models/" + std::to_string(model.source_index) + "/engine_type",
          "Unknown engine_type: " + model.engine_type);
      continue;
    }
    if (engine) {
      model_capabilities[model.model_id] = engine->capability;
      model_thread_models[model.model_id] = engine->thread_model;

      ValidateConfigFields(
          engine->config_fields, model.config,
          "/models/" + std::to_string(model.source_index) + "/config", "",
          &report);
    }
  }

  auto nodes = EffectiveNodes(parsed);
  std::unordered_map<std::string, const ParsedNodeConfig*> node_by_id;
  std::unordered_map<std::string, const NodeDefinition*> def_by_id;
  std::unordered_map<std::string, std::string> model_id_by_node;
  for (const auto& node : nodes) {
    node_by_id[node.id] = &node;
    const auto* definition = PipelineCatalog::FindNode(node.node_type);
    bool factory_has = NodeFactory::Instance().Has(node.node_type);
    if (!factory_has || (!definition && policy == ValidationPolicy::kStrict)) {
      Add(&report, DiagnosticCode::kUnknownNodeType,
          "/pipeline/" + std::to_string(node.source_index) + "/node_type",
          "Unknown node_type or missing catalog definition: " + node.node_type,
          node.id);
      continue;
    }
    if (definition) {
      def_by_id[node.id] = definition;

      if (business && !definition->business_names.empty() &&
          std::find(definition->business_names.begin(),
                    definition->business_names.end(),
                    parsed.business_name) == definition->business_names.end()) {
        Add(&report, DiagnosticCode::kNodeBusinessMismatch,
            "/pipeline/" + std::to_string(node.source_index) + "/node_type",
            "Node type is not declared for business: " + parsed.business_name,
            node.id);
      }

      ValidateConfigFields(
          definition->config_fields, node.config,
          "/pipeline/" + std::to_string(node.source_index) + "/config", node.id,
          &report);

      if (!definition->model_capability.empty()) {
        std::string model_id;
        if (node.config.contains(definition->model_config_field) &&
            node.config[definition->model_config_field].is_string()) {
          model_id =
              node.config[definition->model_config_field].get<std::string>();
        } else {
          auto field = std::find_if(
              definition->config_fields.begin(),
              definition->config_fields.end(), [&](const auto& item) {
                return item.name == definition->model_config_field;
              });
          if (field != definition->config_fields.end() &&
              field->default_value.is_string() &&
              !field->default_value.get<std::string>().empty()) {
            model_id = field->default_value.get<std::string>();
          } else if (parsed.models.size() == 1) {
            model_id = parsed.models.front().model_id;
          }
        }
        auto capability = model_capabilities.find(model_id);
        if (!model_id.empty()) model_id_by_node[node.id] = model_id;
        std::string path = "/pipeline/" + std::to_string(node.source_index) +
                           "/config/" + definition->model_config_field;
        if (capability == model_capabilities.end()) {
          Add(&report, DiagnosticCode::kUnknownModelReference, path,
              "Node references an unknown model_id: " + model_id, node.id);
        } else if (capability->second != definition->model_capability) {
          Add(&report, DiagnosticCode::kModelCapabilityMismatch, path,
              "Node requires model capability '" +
                  definition->model_capability + "' but model provides '" +
                  capability->second + "'",
              node.id);
        }
      }
    }
  }

  const size_t pre_topology_errors = report.diagnostics.size();
  ResolveTopology(nodes, &report);
  plan.topological_order = report.topological_order;
  plan.topological_layers = report.topological_layers;

  if (report.diagnostics.size() != pre_topology_errors ||
      (policy == ValidationPolicy::kStrict && !business) ||
      report.topological_order.size() != nodes.size()) {
    report.ok = report.diagnostics.empty();
    return plan;
  }

  std::unordered_map<std::string, std::vector<std::string>> deps;
  for (const auto& node : nodes) deps[node.id] = node.depends_on;
  std::function<bool(const std::string&, const std::string&)> is_ancestor =
      [&](const std::string& candidate, const std::string& node_id) {
        std::unordered_set<std::string> visited;
        std::vector<std::string> stack = deps[node_id];
        while (!stack.empty()) {
          std::string current = stack.back();
          stack.pop_back();
          if (current == candidate) return true;
          if (!visited.insert(current).second) continue;
          auto it = deps.find(current);
          if (it != deps.end())
            stack.insert(stack.end(), it->second.begin(), it->second.end());
        }
        return false;
      };

  std::unordered_map<std::string, PortDefinition> ingress;
  if (business) {
    for (const auto& port : business->ingress) ingress[port.key] = port;
  }
  std::unordered_map<std::string,
                     std::vector<std::pair<std::string, PortDefinition>>>
      producers;
  for (const auto& id : report.topological_order) {
    auto def_it = def_by_id.find(id);
    if (def_it == def_by_id.end()) continue;
    const auto& definition = *def_it->second;
    const auto& node = *node_by_id[id];
    for (const auto& input : definition.inputs) {
      if (!input.required) continue;
      bool found = false;
      auto root_port = ingress.find(input.key);
      if (root_port != ingress.end() &&
          root_port->second.type_id == input.type_id) {
        found = true;
      }
      auto producer_it = producers.find(input.key);
      if (producer_it != producers.end()) {
        for (const auto& producer : producer_it->second) {
          if (is_ancestor(producer.first, id) &&
              producer.second.type_id == input.type_id) {
            found = true;
            break;
          }
        }
      }
      if (!found && business) {
        std::vector<std::string> suggestions;
        for (const auto& candidate : PipelineCatalog::Nodes()) {
          if (std::any_of(candidate.outputs.begin(), candidate.outputs.end(),
                          [&](const auto& output) {
                            return output.key == input.key &&
                                   output.type_id == input.type_id;
                          })) {
            suggestions.push_back(candidate.node_type);
          }
        }
        Add(&report, DiagnosticCode::kMissingInputProducer,
            "/pipeline/" + std::to_string(node.source_index),
            "No business ingress or ancestor node produces required port '" +
                input.key + "' of type '" + input.type_id + "'",
            id, input.key, {}, suggestions);
      }
    }
    for (const auto& output : definition.outputs) {
      auto& existing = producers[output.key];
      if (!existing.empty() && !output.allow_override) {
        Add(&report, DiagnosticCode::kDuplicatePortProducer,
            "/pipeline/" + std::to_string(node.source_index),
            "Port is produced more than once without override permission: " +
                output.key,
            id, output.key, {existing.back().first});
      }
      existing.push_back({id, output});
    }
  }

  if (business) {
    for (const auto& required : business->egress) {
      bool found = false;
      auto it = producers.find(required.key);
      if (it != producers.end()) {
        found = std::any_of(
            it->second.begin(), it->second.end(), [&](const auto& producer) {
              return producer.second.type_id == required.type_id;
            });
      }
      if (!found) {
        Add(&report, DiagnosticCode::kMissingBusinessOutput, "/pipeline",
            "Pipeline does not produce required business output: " +
                required.key,
            {}, required.key);
      }
    }
  }

  if (parsed.execution_mode == "parallel") {
    for (const auto& layer : report.topological_layers) {
      std::unordered_map<std::string, std::string> writes;
      std::unordered_map<std::string, std::string> serialized_model_users;
      for (const auto& id : layer) {
        auto def_it = def_by_id.find(id);
        if (def_it == def_by_id.end()) continue;
        if (!def_it->second->parallel_safe && layer.size() > 1) {
          Add(&report, DiagnosticCode::kNodeNotParallelSafe, "/pipeline",
              "Node is not declared safe for wavefront parallel execution", id);
        }
        auto model_id = model_id_by_node.find(id);
        if (layer.size() > 1 && model_id != model_id_by_node.end()) {
          auto thread_model = model_thread_models.find(model_id->second);
          if (thread_model != model_thread_models.end() &&
              thread_model->second == EngineThreadModel::kSerialized) {
            auto inserted =
                serialized_model_users.emplace(model_id->second, id);
            if (!inserted.second) {
              const auto& node = *node_by_id.at(id);
              Add(&report, DiagnosticCode::kSerializedEngineConcurrency,
                  "/pipeline/" + std::to_string(node.source_index) +
                      "/config/" + def_it->second->model_config_field,
                  "Parallel layer shares serialized model instance: " +
                      model_id->second,
                  id, {}, {inserted.first->second});
            }
          }
        }
        for (const auto& output : def_it->second->outputs) {
          auto inserted = writes.emplace(output.key, id);
          if (!inserted.second) {
            Add(&report, DiagnosticCode::kParallelWriteConflict, "/pipeline",
                "Parallel layer writes the same port: " + output.key, id,
                output.key, {inserted.first->second});
          }
        }
      }
    }
  }

  report.ok = report.diagnostics.empty();
  return plan;
}

ValidationReport PipelineValidator::Validate(const nlohmann::json& root,
                                             ValidationPolicy policy) {
  return ValidateAndPlan(root, policy).report;
}

bool PipelineValidator::NormalizeExplicitDag(const nlohmann::json& root,
                                             nlohmann::json* output,
                                             ValidationDiagnostic* diagnostic) {
  if (!output) return false;
  if (!root.is_object() || !root.contains("pipeline") ||
      !root["pipeline"].is_array()) {
    ParsedPipelineConfig parsed;
    PipelineDiagnostic parse_diag;
    ParsePipelineConfig(root, &parsed, &parse_diag);
    if (diagnostic) {
      diagnostic->code = PipelineErrorCodeToDiagnosticCode(parse_diag.code);
      diagnostic->path = parse_diag.path;
      diagnostic->message = parse_diag.message;
    }
    return false;
  }

  nlohmann::json normalized = root;
  auto& pipeline = normalized["pipeline"];
  std::vector<std::string> ids;
  ids.reserve(pipeline.size());
  for (size_t i = 0; i < pipeline.size(); ++i) {
    if (!pipeline[i].is_object()) continue;
    std::string id = pipeline[i].contains("id") &&
                             pipeline[i]["id"].is_string() &&
                             !pipeline[i]["id"].get<std::string>().empty()
                         ? pipeline[i]["id"].get<std::string>()
                         : "node_" + std::to_string(i) + "_" +
                               pipeline[i].value("node_type", "UnknownNode");
    pipeline[i]["id"] = id;
    ids.push_back(std::move(id));
  }

  for (size_t i = 0; i < pipeline.size(); ++i) {
    if (!pipeline[i].is_object()) continue;
    if (!pipeline[i].contains("depends_on") ||
        !pipeline[i]["depends_on"].is_array()) {
      pipeline[i]["depends_on"] = i == 0 ? nlohmann::json::array()
                                         : nlohmann::json::array({ids[i - 1]});
    }
  }

  ParsedPipelineConfig parsed;
  PipelineDiagnostic parse_diag;
  if (!ParsePipelineConfig(normalized, &parsed, &parse_diag)) {
    if (diagnostic) {
      diagnostic->code = PipelineErrorCodeToDiagnosticCode(parse_diag.code);
      diagnostic->path = parse_diag.path;
      diagnostic->message = parse_diag.message;
    }
    return false;
  }

  *output = std::move(normalized);
  return true;
}

}  // namespace alg_framework
